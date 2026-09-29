# SGI emulator (Indy / IP24)

An emulator for the Silicon Graphics Indy workstation aimed at running IRIX
**fast** rather than cycle-exact. Written in C99, built with MSVC 2022 and CMake.

Status: **stage 2 — the Indy PROM runs**: power-on diagnostics, the System
Maintenance Menu and the Command Monitor work on the serial console.

```
>> version
PROM Monitor SGI Version 5.3 Rev B10 R4X00/R5000 IP24 Feb 12, 1996 (BE)
>> hinv
                   System: IP22
                Processor: 100 Mhz R4400, with FPU
              Memory size: 64 Mbytes
```

Emulated so far: the CPU (below), the MC memory controller (RAM banks, RPSS,
VDMA), HPC3, IOC with INT3 interrupts and the 8254 timer, the Z85C30 serial
port, the DS1386 clock with the NVRAM environment, the 8042 keyboard
controller, the 93CS56 EEPROM and a first WD33C93 SCSI controller.

The CPU:

- MIPS III interpreter, 32/64 bit, big-endian
- CP0: 48-entry TLB with page sizes from 4 KB to 16 MB, ASIDs, global entries,
  Random/Wired, precise exceptions with branch-delay slots (EPC, Cause.BD),
  TLB refill and XTLB refill vectors, Count/Compare, external interrupts
- FPU (CP1): single, double, word and long formats, FR=0/1 register layouts,
  rounding modes, IEEE flags/causes/enables, MIPS "legacy" NaNs
- CACHE instruction (tag arrays for the index operations, used by the PROM
  and by kernels to size the caches)
- a fast MMU (virtual page → host pointer, per privilege mode), designed for
  the pre-decoding interpreter and JIT that come later

About 400 million instructions per second on a simple loop, as a plain
interpreter.

## Build and test

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\test_r4k.exe                    # unit tests (TLB, exceptions, FPU...)
.venv\Scripts\python tests\diff_unicorn.py    # differential test against Unicorn
build\Release\test_r4k.exe --bench            # interpreter speed
```

The differential test needs a Python virtual environment with Unicorn:
`python -m venv .venv` then `.venv\Scripts\pip install unicorn==2.1.4`.

## ROMs and IRIX media

The Indy PROM and the IRIX CDs are copyrighted and are **not** part of this
repository. Put them in `roms/` and `cdrom/` (both git-ignored).

```
build\Release\indy_boot.exe --prom roms\ip24prom.bin --trace 200
```

runs the PROM headless: the serial console goes to stdout, `--keys` types on
it, and every access to hardware that isn't emulated yet is logged.

## License

MIT, see `LICENSE`. MAME, GXemul, Linux and NetBSD sources were used only as
documentation of hardware behaviour; no code is copied from them.
