# QEMU + AMDHV: running Windows 10 / 11

## Quick summary

- **QEMU works.** Both Windows 10 and Windows 11 reach Windows Setup through
  HVF/AMDHV on this host (Ryzen 5 3600, macOS, AMDHV kext 0.3.1).
- The one real trap: OVMF shows **"Press any key to boot from CD or DVD"** when
  booting from the ISO. If you miss it, the VM continues to the (still empty) disk
  and then to **PXE**. That is firmware/ISO behaviour, **not** an AMDHV failure.
- Windows 11 also fails the installer's TPM 2.0 / Secure Boot check. The fix is a
  `LabConfig` bypass entered in Setup (step 4).
- After Windows is installed, remove the ISO drive so the VM boots from disk.

MacPorts QEMU already carries the `com.apple.security.hypervisor` entitlement —
**no signing is needed**. `scripts/qemu-test.sh` is kept for reference only.

---

## Step 1 — Prepare a folder and disk

Run this once. `edk2-vars.fd` **must be a copy** — do not use the file directly from
`/opt/local/share/qemu/` (the shipped file is a template and can be overwritten).

```bash
W=~/vm/win10                       # change the folder name as needed
mkdir -p "$W"
cp /opt/local/share/qemu/edk2-i386-vars.fd "$W/edk2-vars.fd"
/opt/local/bin/qemu-img create -f qcow2 "$W/win10.qcow2" 64G
```

For Windows 11, use a separate folder (for example `~/vm/win11`) and repeat the
commands with a `win11.qcow2` filename.

## Step 2 — Start QEMU

Copy the whole block into Terminal. Replace the ISO path with your own image.

```bash
/opt/local/bin/qemu-system-x86_64 \
  -name "Windows 10 (AMDHV)" \
  -accel hvf -cpu host -smp 4 -m 4096 -machine q35 \
  -display cocoa -vga std \
  -drive if=pflash,format=raw,readonly=on,file=/opt/local/share/qemu/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,file="$W/edk2-vars.fd" \
  -drive file="$W/win10.qcow2",if=none,id=disk0,format=qcow2 \
  -device ich9-ahci,id=ahci -device ide-hd,drive=disk0,bus=ahci.0 \
  -drive file=/path/to/Windows10.iso,media=cdrom,if=none,id=cd0,format=raw,readonly=on \
  -device ide-cd,drive=cd0,bus=ahci.1 \
  -boot d -device qemu-xhci -device usb-tablet \
  -monitor unix:/tmp/amdhv-mon.sock,server=on,wait=off -serial none
```

What the important options do:

| option | purpose |
|---|---|
| `-accel hvf` | use Hypervisor.framework → the AMDHV kext |
| `-cpu host -smp 4 -m 4096` | host CPU, 4 vCPUs, 4 GiB RAM |
| `-machine q35` | modern UEFI chipset used by Windows |
| `-drive if=pflash,...edk2-x86_64-code.fd` | OVMF firmware (UEFI) |
| `-drive if=pflash,...edk2-vars.fd` | UEFI variables (the copy from step 1) |
| `-device ich9-ahci,id=ahci` | SATA controller for the disk and CD-ROM |
| `-boot d` | boot from the CD-ROM |
| `-display cocoa` | native QEMU window on macOS |
| `-monitor unix:/tmp/amdhv-mon.sock,...` | socket for automated commands (step 3) |
| `-serial none` | serial disabled — serial input was correlated with a host watchdog panic on this machine |

## Step 3 — Press the key at the CD prompt

As soon as the QEMU window appears, **click the window first** so keyboard input
goes to the guest. When the screen shows:

```
Press any key to boot from CD or DVD...
```

press **any key** (Return is fine). The window is only open for a few seconds.

If you miss it, the screen changes to:

```
BdsDxe: failed to start Boot0001 "UEFI QEMU DVD-ROM QM00015" ... : Time out
BdsDxe: failed to load Boot0002 "UEFI QEMU HARDDISK QM00013" ... : Not Found
>>Start PXE over IPv4.
```

That means OVMF did not boot from the ISO and fell through to PXE. **This is not an
AMDHV error** — restart the VM (or run `system_reset` through the monitor) and press
the key sooner.

### Optional: send the key automatically

Run this in a **second Terminal** while the VM boots:

```bash
{ for i in $(seq 1 70); do echo 'sendkey ret'; sleep 0.4; done; } | nc -U /tmp/amdhv-mon.sock
```

Important: **one monitor connection must stay open** for the whole sequence. Do not
write `echo 'sendkey ret' | nc -U "$MON"` inside the loop — that opens and closes a
new connection for every key, so the keys never reach the guest and the VM still
goes to PXE.

After this, Windows Setup appears: language screen → **Install now** →
**Activate Windows** → disk selection.

## Step 4 — Windows 11 only: skip the system requirements check

With the same command (a different ISO, disk and vars file), Windows 11 enters
Setup but stops at:

> This PC doesn't currently meet Windows 11 system requirements
> The PC must support TPM 2.0.
> The PC must support Secure Boot.

That check is done by the **Windows installer**, not by AMDHV. The usual QEMU fix —
`-machine q35,smm=on` with `edk2-x86_64-secure-code.fd` — **cannot be used here**:
HVF rejects it with
`System Management Mode not supported by this hypervisor`. A virtual TPM alone
also does not satisfy the Secure Boot check.

Verified workaround:

1. Boot as for Windows 10 and press the key at the CD prompt.
2. At the **language selection** screen, press **Shift+F10**. Command Prompt opens.
3. Type these three commands, pressing Return after each one:

   ```cmd
   reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1
   reg add HKLM\SYSTEM\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1
   reg add HKLM\SYSTEM\Setup\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1
   ```

4. Close Command Prompt with `exit`, then continue with **Next → Install now**.

Verified: Setup moves past the requirements screen into the normal install flow
(keyboard selection, disk selection). This bypass only covers the TPM / Secure Boot
/ RAM checks. If your Windows 11 ISO also enforces a newer CPU generation, that
check is not covered by these commands.

## Step 5 — After Windows is installed

Remove the ISO drive and `-boot d` from the command, then boot straight from the
qcow2 disk. OVMF keeps a `UEFI QEMU HARDDISK` boot entry, so the CD prompt no longer
appears and the VM will not fall through to PXE.

---

## Linux (optional)

The structure is the same. Lubuntu 24.04 needed `tsc=reliable` for the guest clock;
without it the desktop never reached the login prompt:

```bash
qemu-system-x86_64 -accel hvf -cpu host -smp 4 -m 8192 -machine pc,hpet=off \
  -kernel /path/to/casper/vmlinuz -initrd /path/to/casper/initrd \
  -append "boot=casper tsc=reliable quiet splash console=ttyS0" \
  -drive file=lubuntu.qcow2,if=none,id=hd0 \
  -device ich9-ahci,id=ahci -device ide-hd,drive=hd0,bus=ahci.0 \
  -display cocoa -vga std -usbdevice tablet
```

Use `-serial file:...` to see where a Linux guest stopped. Do not use interactive
serial input with Windows on this host.

## Troubleshooting

| symptom | cause and fix |
|---|---|
| `>>Start PXE over IPv4` appears | CD boot key was missed — press a key at the prompt, or boot from the installed disk |
| `BdsDxe: ... DVD-ROM ... Time out` | same boot-key timeout, not an AMDHV failure |
| Windows 11 "system requirements" screen | apply the `LabConfig` bypass in step 4; `smm=on` is unsupported by HVF here |
| `sendkey` does not work, VM still reaches PXE | the monitor connection is closed after every key — keep one connection open |
| Linux hangs before login | add `tsc=reliable` to the kernel command line |
| `ret = HV_BAD_ARGUMENT ... hvf_vm_unmap` | old kext without the sub-page unmap fix — rebuild/install the current kext |
| host panic | collect the panic log and check for another hypervisor provider before blaming QEMU |

## Technical notes

- QEMU maps the TPM TIS MMIO region as a 0x400-byte range at `0xfed45000`. Older
  AMDHV builds rejected the sub-page range with `HV_BAD_ARGUMENT` and QEMU aborted
  while creating the device. Current builds round the range to page boundaries
  (commit `356d903`).
- Verified on this host: the OVMF → `BOOTX64.EFI` → `cdboot.efi` → Setup flow runs
  through to disk selection, with `vmInvalidExits = 0`, `assertFailures = 0`, no
  panic, and no guest errors logged by QEMU.
- These are single-host observations, not a support promise. QEMU is not an
  officially supported client of this project; for Windows,
  [VirtualBox](VIRTUALBOX.md) is the alternative.
