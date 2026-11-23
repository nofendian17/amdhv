#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <string.h>
#include <mach/kern_return.h>
#include <pexpert/pexpert.h>
#include <libkern/OSAtomic.h>

#include "Frontend.hpp"
#include "SVM.hpp"
#include "HvSupportDecode.hpp"
#include "HvMsrs.hpp"
#include "HvSlots.hpp"
#include "AMDHV.hpp"
#include "KernelDecls.hpp"
#include "Vm.hpp"
#include "VmcsCaps.hpp"
#include "TscOffset.hpp"
#include "ApicState.hpp"
#include "Log.hpp"

/*
 * Native mode: the kext registers itself with xnu as the hypervisor provider, in the place of Apple's AppleHV, so that
 * the unmodified Hypervisor.framework talks to this kext. See docs/NATIVE.md of the RyzenHV project this comes from.
 *
 * The kernel side of the interface below is public (xnu osfmk/kern/hv_support_kext.h), the declarations are repeated
 * here so that the build does not depend on which private headers the SDK ships.
 */
extern "C" {
typedef kern_return_t (*hv_trap_t)(void *target, uint64_t arg);

typedef struct {
    void (*dispatch)(void *vcpu);
    void (*preempt)(void *vcpu);
    void (*suspend)(void);
    void (*thread_destroy)(void *vcpu);
    void (*task_destroy)(void *vm);
    void (*volatile_state)(void *vcpu, int state);
    void (*resume)(void);
    void (*memory_pressure)(void);
} hv_callbacks_t;

extern int hv_get_support(void);                          /* returns hv_support_available: used to read the result back, and its code names the address */
extern "C" int sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);   /* reads kern.hv_support through its OID */
extern kern_return_t hv_set_traps(int trap_type, const hv_trap_t *traps, unsigned trap_count);
extern void hv_release_traps(int trap_type);
extern kern_return_t hv_set_callbacks(hv_callbacks_t callbacks);
extern void hv_release_callbacks(void);
extern void hv_set_task_target(void *target);
extern void hv_set_thread_target(void *target);
extern void *hv_get_task_target(void);
extern void *hv_get_thread_target(void);
}

namespace {


enum { HV_TASK_TRAP = 0, HV_THREAD_TRAP = 1 };
constexpr unsigned kTraps = 16;                       /* the framework uses indices 0..13 */
constexpr kern_return_t kHvUnsupported = (kern_return_t)0xfae9400f;
constexpr kern_return_t kHvUnsupportedCode = kHvUnsupported;

bool gRegistered = false;
uint32_t gMode = 1;          /* 1: answer HV_UNSUPPORTED, 2: answer success so the client goes on and more traps show up */
volatile SInt32 gCount[2][kTraps];

const char *const kTaskNames[kTraps] = {
    "vm_create", "vm_destroy", "space_create", "space_destroy", "vm_map", "vm_unmap", "vm_protect", "vm_sync_tsc",
    "add_pio_notifier", "rem_pio_notifier", "intctl(atpic/ioapic/lapic)", "lapic_msi", "vcpu_create", "vcpu_interrupt", "?14", "?15"};
const char *const kThreadNames[kTraps] = {
    "vcpu_run", "vcpu_run_until", "invalidate_tlb", "enable_managed_msr", "disable_managed_msr", "vcpu_attach/destroy",
    "set_space", "read_vmcs(slot)", "set_shadow_access", "set_apic_address", "set_msr_access", "sync_special_regs", "get_cap_write_vmcs",
    "apic(sub-op)", "?14", "?15"};

/*
 * Logs the call, with the argument as a number and, when it looks like a user pointer that can be read, up to eight
 * words behind it. copyin() fails cleanly for a value that is not a readable address (a deadline, a mask, a slot).
 */
void logTrap(unsigned type, unsigned index, uint64_t arg, SInt32 n)
{
    const char *name = (type == HV_TASK_TRAP ? kTaskNames : kThreadNames)[index];
    uint64_t w[8] = {0};
    unsigned words = 0;
    if (arg >= 0x1000 && arg < 0x00007fffffffffffULL && (arg & 7) == 0) {
        static const unsigned sizes[] = {64, 40, 16, 8};
        for (unsigned i = 0; i < 4 && !words; i++)
            if (copyin((user_addr_t)arg, w, sizes[i]) == 0)
                words = sizes[i] / 8;
    }
    if (words >= 5)
        AMDHV_INFO("trap %s %u %s #%d pid %d arg=%#llx [%#llx %#llx %#llx %#llx %#llx%s]", type == HV_TASK_TRAP ? "task" : "thread", index,
                 name, (int)n, proc_selfpid(), arg, w[0], w[1], w[2], w[3], w[4], words > 5 ? " ..." : "");
    else if (words >= 2)
        AMDHV_INFO("trap %s %u %s #%d pid %d arg=%#llx [%#llx %#llx]", type == HV_TASK_TRAP ? "task" : "thread", index, name, (int)n,
                 proc_selfpid(), arg, w[0], w[1]);
    else if (words == 1)
        AMDHV_INFO("trap %s %u %s #%d pid %d arg=%#llx [%#llx]", type == HV_TASK_TRAP ? "task" : "thread", index, name, (int)n,
                 proc_selfpid(), arg, w[0]);
    else
        AMDHV_INFO("trap %s %u %s #%d pid %d arg=%#llx", type == HV_TASK_TRAP ? "task" : "thread", index, name, (int)n, proc_selfpid(),
                 arg);
}

/* ------------------------------------------------------------------------------------------------------------------------
 * The VM and vCPU objects and the shared pages (layout: the framework's own, mirrored here). The register/VMCS cache
 * protocol of the framework lives in these pages: the client writes what it changed before a run into the rw slice,
 * the kext applies it, runs the guest on the SVM core and writes the exit state back.
 * ---------------------------------------------------------------------------------------------------------------------- */

constexpr kern_return_t kHvOk = 0;
constexpr kern_return_t kHvError = (kern_return_t)0xfae94001;
constexpr kern_return_t kHvBusy = (kern_return_t)0xfae94002;
constexpr kern_return_t kHvBadArg = (kern_return_t)0xfae94003;
constexpr kern_return_t kHvNoRes = (kern_return_t)0xfae94005;
constexpr kern_return_t kHvNoDev = (kern_return_t)0xfae94006;

constexpr unsigned kMaxVcpus = 64;
constexpr uint32_t kRwSlice = 0xe18, kRoSlice = 0x6d8;           /* per-vCPU slices, fixed in the framework */
constexpr uint32_t kRwFpOff = kMaxVcpus * kRwSlice;              /* 0x38600 */
constexpr uint32_t kRoFpOff = kMaxVcpus * kRoSlice;              /* 0x1b600 */
constexpr uint32_t kCacheSlots = 93;                             /* slots 0..92 live in the rw slice; the rest belongs to the kernel */
constexpr uint32_t kRwMsrVal = 0x7d0, kRwMsrValid = 0xc50;      /* cache of MSR values by index, and its valid bitmap (3 words) */
constexpr uint32_t kRoMsrPos = 0x58;                              /* ro: one byte per MSR index, position in the managed list, 0xff = not managed */
constexpr uint32_t kRwValid = 0x2f0, kRwDirty = 0x300, kRwRegs = 0xc80;   /* valid and dirty bitmaps (2 words each), the register array */
constexpr uint32_t kRwTscRelative = 0xc78;                       /* hv_vcpu_set_tsc_relative value, applied when flag 0x100 is set */
constexpr uint32_t kRoSynced = 0xe8, kRoCapMin = 0xf1, kRoCapMax = 0x3d9, kRoCapValid = 0x6c1;   /* ro slice (the arrays are unaligned) */
constexpr uint32_t kInfoBytes = 0x118;

/* VMX capability words offered to the client, indexed by HV_VMX_CAP_*: low 32 bits are the "must be 1" settings,
 * high 32 bits the "may be 1" settings, the same numbers hv_vmx_read_capability returns. */
/* The VMX capability words come from VmcsCaps.hpp, derived from the controls this kext implements. A word is built per VM
 * so that the TPR-shadow switch is reflected in the VMs created after it changes. */

/* VMs that exist. The kernel calls the callbacks WITHOUT a NULL check for every thread and task that has a target, so while this is above 0 the
 * callbacks must stay registered: unregistering is refused. */
volatile SInt32 gLiveVms = 0;

/* The TPR shadow (VMX control bit 21) is offered to every VM: it lets Windows change its CR8 without a VM exit, which is most of the exits of
 * a Windows boot. On by default; the boot-arg amdhv_tpr_shadow=0 or `amd-info hv tpr 0` turns it off for the VMs created from then on. */
volatile int gTprShadow = 1;
volatile int gApicAccel = 0;                           /* amdhv_apic=1: let clients ask for hv_vm_create(HV_VM_ACCEL_APIC) */

volatile int *gSupportPtr = nullptr;                 /* the kernel's hv_support_available once located and written */
int gSupportSaved = 0;
bool gSupportSet = false;

com_amdhv_AMDHV *gSvc = nullptr;     /* for the CPU info a vCPU object needs; set by start(), lives as long as the kext */

struct HvVm;
struct HvVcpu {
    HvVm *vm;
    uint32_t id;
    uint8_t *rw;       /* kernel address of this vCPU's rw slice */
    uint8_t *ro;
    com_amdhv_AMDHVVm *cpu;   /* the vCPU object of the SVM core, joined to vm->shared */
    AMDHVRunIn  in;      /* the run call's buffers live here, not on the kernel stack */
    AMDHVRunOut out;
    AMDHVFpuState fpu;
    uint8_t *msrPage;          /* this vCPU's 4 KiB page in the ro area: the managed MSR list, entries of 16 bytes {msr, value} */
    uint32_t nManaged;
    bool     cpuidPending;     /* the last exit was a CPUID leaf 7 subleaf 0: its answer loses PKU/OSPKE on the way back in */
    uint32_t cpuidLeaf;
    uint32_t cpuidSub;
    uint64_t cr2Page;          /* the CR2 word of the page as this driver last left it: a different value is the client's write (an injected #PF needs it) and is applied */
    uint64_t tprPage;          /* the TPR word of the page as this driver last left it: the client's write is applied when it differs */
    bool     tprLogged;        /* the client's first TPR-shadow use is logged once */
    uint8_t  managedIdx[256];  /* position in the list -> MSR cache index, 0 = a hole */
    amdhv::apic::LapicState lapic;  /* in-kernel APIC state of this vCPU */
    uint64_t applied[144];     /* the last value handed to the core for each MSR cache index */
    uint8_t  appliedKnown[144];
};
struct HvVm {
    IOLock *lock;
    volatile int inFlight;             /* traps currently holding this VM (guarded by `lock`) */
    bool     dying;                    /* tVmDestroy has claimed it; new traps must fail with Busy */
    IOBufferMemoryDescriptor *md[3];   /* rw, ro, commpage */
    IOMemoryMap *map[3];
    uint8_t *kva[3];
    com_amdhv_AMDHVVmShared *shared;   /* the nested page tables and the guest memory pinned from the client */
    uint32_t fpStride;                         /* bytes per vCPU in the two FP-state arrays */
    uint32_t roArrOff;                         /* offset of the per-vCPU 4 KiB pages (managed MSR lists) in the ro area */
    uint64_t tscOffset;                        /* from hv_vm_sync_tsc, handed to vCPUs created later */
    bool     tscValid;
    bool     apicAccel;                        /* created with HV_VM_ACCEL_APIC (opt-in, see gApicAccel) */
    uint64_t caps[amdhv::vmx::CAP_COUNT];      /* the VMX capability words this VM's client was shown (HV_VMX_CAP_*), also what thread trap 12 answers from */
    amdhv::apic::IoApicState ioapic;           /* in-kernel APIC state (opt-in VMs) */
    amdhv::apic::AtpicState atpic[2];          /* primary and secondary 8259A */
    HvVcpu *vcpu[kMaxVcpus];
    uint32_t nVcpus;
};

inline uint32_t roundPage(uint32_t v) { return (v + 0xfffu) & ~0xfffu; }

uint32_t xsaveBytes()
{
    uint32_t a = 0xd, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if (c < 0x240 || c > 0x4000)
        c = 0x340;                                  /* a size every AVX-capable CPU fits in; the framework only reads it */
    return c;
}

/* Every per-VM trap must pin the VM before touching it: tVmDestroy can otherwise free it mid-trap. */
inline bool vmPin(HvVm *vm)
{
    if (!vm)
        return false;
    IOLockLock(vm->lock);
    if (vm->dying) {
        IOLockUnlock(vm->lock);
        return false;
    }
    vm->inFlight++;
    IOLockUnlock(vm->lock);
    return true;
}

inline void vmUnpin(HvVm *vm)
{
    if (!vm)
        return;
    IOLockLock(vm->lock);
    if (vm->inFlight > 0)
        vm->inFlight--;
    IOLockUnlock(vm->lock);
}

void vmFree(HvVm *vm)
{
    if (!vm)
        return;
    if (vm->shared)
        vm->shared->release();                       /* unpins everything that is still mapped */
    for (unsigned i = 0; i < kMaxVcpus; i++) {
        if (vm->vcpu[i]) {
            if (vm->vcpu[i]->cpu)
                vm->vcpu[i]->cpu->release();
            IOFree(vm->vcpu[i], sizeof(HvVcpu));
        }
    }
    for (int i = 2; i >= 0; i--) {
        if (vm->map[i])
            vm->map[i]->release();
        if (vm->md[i])
            vm->md[i]->release();
    }
    if (vm->lock)
        IOLockFree(vm->lock);
    IOFree(vm, sizeof(HvVm));
    OSDecrementAtomic(&gLiveVms);
}

/* Allocates the three areas, maps them into the calling task, fills the interface block. */
HvVm *vmCreate(uint64_t *info)
{
    HvVm *vm = (HvVm *)IOMallocZero(sizeof(HvVm));
    if (!vm)
        return nullptr;
    amdhv::apic::reset(vm->ioapic);
    amdhv::apic::reset(vm->atpic[0]);
    amdhv::apic::reset(vm->atpic[1]);
    OSIncrementAtomic(&gLiveVms);                    /* balanced by vmFree, which every failure path below also reaches */
    vm->lock = IOLockAlloc();
    if (!vm->lock) {
        vmFree(vm);
        return nullptr;
    }

    uint32_t fpStride = (xsaveBytes() + 63u) & ~63u;
    uint32_t roOffArr = roundPage(kRoFpOff + kMaxVcpus * fpStride);
    uint32_t sizes[3] = {roundPage(kRwFpOff + kMaxVcpus * fpStride), roOffArr + kMaxVcpus * 0x1000u, 0x1000u};

    task_t task = current_task();
    for (int i = 0; i < 3; i++) {
        vm->md[i] = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                                                               kIODirectionInOut | kIOMemoryKernelUserShared |
                                                                   kIOMemoryMapperNone,
                                                               sizes[i], 4096);
        if (!vm->md[i]) {
            vmFree(vm);
            return nullptr;
        }
        vm->kva[i] = (uint8_t *)vm->md[i]->getBytesNoCopy();
        bzero(vm->kva[i], sizes[i]);
        if (i == 2) {
            /* hv_vmx_tsc_clock (Hypervisor.framework) is a seqlock over this page: it spins until the 32-bit sequence at +0x10 is non-zero,
             * then returns rdtsc + [+8] - [+0]. Zero in both is the host TSC itself. */
            *(volatile uint32_t *)(vm->kva[2] + 0x10) = 1;
        }
        /* The ro area and the commpage are read-only for the client. */
        vm->map[i] = vm->md[i]->createMappingInTask(task, 0, kIOMapAnywhere | (i == 0 ? 0 : kIOMapReadOnly));
        if (!vm->map[i]) {
            vmFree(vm);
            return nullptr;
        }
    }

    vm->fpStride = fpStride;
    vm->roArrOff = roOffArr;
    IOReturn err = kIOReturnSuccess;
    vm->shared = com_amdhv_AMDHVVmShared::create(task, &err);
    if (!vm->shared) {
        AMDHV_WARN("hv front end: cannot create the shared VM (0x%x)", err);
        vmFree(vm);
        return nullptr;
    }

    bzero(info, kInfoBytes);
    info[0x00 / 8] = kRwFpOff;
    info[0x08 / 8] = kRoFpOff;
    info[0x10 / 8] = fpStride;
    info[0x18 / 8] = roOffArr;
    info[0x20 / 8] = 0x1000;
    uint64_t caps[amdhv::vmx::CAP_COUNT];
    for (unsigned c = 0; c < amdhv::vmx::CAP_COUNT; c++)
        caps[c] = amdhv::vmx::capability(c, gTprShadow != 0);
    for (unsigned c = 0; c < amdhv::vmx::CAP_COUNT; c++) {
        info[(0x28 + 8 * c) / 8] = caps[c];
        vm->caps[c] = caps[c];
    }
    info[0x100 / 8] = vm->map[0]->getAddress();
    info[0x108 / 8] = vm->map[1]->getAddress();
    info[0x110 / 8] = vm->map[2]->getAddress();
    return vm;
}

volatile SInt32 gTraceCount[2][16];

void note(unsigned type, unsigned index, uint64_t arg)
{
    SInt32 n = OSIncrementAtomic((volatile SInt32 *)&gTraceCount[type][index]) + 1;
    if (n <= 24 || (n % 1000) == 0)
        logTrap(type, index, arg, n);
}

kern_return_t hvFromIo(IOReturn r);
inline kern_return_t hvFromIoEarly(IOReturn r) { return hvFromIo(r); }

kern_return_t tVmCreate(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 0, arg);
    if (target)
        return kHvBusy;                              /* this task has a VM already */
    struct { uint64_t flags, info; } a = {0, 0};
    if (copyin((user_addr_t)arg, &a, sizeof(a)))
        return kHvError;
    if ((a.flags & 0x400) && !gApicAccel)
        return kHvUnsupportedCode;              /* the in-kernel APIC variant is opt-in while its handlers are built */
    uint64_t info[kInfoBytes / 8];
    HvVm *vm = vmCreate(info);
    if (!vm)
        return kHvNoRes;
    vm->apicAccel = (a.flags & 0x400) != 0;
    if (vm->apicAccel)
        AMDHV_INFO("hv front end: VM created with the in-kernel APIC requested");
    if (copyout(info, (user_addr_t)a.info, kInfoBytes)) {
        vmFree(vm);
        return kHvError;
    }
    hv_set_task_target(vm);
    return kHvOk;
}

kern_return_t tVmDestroy(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 1, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    IOLockLock(vm->lock);
    unsigned n = vm->nVcpus;
    int inflight = vm->inFlight;
    if (!n && !inflight)
        vm->dying = true;
    IOLockUnlock(vm->lock);
    if (n || inflight)
        return kHvBusy;                              /* a trap or vCPU still holds it: never free under it */
    hv_set_task_target(nullptr);
    vmFree(vm);
    {                                   /* this shows that the teardown came back (a hung exit has no such line) */
        static volatile SInt32 doneLogged;
        if (OSIncrementAtomic(&doneLogged) < 40)
            AMDHV_INFO("hv front end: vm_destroy done (pid %d)", proc_selfpid());
    }
    return kHvOk;
}

void fpuToPage(HvVcpu *v);

kern_return_t tVcpuCreate(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 12, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    if (!vmPin(vm))
        return kHvBusy;
    struct { uint64_t idPtr, flags; } a = {0, 0};
    if (copyin((user_addr_t)arg, &a, sizeof(a))) {
        vmUnpin(vm);
        return kHvError;
    }
    if (a.flags > 3) {
        vmUnpin(vm);
        return kHvBadArg;
    }
    if (hv_get_thread_target()) {
        vmUnpin(vm);
        return kHvBusy;                              /* this thread owns a vCPU already */
    }
    if (!gSvc) {
        vmUnpin(vm);
        return kHvNoDev;
    }
    HvVcpu *v = (HvVcpu *)IOMallocZero(sizeof(HvVcpu));
    if (!v) {
        vmUnpin(vm);
        return kHvNoRes;
    }
    amdhv::apic::reset(v->lapic);
    {
        amdhv::svm::rescan();       /* CPUs that came online after the kext started */
        AMDHVInfo info;
        gSvc->getInfo(&info);
        IOReturn err = kIOReturnSuccess;
        v->cpu = com_amdhv_AMDHVVm::create(info, current_task(), vm->shared, &err);
        if (!v->cpu) {
            AMDHV_WARN("hv front end: cannot create the vCPU object (0x%x)", err);
            IOFree(v, sizeof(HvVcpu));
            vmUnpin(vm);
            return hvFromIoEarly(err);
        }
    }
    IOLockLock(vm->lock);
    unsigned id = 0;
    while (id < kMaxVcpus && vm->vcpu[id])
        id++;                                      /* the next free slot */
    if (id == kMaxVcpus) {
        IOLockUnlock(vm->lock);
        v->cpu->release();
        IOFree(v, sizeof(HvVcpu));
        vmUnpin(vm);
        return kHvNoDev;
    }
    v->vm = vm;
    v->id = id;
    v->rw = vm->kva[0] + id * kRwSlice;
    v->ro = vm->kva[1] + id * kRoSlice;
    bzero(v->rw, kRwSlice);
    bzero(v->ro, kRoSlice);
    memset(v->ro + kRoMsrPos, 0xff, amdhv::hv::kMsrCacheMax + 1);               /* no MSR is managed yet */
    v->msrPage = vm->kva[1] + vm->roArrOff + id * 0x1000u;
    bzero(v->msrPage, 0x1000);
    if (vm->tscValid)
        v->cpu->vmcsWrite(0x2010, vm->tscOffset);      /* a vCPU created after hv_vm_sync_tsc gets the same offset */
    vm->vcpu[id] = v;
    vm->nVcpus++;
    IOLockUnlock(vm->lock);

    uint32_t outId = id;
    if (copyout(&outId, (user_addr_t)a.idPtr, sizeof(outId))) {
        IOLockLock(vm->lock);
        vm->vcpu[id] = nullptr;
        vm->nVcpus--;
        IOLockUnlock(vm->lock);
        v->cpu->release();
        IOFree(v, sizeof(HvVcpu));
        vmUnpin(vm);
        return kHvError;
    }
    fpuToPage(v);                                    /* initial FP state and XCR0 into the page */
    hv_set_thread_target(v);
    vmUnpin(vm);
    return kHvOk;
}

kern_return_t tVcpuDestroy(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 5, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    HvVm *vm = v->vm;
    const unsigned id = v->id;
    hv_set_thread_target(nullptr);
    IOLockLock(vm->lock);
    vm->vcpu[v->id] = nullptr;
    vm->nVcpus--;
    IOLockUnlock(vm->lock);
    v->cpu->release();
    IOFree(v, sizeof(HvVcpu));
    {
        static volatile SInt32 doneLogged;
        if (OSIncrementAtomic(&doneLogged) < 60)
            AMDHV_INFO("hv front end: vcpu%u destroy done (pid %d)", id, proc_selfpid());
    }
    return kHvOk;
}

/* The cache words of a vCPU's rw slice. */
inline uint64_t *slotWord(HvVcpu *v, unsigned slot) { return reinterpret_cast<uint64_t *>(v->rw + 8 + slot * 8); }
inline uint64_t *validWord(HvVcpu *v, unsigned w) { return reinterpret_cast<uint64_t *>(v->rw + kRwValid + w * 8); }
inline uint64_t *dirtyWord(HvVcpu *v, unsigned w) { return reinterpret_cast<uint64_t *>(v->rw + kRwDirty + w * 8); }
inline uint64_t *gprWord(HvVcpu *v, unsigned reg) { return reinterpret_cast<uint64_t *>(v->rw + kRwRegs + reg * 8); }
inline void markValid(HvVcpu *v, unsigned slot) { *validWord(v, slot >> 6) |= 1ull << (slot & 63); }
inline uint64_t *msrValWord(HvVcpu *v, unsigned idx) { return reinterpret_cast<uint64_t *>(v->rw + kRwMsrVal + idx * 8); }
inline uint64_t *msrValidWord(HvVcpu *v, unsigned w) { return reinterpret_cast<uint64_t *>(v->rw + kRwMsrValid + w * 8); }
inline bool msrIsValid(HvVcpu *v, unsigned idx) { return (*msrValidWord(v, idx >> 6) >> (idx & 63)) & 1; }
inline void msrSetValid(HvVcpu *v, unsigned idx, bool on)
{
    if (on)
        *msrValidWord(v, idx >> 6) |= 1ull << (idx & 63);
    else
        *msrValidWord(v, idx >> 6) &= ~(1ull << (idx & 63));
}
constexpr unsigned R_XCR0 = 50, R_DR0 = 41, R_DR4 = 45, R_DR5 = 46, R_DR6 = 47;   /* registers kept in the page, by the framework's numbers */

/* The core's FP state and XCR0 into the page: the ro FP array (what hv_vcpu_read_fpstate reads) and the XCR0 register. */
void fpuToPage(HvVcpu *v)
{
    v->cpu->getFpu(&v->fpu);
    uint8_t *dst = v->vm->kva[1] + kRoFpOff + v->id * v->vm->fpStride;
    uint32_t n = v->fpu.size < v->vm->fpStride ? v->fpu.size : v->vm->fpStride;
    bcopy(v->fpu.area, dst, n);
    *gprWord(v, R_XCR0) = v->fpu.xcr0;
}

/* Thread trap 7: the client read a slot that is not valid in its cache: ask the SVM core for the field and put it there. */
kern_return_t tReadSlot(void *target, uint64_t slot)
{
    note(HV_THREAD_TRAP, 7, slot);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    if (slot == 0 || slot >= kCacheSlots)
        return kHvBadArg;
    uint32_t field = amdhv::hv::kSlotField[slot];
    uint64_t val = 0;
    IOReturn rc = v->cpu->vmcsRead(field, &val);
    if (rc != kIOReturnSuccess)
        return hvFromIo(rc);
    *slotWord(v, (unsigned)slot) = val;
    markValid(v, (unsigned)slot);
    return kHvOk;
}

/* Thread trap 11: the client wants DR0..DR7 and the page does not have them current: read them from the core (DR7 is a VMCS slot, the client
 * gets it with the other slots; DR4 and DR5 alias DR6 and DR7). */
kern_return_t tSyncSpecial(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 11, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    uint64_t val = 0;
    for (unsigned n = 0; n < 4; n++) {
        val = 0;
        v->cpu->vmcsRead(0xF010 + 2 * n, &val);
        *gprWord(v, R_DR0 + n) = val;
    }
    val = 0;
    v->cpu->vmcsRead(0xF002, &val);
    *gprWord(v, R_DR6) = val;
    *gprWord(v, R_DR4) = val;
    val = 0;
    v->cpu->vmcsRead(0x681a, &val);
    *gprWord(v, R_DR5) = val;
    v->ro[kRoSynced] |= 1;
    return kHvOk;
}

/* Thread trap 12: capability of a writable field ("may be 0" / "may be 1" words). The control words of VMX have limits: the bits that must be 1
 * (allowed 0-settings, the low half of the capability word) and the bits that may be 1 (allowed 1-settings, the high half), the same numbers
 * hv_vmx_read_capability gives. Everything else (guest state, 64-bit fields) can be written freely. */
kern_return_t tCapWrite(void *target, uint64_t slot)
{
    note(HV_THREAD_TRAP, 12, slot);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    if (slot >= kCacheSlots)
        return kHvBadArg;
    uint64_t lo = 0, hi = ~0ull, valid;
    int ci = amdhv::vmx::capIndexForControl(amdhv::hv::kSlotField[slot]);
    if (ci >= 0) {
        lo = v->vm->caps[ci] & 0xFFFFFFFFull;
        hi = v->vm->caps[ci] >> 32;
    }
    __builtin_memcpy(v->ro + kRoCapMin + slot * 8, &lo, 8);
    __builtin_memcpy(v->ro + kRoCapMax + slot * 8, &hi, 8);
    __builtin_memcpy(&valid, v->ro + kRoCapValid + (slot >> 6) * 8, 8);
    valid |= 1ull << (slot & 63);
    __builtin_memcpy(v->ro + kRoCapValid + (slot >> 6) * 8, &valid, 8);
    return kHvOk;
}

kern_return_t tInvalidateTlb(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 2, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v || !v->cpu)
        return kHvNoDev;
    /* The guest TLB is no longer flushed on every entry, so this call must actually schedule one. */
    v->cpu->forceTlbFlush();
    return kHvOk;
}

/* Managed MSRs (thread traps 3 enable, 4 disable, 10 set access): the guest uses the MSR natively; the client reads its value from the
 * per-vCPU list in the ro area. */
/* MSRs the framework keeps in VMCS slots (indices 145..150: SYSENTER_CS/ESP/EIP, FS_BASE, GS_BASE, DEBUGCTL): their values already travel
 * through the slot cache, only the guest's native access is switched here. Their bytes in the ro position table do not exist (they would
 * land on the synced flag and the mode byte), so nothing is written there. */
uint32_t slotMsr(unsigned idx)
{
    switch (idx) {
    case 145: return 0x174;
    case 146: return 0x175;
    case 147: return 0x176;
    case 148: return 0xc0000100;
    case 149: return 0xc0000101;
    case 150: return 0x1d9;
    case 144: return 0x10;          /* the TSC: the guest reads it natively, the offset is the VMCB's */
    }
    return 0;
}

kern_return_t managedOn(HvVcpu *v, unsigned idx)
{
    if (uint32_t sm = slotMsr(idx)) {
        uint64_t unused = 0;
        return hvFromIo(v->cpu->msrOp(AMDHV_MSR_NATIVE_ENABLE, sm, 0, &unused));
    }
    if (idx < 1 || idx > amdhv::hv::kMsrCacheMax || !amdhv::hv::kMsrOfIdx[idx])
        return kHvBadArg;
    if (v->ro[kRoMsrPos + idx] != 0xff)
        return kHvOk;                                     /* already managed */
    if (v->nManaged >= 255)
        return kHvNoRes;
    uint32_t msr = amdhv::hv::kMsrOfIdx[idx];
    uint64_t val = 0;
    IOReturn rc = v->cpu->msrOp(AMDHV_MSR_NATIVE_ENABLE, msr, 0, &val);
    if (rc != kIOReturnSuccess)
        return hvFromIo(rc);
    v->cpu->msrOp(AMDHV_MSR_READ, msr, 0, &val);
    unsigned pos = v->nManaged++;
    v->managedIdx[pos] = (uint8_t)idx;
    __builtin_memcpy(v->msrPage + pos * 16, &msr, 4);
    __builtin_memcpy(v->msrPage + pos * 16 + 8, &val, 8);
    v->ro[kRoMsrPos + idx] = (uint8_t)pos;
    msrSetValid(v, idx, false);
    return kHvOk;
}

kern_return_t managedOff(HvVcpu *v, unsigned idx)
{
    if (uint32_t sm = slotMsr(idx)) {
        uint64_t unused = 0;
        return hvFromIo(v->cpu->msrOp(AMDHV_MSR_NATIVE_DISABLE, sm, 0, &unused));
    }
    if (idx < 1 || idx > amdhv::hv::kMsrCacheMax || !amdhv::hv::kMsrOfIdx[idx])
        return kHvBadArg;
    uint8_t pos = v->ro[kRoMsrPos + idx];
    if (pos == 0xff)
        return kHvOk;
    uint64_t val = 0;
    __builtin_memcpy(&val, v->msrPage + pos * 16 + 8, 8);
    IOReturn rc = v->cpu->msrOp(AMDHV_MSR_NATIVE_DISABLE, amdhv::hv::kMsrOfIdx[idx], 0, &val);
    v->managedIdx[pos] = 0;                                /* the list keeps a hole, positions never move */
    v->ro[kRoMsrPos + idx] = 0xff;
    __builtin_memcpy(&val, v->msrPage + pos * 16 + 8, 8);
    *msrValWord(v, idx) = val;                             /* the last value stays readable from the cache */
    msrSetValid(v, idx, true);
    v->applied[idx] = val;
    v->appliedKnown[idx] = 1;
    return rc == kIOReturnSuccess ? kHvOk : hvFromIo(rc);
}

kern_return_t tEnableManaged(void *target, uint64_t idx)
{
    note(HV_THREAD_TRAP, 3, idx);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    return v ? managedOn(v, (unsigned)idx) : kHvNoDev;
}

kern_return_t tDisableManaged(void *target, uint64_t idx)
{
    note(HV_THREAD_TRAP, 4, idx);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    return v ? managedOff(v, (unsigned)idx) : kHvNoDev;
}

/* arg = (access << 32) | cache index; any access means "the guest uses it without exits". */
kern_return_t tSetMsrAccess(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 10, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    return (arg >> 32) ? managedOn(v, (unsigned)arg) : managedOff(v, (unsigned)arg);
}

/* hv_vm_sync_tsc: the guest sees `tsc` now; every vCPU gets the same offset. */
kern_return_t tVmSyncTsc(void *target, uint64_t tsc)
{
    note(HV_TASK_TRAP, 7, tsc);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    uint64_t offset = tsc - (((uint64_t)hi << 32) | lo);
    IOLockLock(vm->lock);
    vm->tscOffset = offset;
    vm->tscValid = true;
    for (unsigned id = 0; id < kMaxVcpus; id++)
        if (vm->vcpu[id] && vm->vcpu[id]->cpu)
            vm->vcpu[id]->cpu->vmcsWrite(0x2010, offset);
    IOLockUnlock(vm->lock);
    return kHvOk;
}

/* The general registers live in the page, indexed by the framework's register number. */
enum { R_RAX = 2, R_RCX = 3, R_RDX = 4, R_RBX = 5, R_RSI = 6, R_RDI = 7, R_RSP = 8, R_RBP = 9, R_R8 = 10, R_CR2 = 38, R_TPR = 49 };
constexpr unsigned kSlotRsp = 75, kSlotRip = 76, kSlotRflags = 77;

const uint32_t kRunFields[AMDHV_RUN_NFIELDS] = {
#define AMDHV_X(enc) enc,
    AMDHV_RUN_FIELDS_LIST(AMDHV_X)
#undef AMDHV_X
};

static kern_return_t vcpuRun(void *target, uint64_t deadline)
{
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    com_amdhv_AMDHVVm *cpu = v->cpu;

    /* 1. What the client changed since the last run: registers from the page, VMCS fields from the dirty slots. */
    /* No PKU/OSPKE for every client: macOS never enables PKRU in XCR0, so the guest cannot get it in its XSAVE state; a Linux guest that sees
     * PKU sets CR4.PKE anyway and then fails every signal frame (get_xsave_addr_user(PKRU) is NULL) - the host's leaf 7 passes the bits through. */
    if (v->cpuidPending && v->cpuidLeaf == 7 && v->cpuidSub == 0)
        *gprWord(v, R_RCX) &= ~((1ull << 3) | (1ull << 4));
    v->cpuidPending = false;

    AMDHVRegs regs;
    cpu->getRegs(&regs);
    regs.rax = *gprWord(v, R_RAX);
    regs.rcx = *gprWord(v, R_RCX);
    regs.rdx = *gprWord(v, R_RDX);
    regs.rbx = *gprWord(v, R_RBX);
    regs.rsi = *gprWord(v, R_RSI);
    regs.rdi = *gprWord(v, R_RDI);
    regs.rbp = *gprWord(v, R_RBP);
    uint64_t *r8_15[8] = {&regs.r8, &regs.r9, &regs.r10, &regs.r11, &regs.r12, &regs.r13, &regs.r14, &regs.r15};
    for (unsigned i = 0; i < 8; i++)
        *r8_15[i] = *gprWord(v, R_R8 + i);
    const uint64_t flags = *reinterpret_cast<uint64_t *>(v->rw);   /* 0x01 slots, 0x04 MSRs, 0x08 misc; 0x20 XCR0, 0x40 DRs, 0x80 FP state, 0x100 TSC */
    uint64_t dirty[2] = {*dirtyWord(v, 0), *dirtyWord(v, 1)};
    auto isDirty = [&](unsigned slot) { return (dirty[slot >> 6] >> (slot & 63)) & 1; };
    if (isDirty(kSlotRsp))
        regs.rsp = *slotWord(v, kSlotRsp);
    if (isDirty(kSlotRip))
        regs.rip = *slotWord(v, kSlotRip);
    if (isDirty(kSlotRflags))
        regs.rflags = *slotWord(v, kSlotRflags);
    unsigned rejected = 0;
    for (unsigned slot = 1; slot < kCacheSlots; slot++) {
        if (!isDirty(slot) || slot == kSlotRsp || slot == kSlotRip || slot == kSlotRflags)
            continue;
        uint32_t field = amdhv::hv::kSlotField[slot];
        /* A field the SVM core does not model is skipped, not fatal: the framework accepts any write to a slot. */
        if (field) {
            IOReturn wr = cpu->vmcsWrite(field, *slotWord(v, slot));
            if (wr != kIOReturnSuccess) {
                rejected++;
                static volatile SInt32 logged;
                if (OSIncrementAtomic(&logged) < 20)    /* the framework told the client at write time; here the write is dropped, so say so */
                    AMDHV_INFO("hv front end: VMCS write refused by the core: field %#x slot %u value %#llx (0x%x)", field, slot,
                             *slotWord(v, slot), wr);
            }
        }
    }
    if (rejected && v->id == 0)
        AMDHV_DBG("hv front end: %u VMCS writes were not accepted by the core", rejected);
    *dirtyWord(v, 0) = 0;
    *dirtyWord(v, 1) = 0;
    *reinterpret_cast<uint64_t *>(v->rw) = 0;

    /* XCR0 and the FP state: the client's copy (rw array) when it wrote one, the core's own otherwise. */
    if (flags & (0x80 | 0x20)) {
        AMDHVFpuState *f = &v->fpu;
        cpu->getFpu(f);
        if (flags & 0x80) {
            uint32_t n = f->size < v->vm->fpStride ? f->size : v->vm->fpStride;
            bcopy(v->vm->kva[0] + kRwFpOff + v->id * v->vm->fpStride, f->area, n);
        }
        /* The client's XCR0 first: the XSTATE_BV trim below must compare against the value the client is
         * enabling in this same batch (it can write a wider XCR0 and an image using it together). */
        f->xcr0 = *gprWord(v, R_XCR0);
        if (flags & 0x80) {
            /* A client can write XSTATE_BV bits that XCR0 (as it just wrote it) does not enable. XRSTOR with a component that
             * XCR0 has not enabled faults, so the core refuses such an image; trim those bits. They are components still in
             * their initial state. */
            if (f->size >= 520) {
                uint64_t bv;
                __builtin_memcpy(&bv, f->area + 512, sizeof(bv));
                const uint64_t keep = bv & (f->xcr0 | 3);          /* x87 and SSE are always allowed */
                if (keep != bv)
                    __builtin_memcpy(f->area + 512, &keep, sizeof(keep));
            }
        }
        IOReturn fr = cpu->setFpu(f);
        if (fr != kIOReturnSuccess)
            AMDHV_INFO("hv front end: FP state/XCR0 refused by the core (0x%x), xcr0 %#llx", fr, (unsigned long long)f->xcr0);
    }
    /* hv_vcpu_set_tsc_relative: the framework records the requested value in the rw slice and sets
     * flag 0x100. The guest must read hv_tsc_clock() + value, and hv_tsc_clock() is rdtsc plus the
     * commpage's (adjust - base), so both terms go into the VMCB TSC offset. */
    if (flags & amdhv::tsc::kRelativeFlag) {
        const volatile uint64_t *comm = reinterpret_cast<const volatile uint64_t *>(v->vm->kva[2]);
        const uint64_t rel = *reinterpret_cast<const uint64_t *>(v->rw + kRwTscRelative);
        const uint64_t off = amdhv::tsc::guestOffset(comm[0], comm[1], rel);
        IOReturn tr = cpu->vmcsWrite(0x2010, off);
        if (tr != kIOReturnSuccess) {
            static volatile SInt32 tscLogged;
            if (OSIncrementAtomic(&tscLogged) < 3)
                AMDHV_INFO("hv front end: TSC offset %#llx refused by the core (0x%x)", (unsigned long long)off,
                         (unsigned)tr);
        }
    }
    /* Debug registers (DR7 is a slot and went with the others). */
    if (flags & 0x40) {
        for (unsigned n = 0; n < 4; n++)
            cpu->vmcsWrite(0xF010 + 2 * n, *gprWord(v, R_DR0 + n));
        cpu->vmcsWrite(0xF002, *gprWord(v, R_DR6));
    }
    /* MSRs of the cache: every valid entry whose value is not what the core was last given. */
    if (flags & 0x4) {
        for (unsigned idx = 1; idx <= amdhv::hv::kMsrCacheMax; idx++) {
            uint32_t msr = amdhv::hv::kMsrOfIdx[idx];
            if (!msr || !msrIsValid(v, idx))
                continue;
            uint64_t val = *msrValWord(v, idx);
            if (v->appliedKnown[idx] && v->applied[idx] == val)
                continue;
            uint64_t unused = 0;
            IOReturn mr = cpu->msrOp(AMDHV_MSR_WRITE, msr, val, &unused);
            v->applied[idx] = val;
            v->appliedKnown[idx] = 1;
            if (mr != kIOReturnSuccess) {
                static volatile SInt32 logged;
                if (OSIncrementAtomic(&logged) < 20)
                    AMDHV_INFO("hv front end: MSR %#x write refused by the core (0x%x), value %#llx", msr, mr, (unsigned long long)val);
            }
        }
    }

    /* NMI-window exiting is answered here: NMI blocking is not tracked, so the window is open at once (a client that sets the control wants
     * to inject an NMI and clears the control when the exit arrives). Exit reason 8, qualification 0, no guest instruction runs. */
    {
        uint64_t procCtls = 0;
        const bool haveCtls = cpu->vmcsRead(0x4002, &procCtls) == kIOReturnSuccess;
        if (haveCtls && !v->tprLogged && (procCtls & (1u << 21)) && !(procCtls & (1u << 19))) {
            v->tprLogged = true;
            AMDHV_INFO("hv front end: vcpu%u of this client uses the TPR shadow (processor controls %#llx)", v->id, (unsigned long long)procCtls);
        }
        if (haveCtls && (procCtls & (1u << 22))) {
            const unsigned slotReason = amdhv::hv::kFieldSlot[0x4402 >> 1], slotQual = amdhv::hv::kFieldSlot[0x6400 >> 1];
            if (slotReason && slotReason < kCacheSlots) {
                *slotWord(v, slotReason) = 8;
                markValid(v, slotReason);
            }
            if (slotQual && slotQual < kCacheSlots) {
                *slotWord(v, slotQual) = 0;
                markValid(v, slotQual);
            }
            /* The VMCS fields of this batch were applied above; the registers (RIP, RSP, RFLAGS, the GPRs) are applied by runEx, which this path skips,
             * and the dirty bits are already cleared. A client's SMM entry can arrive like this (NMI window first, then CS, RIP... in one batch): the
             * guest must get the new CS together with the new RIP, not run off with the old one. */
            cpu->setRegs(&regs);
            return kHvOk;
        }
    }

    /* In-kernel APIC: feed pending IRQs from the 8259A/IOAPIC models into this vCPU's LAPIC, then
     * present the highest priority vector through the VM-entry event-injection field. The core's
     * applyControls() turns that field into VMCB.eventInj; existing client injections win. */
    if (v->vm->apicAccel) {
        if (v->id == 0) {
            for (unsigned p = 0; p < 2; p++) {
                const unsigned vec = amdhv::apic::atpicPending(v->vm->atpic[p]);
                if (vec)
                    amdhv::apic::setIrr(v->lapic, vec);
            }
            const unsigned vec = amdhv::apic::pendingVector(v->vm->ioapic);
            if (vec)
                amdhv::apic::setIrr(v->lapic, vec);
        }
        uint64_t pending = 0;
        const bool freeSlot = cpu->vmcsRead(0x4016, &pending) == kIOReturnSuccess && !(pending & (1ull << 31));
        static volatile SInt32 injLogged;
        if (OSIncrementAtomic(&injLogged) < 8)
            AMDHV_INFO("APIC inject vcpu%u: ioapic irr=%#x pending=%u lapic irr0=%#x tpr=%#x free=%d cur=%#llx",
                       v->id, v->vm->ioapic.irr, amdhv::apic::pendingVector(v->vm->ioapic),
                       (unsigned)v->lapic.irr[0], v->lapic.tpr, freeSlot ? 1 : 0,
                       (unsigned long long)pending);
        if (freeSlot) {
            const unsigned vec = amdhv::apic::takeInterrupt(v->lapic);
            if (vec)
                cpu->vmcsWrite(0x4016, (1ull << 31) | vec);  /* external interrupt, no error code */
        }
    }

    /* 2. Run. */
    *validWord(v, 0) = 0;                              /* the guest changes things: nothing in the cache is trusted until refilled */
    *validWord(v, 1) = 0;
    v->in.flags = AMDHV_RUN_REGS;
    v->in.nWrites = 0;
    v->in.regs = regs;
    /* HV_X86_TPR is the 8-bit APIC TPR (class in bits 7:4); the core's V_TPR is the 4-bit class. A monitor decides from it whether it may inject an
     * interrupt and answers the guest's CR8 reads from it, so a write has to reach the core and read back (read<<4, write>>4). */
    {
        const uint64_t tpr = *gprWord(v, R_TPR);
        if (v->vm->apicAccel)
            v->lapic.tpr = (uint32_t)tpr;
        if (tpr != v->tprPage) {
            cpu->vmcsWrite(0xF004, (tpr >> 4) & 0xF);
            v->tprPage = tpr;
        }
    }
    {
        /* CR2 is a register of the page, not a VMCS slot: a client that injects a #PF writes it there first. */
        const uint64_t cr2 = *gprWord(v, R_CR2);
        if (cr2 != v->cr2Page) {
            cpu->vmcsWrite(0xF000, cr2);
            v->cr2Page = cr2;
        }
    }
    const uint64_t inRax = regs.rax, inRcx = regs.rcx, inRdx = regs.rdx;       /* what the client set before this entry */
    IOReturn rc = cpu->runEx(&v->in, &v->out, deadline);
    if (rc != kIOReturnSuccess) {
        static volatile SInt32 failed;
        if (OSIncrementAtomic(&failed) < 30)
            AMDHV_INFO("hv front end: run vcpu%u failed: 0x%x (rip %#llx)", v->id, rc, (unsigned long long)regs.rip);
        return hvFromIo(rc);
    }
    /*
     * hv_vcpu_get_exec_time reads the first word of the read-only page and never traps, so it has to be
     * current. hv_vcpu_get_idle_time reads the second word; the core does not emulate HLT in the kernel
     * (the client handles that exit), so it stays 0.
     */
    {
        const uint64_t execNs = cpu->execTimeNs();
        __builtin_memcpy(v->ro, &execNs, sizeof(execNs));
    }

    /* What a client that is new to the native path does: the first exits, then one in 5000. Lets a stalled guest be read from amd-info log. */
    {
        static volatile SInt32 runs;
        SInt32 n = OSIncrementAtomic(&runs) + 1;
        if (n <= 120 || (n % 5000) == 0) {
            uint64_t reason = 0, qual = 0, ilen = 0, intr = 0, cr0 = 0;
            for (unsigned i = 0; i < AMDHV_RUN_NFIELDS; i++) {
                const uint64_t f = v->out.fields[i];
                switch (kRunFields[i]) {
                case 0x4402: reason = f; break;
                case 0x6400: qual = f; break;
                case 0x440c: ilen = f; break;
                case 0x4824: intr = f; break;
                case 0x6800: cr0 = f; break;
                default: break;
                }
            }
            AMDHV_INFO("hv front end: run %d vcpu%u exit reason %llu qual %#llx ilen %llu rip %#llx rflags %#llx rax %#llx rcx %#llx rdx %#llx in rax %#llx rcx %#llx rdx %#llx intr %#llx cr0 %#llx",
                     (int)n, v->id, (unsigned long long)(reason & 0xffff), (unsigned long long)qual, (unsigned long long)ilen,
                     (unsigned long long)v->out.regs.rip, (unsigned long long)v->out.regs.rflags, (unsigned long long)v->out.regs.rax,
                     (unsigned long long)v->out.regs.rcx, (unsigned long long)v->out.regs.rdx, (unsigned long long)inRax, (unsigned long long)inRcx,
                     (unsigned long long)inRdx, (unsigned long long)intr, (unsigned long long)cr0);
        }
    }
    {
        uint64_t exitReason = 0;
        for (unsigned i = 0; i < AMDHV_RUN_NFIELDS; i++)
            if (kRunFields[i] == 0x4402)
                exitReason = v->out.fields[i];
        if ((exitReason & 0xFFFF) == 10) {
            v->cpuidPending = true;
            v->cpuidLeaf = (uint32_t)v->out.regs.rax;
            v->cpuidSub = (uint32_t)v->out.regs.rcx;
        }
    }
    fpuToPage(v);                                       /* the core's FP state and XCR0 after the run */
    v->ro[kRoSynced] &= ~1;                             /* the DRs in the page are stale: the next read of one asks (thread trap 11) */
    for (unsigned pos = 0; pos < v->nManaged; pos++) {  /* managed MSRs live in the guest: their values are read from the core after each run */
        unsigned idx = v->managedIdx[pos];
        if (!idx)
            continue;
        uint64_t mv = 0;
        if (cpu->msrOp(AMDHV_MSR_READ, amdhv::hv::kMsrOfIdx[idx], 0, &mv) == kIOReturnSuccess) {
            __builtin_memcpy(v->msrPage + pos * 16 + 8, &mv, 8);
            v->applied[idx] = mv;                       /* what the core holds now: a client write of the old value must not be skipped */
            v->appliedKnown[idx] = 1;
        }
        msrSetValid(v, idx, false);                     /* so that a read goes to the list */
    }

    /* 3. What happened: the registers into the page, the exit information into its slots, exactly those valid. */
    const AMDHVRegs &o = v->out.regs;
    *gprWord(v, R_RAX) = o.rax;
    *gprWord(v, R_RCX) = o.rcx;
    *gprWord(v, R_RDX) = o.rdx;
    *gprWord(v, R_RBX) = o.rbx;
    *gprWord(v, R_RSI) = o.rsi;
    *gprWord(v, R_RDI) = o.rdi;
    *gprWord(v, R_RBP) = o.rbp;
    const uint64_t *or8_15[8] = {&o.r8, &o.r9, &o.r10, &o.r11, &o.r12, &o.r13, &o.r14, &o.r15};
    for (unsigned i = 0; i < 8; i++)
        *gprWord(v, R_R8 + i) = *or8_15[i];
    *slotWord(v, kSlotRsp) = o.rsp;
    markValid(v, kSlotRsp);
    *slotWord(v, kSlotRip) = o.rip;
    markValid(v, kSlotRip);
    *slotWord(v, kSlotRflags) = o.rflags;
    markValid(v, kSlotRflags);
    for (unsigned i = 0; i < AMDHV_RUN_NFIELDS; i++) {
        uint32_t enc = kRunFields[i];
        uint64_t val = v->out.fields[i];
        if (enc == 0xF000) {
            *gprWord(v, R_CR2) = val;                  /* CR2 and TPR are registers in the page, not VMCS slots */
            v->cr2Page = val;
        } else if (enc == 0xF004) {
            *gprWord(v, R_TPR) = (val & 0xF) << 4;
            v->tprPage = (val & 0xF) << 4;
        } else if ((enc >> 1) < amdhv::hv::kFieldSlotCount) {
            unsigned slot = amdhv::hv::kFieldSlot[enc >> 1];
            if (slot && slot < kCacheSlots) {
                *slotWord(v, slot) = val;
                markValid(v, slot);
            }
        }
    }
    return kHvOk;
}

kern_return_t tVcpuRun(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 0, arg);
    return vcpuRun(target, 0);
}

/*
 * hv_vcpu_run_until: arg is a mach_absolute_time deadline, HV_DEADLINE_FOREVER when the client wants the
 * plain run semantics. AMD has no VMX preemption timer, so the core clamps the deadline onto its 10 ms
 * slice and checks it at the exits it already takes (and before entering again). A client that asks for a
 * short wait still gets control back at the deadline instead of after the next unrelated exit.
 */
kern_return_t tVcpuRunUntil(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 1, arg);
    /* 0 is a deadline that passed at boot: keep it distinct from "no deadline" (HV_DEADLINE_FOREVER). */
    const uint64_t deadline = arg == ~0ull ? 0 : (arg ? arg : 1);
    return vcpuRun(target, deadline);
}

/* hv_vcpu_interrupt: `arg` is a mask of vCPU ids. */
kern_return_t tVcpuInterrupt(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 13, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm)
        return kHvNoDev;
    IOLockLock(vm->lock);                              /* a vCPU cannot be freed while the lock is held */
    for (unsigned id = 0; id < kMaxVcpus; id++)
        if (((arg >> id) & 1) && vm->vcpu[id] && vm->vcpu[id]->cpu)
            vm->vcpu[id]->cpu->kick();
    IOLockUnlock(vm->lock);
    return kHvOk;
}

/* Callbacks the kernel makes. They must not log: `suspend` can run with interrupts off. A thread that dies leaves its vCPU to the
 * task; the task's VM is freed with the task. */
void cbNoopVcpu(void *) {}
void cbNoopVolatile(void *, int) {}
void cbTaskDestroy(void *vm) { vmFree(static_cast<HvVm *>(vm)); }

/* hv_suspend() runs on the boot CPU after the other CPUs are off and the APIC is shut down, so it
 * cannot rendezvous or touch MSRs. It only closes the door on new guest entries; the PM notification
 * in AMDHV.cpp clears EFER.SVME/VM_HSAVE_PA while every CPU is still up. hv_resume() runs before the
 * APs are online, so it only moves the epoch that makes each vCPU reload its VMCB. */
void cbHvSuspend(void) { amdhv::svm::noteSuspend(); }
void cbHvResume(void) { amdhv::svm::noteResume(); }

/* IOReturn of the existing VM code -> the codes the framework expects. */
kern_return_t hvFromIo(IOReturn r)
{
    switch ((uint32_t)r) {
    case 0:
        return kHvOk;
    case 0xe00002bd: case 0xe00002be: case 0xe00002db: case 0xe00002de:      /* NoMemory NoResources NoSpace CannotWire */
        return kHvNoRes;
    case 0xe00002c0: case 0xe00002d8: case 0xe00002d9:                       /* NoDevice NotReady NotAttached */
        return kHvNoDev;
    case 0xe00002c1: case 0xe00002e2:                                         /* NotPrivileged NotPermitted */
        return (kern_return_t)0xfae94007;
    case 0xe00002c2: case 0xe00002cf: case 0xe00002d0: case 0xe00002f0:      /* BadArgument NotWritable NotAligned NotFound */
    case 0xe00002f1: case 0xe00002c8:                                         /* Invalid VMError */
        return kHvBadArg;
    case 0xe00002c5: case 0xe00002d5:                                         /* ExclusiveAccess Busy */
        return kHvBusy;
    case 0xe00002c7:                                                          /* Unsupported */
        return kHvUnsupportedCode;
    default:
        return kHvError;
    }
}

/* hv_vm_map_item_t: { space, uva, gpa, size, flags }, 40 bytes. Space 0 is the default address space; the
 * space API creates more (see tVmSpaceCreate). */
struct MapItem { uint64_t space, uva, gpa, size, flags; };

kern_return_t mapItem(HvVm *vm, uint64_t arg, MapItem *it)
{
    if (!vm)
        return kHvNoDev;
    if (copyin((user_addr_t)arg, it, sizeof(*it)))
        return kHvError;
    if (it->space > 0xFFFFFFFFull || (it->flags >> 32))
        return kHvBadArg;
    return kHvOk;
}

/* hv_vm_space_create: `arg` points at the hv_vm_space_t the framework hands back. */
kern_return_t tVmSpaceCreate(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 2, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vmPin(vm))
        return kHvNoDev;
    uint32_t id = 0;
    kern_return_t k = hvFromIo(vm->shared->spaceCreate(&id));
    if (k == kHvOk && copyout(&id, (user_addr_t)arg, sizeof(id))) {
        vm->shared->spaceDestroy(id);           /* do not leak a space the client cannot name */
        k = kHvError;
    }
    vmUnpin(vm);
    return k;
}

/* hv_vm_space_destroy: `arg` is the space ID itself. */
kern_return_t tVmSpaceDestroy(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 3, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vmPin(vm))
        return kHvNoDev;
    kern_return_t k = arg > 0xFFFFFFFFull ? kHvBadArg : hvFromIo(vm->shared->spaceDestroy((uint32_t)arg));
    vmUnpin(vm);
    return k;
}

kern_return_t tVmMap(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 4, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vmPin(vm))
        return kHvNoDev;
    MapItem it;
    kern_return_t k = mapItem(vm, arg, &it);
    if (k) {
        vmUnpin(vm);
        return k;
    }
    kern_return_t rc = hvFromIo(vm->shared->map(current_task(), (uint32_t)it.space, it.uva, it.gpa, it.size,
                                                (uint32_t)it.flags));
    vmUnpin(vm);
    return rc;
}

kern_return_t tVmUnmap(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 5, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vmPin(vm))
        return kHvNoDev;
    MapItem it;
    kern_return_t k = mapItem(vm, arg, &it);
    if (k) {
        vmUnpin(vm);
        return k;
    }
    kern_return_t rc = hvFromIo(vm->shared->unmap((uint32_t)it.space, it.gpa, it.size));
    vmUnpin(vm);
    return rc;
}

kern_return_t tVmProtect(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 6, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vmPin(vm))
        return kHvNoDev;
    MapItem it;
    kern_return_t k = mapItem(vm, arg, &it);
    if (k) {
        vmUnpin(vm);
        return k;
    }
    kern_return_t rc = hvFromIo(vm->shared->protect((uint32_t)it.space, it.gpa, it.size, (uint32_t)it.flags));
    vmUnpin(vm);
    return rc;
}

/* hv_vcpu_set_space: `arg` is the space ID (the vCPU is this thread's target). */
kern_return_t tSetSpace(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 6, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v || !v->cpu)
        return kHvNoDev;
    if (arg > 0xFFFFFFFFull)
        return kHvBadArg;
    return hvFromIo(v->cpu->setSpace((uint32_t)arg));
}

/* hv_vmx_vcpu_set_apic_address: the client tells where its virtual-APIC page is when it uses the TPR shadow. The core keeps the TPR in V_TPR and
 * never reads that page, so there is nothing to remember; the argument block is not read, so a layout we do not know cannot hurt. */
kern_return_t tSetApicAddress(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 9, arg);
    return target ? kHvOk : kHvNoDev;
}

/* hv_vmx_vcpu_set_shadow_access (thread trap 8): shadow-VMCS access bits have no SVM equivalent.
 * read/write_shadow_vmcs already round-trip in the framework's client-side area, so answer success
 * as a compatibility no-op, like the APIC address above. */
kern_return_t tSetShadowAccess(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 8, arg);
    return target ? kHvOk : kHvNoDev;
}

/* hv_vm_atpic, hv_vm_ioapic, hv_vm_lapic and hv_vm_set_apic_bus_freq arrive as one task trap with a
 * packed op code in the first word (the observed enum values are ASCII-ish sequential: 0x6a..0x7a).
 * The state lives in the VM; delivery into the guest is a later slice. */
struct ApicCtlArg { uint64_t op, a, b, c, d, e, f, g; };

kern_return_t tIntCtl(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 10, arg);
    HvVm *vm = static_cast<HvVm *>(target);
    if (!vm || !vmPin(vm))
        return kHvNoDev;
    ApicCtlArg x;
    if (copyin((user_addr_t)arg, &x, sizeof(x))) {
        vmUnpin(vm);
        return kHvError;
    }
    const unsigned op = (unsigned)x.op & 0xFFFF;
    amdhv::apic::AtpicState &pic = vm->atpic[(x.a >> 32) & 1];
    const unsigned irq = (unsigned)x.a & 0xFF;
    switch (op) {
    case 0x74: if (irq < 8 && !(pic.mask & (1u << irq))) pic.request |= 1u << irq; break;
    case 0x75: if (irq < 8) pic.request &= ~(1u << irq); break;
    case 0x76: { uint8_t m = pic.mask; if (copyout(&m, (user_addr_t)x.b, 1)) { vmUnpin(vm); return kHvError; } break; }
    case 0x77: pic.mask = (uint8_t)(x.a >> 32); break;
    case 0x78: { uint32_t ver = 100;   /* HV_ATPIC_STATE_EXT_VER */
                 if (copyout(&ver, (user_addr_t)x.b, 4) || copyout(&pic, (user_addr_t)x.b + 4, sizeof(pic))) { vmUnpin(vm); return kHvError; } break; }
    case 0x79: if (copyin((user_addr_t)x.b + 4, &pic, sizeof(pic))) { vmUnpin(vm); return kHvError; } break;
    case 0x6b: amdhv::apic::assertIrq(vm->ioapic, irq); break;
    case 0x6c: amdhv::apic::deassertIrq(vm->ioapic, irq); break;
    case 0x6d: amdhv::apic::assertIrq(vm->ioapic, irq); amdhv::apic::deassertIrq(vm->ioapic, irq); break;
    case 0x6f: { uint32_t v = amdhv::apic::ioRead(vm->ioapic, vm->ioapic.ioregsel);
                 if (copyout(&v, (user_addr_t)x.b, 4)) { vmUnpin(vm); return kHvError; } break; }
    case 0x70: amdhv::apic::ioWrite(vm->ioapic, vm->ioapic.ioregsel, (uint32_t)x.b); break;
    case 0x71: { uint32_t ver = 100;   /* HV_IOAPIC_STATE_EXT_VER */
                  static volatile SInt32 ioLogged;
                  if (OSIncrementAtomic(&ioLogged) < 4)
                      AMDHV_INFO("hv front end: IOAPIC get_state a=%#llx b=%#llx", (unsigned long long)x.a,
                                 (unsigned long long)x.b);
                 if (copyout(&ver, (user_addr_t)x.a, 4) || copyout(&vm->ioapic, (user_addr_t)x.a + 8, sizeof(vm->ioapic))) { vmUnpin(vm); return kHvError; } break; }
    case 0x72: { static volatile SInt32 ioLogged2;
                 if (OSIncrementAtomic(&ioLogged2) < 4)
                     AMDHV_INFO("hv front end: IOAPIC put_state a=%#llx b=%#llx", (unsigned long long)x.a,
                                (unsigned long long)x.b);
                 if (copyin((user_addr_t)x.a + 8, &vm->ioapic, sizeof(vm->ioapic))) { vmUnpin(vm); return kHvError; } break; }
    case 0x73: if ((x.b & 0x10000u) == 0 && vm->vcpu[0])
                   amdhv::apic::setIrr(vm->vcpu[0]->lapic, (unsigned)(x.b & 0xFF)); break;
    case 0x6a: break;                       /* vcpu/vector are in a nested block; logged, not delivered yet */
    case 0x7a: break;                       /* APIC bus frequency: no timer emulation yet */
    default: vmUnpin(vm); return kHvUnsupportedCode;
    }
    vmUnpin(vm);
    return kHvOk;
}

/* hv_vm_lapic_msi packs (data << 32) | addr. MSI routing into the guest is a later slice. */
kern_return_t tLapicMsi(void *target, uint64_t arg)
{
    note(HV_TASK_TRAP, 11, arg);
    return target ? kHvOk : kHvNoDev;
}

/* Thread trap 13: per-vCPU APIC sub-ops. Pointer words are shifted left by 8; the low byte is a
 * tag (0x8001 apic_read, 0x8002 apic_write, 3 put_state, 4 get_state) or a small code for
 * ctrl/LVT/LSC. The SDK ext structs put state at +8 (after version and padding). */
kern_return_t tApicSubOp(void *target, uint64_t arg)
{
    note(HV_THREAD_TRAP, 13, arg);
    HvVcpu *v = static_cast<HvVcpu *>(target);
    if (!v)
        return kHvNoDev;
    uint64_t w[4] = {0, 0, 0, 0};
    if (copyin((user_addr_t)arg, w, sizeof(w)) != 0)
        return kHvError;
    const unsigned code = (unsigned)w[0] & 0xFFFF;
    const user_addr_t p1 = (user_addr_t)(w[1] >> 8), p2 = (user_addr_t)(w[2] >> 8),
                      p3 = (user_addr_t)(w[3] >> 8);
    /* p2/p3 are pointers to pointers: the first word holds a user address whose low half the old
     * single deref returned as a bogus "offset" (0xb9814d80). Follow both levels. */
    auto arg4 = [](user_addr_t slot, uint64_t *out) -> int {
        user_addr_t inner = 0;
        const int e = copyin(slot, &inner, sizeof(inner));
        static volatile SInt32 logged;
        if (e == 0 && OSIncrementAtomic(&logged) < 12)
            AMDHV_INFO("hv front end: apic arg slot=%#llx inner=%#llx", (unsigned long long)slot,
                       (unsigned long long)inner);
        return e ? 1 : copyin(inner, out, 4);
    };
    auto arg8 = [](user_addr_t slot, uint64_t *out) -> int {
        user_addr_t inner = 0;
        return copyin(slot, &inner, sizeof(inner)) ? 1 : copyin(inner, out, 8);
    };
    switch (code) {
    case 0x8001: {                                   /* apic_read: result, offset */
        uint64_t off = 0;
        const int e = arg4(p2, &off);
        static volatile SInt32 logged;
        if (OSIncrementAtomic(&logged) < 8)
            AMDHV_INFO("hv front end: apic_read p1=%#llx p2=%#llx p3=%#llx off=%#llx e=%d",
                       (unsigned long long)p1, (unsigned long long)p2, (unsigned long long)p3,
                       (unsigned long long)off, e);
        if (e) return kHvError;
        const uint32_t val = amdhv::apic::readReg(v->lapic, (uint32_t)off);
        return copyout(&val, p1, 4) ? kHvError : kHvOk;
    }
    case 0x8002: {                                   /* apic_write: no-side-effect, offset, value */
        uint64_t off = 0, val = 0;
        const int e1 = arg4(p2, &off), e2 = arg4(p3, &val);
        static volatile SInt32 logged2;
        if (OSIncrementAtomic(&logged2) < 8)
            AMDHV_INFO("hv front end: apic_write p1=%#llx p2=%#llx p3=%#llx off=%#llx val=%#llx e=%d/%d",
                       (unsigned long long)p1, (unsigned long long)p2, (unsigned long long)p3,
                       (unsigned long long)off, (unsigned long long)val, e1, e2);
        if (e1 || e2) return kHvError;
        amdhv::apic::writeReg(v->lapic, (uint32_t)off, (uint32_t)val);
        const uint8_t noSideEffect = 1;
        if (p1 && copyout(&noSideEffect, p1, 1))
            return kHvError;
        return kHvOk;
    }
    case 0xe003:                                     /* put_state */
    case 0x0003: {
        uint8_t buf[8 + sizeof(amdhv::apic::LapicState)];
        if (copyin((user_addr_t)(w[0] >> 8), buf, sizeof(buf)))
            return kHvError;
        return amdhv::apic::decodeState(v->lapic, buf) ? kHvOk : kHvError;
    }
    case 0xe004:                                     /* get_state */
    case 0x0004: {
        uint8_t buf[8 + sizeof(amdhv::apic::LapicState)];
        amdhv::apic::encodeState(v->lapic, buf);
        return copyout(buf, (user_addr_t)(w[0] >> 8), sizeof(buf)) ? kHvError : kHvOk;
    }
    case 0x0006: {                                   /* apic_ctrl: controls pointer */
        uint64_t ctrls = 0;
        if (p1 && arg8(p1, &ctrls) == 0)
            v->lapic.apic_controls = ctrls;
        return kHvOk;
    }
    case 0x0107:                                     /* trigger_lvt: no vector supplied by the framework */
    case 0x0005:                                     /* LSC enter / invalidate */
    case 0x0000:                                     /* inject_extint */
        return kHvOk;
    default: {
        static volatile SInt32 logged;
        if (OSIncrementAtomic(&logged) < 8)
            AMDHV_INFO("hv front end: APIC subop %#llx unknown code %#x", (unsigned long long)arg, code);
        return kHvUnsupportedCode;
    }
    }
}

template <unsigned Type, unsigned Index>
kern_return_t stub(void *target, uint64_t arg)
{
    (void)target;
    SInt32 n = OSIncrementAtomic((volatile SInt32 *)&gCount[Type][Index]) + 1;
    if (n <= 24 || (n % 1000) == 0)
        logTrap(Type, Index, arg, n);
    return gMode >= 2 ? KERN_SUCCESS : kHvUnsupported;
}

#define S(T, I) stub<T, I>
/* Task traps: 0 vm_create, 1 vm_destroy, 2/3 space_create/destroy, 4 vm_map, 5 vm_unmap, 6 vm_protect, 7 vm_sync_tsc,
 * 12 vcpu_create, 13 vcpu_interrupt are real; the rest is logged and answered per mode. */
const hv_trap_t kTaskTable[kTraps] = {tVmCreate, tVmDestroy, tVmSpaceCreate, tVmSpaceDestroy, tVmMap, tVmUnmap, tVmProtect,
                                      tVmSyncTsc, S(0, 8), S(0, 9), tIntCtl, tLapicMsi, tVcpuCreate, tVcpuInterrupt, S(0, 14),
                                      S(0, 15)};
/* Thread traps: 0 run, 1 run_until (deadline clamped onto the slice), 2 invalidate_tlb, 3/4 managed MSRs on/off, 5
 * vcpu_destroy, 6 set address space, 7 read slot, 8 set shadow access (no-op), 9 set APIC address (no-op),
 * 10 set_msr_access, 11 sync special regs, 12 capability of a writable slot are real. */
const hv_trap_t kThreadTable[kTraps] = {tVcpuRun, tVcpuRunUntil, tInvalidateTlb, tEnableManaged, tDisableManaged, tVcpuDestroy,
                                        tSetSpace, tReadSlot, tSetShadowAccess, tSetApicAddress, tSetMsrAccess, tSyncSpecial, tCapWrite,
                                        tApicSubOp, S(1, 14), S(1, 15)};
#undef S

} // namespace

namespace amdhv {
namespace hv {

bool registered() { return gRegistered; }
uint32_t mode() { return gRegistered ? gMode : 0; }

bool setTprShadow(bool on)
{
    gTprShadow = on ? 1 : 0;
    AMDHV_SYNC("hv front end: TPR shadow is %s for the VMs created from now on", on ? "offered" : "not offered");
    return true;
}

bool tprShadow() { return gTprShadow != 0; }
bool sysctlTaken() { return gSupportSet; }

static bool registerTraps()
{
    /*
     * On this Mac Apple's own AppleHV is loaded and has registered its tables (a native hv_vm_create answers with AppleHV's own
     * error). The kernel accepts only one registration, so Apple's is released first. Its callbacks are left alone: they are only
     * called for tasks and threads that have a target, and AppleHV never set one on a machine where its VM creation fails.
     */
    hv_release_traps(HV_THREAD_TRAP);
    hv_release_traps(HV_TASK_TRAP);
    if (hv_set_traps(HV_TASK_TRAP, kTaskTable, kTraps) != KERN_SUCCESS) {
        AMDHV_WARN("hv front end: hv_set_traps(task) refused");
        return false;
    }
    if (hv_set_traps(HV_THREAD_TRAP, kThreadTable, kTraps) != KERN_SUCCESS) {
        AMDHV_WARN("hv front end: hv_set_traps(thread) refused");
        hv_release_traps(HV_TASK_TRAP);
        return false;
    }
    /* The callbacks free a task's VM when the task ends, and keep the scheduler's calls (dispatch, preempt, suspend, ...) harmless.
     * Apple's callbacks are released first, as with the traps. */
    hv_release_callbacks();
    hv_callbacks_t cb = {};
    cb.dispatch = cbNoopVcpu;
    cb.preempt = cbNoopVcpu;
    cb.suspend = cbHvSuspend;
    cb.thread_destroy = cbNoopVcpu;
    cb.task_destroy = cbTaskDestroy;
    cb.volatile_state = cbNoopVolatile;
    cb.resume = cbHvResume;
    if (hv_set_callbacks(cb) != KERN_SUCCESS) {
        AMDHV_WARN("hv front end: hv_set_callbacks refused");
        hv_release_traps(HV_THREAD_TRAP);
        hv_release_traps(HV_TASK_TRAP);
        return false;
    }
    gRegistered = true;
    return true;
}

bool setMode(uint32_t m)
{
    if (m == 0) {
        if (gRegistered && gLiveVms > 0) {
            AMDHV_WARN("hv front end: %d VM(s) exist, not unregistering (the kernel calls the callbacks without a NULL check)", (int)gLiveVms);
            return false;
        }
        if (gRegistered) {
            setSysctl(false);
            hv_release_traps(HV_THREAD_TRAP);
            hv_release_traps(HV_TASK_TRAP);
            hv_release_callbacks();
            gRegistered = false;
            AMDHV_SYNC("AMDHV hv front end: unregistered (Apple's tables are not put back)");
        }
        return true;
    }
    if (m > 2)
        m = 2;
    if (!gRegistered) {
        AMDHV_SYNC("AMDHV hv front end: registering in place of AppleHV");
        if (!registerTraps())
            return false;
    }
    gMode = m;
    AMDHV_INFO("hv front end: registered (mode %u: traps are logged and answered %s)", gMode,
             gMode >= 2 ? "success" : "HV_UNSUPPORTED");
    return true;
}

/*
 * kern.hv_support is SYSCTL_INT(... &hv_support_available ...): an int in the kernel's data that is not exported to kexts, and its OID is permanent
 * (replacing the OID panicked the kernel, see the notes of the RyzenHV project). What a kext can do is write the int. Its address is the one the
 * kernel's own hv_get_support() uses, decoded from its code (`mov eax,[rip+disp32]`, matched byte for byte): that is the layout of the RUNNING
 * kernel. The independent check is the OID: after the write, sysctlbyname("kern.hv_support") must read 1 as well; if it does not, the old value is
 * put back and the address is refused.
 */
bool addressFromCode(volatile int **out)
{
    const void *p = amdhv::hv::decodeSupportPtr(reinterpret_cast<const uint8_t *>(&hv_get_support));
    if (!p)
        return false;
    *out = reinterpret_cast<volatile int *>(const_cast<void *>(p));
    return true;
}

bool oidValue(int *v)
{
    int x = -1;
    size_t len = sizeof(x);
    if (sysctlbyname("kern.hv_support", &x, &len, nullptr, 0) != 0 || len != sizeof(x))
        return false;
    *v = x;
    return true;
}

bool locateHvSupport(volatile int **out)
{
    volatile int *fromCode = nullptr;
    bool codeOk = addressFromCode(&fromCode);
    AMDHV_INFO("hv front end: hv_support_available from hv_get_support's code %#llx (%s)", (unsigned long long)(uintptr_t)fromCode,
             codeOk ? "decoded" : "code not as expected");
    if (!codeOk || ((uintptr_t)fromCode & 3))
        return false;
    *out = fromCode;
    return true;
}

bool setSysctl(bool on)
{
    if (!on) {
        if (gSupportSet && gSupportPtr) {
            *gSupportPtr = gSupportSaved;
            gSupportSet = false;
            AMDHV_INFO("hv front end: kern.hv_support back to %d", gSupportSaved);
        }
        return true;
    }
    if (!gRegistered)
        return false;
    if (gSupportSet)
        return true;
    volatile int *p = nullptr;
    if (!locateHvSupport(&p)) {
        AMDHV_WARN("hv front end: hv_get_support() is not the code expected (another kernel build); kern.hv_support is left alone");
        return false;
    }
    int cur = *p;
    if (cur != 0 && cur != 1) {
        AMDHV_WARN("hv front end: the int at the located address is %d, not 0 or 1; kern.hv_support is left alone", cur);
        return false;
    }
    int viaOid = -1;
    if (!oidValue(&viaOid) || viaOid != cur) {          /* before writing: the OID must show the same value as the located int */
        AMDHV_WARN("hv front end: kern.hv_support reads %d through its OID but the located int is %d; refusing, nothing written", viaOid, cur);
        return false;
    }
    /*
     * One more witness before writing: the int we decoded must be the one hv_get_support() reads.
     * The OID check above only proves that the OID and the decoded int agree; a wrong decode that
     * happens to hold the same value (0 on AMD) would pass it. If the accessor and the decoded int
     * disagree, the decode is wrong and nothing is written.
     */
    const int viaAccessor = hv_get_support();
    if (viaAccessor != cur) {
        AMDHV_WARN("hv front end: hv_get_support() reads %d but the decoded int is %d; refusing, nothing written", viaAccessor, cur);
        return false;
    }
    gSupportPtr = p;
    gSupportSaved = cur;
    *p = 1;
    if (hv_get_support() != 1 || !oidValue(&viaOid) || viaOid != 1) {      /* the kernel's accessor and the OID must both see it */
        *p = cur;
        AMDHV_WARN("hv front end: after the write hv_get_support() gives %d and the OID %d, put back %d, refusing", hv_get_support(), viaOid, cur);
        return false;
    }
    gSupportSet = true;
    AMDHV_INFO("hv front end: kern.hv_support was %d, now 1 (read back through hv_get_support and the OID)", cur);
    return true;
}

void start(com_amdhv_AMDHV *service)
{
    gSvc = service;
    uint32_t v = 0;
    uint32_t tprArg = 1;
    if (PE_parse_boot_argn("amdhv_tpr_shadow", &tprArg, sizeof(tprArg)))
        gTprShadow = tprArg ? 1 : 0;
    uint32_t apicArg = 0;
    if (PE_parse_boot_argn("amdhv_apic", &apicArg, sizeof(apicArg)))
        gApicAccel = apicArg ? 1 : 0;
    if (!PE_parse_boot_argn("amdhv_hv", &v, sizeof(v)) || v == 0)
        return;
    if (!setMode(v))
        return;
    /* kern.hv_support = 1 by default once the traps are registered: QEMU (and other clients) check the sysctl
     * before they try Hypervisor.framework. PE_parse_boot_argn returns false when the argument is absent, so
     * parse first and only test the value: absent means the default (1), amdhv_hv_sysctl=0 means leave it. */
    uint32_t sc = 1;
    PE_parse_boot_argn("amdhv_hv_sysctl", &sc, sizeof(sc));
    if (sc)
        setSysctl(true);
}

void stop()
{
    setSysctl(false);
    setMode(0);
}

} // namespace hv
} // namespace amdhv
