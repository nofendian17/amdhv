# Manual install to the OpenCore EFI

This is the raw, step-by-step equivalent of `scripts/install-to-efi.sh`, for when you want to do
each change yourself or the script cannot parse your `config.plist`.

Work from the repo root after building the kext (`make kext`). Keep a bootable EFI **without**
AMDHV so you can recover from a bad edit.

## 1. Mount the EFI partition and back up the config

```bash
diskutil list                      # find the EFI partition, e.g. disk0s1
sudo diskutil mount disk0s1        # adjust the disk identifier; should appear at /Volumes/EFI
ls /Volumes/EFI/EFI/OC             # must contain config.plist and Kexts/
```

```bash
PLIST=/Volumes/EFI/EFI/OC/config.plist
cp -p "$PLIST" "$PLIST.manual-backup"
```

## 2. Copy the kext

```bash
rm -rf /Volumes/EFI/EFI/OC/Kexts/AMDHV.kext
ditto build/AMDHV.kext /Volumes/EFI/EFI/OC/Kexts/AMDHV.kext
```

Check what was copied:

```bash
/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' \
  /Volumes/EFI/EFI/OC/Kexts/AMDHV.kext/Contents/Info.plist   # com.amdhv.AMDHV
```

## 3. Add the Kernel → Add entry by hand

If your config has no `Kernel` → `Add` array, create it first:

```bash
/usr/libexec/PlistBuddy -c 'Add :Kernel dict' "$PLIST"       # only if :Kernel is missing
/usr/libexec/PlistBuddy -c 'Add :Kernel:Add array' "$PLIST"  # only if :Kernel:Add is missing
```

Append a new entry. `N` must be the next free index in the array:

```bash
N=$(/usr/libexec/PlistBuddy -c 'Print :Kernel:Add' "$PLIST" | grep -c BundlePath || true)
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:$N dict" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:$N:BundlePath string AMDHV.kext" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:$N:Comment string AMDHV - Hypervisor.framework on AMD" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:$N:Enabled bool true" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:$N:ExecutablePath string AMDHV.kext/Contents/MacOS/AMDHV" "$PLIST"
/usr/libexec/PlistBuddy -c "Add :Kernel:Add:$N:PlistPath string AMDHV.kext/Contents/Info.plist" "$PLIST"
```

The fields are:

| field | value |
| --- | --- |
| `BundlePath` | `AMDHV.kext` (relative to `EFI/OC/Kexts/`) |
| `Comment` | anything descriptive |
| `Enabled` | `true` |
| `ExecutablePath` | `AMDHV.kext/Contents/MacOS/AMDHV` |
| `PlistPath` | `AMDHV.kext/Contents/Info.plist` |
| `MinKernel` / `MaxKernel` | leave empty — do not restrict the macOS version |

OpenCore also accepts paths relative to the kext bundle itself (`Contents/MacOS/AMDHV`,
`Contents/Info.plist`); the EFI on the reference machine uses that form with `Arch = Any`.
Both work — do not add both entries.

Skip this step entirely if `Print :Kernel:Add` already shows an `AMDHV.kext` entry.

## 4. Add the boot-args

The two required arguments are `amdhv_enable_svm=1` and `amdhv_hv=1`. They live in the GUID
`7C436110-AB2A-4BBB-A880-FE41995C9F82` under `NVRAM → Add`:

```bash
/usr/libexec/PlistBuddy -c \
  'Print :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args' "$PLIST"
```

`Set` replaces the whole string, so include your existing arguments. On the reference machine the
final value is:

```
-v alcid=1 revcpu=1 amdhv_enable_svm=1 amdhv_hv=1 agdpmod=pikera vsmchbkp=0
```

```bash
/usr/libexec/PlistBuddy -c \
  'Set :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args -v alcid=1 revcpu=1 amdhv_enable_svm=1 amdhv_hv=1 agdpmod=pikera vsmchbkp=0' \
  "$PLIST"
```

Only `amdhv_enable_svm=1 amdhv_hv=1` are AMDHV's own. The others are host-specific: `revcpu=1`
helps AMD topologies, and `vsmchbkp=0` works around a Lilu/VirtualSMC `resetSystem` panic seen on
this machine — it is not an AMDHV requirement. Optional AMDHV flags such as `amdhv_apic=1`
(in-kernel APIC) or `amdhv_asid=0` (no per-vCPU ASIDs) are not added by the installer; add them to
the same `boot-args` string by hand when needed. See the [boot-args table](../README.md#boot-args).

## 5. Validate the plist before rebooting

```bash
plutil -lint "$PLIST"
/usr/libexec/PlistBuddy -c 'Print :Kernel:Add' "$PLIST" | grep -A6 'AMDHV.kext'
/usr/libexec/PlistBuddy -c \
  'Print :NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args' "$PLIST"
```

## 6. Reboot and verify

Boot from this EFI (select the EFI/OpenCore entry in the boot picker), then:

```bash
sysctl kern.hv_support                 # must print 1
sudo build/amd-info                    # SVM must be "active on" every CPU
sudo build/amd-info selftest           # optional: verify SVM on every CPU
sudo build/amd-info stats              # vmCreates/vmExits grow when a client VM runs
```

If `kern.hv_support` is still 0, the kext did not load: re-check the Kernel → Add entry, the
boot-args, and `sudo build/amd-info log`.

## Rollback

```bash
cp -p "$PLIST.manual-backup" "$PLIST"
rm -rf /Volumes/EFI/EFI/OC/Kexts/AMDHV.kext
```

Then reboot. Also remove a RyzenHV entry from `EFI/OC/Kexts` and Kernel → Add if one is present —
two hypervisor providers cannot coexist.

## GUI alternative

In OpenCore Configurator or ProperTree: open `EFI/OC/config.plist`, go to **Kernel → Add**, add a
dict with the six fields from step 3, then go to **NVRAM → Add → 7C436110-... → boot-args** and
append the two AMDHV arguments. Save, then run the `plutil -lint` check above.
