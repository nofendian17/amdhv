/*
 * VmcsCaps.hpp - the VMX capability words AMDHV advertises, derived from the controls it implements.
 *
 * A Hypervisor.framework client builds its execution controls from hv_vmx_read_capability():
 *
 *     control = (wanted | (cap & 0xffffffff)) & (cap >> 32);
 *
 * The low 32 bits are the settings that must be 1, the high 32 bits the settings that may be 1
 * (Intel SDM Vol. 3, 25.6.1; Apple's allowed_0/allowed_1 masks have the same meaning). Advertising a
 * control that is not implemented makes clients set it and then silently lose the feature, so this
 * table only lists controls the kext actually honours.
 *
 * Each control field keeps three sets:
 *   mustOne   - bits that are always on (advertised as allowed_0)
 *   mayOne    - bits a client may turn on (advertised as allowed_1)
 *   ignored   - bits accepted for compatibility but not advertised. They are controls that the
 *               previous, more permissive table advertised (VPID, I/O and MSR bitmaps) or bits a
 *               client may still set unconditionally. Ignoring them never promises a feature: the
 *               kext keeps doing what it already did.
 *
 * Anything outside mayOne|ignored is refused by VmcsState::write() and by the client-side field
 * helper, so a control the kext cannot honour is not silently shadowed.
 *
 * Pure C++ (stdint only), tested on the host by tests/vmcs_state_test.cpp.
 */
#ifndef AMDHV_VMCS_CAPS_HPP
#define AMDHV_VMCS_CAPS_HPP

#include <stdint.h>

namespace amdhv {
namespace vmx {

/* HV_VMX_CAP_* (hv_vmx.h). The first 17 are the words the framework exposes in its info page. */
enum CapIndex : unsigned {
    CAP_PINBASED = 0,
    CAP_PROCBASED = 1,
    CAP_PROCBASED2 = 2,
    CAP_ENTRY = 3,
    CAP_EXIT = 4,
    CAP_BASIC = 5,
    CAP_TRUE_PINBASED = 6,
    CAP_TRUE_PROCBASED = 7,
    CAP_TRUE_ENTRY = 8,
    CAP_TRUE_EXIT = 9,
    CAP_MISC = 10,
    CAP_CR0_FIXED0 = 11,
    CAP_CR0_FIXED1 = 12,
    CAP_CR4_FIXED0 = 13,
    CAP_CR4_FIXED1 = 14,
    CAP_VMCS_ENUM = 15,
    CAP_EPT_VPID_CAP = 16,
    CAP_COUNT = 17
};

/* VMCS control fields. */
enum : uint32_t {
    C_PIN_BASED = 0x4000,
    C_CPU_BASED = 0x4002,
    C_VMEXIT_CONTROLS = 0x400c,
    C_VMENTRY_CONTROLS = 0x4012,
    C_CPU_BASED2 = 0x401e
};

/**
 * @brief Return a single-bit mask.
 *
 * @param n Bit index.
 *
 * @return The uint32_t result.
 */
constexpr uint32_t bit(unsigned n) { return 1u << n; }

struct ControlCaps {
    uint32_t mustOne;
    uint32_t mayOne;
    uint32_t ignored;
};

/**
 * @brief Return the pin-based execution control capabilities.
 *
 * @return The ControlCaps result.
 */
constexpr ControlCaps pinBasedCaps()
{
    const uint32_t must = bit(1) | bit(2) | bit(4);          /* reserved bits Intel requires to be 1 */
    const uint32_t may = must | bit(0) | bit(3);             /* external interrupts, NMI */
    return { must, may, bit(5) | bit(6) | bit(7) };
}

/**
 * @brief Return the primary execution control capabilities.
 *
 * @param tprShadow Whether the TPR shadow is offered.
 *
 * @return The ControlCaps result.
 */
constexpr ControlCaps procBasedCaps(bool tprShadow)
{
    const uint32_t must = bit(1) | bit(4) | bit(5) | bit(6) | bit(8) | bit(13) | bit(14) | bit(26);
    uint32_t may = must | bit(2) | bit(3) | bit(7) | bit(9) | bit(10) | bit(11) | bit(12) |
                   bit(15) | bit(16) | bit(19) | bit(20) | bit(22) | bit(23) | bit(24) | bit(29) | bit(30) | bit(31);
    if (tprShadow)
        may |= bit(21);
    uint32_t ignored = bit(25) | bit(27) | bit(28);
    if (!tprShadow)
        ignored |= bit(21);
    return { must, may, ignored };
}

/**
 * @brief Return the secondary execution control capabilities.
 *
 * @return The ControlCaps result.
 */
constexpr ControlCaps procBased2Caps()
{
    const uint32_t must = 0;
    const uint32_t may = bit(1) | bit(3) | bit(6) | bit(7) | bit(12);   /* EPT, RDTSCP, WBINVD, unrestricted, INVPCID */
    return { must, may, bit(5) };                                        /* VPID: accepted, not offered */
}

/** VM-entry/exit controls: kept compatible with the Intel-shaped words clients already expect. */
constexpr ControlCaps entryCaps() { return { 0x000011ffu, 0x0000d3ffu, 0 }; }
/**
 * @brief Return the VM-exit control capability bits.
 *
 * @return The ControlCaps result.
 */
constexpr ControlCaps exitCaps() { return { 0x00036dffu, 0x003fefffu, 0 }; }

/**
 * @brief Check whether a VMCS field is an execution control field.
 *
 * @param field VMCS field encoding.
 *
 * @return The bool result.
 */
constexpr bool isControlField(uint32_t field)
{
    return field == C_PIN_BASED || field == C_CPU_BASED || field == C_CPU_BASED2 ||
           field == C_VMENTRY_CONTROLS || field == C_VMEXIT_CONTROLS;
}

/**
 * @brief Return the must-one, may-one and ignored bits of a control field.
 *
 * @param field VMCS field encoding.
 * @param tprShadow Whether the TPR shadow is offered.
 *
 * @return The ControlCaps result.
 */
constexpr ControlCaps controlCaps(uint32_t field, bool tprShadow)
{
    switch (field) {
    case C_PIN_BASED:        return pinBasedCaps();
    case C_CPU_BASED:        return procBasedCaps(tprShadow);
    case C_CPU_BASED2:       return procBased2Caps();
    case C_VMENTRY_CONTROLS: return entryCaps();
    case C_VMEXIT_CONTROLS:  return exitCaps();
    default:                 return { 0, 0, 0 };
    }
}

/**
 * @brief Map a VMCS control field to its capability index.
 *
 * @param field VMCS field encoding.
 *
 * @return The int result.
 */
constexpr int capIndexForControl(uint32_t field)
{
    switch (field) {
    case C_PIN_BASED:        return CAP_PINBASED;
    case C_CPU_BASED:        return CAP_PROCBASED;
    case C_CPU_BASED2:       return CAP_PROCBASED2;
    case C_VMENTRY_CONTROLS: return CAP_ENTRY;
    case C_VMEXIT_CONTROLS:  return CAP_EXIT;
    default:                 return -1;
    }
}

/**
 * @brief Return a capability word by index.
 *
 * @param c Callee context.
 *
 * @return The uint64_t result.
 */
constexpr uint64_t capWord(ControlCaps c)
{
    return (uint64_t)c.mustOne | ((uint64_t)c.mayOne << 32);
}

/** The 64-bit word hv_vmx_read_capability(index) returns. */
constexpr uint64_t capability(unsigned index, bool tprShadow)
{
    switch (index) {
    case CAP_PINBASED:        return capWord(pinBasedCaps());
    case CAP_PROCBASED:       return capWord(procBasedCaps(tprShadow));
    case CAP_PROCBASED2:      return capWord(procBased2Caps());
    case CAP_ENTRY:           return capWord(entryCaps());
    case CAP_EXIT:            return capWord(exitCaps());
    case CAP_BASIC:           return 0x98100000000001ull;
    case CAP_TRUE_PINBASED:   return capWord(pinBasedCaps());
    case CAP_TRUE_PROCBASED:  return capWord(procBasedCaps(tprShadow));
    case CAP_TRUE_ENTRY:      return 0xd3fb000011fbull;
    case CAP_TRUE_EXIT:       return 0x3feffb00036dfbull;
    case CAP_MISC:            return 0x40040ull;
    case CAP_CR0_FIXED0:      return 0x20ull;
    case CAP_CR0_FIXED1:      return 0xffffffffull;
    case CAP_CR4_FIXED0:      return 0;
    case CAP_CR4_FIXED1:      return 0xffffffull;
    case CAP_VMCS_ENUM:       return 0x5cull;
    /* NPT is a 4-level walk over 4 KiB leaves with WB memory. No execute-only, 2 MiB/1 GiB leaves,
     * INVEPT, VPID or A/D bits. */
    case CAP_EPT_VPID_CAP:    return bit(6) | bit(14);
    default:                  return 0;
    }
}

/** True when every bit of `value` is either advertised or an accepted no-op. */
constexpr bool controlAllowed(uint32_t field, uint64_t value, bool tprShadow)
{
    if (!isControlField(field))
        return true;
    const ControlCaps c = controlCaps(field, tprShadow);
    return ((uint32_t)value & ~(c.mayOne | c.ignored)) == 0;
}

/** Bits of a control write that were accepted but have no effect (for one-time diagnostics). */
constexpr uint32_t ignoredControlBits(uint32_t field, uint64_t value, bool tprShadow)
{
    if (!isControlField(field))
        return 0;
    return (uint32_t)value & controlCaps(field, tprShadow).ignored;
}

} // namespace vmx
} // namespace amdhv

#endif /* AMDHV_VMCS_CAPS_HPP */
