/*
 * TscAdjust.hpp - keeping a guest's TSC continuous across a host sleep.
 *
 * On some hardware the host TSC restarts from zero on wake. A guest's TSC is the host TSC plus the
 * VMCB's tscOffset, so the guest would stand still (or go back) until the host TSC had caught up
 * again, and a Linux guest's clock would freeze for as long as the machine had been up before it
 * slept. The SVM code counts how far the TSC went back; each vCPU adds the part it has not added
 * yet to its own TSC offset before its next VMRUN. Keeping the arithmetic here makes it testable
 * without a reboot.
 */
#ifndef AMDHV_TSC_ADJUST_HPP
#define AMDHV_TSC_ADJUST_HPP

#include <stdint.h>

namespace amdhv {
namespace tsc {

/*
 * Total ticks the host TSC was behind after the wakes so far. `beforeSleep` is the TSC read in
 * suspend(), `now` the one read in resume(). A TSC that did not go back changes nothing, and the
 * sum wraps like the offset it ends up in.
 */
inline uint64_t afterWake(uint64_t total, uint64_t beforeSleep, uint64_t now)
{
    return now < beforeSleep ? total + (beforeSleep - now) : total;
}

/*
 * What a vCPU still has to add to its TSC offset: the part of `total` it has not applied yet.
 * Each vCPU tracks what it has applied, so a vCPU created after a wake adds the whole total.
 */
inline uint64_t pending(uint64_t total, uint64_t applied) { return total - applied; }

} // namespace tsc
} // namespace amdhv

#endif
