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
  timer, CACHE; `r4k_fpu.c` CP1; `r4k_disasm.c`; `r4k_jit.c` the JIT to
  x86-64 (emitter, block compiler, block cache, chaining, jump cache).
  `r4k_internal.h` has the inline load/store fast paths shared by these
  files (not API).
- `src/mem/physmem.c` physical address space: 4 KB pages, RAM/ROM as direct
  host pointers, I/O through device callbacks.
- `tests/test_r4k.c` unit tests; `tests/r4k_oracle.c` + `tests/diff_unicorn.py`
  differential test against Unicorn.
- `tools/indy_boot.c` headless boot of the PROM (like `archie_boot`);
  `tools/prof.c` its sampling profiler (`--prof`).
- `src/indy` the machine: `indy.c` (assembly, physical map, run loop in
  slices of 2000 instructions), `mc.c` (MC: registers, RPSS, RAM banks from
  MEMCFG, VDMA fill), `hpc3.c` (HPC3 + IOC + INT3 routing), and one file per
  chip: `pit8254`, `z85c30`, `ds1386`, `eeprom93`, `kbc8042`, `wd33c93`,
  `scsi_dev` (disk / CD-ROM on an image file, 512-byte blocks) and `newport`
  (graphics). Each chip is standalone (no `Indy` dependency) and tested in
  `tests/test_devices.c`. Coming: Seeq, HAL2.
- `res/`: the icon (`indy.png` from the user's 3D SGI logo, `indy.ico`
  made from it with Pillow, 16-256 px) and `indy.rc` (icon + start dialog,
  English texts, UTF-8). `indy.exe` without arguments shows the dialog and
  keeps the last settings in `indy.ini` next to the exe.
- `src/frontend/indy_win32.c` the window (`indy.exe`): one loop that runs a
  frame of emulated time, redraws only when the Newport changed (`gen`),
  sleeps to stay at real time. `png.c` comes from ArchieEmu.
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
                             [--jit] [--jit-block N] [--snapshot] [--rtc T] [--hash MS] [--prof]
R4K_JIT=N build\Release\test_r4k.exe          # unit tests / --bench with the JIT (blocks <= N)
R4K_JIT=N .venv\Scripts\python tests\diff_unicorn.py ...   # r4k_oracle with the JIT
```

JIT workflow. `--jit` turns it on in indy_boot (the window has it on by
default, `--nojit` turns it off). `--snapshot` keeps disk writes in memory
(the image is not touched, runs are repeatable), `--rtc T` fixes the clock
(Unix time; otherwise the host time makes every run different), `--hash MS`
prints `H icount pc hash` of the CPU state every MS emulated ms. The JIT must
give the *same* hashes as the interpreter: run the same command with and
without `--jit` and diff the `H` lines (the first differing line brackets
the bug; then `--trace`). Reference runs, both identical with the JIT:

```
indy_boot --prom roms\ip24prom.bin --disk disks\irix53.img --snapshot --rtc 1790000000 --hash 1000
          --ms 215000 --io 0 --exc 0 --keys "{T=7}1{CR}{T=200}root{CR}{T=203}{CR}{T=206}uname -a{CR}{T=208}hinv{CR}"
indy_boot --prom roms\ip24prom.bin --nvram roms\nvram\indy-us.nvram --disk disks\irix65.img --snapshot
          --rtc 1790000000 --hash 2000 --ram 128 --ms 210000 --io 0 --exc 0 --kbd --keys "{T=205}{PNG}" --png x
```
(5.3: ~7 s with the JIT, ~100 s without; 6.5 to the graphical login: ~18 s
and ~100 s.) **The reference hashes depend on the disk contents**: `--snapshot`
protects the image from indy_boot, not from the user's `indy.exe` sessions,
so after the user has used a disk in the window, regenerate the interpreter
reference before comparing (a whole-run divergence at the same point for
interpreter and JIT means a stale reference, not a JIT bug). A GL/FPU check:
the desktop with `bin/atlantis &` in /usr/demos (same keys as the buttonfly
run, `--ms 250000`). Debug switches: `R4K_JIT_NOREGS=1` (no host register
caching), `R4K_JIT_NOFPU=1` (COP1 through the interpreter) to bisect a
divergence. `--prof` samples the main thread at 1 kHz and prints host
functions (PDB symbols, Release builds have them) and the hottest JIT blocks
by MIPS address; the JIT statistics at the end list blocks, invalidations,
flushes, why the dispatcher was entered, and the instructions and pcs that
still go through the interpreter.

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
`--dis ADDR[,N]` disassembles N (48) instructions of the final memory
(kernel modules at `0xC0...` too), `--kbdlog` every 8042 data access.

More `--keys` commands for driving the desktop headless: `{M=dx,dy,b}` moves
the PS/2 mouse in steps of 4 (no X acceleration; b = buttons, 1 = left),
`{K=1E}` / `{K=E048}` presses and releases a set-1 scan code (E0 prefix as
0xE0xx), `{D=..}` / `{U=..}` only press / release (modifiers), `{PNG}` saves
`<--png>.N.png` and prints the Newport command count (use it with
`--nptrace-after` / `--nplog`, which then start from that command), `{REG}`
prints pc/ra/a0-a2/Status (sample a hang). The pointer starts at the screen
centre (640,512).

Graphical boot to the desktop and a program in the root Console (~2 min real):

```
indy_boot --prom roms\ip24prom.bin --nvram roms\nvram\indy-us.nvram --disk disks\irix53.img
          --ms 230000 --io 0 --exc 0 --kbd --png shot
          --keys "{T=160}root{CR}{T=205}{M=-400,280,0}{M=0,0,1}{M=0,0,0}{WAIT}
                  cd /usr/demos{CR}bin/buttonfly &{CR}{T=225}{PNG}"
```
(one line; `roms\nvram\indy.nvram` is the window's NVRAM with console=g and
autoboot, the login panel appears at ~150 s emulated). Never run indy_boot
on `disks\irix53.img` while `indy.exe` is using it.

Boot IRIX 5.3 from the installed disk to a root shell (about 2 minutes real):

```
indy_boot --prom roms\ip24prom.bin --disk disks\irix53.img --ms 235000 --io 0 --exc 0
          --keys "{T=7}1{CR}{T=200}root{CR}{T=203}{CR}{T=206}uname -a{CR}{T=208}hinv{CR}"
```

`disks\irix53.img` is modified by IRIX (logs, autoconfig rebuilt `/unix`);
the pristine copy is `D:\SGI_mame0256s\irix53.chd` (`chdman extracthd`).
The MAME install had only the buttonfly menus (`demos.sw.demos`); on
2026-09-30 the full demos (games, tools, visualization: flight, atlantis,
ideas, bz, ...) were installed from `cdrom\irix53.iso` with
`inst -f /CDROM/dist/demos`, `install *`, `go` (the CD automounts on
`/CDROM`). Copy from just before: `disks\irix53-backup.img`. End headless
sessions that write to the disk with `shutdown -y -g0` (halts at the PROM
"Okay to power off" panel).
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

## JIT invariants (r4k_jit.c)

- **Exactness**: the JIT executes exactly the instructions the interpreter
  would. A block runs only if `ninsn <= limit - icount` (else the dispatcher
  interprets up to the limit, `R4K_JIT_NOFIT`); every chained jump and jump
  cache hit checks the same budget. A helper (the interpreter for one
  instruction) returns "exit" on exception/ERET (`pc_set`), kick
  (`limit == 0`), `jit_exit` (code rewritten, flush, physical map changed).
  Helpers get pc, delay slot flag and the count of block instructions up to
  and including theirs (kd), so EPC/BD/icount are exact. Keep it that way:
  the equivalence of whole IRIX boots is the main test.
- Blocks are keyed by (host pointer, va, mode | allow64 << 2); they never
  cross a 4 KB page; a branch whose delay slot is outside the block, or a
  branch in a delay slot, is left to the interpreter.
- MIPS registers live in `R4k` (rbx = R4k*); rax/rcx/rdx/r8-r10 are
  scratch. Each block is compiled twice: the first pass counts register
  uses, the second keeps the most used MIPS registers in the callee-saved
  host registers (rbp rsi rdi r12-r15; only rbp r12-r15 on System V),
  loaded at block entry. Every GPR access goes through `gpr_ld/gpr_st/...`;
  dirty ones are written back before every helper call and exit (the stub
  records the dirty mask of its origin); after a helper, rt and rd are
  reloaded (the only GPRs an interpreted instruction can write).
- Conditional branches store the resolved next pc in `branch_target` before
  the delay slot (a helper exiting in the slot uses it).
- The block key also holds Status.FR, Status.CU1 and FCSR.RM (CTC1 ends the
  block). FPU operations run on SSE with a clean MXCSR (rounding from RM)
  and read the flags back: anything but inexact (invalid, overflow,
  underflow, divide by zero), a NaN/infinite/denormal result or an enabled
  inexact trap re-executes the instruction in the interpreter, which has
  all the MIPS NaN/FS rules. MXCSR goes back to the C default after RM != 0.
- `jr`/`jalr` in kernel kseg0/kseg1 blocks first check a per-site JitRet
  (last kseg0/1 target, filled by `dyn_ret`), valid while its `gen` equals
  `c->jit_gen` (bumped by every invalidation and flush).
- Code pages: compiling sets PM_CODE on the physical page (and its mirrors,
  `pm_add_mirror`: the MC maps the first 512 KB at 0x08000000 too) and
  removes fast write tags to that host page (`r4k_fast_protect`).
  `code_write` drops the page's blocks, their chain links and jump cache
  entries, clears PM_CODE, sets `jit_exit`. Any change of the physical map
  (`pm->map_gen`) flushes everything.
- Chaining: only constant targets in the same virtual page as the block, or
  kseg0/kseg1 to kseg0/kseg1 in kernel mode (translation cannot differ).
  Exits after COP0 writes in mapped code are never chained. Register jumps,
  other exits, exceptions and ERET go through `dyn` (shared stub: fast MMU
  of the *current* mode + jump cache by host pointer); misses go to C.
- In kseg0/kseg1 blocks COP0 instructions do not end the block (the TLB
  cannot change that code; Status/Cause/Compare writes kick, ERET sets pc).

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
- VDMA (MC virtual DMA), from the IRIX ng1 driver: MODE 0x02 = GIO to
  memory, 0x08 = fill (value = GIO_ADR), 0x10 = increment, 0x40 long burst;
  SIZE = lines << 16 | bytes per line, STRIDE low 16 bits signed bytes added
  after each line (bit 16 = zoom 1). The GIO address is fixed (Newport
  hostrw0 go, `0x1F0F0A30`, 64-bit words). The PROM clears RAM with mode
  0x5A and wants DMA_RUN 0x40 right after the start, then clear. DMA_RUN
  after the end: 0x08 = done; 0 without 0x08 = stopped on an invalid page
  (MEMADR = that address), and the kernel sets the PTE valid bit and
  resumes with STDMA (0x2044) = 1; other bits are errors. DMA_CTRL (0x16C)
  bit 0x10 = MEMADR is virtual: DMA TLB at 0x184 + 16k (HI = 4 MB segment)
  / 0x18C + 16k (LO = EntryLo of the page-table page, bit 1 valid), PTE
  4 bytes per page, phys = (pte & 0x03FFFFC0) << 6. The async path waits for
  the end interrupt on local0 0x10 and clears DMA_INTR_CAUSE (0x164) with 0.
  Without these, "ng1: pixel dma timeout".
- VDMA alignment: the memory side is a byte stream (X repaints the desktop
  background with lines of 126/130 bytes from odd addresses, STRIDE =
  256 - width); GIO_ADR carries the same misalignment (`hostrw0 + 6`), so
  each line starts at byte lane GIO_ADR & 7 of a 64-bit word and the
  first/last words are partial. The Newport draws only the pixels of the
  enabled bytes (see Newport facts). Word-sized transfers gave the vertical
  stripes and stale window corners after minimizing a window.
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
  clocks counters 0 and 1. The Newport vertical retrace is local1 bit 7
  (IRIX imask1 = 0xA2), not a mapped source: IRIX does GL buffer swaps and
  cursor work in that interrupt and clears it by reading REX3 status.
- HAL2 (audio, PBUS channel 0 at `0x1FBD8000`): revision register (+0x20)
  bit 15 = audio *absent*. Reading 0 there made IRIX use audio; the first
  desktop sound (an error beep, System Shutdown) then hung the kernel at
  splhi refilling the ring buffer forever (keyboard and mouse dead).
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
  08:00:69:0E:AF:65 when missing and then fixes the checksum
  (`rtc_fix_checksum`: 256 bytes from 0x40, skip byte 0, rotate left then
  XOR, start 0x8B — derived from two valid NVRAMs). A wrong checksum makes
  the PROM reinitialise the whole environment.
- WD33C93 chunked transfers: IRIX programs the transfer count for one HPC3
  descriptor chain (~256 KB) even when the command asks for more (fsck reads
  2.2 MB at once). When the count ends in the data phase the chip pauses with
  status 0x49/0x48 (unexpected phase, data in/out) and the command phase
  register must read **0x45**; IRIX then reloads count and DMA and resumes
  Select-and-Transfer. With 0x41 IRIX resets the bus; finishing early makes
  fsck print "CAN NOT BLOCK READ" forever (thousands of lines on the
  graphical textport = what made the graphical boot look slow).

## Newport facts (checked against the PROM and the IRIX X server)

- REX3 at `0x1F0F0000`: "set" registers at +0, "go" at +0x800 (the write also
  starts the command), config registers at +0x1300/+0x1B00, DCB (VC2, CMAP,
  XMAP9, BT445...) through dcbmode 0x238 / dcbdata0 0x240: slave in bits
  10:7, CRS in 6:4, width in 1:0 (0 = 4 bytes); a write moves the top N bytes
  of the access, a read returns them right-justified.
- Register widths come from the PROM's own register test (breakpoint on its
  compare with `--bp BFC08A38` / `BFC08AD4`): coordinates are fixed point in
  bits 26:7 (integer 26:11), `xstarti` & co. clear the fraction; slopes
  (0x210-0x21C) are sign-magnitude; see the masks in `rex_write`.
- Board id: CMAP revision register (CRS 6): bit 7 = 8 planes, 6:4 board rev.
- Framebuffer is 1344 wide; the visible area starts at column 8 (the PROM
  uses xywin 0x1008). Rows wrap at 1024 (the PROM draws row 1024 = row 0).
- XMAP9 mode register: pixel depth in bits 11:10 (0 = 4, 1 = 8, 2 = 12,
  3 = 24 bit), mode in 9:8 (0 = CI, else RGB), colormap bank (msb) in 7:3,
  bit 0 = displayed buffer of a double-buffered window (1 = high nibble /
  half). CI uses `cmap[msb<<8 | pix]`; banks 0x1D-0x1F are gamma ramps;
  cursor colours at `(curs_msb << 5) | v`.
- GL (buttonfly) on the XL8: 4-bit CI double buffered. Drawing with depth 4
  replicates the value in both nibbles and wrmask (0xF0 / 0x0F) picks the
  buffer (same for 8/12 bit on 24 planes); drawmode1 bit 5 (DBLSRC) reads
  the high one. The swap is the kernel flipping XMAP mode bit 0 at retrace.
- GL writes IEEE floats plus 4096 into coordinates and colours: the REX3
  keeps the mantissa. The "F" registers (xstartf 0x138 ... yendf 0x144,
  xendf1 0x14C) have a 12-bit unsigned integer in bits 22:11 (mask
  0x007FFF80), unlike xstart 0x100 (26:11 signed). In CI mode the colour
  index is the integer part of colorred (colori writes it there).
- Gouraud: drawmode0 SHADE (0x40000) makes spans/lines add the slopes
  (0x210 red, 0x218 green, 0x21C blue) to the colour registers at every
  pixel. GL writes the slopes as biased floats, so they are *sign-magnitude
  as written* (bit 31 sign, low bits magnitude; -4096.0 = slope 0), while
  the read-back must follow the two's-complement rule the PROM register
  test checks (both are kept: `slope[]` and `misc[]`). A colour channel is
  bits 19:11 of its register (above are float exponent/bias, not a sign):
  0-255 as is, 256-383 overshoot (255), 384-511 undershoot (0).
- Lines without drawmode0 DOSETUP (0x20) do not recompute the line from
  the end points: GL draws points that way (dm0 0x0A, only xstartf/ystartf
  written with a 64-bit go), one pixel. Drawing to the stale xend/yend gave
  the fans of rays converging on a corner in Meteor Blast.
- XL24 (`--gfx 24`): CMAP board revision must be 2 (bits 6:4); from 3 up
  the PROM reads the monitor elsewhere and picks 1024x768. GL double
  buffered RGB is 12 bit per buffer: buffer 0 in bits 11:0, buffer 1 in
  23:12 (GL wrmask 0xFFF / 0xFFF000), shown as 4:4:4 (XMAP mode size 2,
  RGB). In RGB 4/12-bit fast clear colorvram is 8:8:8 and gets packed
  (buttonfly 0x191919). buttonfly and ideas_ogl look right on XL24.
- Open: the "textured cube" demo (software texturing) shows horizontal
  smears on XL8 and XL24; suspect the unimplemented VDMA zoom (STRIDE
  bits 16+, DMA_COUNT) or pixel zoom used by lrectwrite.
- Pattern commands (go on zpattern): with drawmode0 L32 (0x8000) at most 32
  pixels (X text, `0x9106`); without L32 but with STOPX they run to xend
  (GL `charstr` text, `0x1106`, one glyph row per command). Drawing one
  pixel there made flight's text come out as dashes.
- A 64-bit write to the go area is two registers and one command, started
  after the second (GL writes xstarti+xendf1 at 0x948).
- Host data writes: 64-bit at hostrw0 go = hostrw0 + hostrw1, one command;
  32-bit at hostrw0 go = hostrw0 (+ hostrw1 with RWDBL, X's trick); any
  other size or offset in 0xA30-0xA37 is a partial word from the VDMA and
  draws only the pixels in those bytes (`host_partial`). The MC therefore
  never sends 32 bits at lane 0 for a partial word (two 16-bit writes).
- The "demos" account background is a 256x256 tile dithered between two
  colormap cells (0x31/0x32 of bank 0x12, 1F0051 and 135E5E as written by
  X): the grain is genuine, not an emulation bug (still to compare with
  MAME).
- Popup (planes 5) and CID (6): the 2-bit value is replicated over the byte,
  X draws menus with wrmask 0xCC; the popup shown is bits 3:2 (colour
  `(pup_msb << 5) | v`). Without the replication Toolchest menus and
  dialogs never appeared.
- RGB mode with 32-bit host pixels (HD 3, ABGR, red in the low byte) into
  a smaller depth: the REX3 packs to 3:3:2 (4:4:4, 1:2:1) with a 4x4
  ordered dither when drawmode1 bit 16 is set (ipaste, desktop
  backgrounds); 32-bit reads unpack. Writing the low byte gave noise.
- DID tables: VC2 register 5 points to one pointer per display line; each
  line is a list of `(x << 5) | DID` ending with 0xFFFF, x in framebuffer
  columns. X uses different XMAP modes per window (root CI bank 0x12, the
  login panel 8-bit RGB): without DIDs the X screen is black.
- Host data (CHOST): HD bits 9:8 of drawmode1 and RWPCKD give pixels per
  word, MSB first; RWDBL means every go on hostrw0 draws hostrw0 and then
  hostrw1 (X writes hostrw1 once). At the end of a block row the rest of the
  word is discarded (X starts each row on a new word): continuing into the
  next row smears text and icons.
- Fast clear (drawmode1 FCLR) uses colorvram; X and the PROM use it.
- Profiling helpers: `--nptrace N [--nptrace-after M]` prints commands,
  indy_boot prints commands per (drawmode0, drawmode1) with pixel counts, the
  rdtsc time spent in Newport accesses, fast-MMU flushes.

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
- The Bash heredoc rule above also bites Python one-liners that contain C
  `printf("...\n")`: write edit scripts with the Write tool (scratchpad) and
  run them, or use the Edit tool.
- Host timing is noisy (the same indy_boot run varies 7.5-10.5 s): compare
  speed changes with repeated runs or with `--prof` percentages.
- The PROM's delay loops read the DS1386 tens of millions of times; the
  calendar conversion is cached per hundredth of a second.
- Unicorn caches translated blocks: every case goes at a fresh address and
  `ctl_remove_cache` is called; the code area must stay inside mapped RAM.

## Stages

1. **CPU R4400** — done: interpreter, CP0, TLB, exceptions, FPU, verified.
2. PROM prompt — done: power-on diagnostics, System Maintenance Menu and
   Command Monitor on the serial console (`hinv`, `printenv`, `date`).
   Open: hinv reports a 128 KB secondary cache (should be 1 MB); CPU speed
   is shown as 100 MHz (ps_per_insn = 10000; `--mips N` changes it).
3. SCSI and IRIX on the serial console — done: the PROM boots `sash` and
   `/unix` from `disks/irix53.img`, IRIX 5.3 reaches multi-user and a root
   shell (hinv, df, ps work). About 200 MIPS, i.e. 2x a 100 MHz Indy.
4. Newport and window — done: graphical PROM, X login, Indigo Magic desktop;
   `indy.exe` window with PS/2 keyboard and mouse, real time (100 MIPS
   capped; ~200 MIPS available). VDMA, GL double buffering, retrace,
   popup menus and dithered RGB images work (buttonfly, ipaste, System
   Shutdown with power off), GL demos (flight, atlantis, ideas_ogl with
   Gouraud, blast). Open: overlay planes, XL24, stipples/blending barely
   tested, VDMA zoom, the exact REX3 dither matrix.
   **Keyboard layout**: the PROM variable `keybd` (NVRAM 0xFA, i.e. env
   offset 0xBA) selects the X keymap (`/usr/lib/X11/input/PC/ES.xkm`,
   IT, GB, DE, FR...), read at boot. Set it from IRIX as root with
   `nvram keybd ES` and reboot: X then gives n-tilde, `"` on Shift+2, `@` on
   AltGr+2. Writing it ourselves does not work: `rtc_fix_checksum` only
   matches NVRAMs with an empty keybd (IRIX's checksum with keybd=ES is 43,
   the formula gives 6; the real algorithm is still unknown). The window's
   `roms\nvram\indy.nvram` now has keybd=ES (written by IRIX; the user's
   keyboard is Spanish); `roms\nvram\indy-us.nvram` is the US copy to use
   with indy_boot, whose --keys typing assumes a US layout. Without keybd,
   AltGr (Windows: fake LCtrl + RAlt) reached IRIX as Ctrl+Alt and xwsh
   printed `^[[NNNq`; indy_win32.c now drops the fake LCtrl (same message
   time as the RAlt that follows).
5. CD-ROM install from `cdrom/irix53.iso`, IRIX 6.5, ethernet (Seeq), audio.
   CD-ROM: works (automount on /CDROM, `inst` installed the demos).
   IRIX 6.5: `disks/irix65.img` (2 GB, from `D:/Mame/SGI/irix65.chd` with
   `chdman extracthd`) is "IRIX Release 6.5 IP22" (2003) with Blender 1.0
   on the desktop. With `--ram 128` it reaches the graphical login in
   ~200 s emulated, the desktop as root, and a login on the serial
   console ("IRIS console login", answer the TERM prompt first);
   `shutdown -y -g0` is clean. The 5.3 demos were copied into 6.5's
   `/usr/demos` (the demos user's home): boot 6.5 with the 5.3 disk as
   second disk (`--disk irix65.img --disk irix53.img`, SCSI ID 2), then
   `fsck -y /dev/rdsk/dks0d2s0`, `mount -r -t efs /dev/dsk/dks0d2s0 /mnt53`,
   tar copy. buttonfly and ideas run on 6.5. In `--keys`, `{KBD}` / `{SER}`
   switch between PS/2 keyboard and serial mid-session (graphical login,
   then commands and shutdown on the serial console). With `--mips 200`
   the 6.5 boot runs at ~187 MIPS real.
6. Speed: JIT to x86-64 — first version done (2026-09-30). The planned
   pre-decoded interpreter was skipped: the JIT falls back to the
   interpreter per instruction instead. Verified by the unit tests and the
   Unicorn diff with `R4K_JIT`, and by bit-identical IRIX 5.3 (shell) and
   6.5 (graphical login) boots with and without JIT (`--hash`). Bench loop
   4400 MIPS (interpreter 390); IRIX 5.3 boot 8 s instead of 100 s. The
   boot time is now mostly IRIX's idle loop.
   Same day, second round: native FPU (SSE, see JIT invariants), host
   register caching, SC, LWL/LWR/LDL/LDR, DIV/DIVU, MTC0 of
   EntryLo0/1/PageMask, the `jr` return cache. The Unicorn oracle now
   prefills the fast MMU for the data page in JIT mode (before, every
   single-instruction memory case took the slow path and the translated
   fast paths were only tested by the IRIX runs). Bench 4760 MIPS; 5.3
   boot ~7 s, 6.5 ~18 s, atlantis desktop 250 s emulated in ~22 s. In GL
   demos the Newport rendering (`to_screen`, `put_pixel`, `pack_rgb`) is
   now the largest cost, and GL's stores to REX3 registers are I/O.
   Still interpreted: LL (LLAddr needs the physical address), SWL/SWR/SDL/
   SDR, TLBWR/ERET, traps, ROUND/CEIL/FLOOR and L-format FPU conversions.
   Other ideas: `tlb_map` (linear 48-entry scan, ~3%); bigger `SLICE` in
   indy.c (every 2000 instructions the JIT goes back to C). Idle skipping
   (fast-forward when IRIX idles) would change timing: ask the user first.
   Open glitch seen by the user in the window (with and without JIT): black
   areas in context menus while moving up and down the entries.
7. **Next (agreed with the user 2026-09-30): Indigo2.** It shares MC and
   HPC3 with the Indy; differences are the board (IP22 R4400; IP26 R8000
   and IP28 R10000 are MIPS IV), front panel, EISA, and the graphics
   (Extreme/XL/Impact). Then ethernet, audio (HAL2).
   CPU notes from the same discussion: an R5000 Indy (same IP24 board and
   PROM, MAME `indy_5015`) would give MIPS IV (nekoware and `-mips4`
   software under 6.5): MOVN/MOVZ, MOVF/MOVT, PREF, COP1X (indexed FPU
   loads/stores, MADD/MSUB/NMADD/NMSUB, RECIP/RSQRT), 8 FPU condition codes,
   Status.XX, the R5000 CP0 (PRId, Config, 32+32 KB caches, secondary cache
   ops, no VCE). Emulating a "bigger" CPU is not slower (only the ISA and
   CP0 are emulated); the CPU type would go into the JIT block key and the
   native C.cond must fall back for cc != 0. R10000 64-bit kernels run from
   xkphys: extend the JIT's unmapped-region chaining (today kseg0/kseg1).
   IRIX's idle loop special-cases the R4600 (PRId 0x20), probably for
   `wait`: check whether R5000 uses it (would allow idle skipping).
8. **Public GitHub release (later, with calm; agreed 2026-09-30).** The
   history is clean (no ROM/disk/NVRAM ever committed). To do: a public
   README with "getting started" (where the PROM goes, how to get IRIX,
   what works: 5.3/6.5, GL demos, CD-ROM; not yet: network, audio, known
   glitches); a GitHub Action building the Windows binaries for Releases;
   a tool to create an empty disk image plus a guide to install IRIX from
   one's own CD (fx to partition); create the NVRAM on first start (MAC +
   checksum, as indy_boot does) instead of needing MAME's; recompress the
   3.9 MB screenshots in docs/; generalize the local `D:\` paths in
   CLAUDE.md. The icon is the user's 3D render of the SGI cube, a
   trademark (now HPE): add a "not affiliated, trademarks of their owners"
   note or use a different icon. PROM, IRIX, demos, nekoware and MAME's
   NVRAM must never be distributed.
