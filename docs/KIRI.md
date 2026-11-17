# Docker with Kiri on AMDHV

This gets you Docker containers with [Kiri](https://github.com/libracoder/kiri), a lightweight Rust container runtime that serves the Docker Engine API. On macOS, Kiri boots a minimal Linux VM using Apple's `Virtualization.framework` (`VZVirtualMachine`); on an AMD Hackintosh, that framework requires AMDHV's `Hypervisor.framework` backing (`amdhv_hv=1` and `kern.hv_support=1`).

Unlike QEMU backends that link directly against `Hypervisor.framework` (`hv_vm_create`), `Virtualization.framework` requires the `com.apple.security.virtualization` code-signing entitlement (bundled and notarized in official `Kiri.app` releases). No Rosetta is needed on Intel: `linux/amd64` images run natively in the x86_64 guest.

## 1. Install

Download and install `Kiri-<version>-x86_64.dmg` from [runkiri.com/releases](https://runkiri.com/releases/) or [GitHub Releases](https://github.com/libracoder/kiri/releases).

Verify the virtualization entitlement on both the app bundle and inner binary:

```bash
codesign -d --entitlements - /Applications/Kiri.app 2>&1 | grep virtual
codesign -d --entitlements - /Applications/Kiri.app/Contents/MacOS/kiri 2>&1 | grep virtual
# both must show: com.apple.security.virtualization
```

First launch configures the `kiri` Docker context pointing to `unix://$HOME/.kiri/run/docker.sock` and installs the background daemon (`~/Library/LaunchAgents/com.kiri.daemon.plist`).

## 2. Check AMDHV is up

Kiri calls `VZVirtualMachineConfiguration` and `VZVirtualMachine::startWithCompletionHandler` as soon as the first container request arrives (or via the menu bar / window Start button).

```bash
sysctl kern.hv_support       # must print 1 (the kext sets it at boot)
sudo ~/amdhv/build/amd-info  # SVM must be "active on" every CPU
```

If `kern.hv_support` is 0, the kext is not loaded or the boot-args are missing — see the [README](../README.md#install) Install section first.

## 3. Start Kiri and run a container

```bash
kiri start                                 # boot the VM (or click Start in the Kiri window)
docker --context kiri run --rm alpine echo hello
```

The `kiri` CLI wrapper always targets Kiri regardless of the active Docker context:

```bash
kiri run --rm alpine echo hello
kiri compose up -d
```

The Kiri VM powers off automatically after 60 seconds idle (`VM asleep: idle`) and wakes on the next request.

## 4. Verify it runs through AMDHV

```bash
sudo ~/amdhv/build/amd-info stats                # vmRuns grows while the Kiri VM runs
                                                 # vmFpuMismatches must stay 0
tail -n 20 ~/.kiri/daemon.log                    # shows "VM booted ms=... cpus=... memory_mib=..."
docker --context kiri run --rm alpine echo hello # end-to-end test
```

## 5. Daily use

| command | what it does |
|---|---|
| `kiri start` / `kiri stop` | start or stop the Kiri VM |
| `kiri ps` / `kiri images` | list containers and images (same as `docker --context kiri ps`) |
| `kiri compose up -d` | start a Compose project on the Kiri context |
| `kiri import` | import existing images, volumes, and containers from Colima/Docker Desktop/OrbStack |
| `kiri settings` | configure VM CPU/RAM (`~/.kiri/vm.json`) or edit directly (`KIRI_CPUS`, `KIRI_MEMORY_MIB`) |

Default VM resources on Intel start at 4096 MiB RAM (`4096` in `~/.kiri/vm.json`); adjust CPU and memory in the Kiri window or `~/.kiri/vm.json` before starting the VM.

## Troubleshooting

| symptom | cause and fix |
|---|---|
| `kiri start` hangs or fails immediately | AMDHV not up: verify `sysctl kern.hv_support` returns `1` and `sudo ~/amdhv/build/amd-info` shows SVM active |
| `the VM did not start within 30 s` in `~/.kiri/daemon.log` | check `sudo ~/amdhv/build/amd-info stats` and `log -f` while triggering `kiri start` to see if `VMRUN` faults (`#VC` / nested page table faults) |
| `codesign` check shows no entitlement | only run official signed/notarized Kiri builds, or sign a custom build with `crates/kiri/kiri.entitlements` |
| `amd64` image fails on Intel | Kiri x86_64 guest runs `amd64` images natively; verify the Kiri app version matches the CPU (`x86_64` vs `arm64`) |
| `kern.hv_support` reads 0 | AMDHV not loaded: check Kernel → Add in `config.plist`, boot-args `amdhv_enable_svm=1 amdhv_hv=1`, and `sudo ~/amdhv/build/amd-info log` |