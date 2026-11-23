# ADR-002: Keep host-testable logic header-only and track coverage with gcov

## Status
Accepted

## Date
2026-11-23

## Context
The 18 host tests compile logic directly from `kext/Source/*.hpp` on any OS;
they do not link against the kext. Modules such as `Npt.hpp`, `VmcsState.hpp`,
`Asid.hpp` and the policy headers are therefore header-only by design, not by
accident. Their inline functions are the unit under test.

Coverage was previously unmeasured, so it was impossible to tell whether a new
branch in those headers had a test. The project also cannot run the kernel code
in a host process, so a coverage number can only ever describe the host-testable
subset.

## Decision
Keep the host-testable policy and data-structure headers header-only. Add a
`make coverage` target that builds the 18 test binaries with `--coverage`, runs
them, and invokes `gcov -p -b` once per binary into its own directory (shared
headers would otherwise overwrite each other's `.gcov` files).

## Alternatives Considered

### Move implementations into .cpp and link a shared library
- Pros: faster incremental compiles, real translation-unit boundaries.
- Cons: the kext and the host tests would need to link the same new library
  with different toolchains (`-mkernel` vs. host C++), and the ABI-visible
  headers would have to stay in sync with it. Rejected: build complexity and
  ABI risk for code that already compiles and tests reliably.

### Adopt Google Test / Catch2
- Pros: richer assertions and test discovery.
- Cons: a new dependency that must also work on the macOS host build and would
  replace the existing dependency-free `CHECK` convention in 18 files.
  Rejected: no behavioural gap that the current harness cannot express.

### Chase 100% line coverage
- Pros: a clean-looking number.
- Cons: the remaining uncovered lines are defensive guards that the public API
  cannot reach (full-pool and 32-bit wraparound paths in `SpacePool.hpp`, the
  post-guard `n < i + 6` check in `ApicMmio.hpp`, an inconsistent-state fallback
  in `Asid.hpp`, and a redundant bounds check in `MsrMap.hpp`). Forcing them
  would require exposing internals or writing tests that prove nothing.
  Rejected.

## Consequences
- Baseline line coverage of `kext/Source` was 97.15% (1022/1052). Added tests
  for exit-code mapping, I/O APIC registers, truncated MMIO decoding, NPT init
  and segment errors, and VMCS control defaults raised it to 99.24%
  (1045/1053).
- The number covers host-testable headers only. `VmRun.cpp`, `Frontend.cpp`
  and the rest of the kernel side are out of scope by construction; do not
  quote 99.24% as kext-wide coverage.
- `make coverage` writes instrumented binaries and gcov data under `build/cov/`,
  which is gitignored.
