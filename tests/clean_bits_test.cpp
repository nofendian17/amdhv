/* clean_bits_test.cpp - host tests for kext/Source/CleanBits.hpp and the VMCB clean-bit bookkeeping. */
#include <stdio.h>
#include <string.h>

#include "../kext/Source/VmcsState.hpp"

using namespace amdhv::vmcb;
using amdhv::vmx::VmcsState;

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main()
{
    /* Every group, by the offset of a field it covers. */
    CHECK(cleanBitForOffset(0x000) == CLEAN_INTERCEPTS);   /* intercept CR */
    CHECK(cleanBitForOffset(0x008) == CLEAN_INTERCEPTS);   /* exception bitmap */
    CHECK(cleanBitForOffset(0x03C) == CLEAN_INTERCEPTS);   /* pause filter threshold */
    CHECK(cleanBitForOffset(0x050) == CLEAN_INTERCEPTS);   /* TSC offset */
    CHECK(cleanBitForOffset(0x040) == CLEAN_IOPM);         /* IOPM base */
    CHECK(cleanBitForOffset(0x048) == CLEAN_IOPM);         /* MSRPM base */
    CHECK(cleanBitForOffset(0x058) == CLEAN_ASID);
    CHECK(cleanBitForOffset(0x060) == CLEAN_TPR);          /* INT_CTL */
    CHECK(cleanBitForOffset(0x064) == CLEAN_TPR);          /* INT_VECTOR */
    CHECK(cleanBitForOffset(0x090) == CLEAN_NP);           /* NPT_EN */
    CHECK(cleanBitForOffset(0x0B0) == CLEAN_NP);           /* N_CR3 */
    CHECK(cleanBitForOffset(0x098) == CLEAN_AVIC);

    /* State save area. */
    CHECK(cleanBitForOffset(0x400) == CLEAN_SEG);          /* ES */
    CHECK(cleanBitForOffset(0x41C) == CLEAN_SEG);          /* CS.base */
    CHECK(cleanBitForOffset(0x4CB) == CLEAN_SEG);          /* CPL */
    CHECK(cleanBitForOffset(0x460) == CLEAN_DT);           /* GDTR */
    CHECK(cleanBitForOffset(0x488) == CLEAN_DT);           /* IDTR.base */
    CHECK(cleanBitForOffset(0x4D0) == CLEAN_CR);           /* EFER */
    CHECK(cleanBitForOffset(0x548) == CLEAN_CR);           /* CR4 */
    CHECK(cleanBitForOffset(0x550) == CLEAN_CR);           /* CR3 */
    CHECK(cleanBitForOffset(0x558) == CLEAN_CR);           /* CR0 */
    CHECK(cleanBitForOffset(0x560) == CLEAN_DR);           /* DR7 */
    CHECK(cleanBitForOffset(0x568) == CLEAN_DR);           /* DR6 */
    CHECK(cleanBitForOffset(0x640) == CLEAN_CR2);
    CHECK(cleanBitForOffset(0x668) == CLEAN_NP);           /* gPAT */
    CHECK(cleanBitForOffset(0x670) == CLEAN_LBR);          /* DEBUGCTL */
    CHECK(cleanBitForOffset(0x678) == CLEAN_LBR);          /* BR_FROM */

    /* Fields with no clean bit are always loaded. */
    CHECK(cleanBitForOffset(0x440) == CLEAN_NONE);         /* FS */
    CHECK(cleanBitForOffset(0x470) == CLEAN_NONE);         /* LDTR */
    CHECK(cleanBitForOffset(0x570) == CLEAN_NONE);         /* RFLAGS */
    CHECK(cleanBitForOffset(0x578) == CLEAN_NONE);         /* RIP */
    CHECK(cleanBitForOffset(0x600) == CLEAN_NONE);         /* STAR */
    CHECK(cleanBitForOffset(0x0A8) == CLEAN_NONE);         /* event injection */
    CHECK(cleanBitForOffset(0x0C0) == CLEAN_NONE);         /* clean itself */

    /* The two groups the processor may change without updating the VMCB stay dirty. */
    CHECK((kCleanableMask & (1u << CLEAN_TPR)) == 0);
    CHECK((kCleanableMask & (1u << CLEAN_CR2)) == 0);
    CHECK((kCleanableMask & (1u << CLEAN_INTERCEPTS)) != 0);
    CHECK((kCleanableMask & (1u << CLEAN_NP)) != 0);

    Vmcb v;
    memset(&v, 0, sizeof(v));
    VmcsState s(&v);

    /* A write through the VMCS interface dirties the group it lands in. */
    v.control.clean = kCleanableMask;
    CHECK(s.write(0x6802 /* GUEST_CR3 */, 0x1000) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_CR)) == 0);

    v.control.clean = kCleanableMask;
    CHECK(s.write(0x4004 /* exception bitmap */, 1) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_INTERCEPTS)) == 0);

    v.control.clean = kCleanableMask;
    CHECK(s.write(0x681a /* GUEST_DR7 */, 0x400) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_DR)) == 0);

    v.control.clean = kCleanableMask;
    CHECK(s.write(0x6816 /* GDTR base */, 0x1000) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_DT)) == 0);

    v.control.clean = kCleanableMask;
    CHECK(s.write(0x0802 /* CS selector */, 0x8) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_SEG)) == 0);

    v.control.clean = kCleanableMask;
    CHECK(s.write(amdhv::vmx::X_CR2, 0x2000) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_CR2)) == 0);

    v.control.clean = kCleanableMask;
    CHECK(s.write(0x2804 /* GUEST_IA32_PAT */, 0x600) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_NP)) == 0);

    v.control.clean = kCleanableMask;
    CHECK(s.write(0x2802 /* GUEST_IA32_DEBUGCTL */, 0) == amdhv::vmx::R_OK);
    CHECK((v.control.clean & (1u << CLEAN_LBR)) == 0);

    /* RIP has no clean bit: writing it must not clear any group. */
    v.control.clean = kCleanableMask;
    CHECK(s.write(0x681e /* GUEST_RIP */, 0x1000) == amdhv::vmx::R_OK);
    CHECK(v.control.clean == kCleanableMask);

    /* markAllClean / markAllDirty. */
    s.markAllClean();
    CHECK(v.control.clean == kCleanableMask);
    s.markAllDirty();
    CHECK(v.control.clean == 0);

    /* applyControls only dirties the intercepts when the derived intercepts actually change. */
    memset(&v, 0, sizeof(v));
    VmcsState a(&v);
    a.markAllDirty();
    a.applyControls();
    a.markAllClean();
    CHECK(v.control.clean == kCleanableMask);
    a.applyControls();
    CHECK(v.control.clean == kCleanableMask);
    CHECK(a.write(amdhv::vmx::F_CPU_BASED, amdhv::vmx::CPU_HLT_EXIT) == amdhv::vmx::R_OK);
    a.applyControls();
    CHECK((v.control.clean & (1u << CLEAN_INTERCEPTS)) == 0);
    /* The CR group did not change (same EFER), so it stays clean. */
    CHECK((v.control.clean & (1u << CLEAN_CR)) != 0);

    /* Pause filtering: only when the client intercepts PAUSE and the CPU reports the feature. */
    memset(&v, 0, sizeof(v));
    VmcsState p(&v);
    p.setPauseFilter(true, true);
    CHECK(p.write(amdhv::vmx::F_CPU_BASED, amdhv::vmx::CPU_PAUSE_EXIT) == amdhv::vmx::R_OK);
    p.markAllDirty();
    p.applyControls();
    CHECK(v.control.pauseFilterCount == 3000);
    CHECK(v.control.pauseFilterThresh == 128);

    memset(&v, 0, sizeof(v));
    VmcsState q(&v);
    q.setPauseFilter(false, false);
    CHECK(q.write(amdhv::vmx::F_CPU_BASED, amdhv::vmx::CPU_PAUSE_EXIT) == amdhv::vmx::R_OK);
    q.applyControls();
    CHECK(v.control.pauseFilterCount == 0);
    CHECK(v.control.pauseFilterThresh == 0);

    /* Without the PAUSE intercept the fields are not used, even when the CPU has the feature. */
    memset(&v, 0, sizeof(v));
    VmcsState r(&v);
    r.setPauseFilter(true, true);
    r.applyControls();
    CHECK(v.control.pauseFilterCount == 0);
    CHECK(v.control.pauseFilterThresh == 0);

    if (fails) { printf("clean_bits_test: %d FAILED\n", fails); return 1; }
    printf("clean_bits_test: OK\n");
    return 0;
}
