/*
 * apic_test.cpp - host-side tests for kext/Source/arch/ApicState.hpp.
 *
 * The guest-facing wiring needs a kext, but the register rules do not.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../kext/Source/arch/ApicState.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testLapicPriority()
{
    amdhv::apic::LapicState s;
    amdhv::apic::reset(s);
    CHECK(amdhv::apic::tprClass(s) == 0);
    CHECK(amdhv::apic::pprClass(s) == 0);
    s.tpr = 0x50;
    CHECK(amdhv::apic::tprClass(s) == 5);
    CHECK(amdhv::apic::pprClass(s) == 5);
    amdhv::apic::setIrr(s, 0x61);
    CHECK(amdhv::apic::takeInterrupt(s) == 0x61);   /* class 6 beats TPR class 5 */
    CHECK(amdhv::apic::highestIsrVector(s) == 0x61);
    CHECK(amdhv::apic::pprClass(s) == 6);
    CHECK(amdhv::apic::takeInterrupt(s) == 0);
    amdhv::apic::setIrr(s, 0x55);                   /* same class as TPR: blocked */
    CHECK(amdhv::apic::takeInterrupt(s) == 0);
    amdhv::apic::setEoi(s, 0x61);
    CHECK(amdhv::apic::highestIsrVector(s) == 0);
    s.tpr = 0x40;                               /* class 4 now lets the vector 0x55 through */
    CHECK(amdhv::apic::takeInterrupt(s) == 0x55);
}

void testLapicBounds()
{
    amdhv::apic::LapicState s;
    amdhv::apic::reset(s);
    amdhv::apic::setIrr(s, 0);
    amdhv::apic::setIrr(s, 15);
    amdhv::apic::setIrr(s, 256);
    for (unsigned i = 0; i < 8; i++)
        CHECK(s.irr[i] == 0);
    amdhv::apic::setIrr(s, 255);
    CHECK(s.irr[7] == (1u << 31));
    CHECK(amdhv::apic::takeInterrupt(s) == 255);
    amdhv::apic::setEoi(s, 255);
    CHECK(s.isr[7] == 0);
}

void testIoApic()
{
    amdhv::apic::IoApicState s;
    amdhv::apic::reset(s);
    CHECK(amdhv::apic::ioRead(s, 0x01) == 0x00170011u);   /* 24 redirection entries */
    CHECK((amdhv::apic::ioRead(s, 0x10) & 0x10000u) != 0); /* masked by default */
    amdhv::apic::ioWrite(s, 0x10, 0x21);                  /* vector 0x21, unmasked */
    CHECK(amdhv::apic::ioRead(s, 0x10) == 0x21);
    amdhv::apic::assertIrq(s, 0);
    CHECK(amdhv::apic::pendingVector(s) == 0x21);
    amdhv::apic::deassertIrq(s, 0);
    CHECK(amdhv::apic::pendingVector(s) == 0);
    /* A high half write keeps the low half, so masking one entry does not disturb its vector. */
    amdhv::apic::ioWrite(s, 0x11, 1);
    CHECK(amdhv::apic::ioRead(s, 0x10) == 0x21);
    CHECK((amdhv::apic::ioRead(s, 0x11) & 1) == 1);
    amdhv::apic::ioWrite(s, 0x10, 0x10021);              /* mask it again */
    amdhv::apic::assertIrq(s, 0);
    CHECK(amdhv::apic::pendingVector(s) == 0);

    /* ID register, reserved/version registers and unknown selectors. */
    amdhv::apic::ioWrite(s, 0x00, 0x05000000u);
    CHECK(amdhv::apic::ioRead(s, 0x00) == 0x05000000u);
    CHECK(amdhv::apic::ioRead(s, 0x02) == 0);
    CHECK(amdhv::apic::ioRead(s, 0x60) == 0);
    amdhv::apic::ioWrite(s, 0x60, 0);                     /* unknown selector: no-op */
    CHECK(amdhv::apic::ioRead(s, 0x60) == 0);

    /* Out-of-range vectors and empty state must not corrupt the register model. */
    amdhv::apic::LapicState q;
    amdhv::apic::reset(q);
    CHECK(amdhv::apic::highestIsrVector(q) == 0);
    CHECK(amdhv::apic::highestBit32(0) == 0);   /* no bit set: the scan falls through */
    amdhv::apic::setEoi(q, 15);
    amdhv::apic::setEoi(q, 256);
    CHECK(amdhv::apic::highestIsrVector(q) == 0);
}

void testAtpic()
{
    amdhv::apic::AtpicState s;
    amdhv::apic::reset(s);
    CHECK(s.mask == 0xFF);
    s.mask &= ~(1u << 4);
    s.request = 1u << 4;
    CHECK((s.request & ~s.mask) != 0);                    /* IRQ4 can be serviced */
    CHECK(amdhv::apic::atpicPending(s) == 0x34);          /* default ICW2 base 0x30 + IRQ4 */
    s.mask |= 1u << 4;
    CHECK(amdhv::apic::atpicPending(s) == 0);
    s.mask &= ~(1u << 4);
    s.service = 1u << 4;
    s.request = 0;
    CHECK(s.service == (1u << 4));
}

void testLapicRegisters()
{
    amdhv::apic::LapicState s;
    amdhv::apic::reset(s);
    amdhv::apic::writeReg(s, 0x080, 0x50);
    CHECK(amdhv::apic::readReg(s, 0x080) == 0x50);
    CHECK(amdhv::apic::readReg(s, 0x0A0) == 0x50);
    amdhv::apic::writeReg(s, 0x300, 0x21);
    CHECK(amdhv::apic::readReg(s, 0x300) == 0x21);
    /* EOI clears the highest in-service vector, not the ESR register. */
    amdhv::apic::setIrr(s, 0x61);
    CHECK(amdhv::apic::takeInterrupt(s) == 0x61);
    CHECK(amdhv::apic::highestIsrVector(s) == 0x61);
    amdhv::apic::writeReg(s, 0x0B0, 0);
    CHECK(amdhv::apic::highestIsrVector(s) == 0);
    amdhv::apic::writeReg(s, 0x350, 0xE0);
    CHECK(amdhv::apic::readReg(s, 0x350) == 0xE0);
    CHECK(amdhv::apic::readReg(s, 0x777) == 0);
    uint8_t ext[8 + sizeof(amdhv::apic::LapicState)] = {0};
    amdhv::apic::encodeState(s, ext);
    uint32_t ver = 0;
    memcpy(&ver, ext, 4);
    CHECK(ver == 100);
    amdhv::apic::LapicState copy;
    amdhv::apic::reset(copy);
    CHECK(amdhv::apic::decodeState(copy, ext));
    CHECK(amdhv::apic::readReg(copy, 0x080) == 0x50);
    ext[0] = 0;                                   /* wrong version must be refused */
    CHECK(!amdhv::apic::decodeState(copy, ext));
}

void testLayouts()
{
    CHECK(sizeof(amdhv::apic::LapicState) == 240);
    CHECK(sizeof(amdhv::apic::IoApicState) == 272);
    CHECK(sizeof(amdhv::apic::AtpicState) == 16);
}

/* Wire formats captured from Apple's framework trap log (task trap 10).
 * hv_vm_lapic_set_intr(0, 0x41, edge)    -> a=0x7f4100000000, b low byte 0
 * hv_vm_lapic_set_intr(1, 0x44, level)   -> a=0x7f4400000001, b low byte 2
 * hv_vm_send_ioapic_intr(0x30)           -> a=0x30, b is an opaque cookie
 * IOAPIC redirection entry: vector 7:0, mask bit 16, destination 63:56. */
void testApicWireDecode()
{
    amdhv::apic::LapicIntrWire lapic = amdhv::apic::decodeLapicIntr(0x7f4100000000ull, 0x91f483e00000002ull);
    CHECK(lapic.vcpu == 0);
    CHECK(lapic.vector == 0x41);
    CHECK(lapic.trigger == 2);

    lapic = amdhv::apic::decodeLapicIntr(0x7f4400000001ull, 0x3f6a511900000000ull);
    CHECK(lapic.vcpu == 1);
    CHECK(lapic.vector == 0x44);
    CHECK(lapic.trigger == 0);

    amdhv::apic::IoapicIntrWire io = amdhv::apic::decodeIoapicIntr(0x30ull);
    CHECK(io.vector == 0x30);
    CHECK(io.destination == 0);
    CHECK(!io.masked);
    CHECK(io.valid);

    io = amdhv::apic::decodeIoapicIntr(0x02000000000000f0ull);  /* dest 2, vector 0xf0 */
    CHECK(io.vector == 0xf0);
    CHECK(io.destination == 2);
    CHECK(!io.masked);

    io = amdhv::apic::decodeIoapicIntr(0x0000000000010021ull);  /* masked redirection entry */
    CHECK(io.vector == 0x21);
    CHECK(io.masked);
    CHECK(!io.valid);

    io = amdhv::apic::decodeIoapicIntr(0xff00000000000060ull);  /* broadcast destination */
    CHECK(io.destination == 0xff);
    CHECK(io.valid);

    io = amdhv::apic::decodeIoapicIntr(0x0full);                 /* vector below the APIC's valid range */
    CHECK(io.vector == 0x0f);
    CHECK(!io.valid);
}

/* hv_vm_lapic_msi hands the trap (data << 32) | addr. Captured wire values:
 * addr 0xfee00000, data 0x41 -> arg 0x41fee00000 (physical destination 0)
 * addr 0xfee01000, data 0x50 -> arg 0x50fee01000 (physical destination 1)
 * MSI address: 31:20 0xfee, 19:12 destination, bit 3 redirection hint, bit 2 logical mode.
 * MSI data: 7:0 vector. */
void testApicMsiDecode()
{
    amdhv::apic::LapicMsiWire m = amdhv::apic::decodeLapicMsi(0xFEE00000ull, 0x41ull);
    CHECK(m.valid);
    CHECK(m.vector == 0x41);
    CHECK(m.destination == 0);
    CHECK(!m.logical);
    CHECK(!m.redirect);

    m = amdhv::apic::decodeLapicMsi(0xFEE01000ull, 0x50ull);
    CHECK(m.valid);
    CHECK(m.vector == 0x50);
    CHECK(m.destination == 1);

    m = amdhv::apic::decodeLapicMsi(0xFEE00008ull, 0x60ull);     /* redirection hint (bit 3) */
    CHECK(m.valid);
    CHECK(m.redirect);
    CHECK(!m.logical);

    m = amdhv::apic::decodeLapicMsi(0xFEE00004ull, 0x61ull);     /* logical destination mode (bit 2) */
    CHECK(m.valid);
    CHECK(m.logical);

    m = amdhv::apic::decodeLapicMsi(0xFEC00000ull, 0x62ull);     /* IOAPIC address, not an MSI */
    CHECK(!m.valid);

    m = amdhv::apic::decodeLapicMsi(0xFEE00000ull, 0x0full);     /* vector below the APIC's valid range */
    CHECK(!m.valid);
}

} // namespace

int main()
{
    testLapicPriority();
    testLapicBounds();
    testIoApic();
    testAtpic();
    testLapicRegisters();
    testLayouts();
    testApicWireDecode();
    testApicMsiDecode();
    if (fails) {
        printf("apic_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("apic_test: OK\n");
    return 0;
}
