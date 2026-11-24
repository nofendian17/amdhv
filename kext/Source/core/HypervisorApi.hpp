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

} // namespace hv
} // namespace amdhv

#endif /* AMDHV_HYPERVISOR_API_HPP */
