# What Virtualization.framework uses, and what AMDHV answers

This page records the public Hypervisor.framework surface a Virtualization.framework (VZ) client
can reach, what AMDHV implements today, and how to check a build without guessing from the kernel
log. `tools/hvf-probe` prints the same table on a live machine:

```bash
make tools
./build/hvf-probe          # needs the com.apple.security.hypervisor entitlement, already applied
```

The probe forks/execs itself for the three probes that need a task without a VM (the in-kernel APIC
variant, a second `hv_vm_create`, and the vCPU count). Everything else runs in one task.

## What VZ actually calls

A VZ VM on x86 is a userspace device model on top of a small hypervisor API. The live trap log of a
running VZ VM (`com.apple.Virtualization.VirtualMachine`, Kiri with six containers) shows only:

| trap | call | frequency |
| --- | --- | --- |
| task 12 | `hv_vcpu_create` | once per vCPU |
| thread 1 | `hv_vcpu_run_until` (`HV_DEADLINE_FOREVER`) | after every exit |
| thread 7 | `hv_vmx_vcpu_read_vmcs` (exit fields) | after every exit |
| task 13 | `hv_vcpu_interrupt` | when the device model injects an IRQ |

No in-kernel APIC, IOAPIC, PIO notifier or address-space call appears. That is why the missing
stubs below do not stop VZ today: VZ emulates the APIC, the timer and every device itself and only
needs memory mapping, vCPU state, VM exits and interrupt injection.

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
| `hv_vm_create` | yes | one VM per task; `HV_VM_ACCEL_APIC` is refused with `HV_UNSUPPORTED` |
| `hv_vm_destroy` | yes | `HV_BUSY` while a vCPU or a trap still holds the VM |
| `hv_vm_map` / `unmap` / `protect` | yes | map/protect require 4 KiB-aligned ranges; unmap accepts sub-page ranges and rounds them to page boundaries |
| `hv_vm_sync_tsc` | yes | handed to vCPUs created later too |
| `hv_vm_space_create` / `map_space` / `protect_space` / `unmap_space` / `space_destroy` | no | stubs answer `HV_UNSUPPORTED` |
| `hv_vm_add_pio_notifier` / `remove` | no | stubs; VZ uses I/O exits instead |
| `hv_vm_atpic_*` / `hv_vm_ioapic_*` / `hv_vm_lapic_*` | no | in-kernel APIC only, which `hv_vm_create` refuses |
| `hv_vcpu_create` / `destroy` | yes | one vCPU per thread, up to 64 |
| `hv_vcpu_read/write_register` | yes | through the shared page, no trap |
| `hv_vcpu_read/write_msr` | yes | MSR cache plus native/managed lists |
| `hv_vcpu_enable_native_msr` / `set_msr_access` | yes | |
| `hv_vcpu_read/write_fpstate` | yes | XSAVE, XCR0 |
| `hv_vcpu_flush` | n/a | deprecated by Apple, always `HV_UNSUPPORTED` |
| `hv_vcpu_invalidate_tlb` | yes | |
| `hv_vcpu_run` | yes | blocks until a VM exit; 10 ms host slice |
| `hv_vcpu_run_until` | yes | finite deadline folded into the slice, reported as exit 52 |
| `hv_vcpu_interrupt` | yes | IPI to the CPU in the guest |
| `hv_vcpu_inject_extint` | no | thread trap 13 sub-op 0; only meaningful with the in-kernel APIC |
| `hv_vcpu_apic_*` | no | thread trap 13 sub-ops 1/2/4/6/7; in-kernel APIC only |
| `hv_vcpu_get_exec_time` | yes | ro page offset 0, updated after every run |
| `hv_vcpu_get_idle_time` | yes (0) | ro page offset 8; HLT is a client exit, not emulated in the kernel |
| `hv_vcpu_exit_info` / `vmx_status` | `HV_ERROR` | ro page 0x20/0x30 are 0, i.e. "no extra information", as on Apple |
| `hv_vmx_read_capability` | yes | the VMX capability words VZ/QEMU build their controls from |
| `hv_vmx_vcpu_read/write_vmcs` | yes | the VMCS shadow in `VmcsState.hpp` |

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
AMDHV 0.1.0 loaded:

| guest | client | result |
| --- | --- | --- |
| Windows 10 (64-bit), 4 vCPU, 8 GiB | VirtualBox 7.2.22 | boots to desktop; Task Manager reports 4 vCPUs and uptime advances |
| Android 17 AVD (`Medium_Phone_API_37.0`) | Android Studio emulator 37.3.10 | boots to launcher; ADB online, input, UI and network work |
| LinuxKit / Docker Desktop | VZ | works; engine starts and containers serve traffic |
| Lubuntu 24.04, 4 vCPU, QEMU `tsc=reliable` | QEMU 11.1.3 `-accel hvf` | boots to login prompt |
| Windows 10 and 11 | QEMU 11.1.3 `-accel hvf` | OVMF, `BOOTX64.EFI` and `cdboot.efi` load and the Tianocore logo appears, but setup never starts |

The VirtualBox and Android results are not guesses from a window being open: `com.amdhv.AMDHV`'s
`vmExits` counter stops increasing while the VirtualBox VM is paused or the emulator process is
suspended, and resumes when each is allowed to run. No other VBox kernel extension is loaded, and
`vmInvalidExits` and `assertFailures` stayed at zero. So Windows and Android do run through
HVF/AMDHV — the QEMU hang is specific to QEMU's HVF accelerator, not a missing capability in this
kext. The QEMU attempts produced no host panic either.

### Support status

None of these clients is officially supported by this project. See the
[client index](CLIENTS.md) for the per-client status, guides and the shared verification method.

QEMU's TPM device setup additionally needs the sub-page `hv_vm_unmap` rounding fixed
(commit `356d903`); without it QEMU aborts with `HV_BAD_ARGUMENT` before the guest starts.

## A framework quirk worth knowing

Calling `hv_vm_create` a second time in one task answers `HV_BUSY` (correct), but then
`hv_vm_destroy` aborts the process inside `hv_vmx_vm_destroy` (`malloc_zone_error`, SIGABRT). The
kext is not involved: Apple's userspace framework corrupts its own heap on the error path. The
probe runs that sequence in a child and exits before destroying, and so should any test that checks
the one-VM-per-task rule.
