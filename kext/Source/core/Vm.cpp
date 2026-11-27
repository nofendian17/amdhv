#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#include "KernelDecls.hpp"
#include "Frontend.hpp"
#include "Asid.hpp"
#include "MsrMap.hpp"
#include "RunDeadline.hpp"
#include "SleepPolicy.hpp"
#include "TlbPolicy.hpp"
#include "X86.hpp"
#include "Log.hpp"
#include "SVM.hpp"
#include "Vm.hpp"
#include "ApicMmio.hpp"

#define super OSObject

OSDefineMetaClassAndStructors(com_amdhv_AMDHVVm, OSObject)

using namespace amdhv;
using namespace amdhv::vmcb;

/* SvmRun.S addresses these by hand. */
static_assert(offsetof(GuestGprs, rbx) == 0, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rcx) == 8, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rdx) == 16, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rsi) == 24, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rdi) == 32, "GuestGprs layout");
static_assert(offsetof(GuestGprs, rbp) == 40, "GuestGprs layout");
static_assert(offsetof(GuestGprs, r8) == 48, "GuestGprs layout");
static_assert(offsetof(GuestGprs, r15) == 104, "GuestGprs layout");
static_assert(sizeof(GuestGprs) == 112, "GuestGprs layout");


namespace {

const uint64_t kGuestPat = 0x0007040600070406ull;   /* power-on default PAT */


/**
 * @brief Set Seg.
 *
 * @param s State object to operate on.
 * @param selector Segment selector.
 * @param base Base address.
 * @param limit Segment limit.
 * @param attrib Segment attribute bits.
 */
void setSeg(Seg &s, uint16_t selector, uint64_t base, uint32_t limit, uint16_t attrib)
{
    s.selector = selector;
    s.base     = base;
    s.limit    = limit;
    s.attrib   = attrib;
}


/* The ASID pool is host-wide (an ASID belongs to the CPU, not to a VM), so it has one lock. */
amdhv::asid::Pool gAsidPool;
IOLock *gAsidLock = nullptr;
bool    gAsidReady = false;

/**
 * @brief Return the lazily created ASID pool lock.
 */
IOLock *asidLock()
{
    if (!gAsidLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, &gAsidLock))
            IOLockFree(l);
    }
    return gAsidLock;
}

/**
 * @brief Reserve a free ASID from the host-wide pool.
 *
 * @return The uint32_t result.
 */
uint32_t asidAlloc()
{
    IOLock *l = asidLock();
    if (!l)
        return 0;
    IOLockLock(l);
    if (!gAsidReady) {
        uint32_t r[4];
        amdhv_cpuid(0x8000000Au, 0, r);    /* EBX = the number of ASIDs, including the host's */
        gAsidPool.init(r[1]);
        gAsidReady = true;
    }
    const uint32_t id = gAsidPool.alloc();
    IOLockUnlock(l);
    return id;
}

/**
 * @brief Return an ASID to the host-wide pool.
 *
 * @param id Identifier.
 */
void asidFree(uint32_t id)
{
    if (!id)
        return;
    IOLock *l = asidLock();
    if (!l)
        return;
    IOLockLock(l);
    gAsidPool.free(id);
    IOLockUnlock(l);
}

} // namespace

/* ---- one vCPU ---- */

com_amdhv_AMDHVVm *com_amdhv_AMDHVVm::create(const AMDHVInfo &info, task_t owner,
                                                             com_amdhv_AMDHVVmShared *shared, IOReturn *err)
{
    if (info.nCpusSvmActive == 0 || info.nCpusSvmActive != info.nCpusSvmUsable) {
        AMDHV_WARN("VmCreate: SVM is not active on all CPUs (%u of %u), boot with amdhv_enable_svm=1",
                 info.nCpusSvmActive, info.nCpusSvmUsable);
        *err = kIOReturnNotReady;
        return nullptr;
    }
    if (!(info.svmFeatures & AMDHV_SVM_FEAT_NP) || !(info.svmFeatures & AMDHV_SVM_FEAT_NRIPS)) {
        AMDHV_ERR("VmCreate: CPU needs NPT and NRIPS (features=0x%x)", info.svmFeatures);
        *err = kIOReturnUnsupported;
        return nullptr;
    }
    if (info.numAsids < 2) {
        /* ASID 0 is the host's; the guest uses 1. */
        AMDHV_ERR("VmCreate: CPU offers %u ASIDs, need at least 2", info.numAsids);
        *err = kIOReturnUnsupported;
        return nullptr;
    }
    /* APM 15.25.8: with host EFER.NXE clear, every NPT mapping is executable regardless of its NX
     * bit. Refuse the VM rather than silently ignoring the client's execute permissions. */
    if (!(amdhv_rdmsr(AMDHV_MSR_EFER) & AMDHV_EFER_NXE)) {
        AMDHV_ERR("VmCreate: host EFER.NXE is clear, NPT cannot enforce execute permission");
        *err = kIOReturnUnsupported;
        return nullptr;
    }

    com_amdhv_AMDHVVmShared *sh = shared;
    if (sh)
        sh->retain();
    else
        sh = com_amdhv_AMDHVVmShared::create(owner, err);
    if (!sh)
        return nullptr;

    com_amdhv_AMDHVVm *vm = OSTypeAlloc(com_amdhv_AMDHVVm);
    if (!vm) {
        sh->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    vm->fShared = sh;                  /* the vCPU owns this reference from now on */
    if (!vm->init()) {
        vm->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }
    vm->fHasFlushByAsid = (info.svmFeatures & AMDHV_SVM_FEAT_FLUSH_BY_ASID) != 0;
    vm->fHasVmcbClean   = (info.svmFeatures & AMDHV_SVM_FEAT_VMCB_CLEAN) != 0;
    vm->fHasDecodeAssists = (info.svmFeatures & AMDHV_SVM_FEAT_DECODE_ASSISTS) != 0;
    vm->fHasPauseFilter = (info.svmFeatures & AMDHV_SVM_FEAT_PAUSE_FILTER) != 0;
    vm->fHasPauseThresh = (info.svmFeatures & AMDHV_SVM_FEAT_PAUSE_THRESH) != 0;
    IOReturn arc = sh->attach(vm, &vm->fIndex);
    if (arc != kIOReturnSuccess) {
        AMDHV_WARN("VmCreate: VM %llu has no room for another vCPU", (unsigned long long)sh->id());
        vm->release();
        *err = arc;
        return nullptr;
    }
    if (!vm->setup()) {
        vm->release();
        *err = kIOReturnNoMemory;
        return nullptr;
    }

    AMDHV_STAT_INC(vmCreates);
    *err = kIOReturnSuccess;
    return vm;
}

bool com_amdhv_AMDHVVm::init()
{
    fAsid = 0;                     /* free() must not return a garbage ASID if super::init() fails */
    fSpace = 0;                    /* the default address space until hv_vcpu_set_space */
    if (!super::init())
        return false;

    /* free() must be safe after any partial failure in setup(), so do not rely on zeroed allocations. */
    com_amdhv_AMDHVVmShared *keep = fShared;       /* set by create() before init() */
    bzero(&fVmcb, sizeof(fVmcb));
    bzero(&fHostVmcb, sizeof(fHostVmcb));
    bzero(&fIopm, sizeof(fIopm));
    bzero(&fMsrpm, sizeof(fMsrpm));
    fShared      = keep;
    fIndex       = ~0u;
    bzero(&fGprs, sizeof(fGprs));
    bzero(&fHostFpu, sizeof(fHostFpu));
    bzero(&fGuestFpu, sizeof(fGuestFpu));
    bzero(&fScratchFpu, sizeof(fScratchFpu));
    fGuestXcr0     = amdhv::fpu::XCR0_X87;
    fXcr0Supported = 0;
    fFpuSize       = 0;
    fKick          = 0;
    fHasTscAux     = false;
    fTscNative     = false;
    fApicAccel     = false;
    fApicLapic     = nullptr;
    fApicIoApic    = nullptr;
    fForceTlbFlush = 0;
    fTlbGenSeen    = 0;           /* 0 != fTlbGen (starts at 1): the first run flushes */
    fSleepEpoch    = svm::sleepEpoch();
    fHasFlushByAsid = false;      /* create() fills this in from CPUID before setup() */
    fHasVmcbClean   = false;      /* same */
    fHasPauseFilter = false;      /* same */
    fHasPauseThresh = false;      /* same */
    fLastCpu       = -1;          /* no CPU has cached this ASID yet: the first run flushes */
    {
        uint32_t asidMode = 1;
        PE_parse_boot_argn("amdhv_asid", &asidMode, sizeof(asidMode));
        fAsidMode = asidMode != 0;
    }
    fRunCpu        = -1;
    fFirstRun    = true;
    fHaveNextRip = false;
    fExecAbs     = 0;

    fLock = IOLockAlloc();
    fVmcs.setTprShadow(amdhv::hv::tprShadow());
    return fLock != nullptr;
}

void com_amdhv_AMDHVVm::free()
{
    if (fShared) {
        if (fIndex != ~0u)
            fShared->detach(this);
    }
    if (fAsid) {
        asidFree(fAsid);
        fAsid = 0;
    }
    physFree(&fScratchFpu);
    physFree(&fGuestFpu);
    physFree(&fHostFpu);
    physFree(&fMsrpm);
    physFree(&fIopm);
    physFree(&fHostVmcb);
    physFree(&fVmcb);
    if (fLock) {
        IOLockFree(fLock);
        fLock = nullptr;
    }
    if (fShared) {                    /* last: the page tables go when the last vCPU is gone */
        fShared->release();
        fShared = nullptr;
    }
    super::free();
}

bool com_amdhv_AMDHVVm::setup()
{
    /*
     * One ASID per vCPU, like KVM: two vCPUs of the same VM can be in different address spaces
     * (different guest CR3) at the same time, and a shared ASID would let one vCPU's translations be
     * used by the other on a CPU where both have run. 0 means the pool is empty; the vCPU then shares
     * the host's ASID and run() flushes the whole TLB on every entry.
     */
    fAsid = asidAlloc();
    if (!fAsid)
        AMDHV_WARN("VmCreate: no ASID left, vCPU %u shares the host's ASID (flush on every run)", fIndex);

    if (!physAlloc(&fVmcb, 4096) || !physAlloc(&fHostVmcb, 4096) || !physAlloc(&fIopm, 12288) ||
        !physAlloc(&fMsrpm, 8192))
        return false;

    {
        uint32_t r[4];
        amdhv_cpuid(0x80000001u, 0, r);
        fHasTscAux = (r[3] >> 27) & 1;           /* RDTSCP */
    }

    /* Extended state areas. The size is what XSAVE needs for the XCR0 this thread has now. */
    {
        uint32_t r[4];
        amdhv_cpuid(0xD, 0, r);
        fXcr0Supported = ((uint64_t)r[3] << 32) | r[0];
        fFpuSize       = r[1];
        if (fFpuSize < amdhv::fpu::kHeaderEnd || fFpuSize > AMDHV_FPU_AREA_MAX) {
            AMDHV_ERR("VM: XSAVE area of %u bytes is not supported (limit %u)", fFpuSize, AMDHV_FPU_AREA_MAX);
            return false;
        }
        if (!physAlloc(&fHostFpu, 4096) || !physAlloc(&fGuestFpu, 4096) || !physAlloc(&fScratchFpu, 4096))
            return false;
        amdhv::fpu::initArea((uint8_t *)fGuestFpu.va, 4096);
        fGuestXcr0 = amdhv::fpu::XCR0_X87;
    }

    fVmcs.attach((amdhv::vmcb::Vmcb *)fVmcb.va);
    fVmcs.setPauseFilter(fHasPauseFilter, fHasPauseThresh);

    /* Intercept every I/O port and every MSR: userland decides what the guest may touch. */
    memset(fIopm.va, 0xFF, fIopm.size);
    memset(fMsrpm.va, 0xFF, fMsrpm.size);

    fFirstRun = true;
    fSpace = 0;
    resetLocked(0, 0);
    uint64_t rootPa = 0;
    fShared->spaceRoot(0, &rootPa);
    AMDHV_INFO("vCPU %u of VM %llu created: vmcb pa 0x%llx, npt root pa 0x%llx", fIndex,
             (unsigned long long)fShared->id(), (unsigned long long)fVmcb.pa,
             (unsigned long long)rootPa);
    return true;
}

IOReturn com_amdhv_AMDHVVm::map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags)
{
    return fShared->map(task, 0, uva, gpa, size, flags);
}

IOReturn com_amdhv_AMDHVVm::unmap(uint64_t gpa, uint64_t size)
{
    return fShared->unmap(0, gpa, size);
}

IOReturn com_amdhv_AMDHVVm::protect(uint64_t gpa, uint64_t size, uint32_t flags)
{
    return fShared->protect(0, gpa, size, flags);
}

IOReturn com_amdhv_AMDHVVm::setSpace(uint32_t space)
{
    IOLockLock(fLock);
    /*
     * Hold fRw for the whole lookup+bind. spaceDestroy() takes fRw write and checks
     * spaceInUse(); without this, it could free the NPT root between spaceRoot() and the
     * fSpace/nestedCr3 update, leaving this vCPU running with a dangling N_CR3.
     * Lock order fLock -> fRw matches run()/spacePerm().
     */
    IORWLockRead(fShared->rw());
    uint64_t root = 0;
    IOReturn rc = kIOReturnSuccess;
    if (!fShared->spaceRootLocked(space, &root)) {
        rc = kIOReturnBadArgument;
    } else if (space != fSpace) {
        fSpace = space;
        ((Vmcb *)fVmcb.va)->control.nestedCr3 = root;
        fVmcs.markDirty(CLEAN_NP);      /* N_CR3 lives in the nested-paging clean group */
        fForceTlbFlush = 1;             /* translations of the old space must not survive */
    }
    IORWLockUnlock(fShared->rw());
    IOLockUnlock(fLock);
    return rc;
}

/* Real-mode reset state. Caller holds the lock (or is still constructing). */
void com_amdhv_AMDHVVm::resetLocked(uint16_t cs, uint16_t ip)
{
    Vmcb *v = (Vmcb *)fVmcb.va;
    bzero(v, sizeof(*v));
    bzero(&fGprs, sizeof(fGprs));

    Control &c = v->control;
    c.clean         = 0;             /* every group is dirty on a fresh VMCB */
    c.interceptDr   = 0xFFFFFFFFu;   /* DR0-3 are not swapped by VMRUN, never let the guest touch them */
    c.interceptVec3 = V3_INTR | V3_NMI | V3_SMI | V3_INIT | V3_CPUID | V3_INVD | V3_HLT | V3_IOIO |
                      V3_MSR | V3_SHUTDOWN;
    c.interceptVec4 = V4_VMRUN | V4_VMMCALL | V4_VMLOAD | V4_VMSAVE | V4_STGI | V4_CLGI |
                      V4_SKINIT | V4_WBINVD | V4_MONITOR | V4_MWAIT;
    c.iopmBasePa    = fIopm.pa;
    c.msrpmBasePa   = fMsrpm.pa;
    c.asid          = fAsid;                   /* per vCPU; 0 is the host's (and forces a full flush) */
    c.tlbCtl        = (fAsid && fHasFlushByAsid) ? TLB_FLUSH_BY_ASID : TLB_FLUSH_ALL;
    fTlbGenSeen     = 0;
    fForceTlbFlush  = 1;                       /* a reset VMCB must not reuse an old translation */
    fLastCpu        = -1;
    c.intCtl        = INT_CTL_V_INTR_MASKING;  /* guest EFLAGS.IF does not gate host interrupts */
    c.nestedCtl     = NESTED_CTL_NP_ENABLE;
    uint64_t nestedRoot = 0;
    if (!fShared->spaceRoot(fSpace, &nestedRoot)) {
        fSpace = 0;
        fShared->spaceRoot(0, &nestedRoot);
    }
    c.nestedCr3     = nestedRoot;

    Save &s = v->save;
    setSeg(s.es, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.ss, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.ds, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.fs, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.gs, 0, 0, 0xFFFF, ATTR_REAL_DATA);
    setSeg(s.cs, cs, (uint64_t)cs << 4, 0xFFFF, ATTR_REAL_CODE);
    setSeg(s.gdtr, 0, 0, 0xFFFF, 0);
    setSeg(s.idtr, 0, 0, 0xFFFF, 0);
    setSeg(s.ldtr, 0, 0, 0xFFFF, ATTR_LDT);
    setSeg(s.tr, 0, 0, 0xFFFF, ATTR_TSS_BUSY);

    s.cpl    = 0;
    s.efer   = EFER_SVME;      /* mandatory */
    s.cr0    = 0x10;           /* ET, real mode, caches on */
    s.cr3    = 0;
    s.cr4    = 0;
    s.dr6    = 0xFFFF0FF0ull;
    s.dr7    = 0x400;
    s.rflags = 0x2;
    s.rip    = ip;
    s.rsp    = 0;
    s.rax    = 0;
    s.gPat   = kGuestPat;

    fHaveNextRip = false;
    fVmcs.reset();
    if (fGuestFpu.va) {
        amdhv::fpu::initArea((uint8_t *)fGuestFpu.va, 4096);
        fGuestXcr0 = amdhv::fpu::XCR0_X87;
    }
}

IOReturn com_amdhv_AMDHVVm::getRegs(AMDHVRegs *out)
{
    IOLockLock(fLock);
    const Vmcb *v = (const Vmcb *)fVmcb.va;
    out->rax = v->save.rax;
    out->rbx = fGprs.rbx;
    out->rcx = fGprs.rcx;
    out->rdx = fGprs.rdx;
    out->rsi = fGprs.rsi;
    out->rdi = fGprs.rdi;
    out->rbp = fGprs.rbp;
    out->rsp = v->save.rsp;
    out->r8  = fGprs.r8;
    out->r9  = fGprs.r9;
    out->r10 = fGprs.r10;
    out->r11 = fGprs.r11;
    out->r12 = fGprs.r12;
    out->r13 = fGprs.r13;
    out->r14 = fGprs.r14;
    out->r15 = fGprs.r15;
    out->rip    = v->save.rip;
    out->rflags = v->save.rflags;
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_amdhv_AMDHVVm::setRegs(const AMDHVRegs *in)
{
    IOLockLock(fLock);
    Vmcb *v = (Vmcb *)fVmcb.va;
    v->save.rax = in->rax;
    fGprs.rbx = in->rbx;
    fGprs.rcx = in->rcx;
    fGprs.rdx = in->rdx;
    fGprs.rsi = in->rsi;
    fGprs.rdi = in->rdi;
    fGprs.rbp = in->rbp;
    v->save.rsp = in->rsp;
    fGprs.r8  = in->r8;
    fGprs.r9  = in->r9;
    fGprs.r10 = in->r10;
    fGprs.r11 = in->r11;
    fGprs.r12 = in->r12;
    fGprs.r13 = in->r13;
    fGprs.r14 = in->r14;
    fGprs.r15 = in->r15;
    /*
     * Writing back the registers you just read (the usual "answer CPUID" dance)
     * must not cancel a pending skip. Only a changed RIP means userland moved
     * the guest itself.
     */
    if (v->save.rip != in->rip)
        fHaveNextRip = false;
    v->save.rip    = in->rip;
    v->save.rflags = in->rflags | 0x2;   /* bit 1 is always set */
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

/*
 * At most one kick IPI per CPU is in flight: mp_cpus_call draws from a small pool in the kernel, and the handler may run
 * after the VM that asked for it is gone, so it only touches this array, never the VM.
 */
static volatile uint8_t gKickInFlight[64];

/**
 * @brief Clear the in-flight kick flag on the target CPU.
 */
static void kickAction(void *)
{
    int cpu = cpu_number();
    if (cpu >= 0 && cpu < 64)
        gKickInFlight[cpu] = 0;
}

void com_amdhv_AMDHVVm::kick()
{
    fKick = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int cpu = fRunCpu;
    if (cpu >= 0 && cpu < 64 && __sync_bool_compare_and_swap(&gKickInFlight[cpu], 0, 1))
        mp_cpus_call(1ull << cpu, 2 /* NOSYNC */, kickAction, nullptr);
}

uint64_t com_amdhv_AMDHVVm::execTimeNs()
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(fExecAbs, &ns);
    return ns;
}

IOReturn com_amdhv_AMDHVVm::msrOp(uint32_t op, uint32_t msrNum, uint64_t value, uint64_t *out)
{
    using namespace amdhv::msr;
    Info mi = info(msrNum);
    if (mi.kind == K_UNKNOWN)
        return kIOReturnBadArgument;

    IOReturn rc = kIOReturnSuccess;
    IOLockLock(fLock);
    switch (op) {
    case AMDHV_MSR_READ: {
        uint64_t tsc = 0;
        if (mi.kind == K_TSC) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            tsc = ((uint64_t)hi << 32) | lo;
        }
        *out = 0;
        if (fVmcs.readMsr(msrNum, tsc, out) != amdhv::vmx::R_OK)
            rc = kIOReturnBadArgument;
        break;
    }
    case AMDHV_MSR_WRITE: {
        uint64_t tsc = 0;
        if (mi.kind == K_TSC) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            tsc = ((uint64_t)hi << 32) | lo;
        }
        if (fVmcs.writeMsr(msrNum, tsc, value) != amdhv::vmx::R_OK)
            rc = kIOReturnBadArgument;
        else if (mi.kind == K_EFER)
            __atomic_store_n(&fForceTlbFlush, 1, __ATOMIC_RELEASE);   /* paging mode changed */
        break;
    }
    case AMDHV_MSR_NATIVE_ENABLE:
    case AMDHV_MSR_NATIVE_DISABLE: {
        const bool enable = op == AMDHV_MSR_NATIVE_ENABLE;
        if (mi.kind == K_TSC)
            fTscNative = enable;   /* run() must emulate RDMSR/WRMSR 10h while the client thinks it is native */
        /* The guest EFER must keep SVME set, so it can never be written without an exit. */
        if (mi.kind == K_EFER)
            rc = kIOReturnBadArgument;
        else if (enable && amdhv::msr::mustIntercept(msrNum)) {
            /* Answer success so the client believes the MSR is native, but leave the MSRPM intercept
             * set: run() emulates the access. TSC is the one that would corrupt the host: SVM applies
             * TSC_OFFSET to reads only, so a native WRMSR 10h writes the physical TSC. */
        } else if (!setNative((uint8_t *)fMsrpm.va, msrNum, enable))
            rc = kIOReturnBadArgument;
        break;
    }
    default:
        rc = kIOReturnBadArgument;
    }
    IOLockUnlock(fLock);
    return rc;
}

IOReturn com_amdhv_AMDHVVm::getFpu(AMDHVFpuState *out)
{
    bzero(out, sizeof(*out));
    IOLockLock(fLock);
    out->xcr0 = fGuestXcr0;
    out->size = fFpuSize;
    bcopy(fGuestFpu.va, out->area, fFpuSize);
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_amdhv_AMDHVVm::setFpu(const AMDHVFpuState *in)
{
    /* Everything XRSTOR or XSETBV would fault on is refused here: a fault in the kernel is a panic. */
    const uint64_t hostX = amdhv::fpu::xgetbv0();
    if (in->size != fFpuSize || !amdhv::fpu::validXcr0(in->xcr0, fXcr0Supported) || (in->xcr0 & ~hostX) ||
        !amdhv::fpu::validArea(in->area, fFpuSize, in->xcr0 | amdhv::fpu::XCR0_X87 | amdhv::fpu::XCR0_SSE))
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    fGuestXcr0 = in->xcr0;
    bcopy(in->area, fGuestFpu.va, fFpuSize);
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_amdhv_AMDHVVm::vmcsRead(uint32_t field, uint64_t *value)
{
    IOLockLock(fLock);
    amdhv::vmx::Result r = fVmcs.read(field, value);
    IOLockUnlock(fLock);
    return r == amdhv::vmx::R_OK ? kIOReturnSuccess : kIOReturnBadArgument;
}

IOReturn com_amdhv_AMDHVVm::vmcsReadMany(const AMDHVReadMany *in, AMDHVReadManyOut *out)
{
    bzero(out, sizeof(*out));
    if (in->count > AMDHV_READMANY_MAX)
        return kIOReturnBadArgument;
    for (uint32_t i = 0; i < in->count; i++) {
        IOReturn rc = vmcsRead(in->fields[i], &out->values[i]);
        if (rc != kIOReturnSuccess)
            return rc;
    }
    return kIOReturnSuccess;
}

IOReturn com_amdhv_AMDHVVm::runEx(const AMDHVRunIn *in, AMDHVRunOut *out, uint64_t deadline)
{
    bzero(out, sizeof(*out));
    if (in->nWrites > AMDHV_RUN_MAX_WRITES)
        return kIOReturnBadArgument;

    IOReturn rc;
    if (in->flags & AMDHV_RUN_REGS) {
        rc = setRegs(&in->regs);
        if (rc != kIOReturnSuccess)
            return rc;
    }
    for (uint32_t i = 0; i < in->nWrites; i++) {
        rc = vmcsWrite(in->writes[i].field, in->writes[i].value);
        if (rc != kIOReturnSuccess) {
            out->failedWrite = i;
            return rc;
        }
    }
    if (!(in->flags & AMDHV_RUN_NO_RUN)) {
        rc = run((in->flags & AMDHV_RUN_SKIP) != 0, &out->exit, deadline);
        if (rc != kIOReturnSuccess)
            return rc;
    }

    getRegs(&out->regs);
    unsigned n = 0;
    IOLockLock(fLock);                                    /* one lock for all the fields, not one per field */
#define AMDHV_X(enc) do { uint64_t v_ = 0; (void)fVmcs.read(enc, &v_); out->fields[n++] = v_; } while (0);
    AMDHV_RUN_FIELDS_LIST(AMDHV_X)
#undef AMDHV_X
    IOLockUnlock(fLock);
    return kIOReturnSuccess;
}

IOReturn com_amdhv_AMDHVVm::vmcsWrite(uint32_t field, uint64_t value)
{
    /* MOV to DR0-DR3 faults on a non-canonical address, and the kext loads these in kernel mode. */
    if (field >= amdhv::vmx::X_DR0 && field <= amdhv::vmx::X_DR3 && (uint64_t)(((int64_t)(value << 16)) >> 16) != value)
        return kIOReturnBadArgument;
    IOLockLock(fLock);
    amdhv::vmx::Result r = fVmcs.write(field, value);
    if (r == amdhv::vmx::R_OK && field == amdhv::vmx::F_GUEST_RIP)
        fHaveNextRip = false;           /* the client moved RIP itself */
    /*
     * Paging state written through the API: the TLB may still hold translations for the old CR3/paging
     * mode, and VMRUN does not flush (that is what ASIDs are for). The client normally calls
     * hv_vcpu_invalidate_tlb for this; do it here as well so a client that forgets cannot run on stale
     * translations. It costs one flush on the next entry, and these writes are rare.
     */
    if (r == amdhv::vmx::R_OK && (field == amdhv::vmx::F_GUEST_CR3 || field == amdhv::vmx::F_GUEST_CR0 ||
                                  field == amdhv::vmx::F_GUEST_CR4 || field == amdhv::vmx::F_GUEST_EFER))
        __atomic_store_n(&fForceTlbFlush, 1, __ATOMIC_RELEASE);
    IOLockUnlock(fLock);
    switch (r) {
    case amdhv::vmx::R_OK:         return kIOReturnSuccess;
    case amdhv::vmx::R_READ_ONLY:  return kIOReturnNotWritable;
    default:                     return kIOReturnBadArgument;
    }
}

void com_amdhv_AMDHVVm::dumpVmcb(const char *why)
{
    const Vmcb *v = (const Vmcb *)fVmcb.va;
    AMDHV_ERR("VMCB dump: %s", why);
    AMDHV_ERR("  efer=0x%llx cr0=0x%llx cr3=0x%llx cr4=0x%llx rflags=0x%llx rip=0x%llx",
            (unsigned long long)v->save.efer, (unsigned long long)v->save.cr0,
            (unsigned long long)v->save.cr3, (unsigned long long)v->save.cr4,
            (unsigned long long)v->save.rflags, (unsigned long long)v->save.rip);
    AMDHV_ERR("  cs sel=0x%x base=0x%llx lim=0x%x attr=0x%x  dr6=0x%llx dr7=0x%llx",
            v->save.cs.selector, (unsigned long long)v->save.cs.base, v->save.cs.limit,
            v->save.cs.attrib, (unsigned long long)v->save.dr6, (unsigned long long)v->save.dr7);
    AMDHV_ERR("  asid=%u nested_ctl=0x%llx n_cr3=0x%llx v3=0x%x v4=0x%x int_ctl=0x%x",
            v->control.asid, (unsigned long long)v->control.nestedCtl,
            (unsigned long long)v->control.nestedCr3, v->control.interceptVec3,
            v->control.interceptVec4, v->control.intCtl);
    AMDHV_ERR("  iopm=0x%llx msrpm=0x%llx exit=0x%llx info1=0x%llx info2=0x%llx",
            (unsigned long long)v->control.iopmBasePa, (unsigned long long)v->control.msrpmBasePa,
            (unsigned long long)v->control.exitCode, (unsigned long long)v->control.exitInfo1,
            (unsigned long long)v->control.exitInfo2);
}
