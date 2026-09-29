# SGI emulator (Indy / IP24)

An emulator for the Silicon Graphics Indy workstation aimed at running IRIX
**fast** rather than cycle-exact. Written in C99, built with MSVC 2022 and CMake.

Status: **IRIX 5.3 boots to the Indigo Magic desktop.** The Indy PROM runs
its diagnostics and graphical interface on the emulated Newport (XL8) board,
loads `sash` and the kernel from an emulated SCSI disk, and IRIX comes up with
the X server, the graphical login and the desktop, in real time.

![IRIX 5.3 desktop](docs/irix-desktop.png)

| PROM | Login |
|---|---|
| ![PROM](docs/prom.png) | ![Login](docs/irix-login.png) |

Emulated: the R4400 CPU (below), the MC memory controller (RAM banks, RPSS,
VDMA), HPC3 with SCSI DMA, IOC with INT3 interrupts and the 8254 timer, the
Z85C30 serial port, the DS1386 clock with the NVRAM environment, the 8042
keyboard/mouse controller, the 93CS56 EEPROM, the WD33C93 SCSI controllers,
SCSI disks / CD-ROMs on image files, and the Newport graphics (REX3 drawing
engine, VC2 with cursor and DID tables, CMAP, XMAP9, BT445).

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

About 200 million instructions per second while booting IRIX, as a plain
interpreter: twice a 100 MHz Indy. The window runs at real time.

## Running it

```
build\Release\indy.exe [--prom roms\ip24prom.bin] [--disk disks\irix53.img] [--cdrom cdrom\x.iso]
```

Without arguments it looks for `roms\ip24prom.bin` and `disks\irix53.img`.
Click in the window to capture the mouse, Ctrl+F11 releases it. The keyboard
is passed through by physical key (PS/2 scan codes): choose the layout in
IRIX. The serial console is printed in the terminal that started the program.
The NVRAM (clock and PROM environment) is saved in `roms\nvram\indy.nvram`.

## Build and test

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
build\Release\test_r4k.exe                    # CPU unit tests (TLB, exceptions, FPU...)
build\Release\test_devices.exe                # the Indy chips one by one
.venv\Scripts\python tests\diff_unicorn.py    # differential test against Unicorn
build\Release\test_r4k.exe --bench            # interpreter speed
```

The differential test needs a Python virtual environment with Unicorn:
`python -m venv .venv` then `.venv\Scripts\pip install unicorn==2.1.4`.

`build\Release\indy_boot.exe` runs the machine headless: serial console on
stdout, `--keys` typed on the serial port or (`--kbd`) on the PS/2 keyboard,
`--png` saves the screen, and many tracing options (see `CLAUDE.md`).

## ROMs and IRIX media

The Indy PROM, disk images and IRIX CDs are copyrighted and are **not** part
of this repository. Put them in `roms/`, `disks/` and `cdrom/` (git-ignored).

## License

MIT, see `LICENSE`. MAME, GXemul, Linux and NetBSD sources were used only as
documentation of hardware behaviour; no code is copied from them.
