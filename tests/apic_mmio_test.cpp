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

void testAddressPrefix()
{
    /* 66 67 89 05 00 00 C0 FE = MOV [0xFEC00000], EAX in 16-bit real mode:
     * 66 sets a 32-bit operand, 67 a 32-bit address. */
    const uint8_t write[] = { 0x66, 0x67, 0x89, 0x05, 0x00, 0x00, 0xC0, 0xFE };
    amdhv::apic::MmioOp op = amdhv::apic::decodeMmio(write, sizeof(write));
    CHECK(op.valid);
    CHECK(op.isWrite);
    CHECK(op.disp == 0xFEC00000u);
    CHECK(op.length == 8);

    const uint8_t read[] = { 0x67, 0x66, 0x8B, 0x05, 0x10, 0x00, 0xC0, 0xFE };
    op = amdhv::apic::decodeMmio(read, sizeof(read));
    CHECK(op.valid);
    CHECK(!op.isWrite);
    CHECK(op.disp == 0xFEC00010u);
    CHECK(op.length == 8);
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

void testDecodeAssists()
{
    /* VMCB 0x0D1: only insnLen bytes are valid, the rest of the 15-byte array is stale. */
    uint8_t bytes[15] = { 0x8B, 0x05, 0x80, 0x00, 0xE0, 0xFE, 0xCC, 0xCC, 0xCC };
    amdhv::apic::MmioOp op = amdhv::apic::decodeMmioAssist(bytes, 6, 0xFEE00080u);
    CHECK(op.valid);
    CHECK(!op.isWrite);
    CHECK(op.reg == 0);
    CHECK(op.disp == 0xFEE00080u);
    CHECK(op.length == 6);

    /* A captured instruction must match the GPA that faulted; otherwise it is unrelated. */
    CHECK(!amdhv::apic::decodeMmioAssist(bytes, 6, 0xFEE000B0u).valid);

    /* No bytes captured, or a truncated capture, means fall back to the old NPT lookup. */
    CHECK(!amdhv::apic::decodeMmioAssist(bytes, 0, 0xFEE00080u).valid);
    CHECK(!amdhv::apic::decodeMmioAssist(bytes, 5, 0xFEE00080u).valid);
    CHECK(!amdhv::apic::decodeMmioAssist(bytes, 16, 0xFEE00080u).valid);

    /* Two prefixes need 8 captured bytes; 7 is not enough. */
    const uint8_t prefixed[15] = { 0x66, 0x67, 0x89, 0x05, 0x00, 0x00, 0xC0, 0xFE };
    op = amdhv::apic::decodeMmioAssist(prefixed, 8, 0xFEC00000u);
    CHECK(op.valid);
    CHECK(op.isWrite);
    CHECK(op.disp == 0xFEC00000u);
    CHECK(!amdhv::apic::decodeMmioAssist(prefixed, 7, 0xFEC00000u).valid);
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
    testAddressPrefix();
    testWrite();
    testRejectsEverythingElse();
    testDecodeAssists();
    testOffset();
    if (fails) {
        printf("apic_mmio_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("apic_mmio_test: OK\n");
    return 0;
}
