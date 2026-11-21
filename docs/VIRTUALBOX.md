# VirtualBox on AMDHV

VirtualBox 7.2.22 runs on macOS through Hypervisor.framework on this host — there is no VBox
kernel extension loaded, only `com.amdhv.AMDHV`. A Windows 10 (64-bit) guest with 4 vCPU and 8 GiB
boots to the desktop.

**Not an officially supported client.** This is a single-host observation, not a support promise.

## 1. Check AMDHV is up

```bash
sysctl kern.hv_support                 # must be 1
sudo build/amd-info  # SVM must be "active on" every CPU
kextstat | grep -i amdhv               # com.amdhv.AMDHV must be loaded
```

Start the VM after boot.

## 2. Guest configuration used for the test

| setting | value |
| --- | --- |
| Guest OS | Windows 10 (64-bit) |
| CPUs / memory | 4 vCPU, 8192 MiB |
| Network | NAT, `cableconnected1=on` |
| Video | 1024x768x32 |
| Guest Additions | not installed (`GuestAdditionsRunLevel=0`) |

Create the VM normally in VirtualBox and install Windows from the ISO. No AMDHV-specific guest
settings are needed.

## 3. Verify it runs through AMDHV

A running window is not proof. Pause and resume the VM while watching the kext counter:

```bash
VBoxManage showvminfo <vm-name> --machinereadable | grep VMState
VBoxManage controlvm <vm-name> pause
sudo build/amd-info stats | grep vmExits    # stops increasing
VBoxManage controlvm <vm-name> resume
sudo build/amd-info stats | grep vmExits    # increases again
```

With the VM paused, `vmExits` showed a delta of 0; after resume it grew steadily. No other
VirtualBox kernel extension was loaded. `vmInvalidExits` and `assertFailures` stayed at zero.

## 4. Daily use

```bash
VBoxManage list vms
VBoxManage startvm <vm-name>
VBoxManage controlvm <vm-name> savestate
```

## Troubleshooting

| symptom | cause and fix |
|---|---|
| VM will not start | check `sysctl kern.hv_support` and AMDHV boot-args before touching VirtualBox settings |
| black screen / no boot | confirm the guest disk and ISO are attached; a visible window alone does not prove the guest booted |
| host panic | collect the panic log and check for a second hypervisor provider (e.g. RyzenHV) in `EFI/OC/Kexts` |
