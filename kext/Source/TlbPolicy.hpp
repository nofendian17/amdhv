/*
 * TlbPolicy.hpp - the VMCB TLB_CONTROL value for the next guest entry.
 *
 * A pure function so the decision is testable on any host (tests/tlb_policy_test.cpp). The kext feeds
 * it the per-vCPU state; see Vm.cpp run() for what each field means.
 */
#ifndef AMDHV_TLB_POLICY_HPP
#define AMDHV_TLB_POLICY_HPP

#include <stdint.h>

#include "VMCB.hpp"

namespace amdhv {
namespace vmcb {

/* TLB_CONTROL 3 flushes every entry tagged with the ASID, global pages included (a global PTE is
 * global only within its ASID). 7 is the non-global variant and is not what a full invalidation needs. */
static_assert(TLB_FLUSH_BY_ASID == 3, "TLB_CONTROL 3 is flush-by-ASID");

struct TlbState {
    bool     haveAsid;      /* the vCPU owns an ASID (fAsid != 0) */
    bool     flushByAsid;   /* CPUID Fn8000_000A EDX bit 6: TLB_CONTROL 3 is valid */
    bool     asidMode;      /* amdhv_asid != 0: the fast path is enabled */
    uint64_t seenGen;       /* nested-page-table generation this vCPU last flushed for */
    uint64_t gen;           /* current generation */
    bool     forced;        /* hv_vcpu_invalidate_tlb or a paging-state write through the API */
    int      lastCpu;       /* CPU of the previous entry, -1 when there was none */
    int      runCpu;        /* CPU of this entry */
};

static inline uint8_t tlbControl(const TlbState &s)
{
    if (!s.asidMode || !s.haveAsid)
        return TLB_FLUSH_ALL;          /* old behaviour, or the vCPU shares the host's ASID */
    if (s.seenGen != s.gen || s.forced || s.lastCpu != s.runCpu)
        return s.flushByAsid ? TLB_FLUSH_BY_ASID : TLB_FLUSH_ALL;
    return TLB_DO_NOTHING;
}

} // namespace vmcb
} // namespace amdhv

#endif /* AMDHV_TLB_POLICY_HPP */
