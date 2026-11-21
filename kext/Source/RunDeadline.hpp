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

/*
 * `sliceEnd` is the end of the core's own timeslice; `clientDeadline` is the run_until deadline, or
 * 0 for "no client deadline" (hv_vcpu_run / HV_DEADLINE_FOREVER). The result is never later than
 * sliceEnd, so a client cannot keep a host CPU longer than the slice.
 */
inline uint64_t stopTime(uint64_t sliceEnd, uint64_t clientDeadline)
{
    return (clientDeadline != 0 && clientDeadline < sliceEnd) ? clientDeadline : sliceEnd;
}

/* True once `now` has reached `stop`: do not enter (or re-enter) the guest. */
inline bool expired(uint64_t now, uint64_t stop) { return now >= stop; }

} // namespace run
} // namespace amdhv

#endif
