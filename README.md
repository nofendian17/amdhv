# AMDHV

Run **Hypervisor.framework** guests on a **Hackintosh with an AMD CPU** — Docker/VZ, VirtualBox and
the Android emulator.

AMDHV is a kernel extension. At boot it registers with macOS **in place of Apple's AppleHV**, so the
unmodified Hypervisor.framework talks to it. The hardware work is done by AMD-V (SVM).
It is a simplified rewrite of the RyzenHV project: native mode only, no shim, no client patches.

> **Experimental.** Test on a USB stick with its own OpenCore EFI first. Keep a working EFI **without**
> this kext. A kernel bug is a panic.

## Requirements

- "SVM Mode" **enabled in the BIOS**.
- Xcode command line tools (`xcode-select --install`).
- OpenCore.

A VM client that uses Hypervisor.framework: a VZ-based Docker runtime (Docker Desktop, Kiri or
colima), VirtualBox, or the Android emulator. QEMU with `-accel hvf` is no longer a target — see
[QEMU is not the supported path](#qemu-is-not-the-supported-path).

## Build

```bash
make test          # 17 host tests, runs anywhere
make kext tools    # build/AMDHV.kext, build/amd-info, build/hvf-smoke, build/hvf-probe
```

The kext Makefile tracks header dependencies: editing a `.hpp`/`.h` rebuilds every object that includes it.

## Install

Raw step-by-step equivalent (mount EFI, copy kext, edit `config.plist` with PlistBuddy, validate):
[docs/INSTALL_TO_EFI.md](docs/INSTALL_TO_EFI.md).

1. Copy the kext into OpenCore and set the two needed boot-args (keeps a `config.plist.amdhv-backup`):
   ```bash
   ./scripts/install-to-efi.sh /Volumes/EFI
   ```
2. Reboot.
3. Check:
   ```bash
   sysctl kern.hv_support        # must print 1 (the kext sets it automatically)
   sudo build/amd-info           # SVM must be "active on" every CPU
   sudo build/amd-info selftest  # verifies SVM state on every CPU
   ```

## Run a VM

Per-client guides: [docs/CLIENTS.md](docs/CLIENTS.md).

| client | guide | observed on this host |
| --- | --- | --- |
| Docker Desktop 4.100.0 (VZ) | [DOCKER_DESKTOP.md](docs/DOCKER_DESKTOP.md) | engine works; containers serve traffic |
| Kiri 0.2.2 (VZ) | [KIRI.md](docs/KIRI.md) | Linux/amd64 natively on Intel; Docker API works |
| colima 0.10.3 (`--vm-type vz`) | [COLIMA.md](docs/COLIMA.md) | 4 vCPU/8 GiB VM starts; `docker run hello-world` succeeds |
| VirtualBox 7.2.22 | [VIRTUALBOX.md](docs/VIRTUALBOX.md) | Windows 10 (64-bit, 4 vCPU, 8 GiB) boots to desktop |
| Android Studio emulator 37.3.10 | [ANDROID_STUDIO.md](docs/ANDROID_STUDIO.md) | Android 17 AVD boots; ADB, input and network work |
| QEMU 11.1.3 (`-accel hvf`) | [QEMU.md](docs/QEMU.md) | Linux boots; Windows 10/11 stop at the Tianocore logo |

These are single-host observations, not a support promise: colima, VirtualBox, Docker Desktop,
Android Studio and QEMU are **not officially supported clients** of this project. Each guide covers
install, verification through AMDHV and troubleshooting. What each Hypervisor.framework call answers
on a given build is in [docs/CAPABILITIES.md](docs/CAPABILITIES.md), and `./build/hvf-probe` prints
it live. Start the client after boot, when `kern.hv_support` is 1.

### QEMU is not the supported path

QEMU with `-accel hvf` runs Linux guests here (Lubuntu 24.04 needs `tsc=reliable`), but Windows
10 and 11 stop at the Tianocore logo after `cdboot.efi` loads — setup never starts. Windows works
under VirtualBox and Android works under the Android emulator through the same HVF/AMDHV stack, so
this is QEMU-specific. See [docs/QEMU.md](docs/QEMU.md) for details; `scripts/qemu-test.sh` is kept
for reference only and no signing step is needed with MacPorts QEMU.

## Boot-args

Only two are needed — exactly what `install-to-efi.sh` adds:

```
amdhv_enable_svm=1 amdhv_hv=1
```

Everything else is on by default or diagnostics only. Do not add more unless troubleshooting.

| boot-arg | effect |
|---|---|
| `amdhv_enable_svm=1` | **required** — turns SVM on |
| `amdhv_hv=1` | **required** — registers the Hypervisor.framework trap tables |
| `amdhv_hv_sysctl=0` | optional — leaves `kern.hv_support` alone (default: set it to 1) |
| `amdhv_tpr_shadow=0` | optional — hides the TPR shadow from guests (default: offered) |
| `amdhv_selftest=1` | diagnostics — verify SVM state after enable and after wake |
| `amdhv_loglevel=N` / `amdhv_iolog=N` | diagnostics — log levels, also settable with `amd-info level N` |
| `-amdhv_off` | **emergency** — kext loads but does nothing |

## Troubleshooting

```bash
sysctl kern.hv_support          # 1? if not: see boot-args above
sudo build/amd-info stats       # vmRuns grows while a VM runs; vmFpuMismatches must stay 0
sudo build/amd-info log -f      # live kernel log
```

If a RyzenHV kext is still installed, remove it from `EFI/OC/Kexts` and its Kernel → Add entry first:
the two kexts fight over SVM and only one can register as the hypervisor provider.

## Capacity

Up to 64 vCPUs per VM, matching Apple's API and the 64 slices the front end reserves (VZ reports 64
as `maximumAllowedCPUCount` on macOS 15 x86). One VM per process, as in Apple's API; separate
processes can each run a VM, but several at once are less tested than a single one.

## Limits

- Up to 64 extra address spaces per VM (`hv_vm_space_create`); the 65th returns `HV_NO_RESOURCES`.
- A single `hv_vm_map` range is capped at 16 GiB, and nested page tables use 4 KiB leaves only (no
  2 MiB or 1 GiB leaves).
- Hosts with more than 64 logical CPUs enable SVM on the first 64 only (`AMDHV_MAX_CPUS`).
- Sleep/wake with a running VM is untested — shut VMs down first.
- No entitlement check in native mode: any local process can create a VM. Not for machines with
  untrusted local users.
- Windows under QEMU is blocked at the Tianocore logo; Windows under VirtualBox works. Neither
  client is officially supported by this project.
- Verified on this rewrite: **Zen 2** (Matisse, Ryzen 5 3600) with macOS Sequoia — `hvf-smoke`,
  `amd-info`, VZ/Docker (Kiri, Docker Desktop), VirtualBox and the Android emulator all work. The
  ported core was additionally tested on a Ryzen 7 5800H by the original RyzenHV project. Re-test
  on yours with `hvf-smoke` and `./build/hvf-probe`.

## Sources

```
kext/          the kext: SVM, VMCB, VMRUN, nested page tables, the Hypervisor.framework front end
include/       kext ⇄ tools ABI, log ring, VMCS field rules
tools/         amd-info (the CLI above), hvf-smoke (public-API acceptance test),
               hvf-probe (which API calls this build answers, see docs/CAPABILITIES.md)
tests/         the 17 host tests
scripts/       EFI install, symbol check, legacy QEMU boot test
docs/          INSTALL_TO_EFI.md (manual install), CLIENTS.md (client index),
               CAPABILITIES.md (API matrix),
               COMPATIBILITY_CHECKLIST.md (enablement, SVM and HVF checklist, gap register),
               DOCKER_DESKTOP.md, KIRI.md, COLIMA.md, VIRTUALBOX.md,
               ANDROID_STUDIO.md, QEMU.md
```

MIT license, see `LICENSE`. Ported from RyzenHV (MIT) — keep that copyright when redistributing.
