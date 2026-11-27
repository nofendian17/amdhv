#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/OSAtomic.h>

#include "KernelDecls.hpp"
#include "Log.hpp"
#include "SleepPolicy.hpp"
#include "SVM.hpp"
#include "X86.hpp"

namespace {

struct CpuProbe {
    uint32_t vendorAmd;
    uint32_t family, model, stepping;
    uint32_t svmSupported;
    uint32_t svmRevision;
    uint32_t numAsids;
    uint32_t features;
    uint32_t svmDisabled;
    uint32_t svmLocked;
    uint32_t eferSvme;
};

struct CpuVerify {
    uint64_t efer;
    uint64_t hsave;
    uint32_t checked;
};

CpuProbe                  gProbe[AMDHV_MAX_CPUS];
CpuVerify                 gVerify[AMDHV_MAX_CPUS];
IOBufferMemoryDescriptor *gHostSave[AMDHV_MAX_CPUS];
uint64_t                  gHostSavePa[AMDHV_MAX_CPUS];
uint8_t                   gSeenCpu[AMDHV_MAX_CPUS];     /* this CPU has been probed */
uint8_t                   gOn[AMDHV_MAX_CPUS];          /* SVM is on here (counted in gActive) */
IOLock                   *gRescanLock;
volatile SInt32           gNewCpus;                   /* CPUs a rescan found */
volatile SInt32           gRescanSeen;                /* CPUs a rescan's rendezvous reached */
bool                      gWantEnabled;               /* enableAll() succeeded: CPUs that show up later get SVM too */
uint32_t                  gNCpus;
volatile SInt32           gActive;
volatile SInt32           gHealed;    /* times ensureThisCpu() restored EFER.SVME or VM_HSAVE_PA */
volatile SInt32           gSeen;      /* CPUs that ran probeAction */
volatile SInt32           gMaxIndex;  /* highest cpu_number() seen */
volatile bool             gReadMsrs = true;   /* set before the probe rendezvous */
volatile bool             gSuspended;
volatile bool             gStopping;           /* disableAll() rendezvous: no new guest entries */
uint64_t                  gSleepEpoch;          /* bumped on suspend/resume: see SleepPolicy.hpp */

/**
 * @brief Check whether a CPU can run SVM guests.
 *
 * @param p Page or pointer.
 *
 * @return The bool result.
 */
bool cpuUsable(const CpuProbe &p)
{
    return p.vendorAmd && p.svmSupported && !p.svmDisabled;
}

/** gMaxIndex is raised by several CPUs at once, so a plain compare-and-store could lose the highest one. */
void noteIndex(int cpu)
{
    for (;;) {
        SInt32 m = gMaxIndex;
        if (cpu <= m || OSCompareAndSwap((UInt32)m, (UInt32)cpu, (volatile UInt32 *)&gMaxIndex))
            return;
    }
}

/**
 * @brief Probe SVM support on the current CPU.
 *
 * @param cpu CPU index.
 */
void probeThisCpu(int cpu)
{
    CpuProbe &p = gProbe[cpu];
    uint32_t r[4];

    amdhv_cpuid(0, 0, r);
    p.vendorAmd = amdhv_is_amd_vendor(r[1], r[2], r[3]) ? 1u : 0u;
    if (!p.vendorAmd)
        return;

    amdhv_cpuid(1, 0, r);
    p.family   = amdhv_amd_family(r[0]);
    p.model    = amdhv_amd_model(r[0]);
    p.stepping = amdhv_amd_stepping(r[0]);

    amdhv_cpuid(0x80000000u, 0, r);
    uint32_t maxExt = r[0];

    if (maxExt >= 0x80000001u) {
        amdhv_cpuid(0x80000001u, 0, r);
        p.svmSupported = (r[2] >> 2) & 1;
    }

    if (p.svmSupported && maxExt >= 0x8000000Au) {
        amdhv_cpuid(0x8000000Au, 0, r);
        p.svmRevision = r[0] & 0xFF;
        p.numAsids    = r[1];
        p.features    = r[3];
    }

    if (!gReadMsrs)
        return;

    if (p.svmSupported) {
        uint64_t vmcr = amdhv_rdmsr(AMDHV_MSR_VM_CR);
        p.svmDisabled = (vmcr & AMDHV_VM_CR_SVMDIS) ? 1 : 0;
        p.svmLocked   = (vmcr & AMDHV_VM_CR_LOCK) ? 1 : 0;
    }

    p.eferSvme = (amdhv_rdmsr(AMDHV_MSR_EFER) & AMDHV_EFER_SVME) ? 1 : 0;
}

/**
 * @brief Probe one framework API call and report its result.
 */
void probeAction(void *)
{
    int cpu = cpu_number();
    OSIncrementAtomic(&gSeen);
    noteIndex(cpu);
    if (cpu < 0 || cpu >= AMDHV_MAX_CPUS)
        return;
    gSeenCpu[cpu] = 1;
    probeThisCpu(cpu);
}

/** A later look at the CPUs: only the ones that were not probed yet. */
void rescanProbeAction(void *)
{
    int cpu = cpu_number();
    OSIncrementAtomic(&gRescanSeen);
    if (cpu < 0 || cpu >= AMDHV_MAX_CPUS || gSeenCpu[cpu])
        return;
    noteIndex(cpu);
    OSIncrementAtomic(&gNewCpus);
    gSeenCpu[cpu] = 1;
    probeThisCpu(cpu);
}

/**
 * @brief Enable a CPU feature during the rescan.
 */
void enableAction(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= AMDHV_MAX_CPUS)
        return;
    if (gOn[cpu])
        return;               /* counted already (a rescan runs this on every CPU again) */
    if (!cpuUsable(gProbe[cpu]) || !gHostSavePa[cpu])
        return;

    /* Read-modify-write: keep every EFER bit the kernel already relies on. APM 15.30.1 makes SVME
     * an MBZ write while VM_CR.SVMDIS is set, so verify the write before counting this CPU as on. */
    uint64_t efer = amdhv_rdmsr(AMDHV_MSR_EFER);
    if (!(efer & AMDHV_EFER_SVME)) {
        amdhv_wrmsr(AMDHV_MSR_EFER, efer | AMDHV_EFER_SVME);
        if (!(amdhv_rdmsr(AMDHV_MSR_EFER) & AMDHV_EFER_SVME)) {
            AMDHV_ERR("cpu %u: EFER.SVME stayed clear (SVMDIS set), not counted as active", cpu);
            return;
        }
    }
    amdhv_wrmsr(AMDHV_MSR_VM_HSAVE_PA, gHostSavePa[cpu]);

    gOn[cpu] = 1;
    __atomic_fetch_add(&gActive, 1, __ATOMIC_ACQ_REL);
}

/**
 * @brief Disable a CPU feature during the rescan.
 */
void disableAction(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= AMDHV_MAX_CPUS)
        return;
    if (!cpuUsable(gProbe[cpu]))
        return;

    uint64_t efer = amdhv_rdmsr(AMDHV_MSR_EFER);
    if (efer & AMDHV_EFER_SVME) {
        amdhv_wrmsr(AMDHV_MSR_VM_HSAVE_PA, 0);
        amdhv_wrmsr(AMDHV_MSR_EFER, efer & ~AMDHV_EFER_SVME);
    }
    gOn[cpu] = 0;
}

/**
 * @brief Verify one CPU feature after enablement.
 */
void verifyAction(void *)
{
    int cpu = cpu_number();
    if (cpu < 0 || cpu >= AMDHV_MAX_CPUS)
        return;
    if (!cpuUsable(gProbe[cpu]))
        return;

    CpuVerify &v = gVerify[cpu];
    v.efer    = amdhv_rdmsr(AMDHV_MSR_EFER);
    v.hsave   = amdhv_rdmsr(AMDHV_MSR_VM_HSAVE_PA);
    v.checked = 1;
}

/**
 * @brief Allocate the host save state area for a CPU.
 *
 * @param cpu CPU index.
 *
 * @return The bool result.
 */
bool allocHostSave(uint32_t cpu)
{
    /* One 4K page, physically contiguous, 4K aligned, below 2^52.
     * kIOMemoryMapperNone: the page is addressed by physical address (VM_HSAVE_PA), never DMA-mapped.
     * Without it IOBufferMemoryDescriptor asks IOMapper::checkForSystemMapper(), which at boot waits
     * for the system mapper and prints the "waitForSystemMapper" backtrace with this kext in it. */
    IOBufferMemoryDescriptor *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryMapperNone, 4096,
        0x000FFFFFFFFFF000ULL);
    if (!md) {
        AMDHV_ERR("cpu %u: host save area: allocation failed", cpu);
        return false;
    }

    bzero(md->getBytesNoCopy(), 4096);

    IOByteCount len = 0;
    addr64_t pa = md->getPhysicalSegment(0, &len, kIOMemoryMapperNone);
    if (!pa || len < 4096 || (pa & 0xFFF)) {
        AMDHV_ERR("cpu %u: host save area: bad physical segment pa=0x%llx len=%llu", cpu,
                (unsigned long long)pa, (unsigned long long)len);
        md->release();
        return false;
    }

    gHostSave[cpu]   = md;
    gHostSavePa[cpu] = pa;
    AMDHV_DBG("cpu %u: host save area at pa 0x%llx", cpu, (unsigned long long)pa);
    return true;
}

/**
 * @brief Release Host Save.
 */
void freeHostSave()
{
    for (uint32_t i = 0; i < AMDHV_MAX_CPUS; i++) {
        if (gHostSave[i]) {
            gHostSave[i]->release();
            gHostSave[i] = nullptr;
        }
        gHostSavePa[i] = 0;
    }
}

/**
 * @brief Count the CPUs that can run SVM guests.
 *
 * @return The uint32_t result.
 */
uint32_t usableCount()
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < gNCpus; i++) {
        if (cpuUsable(gProbe[i]))
            n++;
    }
    return n;
}

} // namespace

namespace amdhv {
namespace svm {

/**
 * @brief Probe the supported framework APIs.
 *
 * @param out Output array of four registers.
 * @param readMsrs Whether to read MSRs.
 */
void probe(AMDHVInfo *out, bool readMsrs)
{
    if (!gRescanLock)
        gRescanLock = IOLockAlloc();
    bzero(gProbe, sizeof(gProbe));
    bzero(gSeenCpu, sizeof(gSeenCpu));
    gReadMsrs = readMsrs;

    /*
     * The CPU count is not exported to kexts (ml_get_max_cpus does not exist on Sequoia), so count
     * the CPUs that take part in the rendezvous. Indices are contiguous from 0.
     */
    gSeen     = 0;
    gMaxIndex = 0;
    mp_rendezvous_no_intrs(probeAction, nullptr);
    AMDHV_STAT_INC(probeRuns);

    gNCpus = (uint32_t)gMaxIndex + 1;
    if ((uint32_t)gSeen != gNCpus)
        AMDHV_WARN("rendezvous ran on %d CPUs but the highest cpu_number is %d", (int)gSeen, (int)gMaxIndex);
    if (gNCpus > AMDHV_MAX_CPUS) {
        AMDHV_WARN("machine reports %u CPUs, only the first %u are handled", gNCpus, AMDHV_MAX_CPUS);
        gNCpus = AMDHV_MAX_CPUS;
    }
    AMDHV_INFO("probe: %u CPUs online now; more may come up later, rescan() picks them up", gNCpus);

    bzero(out, sizeof(*out));
    out->abiVersion = AMDHV_ABI_VERSION;
    out->nCpus      = gNCpus;

    /* CPU 0 is the reference; the per-CPU usable count catches asymmetric setups. */
    const CpuProbe &p = gProbe[0];
    out->vendorIsAmd       = p.vendorAmd;
    out->cpuFamily         = p.family;
    out->cpuModel          = p.model;
    out->cpuStepping       = p.stepping;
    out->svmSupported      = p.svmSupported;
    out->svmDisabledByBios = p.svmDisabled;
    out->svmLocked         = p.svmLocked;
    out->svmRevision       = p.svmRevision;
    out->numAsids          = p.numAsids;
    out->svmFeatures       = p.features;
    out->nCpusSvmUsable    = usableCount();
    out->nCpusSvmActive    = __atomic_load_n(&gActive, __ATOMIC_ACQUIRE);

    for (uint32_t i = 0; i < gNCpus; i++) {
        const CpuProbe &c = gProbe[i];
        AMDHV_DBG("cpu %u: amd=%u fam=0x%x model=0x%x svm=%u dis=%u lock=%u rev=%u asids=%u "
                "feat=0x%x efer.svme=%u",
                i, c.vendorAmd, c.family, c.model, c.svmSupported, c.svmDisabled, c.svmLocked,
                c.svmRevision, c.numAsids, c.features, c.eferSvme);
    }
}

/**
 * @brief Enable SVM on all usable CPUs.
 *
 * @return The uint32_t result.
 */
uint32_t enableAll()
{
    AMDHV_STAT_INC(enableCalls);

    const uint32_t before = __atomic_load_n(&gActive, __ATOMIC_ACQUIRE);
    if (before && before == usableCount())
        return before;                 /* fully enabled, nothing to retry */

    for (uint32_t i = 0; i < gNCpus; i++) {
        if (cpuUsable(gProbe[i]) && !allocHostSave(i)) {
            AMDHV_ERR("enableAll: giving up, SVM stays off");
            AMDHV_STAT_INC(enableFailures);
            freeHostSave();
            return 0;
        }
    }

    /* If the next step panics, this is the last line you will see. */
    AMDHV_SYNC("enabling SVM on %u CPUs now (writing EFER and VM_HSAVE_PA)", usableCount());

    mp_rendezvous_no_intrs(enableAction, nullptr);

    uint32_t active = __atomic_load_n(&gActive, __ATOMIC_ACQUIRE);
    AMDHV_STAT_ADD(enableCpusOk, active - before);   /* CPUs added by this call, not a recount */
    if (active != usableCount()) {
        AMDHV_ERR("enableAll: only %u of %u CPUs took the enable path", active, usableCount());
        AMDHV_STAT_INC(enableFailures);
    } else {
        AMDHV_INFO("SVM enabled on %u CPUs", active);
    }
    gWantEnabled = active > 0;
    return active;
}

/**
 * @brief Take all CPUs out of SVM mode.
 */
void disableAll()
{
    AMDHV_STAT_INC(disableCalls);

    /* Close the door before the rendezvous: a vCPU that passes activeOnAllUsable() here could
     * otherwise re-enable SVME in ensureThisCpu() after disableAction ran, then take #VMEXIT
     * into a host save page that freeHostSave() is about to release. */
    __atomic_store_n(&gStopping, true, __ATOMIC_SEQ_CST);

    if (__atomic_load_n(&gActive, __ATOMIC_ACQUIRE)) {
        AMDHV_SYNC("disabling SVM on all CPUs");
        mp_rendezvous_no_intrs(disableAction, nullptr);
        __atomic_store_n(&gActive, 0, __ATOMIC_RELEASE);
    }
    bzero(gOn, sizeof(gOn));
    gWantEnabled = false;
    __atomic_store_n(&gSuspended, false, __ATOMIC_RELEASE);
    freeHostSave();
    __atomic_store_n(&gStopping, false, __ATOMIC_RELEASE);
}

/**
 * @brief Record Suspend.
 */
void noteSuspend()
{
    /* Probe-only and stopped kexts have nothing to tear down. Without this guard a suspend would
     * set the flag and resume() would enable SVM on a machine that never asked for it. */
    if (!gWantEnabled && __atomic_load_n(&gActive, __ATOMIC_ACQUIRE) == 0)
        return;
    amdhv::svm::beginSuspend(gSuspended, gSleepEpoch);
}

/**
 * @brief Record Resume.
 */
void noteResume()
{
    amdhv::svm::noteWake(gSleepEpoch);
}

/**
 * @brief Return the current sleep epoch.
 *
 * @return The uint64_t result.
 */
uint64_t sleepEpoch()
{
    return __atomic_load_n(&gSleepEpoch, __ATOMIC_ACQUIRE);
}

/**
 * @brief Notify the SVM layer that the system is suspending.
 */
void suspend()
{
    /* Before the rendezvous, not after: see SleepPolicy.hpp. A vCPU that is about to call
     * ensureThisCpu() must see the suspend and leave EFER.SVME alone. */
    noteSuspend();

    if (!__atomic_load_n(&gActive, __ATOMIC_ACQUIRE))
        return;

    AMDHV_SYNC("sleep: turning SVM off, host save areas are kept");
    mp_rendezvous_no_intrs(disableAction, nullptr);
    __atomic_store_n(&gActive, 0, __ATOMIC_RELEASE);
    bzero(gOn, sizeof(gOn));
}

/**
 * @brief Notify the SVM layer that the system has resumed.
 *
 * @return The uint32_t result.
 */
uint32_t resume()
{
    if (!__atomic_load_n(&gSuspended, __ATOMIC_ACQUIRE))
        return 0;
    if (!gWantEnabled) {
        /* Defensive: a suspended flag with SVM never enabled (or already stopped) must not turn
         * SVM on at wake. */
        amdhv::svm::endResume(gSuspended, gSleepEpoch);
        return 0;
    }

    AMDHV_SYNC("wake: turning SVM back on");
    __atomic_store_n(&gActive, 0, __ATOMIC_RELEASE);
    bzero(gOn, sizeof(gOn));
    mp_rendezvous_no_intrs(enableAction, nullptr);

    uint32_t active = __atomic_load_n(&gActive, __ATOMIC_ACQUIRE);
    /* Clear "suspended" even when the re-enable was partial: rescan() can finish it, and
     * activeOnAllUsable() still refuses to run a guest until every usable CPU is back. */
    amdhv::svm::endResume(gSuspended, gSleepEpoch);

    if (active != usableCount())
        AMDHV_ERR("wake: only %u of %u CPUs re-enabled", active, usableCount());
    else
        AMDHV_INFO("wake: SVM re-enabled on %u CPUs", active);
    return active;
}

/**
 * @brief Verify SVM state on every usable CPU.
 *
 * @param ok Success flag.
 * @param total Total byte count.
 *
 * @return The bool result.
 */
bool verifyAll(uint32_t *ok, uint32_t *total)
{
    *ok = 0;
    *total = 0;
    if (!__atomic_load_n(&gActive, __ATOMIC_ACQUIRE)) {
        AMDHV_WARN("selftest: SVM is not active, nothing to verify");
        return false;
    }

    bzero(gVerify, sizeof(gVerify));
    mp_rendezvous_no_intrs(verifyAction, nullptr);

    for (uint32_t i = 0; i < gNCpus; i++) {
        const CpuVerify &v = gVerify[i];
        if (!v.checked)
            continue;
        (*total)++;

        bool svme  = (v.efer & AMDHV_EFER_SVME) != 0;
        bool hsave = v.hsave == gHostSavePa[i];
        if (svme && hsave) {
            (*ok)++;
            AMDHV_DBG("selftest cpu %u: ok (efer=0x%llx hsave=0x%llx)", i,
                    (unsigned long long)v.efer, (unsigned long long)v.hsave);
        } else {
            AMDHV_ERR("selftest cpu %u: MISMATCH efer=0x%llx (svme=%d) hsave=0x%llx expected 0x%llx",
                    i, (unsigned long long)v.efer, svme, (unsigned long long)v.hsave,
                    (unsigned long long)gHostSavePa[i]);
        }
    }

    AMDHV_STAT_INC(selftestRuns);
    AMDHV_STAT_ADD(selftestCpusOk, *ok);
    AMDHV_INFO("selftest: %u of %u CPUs ok", *ok, *total);
    return true;
}

/**
 * @brief Count the CPUs with SVM active.
 *
 * @return The uint32_t result.
 */
uint32_t activeCpuCount()
{
    return __atomic_load_n(&gActive, __ATOMIC_ACQUIRE);
}

/**
 * @brief Check whether SVM is active on every usable CPU.
 *
 * @return The bool result.
 */
bool activeOnAllUsable()
{
    return canEnterGuest(__atomic_load_n(&gSuspended, __ATOMIC_ACQUIRE) ||
                         __atomic_load_n(&gStopping, __ATOMIC_ACQUIRE), __atomic_load_n(&gActive, __ATOMIC_ACQUIRE), usableCount());
}

/**
 * @brief Make sure SVM is enabled on the current CPU.
 *
 * @return The int result.
 */
int ensureThisCpu()
{
    /* A suspend may be in progress on another CPU. Do not repair EFER.SVME/VM_HSAVE_PA now: the
     * suspend rendezvous is about to clear them (or already did), and the caller must not VMRUN. */
    if (__atomic_load_n(&gSuspended, __ATOMIC_ACQUIRE) || __atomic_load_n(&gStopping, __ATOMIC_ACQUIRE))
        return -3;

    int cpu = cpu_number();
    if (cpu < 0 || cpu >= AMDHV_MAX_CPUS)
        return -1;
    if (!gSeenCpu[cpu])
        return -2;
    if (!cpuUsable(gProbe[cpu]) || !gHostSavePa[cpu])
        return -1;
    uint64_t efer = amdhv_rdmsr(AMDHV_MSR_EFER);
    uint64_t hsave = amdhv_rdmsr(AMDHV_MSR_VM_HSAVE_PA);
    if ((efer & AMDHV_EFER_SVME) && hsave == gHostSavePa[cpu])
        return 0;
    if (!(efer & AMDHV_EFER_SVME)) {
        amdhv_wrmsr(AMDHV_MSR_EFER, efer | AMDHV_EFER_SVME);
        /* APM 15.30.1: with SVMDIS set the SVME write is silently ignored. Never let VMRUN reach
         * CLGI with SVME clear -- that is #UD in kernel mode. */
        if (!(amdhv_rdmsr(AMDHV_MSR_EFER) & AMDHV_EFER_SVME))
            return -4;
    }
    if (hsave != gHostSavePa[cpu])
        amdhv_wrmsr(AMDHV_MSR_VM_HSAVE_PA, gHostSavePa[cpu]);
    /* enableAll/rescan count a CPU in gOn/gActive only when enableAction ran; if EFER/HSAVE drifted later we repaired
     * them here but the counters stayed behind, so activeOnAllUsable() and the stats would lie. Keep them in sync. */
    if (!gOn[cpu]) {
        gOn[cpu] = 1;
        __atomic_fetch_add(&gActive, 1, __ATOMIC_ACQ_REL);
    }
    OSIncrementAtomic(&gHealed);
    return 1;
}

/**
 * @brief Count the CPUs that were re-enabled after sleep.
 *
 * @return The uint32_t result.
 */
uint32_t healedCount()
{
    return (uint32_t)gHealed;
}

/**
 * @brief Return the number of logical CPUs.
 *
 * @return The uint32_t result.
 */
uint32_t cpuCount()
{
    return gNCpus;
}

/**
 * @brief Count the CPUs that support SVM.
 *
 * @return The uint32_t result.
 */
uint32_t usableCpuCount()
{
    return usableCount();
}

/**
 * @brief Rescan CPU SVM capabilities and refresh per-CPU state.
 *
 * @return The uint32_t result.
 */
uint32_t rescan()
{
    if (!gRescanLock)
        return 0;

    uint32_t enabled = 0;
    IOLockLock(gRescanLock);

    gNewCpus = 0;
    gRescanSeen = 0;
    mp_rendezvous_no_intrs(rescanProbeAction, nullptr);
    /* Logged the first time even when nothing is new: it tells whether the rendezvous reaches the late CPUs at all. */
    static bool sLoggedOnce;
    if (gNewCpus || !sLoggedOnce) {
        sLoggedOnce = true;
        AMDHV_INFO("rescan: the rendezvous reached %d CPUs (highest index %d), %d of them new", (int)gRescanSeen, (int)gMaxIndex, (int)gNewCpus);
    }
    if (gNewCpus) {
        uint32_t n = (uint32_t)gMaxIndex + 1;
        gNCpus = n > AMDHV_MAX_CPUS ? AMDHV_MAX_CPUS : n;
        AMDHV_INFO("rescan: %d CPUs came online after the start, %u known now, %u can use SVM",
                 (int)gNewCpus, gNCpus, usableCount());
    }

    if (gWantEnabled) {
        bool need = false;
        for (uint32_t i = 0; i < gNCpus; i++) {
            if (!cpuUsable(gProbe[i]) || gOn[i])
                continue;
            /* Allocate even while suspended: wake's enableAction() skips CPUs without a save
             * page, and the allocation itself is safe with the host asleep. */
            if (!gHostSavePa[i] && !allocHostSave(i))
                continue;
            if (!__atomic_load_n(&gSuspended, __ATOMIC_ACQUIRE))
                need = true;
        }
        if (need) {
            int before = (int)__atomic_load_n(&gActive, __ATOMIC_ACQUIRE);
            AMDHV_SYNC("rescan: enabling SVM on the CPUs that came up late");
            mp_rendezvous_no_intrs(enableAction, nullptr);
            enabled = (uint32_t)((int)__atomic_load_n(&gActive, __ATOMIC_ACQUIRE) - before);
            AMDHV_INFO("rescan: SVM now on %d of %u CPUs", (int)__atomic_load_n(&gActive, __ATOMIC_ACQUIRE), usableCount());
        }
    }

    IOLockUnlock(gRescanLock);
    return enabled;
}

} // namespace svm
} // namespace amdhv
