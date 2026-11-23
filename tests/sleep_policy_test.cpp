/* sleep_policy_test.cpp - host tests for kext/Source/policy/SleepPolicy.hpp. */
#include <stdio.h>

#include "../kext/Source/policy/SleepPolicy.hpp"

using namespace amdhv::svm;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    bool     suspended = false;
    uint64_t epoch     = 0;

    /* A healthy machine: every usable CPU has SVM on. */
    CHECK(canEnterGuest(suspended, 12, 12));
    CHECK(!canEnterGuest(suspended, 0, 12));
    CHECK(!canEnterGuest(suspended, 6, 12));      /* a partial re-enable must not run a guest */

    /* Suspend: the flag must be set before the EFER/HSAVE_PA rendezvous. The old code set it after,
     * so a vCPU could still pass activeOnAllUsable() and call ensureThisCpu() during teardown. */
    beginSuspend(suspended, epoch);
    CHECK(suspended);
    CHECK(epoch == 1);
    CHECK(!canEnterGuest(suspended, 12, 12));     /* teardown window: no new VMRUN */
    CHECK(needsReload(0, epoch));

    /* Both notifications report the same sleep; the second one must not move the epoch again. */
    beginSuspend(suspended, epoch);
    CHECK(epoch == 1);

    /* hv_resume() runs before the APs are online: it only moves the epoch. */
    noteWake(epoch);
    CHECK(epoch == 2);
    CHECK(suspended);                             /* still no guest entry */
    CHECK(!canEnterGuest(suspended, 12, 12));

    /* PM resume re-enabled every CPU; only now may a guest run again. */
    endResume(suspended, epoch);
    CHECK(!suspended);
    CHECK(epoch == 3);
    CHECK(canEnterGuest(suspended, 12, 12));

    /* Every vCPU built before the sleep sees a different epoch and reloads its VMCB. */
    CHECK(needsReload(0, epoch));
    CHECK(!needsReload(epoch, epoch));

    /* A failed/partial re-enable still ends the suspended state (rescan() can finish the job) but
     * canEnterGuest() stays false until the count matches. */
    beginSuspend(suspended, epoch);
    endResume(suspended, epoch);
    CHECK(!suspended);
    CHECK(!canEnterGuest(suspended, 6, 12));
    CHECK(canEnterGuest(suspended, 12, 12));

    /* A second resume without a suspend is not a transition. */
    uint64_t before = epoch;
    endResume(suspended, epoch);
    CHECK(epoch == before);

    if (fails) { printf("sleep_policy_test: %d FAILED\n", fails); return 1; }
    printf("sleep_policy_test: OK\n");
    return 0;
}
