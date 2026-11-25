/*
 * tsc_offset_test.cpp - host-side tests for kext/Source/svm/TscOffset.hpp.
 *
 * The VMCB load itself needs guest mode, but the offset arithmetic does not.
 */
#include <stdio.h>
#include <stdint.h>

#include "../kext/Source/svm/TscOffset.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testClockIsHostTscWhenBothWordsAreZero()
{
    /* The front end initialises the commpage with base = adjust = 0: hv_tsc_clock() is rdtsc. */
    CHECK(amdhv::tsc::guestOffset(0, 0, 0) == 0);
    CHECK(amdhv::tsc::guestOffset(0, 0, 0x1234) == 0x1234);
}

void testClockAdjustIsFoldedIn()
{
    /* hv_tsc_clock() = rdtsc + adjust - base, so the offset carries the same delta. */
    CHECK(amdhv::tsc::guestOffset(100, 300, 0) == 200);
    CHECK(amdhv::tsc::guestOffset(300, 100, 0) == (uint64_t)-200);
    CHECK(amdhv::tsc::guestOffset(100, 300, 0x50) == 280);
    /* Negative relative values are ordinary two's-complement offsets. */
    CHECK(amdhv::tsc::guestOffset(0, 0, (uint64_t)-0x1000) == (uint64_t)-0x1000);
}

void testWrapsLikeTheHardware()
{
    const uint64_t base = 1;
    const uint64_t adjust = 0;
    const uint64_t rel = 0;
    CHECK(amdhv::tsc::guestOffset(base, adjust, rel) == (uint64_t)-1);
    CHECK(amdhv::tsc::guestOffset(UINT64_MAX, 1, 5) == 7);
}

void testRelativeIsAddedOnce()
{
    for (uint64_t r = 0; r < 8; r++) {
        const uint64_t delta = 0x1000;
        CHECK(amdhv::tsc::guestOffset(7, 7 + delta, r) == delta + r);
    }
    CHECK((amdhv::tsc::kRelativeFlag & 0x100) != 0);
    CHECK((amdhv::tsc::kRelativeFlag & 0x4) == 0);   /* not the MSR-cache flag */
}

void testSyncTscConsumesTheRelativeRequest()
{
    /* hv_vm_sync_tsc is the last writer: a pending set_tsc_relative request must not survive it,
     * or the next run reapplies (adjust - base) + relative over the VM-wide offset. */
    const uint64_t flagsWithRelative = amdhv::tsc::kRelativeFlag | 0x01 | 0x04;
    CHECK(amdhv::tsc::flagsAfterSync(flagsWithRelative) == (0x01 | 0x04));
    CHECK(amdhv::tsc::flagsAfterSync(0x01) == 0x01);   /* other flags are untouched */
    /* A consumed request must not be applied by a later run. */
    CHECK(amdhv::tsc::relativeApplies(flagsWithRelative));
    CHECK(!amdhv::tsc::relativeApplies(amdhv::tsc::flagsAfterSync(flagsWithRelative)));
    /* A sync between the flags snapshot and the write invalidates the captured request. */
    CHECK(amdhv::tsc::relativeApplies(flagsWithRelative, 7, 7));
    CHECK(!amdhv::tsc::relativeApplies(flagsWithRelative, 7, 8));
}

void testSyncBetweenRunsIsAppliedToAnExistingVcpu()
{
    const uint64_t flagsWithRelative = amdhv::tsc::kRelativeFlag | 0x01;

    /* A sync that completed before this run started still has to reach a vCPU created earlier:
     * its applied generation lags the VM's generation even though no change happens during the run. */
    CHECK(amdhv::tsc::chooseOffset(amdhv::tsc::flagsAfterSync(flagsWithRelative), 1, 1, 0) ==
          amdhv::tsc::OffsetChoice::Sync);

    /* A relative request written after a still-unapplied sync waits one run: the sync wins first. */
    CHECK(amdhv::tsc::chooseOffset(flagsWithRelative, 1, 1, 0) == amdhv::tsc::OffsetChoice::Sync);

    /* With no sync pending, a relative request is applied once. */
    CHECK(amdhv::tsc::chooseOffset(flagsWithRelative, 1, 1, 1) == amdhv::tsc::OffsetChoice::Relative);

    /* A sync landing after the run's snapshot wins over the captured relative request. */
    CHECK(amdhv::tsc::chooseOffset(flagsWithRelative, 1, 2, 1) == amdhv::tsc::OffsetChoice::Sync);

    /* Nothing pending: leave the VMCB offset as it is. */
    CHECK(amdhv::tsc::chooseOffset(0x01, 1, 1, 1) == amdhv::tsc::OffsetChoice::None);
}

} // namespace

int main()
{
    testClockIsHostTscWhenBothWordsAreZero();
    testClockAdjustIsFoldedIn();
    testWrapsLikeTheHardware();
    testRelativeIsAddedOnce();
    testSyncTscConsumesTheRelativeRequest();
    testSyncBetweenRunsIsAppliedToAnExistingVcpu();
    if (fails) {
        printf("tsc_offset_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("tsc_offset_test: OK\n");
    return 0;
}
