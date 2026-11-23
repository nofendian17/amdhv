/*
 * ApicMmio.hpp - decode the guest instruction behind an APIC-page NPF.
 *
 * A guest that talks to its local APIC directly uses a 32-bit absolute address around 0xFEE00000
 * (or the I/O APIC at 0xFEC00000). AMD's NPF gives the faulting GPA but not the instruction, so the
 * minimal decoder below handles the two MOV encodings such guests actually use:
 *
 *   8B 05 <disp32>   MOV EAX, [disp32]        (read)
 *   89 05 <disp32>   MOV [disp32], EAX        (write)
 *   8B 0D / 15 / 1D ... and the matching 89 forms for the other GPRs
 *
 * Anything else is reported as invalid and left to the userspace VMM. The decoder is pure so
 * tests/apic_mmio_test.cpp can exercise it on any host.
 */
#ifndef AMDHV_APIC_MMIO_HPP
#define AMDHV_APIC_MMIO_HPP

#include <stddef.h>
#include <stdint.h>

namespace amdhv {
namespace apic {

struct MmioOp {
    bool  valid;
    bool  isWrite;
    unsigned reg;          /* 0=RAX, 1=RCX, 2=RDX, 3=RBX, 4=RSP, 5=RBP, 6=RSI, 7=RDI */
    uint32_t disp;         /* absolute guest physical address of the access */
    unsigned length;       /* instruction bytes consumed */
};

inline MmioOp decodeMmio(const uint8_t *code, size_t n)
{
    MmioOp op = { false, false, 0, 0, 0 };
    if (n < 6)
        return op;
    size_t i = 0;
    if (code[i] == 0x66) {                    /* operand-size prefix: real-mode guests need it for EAX */
        if (n < 7)
            return op;
        i++;
    }
    if (n < i + 6)
        return op;
    const uint8_t opcode = code[i], modrm = code[i + 1];
    if (opcode != 0x8B && opcode != 0x89)
        return op;
    if ((modrm & 0xC7) != 0x05)            /* mod=00, rm=101: disp32 follows */
        return op;
    op.valid   = true;
    op.isWrite = opcode == 0x89;
    op.reg     = (modrm >> 3) & 7;
    op.disp    = (uint32_t)code[i + 2] | ((uint32_t)code[i + 3] << 8) |
                 ((uint32_t)code[i + 4] << 16) | ((uint32_t)code[i + 5] << 24);
    op.length  = (unsigned)i + 6;
    return op;
}

/* The register file offset of an APIC MMIO access within its 4 KiB page. */
inline uint32_t mmioOffset(uint64_t gpa, uint64_t base)
{
    return (gpa >= base && gpa < base + 0x1000) ? (uint32_t)(gpa - base) : 0xFFFFFFFFu;
}

} // namespace apic
} // namespace amdhv

#endif
