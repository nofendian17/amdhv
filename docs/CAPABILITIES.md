# What Virtualization.framework uses, and what AMDHV answers

This page records the public Hypervisor.framework surface a Virtualization.framework (VZ) client
can reach, what AMDHV implements today, and how to check a build without guessing from the kernel
log. `tools/hvf-probe` prints the same table on a live machine:

```bash
make tools
./build/hvf-probe          # needs the com.apple.security.hypervisor entitlement, already applied
```

The probe forks/execs itself for the four probes that need a task without a VM (the in-kernel APIC
variant, a second `hv_vm_create`, the vCPU count and the full API matrix). Everything else runs in
one task.

The enablement, SVM-core and framework checklist behind this matrix — including what is deliberately
not implemented and the gap register — is in
[COMPATIBILITY_CHECKLIST.md](COMPATIBILITY_CHECKLIST.md).

Version caveat: the probe records below were measured on the 2026-11-23 validation build (AMDHV
0.2.0). The current tree is **v0.3.3** (`CHANGELOG.md`, tag `v0.3.3`; `kext/Makefile` stamps that
tag into the built `Info.plist`, whose committed fallback is 0.3.0). The current tree's `make test`
runs 22 host tests and the kext imports 398 symbols; the older counts in the records below are from
the 2026-11-23 validation build.

## What VZ actually calls

A VZ VM on x86 is a userspace device model on top of a small hypervisor API. The live trap log of a
running VZ VM (`com.apple.Virtualization.VirtualMachine`, Kiri with six containers) shows only:

| trap | call | frequency |
| --- | --- | --- |
| task 12 | `hv_vcpu_create` | once per vCPU |
| thread 1 | `hv_vcpu_run_until` (`HV_DEADLINE_FOREVER`) | after every exit |
| thread 7 | `hv_vmx_vcpu_read_vmcs` (exit fields) | after every exit |
| task 13 | `hv_vcpu_interrupt` | when the device model injects an IRQ |

No in-kernel APIC, IOAPIC, PIO notifier or address-space call appears in that log. That is why the
remaining missing stubs below do not stop VZ today: VZ emulates the APIC, the timer and every device
itself and only needs memory mapping, vCPU state, VM exits and interrupt injection.

VZ uses `hv_vcpu_run_until`, not `hv_vcpu_run`: the live trap log shows `thread 1 vcpu_run_until` with
arg `0xffffffffffffffff` (`HV_DEADLINE_FOREVER`) after every exit. (`hv_vcpu_run` has no deadline
argument, so it cannot be "called with FOREVER".) VZ therefore does not depend on the preemption
timer, but the `run_until` path is on its hot path today. A finite deadline is clamped onto the
core's 10 ms slice and reported as VMX exit reason 52; VZ never asks for one.

## Baseline probe output (before the fixes in this branch)

```
hv_vm_create(HV_VM_ACCEL_APIC)                       HV_UNSUPPORTED
hv_vm_create twice in one task                       HV_BUSY
vCPUs created in one VM (per-thread)                 16 of 64

hv_vm_create(HV_VM_DEFAULT)                          ok
hv_vm_map (1 MiB, R|W|X)                             ok
hv_vm_map over an existing mapping                   HV_BUSY
hv_vm_map unaligned user address                     HV_BAD_ARGUMENT
hv_vm_protect                                        ok
hv_vm_sync_tsc                                       ok
hv_vm_space_create                                   HV_UNSUPPORTED

hv_vcpu_create                                       ok
hv_vcpu_read_register(RIP)                           ok
hv_vcpu_write_msr(LSTAR)                             ok
hv_vcpu_read_msr(LSTAR)                              ok
hv_vcpu_enable_native_msr(FS_BASE)                   ok
hv_vcpu_set_msr_access(FS_BASE, off)                 ok
hv_vcpu_read_fpstate                                 ok
hv_vcpu_invalidate_tlb                               ok
hv_vcpu_inject_extint                                HV_UNSUPPORTED
hv_vcpu_vmx_status                                   HV_ERROR
hv_vcpu_exit_info                                    HV_ERROR

hv_vcpu_run (guest executes HLT)                     ok, exit 12
hv_vcpu_run_until(past deadline)                     ok, ~10 ms, exit 1
hv_vcpu_run_until(FOREVER) + interrupt               ok, ~10 ms, exit 1

hv_vcpu_get_exec_time                                ok
hv_vcpu_get_idle_time                                ok
  (exec / idle)                                      0 ns / 0 ns
hv_vcpu_apic_read(0x80)                              HV_UNSUPPORTED
hv_vcpu_apic_write(0x80)                             HV_UNSUPPORTED
hv_vm_lapic_set_intr(0x20)                           HV_UNSUPPORTED
hv_vm_lapic_msi                                      HV_UNSUPPORTED
hv_vm_ioapic_read                                    HV_UNSUPPORTED
hv_vm_ioapic_write                                   HV_UNSUPPORTED
hv_vm_ioapic_assert_irq(0)                           HV_UNSUPPORTED
hv_vm_ioapic_deassert_irq(0)                         HV_UNSUPPORTED
hv_vm_add_pio_notifier(0x3f8)                        HV_UNSUPPORTED
hv_vm_remove_pio_notifier(0x3f8)                     HV_UNSUPPORTED
```

Two answers in that baseline are the visible symptoms of the gaps fixed in this branch:

- `16 of 64`: the front end reserved 64 slices but the core had `fVcpus[16]`. VZ allows up to 64
  vCPUs (`VZVirtualMachineConfiguration.maximumAllowedCPUCount`), so a large VM could not start.
- `run_until(past deadline)` took ~10 ms and reported exit 1: the deadline was ignored and the
  core's 10 ms slice was the only limit. It should return at once with exit 52 (VMX preemption
  timer expired). `run_until(FOREVER)` ending at ~10 ms with exit 1 is the same slice, this time
  reported as an external interrupt — the probe's kick is scheduled 100 ms out, so the slice wins.

## The API matrix

| API | AMDHV | Notes |
| --- | --- | --- |
| `hv_vm_create` | yes | one VM per task; `HV_VM_ACCEL_APIC` needs boot-arg `amdhv_apic=1`, otherwise `HV_UNSUPPORTED` |
| `hv_vm_destroy` | yes | refused while a vCPU or a trap still holds the VM; a live vCPU makes the client answer `HV_ERROR`, and the call succeeds after the last `hv_vcpu_destroy` |
| `hv_vm_map` / `unmap` / `protect` | yes | map/protect require 4 KiB-aligned ranges, and one map is capped at 16 GiB (`AMDHV_MAP_MAX_SIZE`); unmap accepts sub-page ranges and rounds them to page boundaries |
| `hv_vm_sync_tsc` | yes | handed to vCPUs created later too |
| `hv_vm_space_create` / `map_space` / `protect_space` / `unmap_space` / `space_destroy` | yes | up to 64 extra address spaces, each with its own NPT root; `space_destroy` answers `HV_BUSY` while a vCPU is bound to it |
| `hv_vm_add_pio_notifier` / `remove` | yes | xnu `hv_io_notifier_grp_*`; a matching guest `OUT` sends an `hv_ion_message_t` and suppresses the exit; the client needs a send right on the port |
| `hv_vm_atpic_*` / `hv_vm_ioapic_*` / `hv_vm_lapic_*` | opt-in | task trap 10 (`tIntCtl`) with `amdhv_apic=1`; see [In-kernel APIC work](#in-kernel-apic-work-phase-3-in-progress) |
| `hv_vcpu_create` / `destroy` | yes | one vCPU per thread, up to 64 |
| `hv_vcpu_read/write_register` | yes | through the shared page, no trap |
| `hv_vcpu_read/write_msr` | yes | MSR cache plus native/managed lists |
| `hv_vcpu_enable_native_msr` / `set_msr_access` | yes | |
| `hv_vcpu_read/write_fpstate` | yes | XSAVE, XCR0 |
| `hv_vcpu_flush` | n/a | deprecated by Apple, always `HV_UNSUPPORTED` |
| `hv_vcpu_invalidate_tlb` | yes | schedules a TLB flush for that vCPU's next entry |
| `hv_vcpu_set_space` | yes | rebinds the vCPU's VMCB `nCR3` and flushes its ASID on the next entry |
| `hv_vcpu_run` | yes | blocks until a VM exit; 10 ms host slice |
| `hv_vcpu_run_until` | yes | finite deadline folded into the slice, reported as exit 52 |
| `hv_vcpu_interrupt` | yes | IPI to the CPU in the guest |
| `hv_vcpu_inject_extint` | partial | thread trap 13 sub-op 0 is decoded but still a no-op; requires `amdhv_apic=1` |
| `hv_vcpu_apic_*` | partial | thread trap 13 sub-ops 1/2/4/6/7 are decoded; get/put state are real, read/write are no-ops; requires `amdhv_apic=1` |
| `hv_vcpu_get_exec_time` | yes | ro page offset 0, updated after every run |
| `hv_vcpu_get_idle_time` | yes (0) | ro page offset 8; HLT is a client exit, not emulated in the kernel |
| `hv_vcpu_exit_info` | yes (opt-in APIC) | framework reads the ro page at `+0x20`; the front end publishes `HV_VM_EXITINFO_*` on matching in-kernel APIC events and clears it every run |
| `hv_vcpu_vmx_status` | `HV_ERROR` | ro page `+0x30` stays 0, i.e. "no extra information", as on Apple |
| `hv_vmx_read_capability` | yes | the VMX capability words VZ/QEMU build their controls from |
| `hv_vmx_vcpu_read/write_vmcs` | yes | the VMCS shadow in `VmcsState.hpp` |
| `hv_vcpu_set_tsc_relative` | yes | applied on the next run as `commpage(adjust - base) + relative` (`TscOffset.hpp`) |
| `hv_vmx_vcpu_read/write_shadow_vmcs` / `set_shadow_access` | yes (client-side) | values round-trip in the framework's area; SVM has no shadow VMCS for the core |

## Full public API matrix, measured on 0.2.0

The older probe walked client paths and left nine SDK names unmeasured. `hvf-probe --matrix` now
calls all 79 exported x86 functions once with benign arguments in a fresh task and prints each
answer. Run it with:

```bash
./build/hvf-probe --matrix
```

Measured on an AMD Ryzen 5 3600, macOS 15.8.3, SDK macOS 15, with the Phase 1 kext loaded
(com.amdhv.AMDHV 0.2.0, UUID `D7CA29BF-7FA7-3C02-89EA-C22F24539562`). Clock and TSC-offset numbers
change per run:

```
Public x86 Hypervisor.framework API matrix (SDK macOS 15)
  hv_vm_create                                         ok
  hv_vm_space_create                                   ok
  hv_capability                                        ok
    (hv_capability value)                              VCPUMAX=64
  hv_vm_map                                            ok
  hv_vm_protect                                        ok
  hv_vm_map_space                                      ok
  hv_vm_protect_space                                  ok
  hv_vm_unmap_space                                    ok
  hv_vm_unmap                                          ok
  hv_vm_sync_tsc                                       ok
  hv_tsc_clock                                         73091529535968 (uint64_t, not hv_return_t)
  hv_vm_add_pio_notifier                               HV_UNSUPPORTED
  hv_vm_remove_pio_notifier                            HV_UNSUPPORTED
  hv_vcpu_create                                       ok
  hv_vcpu_set_space                                    ok
  hv_vm_space_destroy                                  ok
  hv_vcpu_read_register                                ok
  hv_vcpu_write_register                               ok
  hv_vcpu_read_fpstate                                 ok
  hv_vcpu_write_fpstate                                ok
  hv_vcpu_enable_native_msr                            ok
  hv_vcpu_enable_managed_msr                           ok
  hv_vcpu_set_msr_access                               ok
  hv_vcpu_write_msr                                    ok
  hv_vcpu_read_msr                                     ok
  hv_vcpu_flush                                        HV_UNSUPPORTED
  hv_vcpu_invalidate_tlb                               ok
  hv_vcpu_interrupt                                    ok
  hv_vcpu_set_tsc_relative                             ok
    (set_tsc_relative effect)                          wired (offset 0xffffff7b5757b459 -> 0x1000)
  hv_vcpu_run                                          ok
  hv_vcpu_run_until                                    ok
  hv_vcpu_get_exec_time                                ok
  hv_vcpu_get_idle_time                                ok
  hv_vcpu_vmx_status                                   HV_ERROR
  hv_vcpu_exit_info                                    HV_ERROR
  hv_vm_lapic_set_intr                                 HV_UNSUPPORTED
  hv_vm_lapic_msi                                      HV_UNSUPPORTED
  hv_vm_ioapic_assert_irq                              HV_UNSUPPORTED
  hv_vm_ioapic_deassert_irq                            HV_UNSUPPORTED
  hv_vm_ioapic_pulse_irq                               HV_UNSUPPORTED
  hv_vm_ioapic_read                                    HV_UNSUPPORTED
  hv_vm_ioapic_write                                   HV_UNSUPPORTED
  hv_vm_ioapic_get_state                               HV_UNSUPPORTED
  hv_vm_ioapic_put_state                               HV_UNSUPPORTED
  hv_vm_send_ioapic_intr                               HV_UNSUPPORTED
  hv_vm_atpic_assert_irq                               HV_UNSUPPORTED
  hv_vm_atpic_deassert_irq                             HV_UNSUPPORTED
  hv_vm_atpic_port_read                                HV_UNSUPPORTED
  hv_vm_atpic_port_write                               HV_UNSUPPORTED
  hv_vm_atpic_get_state                                HV_UNSUPPORTED
  hv_vm_atpic_put_state                                HV_UNSUPPORTED
  hv_vm_set_apic_bus_freq                              HV_UNSUPPORTED
  hv_vcpu_inject_extint                                HV_UNSUPPORTED
  hv_vcpu_apic_read                                    HV_UNSUPPORTED
  hv_vcpu_apic_write                                   HV_UNSUPPORTED
  hv_vcpu_apic_get_state                               HV_UNSUPPORTED
  hv_vcpu_apic_put_state                               HV_UNSUPPORTED
  hv_vcpu_exit_init_ap                                 HV_ERROR
  hv_vcpu_exit_startup_ap                              HV_ERROR
  hv_vcpu_exit_ioapic_eoi                              HV_ERROR
  hv_vcpu_exit_apic_access_read                        HV_ERROR
  hv_vcpu_exit_inject_excp                             HV_ERROR
  hv_vcpu_apic_lsc_enter_r32                           HV_UNSUPPORTED
  hv_vcpu_apic_lsc_enter_imm32                         HV_UNSUPPORTED
  hv_vcpu_apic_lsc_invalidate                          HV_UNSUPPORTED
  hv_vcpu_apic_ctrl                                    HV_UNSUPPORTED
  hv_vcpu_apic_trigger_lvt                             HV_UNSUPPORTED
  hv_vmx_vcpu_read_vmcs                                ok
  hv_vmx_vcpu_write_vmcs                               ok
  hv_vmx_vcpu_get_cap_write_vmcs                       ok
  hv_vmx_vcpu_read_shadow_vmcs                         ok
  hv_vmx_vcpu_write_shadow_vmcs                        ok
    (shadow VMCS round-trip)                           wired (read-back 0x1234)
  hv_vmx_vcpu_set_shadow_access                        ok
  hv_vmx_read_capability                               ok
  hv_vmx_get_msr_info                                  ok
    (msr_info value)                                   0
  hv_vmx_vcpu_set_apic_address                         ok
  hv_vmx_vcpu_set_apic_address_space                   ok
  hv_vcpu_destroy                                      ok
  hv_vm_destroy                                        ok
  public API functions observed                        79 of 79
```

Three `ok` answers need a careful reading before they become a compatibility claim:

- `hv_vcpu_set_tsc_relative` now works: the framework writes the relative value to `rw+0xc78` and
  sets flag `0x100`, and the front end folds in the commpage delta before loading the VMCB offset.
  The probe reads `0x1000` after the guest run, replacing the previous baseline from `hv_vm_sync_tsc`.
- `hv_vmx_vcpu_read/write_shadow_vmcs` store and return the value in the framework's client-side
  area (the 0x1234 round-trip proves that), but SVM has no shadow VMCS for the core to consume.
  `hv_vmx_vcpu_set_shadow_access` now answers `ok` as a no-op from thread trap 8, consistent with
  them.
- `hv_vmx_vcpu_set_apic_address_space` is a no-op success, like the already documented
  `hv_vmx_vcpu_set_apic_address`: the core keeps the TPR in `V_TPR` and never reads that page.

`hv_capability` reports `VCPUMAX=64`, matching `AMDHV_MAX_VCPUS_PER_VM`; `hv_tsc_clock` returns a
monotonic value; `hv_vmx_get_msr_info` answers 0 for the Intel-only `IA32_ARCH_CAPABILITIES` info
field. `hv_vcpu_vmx_status` answers `HV_ERROR` because ro page `+0x30` stays 0, as on Apple. The
exit-info helpers are framework decoders of a payload at ro `+0x20/0x28/0x48/0x49/0x4c/0x50`;
without a matching in-kernel APIC event all six answer `HV_ERROR`. With `amdhv_apic=1`,
`hvf-probe --exitinfo-mmio` verifies `exit_info`, `hv_vcpu_exit_apic_access_read` and
`hv_vcpu_exit_ioapic_eoi`; INIT/SIPI and #GP publication is wired but has no live probe.

## Capability words follow the implementation

`hv_vmx_read_capability` no longer returns a fixed copy of an Intel Mac's words. `kext/Source/vmcs/VmcsCaps.hpp`
derives each control word from what the kext actually implements, so a client that canonicalises its
controls through `cap2ctrl` (QEMU, the Android emulator, the smoke test) cannot ask for a feature that
would only be shadowed:

| word | offered now | no longer offered |
| --- | --- | --- |
| pin-based | external-interrupt and NMI exiting | virtual NMIs, preemption timer, posted interrupts |
| primary | TSC offsetting, HLT/INVLPG/MWAIT/RDPMC/RDTSC, CR3/CR8 exiting, MOV-DR, I/O exiting, MONITOR/PAUSE, NMI-window, secondary controls, TPR shadow (switch permitting) | I/O bitmaps, MSR bitmaps, monitor trap flag |
| secondary | EPT/NPT, RDTSCP, WBINVD exiting, unrestricted guest, INVPCID | VPID, virtual APIC/x2APIC, virtual-interrupt delivery, PLE, PML, #VE, TSC scaling, ... |
| EPT/VPID | 4-level walk, WB memory | execute-only, 2 MiB/1 GiB leaves, INVEPT, A/D bits |

Virtual NMIs, I/O bitmaps, MSR bitmaps and VPID are still **accepted** as compatibility no-ops (the old
table advertised them, and VirtualBox/QEMU may write them directly), but they are not advertised. A
control write with any other unadvertised bit is refused by `VmcsState::write()` and logged by the front
end instead of being silently stored; `tests/vmcs_state_test.cpp` covers both the words and the refusal.
NMI-window exiting stays advertised because `Frontend.cpp` answers it immediately.

### Verified with 0.1.2

The rebuilt kext was installed to the EFI partition and loaded after a reboot on 2026-11-22:

- `make test`: all host tests pass, including the new capability and control-rejection checks.
- `hvf-smoke`: `PASS: a VMM written against the public Hypervisor.framework API works`.
- `hvf-probe`: `64 of 64` vCPUs, `run_until(past deadline)` returns in microseconds with exit 52,
  and exec time is non-zero.
- Android emulator 37.3.10: the Android 17 AVD boots to the launcher; ADB, input and network work.
- VirtualBox 7.2.22: a Lubuntu 24.04 live guest (2 vCPU, 2 GiB) reaches the installer with NAT
  connected; pausing the VM stops the AMDHV exit counter and resuming restarts it.
- `vmInvalidExits` and `assertFailures` stayed 0 and no new panic log was written.

## What changed in this branch

- `AMDHV_MAX_VCPUS_PER_VM` is 64, matching the 64 slices the front end already reserves and the
  VZ maximum. After a reboot the probe prints `64 of 64`.
- `hv_vcpu_run_until` carries a deadline into `runEx`/`run`. The core clamps it onto its 10 ms
  slice (`RunDeadline.hpp`, covered by `tests/run_deadline_test.cpp`) and reports a deadline-driven
  return as VMX exit reason 52. A past deadline returns without entering the guest. VZ exercises
  the `HV_DEADLINE_FOREVER` branch of this path on every run (see the trap table above). The probe
  now reports `run_until(past deadline)` in microseconds with exit 52.
- `hv_vcpu_get_exec_time` is real: the core accumulates the time spent inside VMRUN and the front
  end copies it to ro page offset 0 after every run. The probe now shows a non-zero exec time after
  the HLT run (idle stays 0).

All three are built, installed and host-tested. Run `./build/hvf-probe` and compare with the
baseline above: the vCPU count, the `run_until(past deadline)` line and the exec/idle line changed;
everything else should still match.

## Guest compatibility seen on this host

Hypervisor.framework clients other than VZ were also exercised on an AMD Ryzen 5 3600 host with
the 0.1.x kext loaded:

| guest | client | result |
| --- | --- | --- |
| Windows 10 (64-bit), 4 vCPU, 8 GiB | VirtualBox 7.2.22 | boots to desktop; Task Manager reports 4 vCPUs and uptime advances |
| Lubuntu 24.04 live, 2 vCPU, 2 GiB | VirtualBox 7.2.22 | reaches the installer; NAT connected; pause/resume stops and restarts the exit counter (0.1.2) |
| Android 17 AVD (`Medium_Phone_API_37.0`) | Android Studio emulator 37.3.10 | boots to launcher; ADB online, input, UI and network work (re-checked on 0.1.2) |
| LinuxKit / Docker Desktop | VZ | works; engine starts and containers serve traffic |
| Lubuntu 24.04, 4 vCPU, QEMU `tsc=reliable` | QEMU 11.1.3 `-accel hvf` | boots to login prompt |
| Windows 10 and 11 | QEMU 11.1.3 `-accel hvf` | OVMF, `BOOTX64.EFI` and `cdboot.efi` load and Windows Setup starts; a missed CD boot-key press falls through to PXE, and Win11 needs the `LabConfig` requirements bypass |

The VirtualBox and Android results are not guesses from a window being open: `com.amdhv.AMDHV`'s
`vmExits` counter stops increasing while the VirtualBox VM is paused or the emulator process is
suspended, and resumes when each is allowed to run. No other VBox kernel extension is loaded, and
`vmInvalidExits` and `assertFailures` stayed at zero. So Windows and Android do run through
HVF/AMDHV. The QEMU symptom was an OVMF CD boot-key timeout (VM falls through to PXE), plus the
Windows 11 installer's TPM/Secure Boot gate — not a missing capability in this kext. HVF here also
rejects `-machine q35,smm=on` (`System Management Mode not supported by this hypervisor`), so the
standard Secure Boot OVMF path is unavailable. See [QEMU.md](QEMU.md). The QEMU attempts produced no
host panic either.

### Support status

None of these clients is officially supported by this project. See the
[client index](CLIENTS.md) for the per-client status, guides and the shared verification method.

QEMU's TPM device setup additionally needs the sub-page `hv_vm_unmap` rounding fixed
(commit `356d903`); without it QEMU aborts with `HV_BAD_ARGUMENT` before the guest starts.

## In-kernel APIC work (Phase 3, in progress)

The 34 refused calls are not all blocked in the kext: with a default VM, Apple's framework answers
most of them in userspace before any trap is reached. A follow-log capture of `hvf-probe --matrix`
shows only `hv_vm_lapic_msi` reaching task trap 11; the gate is `hv_vm_create(HV_VM_ACCEL_APIC)`,
which is now open with boot-arg `amdhv_apic=1` (below).

The first slice added a host-testable register model (`kext/Source/arch/ApicState.hpp`,
`tests/apic_test.cpp`) whose LAPIC/IOAPIC/8259A layouts match the SDK state structs byte for byte.
`tVmCreate` accepts `HV_VM_ACCEL_APIC` with boot-arg `amdhv_apic=1`; on the rebooted host that made
the call return `ok`, and `hvf-probe --apic` exercised all 32 APIC calls under such a VM.

A trap-log capture decoded task trap 10's op codes (`0x6a`–`0x7a` for LAPIC/IOAPIC/AT-PIC and bus
frequency) and task 11's `(data << 32) | addr` MSI argument. Task traps 10/11 are now wired to the
register model, and thread trap 13 accepts the ten per-vCPU sub-ops as no-ops after decoding their
block codes. On the installed kext, `--apic` shows **27 `ok`** answers out of 32 calls; the five
`hv_vcpu_exit_*` helpers answer `HV_ERROR` without an exit-info event. The front end now publishes
`HV_VM_EXITINFO_*` in the ro page, and `--exitinfo-mmio` live-verifies `exit_info`,
`apic_access_read` and `ioapic_eoi`; INIT/SIPI/#GP publication remains unprobed. IOAPIC get/put
state used the wrong word of the arg block; the fix is live-verified (both calls answer `ok`).

The guest path is now live-verified with `hvf-probe --apic-run`: a real-mode guest with an IVT entry
for vector `0x30` and an IOAPIC redirect from IRQ0 exits with RIP `0x1200` (the handler), not the
entry HLT at `0x1000`. The trap log shows `pending=48` for that run. The fix that made it work was
the `hv_ioapic_state_ext_t` layout: state starts at offset `+8`, not `+4`; the 4-byte shift made the
mask bit appear set. `offsetof` on the macOS 15 SDK gives `ioapic=8`, `apic=8`, `atpic=4`.

The exit helpers never reach a trap: the framework decodes the `HV_VM_EXITINFO_*` payload written
into the ro page. With no event all six answer `HV_ERROR`; `exit_info`, `apic_access_read` and
`ioapic_eoi` are live-verified via `hvf-probe --exitinfo-mmio`. LAPIC `get_state`/`put_state` use the SDK layout (state at
`+8`) and are real; `hv_vcpu_apic_read`/`write` are accepted as no-ops because their private
argument encoding does not expose the offset/value as plain client-memory words. An earlier
get_state version caused an SMAP panic by copying straight to a user pointer; the current code uses
`copyout`/`copyin` through a kernel buffer. Guest-visible routing is exercised only for the
8259A/IOAPIC → LAPIC IRR path above.

Limited to the 8259A/IOAPIC → LAPIC IRR path. See [APIC_PENDING.md](APIC_PENDING.md) for the
decoded wire format, the verified results, and the single remaining verification step; no further
reboot is requested until that batch is ready.

## A framework quirk worth knowing

Calling `hv_vm_create` a second time in one task answers `HV_BUSY` (correct), but then
`hv_vm_destroy` aborts the process inside `hv_vmx_vm_destroy` (`malloc_zone_error`, SIGABRT). The
kext is not involved: Apple's userspace framework corrupts its own heap on the error path. The
probe runs that sequence in a child and exits before destroying, and so should any test that checks
the one-VM-per-task rule.
