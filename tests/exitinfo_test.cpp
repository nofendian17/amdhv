/*
 * exitinfo_test.cpp - host tests for the exit-info payload the framework reads
 * from the vCPU's read-only page (see docs/CAPABILITIES.md).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../include/AMDHVShared.h"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

static uint32_t rd32(const uint8_t *p, unsigned off) { uint32_t v; memcpy(&v, p + off, 4); return v; }
static uint64_t rd64(const uint8_t *p, unsigned off) { uint64_t v; memcpy(&v, p + off, 8); return v; }

void testEncode()
{
    AMDHVExitInfo x = {};
    x.code = 3;                    /* STARTUP_AP */
    x.bitmap = 0b1010;             /* vCPUs 1 and 3 */
    x.value = 0x30;
    x.rip = 0x1200;
    uint8_t ro[0x100] = {};
    amdhv_exitinfo_encode(ro, &x);
    CHECK(rd32(ro, 0x20) == 3);
    CHECK(rd64(ro, 0x28) == 0b1010);
    CHECK(rd32(ro, 0x48) == 0x30);
    CHECK(ro[0x49] == 0);
    CHECK(rd32(ro, 0x4c) == 0);
    CHECK(rd64(ro, 0x50) == 0x1200);
}

void testInjectExcp()
{
    AMDHVExitInfo x = {};
    x.code = 5;                    /* INJECT_EXCP */
    x.value = 13;                  /* #GP */
    x.valid = 1;
    x.err = 0;
    uint8_t ro[0x100] = {};
    amdhv_exitinfo_encode(ro, &x);
    CHECK(rd32(ro, 0x20) == 5);
    CHECK(ro[0x48] == 13);
    CHECK(ro[0x49] == 1);
    CHECK(rd32(ro, 0x4c) == 0);
    CHECK(rd64(ro, 0x50) == 0);    /* restart = 0 */
}

void testClear()
{
    uint8_t ro[0x100];
    memset(ro, 0xAA, sizeof(ro));
    AMDHVExitInfo x = {};
    amdhv_exitinfo_encode(ro, &x);
    CHECK(rd32(ro, 0x20) == 0);
    CHECK(rd64(ro, 0x28) == 0);
    CHECK(rd32(ro, 0x48) == 0);
    CHECK(rd64(ro, 0x50) == 0);
}

} // namespace

int main()
{
    testEncode();
    testInjectExcp();
    testClear();
    if (fails) {
        printf("exitinfo_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("exitinfo_test: OK\n");
    return 0;
}
