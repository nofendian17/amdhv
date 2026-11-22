/* asid_test.cpp - host tests for kext/Source/Asid.hpp. */
#include <stdio.h>

#include "../kext/Source/Asid.hpp"

using namespace amdhv::asid;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    Pool p;

    /* A CPU that offers only the host's ASID cannot give one out. */
    p.init(1);
    CHECK(p.capacity() == 1 && p.inUse() == 1);
    CHECK(p.alloc() == 0);
    p.free(1);                        /* out of range for a 1-ASID pool: ignored */
    CHECK(p.inUse() == 1);

    p.init(0);
    CHECK(p.capacity() == 0);
    CHECK(p.alloc() == 0);

    /* ASID 0 is the host's and is never handed out. */
    p.init(4);
    CHECK(p.capacity() == 4 && p.inUse() == 1);
    const uint32_t a = p.alloc(), b = p.alloc(), c = p.alloc();
    CHECK(a == 1 && b == 2 && c == 3);
    CHECK(p.inUse() == 4);
    CHECK(p.alloc() == 0);            /* exhausted */

    /* A freed id comes back; a double free does not corrupt the count. */
    p.free(b);
    CHECK(p.inUse() == 3);
    p.free(b);
    CHECK(p.inUse() == 3);
    CHECK(p.alloc() == b && p.inUse() == 4);

    /* free(0) and out-of-range ids are no-ops. */
    p.free(0);
    p.free(4);
    p.free(0xFFFFFFFFu);
    CHECK(p.inUse() == 4);

    /* Wraparound: after 1 and 2 the search starts again at 1. */
    p.init(3);
    CHECK(p.alloc() == 1 && p.alloc() == 2);
    p.free(1);
    CHECK(p.alloc() == 1);

    /* No id is handed out twice while it is in use. */
    p.init(1000);
    static bool seen[1000];
    for (int i = 0; i < 1000; i++)
        seen[i] = false;
    for (int i = 0; i < 999; i++) {
        const uint32_t id = p.alloc();
        CHECK(id >= 1 && id < 1000 && !seen[id]);
        if (id < 1000)
            seen[id] = true;
    }
    CHECK(p.alloc() == 0);

    /* A huge CPUID count is capped, not overflowed. */
    p.init(0xFFFFFFFFu);
    CHECK(p.capacity() == kMax);
    CHECK(p.alloc() >= 1);

    if (fails) { printf("asid_test: %d FAILED\n", fails); return 1; }
    printf("asid_test: OK\n");
    return 0;
}
