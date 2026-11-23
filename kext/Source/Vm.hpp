/*
 * Vm.hpp - a virtual machine: one shared part and one object per vCPU.
 *
 * com_amdhv_AMDHVVmShared is what all vCPUs of a VM have in common: the nested page tables (guest physical ->
 * pinned memory, built by amdhv::npt::Table) and the list of the vCPUs. com_amdhv_AMDHVVm is one vCPU: it owns the
 * VMCB, the host VMCB, the I/O and MSR permission bitmaps and the guest's general purpose registers, and has its own
 * lock, so vCPUs run in parallel on different host threads.
 *
 * Guest memory is the calling process's own pages (hv_vm_map), pinned into 4 KiB NPT entries pointing into a Backing.
 *
 * A vCPU's own state is serialised by its lock: run() holds it while the guest runs, which is bounded by the time slice
 * (see Vm.cpp). The page tables have a reader/writer lock: a vCPU holds it for reading while it is inside the guest, and
 * map/unmap/protect take it for writing after kicking every vCPU out, so when they return no vCPU still runs with
 * the old mappings or permissions (each vCPU flushes its own ASID on its next entry; see run()).
 */
#ifndef AMDHV_VM_HPP
#define AMDHV_VM_HPP

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSObject.h>

#include "../../include/AMDHVShared.h"
#include "FpuState.hpp"
#include "ApicState.hpp"
#include "Npt.hpp"
#include "SpacePool.hpp"
#include "VmcsState.hpp"
#include "PhysMem.hpp"
#include "VMCB.hpp"

namespace amdhv {

/* Guest GPRs that VMRUN does not keep in the VMCB (RAX and RSP are in the VMCB). Layout is used by SvmRun.S. */
struct GuestGprs {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
};

} // namespace amdhv


class com_amdhv_AMDHVVm;

class com_amdhv_AMDHVVmShared : public OSObject {
    OSDeclareDefaultStructors(com_amdhv_AMDHVVmShared)

public:
    /* `owner` is the task that created it (a vCPU may only join from the same task). */
    static com_amdhv_AMDHVVmShared *create(task_t owner, IOReturn *err);
    /* Looks a VM up by id; takes a reference, nullptr when there is none or it belongs to another task. */
    static com_amdhv_AMDHVVmShared *find(uint64_t id, task_t owner);

    bool init() override;
    void free() override;

    uint64_t            id() const { return fId; }
    IORWLock           *rw() const { return fRw; }
    uint64_t             tlbGen() const { return fTlbGen; }

    /* hv_vm_map / unmap / protect, in `space` (0 is the default address space). See AMDHVShared.h. */
    IOReturn map(task_t task, uint32_t space, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    IOReturn unmap(uint32_t space, uint64_t gpa, uint64_t size);
    IOReturn protect(uint32_t space, uint64_t gpa, uint64_t size, uint32_t flags);

    /* hv_vm_space_create / destroy. Extra address spaces are nested page table roots of their own. */
    IOReturn spaceCreate(uint32_t *id);
    IOReturn spaceDestroy(uint32_t id);
    /* Root of `space` (0 is the default table), for hv_vcpu_set_space. False when it does not exist. */
    bool     spaceRoot(uint32_t space, uint64_t *pa);
    /* NPT rights of `gpa` in `space` (npt::PERM_*), 0 when unmapped or the space does not exist. */
    uint32_t spacePerm(uint32_t space, uint64_t gpa);
    /* True when a vCPU is currently bound to `space`. */
    bool     spaceInUse(uint32_t space);

    /* The vCPUs, for kicking them out of the guest. attach() fails with kIOReturnNoResources when the VM is full. */
    IOReturn attach(com_amdhv_AMDHVVm *vcpu, uint32_t *index);
    void     detach(com_amdhv_AMDHVVm *vcpu);
    void     kickAll();

    /* A pinned buffer (user or kernel) and how many NPT pages still point into it. */
    struct Backing {
        IOMemoryDescriptor *md;
        uint64_t            live;
        IODirection         dir;      /* what prepare() was called with: complete() must use the same */
    };

    /* Copy `n` bytes at guest physical `gpa` in `space` into `dst`. Caller holds rw(). */
    bool guestBytes(uint32_t space, uint64_t gpa, void *dst, size_t n);

private:
    bool     setup(task_t owner);
    IOReturn mapDescriptorLocked(amdhv::npt::Table &table, IOMemoryDescriptor *md, uint64_t gpa,
                                 uint64_t size, uint32_t perm);
    static void dropPage(Backing *b);
    static void releaseOwner(void *ctx, void *owner);
    amdhv::npt::Table *tableFor(uint32_t space);      /* caller holds fRw */

    uint64_t                  fId;
    task_t                    fOwner;
    IORWLock                 *fRw;
    IOLock                   *fRegLock;
    com_amdhv_AMDHVVm *fVcpus[AMDHV_MAX_VCPUS_PER_VM];
    amdhv::npt::Table           fNpt;
    amdhv::npt::Table           fSpaces[amdhv::space::kMax];
    amdhv::space::Pool          fSpaceIds;
    uint64_t                  fPagesMapped;
    volatile uint64_t         fTlbGen;    /* bumped whenever the nested page tables change */
};

class com_amdhv_AMDHVVm : public OSObject {
    OSDeclareDefaultStructors(com_amdhv_AMDHVVm)

public:
    /*
     * Fails with kIOReturnNotReady if SVM is not active on all CPUs and with
     * kIOReturnUnsupported if the CPU lacks NPT or NRIPS. With `shared` null a new VM is made (this is its first vCPU),
     * otherwise the vCPU joins that VM (kIOReturnNoResources when it is full).
     */
    static com_amdhv_AMDHVVm *create(const AMDHVInfo &info, task_t owner,
                                             com_amdhv_AMDHVVmShared *shared, IOReturn *err);

    com_amdhv_AMDHVVmShared *shared() const { return fShared; }
    uint32_t                         index() const { return fIndex; }

    bool init() override;
    void free() override;

    /* hv_vm_map / unmap / protect. See AMDHVShared.h for the exact rules. */
    IOReturn map(task_t task, uint64_t uva, uint64_t gpa, uint64_t size, uint32_t flags);
    IOReturn unmap(uint64_t gpa, uint64_t size);
    IOReturn protect(uint64_t gpa, uint64_t size, uint32_t flags);

    /* hv_vcpu_set_space: bind this vCPU to another guest address space (0 = the default one). */
    IOReturn setSpace(uint32_t space);
    uint32_t spaceId() const { return fSpace; }

    IOReturn getRegs(AMDHVRegs *out);
    IOReturn setRegs(const AMDHVRegs *in);
    IOReturn run(bool skipInstruction, AMDHVExit *out, uint64_t deadline = 0);
    IOReturn runEx(const AMDHVRunIn *in, AMDHVRunOut *out, uint64_t deadline = 0);            /* apply, run, report in one call */
    IOReturn vmcsReadMany(const AMDHVReadMany *in, AMDHVReadManyOut *out);

    /* The Hypervisor.framework (VMX-shaped) view of the guest. */
    IOReturn vmcsRead(uint32_t field, uint64_t *value);
    IOReturn vmcsWrite(uint32_t field, uint64_t value);

    /* Guest x87/SSE/AVX state and XCR0. */
    IOReturn msrOp(uint32_t op, uint32_t msr, uint64_t value, uint64_t *out);
    /*
     * hv_vcpu_interrupt: no lock, called while another thread sits in run(). Besides raising the flag it sends an IPI to
     * the CPU the guest runs on: the physical interrupt makes VMRUN exit at once, instead of when the host happens to
     * take its next interrupt (up to 10 ms later, too slow for a guest that expects a 250-1000 Hz timer).
     */
    void     kick();
    /* hv_vcpu_invalidate_tlb: flush this vCPU's guest TLB on its next entry. */
    void     forceTlbFlush() { __atomic_store_n(&fForceTlbFlush, 1, __ATOMIC_RELEASE); }

    /* A VM created with HV_VM_ACCEL_APIC gives us its per-vCPU LAPIC model for MMIO emulation. */
    void     setApicModel(bool accel, amdhv::apic::LapicState *model)
    {
        fApicAccel = accel;
        fApicLapic = model;
    }

    IOReturn getFpu(AMDHVFpuState *out);
    IOReturn setFpu(const AMDHVFpuState *in);

    /*
     * hv_vcpu_get_exec_time reads two words straight out of the read-only shared page (offset 0 and
     * 8) and never traps. The front end copies this into offset 0 after every run.
     */
    uint64_t execTimeNs();

private:
    bool     setup();
    void     resetLocked(uint16_t cs, uint16_t ip);
    void     dumpVmcb(const char *why);

    IOLock                   *fLock;
    com_amdhv_AMDHVVmShared *fShared;
    uint32_t                  fIndex;
    amdhv::PhysMem              fVmcb;
    amdhv::PhysMem              fHostVmcb;
    amdhv::PhysMem              fIopm;     /* 12 KiB, all ones: intercept every port */
    amdhv::PhysMem              fMsrpm;    /*  8 KiB, all ones: intercept every MSR  */
    amdhv::vmx::VmcsState       fVmcs;
    amdhv::PhysMem              fHostFpu, fGuestFpu, fScratchFpu;   /* XSAVE areas, 4 KiB aligned */
    uint64_t                  fGuestXcr0;
    uint64_t                  fXcr0Supported;    /* CPUID.0xD.0 EDX:EAX */
    bool                      fHasTscAux;        /* the CPU has RDTSCP, so IA32_TSC_AUX */
    bool                      fTscNative;        /* the client asked for native MSR 0x10: run() emulates it in-kernel */
    bool                      fApicAccel;        /* HV_VM_ACCEL_APIC: emulate APIC MMIO exits in-kernel */
    amdhv::apic::LapicState  *fApicLapic;        /* owned by the front end's HvVcpu */
    uint32_t                  fFpuSize;
    volatile uint32_t         fKick;
    volatile uint32_t         fForceTlbFlush;    /* hv_vcpu_invalidate_tlb asked for a flush */
    uint64_t                  fTlbGenSeen;       /* the tlbGen this vCPU last flushed for */
    uint64_t                  fSleepEpoch;       /* svm::sleepEpoch() this VMCB was last valid for */
    uint32_t                  fAsid;             /* this vCPU's SVM ASID (0 = none, share the host's) */
    volatile uint32_t         fSpace;            /* hv_vm_space_t this vCPU runs in (0 = default) */
    bool                      fHasFlushByAsid;   /* CPUID Fn8000_000A EDX bit 6: TLB_CONTROL 3 is valid */
    bool                      fHasVmcbClean;     /* CPUID Fn8000_000A EDX bit 5: VMCB clean bits are valid */
    bool                      fHasPauseFilter;   /* CPUID Fn8000_000A EDX bit 10: pause filter count */
    bool                      fHasPauseThresh;   /* CPUID Fn8000_000A EDX bit 12: advanced pause filter */
    bool                      fAsidMode;         /* amdhv_asid=0: flush on every entry, as before */
    int                       fLastCpu;          /* CPU this vCPU last entered the guest on, -1 = none */
    volatile int              fRunCpu;           /* CPU inside VMRUN, -1 when none; for kick() */
    amdhv::GuestGprs            fGprs;
    bool                      fFirstRun;
    bool                      fHaveNextRip;
    uint64_t                  fExecAbs;          /* total time inside VMRUN, mach absolute units */
};

#endif /* AMDHV_VM_HPP */
