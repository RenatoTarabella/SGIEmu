/*
 * r4k.c - Interprete R4400: ciclo di esecuzione e istruzioni intere
 *
 * Delay slot: un salto mette branch_pending = 1 e branch_target; l'istruzione
 * dopo (il delay slot) gira con in_ds = 1 e alla fine il pc va a
 * branch_target. Un'eccezione nel delay slot usa in_ds per EPC = pc - 4 e
 * Cause.BD. I branch "likely" non presi saltano il delay slot (pc += 8).
 *
 * Le istruzioni a 32 bit usano i 32 bit bassi dei registri ed estendono il
 * segno del risultato, come QEMU (il comportamento con operandi non estesi
 * e' indefinito nell'architettura).
 */
#include "cpu/r4k_internal.h"
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#define RS   ((insn >> 21) & 31)
#define RT   ((insn >> 16) & 31)
#define RD   ((insn >> 11) & 31)
#define SA   ((insn >> 6) & 31)
#define SIMM ((uint64_t)(int64_t)(int16_t)insn)
#define UIMM ((uint64_t)(uint16_t)insn)

#define NEED64() do { if (!c->allow64) { r4k_exception(c, EXC_RI, 0); return; } } while (0)

static inline void branch(R4k *c, int cond, uint64_t target)
{
    c->branch_pending = 1;
    c->branch_target = cond ? target : c->pc + 8;
}

static inline void branch_likely(R4k *c, int cond, uint64_t target)
{
    if (cond) { c->branch_pending = 1; c->branch_target = target; }
    else      { c->pc += 8; c->pc_set = 1; }
}

static inline void mul64s(int64_t a, int64_t b, uint64_t *hi, uint64_t *lo)
{
#if defined(_MSC_VER)
    int64_t h;
    *lo = (uint64_t)_mul128(a, b, &h);
    *hi = (uint64_t)h;
#else
    __int128 p = (__int128)a * b;
    *lo = (uint64_t)p; *hi = (uint64_t)(p >> 64);
#endif
}

static inline void mul64u(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
#if defined(_MSC_VER)
    *lo = _umul128(a, b, hi);
#else
    unsigned __int128 p = (unsigned __int128)a * b;
    *lo = (uint64_t)p; *hi = (uint64_t)(p >> 64);
#endif
}

/* ---- load/store non allineate (big-endian) ---- */

/* Legge la parola allineata; se serve la via lenta prova prima il byte
   all'indirizzo vero, cosi' BadVAddr e' quello dell'istruzione. */
static int ld_unal(R4k *c, uint64_t va, int size, uint64_t *v)
{
    uint64_t al = va & ~(uint64_t)(size - 1), dummy;
    R4kFast *f = r4k_fast(c, al);
    if (f->tag_r == (al & ~(uint64_t)PM_PAGE_MASK)) {
        const uint8_t *h = FAST_HOST(f, al);
        *v = size == 4 ? be32(h) : be64(h);
        return 1;
    }
    if (!r4k_read_slow(c, va, 1, &dummy)) return 0;
    return r4k_read_slow(c, al, size, v);
}

static void lwl(R4k *c, int rt, uint64_t va)
{
    uint64_t w;
    if (!ld_unal(c, va, 4, &w)) return;
    int sh = 8 * (int)(va & 3);
    uint32_t keep = sh ? (1u << sh) - 1 : 0;
    c->r[rt] = SX32(((uint32_t)w << sh) | ((uint32_t)c->r[rt] & keep));
}

static void lwr(R4k *c, int rt, uint64_t va)
{
    uint64_t w;
    if (!ld_unal(c, va, 4, &w)) return;
    int sh = 8 * (3 - (int)(va & 3));
    uint32_t keep = ~(0xFFFFFFFFu >> sh);
    c->r[rt] = SX32(((uint32_t)w >> sh) | ((uint32_t)c->r[rt] & keep));
}

static void ldl(R4k *c, int rt, uint64_t va)
{
    uint64_t d;
    if (!ld_unal(c, va, 8, &d)) return;
    int sh = 8 * (int)(va & 7);
    uint64_t keep = sh ? ((uint64_t)1 << sh) - 1 : 0;
    c->r[rt] = (d << sh) | (c->r[rt] & keep);
}

static void ldr(R4k *c, int rt, uint64_t va)
{
    uint64_t d;
    if (!ld_unal(c, va, 8, &d)) return;
    int sh = 8 * (7 - (int)(va & 7));
    uint64_t keep = ~(~(uint64_t)0 >> sh);
    c->r[rt] = (d >> sh) | (c->r[rt] & keep);
}

/* SWL/SWR/SDL/SDR: byte per byte, tutti nella stessa pagina; il primo
   byte scritto e' all'indirizzo vero, quindi le eccezioni sono giuste. */
static void swl(R4k *c, uint64_t v, uint64_t va, int size)
{
    int b = (int)(va & (uint64_t)(size - 1));
    for (int i = 0; i < size - b; i++)
        if (!st8(c, va + (uint64_t)i, v >> (8 * (size - 1 - i)))) return;
}

static void swr(R4k *c, uint64_t v, uint64_t va, int size)
{
    int b = (int)(va & (uint64_t)(size - 1));
    for (int j = b; j >= 0; j--)
        if (!st8(c, va - (uint64_t)(b - j), v >> (8 * (b - j)))) return;
}

/* ---- esecuzione di un'istruzione ---- */

static void special(R4k *c, uint32_t insn)
{
    uint64_t *r = c->r;
    uint64_t a = r[RS], b = r[RT], res;
    switch (insn & 0x3F) {
    case 0x00: r[RD] = SX32((uint32_t)b << SA); return;                    /* SLL */
    case 0x02: r[RD] = SX32((uint32_t)b >> SA); return;                    /* SRL */
    case 0x03: r[RD] = SX32((int32_t)(uint32_t)b >> SA); return;           /* SRA */
    case 0x04: r[RD] = SX32((uint32_t)b << (a & 31)); return;              /* SLLV */
    case 0x06: r[RD] = SX32((uint32_t)b >> (a & 31)); return;              /* SRLV */
    case 0x07: r[RD] = SX32((int32_t)(uint32_t)b >> (a & 31)); return;     /* SRAV */
    case 0x08: branch(c, 1, a); return;                                    /* JR */
    case 0x09: branch(c, 1, a); r[RD] = c->pc + 8; return;                 /* JALR */
    case 0x0C: r4k_exception(c, EXC_SYS, 0); return;                       /* SYSCALL */
    case 0x0D: r4k_exception(c, EXC_BP, 0); return;                        /* BREAK */
    case 0x0F: return;                                                     /* SYNC */
    case 0x10: r[RD] = c->hi; return;                                      /* MFHI */
    case 0x11: c->hi = a; return;                                          /* MTHI */
    case 0x12: r[RD] = c->lo; return;                                      /* MFLO */
    case 0x13: c->lo = a; return;                                          /* MTLO */
    case 0x14: NEED64(); r[RD] = b << (a & 63); return;                    /* DSLLV */
    case 0x16: NEED64(); r[RD] = b >> (a & 63); return;                    /* DSRLV */
    case 0x17: NEED64(); r[RD] = (uint64_t)((int64_t)b >> (a & 63)); return; /* DSRAV */
    case 0x18: {                                                           /* MULT */
        int64_t p = (int64_t)(int32_t)a * (int32_t)b;
        c->lo = SX32((uint32_t)p); c->hi = SX32((uint32_t)((uint64_t)p >> 32));
        return;
    }
    case 0x19: {                                                           /* MULTU */
        uint64_t p = (uint64_t)(uint32_t)a * (uint32_t)b;
        c->lo = SX32((uint32_t)p); c->hi = SX32((uint32_t)(p >> 32));
        return;
    }
    case 0x1A: {                                                           /* DIV */
        int32_t n = (int32_t)a, d = (int32_t)b;
        if (d == 0 || (n == INT32_MIN && d == -1)) d = 1;
        c->lo = SX32((uint32_t)(n / d)); c->hi = SX32((uint32_t)(n % d));
        return;
    }
    case 0x1B: {                                                           /* DIVU */
        uint32_t n = (uint32_t)a, d = (uint32_t)b;
        if (d == 0) d = 1;
        c->lo = SX32(n / d); c->hi = SX32(n % d);
        return;
    }
    case 0x1C: NEED64(); mul64s((int64_t)a, (int64_t)b, &c->hi, &c->lo); return; /* DMULT */
    case 0x1D: NEED64(); mul64u(a, b, &c->hi, &c->lo); return;             /* DMULTU */
    case 0x1E: {                                                           /* DDIV */
        NEED64();
        int64_t n = (int64_t)a, d = (int64_t)b;
        if (d == 0 || (n == INT64_MIN && d == -1)) d = 1;
        c->lo = (uint64_t)(n / d); c->hi = (uint64_t)(n % d);
        return;
    }
    case 0x1F: {                                                           /* DDIVU */
        NEED64();
        uint64_t d = b ? b : 1;
        c->lo = a / d; c->hi = a % d;
        return;
    }
    case 0x20: {                                                           /* ADD */
        int64_t s = (int64_t)(int32_t)a + (int32_t)b;
        if (s != (int32_t)s) { r4k_exception(c, EXC_OV, 0); return; }
        r[RD] = (uint64_t)s;
        return;
    }
    case 0x21: r[RD] = SX32((uint32_t)a + (uint32_t)b); return;            /* ADDU */
    case 0x22: {                                                           /* SUB */
        int64_t s = (int64_t)(int32_t)a - (int32_t)b;
        if (s != (int32_t)s) { r4k_exception(c, EXC_OV, 0); return; }
        r[RD] = (uint64_t)s;
        return;
    }
    case 0x23: r[RD] = SX32((uint32_t)a - (uint32_t)b); return;            /* SUBU */
    case 0x24: r[RD] = a & b; return;
    case 0x25: r[RD] = a | b; return;
    case 0x26: r[RD] = a ^ b; return;
    case 0x27: r[RD] = ~(a | b); return;
    case 0x2A: r[RD] = (int64_t)a < (int64_t)b; return;                    /* SLT */
    case 0x2B: r[RD] = a < b; return;                                      /* SLTU */
    case 0x2C:                                                             /* DADD */
        NEED64();
        res = a + b;
        if (((a ^ res) & (b ^ res)) >> 63) { r4k_exception(c, EXC_OV, 0); return; }
        r[RD] = res;
        return;
    case 0x2D: NEED64(); r[RD] = a + b; return;                            /* DADDU */
    case 0x2E:                                                             /* DSUB */
        NEED64();
        res = a - b;
        if (((a ^ b) & (a ^ res)) >> 63) { r4k_exception(c, EXC_OV, 0); return; }
        r[RD] = res;
        return;
    case 0x2F: NEED64(); r[RD] = a - b; return;                            /* DSUBU */
    case 0x30: if ((int64_t)a >= (int64_t)b) r4k_exception(c, EXC_TR, 0); return; /* TGE */
    case 0x31: if (a >= b) r4k_exception(c, EXC_TR, 0); return;            /* TGEU */
    case 0x32: if ((int64_t)a < (int64_t)b) r4k_exception(c, EXC_TR, 0); return;  /* TLT */
    case 0x33: if (a < b) r4k_exception(c, EXC_TR, 0); return;             /* TLTU */
    case 0x34: if (a == b) r4k_exception(c, EXC_TR, 0); return;            /* TEQ */
    case 0x36: if (a != b) r4k_exception(c, EXC_TR, 0); return;            /* TNE */
    case 0x38: NEED64(); r[RD] = b << SA; return;                          /* DSLL */
    case 0x3A: NEED64(); r[RD] = b >> SA; return;                          /* DSRL */
    case 0x3B: NEED64(); r[RD] = (uint64_t)((int64_t)b >> SA); return;     /* DSRA */
    case 0x3C: NEED64(); r[RD] = b << (SA + 32); return;                   /* DSLL32 */
    case 0x3E: NEED64(); r[RD] = b >> (SA + 32); return;                   /* DSRL32 */
    case 0x3F: NEED64(); r[RD] = (uint64_t)((int64_t)b >> (SA + 32)); return; /* DSRA32 */
    default:   r4k_exception(c, EXC_RI, 0); return;
    }
}

static void regimm(R4k *c, uint32_t insn)
{
    int64_t a = (int64_t)c->r[RS];
    uint64_t t = c->pc + 4 + (SIMM << 2), imm = SIMM;
    switch (RT) {
    case 0x00: branch(c, a < 0, t); return;                                /* BLTZ */
    case 0x01: branch(c, a >= 0, t); return;                               /* BGEZ */
    case 0x02: branch_likely(c, a < 0, t); return;                         /* BLTZL */
    case 0x03: branch_likely(c, a >= 0, t); return;                        /* BGEZL */
    case 0x08: if (a >= (int64_t)imm) r4k_exception(c, EXC_TR, 0); return; /* TGEI */
    case 0x09: if ((uint64_t)a >= imm) r4k_exception(c, EXC_TR, 0); return;/* TGEIU */
    case 0x0A: if (a < (int64_t)imm) r4k_exception(c, EXC_TR, 0); return;  /* TLTI */
    case 0x0B: if ((uint64_t)a < imm) r4k_exception(c, EXC_TR, 0); return; /* TLTIU */
    case 0x0C: if ((uint64_t)a == imm) r4k_exception(c, EXC_TR, 0); return;/* TEQI */
    case 0x0E: if ((uint64_t)a != imm) r4k_exception(c, EXC_TR, 0); return;/* TNEI */
    case 0x10: c->r[31] = c->pc + 8; branch(c, a < 0, t); return;          /* BLTZAL */
    case 0x11: c->r[31] = c->pc + 8; branch(c, a >= 0, t); return;         /* BGEZAL */
    case 0x12: c->r[31] = c->pc + 8; branch_likely(c, a < 0, t); return;   /* BLTZALL */
    case 0x13: c->r[31] = c->pc + 8; branch_likely(c, a >= 0, t); return;  /* BGEZALL */
    default:   r4k_exception(c, EXC_RI, 0); return;
    }
}

static inline int cop1_usable(R4k *c)
{
    if (c->cp0[CP0_STATUS] & ST_CU1) return 1;
    r4k_exception(c, EXC_CPU, 1);
    return 0;
}

static inline void exec(R4k *c, uint32_t insn)
{
    uint64_t *r = c->r;
    uint64_t v;
    int rt = RT;
    uint64_t ea = r[RS] + SIMM;           /* per load e store */

    switch (insn >> 26) {
    case 0x00: special(c, insn); return;
    case 0x01: regimm(c, insn); return;
    case 0x02:                                                             /* J */
        branch(c, 1, ((c->pc + 4) & ~(uint64_t)0x0FFFFFFF) | ((insn & 0x03FFFFFF) << 2));
        return;
    case 0x03:                                                             /* JAL */
        r[31] = c->pc + 8;
        branch(c, 1, ((c->pc + 4) & ~(uint64_t)0x0FFFFFFF) | ((insn & 0x03FFFFFF) << 2));
        return;
    case 0x04: branch(c, r[RS] == r[rt], c->pc + 4 + (SIMM << 2)); return;             /* BEQ */
    case 0x05: branch(c, r[RS] != r[rt], c->pc + 4 + (SIMM << 2)); return;             /* BNE */
    case 0x06: branch(c, (int64_t)r[RS] <= 0, c->pc + 4 + (SIMM << 2)); return;        /* BLEZ */
    case 0x07: branch(c, (int64_t)r[RS] > 0, c->pc + 4 + (SIMM << 2)); return;         /* BGTZ */
    case 0x08: {                                                           /* ADDI */
        int64_t s = (int64_t)(int32_t)r[RS] + (int16_t)insn;
        if (s != (int32_t)s) { r4k_exception(c, EXC_OV, 0); return; }
        r[rt] = (uint64_t)s;
        return;
    }
    case 0x09: r[rt] = SX32((uint32_t)r[RS] + (uint32_t)SIMM); return;     /* ADDIU */
    case 0x0A: r[rt] = (int64_t)r[RS] < (int64_t)SIMM; return;             /* SLTI */
    case 0x0B: r[rt] = r[RS] < SIMM; return;                               /* SLTIU */
    case 0x0C: r[rt] = r[RS] & UIMM; return;                               /* ANDI */
    case 0x0D: r[rt] = r[RS] | UIMM; return;                               /* ORI */
    case 0x0E: r[rt] = r[RS] ^ UIMM; return;                               /* XORI */
    case 0x0F: r[rt] = SX32((uint32_t)UIMM << 16); return;                 /* LUI */
    case 0x10: r4k_cop0(c, insn); return;
    case 0x11: if (cop1_usable(c)) r4k_cop1(c, insn); return;
    case 0x12:                                                             /* COP2: assente */
        r4k_exception(c, (c->cp0[CP0_STATUS] & 0x40000000u) ? EXC_RI : EXC_CPU, 2);
        return;
    case 0x13:                                                             /* COP3 */
        r4k_exception(c, (c->cp0[CP0_STATUS] & 0x80000000u) ? EXC_RI : EXC_CPU, 3);
        return;
    case 0x14: branch_likely(c, r[RS] == r[rt], c->pc + 4 + (SIMM << 2)); return;      /* BEQL */
    case 0x15: branch_likely(c, r[RS] != r[rt], c->pc + 4 + (SIMM << 2)); return;      /* BNEL */
    case 0x16: branch_likely(c, (int64_t)r[RS] <= 0, c->pc + 4 + (SIMM << 2)); return; /* BLEZL */
    case 0x17: branch_likely(c, (int64_t)r[RS] > 0, c->pc + 4 + (SIMM << 2)); return;  /* BGTZL */
    case 0x18: {                                                           /* DADDI */
        NEED64();
        uint64_t a = r[RS], res = a + SIMM;
        if (((a ^ res) & (SIMM ^ res)) >> 63) { r4k_exception(c, EXC_OV, 0); return; }
        r[rt] = res;
        return;
    }
    case 0x19: NEED64(); r[rt] = r[RS] + SIMM; return;                     /* DADDIU */
    case 0x1A: NEED64(); ldl(c, rt, ea); return;
    case 0x1B: NEED64(); ldr(c, rt, ea); return;
    case 0x20: if (ld8(c, ea, &v))  r[rt] = (uint64_t)(int64_t)(int8_t)v; return;   /* LB */
    case 0x21: if (ld16(c, ea, &v)) r[rt] = (uint64_t)(int64_t)(int16_t)v; return; /* LH */
    case 0x22: lwl(c, rt, ea); return;
    case 0x23: if (ld32(c, ea, &v)) r[rt] = SX32(v); return;               /* LW */
    case 0x24: if (ld8(c, ea, &v))  r[rt] = v; return;                     /* LBU */
    case 0x25: if (ld16(c, ea, &v)) r[rt] = v; return;                     /* LHU */
    case 0x26: lwr(c, rt, ea); return;
    case 0x27: NEED64(); if (ld32(c, ea, &v)) r[rt] = v; return;           /* LWU */
    case 0x28: st8(c, ea, r[rt]); return;                                  /* SB */
    case 0x29: st16(c, ea, r[rt]); return;                                 /* SH */
    case 0x2A: swl(c, (uint32_t)r[rt], ea, 4); return;
    case 0x2B: st32(c, ea, r[rt]); return;                                 /* SW */
    case 0x2C: NEED64(); swl(c, r[rt], ea, 8); return;                     /* SDL */
    case 0x2D: NEED64(); swr(c, r[rt], ea, 8); return;                     /* SDR */
    case 0x2E: swr(c, (uint32_t)r[rt], ea, 4); return;
    case 0x2F: r4k_cache_op(c, insn); return;                              /* CACHE */
    case 0x30:                                                             /* LL */
    case 0x34:                                                             /* LLD */
        if (insn >> 26 == 0x34) { NEED64(); if (!ld64(c, ea, &v)) return; r[rt] = v; }
        else { if (!ld32(c, ea, &v)) return; r[rt] = SX32(v); }
        c->ll_bit = 1;
        {
            uint64_t pa;
            if (r4k_translate_debug(c, ea, 0, &pa)) c->cp0[CP0_LLADDR] = pa >> 4;
        }
        return;
    case 0x31:                                                             /* LWC1 */
        if (cop1_usable(c) && ld32(c, ea, &v)) set_fpr_w(c, rt, (uint32_t)v);
        return;
    case 0x35:                                                             /* LDC1 */
        if (cop1_usable(c) && ld64(c, ea, &v)) set_fpr_l(c, rt, v);
        return;
    case 0x37: NEED64(); if (ld64(c, ea, &v)) r[rt] = v; return;           /* LD */
    case 0x38:                                                             /* SC */
        if (!c->ll_bit) { r[rt] = 0; return; }
        if (st32(c, ea, r[rt])) r[rt] = 1;
        return;
    case 0x3C:                                                             /* SCD */
        NEED64();
        if (!c->ll_bit) { r[rt] = 0; return; }
        if (st64(c, ea, r[rt])) r[rt] = 1;
        return;
    case 0x39: if (cop1_usable(c)) st32(c, ea, fpr_w(c, rt)); return;      /* SWC1 */
    case 0x3D: if (cop1_usable(c)) st64(c, ea, fpr_l(c, rt)); return;      /* SDC1 */
    case 0x3F: NEED64(); st64(c, ea, r[rt]); return;                       /* SD */
    case 0x32: case 0x36: case 0x3A: case 0x3E:                            /* LWC2 LDC2 SWC2 SDC2 */
        r4k_exception(c, (c->cp0[CP0_STATUS] & 0x40000000u) ? EXC_RI : EXC_CPU, 2);
        return;
    default:
        r4k_exception(c, EXC_RI, 0);
        return;
    }
}

/* ---- ciclo di esecuzione ---- */

static inline void step_one(R4k *c)
{
    uint64_t pc = c->pc;
    uint64_t target = c->branch_target;
    int in_ds = c->branch_pending;
    uint32_t insn;

    c->in_ds = in_ds;
    c->branch_pending = 0;
    c->pc_set = 0;
    c->icount++;

    R4kFast *f = r4k_fast(c, pc);
    if (f->tag_x == (pc & ~(uint64_t)0xFFC)) insn = be32(FAST_HOST(f, pc));
    else if (!r4k_fetch_slow(c, pc, &insn)) return;

    exec(c, insn);
    c->r[0] = 0;
    if (!c->pc_set) c->pc = in_ds ? target : pc + 4;
}

/* timer e interrupt, tra un'istruzione e l'altra */
static void service(R4k *c)
{
    if (c->icount >= c->timer_event) r4k_timer_fire(c);
    if (r4k_irq_ready(c)) {
        c->in_ds = c->branch_pending;
        r4k_exception(c, EXC_INT, 0);
    }
}

uint64_t r4k_run(R4k *c, uint64_t n)
{
    uint64_t start = c->icount;
    c->run_end = start + n;
    c->stop = 0;
    while (!c->stop && c->icount < c->run_end) {
        service(c);
        if (c->stop) break;
        c->limit = c->timer_event < c->run_end ? c->timer_event : c->run_end;
        while (c->icount < c->limit) step_one(c);
    }
    return c->icount - start;
}

void r4k_step(R4k *c)
{
    service(c);
    step_one(c);
}

void r4k_stop(R4k *c)
{
    c->stop = 1;
    c->limit = 0;
}

uint32_t r4k_fpr_w(const R4k *c, int n) { return fpr_w(c, n); }
uint64_t r4k_fpr_l(const R4k *c, int n) { return fpr_l(c, n); }
void r4k_set_fpr_w(R4k *c, int n, uint32_t v) { set_fpr_w(c, n, v); }
void r4k_set_fpr_l(R4k *c, int n, uint64_t v) { set_fpr_l(c, n, v); }
