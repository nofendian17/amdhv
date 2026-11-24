/*
 * HvKextAbi.hpp - kext-side view of xnu's kern/hv_support_kext.h.
 *
 * The hypervisor provider ABI is identical in the two xnu revisions checked:
 *   xnu-11417.140.69 (macOS 15.8.3, the audit host)
 *     https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/hv_support_kext.h
 *   xnu-12377.121.6 (newer tag reviewed for forward compatibility)
 *     https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/kern/hv_support_kext.h
 *   hv_trap_t, the trap tables, hv_set_traps(), hv_set_callbacks(),
 *   hv_suspend()/hv_resume() and this eight-field callback struct are unchanged.
 *
 * xnu-12377.121.6 only changes osfmk/kern/hv_io_notifier.c to use IOT_ANY
 * instead of IKOT_UNKNOWN for typed Mach ports. AMDHV does not call the
 * notifier API, so that change does not affect the provider ABI. Keeping the
 * callback layout here (and testing it) prevents a silent field-order drift.
 */
#ifndef AMDHV_HV_KEXT_ABI_HPP
#define AMDHV_HV_KEXT_ABI_HPP

typedef struct hv_callbacks_s {
    void (*dispatch)(void *vcpu);
    void (*preempt)(void *vcpu);
    void (*suspend)(void);
    void (*thread_destroy)(void *vcpu);
    void (*task_destroy)(void *vm);
    void (*volatile_state)(void *vcpu, int state);
    /* HV_CALLBACKS_RESUME_DEFINED is set in both xnu revisions. */
    void (*resume)(void);
    void (*memory_pressure)(void);   /* unused by xnu, must still occupy slot 7 */
} hv_callbacks_t;

#endif /* AMDHV_HV_KEXT_ABI_HPP */
