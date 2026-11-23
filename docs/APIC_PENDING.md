# APIC work: consolidated status (no further reboot requested yet)

This file collects everything learned so far so the remaining work can land in **one** kext build
and be checked in **one** reboot. The current tree already contains all fixes except the final
argument decoder, which is present but unverified.

## Verified on live boots

| Item | Result |
| --- | --- |
| `hv_vm_create(HV_VM_ACCEL_APIC)` with `amdhv_apic=1` | `ok` |
| Task trap 10 op codes | `0x6a`–`0x7a` mapped; AT-PIC/IOAPIC/LAPIC state, IRQ, MSI, bus frequency |
| Task trap 11 | `hv_vm_lapic_msi` = `(data << 32) | addr` |
| `hv_ioapic_state_ext_t` layout | `version(4) + pad(4) + state(272)` → state at `+8`, **not** `+4` |
| `hv_apic_state_ext_t` layout | state at `+8`; `hv_atpic_state_ext_t` state at `+4` |
| Guest interrupt delivery | `--apic-run`: IRQ0 → vector `0x30`, guest exits at handler RIP `0x1200` |
| SMAP panic | caused by `memcpy` to a user pointer in get_state; fixed with `copyout`/`copyin` via a kernel buffer |
| Failures after last reboot | `vmInvalidExits`, `assertFailures`, `enableFailures`, `vmFpuMismatches` = 0 |
| Full probe | `79 of 79`, `hvf-smoke` `PASS`, `make test` 18/18 |

## Decoded thread-13 wire format

Pointer words are shifted left 8; low byte is a tag:

| Tag | Call | Notes |
| --- | --- | --- |
| `0` | `hv_vcpu_inject_extint` | no vector supplied |
| `0x8001` | `hv_vcpu_apic_read` | `p1` = result, `p2`/`p3` = argument chain |
| `0x8002` | `hv_vcpu_apic_write` | `p1` = bool, `p2` = offset chain, `p3` = value chain |
| `0xe003` | `hv_vcpu_apic_put_state` | pointer = `w0 >> 8`; ext state at `+8` |
| `0xe004` | `hv_vcpu_apic_get_state` | same |
| `0x0006` | `hv_vcpu_apic_ctrl` | controls behind `p1` |
| `0x0107` | `hv_vcpu_apic_trigger_lvt` | no vector supplied |
| `0x0005` | LSC enter / invalidate | no-op |

Observed pointer chain for `apic_write` (run at uptime 60s):

```
p2 = 0x7ff7bacc5bc0 -> inner 0x7ff7bacc5d80
p3 = 0x7ff7bacc5ae0 -> inner 0x7ff7bacc5b50
single deref: off = 0xbacc5d80, val = 0xbacc5b80   (pointer low halves)
double deref: off = 0, val = 0xbacc5b80            (still a pointer low half)
```

Conclusion: the offset/value are not present as plain words in client memory, so further decoding
would be guesswork with a reboot per attempt. `hv_vcpu_apic_read`/`write` are therefore accepted as
**compatibility no-ops** (read returns zero) and are not claimed as semantically implemented.
`get_state`/`put_state` use the stable SDK struct layout and are implemented for real.

## Still open

1. **No further reboot is required for correctness.** The final build accepts all 32 APIC calls;
   `apic_read`/`write` are no-ops by design, documented as such. Install it whenever convenient and
   optionally confirm `hvf-probe --apic` shows no `HV_ERROR` besides the five `exit_*` helpers.
2. **Five `hv_vcpu_exit_*` helpers** answer `HV_ERROR` and never reach a trap, even with
   `HV_VM_ACCEL_APIC`. They are a framework-to-client notification mechanism, not a provider call:
   the provider would have to synthesize the matching APIC exit first, and the kernel never exposes
   an API for that. A dyld interpose shim in the client process is the only remaining avenue; that is
   a separate userspace project and is explicitly out of scope here. Do not add kext stubs for them —
   the calls never arrive.
4. **No guest-visible LAPIC MMIO emulation** — APIC register access is only through the API model;
   `kext/Source/arch/ApicMmio.hpp` + `tests/apic_mmio_test.cpp` decode the two absolute MOV forms a guest uses
   for absolute APIC accesses. The NPF path now uses that decoder, but only emulates when the decoded
   address equals the faulting GPA; the instruction is read from guest memory through
   `VmShared::guestBytes()`. Both the LAPIC page (`0xFEE00000`) and the I/O APIC IOREGSEL/IOWIN
   pair (`0xFEC00000`/`+0x10`) are wired to the models. EOI writes to LAPIC `0x0B0` clear the
   highest ISR bit (host-tested), so an injected interrupt can be retired by a guest EOI.
   Live check: with `amdhv_apic=1`, the guest MOV to `0xFEE00080` completes and the following HLT
   exits at `RIP 0x1007`. EAX reads `0x160`, not the modelled TPR `0x55`: Apple's framework maps the
   LAPIC page itself in an APIC-accelerated VM, so the NPF fallback did not fire. Treat the NPF path
   as a safety net, not the primary APIC access route.
5. The APIC work is merged into `main` (PR #6 / `release/v0.3.0`); the live gaps above still mean
    this is not claimed as full compatibility.

## Why no more reboot was requested

The last debug builds each needed a reboot to learn one more pointer level. All that information is
now consolidated here and in the tree, so the next reboot is for one combined verification, not for
another discovery step.

## Final verification (kext `FCF902DB-F14E-3CF5-8C51-AC966D9F7499`)

- UUID of the loaded kext matches the built kext exactly.
- `hvf-probe --apic`: 31 calls answer `ok`; only the five `hv_vcpu_exit_*` helpers answer `HV_ERROR`.
- `hvf-probe --apic-run`: still exits at `RIP 0x1200` (guest interrupt delivered).
- `hvf-probe`: `79 of 79`, `hv_vm_create(HV_VM_ACCEL_APIC)` `ok`, 64/64 vCPU, deadline exit 52.
- `hvf-smoke`: `PASS`; `make test`: 18/18.
- All failure counters (`vmInvalidExits`, `assertFailures`, `enableFailures`, `vmFpuMismatches`) are 0.
- EOI and I/O APIC MMIO changes are in this build; the guest APIC pages are mapped by Apple's
  framework, so the NPF fallback is not exercised by the current probes.
