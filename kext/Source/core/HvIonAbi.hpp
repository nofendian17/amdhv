/*
 * HvIonAbi.hpp - kext-side view of xnu's kern/hv_io_notifier.h.
 *
 * The SDK header only exports the kHV_ION_* flag constants; the struct and
 * function declarations live behind KERNEL_PRIVATE, which the public SDK does
 * not ship. Declared here, as HvKextAbi.hpp does for hv_support_kext.h, so the
 * build does not depend on a private SDK header.
 * Source: https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/hv_io_notifier.h
 */
#ifndef AMDHV_HV_ION_ABI_HPP
#define AMDHV_HV_ION_ABI_HPP

#include <mach/kern_return.h>
#include <stddef.h>
#include <stdint.h>

extern "C" {
typedef struct hv_ion_grp hv_ion_grp_t;

typedef struct {
    uint64_t addr;
    uint64_t size;
    uint64_t value;
    uint32_t port_name;
    uint32_t flags;
} hv_ion_t;

kern_return_t hv_io_notifier_grp_alloc(hv_ion_grp_t **grp);
void          hv_io_notifier_grp_free(hv_ion_grp_t **grp);
kern_return_t hv_io_notifier_grp_add(hv_ion_grp_t *grp, const hv_ion_t *notifier);
kern_return_t hv_io_notifier_grp_remove(hv_ion_grp_t *grp, const hv_ion_t *notifier);
kern_return_t hv_io_notifier_grp_fire(hv_ion_grp_t *grp, uint64_t addr, size_t size, uint64_t value);
}

#endif /* AMDHV_HV_ION_ABI_HPP */
