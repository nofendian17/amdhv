/*
 * hv_api_constants_test.cpp - host-side checks for the Hypervisor.framework
 * constants AMDHV decodes in the kernel front end.
 *
 * Source: Apple Hypervisor.framework, hv_types.h (macOS 15 SDK):
 *   HV_VM_ACCEL_APIC = UINT64_C(1) << 10
 * Public reference: https://developer.apple.com/documentation/hypervisor/hv_vm_accel_apic
 */
#include <stdio.h>
#include <stdint.h>

#include "../kext/Source/core/HypervisorApi.hpp"

namespace {

int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

void testApicAccelFlagIsTheSdkBit()
{
    /* HV_VM_ACCEL_APIC is bit 10, not a VMX control bit or the TSC flag. */
    CHECK(amdhv::hv::kVmAccelApic == (UINT64_C(1) << 10));
    CHECK(amdhv::hv::kVmAccelApic == UINT64_C(0x400));
    CHECK((amdhv::hv::kVmAccelApic & UINT64_C(0x100)) == 0);
}

} // namespace

int main()
{
    testApicAccelFlagIsTheSdkBit();
    if (fails) {
        printf("hv_api_constants_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("hv_api_constants_test: OK\n");
    return 0;
}
