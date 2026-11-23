/*
 * SleepPolicy.hpp - the state rules for an S3/S4 sleep with live VMs.
 *
 * Kept free of kernel headers so the ordering can be tested on any host
 * (tests/sleep_policy_test.cpp). The two rules that matter:
 *
 *  1. "suspended" must be set *before* the CPUs are taken out of SVM mode. A vCPU that is between its
 *     last check and VMRUN would otherwise re-enable EFER.SVME (ensureThisCpu) behind the suspend and
 *     enter the guest while the system is powering down.
 *  2. The processor's cached copy of a VMCB is lost when power is removed, so every clean bit is
 *     stale after a resume. An epoch that changes on suspend and resume lets each vCPU notice without
 *     walking a list of VMs from the power-management callback.
 */
#ifndef AMDHV_SLEEP_POLICY_HPP
#define AMDHV_SLEEP_POLICY_HPP

#include <stdint.h>

namespace amdhv {
namespace svm {

/** VmRun may enter the guest only when no sleep is in progress and every usable CPU has SVM on. */
static inline bool canEnterGuest(bool suspended, uint32_t activeCpus, uint32_t usableCpus)
{
    return !suspended && activeCpus > 0 && activeCpus == usableCpus;
}

/** A VMCB built before the sleep must be reloaded in full after it. */
static inline bool needsReload(uint64_t vcpuEpoch, uint64_t sleepEpoch)
{
    return vcpuEpoch != sleepEpoch;
}

/** First step of a suspend: refuse new guest entries, then tear the CPUs down. Idempotent, because the
 * power-management notification and the kernel's hv_suspend() callback both report the same event. */
static inline void beginSuspend(bool &suspended, uint64_t &epoch)
{
    if (!suspended) {
        suspended = true;
        __atomic_add_fetch(&epoch, 1, __ATOMIC_RELEASE);
    }
}

/** The system is coming back but the CPUs are not re-enabled yet: only the epoch moves. */
static inline void noteWake(uint64_t &epoch)
{
    __atomic_add_fetch(&epoch, 1, __ATOMIC_RELEASE);
}

/** Last step of a resume, after every usable CPU has SVM on again (or the attempt has finished). */
static inline void endResume(bool &suspended, uint64_t &epoch)
{
    if (suspended) {
        suspended = false;
        __atomic_add_fetch(&epoch, 1, __ATOMIC_RELEASE);
    }
}

} // namespace svm
} // namespace amdhv

#endif /* AMDHV_SLEEP_POLICY_HPP */
