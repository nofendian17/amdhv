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

A VM client that uses Hypervisor.framework: a VZ-based Docker runtime (Docker Desktop, Kiri,
colima with the `vz` backend), VirtualBox, or the Android emulator. QEMU with `-accel hvf` is no
longer a target — see [QEMU is not the supported path](#qemu-is-not-the-supported-path).

## Build

```bash
make test          # 10 unit tests, runs anywhere
make kext tools    # build/AMDHV.kext, build/amd-info, build/hvf-smoke, build/hvf-probe
```

Note: the kext Makefile does not track header changes. After editing a `.hpp`, run `rm -rf build/kext-obj` first.

## Install

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

| client | observed on this host |
| --- | --- |
| Docker Desktop (VZ) | engine works; containers serve traffic |
| Kiri (VZ) | runs Linux/amd64 natively on Intel, exposes the Docker API |
| colima (`--vm-type vz`) | VZ backend works through Hypervisor.framework |
| VirtualBox 7.2.22 | Windows 10 (64-bit, 4 vCPU, 8 GiB) boots to desktop |
| Android Studio emulator 37.3.10 | Android 17 AVD boots; ADB, input and network work |
| QEMU 11.1.3 (`-accel hvf`) | Linux boots; Windows 10/11 stop at the Tianocore logo |

These are single-host observations, not a support promise: VirtualBox, Docker Desktop, Android
Studio and QEMU are **not officially supported clients** of this project. What each
Hypervisor.framework call answers on a given build is in [docs/CAPABILITIES.md](docs/CAPABILITIES.md),
and `./build/hvf-probe` prints it live.

### colima / Kiri (Docker)

- **Kiri** (`Virtualization.framework` backend): full setup guide [docs/KIRI.md](docs/KIRI.md). Uses
  `VZVirtualMachine` (which requires `amdhv_hv=1` and `com.apple.security.virtualization`), exposes
  the Docker API at `~/.kiri/run/docker.sock` (`docker --context kiri`), and runs `linux/amd64`
  natively on Intel.
- **colima**: use `colima start --vm-type vz` — the VZ backend runs through Hypervisor.framework.
  The old `qemu` VM type is not the recommended path here; see [docs/COLIMA.md](docs/COLIMA.md) for
  the full setup. Start the VM after boot, when `kern.hv_support` is 1.

### QEMU is not the supported path

QEMU with `-accel hvf` can run Linux guests here (Lubuntu 24.04 needs `tsc=reliable`), but Windows
10 and 11 load OVMF, `BOOTX64.EFI` and `cdboot.efi` and then stop at the Tianocore logo — setup
never starts. This is specific to QEMU's HVF accelerator: Windows boots normally under VirtualBox
and Android boots under the Android emulator, both through HVF/AMDHV. QEMU also needs the sub-page
`hv_vm_unmap` rounding in this kext or it aborts with `HV_BAD_ARGUMENT` when creating a TPM device.

The scripts `scripts/sign-qemu.sh` and `scripts/qemu-test.sh` are kept for reference only and are
not part of the main workflow.

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

## Limits

- 64 vCPUs per VM (VZ's maximum, and what the front end reserves). One VM program at a time is safest.
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
tests/         the 10 unit tests
scripts/       EFI install, symbol check, legacy QEMU signing/boot test
docs/          CAPABILITIES.md, COLIMA.md, KIRI.md
```

MIT license, see `LICENSE`. Ported from RyzenHV (MIT) — keep that copyright when redistributing.
