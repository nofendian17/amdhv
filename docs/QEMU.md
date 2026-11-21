# QEMU on AMDHV — not the supported path

QEMU 11.1.3 with `-accel hvf` can run **Linux** guests here, but this is no longer the recommended
way to use the kext. Windows 10 and 11 load OVMF, `BOOTX64.EFI` and `cdboot.efi` and then stop at
the Tianocore logo — setup never starts. Windows under VirtualBox and Android under the Android
emulator both work through the same HVF/AMDHV stack, so the hang is specific to QEMU's HVF
accelerator, not a missing capability in AMDHV.

`scripts/qemu-test.sh` is kept for reference only. There is no signing step for MacPorts QEMU: it
already carries `com.apple.security.hypervisor`.

## Working Linux example

Lubuntu 24.04 desktop, 4 vCPU:

```bash
qemu-system-x86_64 -accel hvf -cpu host -smp 4 -m 8192 \
  -kernel /path/to/casper/vmlinuz -initrd /path/to/casper/initrd \
  -append "boot=casper tsc=reliable quiet splash console=ttyS0" \
  -machine pc,hpet=off \
  -drive file=lubuntu.qcow2,if=none,id=hd0 \
  -device ich9-ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -display cocoa -vga std -usbdevice tablet
```

`tsc=reliable` was needed for the guest clock; without it the desktop guest did not reach the login
prompt. The serial console (`-serial file:...`) is the easiest way to see where a guest stopped.

## Windows behaviour

With OVMF, the Windows ISO booted to `FS0:\EFI\BOOT\BOOTX64.EFI` via the EFI shell; `cdboot.efi`
then loaded and the Tianocore logo appeared, but the Windows Setup screen never did. This happened
for Windows 10 (no TPM, std VGA and QXL) and Windows 11 (with TPM, std VGA). No host panic, no
`vmInvalidExits`, no `assertFailures` — the guest simply did not advance.

Do not spend time on QEMU signing or boot-args to fix that: use
[VirtualBox](VIRTUALBOX.md) for Windows instead.

## Kext requirement for QEMU devices

QEMU maps the TPM TIS MMIO region as a 0x400-byte range at `0xfed45000`. Older AMDHV builds
rejected the sub-page range with `HV_BAD_ARGUMENT` and QEMU aborted while creating the device.
Current builds round the range to page boundaries (commit `356d903`), so the TPM device is created
successfully.

## Troubleshooting

| symptom | cause and fix |
|---|---|
| guest hangs at boot | check `sysctl kern.hv_support` and add `tsc=reliable` for Linux guests |
| `ret = HV_BAD_ARGUMENT ... hvf_vm_unmap` | old kext without the sub-page unmap fix — rebuild/install the current kext |
| Windows stops at Tianocore | QEMU-specific limitation; use VirtualBox |
| host panic | collect the panic log and check for another hypervisor provider before blaming QEMU |
