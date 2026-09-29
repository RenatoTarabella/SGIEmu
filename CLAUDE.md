# CLAUDE.md — working notes for the SGI emulator

Context for anyone (human or AI) working on this repository. `ANALISI.md`
(Italian) holds the preliminary analysis: why the Indy, where the
documentation comes from, why MAME is slow, how the CPU and JIT should be
built, and the stages. `README.md` is the user-facing overview. This file
holds conventions, the test workflow, design invariants and lessons learned.

Sister project: `..\ARM` (ArchieEmu, ARMv2/Archimedes) — same style and
toolchain; its `CLAUDE.md` has the general lessons.

## Goal

SGI Indy (IP24) with an R4400, IRIX 5.3/6.5 usable and **fast**. Not
cycle-exact: timing only needs to be believable (Count/Compare, 8254,
interrupts). Then Indigo2 (IP22), which shares the ASICs.

## Layout

- `src/cpu` R4400: `r4k.c` run loop and integer instructions, `r4k_mmu.c`
  address translation / TLB lookup / fast MMU / slow memory paths,
  `r4k_cp0.c` CP0 registers, TLB instructions, exceptions, interrupts,
  timer, CACHE; `r4k_fpu.c` CP1; `r4k_disasm.c`. `r4k_internal.h` has the
  inline load/store fast paths shared by these files (not API).
- `src/mem/physmem.c` physical address space: 4 KB pages, RAM/ROM as direct
  host pointers, I/O through device callbacks.
- `tests/test_r4k.c` unit tests; `tests/r4k_oracle.c` + `tests/diff_unicorn.py`
  differential test against Unicorn.
- `tools/indy_boot.c` headless boot of the PROM (like `archie_boot`).
- Coming: `src/indy/` one module per ASIC (MC, HPC3, IOC2/INT2 + 8254,
  Newport/REX3, WD33C93, Seeq 8003, Z85C30, DS1286), `src/frontend` Win32.
- `roms/` (PROM) and `cdrom/` (IRIX images) exist only locally and are
  **git-ignored: never commit or redistribute them**.
- Source comments and commit messages are in Italian (ASCII: `e'`, `perche'`);
  README and this file are in English.

## Build and test

Windows only, no gcc: MSVC 2022 through CMake.

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64    (once)
cmake --build build --config Release
build\Release\test_r4k.exe                  # must print "0 falliti"
.venv\Scripts\python tests\diff_unicorn.py [cases] [seed] [category]
build\Release\test_r4k.exe --bench          # MIPS of the interpreter
build\Release\indy_boot.exe --prom roms\<file> [--trace N] [--trace-at ADDR] [--io N] [--exc N] [--n N]
```

Categories of the diff: `alu muldiv trap mem branch fpu misc`. Run 100k+
cases with a couple of seeds after touching the CPU (100k takes ~25 s).

**Test from the command line before handing anything over**; don't make the
user type long test lines.

`.venv` has unicorn 2.1.4, keystone and capstone. Unicorn is used with
`UC_CPU_MIPS64_R4000`, big-endian, and the CPU runs with Status.ERL=1 on both
sides so kuseg is unmapped and addresses are physical.

## Design invariants (don't break them)

- **Memory is stored big-endian** in host memory; loads bswap. DMA and
  LWL/LWR work on bytes in natural order.
- **Fast MMU**: `c->fast[mode][(va>>12)&1023]`, tag = virtual page; the fast
  test is `tag == va & ~(0xFFF & ~(size-1))`, which checks page *and*
  alignment. Only RAM/ROM pages are ever entered; I/O always takes the slow
  path. `tag_w` is set only for writable RAM without `PM_CODE`.
  It must be flushed when the *mapping* changes: TLBWI/TLBWR (old and new
  entry's pages), EntryHi ASID change (MTC0 or TLBR), Status ERL/KX/SX/UX
  change, ERET from ERL, and whenever the machine changes the physical map
  (`r4k_fast_flush`). KSU/EXL changes only switch the table, no flush.
- **Physical indexing for translated code**: pages holding translated code
  get `PM_CODE`; writes to them go through `pm_write` which calls
  `pm->code_write`. After setting `PM_CODE` call `r4k_fast_flush` so no
  stale `tag_w` survives. Block caches must be keyed by physical address.
- **Exceptions**: `r4k_exception*` sets pc, EXL, EPC/BD (only if EXL was 0)
  and `c->pc_set`; the instruction must return without writing anything.
  Every load/store helper returns 0 when it raised. `c->pc` is the address
  of the current instruction throughout `exec`.
- **Run loop**: the inner loop only compares `icount < limit`. Anything that
  may need attention (interrupt line change, Status/Cause write, ERET,
  Compare/Count write, `r4k_stop`) calls `r4k_kick` (limit = 0) so the outer
  loop re-checks timer and interrupts.
- Count advances by 1 every `2^count_shift` instructions (default 2), computed
  lazily; `timer_event` is the icount at which Count == Compare.
- 32-bit ALU ops use the low 32 bits of the operands and sign-extend the
  result (unpredictable in the architecture for non-sign-extended inputs).

## Known, deliberate differences from Unicorn/QEMU

`diff_unicorn.py` avoids or tolerates these (`qemu_quirk`); we follow the
R4400:

- SPECIAL2 (opcode 0x1C, e.g. `mul`) and SPECIAL function 0x05: QEMU executes
  them even in the R4000 model; the R4400 raises RI.
- FPU L format (CVT.L, ROUND.L..., CVT.S.L, CVT.D.L): QEMU raises RI unless
  MIPS IV; they are MIPS III, implemented here, checked in `test_r4k.c`.
- BLEZ/BGTZ with rt != 0: QEMU raises RI (R6 rule); here rt is ignored.
- C.cond with nonzero bits 10:6: QEMU reads them as MIPS IV cc / MIPS-3D abs.
- FS=1 flushing a denormal result: here Underflow+Inexact are signalled
  (R4000 manual), QEMU signals nothing.
- CVT.S.D of a qNaN whose payload vanishes: QEMU returns infinity, here the
  default NaN.
- Division by zero / INT_MIN÷-1: result copied from QEMU (LO = dividend,
  HI = 0); the real chip leaves unpredictable values.
- Denormals and NaN operands are computed directly; the real R4400 traps
  with Unimplemented Operation and IRIX emulates. Out-of-range float→int
  gives Invalid and 0x7FFFFFFF(FFFFFFFF), as QEMU.

## Lessons learned

- MSVC: with `#pragma fenv_access(on)`, `isnan(NAN)` on a value built from
  the `NAN` macro was false: test NaNs on the bit patterns, never via `NAN`.
- `r4k_stop` from an exception hook inside `service()` must be honoured
  before the inner loop starts again.
- Unicorn caches translated blocks: every case goes at a fresh address and
  `ctl_remove_cache` is called; the code area must stay inside mapped RAM.

## Stages

1. **CPU R4400** — done: interpreter, CP0, TLB, exceptions, FPU, verified.
2. PROM prompt: CPU + MC + HPC3 + serial (Z85C30) + IOC2/INT2/8254 + DS1286.
   The PROM talks on the serial port before graphics.
3. Newport minimal: the graphical PROM and its menu.
4. SCSI (WD33C93) and disk: sash / fx, install IRIX from CD image.
5. IRIX multi-user, then the desktop.
6. Speed: pre-decoded interpreter per physical page, then JIT with lockstep.
7. Later: Indigo2, ethernet, audio (HAL2), maybe IMPACT.
