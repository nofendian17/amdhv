/*
 * MsrMap.hpp - guest MSRs: which ones live in the VMCB, and where their bits are in the SVM MSR permission map.
 *
 * Hypervisor.framework lets a client make some MSRs "native" (hv_vcpu_enable_native_msr): the guest then reads and
 * writes them without a VM exit, and the framework keeps the value in the vCPU. On SVM the value of these MSRs is
 * in the VMCB save area (VMSAVE/VMLOAD switch them), and "native" means the MSRPM bits for that MSR are clear.
 *
 * Pure C++, tested on the host (tests/msr_map_test.cpp).
 */
#ifndef AMDHV_MSRMAP_HPP
#define AMDHV_MSRMAP_HPP

#include <stddef.h>
#include <stdint.h>

#include "VMCB.hpp"

namespace amdhv {
namespace msr {

enum : uint32_t {
    MSR_TSC            = 0x10,
    MSR_SYSENTER_CS    = 0x174,
    MSR_SYSENTER_ESP   = 0x175,
    MSR_SYSENTER_EIP   = 0x176,
    MSR_PAT            = 0x277,
    MSR_EFER           = 0xC0000080,
    MSR_STAR           = 0xC0000081,
    MSR_LSTAR          = 0xC0000082,
    MSR_CSTAR          = 0xC0000083,
    MSR_SFMASK         = 0xC0000084,
    MSR_FS_BASE        = 0xC0000100,
    MSR_GS_BASE        = 0xC0000101,
    MSR_KERNEL_GS_BASE = 0xC0000102,
    MSR_TSC_AUX        = 0xC0000103
};

enum Kind : uint8_t {
    K_UNKNOWN = 0,   /* not a guest MSR we keep: always intercepted, the client emulates it */
    K_VMCB,          /* a plain 64-bit slot in the VMCB */
    K_TSC,           /* guest TSC = host TSC + VMCB tsc offset */
    K_EFER,          /* the guest EFER, with SVME hidden */
    K_TSC_AUX        /* RDTSCP's value: not in the VMCB, the kext swaps the real MSR around VMRUN */
};

struct Info {
    Kind     kind;
    uint16_t vmcbOff;    /* K_VMCB: byte offset in the 4 KiB VMCB */
    uint8_t  vmcbSize;   /* K_VMCB: bytes */
    uint8_t  valueBits;  /* K_VMCB: how many low bits the MSR has (the SYSENTER_CS MSR is 32 bits) */
};

#define AMDHV_MSR_SAVE(member) ((uint16_t)(0x400 + offsetof(amdhv::vmcb::Save, member)))

/**
 * @brief Return the VMCB location and width of an MSR.
 *
 * @param msr MSR index.
 *
 * @return The Info result.
 */
static inline Info info(uint32_t msr)
{
    using amdhv::vmcb::Save;
    switch (msr) {
    case MSR_TSC:            return { K_TSC, 0, 8, 64 };
    case MSR_EFER:           return { K_EFER, AMDHV_MSR_SAVE(efer), 8, 64 };
    case MSR_TSC_AUX:        return { K_TSC_AUX, 0, 8, 32 };
    case MSR_PAT:            return { K_VMCB, AMDHV_MSR_SAVE(gPat), 8, 64 };
    case MSR_SYSENTER_CS:    return { K_VMCB, AMDHV_MSR_SAVE(sysenterCs), 8, 32 };
    case MSR_SYSENTER_ESP:   return { K_VMCB, AMDHV_MSR_SAVE(sysenterEsp), 8, 64 };
    case MSR_SYSENTER_EIP:   return { K_VMCB, AMDHV_MSR_SAVE(sysenterEip), 8, 64 };
    case MSR_STAR:           return { K_VMCB, AMDHV_MSR_SAVE(star), 8, 64 };
    case MSR_LSTAR:          return { K_VMCB, AMDHV_MSR_SAVE(lstar), 8, 64 };
    case MSR_CSTAR:          return { K_VMCB, AMDHV_MSR_SAVE(cstar), 8, 64 };
    case MSR_SFMASK:         return { K_VMCB, AMDHV_MSR_SAVE(sfmask), 8, 64 };
    case MSR_KERNEL_GS_BASE: return { K_VMCB, AMDHV_MSR_SAVE(kernelGsBase), 8, 64 };
    case MSR_FS_BASE:        return { K_VMCB, (uint16_t)(AMDHV_MSR_SAVE(fs) + offsetof(amdhv::vmcb::Seg, base)), 8, 64 };
    case MSR_GS_BASE:        return { K_VMCB, (uint16_t)(AMDHV_MSR_SAVE(gs) + offsetof(amdhv::vmcb::Seg, base)), 8, 64 };
    default:                 return { K_UNKNOWN, 0, 0, 0 };
    }
}

/**
 * @brief Check whether an MSR is an Intel-only speculation MSR.
 *
 * @param msr MSR index.
 *
 * @return The bool result.
 */
static inline bool intelOnlySpecMsr(uint32_t msr)
{
    return msr == 0x48 || msr == 0x49;
}

/**
 * @brief Check whether an MSR must stay intercepted for host safety.
 *
 * @param msr MSR index.
 *
 * @return The bool result.
 */
static inline bool mustIntercept(uint32_t msr)
{
    Info i = info(msr);
    return i.kind == K_EFER || i.kind == K_TSC || intelOnlySpecMsr(msr);
}

/* Is `msr` an address a guest could even write to the 8 KiB map? */
constexpr size_t kMsrpmSize = 8192;

/**
 * @brief Map an MSR index to a bitmap byte and bit.
 *
 * @param msr MSR index.
 * @param byteOffset Byte offset.
 * @param bit Bit index.
 *
 * @return The bool result.
 */
static inline bool msrpmBit(uint32_t msr, uint32_t *byteOffset, unsigned *bit)
{
    uint32_t base;
    uint32_t region;
    if (msr <= 0x1FFF)                              { base = 0;          region = 0x0000; }
    else if (msr >= 0xC0000000u && msr <= 0xC0001FFFu) { base = 0xC0000000u; region = 0x0800; }
    else if (msr >= 0xC0010000u && msr <= 0xC0011FFFu) { base = 0xC0010000u; region = 0x1000; }
    else
        return false;
    uint32_t idx = (msr - base) * 2;                // bit index inside the region
    *byteOffset = region + idx / 8;
    *bit        = idx % 8;
    return true;
}

/** Make reads and writes of `msr` native (enable) or intercepted again. The map is all-ones when nothing is native. */
static inline bool setNative(uint8_t *msrpm, uint32_t msr, bool enable)
{
    uint32_t off;
    unsigned bit;
    if (!msrpmBit(msr, &off, &bit) || off >= kMsrpmSize)
        return false;
    uint8_t mask = (uint8_t)(3u << bit);            // read bit and write bit: always an even bit, so both in one byte
    if (enable)
        msrpm[off] &= (uint8_t)~mask;
    else
        msrpm[off] |= mask;
    return true;
}

/**
 * @brief Check whether an MSR is marked native in the MSRPM.
 *
 * @param msrpm MSR permission bitmap.
 * @param msr MSR index.
 *
 * @return The bool result.
 */
static inline bool isNative(const uint8_t *msrpm, uint32_t msr)
{
    uint32_t off;
    unsigned bit;
    if (!msrpmBit(msr, &off, &bit) || off >= kMsrpmSize)
        return false;
    return ((msrpm[off] >> bit) & 3) == 0;
}

} // namespace msr
} // namespace amdhv

#endif /* AMDHV_MSRMAP_HPP */
