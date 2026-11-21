# Android Studio emulator on AMDHV

The Android emulator 37.3.10 runs a 64-bit x86_64 Android 17 AVD (`Medium_Phone_API_37.0`,
`sdk_gphone64_x86_64`) through Hypervisor.framework on this host. Its bundled
`qemu-system-x86_64` carries `com.apple.security.hypervisor`, so no additional signing is needed.

**Not an officially supported client.** Verified on the reference host: boot completes, ADB is
online, input and network work.

## 1. Check AMDHV is up

```bash
codesign -d --entitlements - ~/Library/Android/sdk/emulator/qemu/darwin-x86_64/qemu-system-x86_64 \
  | grep hypervisor
sysctl kern.hv_support                 # must be 1
sudo build/amd-info  # SVM must be "active on" every CPU
```

Start the emulator after boot.

## 2. Start the AVD

```bash
~/Library/Android/sdk/emulator/emulator -avd Medium_Phone_API_37.0
```

Or start it from Android Studio's Device Manager. Use an **x86_64** system image — ARM images
would be emulated rather than run through HVF.

## 3. Verify it booted

```bash
ADB=~/Library/Android/sdk/platform-tools/adb
$ADB devices -l                        # emulator-5554 device
$ADB shell getprop sys.boot_completed  # 1
$ADB shell getprop init.svc.bootanim   # stopped
$ADB shell getprop ro.build.version.release
$ADB shell ping -c 2 -W 2 8.8.8.8      # network check
$ADB shell input keyevent KEYCODE_HOME # input check
```

The observed guest had `sys.boot_completed=1`, a running launcher, Chrome first-run UI and a
working network.

## 4. Verify it runs through AMDHV

Suspend and resume the emulator's `qemu-system-x86_64` process while watching the counter:

```bash
pgrep -fl 'sdk/emulator/qemu.*qemu-system-x86_64'
# with PID <pid>:
kill -STOP <pid>; sleep 3; sudo build/amd-info stats | grep vmExits   # delta 0
kill -CONT <pid>; sleep 3; sudo build/amd-info stats | grep vmExits   # increases
```

In the test, `vmExits` had a delta of 0 while suspended and grew after resume, confirming that the
emulator runs through HVF/AMDHV. `vmInvalidExits` and `assertFailures` stayed at zero.

## Troubleshooting

| symptom | cause and fix |
|---|---|
| AVD stays on the boot animation | check `sysctl kern.hv_support` and start the emulator after boot |
| `adb devices` is empty | wait for boot, then `$ADB devices`; restart adb with `$ADB kill-server && $ADB start-server` |
| `hv_vm_create` permission denied | the emulator binary lost its `com.apple.security.hypervisor` entitlement (e.g. after a manual copy); reinstall the SDK |
| `kern.hv_support` reads 0 | AMDHV not loaded: check Kernel → Add in `config.plist`, boot-args `amdhv_enable_svm=1 amdhv_hv=1`, and `sudo build/amd-info log` |
