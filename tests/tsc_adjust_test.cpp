/*
 * tsc_adjust_test.cpp - host-side tests for kext/Source/TscAdjust.hpp.
 *
 * The host TSC only goes back across a real sleep, but the arithmetic that repairs the guest's
 * offset does not need one.
 */
#include <stdio.h>
#include <stdint.h>

#include "../kext/Source/TscAdjust.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testAfterWake()
{
    /* The usual case on hardware whose TSC keeps counting: nothing to repair. */
    CHECK(amdhv::tsc::afterWake(0, 1000, 1000) == 0);
    CHECK(amdhv::tsc::afterWake(0, 1000, 2000) == 0);
    CHECK(amdhv::tsc::afterWake(77, 1000, 2000) == 77);
    /* The TSC restarted: the guest needs the difference that went missing. */
    CHECK(amdhv::tsc::afterWake(0, 1000, 0) == 1000);
    CHECK(amdhv::tsc::afterWake(0, 1000, 250) == 750);
    /* A second sleep adds to what the first one took, it does not replace it. */
    CHECK(amdhv::tsc::afterWake(1000, 5000, 1000) == 5000);
    CHECK(amdhv::tsc::afterWake(5000, 7000, 6500) == 5500);
    /* One tick behind is still a backwards TSC. */
    CHECK(amdhv::tsc::afterWake(0, 1000, 999) == 1);
}

void testPending()
{
    /* Nothing new since the last VMRUN: the offset stays as it is. */
    CHECK(amdhv::tsc::pending(1000, 1000) == 0);
    CHECK(amdhv::tsc::pending(0, 0) == 0);
    /* A vCPU that has applied nothing (created after the wake) adds the whole total. */
    CHECK(amdhv::tsc::pending(1000, 0) == 1000);
    /* A vCPU that already applied the first wake adds only the second. */
    CHECK(amdhv::tsc::pending(1500, 1000) == 500);
}

void testOffsetStaysContinuous()
{
    /* What run() does: offset += pending(total, applied); applied = total. The guest's TSC
     * (host TSC + offset) must not go back across a sleep. */
    const uint64_t hostBefore = 50000;     /* TSC in suspend() */
    const uint64_t hostAfter  = 3000;      /* TSC in resume(): the host restarted */
    uint64_t total = 0, applied = 0, offset = 0;

    total = amdhv::tsc::afterWake(total, hostBefore, hostAfter);
    offset += amdhv::tsc::pending(total, applied);
    applied = total;
    /* Guest time at the last exit was hostBefore + offset; after the wake it is hostAfter + offset
     * and must be at least that. */
    CHECK(hostAfter + offset >= hostBefore);

    /* A second sleep, with the host TSC counting normally this time. */
    total = amdhv::tsc::afterWake(total, 90000, 95000);
    offset += amdhv::tsc::pending(total, applied);
    applied = total;
    CHECK(hostAfter + offset == hostBefore);      /* no double count */
    CHECK(amdhv::tsc::pending(total, applied) == 0);

    /* A vCPU created after both wakes starts at applied = 0 and gets both repairs. */
    uint64_t lateOffset = 0;
    lateOffset += amdhv::tsc::pending(total, 0);
    CHECK(lateOffset == offset);
}

void testWrapsLikeTheOffset()
{
    /* Both the total and the offset are uint64_t and wrap the same way; the repair must not
     * assume they never do. */
    CHECK(amdhv::tsc::pending(5, UINT64_MAX) == 6);
    CHECK(amdhv::tsc::afterWake(UINT64_MAX, 10, 5) == 4);
}

} // namespace

int main()
{
    testAfterWake();
    testPending();
    testOffsetStaysContinuous();
    testWrapsLikeTheOffset();
    if (fails) {
        printf("tsc_adjust_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("tsc_adjust_test: OK\n");
    return 0;
}
