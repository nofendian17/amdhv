/* tlb_policy_test.cpp - host tests for kext/Source/TlbPolicy.hpp. */
#include <stdio.h>

#include "../kext/Source/TlbPolicy.hpp"

using namespace amdhv::vmcb;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* Everything steady: the vCPU keeps its own ASID on the same CPU with unchanged tables. */
static TlbState steady()
{
    TlbState s;
    s.haveAsid    = true;
    s.flushByAsid = true;
    s.asidMode    = true;
    s.seenGen     = 7;
    s.gen         = 7;
    s.forced      = false;
    s.lastCpu     = 3;
    s.runCpu      = 3;
    return s;
}

int main()
{
    TlbState s = steady();
    CHECK(tlbControl(s) == TLB_DO_NOTHING);

    /* A new nested page table generation: flush only this ASID. */
    s = steady(); s.gen = 8;
    CHECK(tlbControl(s) == TLB_FLUSH_BY_ASID);

    /* An explicit request (hv_vcpu_invalidate_tlb / API paging write). */
    s = steady(); s.forced = true;
    CHECK(tlbControl(s) == TLB_FLUSH_BY_ASID);

    /* The vCPU moved: the new CPU may hold this ASID from a previous owner. */
    s = steady(); s.runCpu = 4;
    CHECK(tlbControl(s) == TLB_FLUSH_BY_ASID);
    s = steady(); s.lastCpu = -1;
    CHECK(tlbControl(s) == TLB_FLUSH_BY_ASID);

    /* No FlushByAsid: any flush is a full flush. */
    s = steady(); s.flushByAsid = false; s.gen = 8;
    CHECK(tlbControl(s) == TLB_FLUSH_ALL);
    s = steady(); s.flushByAsid = false;
    CHECK(tlbControl(s) == TLB_DO_NOTHING);

    /* amdhv_asid=0 restores the old behaviour, even with a valid ASID. */
    s = steady(); s.asidMode = false;
    CHECK(tlbControl(s) == TLB_FLUSH_ALL);
    s = steady(); s.asidMode = false; s.flushByAsid = false;
    CHECK(tlbControl(s) == TLB_FLUSH_ALL);

    /* ASID 0 (the pool is empty) shares the host's TLB: always a full flush. */
    s = steady(); s.haveAsid = false;
    CHECK(tlbControl(s) == TLB_FLUSH_ALL);
    s = steady(); s.haveAsid = false; s.flushByAsid = false;
    CHECK(tlbControl(s) == TLB_FLUSH_ALL);

    if (fails) { printf("tlb_policy_test: %d FAILED\n", fails); return 1; }
    printf("tlb_policy_test: OK\n");
    return 0;
}
