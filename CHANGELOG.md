# Changelog

All notable changes to AMDHV are documented here. Format follows Keep a Changelog; versions follow
semantic versioning. The kext version lives in `kext/Info.plist` and should match the tag.

## [Unreleased] — v0.3.0 (planned, not tagged)

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
