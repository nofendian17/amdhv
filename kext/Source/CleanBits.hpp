/*
 * CleanBits.hpp - VMCB clean bits (AMD APM Vol. 2, Appendix B).
 *
 * A set bit tells VMRUN that the corresponding group of VMCB fields has not changed since the last
 * guest entry, so the processor may keep using the state it already has instead of reloading it from
 * memory. A bit that is wrong makes the guest run with stale state, so the offset -> group mapping is
 * explicit here and checked by tests/clean_bits_test.cpp.
 *
 * Two groups are always dirty: INT_CTL/INT_VECTOR (the processor may change them without updating the
 * VMCB) and CR2 (written on #PF). The bits are per logical CPU: a vCPU that moves to another CPU must
 * mark everything dirty, because that CPU has never loaded this VMCB.
 */
#ifndef AMDHV_CLEAN_BITS_HPP
#define AMDHV_CLEAN_BITS_HPP

#include <stdint.h>

namespace amdhv {
namespace vmcb {

enum CleanBit : unsigned {
    CLEAN_INTERCEPTS = 0,   /* CR/DR/exception/general intercepts, pause filter, TSC offset */
    CLEAN_IOPM       = 1,   /* IOPM_BASE_PA, MSRPM_BASE_PA */
    CLEAN_ASID       = 2,   /* ASID */
    CLEAN_TPR        = 3,   /* INT_CTL, INT_VECTOR: always dirty */
    CLEAN_NP         = 4,   /* NPT_EN, N_CR3, gPAT */
    CLEAN_CR         = 5,   /* CR0, CR3, CR4, EFER */
    CLEAN_DR         = 6,   /* DR6, DR7 */
    CLEAN_DT         = 7,   /* GDTR, IDTR */
    CLEAN_SEG        = 8,   /* CS, DS, ES, SS, CPL */
    CLEAN_CR2        = 9,   /* CR2: always dirty */
    CLEAN_LBR        = 10,  /* DEBUGCTL, branch records */
    CLEAN_AVIC       = 11,  /* AVIC state */
    CLEAN_NONE       = 32   /* not covered by a clean bit: always loaded */
};

static const uint32_t kAllCleanMask    = 0x0FFFu;   /* bits 0..11 */
static const uint32_t kAlwaysDirtyMask = (1u << CLEAN_TPR) | (1u << CLEAN_CR2);
static const uint32_t kCleanableMask   = kAllCleanMask & ~kAlwaysDirtyMask;

/*
 * The clean-bit group of a byte offset in the whole 4 KiB VMCB (control area 0x000..0x3FF, state save
 * area 0x400..0xFFF). CLEAN_NONE for fields the processor always loads (RIP, RAX, RSP, RFLAGS, FS/GS,
 * LDTR/TR, event injection, ...).
 */
static inline CleanBit cleanBitForOffset(unsigned off)
{
    /* Control area. */
    if (off <= 0x017) return CLEAN_INTERCEPTS;                  /* intercept vectors */
    if (off == 0x03C || off == 0x03E) return CLEAN_INTERCEPTS;  /* pause filter */
    if (off >= 0x040 && off <= 0x04F) return CLEAN_IOPM;        /* IOPM/MSRPM base */
    if (off >= 0x050 && off <= 0x057) return CLEAN_INTERCEPTS;  /* TSC offset */
    if (off >= 0x058 && off <= 0x05B) return CLEAN_ASID;
    if (off >= 0x060 && off <= 0x067) return CLEAN_TPR;
    if (off >= 0x090 && off <= 0x097) return CLEAN_NP;          /* NPT_EN */
    if (off >= 0x098 && off <= 0x09F) return CLEAN_AVIC;
    if (off >= 0x0B0 && off <= 0x0B7) return CLEAN_NP;          /* N_CR3 */

    /* State save area (VMCB offset = 0x400 + Save offset). */
    if (off >= 0x400 && off <= 0x43F) return CLEAN_SEG;         /* ES, CS, SS, DS */
    if (off >= 0x460 && off <= 0x46F) return CLEAN_DT;          /* GDTR */
    if (off >= 0x480 && off <= 0x48F) return CLEAN_DT;          /* IDTR */
    if (off == 0x4CB) return CLEAN_SEG;                         /* CPL */
    if (off == 0x4D0) return CLEAN_CR;                          /* EFER */
    if (off == 0x548 || off == 0x550 || off == 0x558) return CLEAN_CR;  /* CR4, CR3, CR0 */
    if (off == 0x560 || off == 0x568) return CLEAN_DR;          /* DR7, DR6 */
    if (off == 0x640) return CLEAN_CR2;
    if (off == 0x668) return CLEAN_NP;                          /* gPAT */
    if (off == 0x670) return CLEAN_LBR;                         /* DEBUGCTL */
    if (off >= 0x678 && off <= 0x697) return CLEAN_LBR;         /* branch records */
    return CLEAN_NONE;
}

} // namespace vmcb
} // namespace amdhv

#endif /* AMDHV_CLEAN_BITS_HPP */
