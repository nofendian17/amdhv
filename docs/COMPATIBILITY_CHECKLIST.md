# AMD SVM ↔ Apple Hypervisor.framework compatibility checklist

This is the working checklist for the goal "an AMD CPU runs Apple's Hypervisor.framework clients
optimally". It separates three layers that fail in different ways:

1. **Enablement** — SVM must be on, per CPU, before any client can run.
2. **SVM core** — what the kext does with the VMCB, intercepts, NPT and guest state.
3. **Hypervisor.framework surface** — which `hv_*` calls the unmodified framework can reach.

Each row is marked with evidence, not intent. Status meanings:

| Status | Meaning |
| --- | --- |
| **Yes** | The code path exists and is exercised (host test and/or live probe). |
| **Partial** | Implemented with a documented limitation. |
| **No** | Not implemented; the client gets `HV_UNSUPPORTED` or a no-op. |
| **n/a** | Not applicable to this design. |

Baseline used for this revision (2026-11-23, AMD Ryzen 5 3600, 12 logical CPUs, macOS 15.8.3,
AMDHV 0.2.0 + Phase 1): `make test` passes all 17 tests, `hvf-probe` prints `64 of 64` vCPUs with
`run_until(past deadline)` in microseconds, `hvf-smoke` prints `PASS`, and `amd-info stats` keeps
`enableFailures`, `vmInvalidExits`, `assertFailures` and `vmFpuMismatches` at 0. Re-run those four
after every change; a checklist row without evidence is a hypothesis. `amd-info info` reports 32768
ASIDs and features `0x0013bcff`, including `VmcbClean`, `FlushByAsid`, `PauseFilter` and
`PauseFilterThreshold`.

---

## 1. Enablement (host and firmware)

| Check | Status | Evidence |
| --- | --- | --- |
| AMD vendor and SVM present (`CPUID 8000_0001 ECX[2]`) | **Yes** | `SVM.cpp` `probeThisCpu()` |
| BIOS "SVM Mode" enabled (`VM_CR.SVMDIS` clear) | **Yes** | `VM_CR` is read; `cpuUsable()` refuses a CPU with `SVMDIS` |
| `VM_CR.LOCK` reported, never fought | **Yes** | `AMDHVInfo.svmLocked`; the kext does not try to clear a locked `SVMDIS` |
| `EFER.SVME` set on every usable CPU | **Yes** | `enableAction()` / `disableAction()` |
| `VM_HSAVE_PA` valid on every usable CPU | **Yes** | `allocHostSave()`: 4 KiB, physically contiguous, 4 KiB aligned, below 2^52 |
| CPUs that come online after kext start | **Yes** | `rescan()` probes and enables late CPUs |
| S3/S4 sleep and wake | **Partial** | The ordering bug is fixed and host-tested: `SleepPolicy.hpp` / `tests/sleep_policy_test.cpp` set the suspended state *before* the teardown rendezvous, so no vCPU can re-enable `EFER.SVME` behind it; the PM notification does the teardown while every CPU is up, the kernel's `hv_suspend()`/`hv_resume()` callbacks only move the state (they run after the APs are off / before they are back), and after a resume the sleep epoch makes every vCPU mark all VMCB clean bits dirty and flush its TLB. A real S3 with a live guest is still the acceptance run (section 7, item 9) |
| Per-run re-check of `EFER.SVME` and `VM_HSAVE_PA` | **Yes** | `ensureThisCpu()` repairs drift before `VMRUN`; `healedCount()` counts it |
| Self-test of every CPU | **Yes** | `verifyAll()`; `amd-info selftest` |
| NPT and NRIPS required before a VM is created | **Yes** | `Vm.cpp` `com_amdhv_AMDHVVm::create()` refuses a CPU without both |
| 64 vCPUs per VM | **Yes** | `AMDHV_MAX_VCPUS_PER_VM`; `hvf-probe` creates `64 of 64`, and the 65th is refused |
| Per-CPU tables for 64 logical CPUs | **Partial** | `AMDHV_MAX_CPUS` sizes them for 64; this host exercises only 12, and a CPU with `cpu_number() >= 64` is skipped (section 7, limitation matrix) |
| GIF discipline around the guest | **Yes** | `SvmRun.S`: `CLGI` … `STGI` around `VMSAVE`/`VMLOAD`/`VMRUN` |
| Host interrupts during `VMRUN` | **Yes** | `STI` before `VMRUN` with `V_INTR_MASKING`; `CLI` after the exit |

Acceptance for this layer: `sysctl kern.hv_support` is 1, `amd-info` reports SVM active on every
CPU, `amd-info selftest` passes, and `enableFailures` stays 0.

---

## 2. SVM core (AMD APM Vol. 2, Chapter 15 and Appendix B)

### 2.0 What "fully implemented" means here

The two source documents cover very different scopes, and neither maps 1:1 onto the API AMDHV
has to serve. The audit against APM Vol. 2 rev. 3.40 (Ch. 15 and App. B) and the macOS 15.8
Hypervisor.framework headers gives three separate verdicts:

| Scope | Verdict | Evidence |
| --- | --- | --- |
| SVM mechanisms needed to run the HVF clients listed in section 4 | **Yes**, with the caveats in 2.1 | VMRUN/VMSAVE/VMLOAD and GIF discipline (`SvmRun.S`), intercepts, NPT, event injection, ASID/TLB and clean-bit policy (`Vm.cpp`, `VmcsState.hpp`), all exercised by `hvf-smoke`, VZ and `hvf-probe` with `vmInvalidExits = 0` |
| Every feature described by APM Vol. 2 Ch. 15 / App. B | **No** | The accelerator and isolation features in the table below have no CPUID probe, no enabling VMCB control write and no run path; several cannot be reached from Hypervisor.framework at all |
| Public x86 Hypervisor.framework surface | **Partial** | 32 trap slots: 23 real handlers, 9 logging stubs that answer `HV_UNSUPPORTED`. The in-kernel APIC, IOAPIC, LAPIC, MSI and PIO-notifier families are refused; the rest of the SDK surface is either a shared-page helper (no trap) or a deliberate compatibility no-op |

The older 33047 manual linked in the original goal is the 2005 *Secure Virtual Machine
Architecture Reference Manual*: its equivalent material is Chapter 2 and Appendix C, not
Chapter 15 and Appendix B, and it has no clean bits, pause filter, nRIP or AVIC fields. The
VMCB layout in `VMCB.hpp` follows the newer Appendix B (sources at the end of this page).

Features from current Chapter 15 that AMDHV does **not** implement:

| APM section | Feature | Status | Why / evidence |
| --- | --- | --- | --- |
| 15.21 | NMI virtualization (`V_NMI`, `V_NMI_MASK`, `V_NMI_ENABLE`) | **No** | No CPUID feature bit or VMCB `intCtl` write; physical NMI exits are handed to the client |
| 15.22, 15.32 | SMM support, SMM key / SMM-Lock | **No** | `V3_SMI` is intercepted, but there is no in-kernel SMM state machine and no HVF API for one |
| 15.23, 15.38 | LBR virtualization, instruction-based sampling | **No** | `AMDHV_SVM_FEAT_LBRV` is reported but unused; `VMCB.hpp` reserves the LBR stack fields. IBS is not probed |
| 15.24 | External access protection / DEV | **n/a** | HVF has no IOMMU or device-exclusion-vector surface; devices live in the client |
| 15.25 | GMET, supervisor shadow stacks, read-only guest page tables, INVLPGB/TLBSYNC, vector 5 intercepts | **No** | No CPUID probe, no `nestedCtl` bit, no `interceptVec5` write; the NPT and TLB policy use only what 2.1 lists |
| 15.27, 15.28, 15.31 | SKINIT secure startup, #SX, SVM key | **No** | `V4_SKINIT` is intercepted and reported to the client; no secure loader, TPM or key management exists |
| 15.29 | AVIC and x2AVIC | **No** | `AMDHV_SVM_FEAT_AVIC` is reported but unused; VMCB 0x098/0x0E0/0x0F0/0x0F8 stay zero. HVF refuses `HV_VM_ACCEL_APIC` |
| 15.30 | TSC ratio MSR (`TscRateMsr`) | **No** | Covered by the `TSCRATE` row in 2.2; guest TSC uses `VMCB.tscOffset` only |
| 15.33 | Nested virtualization (vVMSAVE/VMLOAD, VGIF) | **No** | Covered by the `VVMSAVE`/`VGIF` rows in 2.2 |
| 15.34–15.36 | SEV, SEV-ES, SEV-SNP (VMSA, GHCB, RMP, VMPL, VMGEXIT) | **No** | No CPUID Fn8000_001F probe, no key management, no encrypted VMCB; no Apple API exposes it |
| 15.37 | `SPEC_CTRL` virtualization | **No** | The Intel `IA32_SPEC_CTRL`/`IA32_PRED_CMD` MSRs are only emulated as unsupported (`MsrMap.hpp`); the AMD VMCB save field at 0x2E0 is left reserved |
| 15.15/15.16 | Clean bits and TLB control | **Partial** | See 2.1: clean bits only when CPUID reports them, `TLB_CONTROL` uses 0x01/0x03, not the 0x07 non-global flush |

These are gaps against the *specification*, not against the clients in section 4: none of them
is reachable through the calls VZ, QEMU HVF, VirtualBox or the Android emulator make on this
host. Section 5 keeps them out of the P0–P2 roadmap for that reason.

### 2.1 VMCB and intercepts

| Item | Status | Where / notes |
| --- | --- | --- |
| VMCB control area and state-save layout | **Yes** | `VMCB.hpp`; every offset is a `static_assert`, checked by `tests/vmcb_layout.cpp` |
| VMCB clean bits | **Yes** | `CleanBits.hpp` (tested by `tests/clean_bits_test.cpp`): after a successful `VMRUN` every group software did not touch is marked clean, and any write through `VmcsState`/`applyControls()` clears the group it changes. `INT_CTL`/`INT_VECTOR` and `CR2` stay dirty; a vCPU that moves to another CPU reloads everything. Only enabled when CPUID `VMCB_CLEAN` is set |
| Always-on intercepts (effective set after `applyControls()`) | **Yes** | INTR, NMI, SMI, INIT, CPUID, INVD, SHUTDOWN, I/O, MSR, VMRUN, VMMCALL, VMLOAD, VMSAVE, STGI, CLGI, SKINIT |
| Client-driven intercepts mapped from VMX controls | **Yes** | `VmcsState::applyControls()`: HLT, INVLPG, RDPMC, RDTSC/RDTSCP, PAUSE, MWAIT, MONITOR, WBINVD, CR0/CR3/CR4/CR8, MOV-DR, exception bitmap, interrupt window |
| Pause intercept filter | **Yes** | When the client intercepts PAUSE and the CPU reports `PauseFilter`, `applyControls()` sets the count to 3000 (and the threshold to 128 when `PauseThresh` is reported), so a spin loop does not exit on every iteration; the fields stay zero when the CPU lacks the feature |
| I/O permission map | **Yes** | 12 KiB, all ones: every port exits to the client, matching Hypervisor.framework |
| MSR permission map | **Yes** | 8 KiB, all ones; native MSRs clear their bits (`MsrMap.hpp`, `tests/msr_map_test.cpp`) |
| `V_INTR_MASKING` | **Yes** | Always set, so the guest's `EFLAGS.IF` does not gate host interrupts |
| `V_TPR` / TPR shadow | **Yes** | `VmcsState` + CR8 exit path in `Vm::run()`; toggle with `amd-info hv tpr` or `amdhv_tpr_shadow=0` |
| `V_IRQ` / interrupt-window exit | **Yes** | `applyControls()` when the client enables `CPU_INTR_WINDOW` |
| Event injection and re-injection | **Yes** | `eventInj`/`eventInjErr`; external-interrupt and NMI exits are re-injected |
| ASID | **Yes** | One ASID per vCPU from a host-wide pool (`Asid.hpp`, `tests/asid_test.cpp`); when the pool is empty the vCPU shares the host's ASID and flushes the whole TLB every entry |
| TLB control | **Yes** | `TlbPolicy.hpp` (tested by `tests/tlb_policy_test.cpp`): `TLB_FLUSH_BY_ASID` when the nested tables change, the client calls `hv_vcpu_invalidate_tlb`, the client writes CR0/CR3/CR4/EFER through the API, or the vCPU moves to another CPU; `TLB_FLUSH_ALL` when there is no ASID or `amdhv_asid=0` |
| Nested paging (NPT) | **Yes** | `Npt.hpp`: 4-level walk, 4 KiB pages, `US`/`NX`, permissions are the AND of all levels |
| NPT 2 MiB / 1 GiB leaves | **No** | Only 4 KiB leaves; the capability word does not advertise large leaves |
| NPT execute-only pages | **No** | Not advertised |
| NPT accessed/dirty bits | **No** | Not advertised |
| NPF exit information | **Yes** | `exitInfo1`/`exitInfo2` and the permission lookup handed to the client; the GPA is used only when `EXITINFO1` bit 32 (DecodeAssists) says `EXITINFO2` holds one |
| DecodeAssists (APM 15.8) | **Partial** | Used for the two exits the client needs: CR/DR `EXITINFO1` bit 63 names the GPR and NPF bit 32 marks `EXITINFO2` valid. The INTn and INVLPG/INVLPGA assists are not used; those intercepts stay off unless the client asks for them and are reported from the raw exit fields |
| TLB control value 0x07 (non-global flush) | **No** | `TlbPolicy.hpp` emits 0x01 (`TLB_FLUSH_ALL`) or 0x03 (`FLUSH_BY_ASID`) only; non-global flushes are not needed with a per-vCPU ASID |
| Guest state save/restore | **Yes** | `SvmRun.S`: `VMSAVE` host → `VMLOAD` guest → `VMRUN` → `VMSAVE` guest → `VMLOAD` host |
| Guest `EFER.SVME` / `EFER.LMA` | **Yes** | `resetLocked()` and `applyControls()` |
| Guest PAT | **Yes** | VMCB `gPat` |
| Guest DR6/DR7 | **Yes** | VMCB slots |
| Guest DR0–DR3 | **Partial** | Never swapped by `VMRUN`; DR accesses are intercepted (`interceptDr = 0xFFFFFFFF`) |
| Guest `TSC_AUX` (RDTSCP) | **Yes** | Saved and swapped around the run because `VMRUN` does not switch it |
| Guest x87/SSE/AVX and XCR0 | **Yes** | XSAVE areas swapped by hand; `vmFpuMismatches` must stay 0 |
| Guest TSC offset | **Yes** | VMCB `tscOffset`; `hv_vm_sync_tsc` applies the same offset to later vCPUs |
| Guest `WRMSR 10h` (TSC) cannot reach the host TSC | **Yes** | `msr::mustIntercept()` keeps the MSRPM intercept even when the client makes TSC "native" (the framework's slot 144); `run()` emulates RDMSR/WRMSR 10h against `tscOffset` (`tests/msr_map_test.cpp`) |
| Intel-only `IA32_SPEC_CTRL` / `IA32_PRED_CMD` | **Yes** | `hv_vcpu_enable_native_msr` reports success but the intercept stays; `Vm::run()` emulates the access so a Linux guest does not `#GP` (`MsrMap.hpp`) |
| TSC scaling (`TscRateMsr`) | **No** | Not used |
| Next-RIP (`NRIPS`) | **Yes** | Required at VM creation; `nextRip` used for soft event injection and skip-instruction |
| `VMEXIT_INVALID` diagnostics | **Yes** | `dumpVmcb()` prints the state that made `VMRUN` refuse the VMCB |
| 10 ms host slice + client deadline | **Yes** | `RunDeadline.hpp`, `tests/run_deadline_test.cpp`; a past deadline returns exit 52 without entering the guest |
| vCPU kick from another thread | **Yes** | `hv_vcpu_interrupt` raises a flag and sends an IPI to the CPU inside `VMRUN` |

### 2.2 SVM features probed but not used

These are visible in `CPUID Fn8000_000A EDX` and in `AMDHVInfo.svmFeatures`, but no code path turns
them on. They are candidates for the roadmap, not current capabilities. (`VMCB_CLEAN`,
`DECODE_ASSISTS` and `PAUSE_FILTER`/`PAUSE_THRESH` are used now; see 2.1.)

| Feature | Status | Why it matters |
| --- | --- | --- |
| `AVIC` | **No** | In-kernel APIC; Hypervisor.framework refuses `HV_VM_ACCEL_APIC`, so VZ does not need it |
| `VGIF` | **No** | Virtual GIF; host still uses `CLGI`/`STGI` |
| `vVMSAVE/VMLOAD` | **No** | Nested VMSAVE/VMLOAD virtualization |
| `LBRV` | **No** | LBR virtualization |
| `SVML` (SVM lock) | **No** | Nested SVM lock |
| `TSCRATE` | **No** | TSC ratio for migration |

---

## 3. Apple Hypervisor.framework surface

The front end registers the same trap tables AppleHV uses (`hv_set_traps`, `hv_set_callbacks`,
task/thread targets). Indices are the framework's, not ours. A **stub** is logged and answers
`HV_UNSUPPORTED` in mode 1; mode 2 answers success for debugging only.

The macOS 15.8 x86 SDK declares 78 `hv_return_t` functions plus `hv_tsc_clock`. They do not map
1:1 onto the 32 trap slots: register/MSR/VMCS/F-state access and the exec/idle/exit-info queries
read or write the shared pages, so many calls need no trap at all. The slots below split into
**23 real handlers** (task 0–7 and 12–13, thread 0–12) and **9 stubs** (task 8–11 and
14–15, thread 13–15). The tables record trap-level coverage; section 3.4 records the measured
SDK surface. Thread trap 8 (`set_shadow_access`) is a real no-op handler and the TSC-relative path
applies on the next run; both were live-verified after installing the kext.

### 3.1 Task traps

| # | Call | Status | Notes |
| --- | --- | --- | --- |
| 0 | `hv_vm_create` | **Yes** | One VM per task; `HV_VM_ACCEL_APIC` is refused with `HV_UNSUPPORTED` |
| 1 | `hv_vm_destroy` | **Yes** | Refused while a vCPU or an in-flight trap holds the VM; with a live vCPU the client sees `HV_ERROR` and the same call succeeds after the last `hv_vcpu_destroy` (observed 2026-11-23) |
| 2 | `hv_vm_space_create` | **Yes** | Up to 64 extra address spaces; each owns an NPT root (`SpacePool.hpp`) |
| 3 | `hv_vm_space_destroy` | **Yes** | `HV_BUSY` while a vCPU is bound to it; unmaps and unpins its pages |
| 4 | `hv_vm_map` | **Yes** | Pins the caller's pages; 4 KiB alignment; up to `AMDHV_MAP_MAX_SIZE` |
| 5 | `hv_vm_unmap` | **Yes** | Accepts sub-page ranges and rounds them to page boundaries |
| 6 | `hv_vm_protect` | **Yes** | 4 KiB alignment; permissions normalised (W/X imply R) |
| 7 | `hv_vm_sync_tsc` | **Yes** | Stored per VM and applied to vCPUs created later |
| 8 | `hv_vm_add_pio_notifier` | **No** | Stub; VZ uses I/O exits instead |
| 9 | `hv_vm_remove_pio_notifier` | **No** | Stub |
| 10 | `hv_vm_atpic_*` / `hv_vm_ioapic_*` / `hv_vm_lapic_*` | **No** | Stubs; in-kernel APIC only |
| 11 | `hv_vm_lapic_msi` | **No** | Stub |
| 12 | `hv_vcpu_create` | **Yes** | One vCPU per thread, up to 64; same task as the VM. A second create on the same thread is refused with `HV_ERROR` (observed 2026-11-23) |
| 13 | `hv_vcpu_interrupt` | **Yes** | IPI to the CPU in the guest |
| 14–15 | unused | **n/a** | Stubs |

### 3.2 Thread traps

| # | Call | Status | Notes |
| --- | --- | --- | --- |
| 0 | `hv_vcpu_run` | **Yes** | Blocks until a VM exit; 10 ms host slice |
| 1 | `hv_vcpu_run_until` | **Yes** | Deadline folded onto the slice; a past deadline returns exit 52 |
| 2 | `hv_vcpu_invalidate_tlb` | **Yes** | Schedules a flush for that vCPU's next entry (`Vm::forceTlbFlush`) |
| 3–4 | managed-MSR enable/disable | **Yes** | MSR cache plus native/managed lists (`MsrMap.hpp`, `HvMsrs.hpp`) |
| 5 | `hv_vcpu_destroy` | **Yes** | Frees the VMCB, host VMCB, bitmaps and FPU areas |
| 6 | `hv_vcpu_set_space` | **Yes** | The trap argument is the raw space ID (verified); rebinds `nCR3` to that space's NPT root and flushes the vCPU's ASID |
| 7 | `hv_vmx_vcpu_read_vmcs` (slot) | **Yes** | Reads from the VMCS shadow, fills the client's cache slot |
| 8 | `hv_vmx_vcpu_set_shadow_access` | **Yes** | No-op success, live-verified; no shadow VMCS on SVM, but `read/write_shadow_vmcs` round-trip in the framework's client-side area |
| 9 | `hv_vmx_vcpu_set_apic_address` | **Yes** | No-op success; the TPR is kept in `V_TPR`, the page is never read |
| 10 | `hv_vcpu_set_msr_access` | **Yes** | Toggles MSRPM read/write intercepts |
| 11 | sync special registers | **Yes** | DR0–DR7 and other special registers synchronised on demand |
| 12 | writable-VMCS capability | **Yes** | Answers from the capability words in `VmcsCaps.hpp` |
| 13 | APIC sub-ops (`inject_extint`, `apic_read/write`, …) | **No** | Stub; only meaningful with the in-kernel APIC |
| 14–15 | unused | **n/a** | Stubs |

### 3.3 Framework plumbing

| Item | Status | Notes |
| --- | --- | --- |
| Register as the hypervisor provider | **Yes** | Releases AppleHV's traps/callbacks, then installs ours |
| `kern.hv_support` | **Yes** | Written to 1 after registration (`amdhv_hv_sysctl=0` leaves it alone) |
| Task/thread targets | **Yes** | Set on VM/vCPU create, cleared on destroy |
| Shared register page and read-only page | **Yes** | Layout mirrors the framework's; `hv_vcpu_get_exec_time` reads offset 0 without a trap |
| VMCS shadow and capability words | **Yes** | `VmcsState.hpp` / `VmcsCaps.hpp`; words derive from implemented controls |
| TPR shadow switch | **Yes** | Boot-arg `amdhv_tpr_shadow=0` or `amd-info hv tpr 0` |
| Entitlement check | **No** | Native mode: any local process can create a VM (documented limit) |

### 3.4 Every SDK call now has a probe result

`hvf-probe --matrix` calls each of the 79 exported x86 functions once with benign arguments in a
fresh task. On this host (0.2.0, Ryzen 5 3600, macOS 15.8.3) it prints `79 of 79` observed, so
there is no longer an "unverified" bucket. The line-by-line output is in
[CAPABILITIES.md](CAPABILITIES.md#full-public-api-matrix-measured-on-020).

| Group | Functions | Measured answer | Reading |
| --- | --- | --- | --- |
| Capability / clock | `hv_capability`, `hv_tsc_clock` | `ok` (`VCPUMAX=64`), monotonic value | real, framework-side |
| TSC offset | `hv_vcpu_set_tsc_relative` | `ok`, VMCB offset becomes `0x1000` after the run | live-verified after the Phase 1 kext install; framework writes `rw+0xc78` and flag `0x100` |
| Shadow VMCS | `hv_vmx_vcpu_read/write_shadow_vmcs` | `ok`, value round-trips | client-side store only; SVM has no shadow VMCS for the core to consume |
| Shadow access | `hv_vmx_vcpu_set_shadow_access` | `ok` | no-op from thread trap 8, live-verified; consistent with the read/write helpers |
| MSR info / cap-write / APIC address space | `hv_vmx_get_msr_info`, `hv_vmx_vcpu_get_cap_write_vmcs`, `hv_vmx_vcpu_set_apic_address_space` | `ok` (`0` for `IA32_ARCH_CAPABILITIES`), `ok` (thread trap 12), `ok` (no-op like `set_apic_address`) | answers correctly, no extra SVM state needed |
| APIC state / exit helpers | `hv_vm_atpic_*`, `hv_vm_ioapic_get_state/put_state/pulse_irq`, `hv_vm_send_ioapic_intr`, `hv_vm_set_apic_bus_freq`, `hv_vcpu_apic_ctrl/get_state/put_state/trigger_lvt/lsc_*`, `hv_vcpu_exit_init_ap`, `hv_vcpu_exit_startup_ap`, `hv_vcpu_exit_ioapic_eoi`, `hv_vcpu_exit_apic_access_read`, `hv_vcpu_exit_inject_excp` | `HV_UNSUPPORTED` or `HV_ERROR` | same refused in-kernel-APIC family; no change |

`hv_vcpu_flush` is now probed and returns `HV_UNSUPPORTED`, exactly as Apple documents for a
deprecated API. `hv_vcpu_write_fpstate` returns `ok`: it writes the shared FP area that the next run
applies (`Frontend.cpp` reads flag `0x80` and calls `setFpu()` at lines 781–802). Neither name needs
a kext trap of its own.

The two Phase 1 changes are live-verified on the installed kext (UUID
`D7CA29BF-7FA7-3C02-89EA-C22F24539562`): `hvf-probe --matrix` now shows the TSC offset at `0x1000`
after a guest run and `set_shadow_access` answering `ok`. `make test` is 17/17, `hvf-smoke` passes,
and `vmInvalidExits`, `assertFailures`, `enableFailures` and `vmFpuMismatches` stayed at zero.

### 3.5 Phase 3, slices 1–3: APIC foundation and trap decoding

With `amdhv_apic=1` the installed kext accepted `hv_vm_create(HV_VM_ACCEL_APIC)`, and
`hvf-probe --apic` exercised the 32 APIC calls. The live run on the latest installed build returned
`ok` for 27 of them: every AT-PIC op, IOAPIC assert/deassert/pulse/read/write and send-interrupt,
`lapic_set_intr`, `lapic_msi`, `set_apic_bus_freq`, and all ten per-vCPU thread-13 calls
(`inject_extint`, `apic_read/write/get_state/put_state/ctrl/trigger_lvt`, the two LSC enters and
`lsc_invalidate`). Five `hv_vcpu_exit_*` helpers still answer `HV_ERROR`; they do not reach a trap
and appear to be answered by Apple's userspace layer.

Two calls remain broken in that build: IOAPIC get/put state. The diagnostic showed the framework
puts the state pointer in the arg block's `a` word (`0x7ff7...`) and a cookie in `b`
(`0x6daa0aab09840009`); the handler had used `b`. The fix is in this tree and needs a reboot to
confirm. Thread-13 blocks are logged as no-ops: `0` (inject_extint), `0x8001`/`0x8002` (apic
read/write, three pointer words), a tagged pointer to a nested block (get/put state), `0x6` (ctrl),
`0x107` (LVT trigger), `0x5` (LSC enter/invalidate). Their nested contents are still not decoded.

Delivery into the guest is now wired for APIC-accelerated VMs: pending 8259A/IOAPIC requests feed the
vCPU's LAPIC IRR, the guest's TPR updates `LapicState::tpr`, and the highest priority vector is
presented through VM-entry event injection before VMRUN (`Frontend.cpp`). `ApicState.hpp` gained
`atpicPending()` with a host test. This code needs a reboot with the new kext before it is
live-verified; until then it is not claimed as working.

---

## 4. Client compatibility matrix

| Client | What it needs | Status | Evidence |
| --- | --- | --- | --- |
| Docker Desktop (VZ) | `vm_create`, map, vCPU, `run_until`, VMCS reads, interrupt | **Yes** | engine starts; containers serve traffic |
| Kiri (VZ) | same as VZ | **Yes** | Linux/amd64 containers run |
| colima `--vm-type vz` | same as VZ | **Yes** | 4 vCPU / 8 GiB VM starts; `docker run hello-world` succeeds |
| VirtualBox 7.2.22 | VMX controls, NPT, event injection, TPR shadow | **Yes** | Windows 10 boots to desktop; Lubuntu live reaches installer |
| Android emulator 37.3.10 | VMX controls, NPT, FPU/XSAVE | **Yes** | Android 17 AVD boots; ADB, input and network work |
| QEMU 11.1.3 `-accel hvf` (Linux) | VMX controls, NPT | **Yes** | Lubuntu boots with `tsc=reliable` |
| QEMU 11.1.3 `-accel hvf` (Windows) | additional HVF/QEMU behaviour | **Partial** | stops at the Tianocore logo; not attributed to AMDHV |

These are single-host observations, not a support promise. See `docs/CLIENTS.md`.

---

## 5. Gap register and roadmap

Closed during this review: a guest `WRMSR 10h` can no longer move the host TSC (see section 7,
item 5), the guest TLB is no longer flushed on every entry (each vCPU has its own ASID and uses
`TLB_FLUSH_BY_ASID` only when a translation can actually be stale), VMRUN no longer reloads the
whole guest state on every entry (VMCB clean bits, except the two groups the processor changes by
itself), and an NPF exit now reports a guest physical address only when the processor says one is
valid and looks its permissions up in the vCPU's own address space.

Ordered by what blocks real clients first, not by what is easiest.

### P0 — correctness and stability

- [x] `waitForSystemMapper` at boot (root-caused and fixed). The only emitter in XNU is
      `IOMapper::waitForSystemMapper()`, reached from `IOBufferMemoryDescriptor` when it is not given
      `kIOMemoryMapperNone`. `allocHostSave()` runs from `start()` (boot arg `amdhv_enable_svm=1`) and
      allocated the host-save pages without that flag, so it waited for the system mapper and printed
      the backtrace naming `com.amdhv.AMDHV`. `allocHostSave()`, `amdhv::physAlloc()` and the front
      end's shared pages now pass `kIOMemoryMapperNone`: they are addressed by physical address and
      are never DMA-mapped. Acceptance: on the next boot,
      `log show --last boot --predicate 'eventMessage CONTAINS "waitForSystemMapper"'` has no kernel
      line (section 7, item 2).
- [x] Fix sleep/wake with a running VM. `suspend()` marks the system suspended before the rendezvous,
      so a vCPU that is between its last check and `VMRUN` cannot re-enable `EFER.SVME` in
      `ensureThisCpu()`; that function now refuses with `-3` while a suspend is in progress. The
      kernel's `hv_suspend()`/`hv_resume()` callbacks only move the sleep state, because they run
      after the APs are off / before they are back; the PM notification does the
      `EFER.SVME`/`VM_HSAVE_PA` teardown while every CPU is still up. On resume the sleep epoch
      changes, and each vCPU marks every VMCB clean bit dirty and flushes its TLB before the next
      entry (the processor lost its cached VMCB state). Covered by `tests/sleep_policy_test.cpp`.
- [ ] Confirm the sleep fix with one real S3/S4 while a guest is running. This needs the machine to
      sleep, so it is not done in this session; the procedure and the counters to check are in
      section 7, item 9.
- [x] Panic/hang attribution. None of the reports in the appendix is attributed to the kext: the two
      page faults name Lilu/VirtualSMC with a RIP outside the kext's range, the watchdog's 5.1e9 s is
      the RTC (`AppleRTC: lost battery power`), and the kext counters were zero. A regression test
      cannot be written for a cause that has not been shown to be ours. The two hangs this layer could
      cause are covered instead: `sleep_policy_test` for the suspend/VMRUN ordering and
      `clean_bits_test` for stale VMCB state after a resume. Keep the rule from section 7: reproduce a
      panic with the kext unloaded before adding a test for it.
- [x] Verify that guest TSC activity cannot move the host clock. The only guest-writable path was
      `WRMSR 10h`, now emulated against `tscOffset` (section 7, item 5); `RDTSC`/`RDTSCP` only read
      `host TSC + offset`.

### P1 — performance without changing behaviour

- [x] Use VMCB clean bits (`VMCB_CLEAN`) instead of `clean = 0` on every run. `VmcsState` clears a
      group on every write; `applyControls()` only clears the intercepts group when the derived
      intercepts or EFER actually change; `run()` marks everything clean after a successful `VMRUN`
      and dirty on a new CPU or `VMEXIT_INVALID`. `INT_CTL`/`INT_VECTOR` and `CR2` stay dirty because
      the processor may change them without updating the VMCB.
- [x] Allocate an ASID per vCPU and use `FLUSH_BY_ASID`; `TLB_FLUSH_ALL` remains the fallback when
      there is no ASID or `amdhv_asid=0`. A vCPU also flushes when it moves to another CPU (whose TLB
      may still hold entries for a reused ASID) and when the client changes paging state through the
      API.
- [ ] Offer 2 MiB NPT leaves for large guest ranges; keep 4 KiB for sub-page protection.
      Deferred: a 2 MiB leaf needs a 2 MiB-aligned guest range whose 512 host pages are also
      physically contiguous and 2 MiB-aligned. `mapRange()` already walks the descriptor's physical
      segments (`mdSegment()` calls `getPhysicalSegment`), but for anonymous user memory those
      segments are usually a single 4 KiB page, so the window almost never exists. The table has no
      large-leaf support today either: `encodeLeaf()` never sets PS, `walk()`/`lookup()` assume a
      level-1 leaf, `unmap()`/`protect()` would have to split a large leaf into 512 leaves, and
      `clear()` releases one owner per 4 KiB page. Correctness does not depend on it, so 4 KiB
      leaves stay until a client maps a physically contiguous range (or a diagnostic shows a useful
      share of 2 MiB-aligned contiguous windows).
- [x] Use the DecodeAssists fields where the CPU reports them. The kext never decodes guest
      instructions itself, so the assists matter at two exits: a CR/DR exit's `EXITINFO1` bit 63
      names the GPR, and an NPF exit's bit 32 says `EXITINFO2` holds the guest physical address.
      Both are read only when their valid bit is set: the CR/DR GPR when bit 63 is 1, the NPF GPA
      when bit 32 is 1. Without bit 32 the EPT violation reports GPA 0 and clears the VMX "linear
      address valid"/"translated" bits, so a client declines to emulate instead of using a bogus
      address. Verified on this host: `hvf-smoke` stage 3 stores to an unmapped page and reads back
      `VMCS_GUEST_PHYSICAL_ADDRESS` = 0x101010, so this CPU does set bit 32.
- [x] Configure the PAUSE filter threshold so guest spin loops do not exit on every iteration. Set
      only when the client intercepts PAUSE; the count (3000) and threshold (128) are gated on the
      CPUID `PauseFilter`/`PauseThresh` bits.

### P2 — API surface that some clients may call

- [x] `hv_vm_space_create` / `map_space` / `protect_space` / `unmap_space` / `space_destroy`.
      `SpacePool.hpp` hands out IDs 1..64 (0 is the default space); each ID owns an `npt::Table`.
      `map/unmap/protect` take the space from the framework's map item, and thread trap 6
      (`hv_vcpu_set_space`) points the vCPU's VMCB `nCR3` at that root and flushes its ASID.
      `space_destroy` refuses with `HV_BUSY` while a vCPU is still bound to it.
- [ ] `hv_vm_add_pio_notifier` / `remove` (VZ uses I/O exits, so this is not on the VZ path).
      Deferred: it needs a notifier registry and a mach-port notification on matching port I/O; the
      current front end already reports every I/O exit to the client.
- [ ] `hv_vcpu_inject_extint` and the APIC sub-ops, only if the in-kernel APIC is ever offered.
      Deferred: `hv_vm_create(HV_VM_ACCEL_APIC)` is refused, so there is no in-kernel APIC to inject
      into; VZ emulates the APIC and uses `hv_vcpu_interrupt`.
- [ ] `hv_vcpu_flush` (deprecated by Apple; answer `HV_UNSUPPORTED` deliberately).
      Not planned: Apple deprecated the call; a client that needs the effect uses
      `hv_vcpu_invalidate_tlb`.

### P3 — hardware features that would change the model

- [ ] AVIC (only if `HV_VM_ACCEL_APIC` is ever supported).
- [ ] Virtual GIF, vVMSAVE/VMLOAD, LBR virtualization, SVM lock.
- [ ] TSC scaling for migration-style use cases.
- [ ] NPT execute-only pages and A/D bits.

Acceptance for every item: code + host test where the logic is testable + `hvf-probe` change (if the
API surface changes) + one client run + `amd-info stats` with `vmInvalidExits` and
`assertFailures` at 0.

---

## 6. Verification commands

```bash
make test                    # 15 host tests
make kext tools              # build/AMDHV.kext, amd-info, hvf-smoke, hvf-probe

sudo build/amd-info          # SVM state on every CPU
sudo build/amd-info selftest # verify EFER.SVME and VM_HSAVE_PA per CPU
sudo build/amd-info stats    # vmInvalidExits and assertFailures must stay 0
./build/amd-info log         # kernel ring, including every trap's argument (no root needed)

./build/hvf-smoke            # end-to-end public-API VMM
./build/hvf-probe            # compare against docs/CAPABILITIES.md
sysctl kern.hv_support       # must print 1
```

After a kext change, a reboot is required: the running kext cannot be replaced in place.

### Post-reboot verification (2026-11-22, boot 21:53)

The kext installed in the EFI partition is the build from this tree: the loaded UUID
`EEF2CE05-5716-3A2E-B3AE-D0352C507905` matches `build/AMDHV.kext/Contents/MacOS/AMDHV`, and the
kernel ring reports `built Nov 22 2026 21:33:38`. The version string is still 0.1.2, so use the UUID
or build time to tell this binary from the previous one.

- `log show --last boot --predicate 'eventMessage CONTAINS "waitForSystemMapper"'` returns no kernel
  line. The boot-allocation fix is effective.
- `amd-info info`: 12 logical CPUs, SVM usable and active on all 12, `enableFailures = 0`.
- `amd-info selftest` (root): `12 of 12 CPUs ok`, so `EFER.SVME` and `VM_HSAVE_PA` hold on every CPU.
- `hvf-smoke`: PASS (all stages, including the NPF handler and `hv_vcpu_interrupt`).
- `hvf-probe`: matches `docs/CAPABILITIES.md`. In particular `hv_vm_space_create`, `map_space`,
  `protect_space`, `unmap_space`, `set_space`, and `HV_BUSY` on `space_destroy` while bound all
  succeed/answer as documented; the deliberately unimplemented APIC/PIO/extint calls still answer
  `HV_UNSUPPORTED`.
- Counters after both tools: `vmCreates = 67`, `vmRuns = 73`, `vmExits = 596`,
  `vmInvalidExits = 0`, `vmFpuMismatches = 0`, `assertFailures = 0`.
- No panic was written after the boot (`/Library/Logs/DiagnosticReports` only has the 17:xx reports
  that predate this boot).
- `sleepEvents = 0`, `wakeEvents = 0`: the S3/S4 path is still the one item without a hardware run
  (section 7, item 9).

### Live Virtualization.framework run (2026-11-22 22:40, same boot)

An unmodified client (`com.apple.Virtualization.VirtualMachine`, pid 3875) ran a 6-vCPU guest with
4 GiB of guest RAM for 32 minutes while the counters below were read. Guest RAM accounts for
`vmPagesMapped = 1048576` (4 GiB in 4 KiB leaves). The ring shows `vcpu0`…`vcpu5` entering through
`vcpu_run_until`, and the dominant exit is 48 (VMX EPT-violation, i.e. the AMD NPF path), which is
how the client emulates MMIO.

- `vmCreates = 73`, `vmRuns = 35985555`, `vmExits = 36748682`.
- `vmInvalidExits = 0`, `vmFpuMismatches = 0`, `assertFailures = 0`, `enableFailures = 0`.
- `vmNptFaults = 30573198`: the NPF report path is the hottest path in this run and never produced
  an invalid exit.
- No panic report was written during or after the run.

The three panic reports from earlier the same day (04:39, 17:17, 17:25) predate this build. Their
backtraces implicate `as.vit9696.VirtualSMC` / `as.vit9696.Lilu` (two page faults at CR2
`0xfed80310`) and `com.apple.driver.watchdog` (watchdog timeout), with no AMDHV frame.

### Limitation validation (2026-11-23, boot 09:54, AMDHV 0.2.0)

The installed kext is the 0.2.0 release build (`A68EAC46-466C-3132-98E9-FB0D39B1F2A9`). The host is
the same Ryzen 5 3600 (12 logical CPUs) running macOS 15.8.3 (24H241). `make test` passes all 15
tests, `amd-info selftest` answers `12 of 12 CPUs ok`, and `amd-info info` reports SVM active on all
12 CPUs with `enableFailures = 0`.

| Claim | Result | Evidence |
| --- | --- | --- |
| 64 vCPUs per VM | **Confirmed** | `hvf-probe`: `64 of 64`. A separate 65-thread probe created 64; the 65th `hv_vcpu_create` was refused (`HV_ERROR`, `0xfae94001`, from `AMDHVVmShared::attach()` returning `kIOReturnNoResources`). `kMaxVcpus` and `AMDHV_MAX_VCPUS_PER_VM` are 64. |
| One VM per task | **Confirmed** | `hvf-probe`: `hv_vm_create twice in one task` → `HV_BUSY`; `tVmCreate()` returns `kHvBusy` when the task target is set. |
| Several VM processes at once | **Confirmed for two** | Two `hvf-smoke` processes ran together; both printed `PASS`, and `vmInvalidExits` stayed 0. This does not make "several at once" as tested as a single VM. |
| 64 extra address spaces | **Confirmed** | 65 `hv_vm_space_create` calls: 64 succeed and the 65th returns `HV_NO_RESOURCES` (`0xfae94005`); `SpacePool.hpp` sets `kMax = 64`. |
| Single `hv_vm_map` range | **16 GiB cap confirmed** | `AMDHV_MAP_MAX_SIZE` is 16 GiB (`Vm.cpp`); a request for 16 GiB + 1 returns `HV_BAD_ARGUMENT` (`0xfae94003`). The largest live mapping in the record is 4 GiB. |
| No entitlement check | **Confirmed** | An unsigned binary with no entitlements, run as uid 501, created a VM and a vCPU successfully. `kext/Source` contains no entitlement lookup. The tools are still signed with `com.apple.security.hypervisor` for Apple's framework. |
| Sleep/wake with a live guest | **Still not exercised** | `kern.sleeptime = 0`, `kern.sleep_abs_time = 0`, `kern.hibernatecount = 0`, `pmset` reports `Total Sleep/Wakes since boot: 0`, and the kext reports `sleepEvents = 0`, `wakeEvents = 0`. `tests/sleep_policy_test.cpp` passes; the S3 acceptance run remains section 7, item 9. |
| QEMU Windows hang, VirtualBox Windows | **Not re-run** | No standalone `qemu-system-x86_64` is on `PATH` or in `/usr/local`, `/opt/local` or `/opt/homebrew`; the only copy is the Android emulator's private build, which is not the upstream QEMU the test used. VirtualBox 7.2.22 is present, but its only registered VM is Ubuntu (`deb`) and no Windows ISO is in `~/Downloads` or `~/isos`. The observations in `docs/QEMU.md` and `docs/VIRTUALBOX.md` remain prior evidence, not a fresh run. |
| One vCPU per thread, VM teardown | **Confirmed** | A second `hv_vcpu_create` on the same thread returns `HV_ERROR` (`0xfae94001`). `hv_vm_destroy` with a live vCPU also returns `HV_ERROR`, and the same call succeeds after the last `hv_vcpu_destroy`. The capability tables used to say `HV_BUSY`; the client-visible answer is `HV_ERROR`. |
| 2 MiB / 1 GiB NPT leaves | **Confirmed absent** | `Npt.hpp` `encodeLeaf()` sets no PS bit; `VmcsCaps.hpp` states the capability word does not advertise large leaves; `npt_test` passes. |
| PIO notifier, APIC sub-ops, `hv_vcpu_flush` | **Confirmed unsupported** | `hvf-probe` answers `HV_UNSUPPORTED` for `hv_vcpu_inject_extint`, the APIC/IOAPIC calls and the PIO notifier. A direct call returned `hv_vcpu_flush = HV_UNSUPPORTED`. |
| AVIC, VGIF, vVMSAVE/VMLOAD, LBRV, SVML, TSCRATE | **Confirmed unused** | The CPU reports the feature bits (`amd-info info`), but no VMCB control path enables them and the capability words do not advertise the corresponding controls. |
| >64 logical CPUs | **Not exercisable here** | This host has 12. `AMDHV_MAX_CPUS` is 64, and `probeAction()`/`enableAction()` skip `cpu_number() >= 64`. |
| Guest DR0–DR3 | **Partial, by design** | `Vm.cpp` sets `interceptDr = 0xFFFFFFFF`; VMRUN does not swap DR0–DR3, so the client services them. Not exercised live in this run. |
| Verified host | **Confirmed** | AMD Ryzen 5 3600, macOS 15.8.3 (24H241), 12 logical CPUs, kext 0.2.0; `hvf-smoke`, `hvf-probe`, `amd-info info` and `selftest` all pass. |

Counters after the final runs: `vmCreates = 210`, `vmRuns = 676`, `vmExits = 1763`,
`vmInvalidExits = 0`, `vmFpuMismatches = 0`, `assertFailures = 0`, `enableFailures = 0`,
`sleepEvents = 0`, `wakeEvents = 0`. The kext ring contains 12 warnings: ten `VM map failed` lines
from the probe's deliberate invalid maps and the `VmCreate: ... no room for another vCPU` pair from
the expected refusal of the 65th vCPU. No other warning was written.

---

## 7. Open questions and observed issues

1. **Thread-trap labels (resolved).** `kThreadNames[3]` used to be `disable_managed_msr` and `[4]`
   `enable_managed_msr`, while `kThreadTable` installs the enable handler at index 3 and the disable
   handler at index 4. Verified against the running framework: 1000 calls to
   `hv_vcpu_enable_managed_msr(vcpu, 0x10, true)` logged `trap thread 3 ... arg=0x90` and 1000 calls
   with `false` logged `trap thread 4 ... arg=0x90`, so the table is right and the labels were
   swapped. The labels now match.
2. **`waitForSystemMapper` at boot (root-caused and fixed).** XNU emits this only from
   `IOMapper::waitForSystemMapper()`, which `IOBufferMemoryDescriptor` calls unless the allocation
   passes `kIOMemoryMapperNone`. `allocHostSave()` runs from `start()` when
   `amdhv_enable_svm=1`, and it allocated the host-save pages without that flag, so it waited for the
   system mapper and printed the backtrace that named `com.amdhv.AMDHV`. All physical-memory
   allocations in the kext now pass `kIOMemoryMapperNone`; they are addressed by physical address and
   never DMA-mapped. The warning should be absent on the next boot:
   `log show --last boot --predicate 'eventMessage CONTAINS "waitForSystemMapper"'` should return no
   kernel line.
3. **QEMU Windows hang.** OVMF and `cdboot.efi` load, then setup never starts. Windows works under
   VirtualBox and Android works under the emulator through the same stack, so this is QEMU-specific
   until proven otherwise.
4. **Host clock.** This host reports `AppleRTC: lost battery power - time may be invalid` and has
   produced a watchdog panic with a bogus elapsed time. Treat clock-dependent panics as environment
   issues until the RTC is fixed; do not attribute them to SVM without a trace.
5. **Guest TSC writes (fixed in this revision).** `hv_vcpu_enable_native_msr(0x10)` used to clear the
   MSRPM intercept for the TSC. SVM applies `TSC_OFFSET` to reads only, so a guest `WRMSR 10h` would
   have written the *host* TSC. The intercept now stays set and `run()` emulates the access. This is
   a plausible mechanism for host-clock weirdness, but it does not explain the 5.1e9 s elapsed time
   in the watchdog panic below.
6. **ASID/TLB path (exercised).** The build installed on 2026-11-22 21:53 already carried the
   per-vCPU ASID and `FLUSH_BY_ASID` logic (commit `d0b3751` at 21:18, built at 21:33), so the
   36.7M-exit VZ guest later that evening ran it; the 0.2.0 build on 2026-11-23 adds the sleep fix,
   not a new ASID path. That run finished with `vmInvalidExits = 0` and `assertFailures = 0`, and
   `amd-info info` reports `FlushByAsid`. `amdhv_asid=0` still restores the old
   flush-on-every-entry behaviour for an A/B comparison.
7. **VMCB clean bits (exercised).** The same 21:33 build enabled them, and the 36.7M-exit VZ run
   produced no `VMEXIT_INVALID`, no guest corruption and `assertFailures = 0`. The path is gated on
   CPUID `VMCB_CLEAN`, which this CPU reports. If stale state is ever suspected, set
   `control.clean = 0` unconditionally in `run()` (one line) to restore the old behaviour while
   keeping the rest of the change.
8. **Address spaces (exercised).** `hv_vm_space_create`/`map_space`/`protect_space`/`unmap_space`/
   `space_destroy` allocate real NPT roots. `hvf-probe` passes all of them, answers `HV_BUSY` on
   `space_destroy` while a vCPU is bound and `HV_BAD_ARGUMENT` for an unknown space; a separate
   probe created 64 extra spaces and got `HV_NO_RESOURCES` on the 65th, matching `SpacePool.hpp`
   (`kMax = 64`). The trap ABI was checked against the running framework with `amd-info log`:
   - `hv_vm_space_create(&sp)` passes a pointer to a 4-byte `hv_vm_space_t` (the log showed the
     pointer and the variable's initial `0xdeadbeef`);
   - `hv_vm_space_destroy(id)` and `hv_vcpu_set_space(vcpu, id)` pass the raw ID (`0x77`, `0xdead`,
     `0x1110`, `0x2220` appeared verbatim); the vCPU is the calling thread's;
   - the map item is `{ space, uva, gpa, size, flags }`; `hv_vm_protect_space(0x77, 0x5000, ...)`
     logged `[0x77 0 0x5000 0x1000 ...]`.
   `space_destroy` deliberately answers `HV_BUSY` while a vCPU is still bound; rebind it to the
   default space first.
9. **Sleep with a live guest (code fixed, hardware run pending).** The suspend/resume state machine
   is covered by `tests/sleep_policy_test.cpp`, but the actual S3 path has not been run with a guest
   alive. Acceptance: start `build/hvf-smoke` in a loop (or a VZ/VirtualBox guest), sleep the host,
   wake it, and check that the guest continues and that `amd-info stats` shows `sleepEvents = 1`,
   `wakeEvents = 1`, `vmInvalidExits = 0` and `assertFailures = 0`. If the guest resumes with stale
   state, the first suspect is the clean-bit reload in `run()` (sleep epoch); set
   `control.clean = 0` in `run()` to bisect.

   First hardware attempt, 2026-11-22 22:02 (boot 21:53): the host never entered S3.
   `pmset -g log` reports `Total Sleep/Wakes since boot: 0`, and `kern.sleeptime`,
   `kern.sleep_abs_time` and `kern.hibernatecount` stay 0, so `amd-info stats` shows
   `sleepEvents = 0` and `wakeEvents = 0`. The kernel log shows why: a software sleep at 22:02:10 did
   the full-to-dark transition (`=== START ... f->9`, `dark wake entry` at 22:02:12), and a display
   tickle at 22:02:24 promoted it back to full wake. The system-sleep transition that actually cuts
   power never ran. The kext is registered correctly: it is in the priority client list
   (`kIOMessageSystemCapabilityChange[6] to com.amdhv.AMDHV`), and its handler acknowledges
   immediately (returns `kIOReturnSuccess`, leaving `notify.returnValue` at 0), so no
   `acknowledgeSleepWakeNotification()` is needed. XNU only sends `kIOMessageSystemWillSleep` to
   kernel priority clients from the `kSystemTransitionSleep` branch; `systemMessageFilter()` requires
   `kSystemMessageClientKernel`, which the full-to-dark branch clears. The zero counters are
   therefore expected and are not a kext fault. Acceptance still requires a boot that reaches S3:
   `kern.sleeptime` becomes non-zero, then `sleepEvents = 1`, `wakeEvents = 1`.

   Second attempt, 2026-11-23 09:54 boot (AMDHV 0.2.0): the host still did not sleep.
   `kern.sleeptime`, `kern.sleep_abs_time` and `kern.hibernatecount` are 0, `pmset -g log` reports
   `Total Sleep/Wakes since boot: 0`, and `amd-info stats` shows `sleepEvents = 0`,
   `wakeEvents = 0`. The 0.2.0 build carries the sleep fix; only the hardware S3 run is missing.

### Appendix — panic evidence seen on 2026-11-22

Three panic reports were present: two identical page faults with `CR2 = 0xfed80310` and the same
faulting RIP offset, and one watchdog timeout reporting `5133704748` seconds without a check-in.
The page-fault backtrace names Lilu 1.7.2 and VirtualSMC 1.3.8; the faulting RIP is outside
`com.amdhv.AMDHV`'s loaded range (`0xffffff80053a7000`–`0xffffff80053dafff`). The kext's own counters
were clean (`vmInvalidExits = 0`, `assertFailures = 0`). Keep this as evidence, not as a diagnosis:
reproduce with the kext unloaded before blaming SVM. The TSC-write hazard (item 5) is closed now and
is not a demonstrated cause of either panic.

---

## Sources

- Apple, *Hypervisor.framework*: <https://developer.apple.com/documentation/hypervisor>
- Apple, *Virtualization*: <https://developer.apple.com/documentation/virtualization>
- xnu public kext interface: `osfmk/kern/hv_support_kext.h`
- AMD, *AMD64 Architecture Programmer's Manual, Volume 2: System Programming*, publication
  24593, Chapter 15 (SVM) and Appendix B (VMCB layout). The VMCB offsets in `VMCB.hpp`
  (clean bits 0xC0, nRIP 0xC8, instruction bytes 0xD0, AVIC pointers from 0xE0) follow this
  layout (rev. 3.40 checked for this audit):
  <http://kib.kiev.ua/x86docs/AMD/AMD64/24593_APM_v2-r3.40.pdf>
- AMD, *Secure Virtual Machine Architecture Reference Manual*, publication 33047,
  Chapter 2 (SVM) and Appendix C (VMCB layout). This is the older manual linked from the
  original goal; it predates VMCB clean bits, pause filtering, NRIPS and AVIC, so its
  chapter/appendix numbers are **not** the ones used in section 2:
  <https://www.0x04.net/doc/amd/33047.pdf>
- applevisor (Rust bindings that document the same API surface):
  <https://docs.rs/applevisor/latest/applevisor/>
- This repository: `docs/CAPABILITIES.md` (API matrix), `docs/CLIENTS.md` (per-client guides),
  `kext/Source/VMCB.hpp`, `kext/Source/Npt.hpp`, `kext/Source/VmcsState.hpp`,
  `kext/Source/VmcsCaps.hpp`, `kext/Source/Frontend.cpp`.
