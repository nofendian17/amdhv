# Changelog

All notable changes to AMDHV are documented here. Format follows Keep a Changelog; versions follow
semantic versioning. The `v*` git tag is the release version: `kext/Makefile` stamps it into the
built `Info.plist`. The value committed in `kext/Info.plist` is only a fallback for source archives
made with `git archive`, which have no tag metadata.

## [0.5.0] — 2026-11-27

### Added
- SVM DecodeAssists now supplies the faulting instruction bytes for in-kernel APIC MMIO emulation.
  The APIC path therefore also works when guest paging is enabled; CPUs without DecodeAssists keep
  the previous fallback.

### Changed
- The FPU round-trip self-check is now opt-in via `amdhv_fpu_check=1` (default off). It previously
  added an extra XSAVE and a 512-byte compare to every VM exit; the host/guest FPU swap itself is
  unchanged.

### Fixed
- APIC model state is serialized between task traps 10/11 and `vcpuRun()` with `vm->lock`. The
  unsynchronized read-modify-write on LAPIC IRR/ISR and the IOAPIC/ATPIC models could lose or
  duplicate an interrupt.
- `fKick`, `fRunCpu` and the runtime-writable globals (`gTprShadow`, `gMode`, `gRegistered`,
  `gLevel`, `gIoLogLevel`, `gActive`) now use atomic accesses instead of plain `volatile`.
- Removed a redundant instruction-length check in the DecodeAssists decoder.

## [0.4.0] — 2026-11-26

### Added
- `hv_vm_lapic_set_intr`, `hv_vm_send_ioapic_intr` and `hv_vm_lapic_msi` now really deliver interrupts
  to guest LAPICs. Their framework wire formats are decoded in `ApicState.hpp` and covered by host
  tests; `--apic-inject` drives all three to real guest handlers on hardware.
- `hv_vm_add_pio_notifier` / `hv_vm_remove_pio_notifier` are implemented with xnu's own
  `hv_io_notifier_grp_*` API instead of returning `HV_UNSUPPORTED`. A matching guest `OUT` sends an
  `hv_ion_message_t` and is suppressed. The client must hold a **send right** on the Mach port.
- The vCPU read-only page now publishes `HV_VM_EXITINFO_*` when the in-kernel APIC handles a
  matching event. `hv_vcpu_exit_info`, `hv_vcpu_exit_apic_access_read` and
  `hv_vcpu_exit_ioapic_eoi` are live-verified via `hvf-probe --exitinfo-mmio`. With no event all six
  exit helpers still answer `HV_ERROR`, as on Apple.

### Changed
- Task trap slots 8/9 are real PIO-notifier handlers, so the provider has 28 real trap handlers and
  four logging stubs. The kext imports 398 kernel symbols; `make test` runs 22 host tests.
- Compatibility records now distinguish "probed/answered" from "implemented", and report the
  loaded kext identity and the still-unprobed INIT/SIPI/#GP exit-info paths honestly.

### Fixed
- The two task-trap-10 opcodes were reversed: `0x6a` was a no-op and `0x73` read the vector from an
  opaque framework cookie. MSI was a separate no-op. IRR delivery now targets the decoded vCPU, with
  `0xff`/redirection hint reaching all vCPUs.
- Real-mode APIC MMIO was mis-decoded: `decodeMmio()` now accepts both `0x66` and `0x67` prefixes
  with an 8-byte instruction fetch, and `guestBytes()` adds the intra-page offset that the 4 KiB
  NPT lookup masks away.
- Nested-page-fault instruction length now comes from VMCB `0xD0`; AMD APM §15.7.1 makes `nRIP` zero
  for NPF, so the old length was reported as 0.

## [0.3.3] — 2026-11-26

### Fixed
- `hv_vm_sync_tsc` and the run loop share a TSC lock, a VM-wide sync generation and a per-vCPU
  applied generation. A sync that lands before, during or between runs reaches every vCPU on its
  next run and wins over a stale `set_tsc_relative` request. `tsc_offset_test` covers the decision;
  a post-reboot probe confirmed the VMCB offset changes from 0 to the sync value for a vCPU created
  before the sync.
- `setSpace()` holds `fRw` across the NPT-root lookup and bind, and `tVmDestroy` claims destruction
  under `vm->lock`; a second destroy and traps on a dying VM are refused. `tVmSyncTsc` and
  `tVcpuInterrupt` now pin the VM.
- Teardown keeps SVM on while live VMs exist; `disableAll()` sets a `gStopping` gate that new guest
  entries check before host save pages are freed. The PM notifier is removed before teardown and
  `super::stop()` is always called.
- The sleep epoch and flag are rechecked immediately before VMRUN, with the suspend flag now atomic;
  `rescan()` allocates host-save pages while suspended and `enableAll()` retries a partial enable.
- STI is moved out of VMRUN's interrupt shadow; TPR and CR2 page writes are applied before the
  NMI-window early return. VMCB clean bits and the TLB are reloaded when a full suspend/resume
  completes while a thread is frozen.
- `tVmCreate()` maps its real `IOReturn`, `managedOff()` keeps managed-MSR state intact when the
  core refuses a disable, and the user client rejects 64-bit scalars before truncating selectors.
- The in-kernel APIC MMIO shortcut is skipped when guest CR0.PG is set: VMCB `save.rip` is linear
  while `guestBytes()` takes a guest physical address.
- `tApicSubOp` decodes the sub-op tag from the low byte of word 0, so
  `hv_vcpu_apic_get_state`/`put_state` work on macOS 15.8.3 (`0x9003`/`0x9004` instead of
  `0x0003`/`0x0004`). Verified on hardware after the reboot: `--apic` reports 27 ok.
- Intercepted SMI (`0x62`) maps to VMX reason 6 (`OTHER_SMI`). VMX software exceptions now map type 5
  (#BP/#OF) to SVM type 3 (APM 15.20 Table 15-11); type 6 (ICEBP) keeps the INTn skip.
- VM creation refuses a host with `EFER.NXE` clear (APM 15.25.8), and SVM enablement reads EFER back
  after setting SVME (APM 15.30.1 makes SVME an MBZ write while `SVMDIS` is set): a CPU that ignored
  the write is not counted active and guest entry is refused instead of executing CLGI without SVME.
- `mustIntercept()` keeps every MSR the AMD core does not model intercepted, so a forwarded request
  for an Intel-only MSR cannot clear the MSRPM bits and make a Zen guest #GP. macOS 15.8.3 itself
  rejects those indices with `HV_BAD_ARGUMENT`, so the guard is defensive for forwarded calls.
- `AMDHV_MAX_CPUS` stays at 64: the kick IPI uses a 64-bit CPU mask, so raising the limit would
  silently drop `hv_vcpu_interrupt` on CPU 64+. `hv_api_constants_test` pins both CPU limits.

### Documentation
- `COMPATIBILITY_CHECKLIST.md` records the post-reboot validation on this tree: 20/20 host tests,
  clean static analysis, `hvf-smoke` PASS, `--matrix` 79/79, `--apic` 27 ok, `amd-info selftest`
  12/12 and zero failure counters. Paths that need conditions not reproducible from userspace are
  marked as untested rather than pending reboot.

## [0.3.2] — 2026-11-25

### Added
- `hv_api_constants_test` and `hv_kext_abi_test` guard the documented `HV_VM_ACCEL_APIC` bit and
  the xnu callback layout. `make test` now runs 20 host tests.

### Changed
- The kext reads the APIC-acceleration flag from a documented constant and declares the xnu
  callback struct from a shared header instead of repeating version-sensitive values inline. No
  runtime behaviour changes; APIC remains opt-in via `amdhv_apic=1`.
- `COMPATIBILITY_CHECKLIST.md` is now a table-driven developer reference listing all 79 x86
  Hypervisor.framework APIs and the AMD/SVM features with status and explanation.

### Fixed
- Documentation now reports the measured APIC result (`27 ok` of 32) and the real cause of the five
  `hv_vcpu_exit_*` `HV_ERROR` answers: AMDHV publishes no `HV_VM_EXITINFO_*` code for those helpers
  to decode. Test and version counts match the current tree.

## [0.3.1] — 2026-11-23

### Changed
- Kext sources are grouped by responsibility and `Vm.cpp` is split into vCPU, run-loop and
  shared-state modules. The tools share one naming style and document every function.
- The `v*` git tag is now the release version; `kext/Makefile` stamps it into the built
  `Info.plist`. Releases include `AMDHV-<version>-macos-x86_64.zip` so OCAT can find and unzip the
  kext for update checks.
- CI adds clang static analysis, plist validation and downloadable artifacts.

### Fixed
- The public ring header no longer divides by zero when initialised with size 0; added a host test.

### Documentation
- Added ADRs for the source layout and host-testing strategy, documented the OCAT setup, and
  corrected stale version and merge-status claims.

## [0.3.0] — 2026-11-23

### Added
- `hv_vcpu_set_tsc_relative` now applies the guest TSC offset through `TscOffset.hpp`.
- `hv_vmx_vcpu_set_shadow_access` is a compatibility no-op handler instead of a stub.
- In-kernel APIC foundation: `ApicState.hpp` models LAPIC/IOAPIC/8259A state with SDK-compatible
  layouts; task traps 10/11 and the per-vCPU thread-13 sub-ops are wired.
- Guest interrupt delivery for APIC VMs: 8259A/IOAPIC requests feed LAPIC IRR and are injected
  through VM-entry event injection before VMRUN.
- EOI handling and an NPF fallback that emulates absolute MOV accesses to the LAPIC and I/O APIC
  pages (`ApicMmio.hpp`), guarded by an exact decoded-address check.
- `hvf-probe --matrix`, `--apic`, `--apic-run`, `--apic-mmio` and new host tests
  (`tsc_offset_test`, `apic_test`, `apic_mmio_test`).
- `amdhv_apic=1` boot-arg documentation and a release runbook (`docs/SHIPPING.md`).

### Changed
- `hvf-probe` now measures all 79 exported x86 Hypervisor.framework calls; no API is "unverified".
- `HV_VM_ACCEL_APIC` is opt-in via `amdhv_apic=1` (default off), so existing clients keep the
  previously validated path.
- Internal: kext sources are grouped by responsibility and `Vm.cpp` is split into vCPU, run-loop
  and shared-state modules. Host-test coverage is now measurable with `make coverage` (99.24% of
  host-testable headers). No ABI or runtime behaviour change; rationale in `docs/decisions/`.

### Fixed
- SMAP panic in LAPIC `get_state`: the ext struct is built in a kernel buffer and copied with
  `copyout`/`copyin`.
- IOAPIC state used the wrong arg word and the wrong struct offset (`state` starts at `+8`).
- Thread-13 state tags decoded as `0xe003`/`0xe004`; real-mode MMIO instructions with `0x66`
  operand-size prefix decoded correctly.

### Known gaps (not shipped as supported)
- Five `hv_vcpu_exit_*` helpers still answer `HV_ERROR`; they never reach the kext and need a
  userspace interpose shim.
- `hv_vcpu_apic_read`/`write` are accepted as no-ops; their private argument encoding is not decoded.
- Sleep/wake with a live guest is code-fixed but not hardware-validated (S3/S4).
- Full AMD APM Vol. 2 Ch. 15 / App. B coverage is not claimed.

## [0.2.0] — 2026-11-22

- Sleep/wake fix with VM-local state and suspend epoch; released from tag `v0.2.0`.
- Full documentation of API and APM limitations (`COMPATIBILITY_CHECKLIST.md`, `CAPABILITIES.md`).

## [0.1.2] — 2026-11-22

- Rebuilt kext installed and validated across VZ, Docker Desktop, VirtualBox, Android emulator and
  QEMU Linux; capability words derived from implemented controls.

## [0.1.0] — 2026-11-14

- First public release: SVM core, nested paging, Hypervisor.framework trap registration.
