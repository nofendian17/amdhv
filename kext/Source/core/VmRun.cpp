/*
 * VmRun.cpp - the vCPU run loop and the exit-code classification it needs.
 * Split verbatim out of Vm.cpp; no behaviour change.
 */
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

using namespace amdhv;
using namespace amdhv::vmcb;


namespace {

const uint64_t kTimesliceNs = 10ull * 1000 * 1000;   /* return to userland at least every ~10 ms */


/**
 * @brief Map an SVM exit code to the framework exit reason.
 *
 * @param code Instruction bytes.
 *
 * @return The uint32_t result.
 */
static uint32_t classify(uint64_t code)
{
    switch (code) {
    case EXIT_HLT:      return AMDHV_EXIT_HLT;
    case EXIT_IOIO:     return AMDHV_EXIT_IO;
    case EXIT_CPUID:    return AMDHV_EXIT_CPUID;
    case EXIT_MSR:      return AMDHV_EXIT_MSR;
    case EXIT_NPF:      return AMDHV_EXIT_NPF;
    case EXIT_SHUTDOWN: return AMDHV_EXIT_SHUTDOWN;
    case EXIT_VMMCALL:  return AMDHV_EXIT_VMMCALL;
    case EXIT_INVALID:  return AMDHV_EXIT_INVALID;
    default:            return AMDHV_EXIT_UNHANDLED;
    }
}

} // namespace

IOReturn com_amdhv_AMDHVVm::run(bool skipInstruction, AMDHVExit *out, uint64_t deadline)
{
    IOReturn rc = kIOReturnSuccess;
    Vmcb *v = (Vmcb *)fVmcb.va;

    IOLockLock(fLock);
    AMDHV_STAT_INC(vmRuns);

    if (skipInstruction) {
        if (fHaveNextRip && v->control.nextRip)
            v->save.rip = v->control.nextRip;
        else
            AMDHV_WARN("VmRun: skip requested but the last exit has no next_rip, RIP unchanged");
    }
    fHaveNextRip = false;

    if (fFirstRun) {
        fFirstRun = false;
        /* A wrong VMCB here shows up as VMEXIT_INVALID, anything worse as a panic: leave a trail. */
        AMDHV_SYNC("vCPU %u: first VMRUN, rip=0x%llx cs.base=0x%llx efer=0x%llx cr0=0x%llx nCr3=0x%llx", fIndex,
                 (unsigned long long)v->save.rip, (unsigned long long)v->save.cs.base,
                 (unsigned long long)v->save.efer, (unsigned long long)v->save.cr0,
                 (unsigned long long)v->control.nestedCr3);
    }

    /*
     * Power was removed since this VMCB was last used: the processor's cached copy of every clean
     * group is gone, and so are its TLB entries. Reload the VMCB in full and flush the ASID. The
     * epoch is checked on every run() entry; a resume that happens while this call is already in
     * the loop stops the loop through activeOnAllUsable().
     */
    const uint64_t sleepEpoch = svm::sleepEpoch();
    if (amdhv::svm::needsReload(fSleepEpoch, sleepEpoch)) {
        fSleepEpoch = sleepEpoch;
        fVmcs.markAllDirty();
        __atomic_store_n(&fForceTlbFlush, 1, __ATOMIC_RELEASE);
    }

    fVmcs.applyControls();      /* controls and event injection the client set through the VMCS */

    uint64_t sliceAbs = 0;
    nanoseconds_to_absolutetime(kTimesliceNs, &sliceAbs);
    const uint64_t sliceDeadline = amdhv::run::stopTime(mach_absolute_time() + sliceAbs, deadline);
    /*
     * A client deadline (hv_vcpu_run_until) can only shorten the slice. When it is what ends the run, the
     * exit reason is the VMX preemption-timer reason, which is what an Intel client sees when the timer
     * fires; a slice that ends on its own is still reported as an external interrupt, as before.
     */
    const bool deadlineIsStop = deadline != 0 && sliceDeadline == deadline;
    uint64_t code = 0;
    bool timeslice = false;
    uint32_t syntheticReason = 0;       /* VMX exit reason to report instead of an external interrupt (TPR below threshold) */
    /*
     * An event waiting to go in (the client's injection, or one whose delivery a host interrupt cut short) when we
     * leave without having run the guest long enough to deliver it. It goes back to the client as IDT-vectoring
     * information; dropping it loses the interrupt (the client believes it was delivered).
     */
    uint32_t carryInfo = 0, carryErr = 0;
    auto takePendingEvent = [&]() {
        if (v->control.eventInj & (1u << 31)) {
            carryInfo = v->control.eventInj;
            carryErr  = v->control.eventInjErr;
            v->control.eventInj    = 0;
            v->control.eventInjErr = 0;
        }
    };

    /*
     * An event the client injected must be delivered before the run is cut short, as on Intel: the entry delivers it,
     * and only then can an external-interrupt exit (hv_vcpu_interrupt, the time slice) happen. A client may not look at
     * IDT-vectoring information on such an exit (Parallels does not) and would take the event for delivered, so
     * handing it back is the last resort. `deferrals` bounds how often the exit is put off for it.
     */
    unsigned deferrals = 0;
    auto injectionWaiting = [&]() { return (v->control.eventInj & (1u << 31)) != 0 && deferrals < 16; };

    bool rescanned  = false;          /* CPUs that came online after the kext started: look for them once per run */
    bool needRescan = false;

    for (;;) {
        if (needRescan) {             /* outside the page-table lock and with interrupts on: it runs a rendezvous and may allocate */
            needRescan = false;
            /* Deadlock guard: rescan() takes gRescanLock and does mp_rendezvous_no_intrs, which needs every
             * CPU to be able to service interrupts. Another CPU spinning on fLock at raised IPL would wedge it.
             * Drop fLock around the rendezvous; the VMCB page itself doesn't move, so just re-acquire. */
            IOLockUnlock(fLock);
            svm::rescan();
            IOLockLock(fLock);
        }
        if (fKick) {                  /* hv_vcpu_interrupt from another thread */
            if (injectionWaiting()) {
                deferrals++;          /* enter first; the flag stays set and ends the run right after the delivery */
            } else {
                fKick = 0;
                timeslice = true;
                takePendingEvent();
                break;
            }
        }
        /* A run_until deadline that has already passed: report it, do not enter the guest. */
        if (deadline != 0 && amdhv::run::expired(mach_absolute_time(), deadline) && !injectionWaiting()) {
            timeslice = true;
            syntheticReason = amdhv::vmx::VMX_PREEMPTION_TIMER;
            takePendingEvent();
            break;
        }
        /*
         * The page tables must not change while this vCPU is inside the guest: hold them for reading from here until the
         * exit has been looked at (a VmMap/VmUnmap/VmProtect on another thread waits for this, after kicking us out).
         */
        struct NptReadGuard {
            IORWLock *l;
            explicit NptReadGuard(IORWLock *x) : l(x) { IORWLockRead(l); }
            ~NptReadGuard() { IORWLockUnlock(l); }
        } nptGuard(fShared->rw());
        if (fKick && !injectionWaiting()) {     /* a kick that came in while we waited for the page tables */
            fKick = 0;
            timeslice = true;
            takePendingEvent();
            break;
        }

        v->control.exitCode = 0;
        /* clean bits are maintained by VmcsState: set after a successful VMRUN, cleared on every write */

        uint64_t hostCr2;
        __asm__ volatile("movq %%cr2, %0" : "=r"(hostCr2));   /* guest page faults can overwrite it */

        /* Interrupts off from here until the exit has been looked at. See activeOnAllUsable(). */
        int ints = ml_set_interrupts_enabled(0);
        fRunCpu = cpu_number();               /* interrupts are off: the thread cannot move before VMRUN */
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (fKick && !injectionWaiting()) {   /* a kick that arrived before fRunCpu was visible */
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            continue;
        }
        if (!svm::activeOnAllUsable() && !rescanned) {
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            rescanned  = true;
            needRescan = true;
            continue;
        }
        if (!svm::activeOnAllUsable()) {
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            AMDHV_ERR("VmRun: SVM was turned off under us (sleep?), not entering the guest");
            rc = kIOReturnNotReady;
            break;
        }

        /* activeOnAllUsable() counts what enableAll() did; this looks at THIS cpu's EFER, because CLGI without SVME is a #UD panic. */
        const int runCpu = (int)fRunCpu;
        const int svmFix = svm::ensureThisCpu();
        if (svmFix == -2 && !rescanned) {     /* this CPU was not online when SVM was switched on */
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            rescanned  = true;
            needRescan = true;
            AMDHV_INFO("VmRun: cpu %d was not known to the SVM code, rescanning", runCpu);
            continue;
        }
        if (svmFix == -3) {                   /* a suspend started between the check and here */
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            rc = kIOReturnNotReady;
            break;
        }
        if (svmFix < 0) {
            fRunCpu = -1;
            ml_set_interrupts_enabled(ints);
            AMDHV_ERR("VmRun: SVM cannot be used on cpu %d (ensure returned %d), not entering the guest", runCpu, svmFix);
            rc = kIOReturnNotReady;
            break;
        }

        /* A full suspend+resume can complete while this thread is frozen, so the epoch checked at
         * run() entry is stale here. Re-read it with interrupts off: clean bits and TLB entries
         * cached before the power removal must not be trusted by the VMRUN below. */
        const uint64_t epochNow = svm::sleepEpoch();
        if (epochNow != fSleepEpoch) {
            fSleepEpoch = epochNow;
            fVmcs.markAllDirty();
            __atomic_store_n(&fForceTlbFlush, 1, __ATOMIC_RELEASE);
            fLastCpu = -1;
        }

        /*
         * VMRUN does not touch x87/SSE/AVX state or XCR0. Swap them by hand, still with interrupts off, so
         * the calling process gets its registers back exactly as it left them.
         */
        /*
         * XSAVE only manages the components XCR0 enables, and an OS that has not run XSETBV yet (XCR0 = 1) can
         * still use SSE. So while the guest runs, the hardware XCR0 always includes x87 and SSE; the value the
         * client sees (and a later XSETBV sets) is fGuestXcr0. The only visible difference is that XGETBV in
         * such a guest reads 3 instead of 1.
         */
        const uint64_t guestX = fGuestXcr0 | amdhv::fpu::XCR0_X87 | amdhv::fpu::XCR0_SSE;
        const bool wasTs = amdhv::fpu::clearTs();
        const uint64_t hostX = amdhv::fpu::xgetbv0();
        if (guestX & ~hostX) {
            fRunCpu = -1;
            amdhv::fpu::restoreTs(wasTs);
            ml_set_interrupts_enabled(ints);
            AMDHV_ERR("VmRun: guest XCR0 0x%llx needs state this thread has not enabled (0x%llx)",
                    (unsigned long long)guestX, (unsigned long long)hostX);
            rc = kIOReturnUnsupported;
            break;
        }
        amdhv::fpu::xsave(fHostFpu.va, hostX);
        if (guestX != hostX)
            amdhv::fpu::xsetbv0(guestX);
        amdhv::fpu::xrstor(fGuestFpu.va, guestX);

        /*
         * Debug registers: VMRUN swaps the guest's DR6/DR7 through the VMCB but neither DR0-DR3 nor the host's own
         * values (a debugger may have set breakpoints on the calling process). Swap them by hand: the client lets the
         * guest use them through the framework's register API.
         */
        uint64_t hostDr[6] = { 0, 0, 0, 0, 0, 0 };
        hostDr[0] = amdhv_read_dr(0); hostDr[1] = amdhv_read_dr(1); hostDr[2] = amdhv_read_dr(2);
        hostDr[3] = amdhv_read_dr(3); hostDr[4] = amdhv_read_dr(6); hostDr[5] = amdhv_read_dr(7);
        amdhv_write_dr(7, 0);                       /* no breakpoint can fire while the registers change */
        for (unsigned i = 0; i < 4; i++)
            amdhv_write_dr(i, fVmcs.dr(i));

        /* RDTSCP reads IA32_TSC_AUX, which VMRUN does not switch: put the guest's value in for the run. */
        uint64_t hostTscAux = 0;
        if (fHasTscAux) {
            hostTscAux = amdhv_rdmsr(amdhv::msr::MSR_TSC_AUX);
            amdhv_wrmsr(amdhv::msr::MSR_TSC_AUX, fVmcs.tscAux());
        }

        /*
         * TLB control. This vCPU's guest TLB entries are tagged with its own ASID, so they survive
         * VMRUN and collide with neither the host's (ASID 0) nor another vCPU's. With FlushByAsid the
         * CPU itself flushes only this ASID on guest CR3 writes and CR4.PGE toggles, so those need no
         * exit. Flush only when a translation could actually be stale:
         *   - the nested page tables changed (map/unmap/protect bump the shared fTlbGen),
         *   - the client asked (hv_vcpu_invalidate_tlb),
         *   - this vCPU moved to another CPU, whose TLB may still hold entries for this ASID from a
         *     previous owner (an ASID is reused after free, and a CPU's TLB outlives the vCPU),
         *   - there is no ASID, or amdhv_asid=0 was given: then flush the whole TLB every entry.
         * FLUSH_BY_ASID needs CPUID Fn8000_000A EDX bit 6; without it TLB_CONTROL 3 is not valid.
         * Everything runs with interrupts off here, so runCpu cannot change before VMRUN.
         */
        /* Clean bits are per logical CPU: a CPU that has not run this VMCB must reload every group. */
        if (fHasVmcbClean && fLastCpu != runCpu)
            fVmcs.markAllDirty();
        const uint64_t tlbGen = fShared->tlbGen();
        const bool     forced = __atomic_exchange_n(&fForceTlbFlush, 0, __ATOMIC_ACQ_REL) != 0;
        const amdhv::vmcb::TlbState tlb = { fAsid != 0, fHasFlushByAsid, fAsidMode, fTlbGenSeen,
                                            tlbGen, forced, fLastCpu, runCpu };
        v->control.tlbCtl = amdhv::vmcb::tlbControl(tlb);
        if (v->control.tlbCtl != TLB_DO_NOTHING)
            fTlbGenSeen = tlbGen;
        fLastCpu = runCpu;

        const uint64_t execStart = mach_absolute_time();
        amdhv_svm_vmrun(&fGprs, fVmcb.pa, fHostVmcb.pa);
        fExecAbs += mach_absolute_time() - execStart;
        fRunCpu = -1;

        if (fHasTscAux) {
            fVmcs.setTscAux(amdhv_rdmsr(amdhv::msr::MSR_TSC_AUX));
            amdhv_wrmsr(amdhv::msr::MSR_TSC_AUX, hostTscAux);
        }

        for (unsigned i = 0; i < 4; i++)
            fVmcs.setDr(i, amdhv_read_dr(i));
        amdhv_write_dr(7, 0);
        amdhv_write_dr(0, hostDr[0]); amdhv_write_dr(1, hostDr[1]); amdhv_write_dr(2, hostDr[2]);
        amdhv_write_dr(3, hostDr[3]); amdhv_write_dr(6, hostDr[4]); amdhv_write_dr(7, hostDr[5]);

        amdhv::fpu::xsave(fGuestFpu.va, guestX);
        if (guestX != hostX)
            amdhv::fpu::xsetbv0(hostX);
        amdhv::fpu::xrstor(fHostFpu.va, hostX);
        /* Self-check: the host state must be bit for bit what it was (this code uses no SSE of its own). */
        amdhv::fpu::xsave(fScratchFpu.va, hostX);
        if (!amdhv::fpu::legacyEqual((const uint8_t *)fHostFpu.va, (const uint8_t *)fScratchFpu.va))
            AMDHV_STAT_INC(vmFpuMismatches);
        amdhv::fpu::restoreTs(wasTs);

        __asm__ volatile("movq %0, %%cr2" : : "r"(hostCr2));
        ml_set_interrupts_enabled(ints);   /* a pending host interrupt is taken right here */

        if (svmFix > 0) {
            static volatile SInt32 healLogged;
            if (OSIncrementAtomic(&healLogged) < 20)
                AMDHV_WARN("VmRun: EFER.SVME or VM_HSAVE_PA was wrong on cpu %d and was restored before VMRUN (%u times so far)", runCpu,
                         svm::healedCount());
        }

        code = v->control.exitCode;
        /*
         * A successful VMRUN leaves the VMCB in sync with the state the CPU now holds, so every group
         * software did not change (and the processor does not write behind our back) is clean for the
         * next entry. VMEXIT_INVALID means the VMCB was rejected and nothing was loaded.
         */
        if (fHasVmcbClean) {
            if (code == EXIT_INVALID)
                fVmcs.markAllDirty();
            else
                fVmcs.markAllClean();
        }
        AMDHV_STAT_INC(vmExits);
        /* PIO notifier: Apple suppresses the guest exit when a notifier matches and sends a Mach
         * message instead. Only OUT carries a value to report; an IN still goes to the client. */
        if (code == EXIT_IOIO && fShared->ionGroup() && !amdhv_ioio_is_in(v->control.exitInfo1)) {
            const uint64_t port = amdhv_ioio_port(v->control.exitInfo1);
            const size_t size = amdhv_ioio_size(v->control.exitInfo1);
            if (size) {
                const uint64_t value = v->save.rax & ((1ull << (size * 8)) - 1);
                if (hv_io_notifier_grp_fire(fShared->ionGroup(), port, size, value) == KERN_SUCCESS &&
                    v->control.nextRip) {
                    v->save.rip = v->control.nextRip;    /* step over the matched OUT */
                    continue;
                }
            }
        }
        /* The injection was either delivered or is reported in exitIntInfo; do not let it fire a second time. */
        v->control.eventInj    = 0;
        v->control.eventInjErr = 0;

        /*
         * In-kernel APIC: serve absolute MOV accesses to the LAPIC (0xFEE00000) and I/O APIC
         * (0xFEC00000) pages from the models when the page is not mapped. Only act when the decoded
         * address equals the faulting GPA; anything else still goes to the userspace VMM.
         */
        /* With DecodeAssists the VMCB carries the faulting instruction bytes, independent of guest
         * paging. Without it, guestBytes() needs a guest PHYSICAL address while save.rip is guest
         * LINEAR; with paging on they differ, so leave decoding to the userspace VMM. KVM makes the
         * same choice in svm_handle_page_fault() (insn_bytes vs. decoding from guest memory). */
        const bool guestPaging = (v->save.cr0 & AMDHV_CR0_PG) != 0;
        if (fApicAccel && fApicLapic && code == EXIT_NPF && amdhv_npf_gpa_valid(v->control.exitInfo1)) {
            const uint64_t fault = v->control.exitInfo2;
            const uint32_t offL = amdhv::apic::mmioOffset(fault, 0xFEE00000ull);
            const uint32_t offI = fApicIoApic ? amdhv::apic::mmioOffset(fault, 0xFEC00000ull) : 0xFFFFFFFFu;
            const bool lapic = offL != 0xFFFFFFFFu;
            const bool ioapic = offI != 0xFFFFFFFFu;
            if (lapic || ioapic) {
                amdhv::apic::MmioOp op = { false, false, 0, 0, 0 };
                if (fHasDecodeAssists && v->control.insnLen) {
                    op = amdhv::apic::decodeMmioAssist(v->control.insnBytes, v->control.insnLen, fault);
                } else if (!guestPaging) {
                    uint8_t insn[8] = {0};   /* 66 + 67 + opcode + modrm + disp32 */
                    if (fShared->guestBytes(fSpace, v->save.rip, insn, sizeof(insn))) {
                        op = amdhv::apic::decodeMmio(insn, sizeof(insn));
                        if (op.disp != fault)
                            op.valid = false;
                    }
                }
                if (op.valid) {
                    uint64_t rax = v->save.rax;   /* VMCB.Save is packed: never take its address */
                    uint64_t *reg = op.reg == 0 ? &rax : nullptr;
                    switch (op.reg) {
                    case 1: reg = &fGprs.rcx;  break;
                    case 2: reg = &fGprs.rdx;  break;
                    case 3: reg = &fGprs.rbx;  break;
                    case 5: reg = &fGprs.rbp;  break;
                    case 6: reg = &fGprs.rsi;  break;
                    case 7: reg = &fGprs.rdi;  break;
                    default: break;              /* RSP: never used for APIC registers */
                    }
                    /* An illegal APIC access is reported as #GP through AMDHV_EXITINFO_INJECT_EXCP. */
                    AMDHVExitInfo xi = {};
                    xi.code = AMDHV_EXITINFO_INJECT_EXCP;
                    xi.value = 13;                 /* #GP */
                    xi.valid = 1;
                    bool served = false;
                    if (reg) {
                        if (lapic) {
                            if (op.isWrite) {
                                const uint32_t val = (uint32_t)*reg;
                                if (offL == 0x300) {          /* ICR: INIT/SIPI are published to the client */
                                    const unsigned dm = (val >> 8) & 7;
                                    if (dm == 3 || dm == 6) {
                                        xi.code = dm == 3 ? AMDHV_EXITINFO_INIT_AP : AMDHV_EXITINFO_STARTUP_AP;
                                        xi.bitmap = 1ull << (val & 0xFF);
                                        if (dm == 6)
                                            xi.rip = (uint64_t)(val & 0xFF) << 12;
                                    }
                                } else if (offL == 0xB0) {    /* EOI: vector of the interrupt retired */
                                    xi.code = AMDHV_EXITINFO_IOAPIC_EOI;
                                    xi.value = amdhv::apic::highestIsrVector(*fApicLapic);
                                }
                                amdhv::apic::writeReg(*fApicLapic, offL, val);
                            } else {
                                xi.code = AMDHV_EXITINFO_APIC_ACCESS_READ;
                                xi.value = amdhv::apic::readReg(*fApicLapic, offL);
                                *reg = xi.value;
                            }
                            served = true;
                        } else if (offI == 0 || offI == 0x10) {
                            /* IOREGSEL at +0, IOWIN at +0x10. */
                            if (op.isWrite) {
                                if (offI == 0)
                                    fApicIoApic->ioregsel = (uint32_t)*reg & 0xFF;
                                else {
                                    if (fApicIoApic->ioregsel == 0x10) {
                                        xi.code = AMDHV_EXITINFO_IOAPIC_EOI;
                                        xi.value = (uint8_t)*reg;
                                    }
                                    amdhv::apic::ioWrite(*fApicIoApic, fApicIoApic->ioregsel, (uint32_t)*reg);
                                }
                            } else {
                                *reg = offI == 0 ? fApicIoApic->ioregsel
                                                 : amdhv::apic::ioRead(*fApicIoApic, fApicIoApic->ioregsel);
                                if (offI == 0x10) {
                                    xi.code = AMDHV_EXITINFO_APIC_ACCESS_READ;
                                    xi.value = (uint32_t)*reg;
                                }
                            }
                            served = true;
                        }
                    }
                    setApicExitInfo(xi);
                    if (served) {
                        if (op.reg == 0)
                            v->save.rax = rax;
                        v->save.rip += op.length;
                        fHaveNextRip = false;
                        continue;
                    }
                }
            }
        }

/*
         * MSR_IA32_SPEC_CTRL (0x48) and MSR_IA32_PRED_CMD (0x49) are Intel-only: the AMD CPU has no
         * such MSR, so a native RDMSR/WRMSR raises #GP in the guest. msrOp() reports the client's
         * enable_native_msr call as successful but keeps the MSRPM intercept; handle the exit here
         * as a no-op instead. A Linux guest writes PRED_CMD on each secondary CPU while booting
         * (write_ibpb) and panics if that write ever reaches the hardware.
         */
        if (code == EXIT_MSR && (fGprs.rcx == 0x48 || fGprs.rcx == 0x49)) {
            static volatile SInt32 logged;
            if (OSIncrementAtomic(&logged) <= 4)
                AMDHV_INFO("VmRun: emulated guest access to Intel-only MSR %#llx on AMD",
                           (unsigned long long)fGprs.rcx);
            if (!(v->control.exitInfo1 & 1)) {   /* RDMSR: EDX:EAX = 0 */
                v->save.rax = 0;
                fGprs.rdx   = 0;
            }
            v->save.rip  = v->control.nextRip ? v->control.nextRip : v->save.rip + v->control.insnLen;
            fHaveNextRip = false;
            continue;
        }

        /*
         * MSR_IA32_TSC (0x10): the client can ask for native access (the framework's slot 144 does), but
         * SVM applies TSC_OFFSET to reads only. A native WRMSR would write the *host* TSC, so msrOp()
         * keeps the intercept and the access is emulated here, exactly as msrOp() would: a read adds the
         * offset, a write moves it. The client never sees an exit it did not ask for.
         */
        if (code == EXIT_MSR && fGprs.rcx == 0x10 && fTscNative) {
            uint32_t lo, hi;
            __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
            const uint64_t hostTsc = ((uint64_t)hi << 32) | lo;
            if (v->control.exitInfo1 & 1) {          /* WRMSR: EDX:EAX = the TSC the guest wants */
                const uint64_t value = ((uint64_t)fGprs.rdx << 32) | (uint32_t)v->save.rax;
                v->control.tscOffset = value - hostTsc;
                fVmcs.markDirty(CLEAN_INTERCEPTS);   /* the TSC offset is in the intercepts group */
                fVmcs.markDirty(amdhv::vmcb::CLEAN_INTERCEPTS);   /* TSC offset is in the intercepts group */
            } else {                                  /* RDMSR: EDX:EAX = guest TSC */
                const uint64_t guestTsc = hostTsc + v->control.tscOffset;
                v->save.rax = (uint32_t)guestTsc;
                fGprs.rdx   = (uint32_t)(guestTsc >> 32);
            }
            v->save.rip  = v->control.nextRip ? v->control.nextRip : v->save.rip + v->control.insnLen;
            fHaveNextRip = false;
            continue;
        }

        /*
         * The guest executed an SVM instruction (VMRUN, VMLOAD, VMSAVE, STGI, CLGI, SKINIT). A Hypervisor.framework
         * client's guest has no such instructions, so what it would see on Intel with VMX off is #UD: inject that
         * and let the guest handle it.
         */
        if (code == 0x80 || (code >= 0x82 && code <= 0x86)) {
            v->control.eventInj = (1u << 31) | (3u << 8) | 6;
            continue;
        }

        if (code == EXIT_INTR || code == EXIT_NMI) {
            /* An event that was being delivered when the host interrupt came in must be injected again. */
            if (v->control.exitIntInfo & (1u << 31)) {
                v->control.eventInj    = v->control.exitIntInfo;
                v->control.eventInjErr = v->control.exitIntInfoErr;
            }
            /* The host needed the CPU. It has it now; run on unless the slice is over. */
            if (amdhv::run::expired(mach_absolute_time(), sliceDeadline) && !injectionWaiting()) {
                timeslice = true;
                if (deadlineIsStop)
                    syntheticReason = amdhv::vmx::VMX_PREEMPTION_TIMER;
                takePendingEvent();
                break;
            }
            continue;
        }

        /*
         * TPR shadow, see VmcsState::applyControls: a MOV to CR8 is intercepted only while the client waits for the TPR
         * to drop below its threshold. Handle it here, without a trip to userland: set V_TPR, step over the instruction,
         * and report "TPR below threshold" only when the new value really is below the threshold.
         */
        if (code == 0x18 && fVmcs.tprShadowActive() && (v->control.exitInfo1 >> 63) && v->control.nextRip) {
            unsigned gpr = (unsigned)(v->control.exitInfo1 & 0xF);
            uint64_t val;
            switch (gpr) {
            case 0:  val = v->save.rax; break;
            case 1:  val = fGprs.rcx; break;
            case 2:  val = fGprs.rdx; break;
            case 3:  val = fGprs.rbx; break;
            case 4:  val = v->save.rsp; break;
            case 5:  val = fGprs.rbp; break;
            case 6:  val = fGprs.rsi; break;
            case 7:  val = fGprs.rdi; break;
            case 8:  val = fGprs.r8; break;
            case 9:  val = fGprs.r9; break;
            case 10: val = fGprs.r10; break;
            case 11: val = fGprs.r11; break;
            case 12: val = fGprs.r12; break;
            case 13: val = fGprs.r13; break;
            case 14: val = fGprs.r14; break;
            default: val = fGprs.r15; break;
            }
            if (val <= 0xF) {                               /* anything else is a #GP the client emulates */
                v->control.intCtl = (v->control.intCtl & ~0xFFu) | (uint32_t)val;
                v->save.rip       = v->control.nextRip;
                if (v->control.exitIntInfo & (1u << 31)) {  /* an event that was being delivered goes in again */
                    v->control.eventInj    = v->control.exitIntInfo;
                    v->control.eventInjErr = v->control.exitIntInfoErr;
                }
                if (val < fVmcs.tprThreshold()) {
                    timeslice = true;
                    syntheticReason = amdhv::vmx::VMX_TPR_BELOW_THRESHOLD;
                    takePendingEvent();
                    break;
                }
                if (amdhv::run::expired(mach_absolute_time(), sliceDeadline) && !injectionWaiting()) {
                    timeslice = true;
                    if (deadlineIsStop)
                        syntheticReason = amdhv::vmx::VMX_PREEMPTION_TIMER;
                    takePendingEvent();
                    break;
                }
                continue;
            }
        }
        break;
    }

    if (rc == kIOReturnSuccess) {
        bool vmxTranslated = false;
        bzero(out, sizeof(*out));
        if (timeslice) {
            out->reason = AMDHV_EXIT_TIMESLICE;
            fVmcs.recordSynthetic(syntheticReason ? syntheticReason : amdhv::vmx::VMX_EXT_INTR, carryInfo, carryErr);
        } else {
            out->reason   = classify(code);
            if (out->reason == AMDHV_EXIT_NPF)
                AMDHV_STAT_INC(vmNptFaults);
            out->exitCode = code;
            out->info1    = v->control.exitInfo1;
            out->info2    = v->control.exitInfo2;
            out->nextRip  = v->control.nextRip;
            fHaveNextRip  = v->control.nextRip != 0;
            vmxTranslated = true;
            uint32_t perm = 0;
            if (code == EXIT_NPF && amdhv_npf_gpa_valid(v->control.exitInfo1))
                perm = fShared->spacePerm(fSpace, v->control.exitInfo2 & ~(uint64_t)0xFFF);
            if (!fVmcs.recordExit(perm)) {
                vmxTranslated = false;
                out->reason = AMDHV_EXIT_UNHANDLED;
            }      /* e.g. the guest executed an SVM instruction */
            AMDHV_TRACE("VM exit: code=0x%llx info1=0x%llx info2=0x%llx rip=0x%llx",
                      (unsigned long long)code, (unsigned long long)out->info1,
                      (unsigned long long)out->info2, (unsigned long long)v->save.rip);
        }
        out->rip = v->save.rip;

        if (out->reason == AMDHV_EXIT_INVALID) {
            AMDHV_STAT_INC(vmInvalidExits);
            dumpVmcb("VMRUN rejected the VMCB (VMEXIT_INVALID)");
        } else if (out->reason == AMDHV_EXIT_UNHANDLED && !vmxTranslated) {      /* CR/DR accesses, interrupt windows... are fine in VMX mode */
            AMDHV_WARN("VM exit with unhandled code 0x%llx at rip 0x%llx", (unsigned long long)code,
                     (unsigned long long)v->save.rip);
        }
    }

    IOLockUnlock(fLock);
    return rc;
}

