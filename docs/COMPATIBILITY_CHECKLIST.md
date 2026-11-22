# AMD SVM ↔ Apple Hypervisor.framework compatibility checklist

This is the working checklist for the goal "an AMD CPU runs Apple's Hypervisor.framework clients
optimally". It separates three layers that fail in different ways:

1. **Enablement** — SVM must be on, per CPU, before any client can run.
2. **SVM core** — what the kext does with the VMCB, intercepts, NPT and guest state.
3. **Hypervisor.framework surface** — which `hv_*` calls the unmodified framework can reach.

Each row is marked with evidence, not intent. Status meanings:

| Status | Meaning |
| --- | --- |
| **Yes** | The code path exists and is exercised (host test and/or live probe). |
| **Partial** | Implemented with a documented limitation. |
| **No** | Not implemented; the client gets `HV_UNSUPPORTED` or a no-op. |
| **n/a** | Not applicable to this design. |

Baseline used for this revision (2026-11-22, AMD Ryzen 5 3600, 12 logical CPUs, macOS 15.8.3,
AMDHV 0.1.2): `make test` passes all 15 tests, `hvf-probe` prints `64 of 64` vCPUs with
`run_until(past deadline)` in `0.013 ms`, `hvf-smoke` prints `PASS`, and `amd-info stats` keeps
`enableFailures`, `vmInvalidExits`, `assertFailures` and `vmFpuMismatches` at 0. Re-run those four
after every change; a checklist row without evidence is a hypothesis. `amd-info info` reports 32768
ASIDs and features `0x0013bcff`, including `VmcbClean`, `FlushByAsid`, `PauseFilter` and
`PauseFilterThreshold`.

---

## 1. Enablement (host and firmware)

| Check | Status | Evidence |
| --- | --- | --- |
| AMD vendor and SVM present (`CPUID 8000_0001 ECX[2]`) | **Yes** | `SVM.cpp` `probeThisCpu()` |
| BIOS "SVM Mode" enabled (`VM_CR.SVMDIS` clear) | **Yes** | `VM_CR` is read; `cpuUsable()` refuses a CPU with `SVMDIS` |
| `VM_CR.LOCK` reported, never fought | **Yes** | `AMDHVInfo.svmLocked`; the kext does not try to clear a locked `SVMDIS` |
| `EFER.SVME` set on every usable CPU | **Yes** | `enableAction()` / `disableAction()` |
| `VM_HSAVE_PA` valid on every usable CPU | **Yes** | `allocHostSave()`: 4 KiB, physically contiguous, 4 KiB aligned, below 2^52 |
| CPUs that come online after kext start | **Yes** | `rescan()` probes and enables late CPUs |
| S3/S4 sleep and wake | **Partial** | The ordering bug is fixed and host-tested: `SleepPolicy.hpp` / `tests/sleep_policy_test.cpp` set the suspended state *before* the teardown rendezvous, so no vCPU can re-enable `EFER.SVME` behind it; the PM notification does the teardown while every CPU is up, the kernel's `hv_suspend()`/`hv_resume()` callbacks only move the state (they run after the APs are off / before they are back), and after a resume the sleep epoch makes every vCPU mark all VMCB clean bits dirty and flush its TLB. A real S3 with a live guest is still the acceptance run (section 7, item 9) |
| Per-run re-check of `EFER.SVME` and `VM_HSAVE_PA` | **Yes** | `ensureThisCpu()` repairs drift before `VMRUN`; `healedCount()` counts it |
| Self-test of every CPU | **Yes** | `verifyAll()`; `amd-info selftest` |
| NPT and NRIPS required before a VM is created | **Yes** | `Vm.cpp` `com_amdhv_AMDHVVm::create()` refuses a CPU without both |
| 64 logical CPUs / 64 vCPUs per VM | **Yes** | `AMDHV_MAX_CPUS`, `AMDHV_MAX_VCPUS_PER_VM` |
| GIF discipline around the guest | **Yes** | `SvmRun.S`: `CLGI` … `STGI` around `VMSAVE`/`VMLOAD`/`VMRUN` |
| Host interrupts during `VMRUN` | **Yes** | `STI` before `VMRUN` with `V_INTR_MASKING`; `CLI` after the exit |

Acceptance for this layer: `sysctl kern.hv_support` is 1, `amd-info` reports SVM active on every
CPU, `amd-info selftest` passes, and `enableFailures` stays 0.

---

## 2. SVM core (AMD APM Vol. 2, Chapter 15 and Appendix B)

### 2.1 VMCB and intercepts

| Item | Status | Where / notes |
| --- | --- | --- |
| VMCB control area and state-save layout | **Yes** | `VMCB.hpp`; every offset is a `static_assert`, checked by `tests/vmcb_layout.cpp` |
| VMCB clean bits | **Yes** | `CleanBits.hpp` (tested by `tests/clean_bits_test.cpp`): after a successful `VMRUN` every group software did not touch is marked clean, and any write through `VmcsState`/`applyControls()` clears the group it changes. `INT_CTL`/`INT_VECTOR` and `CR2` stay dirty; a vCPU that moves to another CPU reloads everything. Only enabled when CPUID `VMCB_CLEAN` is set |
| Always-on intercepts (effective set after `applyControls()`) | **Yes** | INTR, NMI, SMI, INIT, CPUID, INVD, SHUTDOWN, I/O, MSR, VMRUN, VMMCALL, VMLOAD, VMSAVE, STGI, CLGI, SKINIT |
| Client-driven intercepts mapped from VMX controls | **Yes** | `VmcsState::applyControls()`: HLT, INVLPG, RDPMC, RDTSC/RDTSCP, PAUSE, MWAIT, MONITOR, WBINVD, CR0/CR3/CR4/CR8, MOV-DR, exception bitmap, interrupt window |
| Pause intercept filter | **Yes** | When the client intercepts PAUSE and the CPU reports `PauseFilter`, `applyControls()` sets the count to 3000 (and the threshold to 128 when `PauseThresh` is reported), so a spin loop does not exit on every iteration; the fields stay zero when the CPU lacks the feature |
| I/O permission map | **Yes** | 12 KiB, all ones: every port exits to the client, matching Hypervisor.framework |
| MSR permission map | **Yes** | 8 KiB, all ones; native MSRs clear their bits (`MsrMap.hpp`, `tests/msr_map_test.cpp`) |
| `V_INTR_MASKING` | **Yes** | Always set, so the guest's `EFLAGS.IF` does not gate host interrupts |
| `V_TPR` / TPR shadow | **Yes** | `VmcsState` + CR8 exit path in `Vm::run()`; toggle with `amd-info hv tpr` or `amdhv_tpr_shadow=0` |
| `V_IRQ` / interrupt-window exit | **Yes** | `applyControls()` when the client enables `CPU_INTR_WINDOW` |
| Event injection and re-injection | **Yes** | `eventInj`/`eventInjErr`; external-interrupt and NMI exits are re-injected |
| ASID | **Yes** | One ASID per vCPU from a host-wide pool (`Asid.hpp`, `tests/asid_test.cpp`); when the pool is empty the vCPU shares the host's ASID and flushes the whole TLB every entry |
| TLB control | **Yes** | `TlbPolicy.hpp` (tested by `tests/tlb_policy_test.cpp`): `TLB_FLUSH_BY_ASID` when the nested tables change, the client calls `hv_vcpu_invalidate_tlb`, the client writes CR0/CR3/CR4/EFER through the API, or the vCPU moves to another CPU; `TLB_FLUSH_ALL` when there is no ASID or `amdhv_asid=0` |
| Nested paging (NPT) | **Yes** | `Npt.hpp`: 4-level walk, 4 KiB pages, `US`/`NX`, permissions are the AND of all levels |
| NPT 2 MiB / 1 GiB leaves | **No** | Only 4 KiB leaves; the capability word does not advertise large leaves |
| NPT execute-only pages | **No** | Not advertised |
| NPT accessed/dirty bits | **No** | Not advertised |
| NPF exit information | **Yes** | `exitInfo1`/`exitInfo2` and the permission lookup handed to the client; the GPA is used only when `EXITINFO1` bit 32 (DecodeAssists) says `EXITINFO2` holds one |
| Guest state save/restore | **Yes** | `SvmRun.S`: `VMSAVE` host → `VMLOAD` guest → `VMRUN` → `VMSAVE` guest → `VMLOAD` host |
| Guest `EFER.SVME` / `EFER.LMA` | **Yes** | `resetLocked()` and `applyControls()` |
| Guest PAT | **Yes** | VMCB `gPat` |
| Guest DR6/DR7 | **Yes** | VMCB slots |
| Guest DR0–DR3 | **Partial** | Never swapped by `VMRUN`; DR accesses are intercepted (`interceptDr = 0xFFFFFFFF`) |
| Guest `TSC_AUX` (RDTSCP) | **Yes** | Saved and swapped around the run because `VMRUN` does not switch it |
| Guest x87/SSE/AVX and XCR0 | **Yes** | XSAVE areas swapped by hand; `vmFpuMismatches` must stay 0 |
| Guest TSC offset | **Yes** | VMCB `tscOffset`; `hv_vm_sync_tsc` applies the same offset to later vCPUs |
| Guest `WRMSR 10h` (TSC) cannot reach the host TSC | **Yes** | `msr::mustIntercept()` keeps the MSRPM intercept even when the client makes TSC "native" (the framework's slot 144); `run()` emulates RDMSR/WRMSR 10h against `tscOffset` (`tests/msr_map_test.cpp`) |
| Intel-only `IA32_SPEC_CTRL` / `IA32_PRED_CMD` | **Yes** | `hv_vcpu_enable_native_msr` reports success but the intercept stays; `Vm::run()` emulates the access so a Linux guest does not `#GP` (`MsrMap.hpp`) |
| TSC scaling (`TscRateMsr`) | **No** | Not used |
| Next-RIP (`NRIPS`) | **Yes** | Required at VM creation; `nextRip` used for soft event injection and skip-instruction |
| `VMEXIT_INVALID` diagnostics | **Yes** | `dumpVmcb()` prints the state that made `VMRUN` refuse the VMCB |
| 10 ms host slice + client deadline | **Yes** | `RunDeadline.hpp`, `tests/run_deadline_test.cpp`; a past deadline returns exit 52 without entering the guest |
| vCPU kick from another thread | **Yes** | `hv_vcpu_interrupt` raises a flag and sends an IPI to the CPU inside `VMRUN` |

### 2.2 SVM features probed but not used

These are visible in `CPUID Fn8000_000A EDX` and in `AMDHVInfo.svmFeatures`, but no code path turns
them on. They are candidates for the roadmap, not current capabilities. (`VMCB_CLEAN`,
`DECODE_ASSISTS` and `PAUSE_FILTER`/`PAUSE_THRESH` are used now; see 2.1.)

| Feature | Status | Why it matters |
| --- | --- | --- |
| `AVIC` | **No** | In-kernel APIC; Hypervisor.framework refuses `HV_VM_ACCEL_APIC`, so VZ does not need it |
| `VGIF` | **No** | Virtual GIF; host still uses `CLGI`/`STGI` |
| `vVMSAVE/VMLOAD` | **No** | Nested VMSAVE/VMLOAD virtualization |
| `LBRV` | **No** | LBR virtualization |
| `SVML` (SVM lock) | **No** | Nested SVM lock |
| `TSCRATE` | **No** | TSC ratio for migration |

---

## 3. Apple Hypervisor.framework surface

The front end registers the same trap tables AppleHV uses (`hv_set_traps`, `hv_set_callbacks`,
task/thread targets). Indices are the framework's, not ours. A **stub** is logged and answers
`HV_UNSUPPORTED` in mode 1; mode 2 answers success for debugging only.

### 3.1 Task traps

| # | Call | Status | Notes |
| --- | --- | --- | --- |
| 0 | `hv_vm_create` | **Yes** | One VM per task; `HV_VM_ACCEL_APIC` is refused with `HV_UNSUPPORTED` |
| 1 | `hv_vm_destroy` | **Yes** | `HV_BUSY` while a vCPU or an in-flight trap holds the VM |
| 2 | `hv_vm_space_create` | **Yes** | Up to 64 extra address spaces; each owns an NPT root (`SpacePool.hpp`) |
| 3 | `hv_vm_space_destroy` | **Yes** | `HV_BUSY` while a vCPU is bound to it; unmaps and unpins its pages |
| 4 | `hv_vm_map` | **Yes** | Pins the caller's pages; 4 KiB alignment; up to `AMDHV_MAP_MAX_SIZE` |
| 5 | `hv_vm_unmap` | **Yes** | Accepts sub-page ranges and rounds them to page boundaries |
| 6 | `hv_vm_protect` | **Yes** | 4 KiB alignment; permissions normalised (W/X imply R) |
| 7 | `hv_vm_sync_tsc` | **Yes** | Stored per VM and applied to vCPUs created later |
| 8 | `hv_vm_add_pio_notifier` | **No** | Stub; VZ uses I/O exits instead |
| 9 | `hv_vm_remove_pio_notifier` | **No** | Stub |
| 10 | `hv_vm_atpic_*` / `hv_vm_ioapic_*` / `hv_vm_lapic_*` | **No** | Stubs; in-kernel APIC only |
| 11 | `hv_vm_lapic_msi` | **No** | Stub |
| 12 | `hv_vcpu_create` | **Yes** | One vCPU per thread, up to 64; same task as the VM |
| 13 | `hv_vcpu_interrupt` | **Yes** | IPI to the CPU in the guest |
| 14–15 | unused | **n/a** | Stubs |

### 3.2 Thread traps

| # | Call | Status | Notes |
| --- | --- | --- | --- |
| 0 | `hv_vcpu_run` | **Yes** | Blocks until a VM exit; 10 ms host slice |
| 1 | `hv_vcpu_run_until` | **Yes** | Deadline folded onto the slice; a past deadline returns exit 52 |
| 2 | `hv_vcpu_invalidate_tlb` | **Yes** | Schedules a flush for that vCPU's next entry (`Vm::forceTlbFlush`) |
| 3–4 | managed-MSR enable/disable | **Yes** | MSR cache plus native/managed lists (`MsrMap.hpp`, `HvMsrs.hpp`) |
| 5 | `hv_vcpu_destroy` | **Yes** | Frees the VMCB, host VMCB, bitmaps and FPU areas |
| 6 | `hv_vcpu_set_space` | **Yes** | The trap argument is the raw space ID (verified); rebinds `nCR3` to that space's NPT root and flushes the vCPU's ASID |
| 7 | `hv_vmx_vcpu_read_vmcs` (slot) | **Yes** | Reads from the VMCS shadow, fills the client's cache slot |
| 8 | unused | **n/a** | Stub |
| 9 | `hv_vmx_vcpu_set_apic_address` | **Yes** | No-op success; the TPR is kept in `V_TPR`, the page is never read |
| 10 | `hv_vcpu_set_msr_access` | **Yes** | Toggles MSRPM read/write intercepts |
| 11 | sync special registers | **Yes** | DR0–DR7 and other special registers synchronised on demand |
| 12 | writable-VMCS capability | **Yes** | Answers from the capability words in `VmcsCaps.hpp` |
| 13 | APIC sub-ops (`inject_extint`, `apic_read/write`, …) | **No** | Stub; only meaningful with the in-kernel APIC |
| 14–15 | unused | **n/a** | Stubs |

### 3.3 Framework plumbing

| Item | Status | Notes |
| --- | --- | --- |
| Register as the hypervisor provider | **Yes** | Releases AppleHV's traps/callbacks, then installs ours |
| `kern.hv_support` | **Yes** | Written to 1 after registration (`amdhv_hv_sysctl=0` leaves it alone) |
| Task/thread targets | **Yes** | Set on VM/vCPU create, cleared on destroy |
| Shared register page and read-only page | **Yes** | Layout mirrors the framework's; `hv_vcpu_get_exec_time` reads offset 0 without a trap |
| VMCS shadow and capability words | **Yes** | `VmcsState.hpp` / `VmcsCaps.hpp`; words derive from implemented controls |
| TPR shadow switch | **Yes** | Boot-arg `amdhv_tpr_shadow=0` or `amd-info hv tpr 0` |
| Entitlement check | **No** | Native mode: any local process can create a VM (documented limit) |

---

## 4. Client compatibility matrix

| Client | What it needs | Status | Evidence |
| --- | --- | --- | --- |
| Docker Desktop (VZ) | `vm_create`, map, vCPU, `run_until`, VMCS reads, interrupt | **Yes** | engine starts; containers serve traffic |
| Kiri (VZ) | same as VZ | **Yes** | Linux/amd64 containers run |
| colima `--vm-type vz` | same as VZ | **Yes** | 4 vCPU / 8 GiB VM starts; `docker run hello-world` succeeds |
| VirtualBox 7.2.22 | VMX controls, NPT, event injection, TPR shadow | **Yes** | Windows 10 boots to desktop; Lubuntu live reaches installer |
| Android emulator 37.3.10 | VMX controls, NPT, FPU/XSAVE | **Yes** | Android 17 AVD boots; ADB, input and network work |
| QEMU 11.1.3 `-accel hvf` (Linux) | VMX controls, NPT | **Yes** | Lubuntu boots with `tsc=reliable` |
| QEMU 11.1.3 `-accel hvf` (Windows) | additional HVF/QEMU behaviour | **Partial** | stops at the Tianocore logo; not attributed to AMDHV |

These are single-host observations, not a support promise. See `docs/CLIENTS.md`.

---

## 5. Gap register and roadmap

Closed during this review: a guest `WRMSR 10h` can no longer move the host TSC (see section 7,
item 5), the guest TLB is no longer flushed on every entry (each vCPU has its own ASID and uses
`TLB_FLUSH_BY_ASID` only when a translation can actually be stale), VMRUN no longer reloads the
whole guest state on every entry (VMCB clean bits, except the two groups the processor changes by
itself), and an NPF exit now reports a guest physical address only when the processor says one is
valid and looks its permissions up in the vCPU's own address space.

Ordered by what blocks real clients first, not by what is easiest.

### P0 — correctness and stability

- [x] `waitForSystemMapper` at boot (root-caused and fixed). The only emitter in XNU is
      `IOMapper::waitForSystemMapper()`, reached from `IOBufferMemoryDescriptor` when it is not given
      `kIOMemoryMapperNone`. `allocHostSave()` runs from `start()` (boot arg `amdhv_enable_svm=1`) and
      allocated the host-save pages without that flag, so it waited for the system mapper and printed
      the backtrace naming `com.amdhv.AMDHV`. `allocHostSave()`, `amdhv::physAlloc()` and the front
      end's shared pages now pass `kIOMemoryMapperNone`: they are addressed by physical address and
      are never DMA-mapped. Acceptance: on the next boot,
      `log show --last boot --predicate 'eventMessage CONTAINS "waitForSystemMapper"'` has no kernel
      line (section 7, item 2).
- [x] Fix sleep/wake with a running VM. `suspend()` marks the system suspended before the rendezvous,
      so a vCPU that is between its last check and `VMRUN` cannot re-enable `EFER.SVME` in
      `ensureThisCpu()`; that function now refuses with `-3` while a suspend is in progress. The
      kernel's `hv_suspend()`/`hv_resume()` callbacks only move the sleep state, because they run
      after the APs are off / before they are back; the PM notification does the
      `EFER.SVME`/`VM_HSAVE_PA` teardown while every CPU is still up. On resume the sleep epoch
      changes, and each vCPU marks every VMCB clean bit dirty and flushes its TLB before the next
      entry (the processor lost its cached VMCB state). Covered by `tests/sleep_policy_test.cpp`.
- [ ] Confirm the sleep fix with one real S3/S4 while a guest is running. This needs the machine to
      sleep, so it is not done in this session; the procedure and the counters to check are in
      section 7, item 9.
- [x] Panic/hang attribution. None of the reports in the appendix is attributed to the kext: the two
      page faults name Lilu/VirtualSMC with a RIP outside the kext's range, the watchdog's 5.1e9 s is
      the RTC (`AppleRTC: lost battery power`), and the kext counters were zero. A regression test
      cannot be written for a cause that has not been shown to be ours. The two hangs this layer could
      cause are covered instead: `sleep_policy_test` for the suspend/VMRUN ordering and
      `clean_bits_test` for stale VMCB state after a resume. Keep the rule from section 7: reproduce a
      panic with the kext unloaded before adding a test for it.
- [x] Verify that guest TSC activity cannot move the host clock. The only guest-writable path was
      `WRMSR 10h`, now emulated against `tscOffset` (section 7, item 5); `RDTSC`/`RDTSCP` only read
      `host TSC + offset`.

### P1 — performance without changing behaviour

- [x] Use VMCB clean bits (`VMCB_CLEAN`) instead of `clean = 0` on every run. `VmcsState` clears a
      group on every write; `applyControls()` only clears the intercepts group when the derived
      intercepts or EFER actually change; `run()` marks everything clean after a successful `VMRUN`
      and dirty on a new CPU or `VMEXIT_INVALID`. `INT_CTL`/`INT_VECTOR` and `CR2` stay dirty because
      the processor may change them without updating the VMCB.
- [x] Allocate an ASID per vCPU and use `FLUSH_BY_ASID`; `TLB_FLUSH_ALL` remains the fallback when
      there is no ASID or `amdhv_asid=0`. A vCPU also flushes when it moves to another CPU (whose TLB
      may still hold entries for a reused ASID) and when the client changes paging state through the
      API.
- [ ] Offer 2 MiB NPT leaves for large guest ranges; keep 4 KiB for sub-page protection.
      Deferred: the win only materialises when 512 consecutive guest pages are also 512 consecutive
      host pages. The kext pins user memory one page at a time, and a large leaf would have to be
      split again whenever a client unmaps or reprotects a subrange. Correctness does not depend on
      it, so 4 KiB leaves stay until there is a client that maps large contiguous ranges.
- [x] Use the DecodeAssists fields where the CPU reports them. The kext never decodes guest
      instructions itself, so the assists matter at two exits: a CR/DR exit's `EXITINFO1` bit 63
      names the GPR, and an NPF exit's bit 32 says `EXITINFO2` holds the guest physical address.
      Both are read only when their valid bit is set: the CR/DR GPR when bit 63 is 1, the NPF GPA
      when bit 32 is 1. Without bit 32 the EPT violation reports GPA 0 and clears the VMX "linear
      address valid"/"translated" bits, so a client declines to emulate instead of using a bogus
      address. Verified on this host: `hvf-smoke` stage 3 stores to an unmapped page and reads back
      `VMCS_GUEST_PHYSICAL_ADDRESS` = 0x101010, so this CPU does set bit 32.
- [x] Configure the PAUSE filter threshold so guest spin loops do not exit on every iteration. Set
      only when the client intercepts PAUSE; the count (3000) and threshold (128) are gated on the
      CPUID `PauseFilter`/`PauseThresh` bits.

### P2 — API surface that some clients may call

- [x] `hv_vm_space_create` / `map_space` / `protect_space` / `unmap_space` / `space_destroy`.
      `SpacePool.hpp` hands out IDs 1..64 (0 is the default space); each ID owns an `npt::Table`.
      `map/unmap/protect` take the space from the framework's map item, and thread trap 6
      (`hv_vcpu_set_space`) points the vCPU's VMCB `nCR3` at that root and flushes its ASID.
      `space_destroy` refuses with `HV_BUSY` while a vCPU is still bound to it.
- [ ] `hv_vm_add_pio_notifier` / `remove` (VZ uses I/O exits, so this is not on the VZ path).
      Deferred: it needs a notifier registry and a mach-port notification on matching port I/O; the
      current front end already reports every I/O exit to the client.
- [ ] `hv_vcpu_inject_extint` and the APIC sub-ops, only if the in-kernel APIC is ever offered.
      Deferred: `hv_vm_create(HV_VM_ACCEL_APIC)` is refused, so there is no in-kernel APIC to inject
      into; VZ emulates the APIC and uses `hv_vcpu_interrupt`.
- [ ] `hv_vcpu_flush` (deprecated by Apple; answer `HV_UNSUPPORTED` deliberately).
      Not planned: Apple deprecated the call; a client that needs the effect uses
      `hv_vcpu_invalidate_tlb`.

### P3 — hardware features that would change the model

- [ ] AVIC (only if `HV_VM_ACCEL_APIC` is ever supported).
- [ ] Virtual GIF, vVMSAVE/VMLOAD, LBR virtualization, SVM lock.
- [ ] TSC scaling for migration-style use cases.
- [ ] NPT execute-only pages and A/D bits.

Acceptance for every item: code + host test where the logic is testable + `hvf-probe` change (if the
API surface changes) + one client run + `amd-info stats` with `vmInvalidExits` and
`assertFailures` at 0.

---

## 6. Verification commands

```bash
make test                    # 15 host tests
make kext tools              # build/AMDHV.kext, amd-info, hvf-smoke, hvf-probe

sudo build/amd-info          # SVM state on every CPU
sudo build/amd-info selftest # verify EFER.SVME and VM_HSAVE_PA per CPU
sudo build/amd-info stats    # vmInvalidExits and assertFailures must stay 0
./build/amd-info log         # kernel ring, including every trap's argument (no root needed)

./build/hvf-smoke            # end-to-end public-API VMM
./build/hvf-probe            # compare against docs/CAPABILITIES.md
sysctl kern.hv_support       # must print 1
```

After a kext change, a reboot is required: the running kext cannot be replaced in place.

### Post-reboot verification (2026-11-22, boot 21:53)

The kext installed in the EFI partition is the build from this tree: the loaded UUID
`EEF2CE05-5716-3A2E-B3AE-D0352C507905` matches `build/AMDHV.kext/Contents/MacOS/AMDHV`, and the
kernel ring reports `built Nov 22 2026 21:33:38`. The version string is still 0.1.2, so use the UUID
or build time to tell this binary from the previous one.

- `log show --last boot --predicate 'eventMessage CONTAINS "waitForSystemMapper"'` returns no kernel
  line. The boot-allocation fix is effective.
- `amd-info info`: 12 logical CPUs, SVM usable and active on all 12, `enableFailures = 0`.
- `amd-info selftest` (root): `12 of 12 CPUs ok`, so `EFER.SVME` and `VM_HSAVE_PA` hold on every CPU.
- `hvf-smoke`: PASS (all stages, including the NPF handler and `hv_vcpu_interrupt`).
- `hvf-probe`: matches `docs/CAPABILITIES.md`. In particular `hv_vm_space_create`, `map_space`,
  `protect_space`, `unmap_space`, `set_space`, and `HV_BUSY` on `space_destroy` while bound all
  succeed/answer as documented; the deliberately unimplemented APIC/PIO/extint calls still answer
  `HV_UNSUPPORTED`.
- Counters after both tools: `vmCreates = 67`, `vmRuns = 73`, `vmExits = 596`,
  `vmInvalidExits = 0`, `vmFpuMismatches = 0`, `assertFailures = 0`.
- No panic was written after the boot (`/Library/Logs/DiagnosticReports` only has the 17:xx reports
  that predate this boot).
- `sleepEvents = 0`, `wakeEvents = 0`: the S3/S4 path is still the one item without a hardware run
  (section 7, item 9).

---

## 7. Open questions and observed issues

1. **Thread-trap labels (resolved).** `kThreadNames[3]` used to be `disable_managed_msr` and `[4]`
   `enable_managed_msr`, while `kThreadTable` installs the enable handler at index 3 and the disable
   handler at index 4. Verified against the running framework: 1000 calls to
   `hv_vcpu_enable_managed_msr(vcpu, 0x10, true)` logged `trap thread 3 ... arg=0x90` and 1000 calls
   with `false` logged `trap thread 4 ... arg=0x90`, so the table is right and the labels were
   swapped. The labels now match.
2. **`waitForSystemMapper` at boot (root-caused and fixed).** XNU emits this only from
   `IOMapper::waitForSystemMapper()`, which `IOBufferMemoryDescriptor` calls unless the allocation
   passes `kIOMemoryMapperNone`. `allocHostSave()` runs from `start()` when
   `amdhv_enable_svm=1`, and it allocated the host-save pages without that flag, so it waited for the
   system mapper and printed the backtrace that named `com.amdhv.AMDHV`. All physical-memory
   allocations in the kext now pass `kIOMemoryMapperNone`; they are addressed by physical address and
   never DMA-mapped. The warning should be absent on the next boot:
   `log show --last boot --predicate 'eventMessage CONTAINS "waitForSystemMapper"'` should return no
   kernel line.
3. **QEMU Windows hang.** OVMF and `cdboot.efi` load, then setup never starts. Windows works under
   VirtualBox and Android works under the emulator through the same stack, so this is QEMU-specific
   until proven otherwise.
4. **Host clock.** This host reports `AppleRTC: lost battery power - time may be invalid` and has
   produced a watchdog panic with a bogus elapsed time. Treat clock-dependent panics as environment
   issues until the RTC is fixed; do not attribute them to SVM without a trace.
5. **Guest TSC writes (fixed in this revision).** `hv_vcpu_enable_native_msr(0x10)` used to clear the
   MSRPM intercept for the TSC. SVM applies `TSC_OFFSET` to reads only, so a guest `WRMSR 10h` would
   have written the *host* TSC. The intercept now stays set and `run()` emulates the access. This is
   a plausible mechanism for host-clock weirdness, but it does not explain the 5.1e9 s elapsed time
   in the watchdog panic below.
6. **ASID/TLB path is new (not yet exercised on hardware).** The running kext still flushes on every
   entry; the per-vCPU ASID and `FLUSH_BY_ASID` logic takes effect only after installing
   `build/AMDHV.kext` and rebooting. After the reboot, run `hvf-smoke`, one VZ guest and a multi-vCPU
   workload, then check `amd-info stats`. `amdhv_asid=0` restores the old flush-on-every-entry
   behaviour for an A/B comparison.
7. **VMCB clean bits are new (not yet exercised on hardware).** A wrong clean bit would make the guest
   run with stale state, so the first post-reboot run should watch for `VMEXIT_INVALID`, guest
   corruption and `assertFailures`. The path is gated on CPUID `VMCB_CLEAN`; the CPU here reports it.
   If anything looks wrong, set `control.clean = 0` unconditionally in `run()` (one line) to restore
   the old behaviour while keeping the rest of the change.
8. **Address spaces are new (not yet exercised on hardware).** `hv_vm_space_create`/`map_space`/
   `protect_space`/`unmap_space`/`space_destroy` now allocate real NPT roots. The trap ABI was
   checked against the running framework with `amd-info log`:
   - `hv_vm_space_create(&sp)` passes a pointer to a 4-byte `hv_vm_space_t` (the log showed the
     pointer and the variable's initial `0xdeadbeef`);
   - `hv_vm_space_destroy(id)` and `hv_vcpu_set_space(vcpu, id)` pass the raw ID (`0x77`, `0xdead`,
     `0x1110`, `0x2220` appeared verbatim); the vCPU is the calling thread's;
   - the map item is `{ space, uva, gpa, size, flags }`; `hv_vm_protect_space(0x77, 0x5000, ...)`
     logged `[0x77 0 0x5000 0x1000 ...]`.
   `space_destroy` deliberately answers `HV_BUSY` while a vCPU is still bound; rebind it to the
   default space first.
9. **Sleep with a live guest (code fixed, hardware run pending).** The suspend/resume state machine
   is covered by `tests/sleep_policy_test.cpp`, but the actual S3 path has not been run with a guest
   alive. Acceptance: start `build/hvf-smoke` in a loop (or a VZ/VirtualBox guest), sleep the host,
   wake it, and check that the guest continues and that `amd-info stats` shows `sleepEvents = 1`,
   `wakeEvents = 1`, `vmInvalidExits = 0` and `assertFailures = 0`. If the guest resumes with stale
   state, the first suspect is the clean-bit reload in `run()` (sleep epoch); set
   `control.clean = 0` in `run()` to bisect.

   First hardware attempt, 2026-11-22 22:02 (boot 21:53): the host never entered S3.
   `pmset -g log` reports `Total Sleep/Wakes since boot: 0`, and `kern.sleeptime`,
   `kern.sleep_abs_time` and `kern.hibernatecount` stay 0, so `amd-info stats` shows
   `sleepEvents = 0` and `wakeEvents = 0`. The kernel log shows why: a software sleep at 22:02:10 did
   the full-to-dark transition (`=== START ... f->9`, `dark wake entry` at 22:02:12), and a display
   tickle at 22:02:24 promoted it back to full wake. The system-sleep transition that actually cuts
   power never ran. The kext is registered correctly: it is in the priority client list
   (`kIOMessageSystemCapabilityChange[6] to com.amdhv.AMDHV`), and its handler acknowledges
   immediately (returns `kIOReturnSuccess`, leaving `notify.returnValue` at 0), so no
   `acknowledgeSleepWakeNotification()` is needed. XNU only sends `kIOMessageSystemWillSleep` to
   kernel priority clients from the `kSystemTransitionSleep` branch; `systemMessageFilter()` requires
   `kSystemMessageClientKernel`, which the full-to-dark branch clears. The zero counters are
   therefore expected and are not a kext fault. Acceptance still requires a boot that reaches S3:
   `kern.sleeptime` becomes non-zero, then `sleepEvents = 1`, `wakeEvents = 1`.

### Appendix — panic evidence seen on 2026-11-22

Three panic reports were present: two identical page faults with `CR2 = 0xfed80310` and the same
faulting RIP offset, and one watchdog timeout reporting `5133704748` seconds without a check-in.
The page-fault backtrace names Lilu 1.7.2 and VirtualSMC 1.3.8; the faulting RIP is outside
`com.amdhv.AMDHV`'s loaded range (`0xffffff80053a7000`–`0xffffff80053dafff`). The kext's own counters
were clean (`vmInvalidExits = 0`, `assertFailures = 0`). Keep this as evidence, not as a diagnosis:
reproduce with the kext unloaded before blaming SVM. The TSC-write hazard (item 5) is closed now and
is not a demonstrated cause of either panic.

---

## Sources

- Apple, *Hypervisor.framework*: <https://developer.apple.com/documentation/hypervisor>
- Apple, *Virtualization*: <https://developer.apple.com/documentation/virtualization>
- xnu public kext interface: `osfmk/kern/hv_support_kext.h`
- AMD, *AMD64 Architecture Programmer's Manual, Volume 2: System Programming* (33047),
  Chapter 15 (SVM) and Appendix B (VMCB layout):
  <https://www.0x04.net/doc/amd/33047.pdf>
- applevisor (Rust bindings that document the same API surface):
  <https://docs.rs/applevisor/latest/applevisor/>
- This repository: `docs/CAPABILITIES.md` (API matrix), `docs/CLIENTS.md` (per-client guides),
  `kext/Source/VMCB.hpp`, `kext/Source/Npt.hpp`, `kext/Source/VmcsState.hpp`,
  `kext/Source/VmcsCaps.hpp`, `kext/Source/Frontend.cpp`.
