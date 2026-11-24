# AMD SVM ↔ Apple Hypervisor.framework compatibility checklist

Developer quick reference. AMDHV replaces AppleHV as the xnu hypervisor provider so that an
**AMD Ryzen / SVM** CPU can run unmodified Hypervisor.framework clients. This page tells you what is
actually implemented, where an API is answered, and what is deliberately not supported.

## 0. Read this first

### Verdict by layer

| Layer | Status | Meaning for a developer |
| --- | --- | --- |
| xnu provider ABI | **Yes** | `hv_set_traps`, `hv_set_callbacks`, task/thread targets and `hv_suspend`/`hv_resume` match `xnu-11417.140.69` and `xnu-12377.121.6`. |
| AMD SVM core on Ryzen | **Yes (subset)** | VMRUN/VMSAVE/VMLOAD, intercepts, NPT 4 KiB, ASID/TLB, clean bits, event injection, TSC and FPU state are real. Full AMD APM Ch.15 is **not** implemented. |
| Hypervisor.framework API used by real clients | **Yes** | VZ, QEMU HVF, VirtualBox and the Android emulator use the implemented subset. |
| Whole public Hypervisor.framework surface | **Partial** | 79 functions are probed and answered; not all are real. PIO notifier is a stub, APIC is opt-in, five exit helpers return `HV_ERROR`. |
| Whole AMD APM Vol.2 Ch.15 / App.B | **No** | AVIC, SEV, NMI virtualization, SMM, nested virtualization, TSC scaling, large NPT leaves, etc. are absent. |

### Status meanings

| Status | Meaning |
| --- | --- |
| **Yes** | Real code path, covered by a host test and/or live probe. |
| **Framework** | Answered by Hypervisor.framework itself (userspace / shared page); no kext trap is involved. |
| **Opt-in** | Real handler exists but is only reachable with boot-arg `amdhv_apic=1`. |
| **No-op** | Returns success but has no VM effect; kept for compatibility. |
| **No** | Stub or unsupported; client sees `HV_UNSUPPORTED` / `HV_ERROR`. |
| **n/a** | Not applicable to the SVM/native-provider design. |

### Versions and evidence

| Item | Value |
| --- | --- |
| Validation host | AMD Ryzen 5 3600, 12 logical CPUs, macOS 15.8.3 (24H241) |
| Validation build | AMDHV 0.2.0, 2026-11-23 run |
| Current tree | **v0.3.1** (`CHANGELOG.md`, tag `v0.3.1`; `Info.plist` fallback 0.3.0, stamped from tag at build time) |
| Host tests | `make test` → **20/20** (`hv_api_constants_test`, `hv_kext_abi_test` added by the API/ABI hardening) |
| Kext build | `make kext` → 393 imported symbols resolve; no ABI change |
| Historical figures | `18 tests` / `18/18` in the records below belong to the 0.2.0 validation build |

`79 of 79` in `hvf-probe --matrix` means **all 79 calls were probed and answered**, not that all 79
are fully implemented. The tables below make that distinction.

---

## 1. Hypervisor.framework API matrix

The macOS 15.x x86 SDK exports **78 `hv_return_t` functions + `hv_tsc_clock`**. They do not map 1:1
to the 32 trap slots: registers, MSRs, VMCS fields and FP state live in shared pages, so many calls
need no trap at all. Trap slots: **26 real handlers**, **6 logging stubs**.

"Answered by" values: **Framework** = userspace/shared page; **Shared page** = framework writes a
field the kext reads on the next VMRUN; **Trap** = xnu dispatches to `Frontend.cpp`; **Stub** = logged
and answered `HV_UNSUPPORTED` (mode 2 answers success for debugging only).

### 1.1 VM, address spaces, memory and clock (13)

| API | Status | Answered by | What AMDHV does on Ryzen |
| --- | --- | --- | --- |
| `hv_capability` | **Yes** | Framework | Returns `VCPUMAX=64`; `HV_CAP_ADDRSPACEMAX` also verified as `64` (direct call: `ret=0`, `value=64`). |
| `hv_vm_create` | **Yes** | Trap 0 | One VM per task; `HV_VM_ACCEL_APIC` (`1<<10`) refused unless `amdhv_apic=1`. |
| `hv_vm_destroy` | **Yes** | Trap 1 | Refused while a vCPU/trap still holds the VM; client sees `HV_ERROR` with a live vCPU. |
| `hv_vm_space_create` | **Yes** | Trap 2 | Up to 64 extra spaces, each with its own NPT root (`SpacePool.hpp`). |
| `hv_vm_space_destroy` | **Yes** | Trap 3 | `HV_BUSY` while a vCPU is bound; unmaps/unpins pages. |
| `hv_vm_map` | **Yes** | Trap 4 | Pins caller pages; 4 KiB aligned; single-map cap 16 GiB. |
| `hv_vm_unmap` | **Yes** | Trap 5 | Accepts sub-page ranges and rounds to page boundaries (needed by QEMU TPM). |
| `hv_vm_protect` | **Yes** | Trap 6 | 4 KiB aligned; W/X imply R. |
| `hv_vm_map_space` | **Yes** | Framework → kext | Same as `hv_vm_map` into a named space. |
| `hv_vm_unmap_space` | **Yes** | Framework → kext | Same as `hv_vm_unmap` from a named space. |
| `hv_vm_protect_space` | **Yes** | Framework → kext | Same as `hv_vm_protect` in a named space. |
| `hv_vm_sync_tsc` | **Yes** | Trap 7 | Stores guest TSC value; sets VMCB `tscOffset = tsc - rdtsc()` for all vCPUs. |
| `hv_tsc_clock` | **Yes** | Framework | Abstract clock (`rdtsc + adjust - base`); monotonic value returned to the client. |

### 1.2 vCPU lifecycle and scheduling (10)

| API | Status | Answered by | What AMDHV does on Ryzen |
| --- | --- | --- | --- |
| `hv_vcpu_create` | **Yes** | Trap 12 | One vCPU per thread, up to 64; second create on the same thread → `HV_ERROR`. |
| `hv_vcpu_destroy` | **Yes** | Thread trap 5 | Frees VMCB, host VMCB, bitmaps and FPU areas. |
| `hv_vcpu_set_space` | **Yes** | Thread trap 6 | Rebinds vCPU `nCR3` to the space's NPT root and flushes its ASID. |
| `hv_vcpu_run` | **Yes** | Thread trap 0 | Blocks until VM exit; 10 ms host slice. |
| `hv_vcpu_run_until` | **Yes** | Thread trap 1 | Deadline folded onto the slice; already-expired deadline returns exit 52 without entering guest. |
| `hv_vcpu_interrupt` | **Yes** | Trap 13 | Raises kick flag and sends an IPI to the CPU inside VMRUN. |
| `hv_vcpu_invalidate_tlb` | **Yes** | Thread trap 2 | Schedules ASID-aware flush for the vCPU's next entry. |
| `hv_vcpu_get_exec_time` | **Yes** | Shared page (offset 0) | Updated after every run; no trap. |
| `hv_vcpu_get_idle_time` | **Yes** | Shared page (offset 8) | Returns 0; HLT is a client exit, not emulated in-kernel. |
| `hv_vcpu_vmx_status` | **No** | Framework | Read-only page is zero ("no extra information") → `HV_ERROR`, as on Apple. |

### 1.3 Registers, MSRs, FP state and TSC (11)

| API | Status | Answered by | What AMDHV does on Ryzen |
| --- | --- | --- | --- |
| `hv_vcpu_read_register` | **Yes** | Shared page | GPR/segment/CR state from the framework register page. |
| `hv_vcpu_write_register` | **Yes** | Shared page | Written before the next VMRUN; DR0–DR7 go through slot flag `0x40`. |
| `hv_vcpu_read_fpstate` | **Yes** | Shared page | XSAVE area swapped by hand; `vmFpuMismatches` must stay 0. |
| `hv_vcpu_write_fpstate` | **Yes** | Shared page (flag `0x80`) | `setFpu()` applies it on the next run. |
| `hv_vcpu_enable_native_msr` | **Yes** | Thread trap 3/4 | Managed/native MSR lists plus MSR cache; TSC MSR `0x10` stays intercepted and is emulated. |
| `hv_vcpu_enable_managed_msr` | **Yes** | Thread trap 3/4 | Same path; labels corrected in section 7 item 1. |
| `hv_vcpu_set_msr_access` | **Yes** | Thread trap 10 | Toggles MSRPM read/write intercepts. |
| `hv_vcpu_read_msr` | **Yes** | Framework + MSR cache | Native MSRs read from VMCB save area or emulated table. |
| `hv_vcpu_write_msr` | **Yes** | Framework + MSR cache | `WRMSR 10h` emulated against `tscOffset` so a guest cannot move the host TSC. |
| `hv_vcpu_flush` | **No (by design)** | Framework | Returns `HV_UNSUPPORTED`; Apple documents it as deprecated/no-effect. Use `hv_vcpu_invalidate_tlb`. |
| `hv_vcpu_set_tsc_relative` | **Yes** | Shared page flag `0x100` | Guest TSC = `hv_tsc_clock() + relative`; VMCB offset = `(adjust - base) + relative` (`TscOffset.hpp`). |

### 1.4 VMCS and capability (10)

| API | Status | Answered by | What AMDHV does on Ryzen |
| --- | --- | --- | --- |
| `hv_vmx_vcpu_read_vmcs` | **Yes** | Thread trap 7 | Reads the VMCS shadow (`VmcsState.hpp`) that maps Intel encodings to VMCB fields. |
| `hv_vmx_vcpu_write_vmcs` | **Yes** | Framework → shadow | Writes are validated against advertised control words. |
| `hv_vmx_vcpu_get_cap_write_vmcs` | **Yes** | Thread trap 12 | Answers from `VmcsCaps.hpp` capability words. |
| `hv_vmx_vcpu_read_shadow_vmcs` | **Yes** | Framework | Client-side store; SVM has no shadow VMCS in hardware. |
| `hv_vmx_vcpu_write_shadow_vmcs` | **Yes** | Framework | Round-trips the client's value; no core state needed. |
| `hv_vmx_vcpu_set_shadow_access` | **No-op** | Thread trap 8 | Returns success; no shadow VMCS to configure on SVM. |
| `hv_vmx_read_capability` | **Yes** | Framework | Capability words derived from what the kext really implements. |
| `hv_vmx_get_msr_info` | **Yes** | Framework | Returns 0 for Intel-only `IA32_ARCH_CAPABILITIES`. |
| `hv_vmx_vcpu_set_apic_address` | **No-op** | Thread trap 9 | Success; TPR kept in `V_TPR`, APIC page never read. |
| `hv_vmx_vcpu_set_apic_address_space` | **No-op** | Framework | Success; same reasoning as `set_apic_address`. |

### 1.5 APIC family (33) — **opt-in**

Default VM: all APIC calls answer `HV_UNSUPPORTED`. With boot-arg `amdhv_apic=1`,
`hv_vm_create(HV_VM_ACCEL_APIC)` is accepted and `hvf-probe --apic` reports **27 ok** out of 32 APIC
calls. The five `hv_vcpu_exit_*` helpers still return `HV_ERROR`.

| API | Status (default → opt-in) | Answered by | What AMDHV does |
| --- | --- | --- | --- |
| `hv_vm_lapic_set_intr` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Sets guest LAPIC IRR; delivered by VM-entry event injection. |
| `hv_vm_lapic_msi` | `HV_UNSUPPORTED` → **No-op** | Trap 11 | Success; MSI routing into the guest is not emulated. |
| `hv_vm_ioapic_assert_irq` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Real IOAPIC model (`ApicState.hpp`). |
| `hv_vm_ioapic_deassert_irq` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Real IOAPIC model. |
| `hv_vm_ioapic_pulse_irq` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Assert + deassert. |
| `hv_vm_ioapic_read` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Reads IOAPIC register. |
| `hv_vm_ioapic_write` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Writes IOAPIC register. |
| `hv_vm_ioapic_get_state` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | SDK ext layout, state at `+8` (fixed from `+4`). |
| `hv_vm_ioapic_put_state` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | SDK ext layout, state at `+8`. |
| `hv_vm_send_ioapic_intr` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Feeds the IOAPIC redirect path. |
| `hv_vm_atpic_assert_irq` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Real 8259A model. |
| `hv_vm_atpic_deassert_irq` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Real 8259A model. |
| `hv_vm_atpic_port_read` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Reads PIC port/mask. |
| `hv_vm_atpic_port_write` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | Writes PIC port/mask. |
| `hv_vm_atpic_get_state` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | SDK 8259A state layout. |
| `hv_vm_atpic_put_state` | `HV_UNSUPPORTED` → **Yes** | Trap 10 | SDK 8259A state layout. |
| `hv_vm_set_apic_bus_freq` | `HV_UNSUPPORTED` → **No-op** | Trap 10 | Success; no APIC timer emulation. |
| `hv_vcpu_inject_extint` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Decoded but not injected into the guest. |
| `hv_vcpu_apic_read` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Private encoding not decoded; read returns 0. |
| `hv_vcpu_apic_write` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Private encoding not decoded; no side effect. |
| `hv_vcpu_apic_get_state` | `HV_UNSUPPORTED` → **Yes** | Thread trap 13 | Real LAPIC state, SDK layout, `copyout` from a kernel buffer. |
| `hv_vcpu_apic_put_state` | `HV_UNSUPPORTED` → **Yes** | Thread trap 13 | Real LAPIC state, `copyin` into a kernel buffer. |
| `hv_vcpu_exit_info` | **No** | Framework | Read-only page is zero: no `HV_VM_EXITINFO_*` code is published. |
| `hv_vcpu_exit_init_ap` | **No** (`HV_ERROR`) | Framework | Needs `HV_VM_EXITINFO_INIT_AP`; never produced. |
| `hv_vcpu_exit_startup_ap` | **No** (`HV_ERROR`) | Framework | Needs `HV_VM_EXITINFO_STARTUP_AP`; never produced. |
| `hv_vcpu_exit_ioapic_eoi` | **No** (`HV_ERROR`) | Framework | Needs `HV_VM_EXITINFO_IOAPIC_EOI`; never produced. |
| `hv_vcpu_exit_apic_access_read` | **No** (`HV_ERROR`) | Framework | Needs `HV_VM_EXITINFO_APIC_ACCESS_READ`; never produced. |
| `hv_vcpu_exit_inject_excp` | **No** (`HV_ERROR`) | Framework | Needs `HV_VM_EXITINFO_INJECT_EXCP`; never produced. |
| `hv_vcpu_apic_lsc_enter_r32` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Decoded and accepted; no deeper effect. |
| `hv_vcpu_apic_lsc_enter_imm32` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Decoded and accepted; no deeper effect. |
| `hv_vcpu_apic_lsc_invalidate` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Decoded and accepted; no deeper effect. |
| `hv_vcpu_apic_ctrl` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Decoded and accepted; no deeper effect. |
| `hv_vcpu_apic_trigger_lvt` | `HV_UNSUPPORTED` → **No-op** | Thread trap 13 | Decoded and accepted; no deeper effect. |

Note: the five exit helpers are **not** merely "rejected by userspace". The root cause is that the
core never publishes `HV_VM_EXITINFO_INIT_AP`, `...STARTUP_AP`, `...IOAPIC_EOI`,
`...APIC_ACCESS_READ` or `...INJECT_EXCP`; the framework helpers therefore have no exit payload.

### 1.6 PIO notifier (2)

| API | Status | Answered by | What AMDHV does |
| --- | --- | --- | --- |
| `hv_vm_add_pio_notifier` | **No** | Stub (task 8) | Logged, returns `HV_UNSUPPORTED` (`0xfae9400f`) in normal mode; mode 2 returns success for debugging. VZ uses I/O exits instead. |
| `hv_vm_remove_pio_notifier` | **No** | Stub (task 9) | Same stub behaviour. |

### 1.7 Notes for API developers

| Topic | Note |
| --- | --- |
| Entitlements | Apple requires `com.apple.security.hypervisor`. AMDHV native mode does **not** check it: an unsigned process as uid 501 created a VM and vCPU in the validation run. The repo tools are still signed with the entitlement for Apple's framework. |
| `hv_vm_destroy` | Client-visible answer with a live vCPU is `HV_ERROR`, not `HV_BUSY`; call after the last `hv_vcpu_destroy`. |
| Limits | 64 vCPUs (65th → `HV_ERROR`), 64 address spaces (65th → `HV_NO_RESOURCES`), 16 GiB per single map. |
| `HV_CAP_ADDRSPACEMAX` | Verified directly on this host: success, value 64. It is framework-side, not a kext trap. |
| Shared-page helpers | Register/MSR/VMCS/FP queries do not reach the kernel; look at `Frontend.cpp` flag bits (`0x01` slots, `0x04` MSRs, `0x40` DRs, `0x80` FP, `0x100` TSC). |

---

## 2. AMD Ryzen / SVM feature matrix

### 2.1 Enablement and host state

| Feature | Status | Implementation / developer note |
| --- | --- | --- |
| AMD vendor + SVM CPUID (`8000_0001:ECX[2]`) | **Yes** | `SVM.cpp::probeThisCpu()`. |
| BIOS SVM enabled (`VM_CR.SVMDIS` clear) | **Yes** | `cpuUsable()` refuses a CPU with `SVMDIS`; locked `VM_CR` is reported, never fought. |
| `EFER.SVME` on every usable CPU | **Yes** | `enableAction()` / `disableAction()`; `ensureThisCpu()` repairs drift before each VMRUN. |
| `VM_HSAVE_PA` | **Yes** | `allocHostSave()`: 4 KiB, contiguous, aligned, below 2^52; `kIOMemoryMapperNone` avoids the boot-time mapper wait. |
| Late CPUs | **Yes** | `rescan()` enables CPUs that come online after kext start. |
| Self-test | **Yes** | `verifyAll()`; `amd-info selftest` → `12 of 12 CPUs ok`. |
| NPT + NRIPS required | **Yes** | VM creation refuses a CPU without both. |
| 64 vCPUs / 64 logical CPUs | **Yes / Partial** | `AMDHV_MAX_VCPUS_PER_VM=64`; per-CPU tables sized for 64, but this host exercises only 12 (`cpu_number() >= 64` is skipped). |
| GIF discipline | **Yes** | `SvmRun.S`: `CLGI` … `STGI` around VMSAVE/VMLOAD/VMRUN. |
| Host interrupts during VMRUN | **Yes** | `STI` before VMRUN with `V_INTR_MASKING`; `CLI` after exit. |
| S3/S4 with live guest | **Partial** | Ordering fix and `sleep_policy_test` pass; real S3 acceptance run still pending. |

### 2.2 VMCB, memory and guest state

| Feature | Status | Implementation / developer note |
| --- | --- | --- |
| VMCB layout (App.B) | **Yes** | `VMCB.hpp` static_asserts: intercept words `000/004/008/00C`, clean `0xC0`, nRIP `0xC8`, insn bytes `0xD0`. |
| VMCB clean bits | **Yes** | `CleanBits.hpp`; only enabled when CPUID reports `VMCB_CLEAN`. `INT_CTL` and `CR2` stay dirty. |
| Always-on intercepts | **Yes** | INTR, NMI, SMI, INIT, CPUID, INVD, SHUTDOWN, I/O, MSR, VMRUN, VMMCALL, VMLOAD, VMSAVE, STGI, CLGI, SKINIT. |
| Client-driven intercepts | **Yes** | HLT, INVLPG, RDPMC, RDTSC(P), PAUSE, MWAIT, MONITOR, WBINVD, CR0/3/4/8, MOV-DR, exception bitmap, interrupt window. |
| PAUSE filter | **Yes** | Count 3000, threshold 128, only when CPUID `PauseFilter`/`PauseThresh` are present. |
| I/O / MSR permission maps | **Yes** | 12 KiB / 8 KiB all-ones; every port/MSR exits to the client; native MSRs clear their bits. |
| `V_INTR_MASKING`, `V_TPR`, `V_IRQ` | **Yes** | TPR shadow with CR8 exit path; interrupt-window injection. |
| Event injection / re-injection | **Yes** | `eventInj` + `eventInjErr`; external interrupt and NMI exits re-injected. |
| ASID + TLB | **Yes** | One ASID per vCPU; `TLB_FLUSH_BY_ASID` only when translations can be stale; `TLB_FLUSH_ALL` fallback. |
| Nested paging | **Yes (4 KiB only)** | 4-level walk, `US`/`NX`, permissions AND across levels. |
| NPF exit info | **Yes** | GPA only used when DecodeAssists `EXITINFO1[32]` says `EXITINFO2` is valid. |
| Guest state save/restore | **Yes** | `VMSAVE host → VMLOAD guest → VMRUN → VMSAVE guest → VMLOAD host`. |
| Guest EFER, PAT, DR6/DR7, DR0–DR3, `TSC_AUX` | **Yes** | DR0–DR3 manually saved/restored around VMRUN (`VmRun.cpp`); `TSC_AUX` swapped because VMRUN does not. |
| x87/SSE/AVX + XCR0 | **Yes** | XSAVE areas swapped by hand; PKU bits masked on CPUID leaf 7 because macOS never enables PKRU. |
| Guest TSC offset / `WRMSR 10h` | **Yes** | `VMCB.tscOffset`; `WRMSR 10h` emulated so the guest cannot write the host TSC. |
| NRIPS / `VMEXIT_INVALID` diagnostics | **Yes** | NRIPS required at creation; `dumpVmcb()` prints the rejected state. |
| Run deadline / kick | **Yes** | 10 ms slice + client deadline; `hv_vcpu_interrupt` sends an IPI into VMRUN. |

### 2.3 AMD features not implemented

These are gaps against the **specification**, not against the clients in section 3. None of them is
reachable through the calls VZ/QEMU/VirtualBox/Android make on this host.

| APM section | Feature | Status | Why / developer note |
| --- | --- | --- | --- |
| 15.15/15.16 | TLB flush 0x07 (non-global) | **No** | Only `0x01`/`0x03` emitted; per-vCPU ASID makes it unnecessary. |
| 15.21 | NMI virtualization | **No** | No `V_NMI` control; physical NMI exits go to the client. |
| 15.22/15.32 | SMM, SMM key | **No** | SMI is intercepted; no in-kernel SMM state machine and no HVF API for one. |
| 15.23/15.38 | LBR virtualization, IBS | **No** | LBRV bit reported but unused; IBS not probed. |
| 15.24 | DEV / device exclusion | **n/a** | HVF has no IOMMU surface; devices live in the client. |
| 15.25 | GMET, shadow stacks, INVLPGB, vector-5 intercepts | **No** | No CPUID probe or VMCB control write. |
| 15.27/15.28/15.31 | SKINIT, #SX, SVM key | **No** | Intercept reported to client; no secure loader. |
| 15.29 | AVIC / x2AVIC | **No** | Feature bit reported but unused. The in-kernel APIC path is opt-in, but AVIC itself is not enabled. |
| 15.30 | `TscRateMsr` / TSC scaling | **No** | Guest TSC uses offset only. |
| 15.33 | Nested virtualization (VGIF, vVMSAVE/VMLOAD) | **No** | Host keeps CLGI/STGI; no nested VMRUN path. |
| 15.34–15.36 | SEV / SEV-ES / SEV-SNP | **No** | No CPUID `8000_001F`, key management, GHCB or RMP handling; Apple exposes no API. |
| 15.37 | `SPEC_CTRL` virtualization | **No** | Intel MSRs emulated as unsupported; AMD VMCB save field at `0x2E0` reserved. |
| NPT | 2 MiB / 1 GiB leaves, execute-only, A/D bits | **No** | `encodeLeaf()` never sets PS; not advertised in capability words. |
| 2.2 | `TSCRATE`, `LBRV`, `SVML`, `VGIF`, `vVMSAVE/VMLOAD` | **No** | Probed in `AMDHVInfo.svmFeatures`, no enabling code path. |

---

## 3. Client compatibility

| Client | Needs | Status | Evidence |
| --- | --- | --- | --- |
| Docker Desktop (VZ) | vm_create, map, vCPU, `run_until`, VMCS reads, interrupt | **Yes** | engine starts; containers serve traffic. |
| Kiri (VZ) | same | **Yes** | Linux/amd64 containers run. |
| colima `--vm-type vz` | same | **Yes** | 4 vCPU / 8 GiB VM starts; `docker run hello-world` succeeds. |
| VirtualBox 7.2.22 | VMX controls, NPT, event injection, TPR shadow | **Yes** | Windows 10 boots to desktop; Lubuntu live reaches installer. |
| Android emulator 37.3.10 | VMX controls, NPT, FPU/XSAVE | **Yes** | Android 17 AVD boots; ADB/input/network work. |
| QEMU 11.1.3 `-accel hvf` (Linux guest) | VMX controls, NPT | **Yes** | Lubuntu boots with `tsc=reliable`. |
| QEMU 11.1.3 `-accel hvf` (Windows guest) | OVMF timing, Win11 installer gates | **Verified** | Windows 10/11 reach Setup; press CD boot key; Win11 needs `LabConfig` (HVF refuses SMM). |

Single-host observations, not a support promise. See `docs/CLIENTS.md`.

---

## 4. Limit register and open work

| Priority | Item | Status | Developer note |
| --- | --- | --- | --- |
| P0 | `waitForSystemMapper` at boot | **Done** | All physical allocations pass `kIOMemoryMapperNone`. |
| P0 | Sleep/wake with live VM | **Code done, run pending** | Suspend state set before rendezvous; sleep epoch forces clean-bit/TLB reload. Real S3 still untested. |
| P0 | Guest `WRMSR 10h` cannot move host TSC | **Done** | MSRPM intercept kept; access emulated against `tscOffset`. |
| P1 | VMCB clean bits | **Done** | No full state reload after every VMRUN. |
| P1 | Per-vCPU ASID + `FLUSH_BY_ASID` | **Done** | `amdhv_asid=0` restores flush-every-entry for A/B. |
| P1 | DecodeAssists | **Done (partial)** | GPR for CR/DR exit, GPA for NPF; INTn/INVLPG assists unused. |
| P1 | PAUSE filter threshold | **Done** | 3000 / 128, CPUID-gated. |
| P1 | 2 MiB NPT leaves | **Deferred** | Needs physically contiguous 2 MiB windows; anonymous user memory rarely provides them. |
| P2 | `hv_vm_space_*` family | **Done** | 64 spaces, real NPT roots, `HV_BUSY` while bound. |
| P2 | PIO notifier | **Open** | Needs notifier registry + mach-port notification; VZ does not use it. |
| P2 | Full APIC semantics | **Open** | `inject_extint` and most sub-ops still no-ops; opt-in only. |
| P2 | `hv_vcpu_flush` | **Closed by design** | Apple deprecated it; use `hv_vcpu_invalidate_tlb`. |
| P3 | AVIC, VGIF, LBRV, SVML, TSC scaling, large NPT leaves | **Open** | Would change the model; not reachable from current clients. |

Acceptance for any change: host test + `hvf-probe` (if API surface changes) + one client run +
`amd-info stats` with `vmInvalidExits = 0` and `assertFailures = 0`.

---

## 5. Verification commands and recorded evidence

| Command | Purpose |
| --- | --- |
| `make test` | 20 host tests; runs anywhere. |
| `make kext tools` | builds `AMDHV.kext`, `amd-info`, `hvf-smoke`, `hvf-probe`. |
| `sudo build/amd-info selftest` | verifies `EFER.SVME` / `VM_HSAVE_PA` on every CPU. |
| `sudo build/amd-info stats` | `enableFailures`, `vmInvalidExits`, `assertFailures`, `vmFpuMismatches` must stay 0. |
| `./build/hvf-smoke` | end-to-end public-API VMM. |
| `./build/hvf-probe --matrix` | all 79 x86 API answers; compare with `docs/CAPABILITIES.md`. |
| `sysctl kern.hv_support` | must print 1 after registration. |

A kext change needs a reboot; the running kext cannot be replaced in place.

| Recorded run | Result | Note |
| --- | --- | --- |
| Post-reboot 2026-11-22 | `hvf-smoke` PASS, `hvf-probe` matches matrix | no `waitForSystemMapper` warning; counters clean. |
| VZ guest, 32 min, 6 vCPU / 4 GiB | 35,985,555 VM runs, 36,748,682 exits, 30.5M NPF exits | `vmInvalidExits = 0`, `assertFailures = 0`; EPT-violation path is the hot path. |
| Limitation validation 2026-11-23 (0.2.0) | 64 vCPUs, 64 spaces, 16 GiB map cap, 2 concurrent VMs | unsigned process created a VM; PIO/APIC/extint answered `HV_UNSUPPORTED`. |
| S3/S4 attempts | host never reached S3 | `sleepEvents = 0`, `wakeEvents = 0`; not a kext fault. Acceptance still pending. |

Open issues and their root-cause notes (thread-trap labels, `waitForSystemMapper`, QEMU boot key,
host RTC, TSC write hazard, ASID/clean-bit/space ABI checks, S3 procedure) are kept in the git
history of this file and in `docs/CLIENTS.md`, `docs/QEMU.md`, `docs/APIC_PENDING.md`.

---

## Sources

- Apple, *Hypervisor.framework*: <https://developer.apple.com/documentation/hypervisor>
  (machine-readable: <https://developer.apple.com/tutorials/data/documentation/hypervisor.json>).
- Apple, *Intel-based Mac*: <https://developer.apple.com/documentation/hypervisor/intel-based-mac>.
- Apple, `HV_VM_ACCEL_APIC`: <https://developer.apple.com/documentation/hypervisor/hv_vm_accel_apic>;
  value `1<<10` and `hv_vcpu_flush` deprecation from the macOS 15.5 SDK headers
  `hv_types.h` and `hv.h`.
- Apple, *Virtualization*: <https://developer.apple.com/documentation/virtualization>.
- Apple, xnu `hv_support_kext.h`: <https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/osfmk/kern/hv_support_kext.h>.
  Forward check: identical ABI at <https://github.com/apple-oss-distributions/xnu/blob/xnu-12377.121.6/osfmk/kern/hv_support_kext.h>;
  that tag only changes `IKOT_UNKNOWN` → `IOT_ANY` in `hv_io_notifier.c`, which AMDHV does not call.
- AMD, *AMD64 APM Vol.2*, pub. 24593, rev. 3.46 (Oct 2026), Ch.15 + App.B:
  <https://docs.amd.com/go/en-US/24593_3.46_APM_Vol2>.
- VMCB cross-checks: Linux `arch/x86/include/asm/svm.h`, FreeBSD `sys/amd64/vmm/amd/vmcb.h`.
- QEMU HVF on x86: <https://www.qemu.org/docs/master/system/introduction.html>,
  `accel/hvf/hvf-all.c`, `target/i386/hvf/hvf.c`.
- This repository: `docs/CAPABILITIES.md`, `docs/CLIENTS.md`, `docs/APIC_PENDING.md`,
  `kext/Source/core/Frontend.cpp`, `kext/Source/svm/VMCB.hpp`, `kext/Source/memory/Npt.hpp`,
  `kext/Source/vmcs/VmcsState.hpp`, `kext/Source/vmcs/VmcsCaps.hpp`.
