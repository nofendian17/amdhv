/*
 * ion_test.cpp - host tests for the PIO notifier wire format the framework sends
 * on task traps 8/9. The mach-port side needs a kext; the decoder does not.
 */
#include <stdio.h>
#include <stdint.h>

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

/* Captured from the framework's own trap log:
 * add    (0x3F8, 1, 0x1234, port 0xa03, flags 0) -> [0x3f8, 1, 0x1234, 0xa03, 0, ...] */
void testDecode()
{
    const uint64_t w[5] = {0x3F8, 1, 0x1234, 0xA03, 0};
    AMDHVIonWire x = amdhv_ion_decode(w);
    CHECK(x.valid);
    CHECK(x.addr == 0x3F8);
    CHECK(x.size == 1);
    CHECK(x.value == 0x1234);
    CHECK(x.portName == 0xA03);
    CHECK(x.flags == 0);
}

void testLimits()
{
    const uint64_t badSize[5] = {0x3F8, 3, 0, 0xA03, 0};
    CHECK(!amdhv_ion_decode(badSize).valid);

    const uint64_t zeroSize[5] = {0x3F8, 0, 0, 0xA03, 0};
    CHECK(!amdhv_ion_decode(zeroSize).valid);

    const uint64_t word4[5] = {0x3F8, 1, 0, 0xA03, 0xC};  /* ANY_VALUE | ANY_SIZE */
    AMDHVIonWire x = amdhv_ion_decode(word4);
    CHECK(x.valid);
    CHECK(x.flags == 0xC);

    const uint64_t nullPort[5] = {0x3F8, 1, 0, 0, 0};
    CHECK(!amdhv_ion_decode(nullPort).valid);
}

} // namespace

int main()
{
    testDecode();
    testLimits();
    if (fails) {
        printf("ion_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("ion_test: OK\n");
    return 0;
}
