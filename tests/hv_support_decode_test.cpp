/*
 * hv_support_decode_test.cpp - host-side tests for kext/Source/HvSupportDecode.hpp.
 *
 * The address itself is kernel-specific, but the RIP-relative arithmetic and the two
 * accepted encodings are not. A bad decode must return nullptr, never a guessed address.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../kext/Source/HvSupportDecode.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testFramePointerBuild()
{
    /* Both the synthetic code and its target live in this frame: the 32-bit displacement
     * must be able to span the distance (a global would be too far away under PIE). */
    int target = 7;
    uint8_t code[16] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x05, 0, 0, 0, 0, 0x5d, 0xc3, 0, 0, 0, 0};
    int32_t disp = (int32_t)((intptr_t)&target - (intptr_t)(code + 10));
    memcpy(code + 6, &disp, sizeof(disp));
    CHECK(amdhv::hv::decodeSupportPtr(code) == &target);
}

void testNoFramePointerBuild()
{
    int target = 7;
    uint8_t code[16] = {0x8b, 0x05, 0, 0, 0, 0, 0xc3, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    int32_t disp = (int32_t)((intptr_t)&target - (intptr_t)(code + 6));
    memcpy(code + 2, &disp, sizeof(disp));
    CHECK(amdhv::hv::decodeSupportPtr(code) == &target);
}

void testRejectsUnknownCode()
{
    uint8_t other[16] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x0d, 0, 0, 0, 0, 0x5d, 0xc3, 0, 0, 0, 0};
    CHECK(amdhv::hv::decodeSupportPtr(other) == nullptr);   /* mov ecx, not eax */
    other[5] = 0x05;
    other[11] = 0x90;
    CHECK(amdhv::hv::decodeSupportPtr(other) == nullptr);   /* no ret where expected */
    uint8_t plain[16] = {0x8b, 0x05, 0, 0, 0, 0, 0x90, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(amdhv::hv::decodeSupportPtr(plain) == nullptr);
}

} // namespace

int main()
{
    testFramePointerBuild();
    testNoFramePointerBuild();
    testRejectsUnknownCode();
    if (fails) {
        printf("hv_support_decode_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("hv_support_decode_test: OK\n");
    return 0;
}
