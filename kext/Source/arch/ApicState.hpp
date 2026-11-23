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
#include <string.h>

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

/**
 * @brief Reset the object to a clean state.
 *
 * @param s State object to operate on.
 */
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

/**
 * @brief Reset the object to a clean state.
 *
 * @param s State object to operate on.
 */
inline void reset(AtpicState &s)
{
    s.ready = s.icw_num = s.rd_cmd_reg = 0;
    s.aeoi = s.poll = s.rotate = s.sfn = 0;
    s.irq_base = 0x30;
    s.request = s.service = s.smm = s.last_request = s.lowprio = 0;
    s.mask = 0xFF;                       /* every IRQ masked until the guestInitialises it */
    s.intr_raised = s.elc = 0;
}

/**
 * @brief Reset the object to a clean state.
 *
 * @param s State object to operate on.
 */
inline void reset(IoApicState &s)
{
    for (unsigned i = 0; i < 32; i++)
        s.rtbl[i] = 0x00010000ull;       /* masked, vector 0 */
    s.irr = s.ioa_id = s.ioregsel = 0;
}

/**
 * @brief Return the task-priority class of the current TPR.
 *
 * @param s State object to operate on.
 *
 * @return The unsigned result.
 */
inline unsigned tprClass(const LapicState &s) { return (s.tpr >> 4) & 0xF; }

/** Highest ISR class currently in service; the PPR is the larger of it and the TPR class. */
inline unsigned highestBit32(uint32_t v)
{
    for (int bit = 31; bit >= 0; bit--)
        if (v & (1u << bit))
            return (unsigned)bit;
    return 0;
}

/**
 * @brief Return the highest-priority vector currently in service.
 *
 * @param s State object to operate on.
 *
 * @return The unsigned result.
 */
inline unsigned highestIsrVector(const LapicState &s)
{
    for (int i = 7; i >= 0; i--)
        if (s.isr[i])
            return (unsigned)i * 32 + highestBit32(s.isr[i]);
    return 0;                            /* no vector in service */
}

/**
 * @brief Return the processor-priority class of the LAPIC.
 *
 * @param s State object to operate on.
 *
 * @return The unsigned result.
 */
inline unsigned pprClass(const LapicState &s)
{
    const unsigned inService = highestIsrVector(s) / 16;
    return tprClass(s) > inService ? tprClass(s) : inService;
}

/**
 * @brief Mark an interrupt request pending in the IRR.
 *
 * @param s State object to operate on.
 * @param vector Interrupt vector.
 */
inline void setIrr(LapicState &s, unsigned vector)
{
    if (vector < 16 || vector > 255)
        return;
    s.irr[vector / 32] |= 1u << (vector & 31);
}

/**
 * @brief Clear an interrupt from the ISR after an EOI.
 *
 * @param s State object to operate on.
 * @param vector Interrupt vector.
 */
inline void setEoi(LapicState &s, unsigned vector)
{
    if (vector < 16 || vector > 255)
        return;
    s.isr[vector / 32] &= ~(1u << (vector & 31));
}

/** Moves the highest-priority IRR entry above the PPR into ISR: the interrupt the core may inject. */
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

/** MMIO view of the LAPIC register file used by hv_vcpu_apic_read/write. Offsets the state struct
 * does not model are accepted but read as zero; the fields a guest actually uses are mapped. */
inline uint32_t readReg(const LapicState &s, uint32_t off)
{
    if (off == 0x080) return s.tpr;
    if (off == 0x090) return s.apr;
    if (off == 0x0A0) return pprClass(s) << 4;
    if (off == 0x0B0) return s.esr;
    if (off == 0x0F0) return s.svr;
    if (off >= 0x100 && off < 0x180) return s.isr[(off - 0x100) >> 4];
    if (off >= 0x180 && off < 0x200) return s.tmr[(off - 0x180) >> 4];
    if (off >= 0x200 && off < 0x280) return s.irr[(off - 0x200) >> 4];
    if (off == 0x300) return s.icr[0];
    if (off == 0x310) return s.icr[1];
    if (off >= 0x320 && off <= 0x370 && ((off - 0x320) & 0xF) == 0) return s.lvt[(off - 0x320) >> 4];
    return 0;
}

/**
 * @brief Write Reg.
 *
 * @param s State object to operate on.
 * @param off Byte offset.
 * @param value Value to apply.
 */
inline void writeReg(LapicState &s, uint32_t off, uint32_t value)
{
    if (off == 0x080) s.tpr = value;
    else if (off == 0x090) s.apr = value;
    else if (off == 0x0B0) setEoi(s, highestIsrVector(s));   /* EOI: clear the highest ISR bit */
    else if (off == 0x0F0) s.svr = value;
    else if (off >= 0x100 && off < 0x180) s.isr[(off - 0x100) >> 4] = value;
    else if (off >= 0x180 && off < 0x200) s.tmr[(off - 0x180) >> 4] = value;
    else if (off >= 0x200 && off < 0x280) s.irr[(off - 0x200) >> 4] = value;
    else if (off == 0x300) s.icr[0] = value;
    else if (off == 0x310) s.icr[1] = value;
    else if (off >= 0x320 && off <= 0x370 && ((off - 0x320) & 0xF) == 0) s.lvt[(off - 0x320) >> 4] = value;
}

/** Encode/decode into a KERNEL buffer. The caller then uses copyout/copyin for the user pointer:
 * memcpy straight to userspace faults under SMAP. */
inline void encodeState(const LapicState &s, void *kernelExt)
{
    const uint32_t ver = 100;                 /* HV_APIC_STATE_EXT_VER */
    memcpy(kernelExt, &ver, 4);               /* +4 is padding in the SDK struct */
    memcpy((uint8_t *)kernelExt + 8, &s, sizeof(s));
}

/**
 * @brief Decode the kernel-provided APIC state blob.
 *
 * @param s State object to operate on.
 * @param kernelExt Kernel-provided extended state blob.
 *
 * @return The bool result.
 */
inline bool decodeState(LapicState &s, const void *kernelExt)
{
    uint32_t ver = 0;
    memcpy(&ver, kernelExt, 4);
    if (ver != 100)                           /* HV_APIC_STATE_EXT_VER */
        return false;
    memcpy(&s, (const uint8_t *)kernelExt + 8, sizeof(s));
    return true;
}

/** Highest unmasked 8259A request, translated to an interrupt vector via ICW2's base. */
inline unsigned atpicPending(const AtpicState &s)
{
    const uint8_t pending = s.request & ~s.mask & ~s.service;
    if (!pending)
        return 0;
    return (unsigned)s.irq_base + highestBit32(pending);
}

/** I/O APIC redirect-table access through the IOREGSEL/IOWIN pair. */
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

/**
 * @brief Write a register of the I/O APIC redirect table.
 *
 * @param s State object to operate on.
 * @param sel I/O APIC register selector.
 * @param value Value to apply.
 */
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

/**
 * @brief Assert an I/O APIC input line.
 *
 * @param s State object to operate on.
 * @param intin I/O APIC input line.
 */
inline void assertIrq(IoApicState &s, unsigned intin)
{
    if (intin < 32)
        s.irr |= 1u << intin;
}

/**
 * @brief Deassert an I/O APIC input line.
 *
 * @param s State object to operate on.
 * @param intin I/O APIC input line.
 */
inline void deassertIrq(IoApicState &s, unsigned intin)
{
    if (intin < 32)
        s.irr &= ~(1u << intin);
}

/** Highest unmasked redirected interrupt, or 0. Bit 16 of a low entry is the mask bit. */
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
