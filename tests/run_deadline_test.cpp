/*
 * run_deadline_test.cpp - host-side tests for kext/Source/policy/RunDeadline.hpp.
 *
 * The run loop itself needs a CPU in guest mode, but the rule that decides when it stops does not.
 */
#include <stdio.h>
#include <stdint.h>

#include "../kext/Source/policy/RunDeadline.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testStopTime()
{
    /* No client deadline: the core's slice is the only limit. */
    CHECK(amdhv::run::stopTime(100, 0) == 100);
    /* A client deadline earlier than the slice wins. */
    CHECK(amdhv::run::stopTime(100, 50) == 50);
    CHECK(amdhv::run::stopTime(100, 1) == 1);
    /* A client deadline later than the slice does not extend it. */
    CHECK(amdhv::run::stopTime(100, 150) == 100);
    /* Equal values pick the same instant either way. */
    CHECK(amdhv::run::stopTime(100, 100) == 100);
    /* A deadline of 1 is a real deadline, not "none": mach_absolute_time is never 0 in practice,
     * but the rule must still be "0 means none" and nothing else. */
    CHECK(amdhv::run::stopTime(100, 2) == 2);
}

void testExpired()
{
    CHECK(!amdhv::run::expired(99, 100));
    CHECK(amdhv::run::expired(100, 100));
    CHECK(amdhv::run::expired(101, 100));
    CHECK(amdhv::run::expired(0, 0));
    /* An already-past deadline is expired immediately, which is what makes run_until(now) return
     * without entering the guest. */
    CHECK(amdhv::run::expired(1000, 999));
}

void testNeverExtendsSlice()
{
    const uint64_t now = 123456;
    const uint64_t sliceEnd = now + 10 * 1000 * 1000;
    for (uint64_t d = 0; d < 40; d += 7) {
        const uint64_t client = d == 0 ? 0 : now + d * 1000 * 1000;
        const uint64_t stop = amdhv::run::stopTime(sliceEnd, client);
        CHECK(stop <= sliceEnd);
        CHECK(client == 0 || stop <= client);
    }
}

} // namespace

int main()
{
    testStopTime();
    testExpired();
    testNeverExtendsSlice();
    if (fails) {
        printf("run_deadline_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("run_deadline_test: OK\n");
    return 0;
}
