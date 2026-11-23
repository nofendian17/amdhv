# Shipping AMDHV (release runbook)

This runbook applies the shipping-and-launch checklist to a kernel extension. AMDHV is not a web
service: there is no canary fleet or health endpoint. The equivalents are boot-args (feature flags),
`amd-info` counters (monitoring), and a known-good EFI (rollback). **Nothing here is a deployment
action by itself** — each stage needs a human decision.

## 1. Current state (evidence, not intent)

| Gate | Evidence | Status |
| --- | --- | --- |
| Host tests | `make test`: 18/18 (`host_test` … `apic_mmio_test`) | Green |
| Kext build | `make kext`, `check-kext-symbols`: 393/393 symbols resolve | Green |
| Public API probe | `hvf-probe`: 79 of 79 functions observed; `--matrix`: 79 of 79 | Green |
| APIC surface | `--apic`: 31 `ok`; five `hv_vcpu_exit_*` `HV_ERROR` (userspace-only, documented) | Known gap |
| Guest delivery | `--apic-run`: IRQ0 → vector `0x30`, exit at `RIP 0x1200` | Green |
| Smoke | `hvf-smoke`: `PASS` | Green |
| Host sanity | `amd-info info`: SVM active 12/12, 32768 ASIDs; `selftest`: 12 of 12 CPUs ok | Green |
| Failure counters | `vmInvalidExits`, `assertFailures`, `enableFailures`, `vmFpuMismatches` = 0 | Green |
| Code review | APIC work merged via PR #6 (`release/v0.3.0`); this refactor branch is still pending review | **Open** |
| Client re-test after APIC work | Recorded 2026-11-23 on the installed 0.3.0 kext — see §7 (VZ/Kiri pass; VirtualBox/QEMU/Android blocked by missing local binaries) | **Partial** |
| Changelog / release tag | `CHANGELOG.md` has a 0.3.0 section matching `kext/Info.plist`; annotated tag `v0.3.0` points at `c096121` on `main`. GitHub release not verifiable from the repo. | **Partial** |

The `v*` tag is the source of truth for the version. `kext/Makefile` writes it into the built
`Info.plist`; the value committed in `kext/Info.plist` is only a fallback for source archives
(`git archive` has no `.git`). Tagging needs no manual version edit.

## 2. Feature flags (boot-args)

| Flag | Default | Purpose | Owner / expiry |
| --- | --- | --- | --- |
| `amdhv_enable_svm=1`, `amdhv_hv=1` | off | Required to run at all | Permanent config |
| `amdhv_apic=1` | **off** | Opt in to `hv_vm_create(HV_VM_ACCEL_APIC)` and the in-kernel APIC path | Keep opt-in until APIC semantics are complete; review at v0.4.0 |
| `amdhv_tpr_shadow=0` | offered | Hide TPR shadow from guests | Debug only |
| `amdhv_asid=0` | on | Disable per-vCPU ASIDs, full flushes | Debug only |
| `amdhv_hv_sysctl=0` | set to 1 | Leave `kern.hv_support` alone | Debug only |
| `-amdhv_off` | absent | Kill switch: kext loads but does nothing | Emergency, permanent |

Rule: ship code with `amdhv_apic=0` (default). The APIC path is a beta gate, not a default
rollout. Test both states in CI terms: the full probe runs with the default VM, `--apic*` runs with
the flag on.

## 3. Staged rollout

1. **Merge / tag**: `docs/limitation-validation`, `docs/boot-args-apic` and `release/v0.3.0` are
   already merged into `main`, the kext version is already **0.3.0**, and the annotated tag
   `v0.3.0` already points at `c096121`. Merge `refactor/structure-and-tools` (source layout,
   docs and tool clean-up; no behaviour change and no version bump needed).
2. **Baseline deploy (flag off)**: build the 0.3.0 kext, install to OpenCore with only the two
   required boot-args. Verify `hvf-smoke`, `hvf-probe`, `amd-info selftest`, counters, and the
   existing clients (VZ/Kiri, VirtualBox, Android, QEMU Linux). This is the production gate.
3. **Beta gate (flag on)**: on a small set of test hosts add `amdhv_apic=1`, run `--apic`,
   `--apic-run`, `--apic-mmio`, and one APIC-using client if available. Monitor for one boot cycle
   and one guest session.
4. **Full rollout**: only if baseline and beta are green. Keep `amdhv_apic` opt-in; "full" means all
   users can run 0.3.0, not that every host enables APIC.

Advance criteria: all failure counters remain 0, no new panic log, `hvf-smoke` PASS, probe 79/79.
Hold/investigate on any nonzero counter or a client regression. Roll back on a kernel panic or a
counter spiking above zero.

## 4. Monitoring and observability

- `amd-info stats`: `vmInvalidExits`, `assertFailures`, `enableFailures`, `vmFpuMismatches` must be 0.
- `amd-info log` / panic log: no new AMDHV frames after a guest run.
- `amd-info info`: SVM active on all logical CPUs after boot and after wake.
- Sleep/wake remains a documented untested path: check `sleepEvents` / `wakeEvents` only after a
  real S3 run; do not claim it from the code fix alone.

## 5. Rollback plan

**Triggers**: kernel panic; any failure counter > 0; a client that previously worked fails to boot;
S3 wake regression.

1. Immediate kill switch: remove `amdhv_apic=1` from `boot-args` (one reboot), or add `-amdhv_off`
   to disable the kext entirely (one reboot).
2. Kext rollback: install the `v0.2.0` kext from the GitHub release or rebuild it with
   `git checkout v0.2.0 && make kext && ./scripts/install-to-efi.sh <EFI>`. The installer keeps
   `config.plist.amdhv-backup`; keep a copy of the previous `AMDHV.kext` before replacing it.
3. Verify: `kmutil showloaded | grep AMDHV`, `amd-info selftest`, `hvf-smoke`, counters.

Time to rollback: flag change < 5 min including reboot; kext replacement < 15 min including reboot.
No data migration is involved — guest disks belong to the clients, not AMDHV.

## 6. Pre-launch sign-off

- [ ] Refactor PR merged into `main`; `v0.3.0` tag verified at `c096121`, GitHub release checked against `CHANGELOG.md`
- [ ] Baseline clients re-tested on the 0.3.0 build with APIC off
- [ ] Beta hosts tested with `amdhv_apic=1`
- [ ] Failure counters still 0 after one hour of real guest use
- [ ] Rollback kext (v0.2.0) and config backup verified present
- [ ] Documentation: README boot-args table, CAPABILITIES, COMPATIBILITY_CHECKLIST, APIC_PENDING

## 7. Client retest (2026-11-23, kext `FCF902DB`)

| Client | Result | Evidence |
| --- | --- | --- |
| VZ via Kiri | **Pass** | `kiri daemon` + `com.apple.Virtualization.VirtualMachine` running; 4 containers healthy; `kiri exec fg-proxy …` printed `Linux 6.18.58 x86_64`; `kiri run --rm alpine:latest …` printed `RUN_GUEST_OK`. `vmExits` advanced by ~250k across the two runs, `vmInvalidExits` stayed 0. |
| `kiri raw run` | **Excluded** | Failed before VM creation with `Invalid virtual machine configuration. The storage device attachment is invalid` (`vmCreates` unchanged) — a local Kiri config problem, not an AMDHV result. |
| VirtualBox | **Blocked** | `/usr/local/bin/VBoxManage` wrapper exists but `/Applications/VirtualBox.app` is gone. |
| QEMU `-accel hvf` | **Blocked** | No `qemu-system-x86_64` anywhere on this host. |
| Android emulator | **Blocked** | No emulator binary/AVD installed. |

`kern.hv_support=1`; failure counters remain 0; no new AMDHV panic since this boot (the only
recent panic file is the older SMAP one from 17:16, already fixed and documented).
