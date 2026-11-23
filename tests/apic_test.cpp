/*
 * apic_test.cpp - host-side tests for kext/Source/ApicState.hpp.
 *
 * The guest-facing wiring needs a kext, but the register rules do not.
 */
#include <stdio.h>
#include <stdint.h>

#include "../kext/Source/ApicState.hpp"

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

void testLayouts()
{
    CHECK(sizeof(amdhv::apic::LapicState) == 240);
    CHECK(sizeof(amdhv::apic::IoApicState) == 272);
    CHECK(sizeof(amdhv::apic::AtpicState) == 16);
}

} // namespace

int main()
{
    testLapicPriority();
    testLapicBounds();
    testIoApic();
    testAtpic();
    testLayouts();
    if (fails) {
        printf("apic_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("apic_test: OK\n");
    return 0;
}
