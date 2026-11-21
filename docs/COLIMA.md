# Docker with colima on AMDHV

This gets you Docker containers on the Hackintosh: colima runs a Linux VM with macOS
**Virtualization.framework** (`--vm-type vz`), which on x86 uses Hypervisor.framework (`hv_vmx_*`)
and therefore AMDHV.

Verified on the reference machine (Zen 2 / Ryzen 5 3600, macOS Sequoia, MacPorts): colima 0.10.3,
lima 2.3.0, 4 vCPU / 8 GiB, `colima start --vm-type vz` and `docker run hello-world`.

**The one rule:** start colima **after** boot, when AMDHV is already up and `kern.hv_support` is 1.

## 1. Install

```bash
sudo port install colima docker       # colima pulls in lima
which colima limactl                  # all in /opt/local/bin
```

No QEMU signing is needed for the VZ backend: `limactl` already carries the
`com.apple.security.virtualization` entitlement, and the VZ path does not involve QEMU.

## 2. Check AMDHV is up

```bash
sysctl kern.hv_support       # must print 1 (the kext sets it at boot)
sudo build/amd-info  # SVM must be "active on" every CPU
```

If `kern.hv_support` is 0, the kext is not loaded or the boot-args are missing — see the
[README](../README.md#install) Install section first.

## 3. Start colima

```bash
colima start --vm-type vz --cpu 4 --memory 8
```

To confirm the backend actually used:

```bash
colima status    # "colima is running using macOS Virtualization.Framework"
```

The old `qemu` VM type is not the recommended path here. If a profile already exists, check it:

```bash
grep vmType ~/.colima/*/colima.yaml
```

## 4. Verify it runs through AMDHV

```bash
colima status                                 # arch x86_64, runtime docker
sudo build/amd-info stats  # vmRuns grows while the VM runs
                                              # vmInvalidExits must stay 0
~/.docker/bin/docker --context colima run --rm hello-world
```

If your `docker` in PATH resolves to another CLI (for example Kiri's), use `~/.docker/bin/docker`
or set `docker context use colima` with that binary.

## 5. Daily use

| command | what it does |
|---|---|
| `colima list` | profiles and their state |
| `colima stop` / `colima start` | the VM does not survive a reboot — start it again after booting |
| `colima restart` | restarts the VM; run this if it came up without AMDHV |
| `colima delete` | removes the profile (deletes the disk image too) |

## Troubleshooting

| symptom | cause and fix |
|---|---|
| `colima status` → "empty value" | the VM never came up: fix the kext first, then `colima start --verbose` |
| `hv_vm_create` permission denied | the VZ client lacks `com.apple.security.virtualization` — reinstall/re-sign limactl; QEMU signing does not apply to the VZ path |
| `kern.hv_support` reads 0 | AMDHV not loaded: check Kernel → Add in `config.plist`, boot-args `amdhv_enable_svm=1 amdhv_hv=1`, and `sudo build/amd-info log` |
