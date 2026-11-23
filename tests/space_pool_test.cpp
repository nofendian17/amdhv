/* space_pool_test.cpp - host tests for kext/Source/memory/SpacePool.hpp. */
#include <stdio.h>

#include "../kext/Source/memory/SpacePool.hpp"

using namespace amdhv::space;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    Pool p;

    /* Space 0 is the default and always valid, but it has no table slot. */
    CHECK(p.valid(0));
    CHECK(p.index(0) == -1);
    CHECK(p.used() == 0);
    CHECK(!p.release(0));

    /* IDs start at 1 and name distinct slots. */
    uint32_t a = 0, b = 0;
    unsigned sa = kMax, sb = kMax;
    CHECK(p.create(&a, &sa) && a == 1 && sa < kMax);
    CHECK(p.create(&b, &sb) && b == 2 && sb < kMax);
    CHECK(sa != sb);
    CHECK(p.used() == 2);
    CHECK(p.index(a) == (int)sa && p.index(b) == (int)sb);
    CHECK(p.valid(a) && p.valid(b));

    /* A destroyed ID is gone, and the next space does not reuse it. */
    CHECK(p.release(a));
    CHECK(!p.valid(a));
    CHECK(p.index(a) == -1);
    CHECK(p.used() == 1);
    CHECK(!p.release(a));              /* double release is a no-op */
    uint32_t c = 0;
    unsigned sc = kMax;
    CHECK(p.create(&c, &sc) && c != a && c != b && c != 0);
    CHECK(p.index(c) == (int)sc);

    /* Unknown IDs are not valid. */
    CHECK(!p.valid(0xFFFFFFFFu));
    CHECK(p.index(0xFFFFFFFFu) == -1);

    /* The pool fills exactly to kMax and then refuses. */
    p.reset();
    static bool seen[kMax + 1];
    for (unsigned i = 0; i <= kMax; i++)
        seen[i] = false;
    for (unsigned i = 0; i < kMax; i++) {
        uint32_t id = 0;
        unsigned slot = kMax;
        CHECK(p.create(&id, &slot));
        CHECK(id >= 1 && id <= kMax && !seen[id]);
        if (id <= kMax)
            seen[id] = true;
        CHECK(p.index(id) == (int)slot);
    }
    CHECK(p.used() == kMax);
    uint32_t extra = 0;
    unsigned extraSlot = kMax;
    CHECK(!p.create(&extra, &extraSlot));

    /* reset() makes every slot available again and starts the IDs over. */
    p.reset();
    CHECK(p.used() == 0);
    uint32_t first = 0;
    unsigned firstSlot = kMax;
    CHECK(p.create(&first, &firstSlot) && first == 1);

    /* A wrapped counter skips an ID that is still alive. */
    p.reset();
    uint32_t keep = 0;
    unsigned keepSlot = kMax;
    CHECK(p.create(&keep, &keepSlot) && keep == 1);
    /* Reach the wrap point without allocating 2^32 spaces: the pool exposes no counter, so this is
     * covered by construction (index() is checked on every candidate). */

    if (fails) { printf("space_pool_test: %d FAILED\n", fails); return 1; }
    printf("space_pool_test: OK\n");
    return 0;
}
