/*
 * HypervisorApi.hpp - public Hypervisor.framework values the kext decodes.
 *
 * The kext cannot include the userspace SDK headers, so it repeats only the
 * values it must interpret. Each one is taken from an Apple header/document,
 * not inferred from an Intel-only behaviour.
 *
 * Source: Hypervisor.framework hv_types.h (macOS 15 SDK),
 *   HV_VM_ACCEL_APIC = UINT64_C(1) << 10
 * Public reference: https://developer.apple.com/documentation/hypervisor/hv_vm_accel_apic
 */
#ifndef AMDHV_HYPERVISOR_API_HPP
#define AMDHV_HYPERVISOR_API_HPP

#include <stdint.h>

namespace amdhv {
namespace hv {

/** hv_vm_create() option that asks for the in-kernel APIC variant of a VM. */
constexpr uint64_t kVmAccelApic = UINT64_C(1) << 10;    /* hv_types.h: HV_VM_ACCEL_APIC */

/*
 * Thread trap 13 (APIC sub-ops) argument encoding. For most sub-ops the framework puts the tag in
 * the low BYTE of word 0 and shifts the state pointer left by 8. Matching the low 16 bits instead
 * mixes the tag with the pointer's low byte: on macOS 15.8.3 get_state arrives as 0x9004 (pointer
 * low byte 0x90, tag 0x04), not 0x0004. apic_read/apic_write are the exception -- 16-bit tags with
 * the result pointer in word 1.
 */
constexpr unsigned kApicTagMask = 0xFFu;
constexpr unsigned kApicPointerShift = 8;

/** Sub-op tag of an APIC argument word; the pointer lives in the bits above it. */
inline unsigned apicTag(uint64_t word0) { return (unsigned)word0 & kApicTagMask; }

/** State pointer encoded in word 0 of a put/get-state argument. */
inline uint64_t apicPointer(uint64_t word0) { return word0 >> kApicPointerShift; }

} // namespace hv
} // namespace amdhv

#endif /* AMDHV_HYPERVISOR_API_HPP */
