/*
 * TscOffset.hpp - the VMCB TSC offset a framework client asked for.
 *
 * hv_vcpu_set_tsc_relative() wants the guest TSC to read hv_tsc_clock() + value. AMDHV implements
 * hv_tsc_clock() as rdtsc + (adjust - base), where both words live in the framework's commpage, so
 * the VMCB offset the core must load is (adjust - base) + value. The arithmetic is unsigned and
 * wraps exactly as the hardware's TSC offset does. Keeping it here makes it testable without a
 * reboot (tests/tsc_offset_test.cpp).
 */
#ifndef AMDHV_TSC_OFFSET_HPP
#define AMDHV_TSC_OFFSET_HPP

#include <stdint.h>

namespace amdhv {
namespace tsc {

/** VMCB.tscOffset so that guest TSC = rdtsc + result = hv_tsc_clock() + relative. */
inline uint64_t guestOffset(uint64_t clockBase, uint64_t clockAdjust, uint64_t relative)
{
    return (clockAdjust - clockBase) + relative;
}

/* The framework sets flag bit 0x100 in the rw slice for hv_vcpu_set_tsc_relative. */
constexpr uint64_t kRelativeFlag = 0x100ull;

} // namespace tsc
} // namespace amdhv

#endif
