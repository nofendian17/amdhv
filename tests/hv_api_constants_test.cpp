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

#include "../include/AMDHVShared.h"
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

void testCpuLimits()
{
    /* The per-CPU SVM arrays and the kick IPI both use 64-bit CPU masks. Raising this without a
     * >64-capable IPI would silently drop hv_vcpu_interrupt on CPU 64+. */
    CHECK(AMDHV_MAX_CPUS == 64);
    CHECK(AMDHV_MAX_VCPUS_PER_VM == 64);
}

void testApicAccelFlagIsTheSdkBit()
{
    /* HV_VM_ACCEL_APIC is bit 10, not a VMX control bit or the TSC flag. */
    CHECK(amdhv::hv::kVmAccelApic == (UINT64_C(1) << 10));
    CHECK(amdhv::hv::kVmAccelApic == UINT64_C(0x400));
    CHECK((amdhv::hv::kVmAccelApic & UINT64_C(0x100)) == 0);
}

void testApicSubOpTagDecoding()
{
    /* The framework packs the state pointer's low byte into bits 8..15 of word 0. On macOS 15.8.3
     * get_state arrives as 0x9004 (pointer 0x...b90, tag 0x04); matching 16 bits lost the call. */
    const uint64_t state = UINT64_C(0x7ff7bd1dfb90);
    const uint64_t getWord = (state << amdhv::hv::kApicPointerShift) | 0x04;
    CHECK(amdhv::hv::apicTag(getWord) == 0x04);
    CHECK(amdhv::hv::apicPointer(getWord) == state);
    CHECK((getWord & UINT64_C(0xFFFF)) == UINT64_C(0x9004));
    CHECK(amdhv::hv::apicTag((state << amdhv::hv::kApicPointerShift) | 0x03) == 0x03);
    CHECK(amdhv::hv::apicTag(UINT64_C(0x8001)) == 0x01);
}

} // namespace

int main()
{
    testCpuLimits();
    testApicAccelFlagIsTheSdkBit();
    testApicSubOpTagDecoding();
    if (fails) {
        printf("hv_api_constants_test: %d failure(s)\n", fails);
        return 1;
    }
    printf("hv_api_constants_test: OK\n");
    return 0;
}
