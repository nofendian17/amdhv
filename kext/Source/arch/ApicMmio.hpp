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

/**
 * @brief Decode an absolute MOV to or from the local APIC MMIO range.
 *
 * @param code Instruction bytes.
 * @param n Number of bytes.
 *
 * @return The MmioOp result.
 */
inline MmioOp decodeMmio(const uint8_t *code, size_t n)
{
    MmioOp op = { false, false, 0, 0, 0 };
    if (n < 6)
        return op;
    size_t i = 0;
    /* 0x66 selects a 32-bit operand, 0x67 a 32-bit address. A real-mode guest needs both for an
     * absolute MOV to an APIC page above 1 MiB; either prefix may come first. */
    for (unsigned seen = 0; seen < 2; seen++) {
        if (code[i] == 0x66 || code[i] == 0x67)
            i++;
        else
            break;
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

/**
 * @brief Decode an APIC MOV from the instruction bytes SVM DecodeAssists saved in the VMCB.
 *
 * Only `len` of the 15 bytes at +0xD1 are valid. Unlike an NPT lookup at save.rip (a guest LINEAR
 * address), these bytes work with guest paging enabled, so the in-kernel APIC path can serve the
 * access instead of handing every MMIO exit to the userspace VMM.
 *
 * @param bytes The 15-byte VMCB instruction buffer.
 * @param len The reported instruction length.
 * @param faultPa The faulting guest physical address from EXITINFO2.
 *
 * @return A valid MmioOp only when the captured instruction really targets `faultPa`.
 */
inline MmioOp decodeMmioAssist(const uint8_t bytes[15], uint8_t len, uint64_t faultPa)
{
    MmioOp op = { false, false, 0, 0, 0 };
    if (len == 0 || len > 15)
        return op;
    op = decodeMmio(bytes, len);
    if (op.valid && (op.length > len || op.disp != faultPa))
        op.valid = false;
    return op;
}

/** The register file offset of an APIC MMIO access within its 4 KiB page. */
inline uint32_t mmioOffset(uint64_t gpa, uint64_t base)
{
    return (gpa >= base && gpa < base + 0x1000) ? (uint32_t)(gpa - base) : 0xFFFFFFFFu;
}

} // namespace apic
} // namespace amdhv

#endif
