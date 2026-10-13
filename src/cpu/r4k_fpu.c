/*
 * r4k_fpu.c - Coprocessore 1 (FPU R4400, MIPS III)
 *
 * I calcoli si fanno con la FPU dell'host (SSE), impostando il modo di
 * arrotondamento di FCSR e leggendo i flag IEEE con fenv. Due differenze
 * da gestire a mano:
 *
 *  - i NaN: il MIPS "legacy" ha il bit piu' alto della mantissa a 1 nei NaN
 *    *segnalanti* (al contrario di x86). Un sNaN in ingresso da' Invalid e
 *    il NaN di default (0x7FBFFFFF / 0x7FF7FFFFFFFFFFFF); un qNaN in
 *    ingresso passa invariato (prima a, poi b), come fa QEMU;
 *  - le conversioni verso intero fuori scala o da NaN danno Invalid e
 *    0x7FFFFFFF (0x7FFFFFFFFFFFFFFF per i long).
 *
 * L'R4400 vero da' Unimplemented Operation per i denormali e i casi
 * strani, e IRIX li emula nel kernel: qui si calcolano direttamente.
 *
 * MIPS IV (R10000, c->mips4): C.cond e BC1 hanno un codice di condizione
 * (cc 0..7, r4k_fcc), MOVF/MOVT/MOVZ/MOVN.fmt, RECIP e RSQRT (calcolati
 * esatti), e COP1X: load/store indicizzate e MADD/MSUB/NMADD/NMSUB senza
 * fusione (il prodotto viene arrotondato prima della somma).
 */
#include <math.h>
#include <fenv.h>
#include "cpu/r4k_internal.h"

#if defined(_MSC_VER)
#pragma fenv_access (on)
#endif

#define FMT_S 16
#define FMT_D 17
#define FMT_W 20
#define FMT_L 21

#define DNAN_S 0x7FBFFFFFu
#define DNAN_D 0x7FF7FFFFFFFFFFFFull

static const int host_rm[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };

static inline float    f_of(uint32_t x) { float f; memcpy(&f, &x, 4); return f; }
static inline double   d_of(uint64_t x) { double d; memcpy(&d, &x, 8); return d; }
static inline uint32_t bits_f(float f)  { uint32_t x; memcpy(&x, &f, 4); return x; }
static inline uint64_t bits_d(double d) { uint64_t x; memcpy(&x, &d, 8); return x; }

static inline int nan_s(uint32_t x)  { return (x & 0x7F800000u) == 0x7F800000u && (x & 0x007FFFFFu); }
static inline int snan_s(uint32_t x) { return nan_s(x) && (x & 0x00400000u); }
static inline int nan_d(uint64_t x)  { return (x & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (x & 0x000FFFFFFFFFFFFFull); }
static inline int snan_d(uint64_t x) { return nan_d(x) && (x & 0x0008000000000000ull); }
static inline int denorm_s(uint32_t x) { return !(x & 0x7F800000u) && (x & 0x007FFFFFu); }
static inline int denorm_d(uint64_t x) { return !(x & 0x7FF0000000000000ull) && (x & 0x000FFFFFFFFFFFFFull); }

static uint32_t host_flags(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT);
    uint32_t f = 0;
    if (e & FE_INEXACT)   f |= FPX_I;
    if (e & FE_UNDERFLOW) f |= FPX_U;
    if (e & FE_OVERFLOW)  f |= FPX_O;
    if (e & FE_DIVBYZERO) f |= FPX_Z;
    if (e & FE_INVALID)   f |= FPX_V;
    return f;
}

/* Scrive Cause; se l'eccezione e' abilitata prende FPE e ritorna 0
   (il registro destinazione non va toccato), altrimenti aggiorna i flag. */
static int fpu_finish(R4k *c, uint32_t flags)
{
    uint32_t en = (c->fcr31 >> 7) & 31;
    c->fcr31 = (c->fcr31 & ~FCSR_CAUSE) | (flags << 12);
    if ((flags & en) || (flags & FPX_E)) { r4k_exception(c, EXC_FPE, 0); return 0; }
    c->fcr31 |= (flags & 31) << 2;
    return 1;
}

/* NaN in ingresso: 1 se il risultato e' gia' deciso */
static int nan2_s(uint32_t a, uint32_t b, uint32_t *res, uint32_t *fl)
{
    if (!nan_s(a) && !nan_s(b)) return 0;
    if (snan_s(a) || snan_s(b)) { *fl |= FPX_V; *res = DNAN_S; return 1; }
    *res = nan_s(a) ? a : b;
    return 1;
}

static int nan2_d(uint64_t a, uint64_t b, uint64_t *res, uint32_t *fl)
{
    if (!nan_d(a) && !nan_d(b)) return 0;
    if (snan_d(a) || snan_d(b)) { *fl |= FPX_V; *res = DNAN_D; return 1; }
    *res = nan_d(a) ? a : b;
    return 1;
}

static int nan1_s(uint32_t a, uint32_t *res, uint32_t *fl)
{
    if (!nan_s(a)) return 0;
    if (snan_s(a)) { *fl |= FPX_V; *res = DNAN_S; } else *res = a;
    return 1;
}

static int nan1_d(uint64_t a, uint64_t *res, uint32_t *fl)
{
    if (!nan_d(a)) return 0;
    if (snan_d(a)) { *fl |= FPX_V; *res = DNAN_D; } else *res = a;
    return 1;
}

/* risultato dell'host: NaN generati (0/0, inf-inf, sqrt(-1)) diventano il
   NaN di default, i denormali con FS=1 diventano zero */
static uint32_t fix_s(R4k *c, uint32_t x, uint32_t *fl)
{
    if (nan_s(x)) return DNAN_S;
    if ((c->fcr31 & FCSR_FS) && denorm_s(x)) { *fl |= FPX_U | FPX_I; return x & 0x80000000u; }
    return x;
}

static uint64_t fix_d(R4k *c, uint64_t x, uint32_t *fl)
{
    if (nan_d(x)) return DNAN_D;
    if ((c->fcr31 & FCSR_FS) && denorm_d(x)) { *fl |= FPX_U | FPX_I; return x & 0x8000000000000000ull; }
    return x;
}

/* conversione verso intero: mode 0..3 come RM, -1 = quello di FCSR */
static uint64_t to_int(R4k *c, double x, int is_nan, int mode, int is64, uint32_t *fl)
{
    const uint64_t bad = is64 ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
    if (is_nan) { *fl |= FPX_V; return bad; }
    double r;
    switch (mode < 0 ? (int)(c->fcr31 & 3) : mode) {
    case 0:  { int old = fegetround(); fesetround(FE_TONEAREST); r = nearbyint(x); fesetround(old); break; }
    case 1:  r = trunc(x); break;
    case 2:  r = ceil(x); break;
    default: r = floor(x); break;
    }
    if (is64 ? !(r >= -9223372036854775808.0 && r < 9223372036854775808.0)
             : !(r >= -2147483648.0 && r <= 2147483647.0)) {
        *fl |= FPX_V;
        return bad;
    }
    if (r != x) *fl |= FPX_I;
    return is64 ? (uint64_t)(int64_t)r : (uint64_t)(uint32_t)(int32_t)r;
}

/* ---- operazioni con formato S o D ---- */

static void fpu_arith(R4k *c, uint32_t insn)
{
    int fmt = (insn >> 21) & 31, fn = insn & 63;
    int ft = (insn >> 16) & 31, fs = (insn >> 11) & 31, fd = (insn >> 6) & 31;
    int dbl = fmt == FMT_D;

    /* MIPS IV: MOVF/MOVT, MOVZ, MOVN copiano senza flag */
    if (c->mips4 && fn >= 17 && fn <= 19 && (fmt == FMT_S || fmt == FMT_D)) {
        int take = fn == 17 ? r4k_fcc(c, (insn >> 18) & 7) == (int)((insn >> 16) & 1)
                 : fn == 18 ? c->r[ft] == 0 : c->r[ft] != 0;
        if (take) { if (dbl) set_fpr_l(c, fd, fpr_l(c, fs)); else set_fpr_w(c, fd, fpr_w(c, fs)); }
        return;
    }

    /* ABS, MOV, NEG: solo il bit di segno, niente flag */
    if (fn >= 5 && fn <= 7 && (fmt == FMT_S || fmt == FMT_D)) {
        if (dbl) {
            uint64_t x = fpr_l(c, fs);
            set_fpr_l(c, fd, fn == 5 ? x & ~(1ull << 63) : fn == 7 ? x ^ (1ull << 63) : x);
        } else {
            uint32_t x = fpr_w(c, fs);
            set_fpr_w(c, fd, fn == 5 ? x & 0x7FFFFFFFu : fn == 7 ? x ^ 0x80000000u : x);
        }
        return;
    }

    int valid;
    switch (fn) {
    case 0: case 1: case 2: case 3: case 4:              /* ADD SUB MUL DIV SQRT */
    case 8: case 9: case 10: case 11:                    /* ROUND.L TRUNC.L CEIL.L FLOOR.L */
    case 12: case 13: case 14: case 15:                  /* ROUND.W ... FLOOR.W */
    case 36: case 37:                                    /* CVT.W CVT.L */
        valid = fmt == FMT_S || fmt == FMT_D; break;
    case 21: case 22:                                    /* RECIP RSQRT (MIPS IV) */
        valid = c->mips4 && (fmt == FMT_S || fmt == FMT_D); break;
    case 32: valid = fmt == FMT_D || fmt == FMT_W || fmt == FMT_L; break;   /* CVT.S */
    case 33: valid = fmt == FMT_S || fmt == FMT_W || fmt == FMT_L; break;   /* CVT.D */
    default: valid = fn >= 48 && (fmt == FMT_S || fmt == FMT_D); break;     /* C.cond */
    }
    if (!valid) { r4k_exception(c, EXC_RI, 0); return; }

    uint32_t fl = 0;
    int rm = (int)(c->fcr31 & 3);
    if (rm) fesetround(host_rm[rm]);
    feclearexcept(FE_ALL_EXCEPT);

    uint64_t res = 0;
    int res64 = 0, write = 1, cond = -1;

    if (fn <= 4) {                                       /* aritmetica */
        if (dbl) {
            uint64_t a = fpr_l(c, fs), b = fn == 4 ? 0 : fpr_l(c, ft), n;
            res64 = 1;
            if (fn == 4 ? nan1_d(a, &n, &fl) : nan2_d(a, b, &n, &fl)) res = n;
            else {
                double x = d_of(a), y = d_of(b), z;
                switch (fn) {
                case 0: z = x + y; break;
                case 1: z = x - y; break;
                case 2: z = x * y; break;
                case 3: z = x / y; break;
                default: z = sqrt(x); break;
                }
                fl |= host_flags();
                res = fix_d(c, bits_d(z), &fl);
            }
        } else {
            uint32_t a = fpr_w(c, fs), b = fn == 4 ? 0 : fpr_w(c, ft), n;
            if (fn == 4 ? nan1_s(a, &n, &fl) : nan2_s(a, b, &n, &fl)) res = n;
            else {
                float x = f_of(a), y = f_of(b), z;
                switch (fn) {
                case 0: z = x + y; break;
                case 1: z = x - y; break;
                case 2: z = x * y; break;
                case 3: z = x / y; break;
                default: z = sqrtf(x); break;
                }
                fl |= host_flags();
                res = fix_s(c, bits_f(z), &fl);
            }
        }
    } else if (fn == 21 || fn == 22) {                   /* RECIP RSQRT */
        if (dbl) {
            uint64_t a = fpr_l(c, fs), n;
            res64 = 1;
            if (nan1_d(a, &n, &fl)) res = n;
            else {
                double x = d_of(a), z = fn == 21 ? 1.0 / x : 1.0 / sqrt(x);
                fl |= host_flags();
                res = fix_d(c, bits_d(z), &fl);
            }
        } else {
            uint32_t a = fpr_w(c, fs), n;
            if (nan1_s(a, &n, &fl)) res = n;
            else {
                float x = f_of(a), z = fn == 21 ? 1.0f / x : 1.0f / sqrtf(x);
                fl |= host_flags();
                res = fix_s(c, bits_f(z), &fl);
            }
        }
    } else if ((fn >= 8 && fn <= 15) || fn == 36 || fn == 37) {    /* verso intero */
        int is64 = fn == 37 || (fn >= 8 && fn <= 11);
        int mode = fn >= 36 ? -1 : (fn & 3);
        double x;
        int is_nan;
        if (dbl) {
            uint64_t a = fpr_l(c, fs);
            is_nan = nan_d(a);
            x = is_nan ? 0 : d_of(a);
        } else {
            uint32_t a = fpr_w(c, fs);
            is_nan = nan_s(a);
            x = is_nan ? 0 : (double)f_of(a);
        }
        res = to_int(c, x, is_nan, mode, is64, &fl);
        res64 = is64;
    } else if (fn == 32) {                               /* CVT.S */
        if (fmt == FMT_D) {
            uint64_t a = fpr_l(c, fs);
            if (nan_d(a)) {
                if (snan_d(a)) { fl |= FPX_V; res = DNAN_S; }
                else {
                    uint32_t m = (uint32_t)((a >> 29) & 0x7FFFFF);
                    res = m ? (uint32_t)((a >> 32) & 0x80000000u) | 0x7F800000u | m : DNAN_S;
                }
            } else {
                float z = (float)d_of(a);
                fl |= host_flags();
                res = fix_s(c, bits_f(z), &fl);
            }
        } else {
            float z = fmt == FMT_W ? (float)(int32_t)fpr_w(c, fs) : (float)(int64_t)fpr_l(c, fs);
            fl |= host_flags();
            res = bits_f(z);
        }
    } else if (fn == 33) {                               /* CVT.D */
        res64 = 1;
        if (fmt == FMT_S) {
            uint32_t a = fpr_w(c, fs);
            if (nan_s(a)) {
                if (snan_s(a)) { fl |= FPX_V; res = DNAN_D; }
                else res = ((uint64_t)(a & 0x80000000u) << 32) | 0x7FF0000000000000ull
                         | ((uint64_t)(a & 0x7FFFFF) << 29);
            } else {
                res = bits_d((double)f_of(a));
                fl |= host_flags();
            }
        } else {
            double z = fmt == FMT_W ? (double)(int32_t)fpr_w(c, fs) : (double)(int64_t)fpr_l(c, fs);
            fl |= host_flags();
            res = bits_d(z);
        }
    } else {                                             /* C.cond */
        int cc = fn & 15, un, eq = 0, lt = 0;
        write = 0;
        if (dbl) {
            uint64_t a = fpr_l(c, fs), b = fpr_l(c, ft);
            un = nan_d(a) || nan_d(b);
            if (snan_d(a) || snan_d(b)) fl |= FPX_V;
            if (!un) { eq = d_of(a) == d_of(b); lt = d_of(a) < d_of(b); }
        } else {
            uint32_t a = fpr_w(c, fs), b = fpr_w(c, ft);
            un = nan_s(a) || nan_s(b);
            if (snan_s(a) || snan_s(b)) fl |= FPX_V;
            if (!un) { eq = f_of(a) == f_of(b); lt = f_of(a) < f_of(b); }
        }
        if (un && (cc & 8)) fl |= FPX_V;
        cond = ((cc & 1) && un) || ((cc & 2) && eq) || ((cc & 4) && lt);
    }

    if (rm) fesetround(FE_TONEAREST);
    if (!fpu_finish(c, fl)) return;
    if (cond >= 0) {
        int cc = c->mips4 ? (int)(insn >> 8) & 7 : 0;
        uint32_t bit = cc ? 1u << (24 + cc) : FCSR_C;
        if (cond) c->fcr31 |= bit; else c->fcr31 &= ~bit;
    } else if (write) {
        if (res64) set_fpr_l(c, fd, res); else set_fpr_w(c, fd, (uint32_t)res);
    }
}

void r4k_cop1(R4k *c, uint32_t insn)
{
    int rt = (insn >> 16) & 31, fs = (insn >> 11) & 31;
    switch ((insn >> 21) & 31) {
    case 0: c->r[rt] = SX32(fpr_w(c, fs)); return;                 /* MFC1 */
    case 1:                                                        /* DMFC1 */
        if (!c->allow64) break;
        c->r[rt] = fpr_l(c, fs);
        return;
    case 2:                                                        /* CFC1 */
        c->r[rt] = fs == 0 ? SX32(c->fcr0) : fs == 31 ? SX32(c->fcr31) : 0;
        return;
    case 4: set_fpr_w(c, fs, (uint32_t)c->r[rt]); return;          /* MTC1 */
    case 5:                                                        /* DMTC1 */
        if (!c->allow64) break;
        set_fpr_l(c, fs, c->r[rt]);
        return;
    case 6:                                                        /* CTC1 */
        if (fs == 31) {
            c->fcr31 = (uint32_t)c->r[rt] & (c->mips4 ? FCSR_MASK | FCSR_CC17 : FCSR_MASK);
            uint32_t cause = (c->fcr31 >> 12) & 63, en = ((c->fcr31 >> 7) & 31) | FPX_E;
            if (cause & en) r4k_exception(c, EXC_FPE, 0);
        }
        return;
    case 8: {                                                      /* BC1F/T/FL/TL */
        int cond = r4k_fcc(c, c->mips4 ? (int)(insn >> 18) & 7 : 0);
        int want = (insn >> 16) & 1, likely = (insn >> 17) & 1;
        uint64_t target = c->pc + 4 + ((uint64_t)(int64_t)(int16_t)insn << 2);
        int take = cond == want;
        if (likely) {
            if (take) { c->branch_pending = 1; c->branch_target = target; }
            else      { c->pc += 8; c->pc_set = 1; }
        } else {
            c->branch_pending = 1;
            c->branch_target = take ? target : c->pc + 8;
        }
        return;
    }
    case FMT_S: case FMT_D: case FMT_W: case FMT_L:
        fpu_arith(c, insn);
        return;
    default: break;
    }
    r4k_exception(c, EXC_RI, 0);
}

/* ---- COP1X (MIPS IV) ---- */

/* MADD MSUB NMADD NMSUB: fd = +-(fs * ft +- fr), il prodotto arrotondato.
   NaN in ingresso: un sNaN da' Invalid e il NaN di default, altrimenti
   passa il primo qNaN tra fs, ft, fr. */
static void madd(R4k *c, uint32_t insn, int dbl)
{
    int fr = (insn >> 21) & 31, ft = (insn >> 16) & 31, fs = (insn >> 11) & 31, fd = (insn >> 6) & 31;
    int kind = (insn >> 3) & 7;                          /* 4 MADD, 5 MSUB, 6 NMADD, 7 NMSUB */
    uint32_t fl = 0;
    int rm = (int)(c->fcr31 & 3);
    if (rm) fesetround(host_rm[rm]);
    feclearexcept(FE_ALL_EXCEPT);
    uint64_t res;
    if (dbl) {
        uint64_t a = fpr_l(c, fs), b = fpr_l(c, ft), r = fpr_l(c, fr), n;
        if (nan2_d(a, b, &n, &fl) || nan1_d(r, &n, &fl)) res = n;
        else {
            double p = d_of(a) * d_of(b);
            double z = (kind & 1) ? p - d_of(r) : p + d_of(r);
            fl |= host_flags();
            res = fix_d(c, bits_d(z), &fl);
            if (kind >= 6 && !nan_d(res)) res ^= 1ull << 63;
        }
    } else {
        uint32_t a = fpr_w(c, fs), b = fpr_w(c, ft), r = fpr_w(c, fr), n;
        if (nan2_s(a, b, &n, &fl) || nan1_s(r, &n, &fl)) res = n;
        else {
            float p = f_of(a) * f_of(b);
            float z = (kind & 1) ? p - f_of(r) : p + f_of(r);
            fl |= host_flags();
            res = fix_s(c, bits_f(z), &fl);
            if (kind >= 6 && !nan_s((uint32_t)res)) res ^= 0x80000000u;
        }
    }
    if (rm) fesetround(FE_TONEAREST);
    if (!fpu_finish(c, fl)) return;
    if (dbl) set_fpr_l(c, fd, res); else set_fpr_w(c, fd, (uint32_t)res);
}

void r4k_cop1x(R4k *c, uint32_t insn)
{
    int fs = (insn >> 11) & 31, fd = (insn >> 6) & 31;
    uint64_t ea = c->r[(insn >> 21) & 31] + c->r[(insn >> 16) & 31];   /* base + indice */
    uint64_t v;
    switch (insn & 63) {
    case 0x00: if (ld32(c, ea, &v)) set_fpr_w(c, fd, (uint32_t)v); return;   /* LWXC1 */
    case 0x01: if (ld64(c, ea, &v)) set_fpr_l(c, fd, v); return;             /* LDXC1 */
    case 0x08: st32(c, ea, fpr_w(c, fs)); return;                            /* SWXC1 */
    case 0x09: st64(c, ea, fpr_l(c, fs)); return;                            /* SDXC1 */
    case 0x0F: return;                                                       /* PREFX */
    case 0x20: case 0x28: case 0x30: case 0x38: madd(c, insn, 0); return;
    case 0x21: case 0x29: case 0x31: case 0x39: madd(c, insn, 1); return;
    default: break;
    }
    r4k_exception(c, EXC_RI, 0);
}
