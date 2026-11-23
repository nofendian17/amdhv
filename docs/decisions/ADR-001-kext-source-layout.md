# ADR-001: Group kext sources by responsibility, split Vm.cpp, keep Frontend.cpp whole

## Status
Accepted

## Date
2026-11-23

## Context
`kext/Source/` was flat: 37 files and ~8,700 lines, with two files dominating:
`Vm.cpp` (1,746 lines, two OSObject classes, a ~530-line `run()`) and
`Frontend.cpp` (1,582 lines). Finding the code for a single concern meant
scanning unrelated functions, and the vCPU run loop shared a translation unit
with page-table and VM-registry code.

Constraints that shaped the change:

- The kext builds with `-mkernel -fapple-kext -fno-exceptions -fno-rtti -nostdinc`.
  IOKit classes use manual `retain()`/`release()`; no STL is available.
- `include/AMDHVShared.h` is the kext-tools ABI. Its structs, the VMCB
  `static_assert` offsets, trap indices and `Info.plist` must not move.
- The kext cannot be exercised in a normal test process, so compile-time and
  symbol checks (`check-kext-symbols.sh`) are the only local proof of safety.

## Decision
Move sources into seven groups — `core/`, `svm/`, `memory/`, `vmcs/`, `arch/`,
`policy/`, `diag/` — and add every group to the kext Makefile's include path so
existing short `#include "X.hpp"` names keep working.

Split `Vm.cpp` along its real dependency boundaries:

- `Vm.cpp` (688 lines): vCPU lifecycle, registers, MSR/FPU/VMCS access.
- `VmRun.cpp` (582 lines): the VMRUN loop and the exit-code classifier it uses.
- `VmShared.cpp` (589 lines): nested page tables, address spaces, VM registry.

Keep `Frontend.cpp` (1,881 lines) as one translation unit, with section banners
for navigation. The counts above include the Doxygen blocks added later; the
split itself removed 1,083 lines from the original `Vm.cpp`.

## Alternatives Considered

### Leave the flat layout
- Pros: zero churn, no path changes in tests or docs.
- Cons: the two largest files keep hiding unrelated concerns; every new symbol
  lands in the same directory. Rejected: the readability cost recurs on every
  change.

### Also split Frontend.cpp
- Pros: no file over ~600 lines.
- Cons: its entire body is one anonymous namespace with shared state
  (`gRegistered`, `gLiveVms`, `gSvc`, trap tables, callbacks). Splitting it means
  exporting ~15 internal symbols through a new header and converting
  file-local linkage to external linkage. Rejected: higher risk than the
  benefit without runtime verification on AMD hardware. Revisit if the front
  end is genuinely extended again.

### Group directories without splitting Vm.cpp
- Pros: smaller diff.
- Cons: a 1,746-line file stays; directory grouping alone does not make the
  run loop easier to find. Rejected.

## Consequences
- New source files must land in the matching group; the kext Makefile now uses
  `find Source -name '*.cpp'`, so no Makefile edit is needed for a new file.
- Host tests and docs had their include paths updated; the codegraph index was
  re-synced after the moves.
- No behaviour change: a line-by-line comparison of the original `Vm.cpp`
  against the three new files showed 0 original lines lost, the imported-symbol
  count stayed at 393, and all 18 host tests still pass.
- `Frontend.cpp` remains above the usual ~1,000-line inspection threshold. The
  section banners and this ADR are the accepted mitigation.
