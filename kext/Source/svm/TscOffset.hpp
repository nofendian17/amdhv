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

/** Flags word after a VM-wide hv_vm_sync_tsc: the pending relative request is consumed so a later
 * run cannot reapply (adjust - base) + relative over the VM-wide offset. */
inline uint64_t flagsAfterSync(uint64_t flags) { return flags & ~kRelativeFlag; }

/** Whether a pending set_tsc_relative request is still live in a flags word. */
inline bool relativeApplies(uint64_t flags) { return (flags & kRelativeFlag) != 0; }

/** A VM-wide sync after this run's flags snapshot wins over a pending relative request. */
inline bool relativeApplies(uint64_t flags, uint64_t genAtEntry, uint64_t genNow)
{
    return relativeApplies(flags) && genAtEntry == genNow;
}

/** Which VMCB offset write this run has to perform. */
enum class OffsetChoice : uint8_t { None, Relative, Sync };

/**
 * @brief Decide the offset write from the run's snapshots and the VM's sync generation.
 *
 * `appliedGen` is the generation whose offset this vCPU's VMCB already holds. A sync that has not
 * been applied yet wins even when it completed before this run started: otherwise the VM-wide
 * offset would never reach a vCPU created before the sync. A sync after the flags snapshot also
 * invalidates a captured relative request, so clients cannot layer both offsets.
 *
 * @param flags Flags word captured at run entry.
 * @param genAtEntry Sync generation captured at run entry.
 * @param genNow Sync generation under the TSC lock before the write.
 * @param appliedGen Sync generation already applied to this vCPU's VMCB.
 * @return The OffsetChoice result.
 */
inline OffsetChoice chooseOffset(uint64_t flags, uint64_t genAtEntry, uint64_t genNow, uint64_t appliedGen)
{
    if (appliedGen != genNow)
        return OffsetChoice::Sync;
    if (relativeApplies(flags, genAtEntry, genNow))
        return OffsetChoice::Relative;
    return OffsetChoice::None;
}

} // namespace tsc
} // namespace amdhv

#endif
