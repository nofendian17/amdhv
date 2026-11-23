/*
 * RunDeadline.hpp - when a run must stop.
 *
 * hv_vcpu_run() has no deadline: it blocks until the guest exits. hv_vcpu_run_until() adds an
 * absolute mach_absolute_time deadline (HV_DEADLINE_FOREVER for the plain behaviour) and is
 * documented to use the VMX preemption timer. AMD SVM has no such timer, so the core folds the
 * client's deadline into the 10 ms slice it already uses to give the host CPU back: the run stops
 * at whichever comes first. Keeping the arithmetic here makes it testable without a reboot.
 */
#ifndef AMDHV_RUN_DEADLINE_HPP
#define AMDHV_RUN_DEADLINE_HPP

#include <stdint.h>

namespace amdhv {
namespace run {

/**
 * @brief Choose the earlier of the time-slice end and the client deadline.
 *
 * @param sliceEnd End of the core's timeslice.
 * @param clientDeadline Client-supplied run deadline.
 *
 * @return The uint64_t result.
 */
inline uint64_t stopTime(uint64_t sliceEnd, uint64_t clientDeadline)
{
    return (clientDeadline != 0 && clientDeadline < sliceEnd) ? clientDeadline : sliceEnd;
}

/** True once `now` has reached `stop`: do not enter (or re-enter) the guest. */
inline bool expired(uint64_t now, uint64_t stop) { return now >= stop; }

} // namespace run
} // namespace amdhv

#endif
