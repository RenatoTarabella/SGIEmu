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
- `src/indy` the machine: `indy.c` (assembly, physical map, run loop in
  slices of 2000 instructions), `mc.c` (MC: registers, RPSS, RAM banks from
  MEMCFG, VDMA fill), `hpc3.c` (HPC3 + IOC + INT3 routing), and one file per
  chip: `pit8254`, `z85c30`, `ds1386`, `eeprom93`, `kbc8042`, `wd33c93`,
  and `scsi_dev` (disk / CD-ROM on an image file, 512-byte blocks).
  Each chip is standalone (no `Indy` dependency) and tested in
  `tests/test_devices.c`. Coming: Newport/REX3, Seeq, HAL2, `src/frontend` Win32.
- `tools/mipsdis.c` disassembles a binary (PROM): `mipsdis FILE BASE FROM COUNT`.
- `ref/` (git-ignored) holds the Linux IP22 headers and drivers downloaded as
  documentation; read them, never copy code.
- `roms/` (PROM) and `cdrom/` (IRIX images) exist only locally and are
  **git-ignored: never commit or redistribute them**.
- Local material (from the user's MAME setups in `D:\SGI_MAME` and
  `D:\SGI_mame0256s`, MAME 0.287 and 0.256):
  - `roms/ip24prom.bin` = `ip24prom.070-9101-011.bin` from MAME's
    `indy_4610.zip` (512 KB); `roms/indy_4610/` also has the keyboard
    `natural.bin` and a PAL dump;
  - `roms/nvram/eeprom` (256 B, 93CS56 with the PROM environment) and
    `roms/nvram/rtc` (8 KB DS1386) as saved by MAME;
  - `disks/irix53.img`: raw 7.5 GB SCSI disk (SGI volume header with sash,
    EFS root) with IRIX 5.3 installed, from `irix53.chd`;
  - `cdrom/irix53.iso`: IRIX 5.3 install CD, converted from the MODE1_RAW
    `sgi_irix_5.3.bin`.
  Not copied: the freeware CD collection (100 GB CHD). MAME itself
  (`mame.exe indy_4610 -gio64_gfx xl24 ...`, `-debug`) can be used as a
  reference to watch the real PROM/IRIX behaviour; `chdman` extracts CHDs.
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
build\Release\test_devices.exe            # chips one by one
build\Release\indy_boot.exe --prom roms\ip24prom.bin [--disk IMG] [--cdrom ISO] [--ms 5000]
                             [--keys "..."] [--trace N] [--trace-at ADDR] [--io N] [--exc N]
                             [--mc] [--scsi] [--nvlog] [--dis ADDR] [--ram MB] [--nvram FILE]
```

`indy_boot` prints the serial console on stdout. `--keys` types on the
serial port after 1 s emulated (`{CR}` Enter, `{WAIT}` half a second); the
menu appears after about 6 s emulated, so start with a dozen `{WAIT}`:

```
indy_boot --prom roms\ip24prom.bin --ms 12000 --io 0 --exc 0 --keys "{WAIT}x12 5{CR}{WAIT}{WAIT}hinv{CR}"
```
(write the twelve `{WAIT}` out). `{T=190}` waits until second 190 emulated.
`--io` prints accesses to hardware that doesn't exist yet, `--mc` every MC
register write, `--exc` exceptions (with the physical address of bus
errors), `--scsi` every SCSI command and DMA, `--nvlog` NVRAM reads,
`--dis ADDR` disassembles 48 instructions of the final memory.

Boot IRIX 5.3 from the installed disk to a root shell (about 2 minutes real):

```
indy_boot --prom roms\ip24prom.bin --disk disks\irix53.img --ms 235000 --io 0 --exc 0
          --keys "{T=7}1{CR}{T=200}root{CR}{T=203}{CR}{T=206}uname -a{CR}{T=208}hinv{CR}"
```

`disks\irix53.img` is modified by IRIX (logs, autoconfig rebuilt `/unix`);
the pristine copy is `D:\SGI_mame0256s\irix53.chd` (`chdman extracthd`).
Other disks: `D:\Mame\SGI\ULTRA64_2GIG_SCSI_IRIX53_confirmed2.img` (raw,
IRIX 5.3 with the N64 kit) and `D:\Mame\SGI\irix65.chd` (IRIX 6.5, XFS).

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

## Indy hardware facts (checked against the real PROM)

- Addresses where the Linux structs are wrong: MC RPSS counter is at
  `0x1FA01004`, the MC DMA (VDMA) registers at `0x1FA02000` (64-bit regs,
  value at +4: MEMADR 2004, SIZE 2014, STRIDE 201C, GIO_ADR 2024, GIO_ADRS
  202C = set+start, MODE 2034, RUN 204C); the WD33C93 of SCSI 0 is at
  `0x1FBC0000` (SASR +3, SCMD +7), not `0x1FBC4000`.
- MC is revision 3 (sysid): bank base `(cfg & 0xFF) << 22`, size
  `((cfg & 0x1F00) + 0x100) << 14`. The PROM sizes a bank by configuring it
  at 128 MB at `0x20000000` and writing at +4/+16/+64 MB: memory beyond the
  SIMMs must wrap. With SBANKS (0x4000) the upper half is the second SIMM
  side: absent here (single-sided), it must *not* wrap.
- The PROM clears RAM with VDMA fill (mode 0x5A, fill value = GIO_ADR) and
  wants DMA_RUN bit 0x40 set right after the start, then clear.
- DS1386: year register = year - 1940 (IRIX `YRREF`); the PROM clears TE
  before reading the clock, so the registers must be snapshotted then.
  The Indy environment lives in the DS1386 NVRAM, not in the 93CS56 EEPROM.
- IOC byte registers are the low byte of 32-bit words (offset +3). Serial:
  channel B at IOC+0x30, A at +0x38. Panel idle value 0xF3 (buttons active
  low); writing 0 in POWERON turns the machine off.
- Interrupts: IP2 = local0 (INT3 istat0 & imask0), IP3 = local1, IP4/IP5 =
  8254 counters 0/1 (cleared by writing TCLEAR), IP6 bus error, IP7 Count.
  Mapped sources (serial 0x20, keyboard 0x10...) reach local0 bit 7 /
  local1 bit 3 through cmeimask0/1. The 8254 counter 2 runs at 1 MHz and
  clocks counters 0 and 1.
- No graphics board: the PROM says "Cannot open video() for output" and uses
  the serial console; GIO reads must give a bus error (the probe expects it).
- HPC3 SCSI DMA channel 0: NDPTR at HPC3+0x10004, CTRL at +0x11004 (ACTIVE
  0x10, DIR 0x04 = memory to device, the opposite of the Linux header
  comment; CRESET 0x40). Descriptors {pbuf, cntinfo (count 0x3FFF, EOX
  bit 31), pnext}. The PROM and IRIX use select-and-transfer (0x08): the
  target status goes in the Target LUN register, WD status 0x16.
- IRIX probes a second WD33C93 at `0x1FBC8000` and then uses it without a
  protected access: it must exist (with no devices), not bus-error.
- The Ethernet address is in the DS1386 NVRAM at 0x13A (base 0x40 +
  NVOFF_ENET 250). If it is zero the PROM creates no `eaddr` and the IRIX
  kernel dies at start (NULL in the MAC parser); indy_boot writes
  08:00:69:0E:AF:65 when missing.

## Lessons learned

- **Backslashes in shell commands on this machine**: the Bash tool turns
  `\\n` (and `\t`, `\\`) into real control characters even inside quoted
  heredocs, which breaks C string literals and Windows paths written from
  inline Python or sed. Edit such text with the Edit tool, or write the
  Python to a `.py` file with the Write tool first.
- MSVC: with `#pragma fenv_access(on)`, `isnan(NAN)` on a value built from
  the `NAN` macro was false: test NaNs on the bit patterns, never via `NAN`.
- `r4k_stop` from an exception hook inside `service()` must be honoured
  before the inner loop starts again.
- Unicorn caches translated blocks: every case goes at a fresh address and
  `ctl_remove_cache` is called; the code area must stay inside mapped RAM.

## Stages

1. **CPU R4400** — done: interpreter, CP0, TLB, exceptions, FPU, verified.
2. PROM prompt — done: power-on diagnostics, System Maintenance Menu and
   Command Monitor on the serial console (`hinv`, `printenv`, `date`).
   Open: hinv reports a 128 KB secondary cache (should be 1 MB); CPU speed
   is shown as 100 MHz (ns_per_insn = 10).
3. SCSI and IRIX on the serial console — done: the PROM boots `sash` and
   `/unix` from `disks/irix53.img`, IRIX 5.3 reaches multi-user and a root
   shell (hinv, df, ps work). About 200 MIPS, i.e. 2x a 100 MHz Indy.
4. Newport: the graphical PROM, then the X server and the desktop (4Dwm).
5. CD-ROM install from `cdrom/irix53.iso`, IRIX 6.5, ethernet (Seeq).
6. Speed: pre-decoded interpreter per physical page, then JIT with lockstep.
7. Later: Indigo2, ethernet, audio (HAL2), maybe IMPACT.
