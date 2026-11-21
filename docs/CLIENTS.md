# Client guide index

AMDHV registers in place of AppleHV, so any x86 client that uses Hypervisor.framework — directly
(`hv_vm_create`) or through Virtualization.framework (`VZVirtualMachine`) — reaches the kext.
This page links the per-client guides.

**None of these clients is officially supported by this project.** The results below are
observations from one host (AMD Ryzen 5 3600 / Zen 2, macOS Sequoia, AMDHV 0.1.0) with the listed
client versions. Treat them as examples, not a compatibility guarantee.

| client | backend | observed result | guide |
| --- | --- | --- | --- |
| Docker Desktop 4.100.0 | VZ | engine starts; containers serve traffic | [DOCKER_DESKTOP.md](DOCKER_DESKTOP.md) |
| Kiri 0.2.2 | VZ | Linux/amd64 runs natively; Docker API works | [KIRI.md](KIRI.md) |
| colima 0.10.3 | VZ | 4 vCPU/8 GiB VM starts; `docker run hello-world` succeeds | [COLIMA.md](COLIMA.md) |
| VirtualBox 7.2.22 | HVF | Windows 10 (64-bit) boots to desktop | [VIRTUALBOX.md](VIRTUALBOX.md) |
| Android Studio emulator 37.3.10 | HVF | Android 17 AVD boots; ADB, input and network work | [ANDROID_STUDIO.md](ANDROID_STUDIO.md) |
| QEMU 11.1.3 (`-accel hvf`) | HVF | Linux boots; Windows 10/11 stop at the Tianocore logo | [QEMU.md](QEMU.md) |

For the low-level API surface, see [CAPABILITIES.md](CAPABILITIES.md), or run `./build/hvf-probe`
on a live machine.

## Before starting any client

Commands below use `build/amd-info` relative to the repo root. If you run them from elsewhere, use
the absolute path to the `build/amd-info` binary.

```bash
sysctl kern.hv_support                 # must be 1
sudo build/amd-info                    # SVM must be "active on" every CPU
sudo build/amd-info selftest           # optional: verify SVM on every CPU
```

Start the VM client **after** boot. If `kern.hv_support` is 0, fix the kext/boot-args first — see
the [README Install section](../README.md#install).

## How to confirm a client really uses AMDHV

`build/amd-info stats` is the shared check. `vmCreates` and `vmExits` grow while a guest runs;
`vmInvalidExits` and `assertFailures` should stay at zero. For a stronger check, suspend/pause the
client's VM process: `vmExits` stops increasing, and resumes when the VM runs again. That method
was used to confirm the VirtualBox and Android results, since a visible window alone proves nothing.

## Common pitfalls

- Two hypervisor providers cannot coexist: remove a RyzenHV kext from `EFI/OC/Kexts` and its
  Kernel → Add entry before installing AMDHV.
- Sleep/wake with a running VM is untested — shut the VM down first.
- A custom or Homebrew QEMU build may lack `com.apple.security.hypervisor`; MacPorts QEMU already
  has it and needs no signing step. VZ clients instead need `com.apple.security.virtualization`.
- The `docker` in PATH may point at Kiri's CLI; use `~/.docker/bin/docker` or an explicit
  `--context` for a specific runtime.
