/*
 * ApicState.hpp - the software APIC registers an in-kernel provider has to keep.
 *
 * The layouts mirror Hypervisor.framework's hv_apic_state / hv_atpic_state / hv_ioapic_state so a
 * get/put trap can copy them verbatim. The register rules are host-testable here
 * (tests/apic_test.cpp); the VMCB/NPT side that makes guest accesses reach them lives in the kext.
 */
#ifndef AMDHV_APIC_STATE_HPP
#define AMDHV_APIC_STATE_HPP

#include <stddef.h>
#include <stdint.h>

namespace amdhv {
namespace apic {

struct LapicState {
    uint64_t apic_gpa;
    uint64_t apic_controls;
    uint64_t tsc_deadline;
    uint32_t apic_id;
    uint32_t ver;
    uint32_t tpr;
    uint32_t apr;
    uint32_t ldr;
    uint32_t dfr;
    uint32_t svr;
    uint32_t isr[8];
    uint32_t tmr[8];
    uint32_t irr[8];
    uint32_t esr;
    uint32_t lvt[7];
    uint32_t icr[2];
    uint32_t icr_timer;
    uint32_t dcr_timer;
    uint32_t ccr_timer;
    uint32_t esr_pending;
    uint32_t boot_state;
    uint32_t aeoi[8];
};

struct AtpicState {
    uint8_t ready, icw_num, rd_cmd_reg;
    uint8_t aeoi, poll, rotate, sfn;
    uint8_t irq_base, request, service, mask, smm, last_request, lowprio, intr_raised, elc;
};

struct IoApicState {
    uint64_t rtbl[32];
    uint32_t irr;
    uint32_t ioa_id;
    uint32_t ioregsel;
};

inline void reset(LapicState &s)
{
    for (unsigned i = 0; i < 8; i++)
        s.isr[i] = s.tmr[i] = s.irr[i] = s.aeoi[i] = 0;
    for (unsigned i = 0; i < 7; i++)
        s.lvt[i] = 0;
    s.icr[0] = s.icr[1] = 0;
    s.apic_gpa = s.apic_controls = s.tsc_deadline = 0;
    s.apic_id = s.ver = s.tpr = s.apr = s.ldr = s.dfr = s.svr = 0;
    s.esr = s.icr_timer = s.dcr_timer = s.ccr_timer = s.esr_pending = 0;
    s.boot_state = 0;
}

inline void reset(AtpicState &s)
{
    s.ready = s.icw_num = s.rd_cmd_reg = 0;
    s.aeoi = s.poll = s.rotate = s.sfn = 0;
    s.irq_base = s.request = s.service = s.smm = s.last_request = s.lowprio = 0;
    s.mask = 0xFF;                       /* every IRQ masked until the guestInitialises it */
    s.intr_raised = s.elc = 0;
}

inline void reset(IoApicState &s)
{
    for (unsigned i = 0; i < 32; i++)
        s.rtbl[i] = 0x00010000ull;       /* masked, vector 0 */
    s.irr = s.ioa_id = s.ioregsel = 0;
}

inline unsigned tprClass(const LapicState &s) { return (s.tpr >> 4) & 0xF; }

/* Highest ISR class currently in service; the PPR is the larger of it and the TPR class. */
inline unsigned highestBit32(uint32_t v)
{
    for (int bit = 31; bit >= 0; bit--)
        if (v & (1u << bit))
            return (unsigned)bit;
    return 0;
}

inline unsigned highestIsrVector(const LapicState &s)
{
    for (int i = 7; i >= 0; i--)
        if (s.isr[i])
            return (unsigned)i * 32 + highestBit32(s.isr[i]);
    return 0;                            /* no vector in service */
}

inline unsigned pprClass(const LapicState &s)
{
    const unsigned inService = highestIsrVector(s) / 16;
    return tprClass(s) > inService ? tprClass(s) : inService;
}

inline void setIrr(LapicState &s, unsigned vector)
{
    if (vector < 16 || vector > 255)
        return;
    s.irr[vector / 32] |= 1u << (vector & 31);
}

inline void setEoi(LapicState &s, unsigned vector)
{
    if (vector < 16 || vector > 255)
        return;
    s.isr[vector / 32] &= ~(1u << (vector & 31));
}

/* Moves the highest-priority IRR entry above the PPR into ISR: the interrupt the core may inject. */
inline unsigned takeInterrupt(LapicState &s)
{
    for (int i = 7; i >= 0; i--) {
        uint32_t v = s.irr[i];
        while (v) {
            const unsigned bit = highestBit32(v);
            const unsigned vector = (unsigned)i * 32 + bit;
            v &= ~(1u << bit);
            if (vector >= 16 && vector / 16 > pprClass(s)) {
                s.irr[i] &= ~(1u << bit);
                s.isr[i] |= 1u << bit;
                return vector;
            }
        }
    }
    return 0;
}

/* I/O APIC redirect-table access through the IOREGSEL/IOWIN pair. */
inline uint32_t ioRead(const IoApicState &s, uint32_t sel)
{
    if (sel == 0x00)
        return s.ioa_id << 24;
    if (sel == 0x01)
        return 0x00170011u;              /* 24 entries, APIC version 0x17 */
    if (sel == 0x02)
        return 0;
    if (sel >= 0x10 && sel < 0x50) {
        const uint64_t e = s.rtbl[(sel - 0x10) / 2];
        return (sel & 1) ? (uint32_t)(e >> 32) : (uint32_t)e;
    }
    return 0;
}

inline void ioWrite(IoApicState &s, uint32_t sel, uint32_t value)
{
    if (sel == 0x00)
        s.ioa_id = value >> 24;
    else if (sel >= 0x10 && sel < 0x50) {
        const unsigned i = (sel - 0x10) / 2;
        if (sel & 1)
            s.rtbl[i] = (s.rtbl[i] & 0xffffffffull) | ((uint64_t)value << 32);
        else
            s.rtbl[i] = (s.rtbl[i] & ~0xffffffffull) | value;
    }
}

inline void assertIrq(IoApicState &s, unsigned intin)
{
    if (intin < 32)
        s.irr |= 1u << intin;
}

inline void deassertIrq(IoApicState &s, unsigned intin)
{
    if (intin < 32)
        s.irr &= ~(1u << intin);
}

/* Highest unmasked redirected interrupt, or 0. Bit 16 of a low entry is the mask bit. */
inline unsigned pendingVector(const IoApicState &s)
{
    unsigned best = 0;
    for (unsigned i = 0; i < 32; i++) {
        if (!(s.irr & (1u << i)) || (s.rtbl[i] & 0x10000u))
            continue;
        const unsigned vector = (unsigned)(s.rtbl[i] & 0xFF);
        if (vector > best)
            best = vector;
    }
    return best;
}

} // namespace apic
} // namespace amdhv

#endif
