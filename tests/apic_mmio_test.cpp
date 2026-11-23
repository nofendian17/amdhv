/*
 * apic_mmio_test.cpp - host-side tests for kext/Source/arch/ApicMmio.hpp.
 */
#include <stdio.h>
#include <stdint.h>

#include "../kext/Source/arch/ApicMmio.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testRead()
{
    /* 8B 05 80 00 EE FE = MOV EAX, [0xFEE00080] (LAPIC TPR). */
    const uint8_t code[] = { 0x8B, 0x05, 0x80, 0x00, 0xE0, 0xFE };
    const amdhv::apic::MmioOp op = amdhv::apic::decodeMmio(code, sizeof(code));
    CHECK(op.valid);
    CHECK(!op.isWrite);
    CHECK(op.reg == 0);
    CHECK(op.disp == 0xFEE00080u);
    CHECK(op.length == 6);
}

void testPrefix()
{
    /* 66 8B 05 80 00 E0 FE = MOV EAX, [0xFEE00080] in 16-bit real mode. */
    const uint8_t code[] = { 0x66, 0x8B, 0x05, 0x80, 0x00, 0xE0, 0xFE };
    const amdhv::apic::MmioOp op = amdhv::apic::decodeMmio(code, sizeof(code));
    CHECK(op.valid);
    CHECK(!op.isWrite);
    CHECK(op.disp == 0xFEE00080u);
    CHECK(op.length == 7);
}

void testWrite()
{
    /* 89 0D 00 03 FE FE = MOV [0xFEE00300], ECX (ICR low). */
    const uint8_t code[] = { 0x89, 0x0D, 0x00, 0x03, 0xE0, 0xFE };
    const amdhv::apic::MmioOp op = amdhv::apic::decodeMmio(code, sizeof(code));
    CHECK(op.valid);
    CHECK(op.isWrite);
    CHECK(op.reg == 1);
    CHECK(op.disp == 0xFEE00300u);
}

void testRejectsEverythingElse()
{
    const uint8_t absolute[] = { 0x8B, 0x05, 0x80, 0x00, 0xE0, 0xFE };
    CHECK(!amdhv::apic::decodeMmio(absolute, 5).valid);          /* truncated */
    /* An operand-size prefix needs one extra byte: 6 bytes total is not enough. */
    const uint8_t shortPrefix[] = { 0x66, 0x8B, 0x05, 0x80, 0x00, 0xE0 };
    CHECK(!amdhv::apic::decodeMmio(shortPrefix, sizeof(shortPrefix)).valid);
    const uint8_t indirect[] = { 0x8B, 0x00, 0x90, 0x90, 0x90, 0x90 };
    CHECK(!amdhv::apic::decodeMmio(indirect, sizeof(indirect)).valid); /* [RAX], not absolute */
    const uint8_t other[] = { 0xA1, 0x80, 0x00, 0xE0, 0xFE, 0x90 };
    CHECK(!amdhv::apic::decodeMmio(other, sizeof(other)).valid); /* MOV AL, moffs */
}

void testOffset()
{
    CHECK(amdhv::apic::mmioOffset(0xFEE00080, 0xFEE00000) == 0x80);
    CHECK(amdhv::apic::mmioOffset(0xFEC00010, 0xFEC00000) == 0x10);
    CHECK(amdhv::apic::mmioOffset(0xFEE01000, 0xFEE00000) == 0xFFFFFFFFu);
    CHECK(amdhv::apic::mmioOffset(0x1000, 0xFEE00000) == 0xFFFFFFFFu);
}

} // namespace

int main()
{
    testRead();
    testPrefix();
    testWrite();
    testRejectsEverythingElse();
    testOffset();
    if (fails) {
        printf("apic_mmio_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("apic_mmio_test: OK\n");
    return 0;
}
