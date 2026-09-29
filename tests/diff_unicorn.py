"""
diff_unicorn.py - Confronto differenziale della CPU R4400 con Unicorn (QEMU).

Genera piccole sequenze casuali (ALU a 32/64 bit, moltiplicazioni e
divisioni, trap, load/store anche non allineate, salti con delay slot e
branch likely, FPU con i modi di arrotondamento e le eccezioni), le esegue
su Unicorn (modello R4000, big-endian) e sull'emulatore (r4k_oracle), e
confronta registri, HI/LO, FCSR, registri FPU, memoria ed eccezione presa.

La CPU gira in kernel con ERL=1, come Unicorn dopo il reset: kuseg non e'
mappato, quindi codice e dati stanno agli stessi indirizzi fisici.

    .venv\\Scripts\\python tests\\diff_unicorn.py [casi] [seme] [categoria]
"""
import random
import struct
import subprocess
import sys
from pathlib import Path

from unicorn import Uc, UcError, UC_ARCH_MIPS, UC_MODE_MIPS64, UC_MODE_BIG_ENDIAN, UC_HOOK_INTR
from unicorn import mips_const as M

ROOT = Path(__file__).resolve().parent.parent
ORACLE = next((p for p in [ROOT / "build/Release/r4k_oracle.exe",
                           ROOT / "build/Debug/r4k_oracle.exe",
                           ROOT / "build/r4k_oracle"] if p.exists()), None)

GPR = [getattr(M, f"UC_MIPS_REG_{k}") for k in range(32)]
FPR = [getattr(M, f"UC_MIPS_REG_F{k}") for k in range(32)]
DATA = 0x80000
CODE = 0x100000
CODE_SLOTS = (0x800000 - CODE) // 64   # slot da 64 byte: indirizzi nuovi per Unicorn
MAXSTEP = 32
M64 = (1 << 64) - 1
ST_BASE = 0x00400004              # BEV | ERL
ST_CU1, ST_FR = 0x20000000, 0x04000000

# eccezioni di QEMU -> ExcCode MIPS
QEMU_EXC = {12: 4, 13: 5, 15: 6, 17: 8, 18: 9, 19: 11, 20: 10, 21: 12, 22: 13,
            23: 15, 26: 2, 27: 3, 28: 7}


def sx32(v):
    v &= 0xFFFFFFFF
    return (v | ~0xFFFFFFFF) & M64 if v & 0x80000000 else v


# ---- codifica ----
def R(fn, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | fn


def I(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def COP1(fmt, ft, fs, fd, fn):
    return (0x11 << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn


def CTC1(rt, fs):
    return (0x11 << 26) | (6 << 21) | (rt << 16) | (fs << 11)


# ---- valori ----
def ival(rng):
    return rng.choice([
        0, 1, 2, 0x7FFFFFFF, 0xFFFFFFFF80000000, M64, M64 - 1, 31, 32, 33, 63, 64,
        0x8000000000000000, 0x7FFFFFFFFFFFFFFF, 0x00000000FFFFFFFF, 0x80000000,
        rng.getrandbits(8), sx32(rng.getrandbits(32)), sx32(rng.getrandbits(32)),
        rng.getrandbits(64), rng.getrandbits(64), sx32(rng.getrandbits(16)),
        1 << rng.randrange(64), sx32(1 << rng.randrange(32)),
    ])


def fval_s(rng):
    return rng.choice([
        0, 0x80000000, 0x3F800000, 0xBF800000, 0x40000000, 0x7F800000, 0xFF800000,
        0x7FBFFFFF, 0x7FC00000, 0x7F800001, 0xFFBFFFFF, 0x00000001, 0x80000001,
        0x00800000, 0x7F7FFFFF, 0xFF7FFFFF, 0x4F000000, 0xCF000000, 0x5F000000,
        0x3F000000, 0x3FC00000, 0x40200000, 0xC0200000,
        rng.getrandbits(32), rng.getrandbits(32),
        struct.unpack(">I", struct.pack(">f", rng.uniform(-1e6, 1e6)))[0],
        struct.unpack(">I", struct.pack(">f", rng.uniform(-4, 4)))[0],
        struct.unpack(">I", struct.pack(">f", float(rng.randrange(-100, 100)) + 0.5))[0],
    ])


def fval_d(rng):
    return rng.choice([
        0, 1 << 63, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000,
        0xFFF0000000000000, 0x7FF7FFFFFFFFFFFF, 0x7FF8000000000000, 0x7FF0000000000001,
        0x0000000000000001, 0x0010000000000000, 0x7FEFFFFFFFFFFFFF, 0x41E0000000000000,
        0xC1E0000000000000, 0x43E0000000000000, 0xC3E0000000000000, 0x3FE0000000000000,
        0x3FF8000000000000, 0x4004000000000000, 0xC004000000000000, 0x36A0000000000000,
        0x47F0000000000000, rng.getrandbits(64), rng.getrandbits(64),
        struct.unpack(">Q", struct.pack(">d", rng.uniform(-1e12, 1e12)))[0],
        struct.unpack(">Q", struct.pack(">d", rng.uniform(-4, 4)))[0],
        struct.unpack(">Q", struct.pack(">d", float(rng.randrange(-100, 100)) + 0.5))[0],
    ])


class Case:
    def __init__(self, rng):
        self.code = []
        self.gpr = {k: ival(rng) for k in range(1, 32)}
        self.fpr = {}
        self.status = ST_BASE | ST_CU1 | ST_FR
        self.fcsr = 0
        self.hi, self.lo = ival(rng), ival(rng)
        self.mem = bytes(rng.getrandbits(8) for _ in range(64))


def reg(rng):
    return rng.randrange(1, 32)


# ---- generatori per categoria ----
ALU_R = [0x04, 0x06, 0x07, 0x14, 0x16, 0x17, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
         0x27, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F]
SHIFT = [0x00, 0x02, 0x03, 0x38, 0x3A, 0x3B, 0x3C, 0x3E, 0x3F]
MULDIV = list(range(0x18, 0x20))
TRAP_R = [0x30, 0x31, 0x32, 0x33, 0x34, 0x36]
TRAP_I = [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0E]
ALU_I = [0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19]
LOADS = {0x20: 1, 0x21: 2, 0x22: 0, 0x23: 4, 0x24: 1, 0x25: 2, 0x26: 0, 0x27: 4,
         0x37: 8, 0x1A: 0, 0x1B: 0, 0x30: 4, 0x34: 8}
STORES = {0x28: 1, 0x29: 2, 0x2A: 0, 0x2B: 4, 0x2C: 0, 0x2D: 0, 0x2E: 0, 0x3F: 8}


# operazioni a 32 bit: con operandi non estesi dal segno il risultato e'
# imprevedibile per l'architettura (e QEMU fa a modo suo): solo valori estesi
OPS32_R = {0x04: "t", 0x06: "t", 0x07: "t", 0x20: "st", 0x21: "st", 0x22: "st", 0x23: "st"}
OPS32_I = {0x08, 0x09}


def gen_alu(rng, c):
    k = rng.random()
    if k < 0.4:
        fn, rs, rt = rng.choice(ALU_R), reg(rng), reg(rng)
        c.code = [R(fn, rs, rt, rng.randrange(32))]
        for f, r_ in (("s", rs), ("t", rt)):
            if f in OPS32_R.get(fn, ""):
                c.gpr[r_] = sx32(c.gpr[r_])
    elif k < 0.6:
        fn, rt = rng.choice(SHIFT), reg(rng)
        c.code = [R(fn, 0, rt, rng.randrange(32), rng.randrange(32))]
        if fn < 4:
            c.gpr[rt] = sx32(c.gpr[rt])
    else:
        op, rs = rng.choice(ALU_I), reg(rng)
        imm = rng.choice([0, 1, -1, 0x7FFF, -0x8000, rng.getrandbits(16)])
        c.code = [I(op, rs, rng.randrange(32), imm)]
        if op in OPS32_I:
            c.gpr[rs] = sx32(c.gpr[rs])


def gen_muldiv(rng, c):
    a, b = reg(rng), reg(rng)
    if rng.random() < 0.15:
        c.gpr[b] = rng.choice([0, M64, sx32(0xFFFFFFFF)])
    if rng.random() < 0.1:
        c.gpr[a] = rng.choice([0x8000000000000000, 0xFFFFFFFF80000000])
    c.code = [R(rng.choice(MULDIV), a, b)]
    if rng.random() < 0.5:
        c.code.append(R(rng.choice([0x10, 0x12]), rd=rng.randrange(32)))
    if rng.random() < 0.2:
        c.code = [R(rng.choice([0x11, 0x13]), reg(rng))] + c.code


def gen_trap(rng, c):
    a, b = reg(rng), reg(rng)
    if rng.random() < 0.3:
        c.gpr[b] = c.gpr[a]
    if rng.random() < 0.5:
        c.code = [R(rng.choice(TRAP_R), a, b)]
    else:
        imm = rng.choice([0, 1, -1, rng.getrandbits(16)])
        if rng.random() < 0.3:
            c.gpr[a] = sx32(imm & 0xFFFF) if imm & 0x8000 else imm & 0xFFFF
        c.code = [I(1, a, rng.choice(TRAP_I), imm)]


def gen_mem(rng, c):
    load = rng.random() < 0.55
    op, size = rng.choice(list((LOADS if load else STORES).items()))
    if size and rng.random() < 0.92:
        e = rng.randrange(0, 57 - size + 1) & ~(size - 1)
    else:
        e = rng.randrange(0, 57)
    imm = rng.randrange(-16, 17)
    base = reg(rng)
    c.gpr[base] = DATA + e - imm
    rt = rng.randrange(32) if load else reg(rng)
    if rt == base and not load:
        rt = rng.randrange(1, 32)
        c.gpr[base] = DATA + e - imm
    c.code = [I(op, base, rt, imm)]
    if op in (0x30, 0x34) and rng.random() < 0.5 and rt != base:   # LL + SC
        c.code.append(I(0x38 if op == 0x30 else 0x3C, base, reg(rng) if rng.random() < 0.5 else rt, imm))


def gen_branch(rng, c):
    x = rng.choice([k for k in range(1, 31)])
    c.gpr[x] = rng.getrandbits(16)
    tail = [I(0x09, x, x, 1), I(0x09, x, x, 2), I(0x09, x, x, 4)]
    rs, rt = reg(rng), reg(rng)
    if rng.random() < 0.35:
        c.gpr[rt] = c.gpr[rs]
    if rng.random() < 0.3:
        c.gpr[rs] = rng.choice([0, 1, M64, 0x8000000000000000])
    kind = rng.random()
    if kind < 0.35:
        op = rng.choice([0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17])
        br = I(op, rs, rt if op in (0x04, 0x05, 0x14, 0x15) else 0, 2)   # BLEZ/BGTZ: rt = 0
    elif kind < 0.6:
        br = I(1, rs, rng.choice([0, 1, 2, 3, 0x10, 0x11, 0x12, 0x13]), 2)
    elif kind < 0.8:
        br = ("J", rng.choice([2, 3]))
    else:
        rd = rng.choice([31, reg(rng)])
        while rd == rs:
            rd = reg(rng)
        br = ("JR", rs, rd, rng.random() < 0.5)
    c.code = [br] + tail


def gen_fpu(rng, c):
    fr = rng.random() < 0.8
    c.status = ST_BASE | ST_CU1 | (ST_FR if fr else 0)
    if rng.random() < 0.02:
        c.status &= ~ST_CU1
    fcsr = rng.randrange(4)
    if rng.random() < 0.3:
        fcsr |= rng.getrandbits(5) << 2
    if rng.random() < 0.15:
        fcsr |= rng.getrandbits(5) << 7
    if rng.random() < 0.1 and not fcsr & 0xF80:
        fcsr |= 1 << 24                  # FS, solo senza trap (vedi qemu_quirk)
    if rng.random() < 0.5:
        fcsr |= 1 << 23
    c.fcsr = fcsr
    c.gpr[1] = fcsr

    def fr_ok(dbl):
        return lambda: rng.randrange(32) if fr or not dbl else rng.randrange(0, 32, 2)

    for k in range(32):
        c.fpr[k] = rng.getrandbits(64 if fr else 32)
    kind = rng.random()
    if kind < 0.75:
        fmt = rng.choice([16, 17])
        dbl = fmt == 17
        pick = fr_ok(dbl)
        # niente formato L (CVT.L, ROUND.L, CVT.D.L...): QEMU li accetta solo
        # dal MIPS IV e da' RI; si provano in test_r4k.c
        fn = rng.choice([0, 1, 2, 3, 4, 5, 6, 7, 12, 13, 14, 15, 32, 33, 36]
                        + list(range(48, 64)))
        if fn == 32 and fmt == 16:
            fmt = rng.choice([17, 20])
        if fn == 33 and fmt == 17:
            fmt = rng.choice([16, 20])
        src_dbl = fmt in (17, 21)
        dst_dbl = (fn in (8, 9, 10, 11, 37, 33)) or (fn < 8 and dbl)
        fs, ft = fr_ok(src_dbl)(), fr_ok(src_dbl)()
        fd = fr_ok(dst_dbl)() if fn < 48 else 0      # C.cond: cc e i bit 7:6 a zero
        for r_ in (fs, ft):
            if fmt == 17:
                v = fval_d(rng)
            elif fmt == 16:
                v = fval_s(rng)
            elif fmt == 20:
                v = sx32(rng.getrandbits(32)) & 0xFFFFFFFF
            else:
                v = rng.choice([rng.getrandbits(64), sx32(rng.getrandbits(32)), rng.getrandbits(20)])
            if fr or not src_dbl:
                if not src_dbl and fr:
                    v |= rng.getrandbits(32) << 32
                c.fpr[r_] = v
            else:
                c.fpr[r_], c.fpr[r_ + 1] = v & 0xFFFFFFFF, v >> 32
        c.code = [CTC1(1, 31), COP1(fmt, ft, fs, fd, fn)]
        if fn >= 48 and rng.random() < 0.5:
            x = rng.randrange(2, 31)
            c.gpr[x] = 0
            bc = (0x11 << 26) | (8 << 21) | (rng.randrange(4) << 16) | 2
            c.code += [bc, I(0x09, x, x, 1), I(0x09, x, x, 2), I(0x09, x, x, 4)]
    elif kind < 0.88:
        base = reg(rng)
        while base == 1:
            base = reg(rng)
        op = rng.choice([0x31, 0x35, 0x39, 0x3D])
        size = 4 if op in (0x31, 0x39) else 8
        e = rng.randrange(0, 57 - size + 1) & ~(size - 1)
        imm = rng.randrange(-16, 17)
        c.gpr[base] = DATA + e - imm
        ft = fr_ok(size == 8)()
        c.code = [CTC1(1, 31), I(op, base, ft, imm)]
    else:
        sub = rng.choice([0, 1, 4, 5, 2])
        rt = reg(rng)
        fs = 31 if sub == 2 else fr_ok(sub in (1, 5))()
        c.code = [CTC1(1, 31), (0x11 << 26) | (sub << 21) | (rt << 16) | (fs << 11)]


def gen_misc(rng, c):
    k = rng.random()
    if k < 0.3:
        c.code = [R(rng.choice([0x0C, 0x0D]), sa=rng.getrandbits(5))]
    else:        # istruzioni che l'R4000 non ha: devono dare RI (QEMU esegue
                 # comunque SPECIAL2 e SPECIAL 0x05, che qui non si provano)
        c.code = [rng.choice([R(0x0A, reg(rng), reg(rng), reg(rng)), R(0x0B, reg(rng), reg(rng), reg(rng)),
                              R(0x01, reg(rng), 0, reg(rng)), R(0x28, 1, 2, 3), I(0x33, reg(rng), reg(rng), 0)])]


GENERATORS = {"alu": (gen_alu, 30), "muldiv": (gen_muldiv, 10), "trap": (gen_trap, 5),
              "mem": (gen_mem, 20), "branch": (gen_branch, 15), "fpu": (gen_fpu, 30),
              "misc": (gen_misc, 2)}


def finalize(c, addr):
    """risolve i salti simbolici ora che l'indirizzo e' noto"""
    words = []
    for w in c.code:
        if isinstance(w, tuple):
            target = addr + 12
            if w[0] == "J":
                w = (w[1] << 26) | ((target >> 2) & 0x3FFFFFF)
            else:
                _, rs, rd, link = w
                c.gpr[rs] = target
                w = R(0x09, rs, 0, rd) if link else R(0x08, rs)
        words.append(w)
    return words


def oracle_line(c, addr, words):
    fr = bool(c.status & ST_FR)
    parts = [f"{addr:x} {c.status:x} {c.fcsr:x} {c.hi & M64:x} {c.lo & M64:x} {len(words):x}"]
    parts += [f"{w:x}" for w in words]
    parts.append(f"{len(c.gpr):x}")
    parts += [f"{k:x} {v & M64:x}" for k, v in c.gpr.items()]
    parts.append(f"{len(c.fpr):x}")
    parts += [f"{k:x} {v & (M64 if fr else 0xFFFFFFFF):x}" for k, v in c.fpr.items()]
    parts.append(c.mem.hex())
    return " ".join(parts)


def run_unicorn(uc, c, addr, words):
    fr = bool(c.status & ST_FR)
    code = b"".join(struct.pack(">I", w) for w in words)
    uc.mem_write(addr, code)
    uc.ctl_remove_cache(addr, addr + len(code))
    uc.mem_write(DATA, c.mem)
    uc.reg_write(M.UC_MIPS_REG_CP0_STATUS, c.status)
    for k in range(1, 32):
        uc.reg_write(GPR[k], c.gpr.get(k, 0) & M64)
    uc.reg_write(M.UC_MIPS_REG_HI, c.hi & M64)
    uc.reg_write(M.UC_MIPS_REG_LO, c.lo & M64)
    for k in range(32):
        uc.reg_write(FPR[k], c.fpr.get(k, 0) & (M64 if fr else 0xFFFFFFFF))
    uc.reg_write(M.UC_MIPS_REG_FCSR, c.fcsr)
    run_unicorn.exc = -1
    try:
        uc.emu_start(addr, addr + len(code), 0, MAXSTEP)
    except UcError as e:
        if run_unicorn.exc < 0:
            run_unicorn.exc = f"err:{e}"
    fp = [uc.reg_read(FPR[k]) & (M64 if fr else 0xFFFFFFFF) for k in range(32)]
    return (run_unicorn.exc, uc.reg_read(M.UC_MIPS_REG_HI) & M64, uc.reg_read(M.UC_MIPS_REG_LO) & M64,
            uc.reg_read(M.UC_MIPS_REG_FCSR) & 0x0183FFFF,
            [uc.reg_read(GPR[k]) & M64 for k in range(32)], fp, bytes(uc.mem_read(DATA, 64)))


def qemu_quirk(c, words, got, exp):
    """Differenze note in cui si segue l'R4400 e non QEMU."""
    if got[0] != exp[0] or got[1:3] != exp[1:3] or got[4:] != exp[4:]:
        # CVT.S.D di un qNaN con la mantissa che si perde: QEMU da' infinito,
        # qui il NaN di default
        w = words[-1]
        fd = (w >> 6) & 31
        if (w >> 21) & 31 == 17 and w & 63 == 32 and got[5][fd] & 0xFFFFFFFF == 0x7FBFFFFF                 and exp[5][fd] & 0x7FFFFFFF == 0x7F800000:
            return got[:5] == exp[:5] and got[6] == exp[6] and                 all(a == b for k, (a, b) in enumerate(zip(got[5], exp[5])) if k != fd)
        return False
    # FS = 1: un risultato denormale azzerato segnala Underflow e Inexact
    # (manuale R4000); QEMU non segnala niente
    if c.fcsr & (1 << 24):
        ui = (3 << 12) | (3 << 2)
        return got[3] & ~ui == exp[3] & ~ui
    return False


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 30000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    only = sys.argv[3] if len(sys.argv) > 3 else None
    if ORACLE is None:
        sys.exit("r4k_oracle non trovato: compilare prima (cmake --build build --config Release)")
    rng = random.Random(seed)
    gens = [(k, g, w) for k, (g, w) in GENERATORS.items() if only in (None, k)]
    names = [k for k, _, _ in gens]
    weights = [w for _, _, w in gens]
    genmap = {n: g for n, g, _ in gens}

    uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS64 | UC_MODE_BIG_ENDIAN)
    uc.ctl_set_cpu_model(M.UC_CPU_MIPS64_R4000)
    uc.mem_map(0, 0x800000)

    def on_intr(u, intno, _):
        run_unicorn.exc = QEMU_EXC.get(intno, f"qemu{intno}")
        u.emu_stop()
    uc.hook_add(UC_HOOK_INTR, on_intr)

    cases = []
    for k in range(count):
        name = rng.choices(names, weights)[0]
        c = Case(rng)
        genmap[name](rng, c)
        addr = CODE + 64 * (k % CODE_SLOTS)
        words = finalize(c, addr)
        cases.append((name, c, addr, words))

    stdin = "\n".join(oracle_line(c, a, w) for _, c, a, w in cases) + "\n"
    out = subprocess.run([str(ORACLE)], input=stdin, capture_output=True, text=True, check=True)
    lines = out.stdout.splitlines()
    assert len(lines) == count, f"l'oracolo ha risposto {len(lines)} righe su {count}"

    failures = {}
    shown = 0
    for (name, c, addr, words), line in zip(cases, lines):
        t = line.split()
        got = (int(t[0]), int(t[1], 16), int(t[2], 16), int(t[3], 16) & 0x0183FFFF,
               [int(x, 16) for x in t[4:36]], [int(x, 16) for x in t[36:68]], bytes.fromhex(t[68]))
        exp = run_unicorn(uc, c, addr, words)
        if got != exp and not qemu_quirk(c, words, got, exp):
            failures[name] = failures.get(name, 0) + 1
            if shown < 12:
                shown += 1
                print(f"DIVERSO [{name}] " + " ".join(f"{w:08X}" for w in words))
                if name == "fpu":
                    w = words[1]
                    fs_, ft_ = (w >> 11) & 31, (w >> 16) & 31
                    print(f"   fcsr iniziale {c.fcsr:08X}  fs=f{fs_}:{c.fpr.get(fs_, 0):016X}"
                          f"  ft=f{ft_}:{c.fpr.get(ft_, 0):016X}")
                labels = ["exc", "hi", "lo", "fcsr"]
                for i, lab in enumerate(labels):
                    if got[i] != exp[i]:
                        print(f"   {lab}: unicorn {exp[i]!r:>20}  r4k {got[i]!r}")
                for k in range(32):
                    if got[4][k] != exp[4][k]:
                        print(f"   r{k}: unicorn {exp[4][k]:016X}  r4k {got[4][k]:016X}  (era {c.gpr.get(k, 0) & M64:016X})")
                for k in range(32):
                    if got[5][k] != exp[5][k]:
                        print(f"   f{k}: unicorn {exp[5][k]:016X}  r4k {got[5][k]:016X}  (era {c.fpr.get(k, 0):016X})")
                if got[6] != exp[6]:
                    print(f"   mem: unicorn {exp[6].hex()}\n        r4k     {got[6].hex()}")
    total = sum(failures.values())
    print(f"{count} casi, {total} differenze" + (f" {failures}" if failures else ""))
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
