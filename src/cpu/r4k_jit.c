/*
 * r4k_jit.c - JIT R4400 -> x86-64 (Windows x64 e System V, solo host
 * little-endian a 64 bit)
 *
 * Blocchi: da un pc fino al primo salto (con il suo delay slot), a
 * un'istruzione che cambia modo o mappa (MTC0, TLBWI, ERET...), alla fine
 * della pagina o a max_block istruzioni. Un blocco sta tutto in una pagina
 * ed e' indicizzato per puntatore host (cioe' per indirizzo fisico) piu'
 * indirizzo virtuale e modo: il codice contiene il pc virtuale (link dei
 * JAL, EPC), e i cambi di TLB/ASID non lo invalidano.
 *
 * Registri MIPS in memoria (rbx = R4k *). Le istruzioni comuni sono
 * tradotte (ALU, shift, salti, load/store con la MMU veloce inline); le
 * altre, e la via lenta delle load/store, chiamano jit_helper che esegue
 * l'istruzione con l'interprete. L'helper riceve pc, delay slot e quante
 * istruzioni del blocco sono state eseguite, per EPC/BD e icount esatti.
 *
 * Esattezza: il blocco parte solo se tutte le sue istruzioni stanno prima
 * di c->limit, e un helper fa uscire il blocco se c'e' stata un'eccezione,
 * un r4k_kick (interrupt, Compare, stop) o una scrittura su codice
 * tradotto. Quindi il JIT esegue le stesse istruzioni dell'interprete,
 * con gli stessi icount: un boot con e senza JIT deve dare lo stesso
 * risultato.
 *
 * Codice riscritto: le pagine con blocchi hanno PM_CODE (anche sugli
 * specchi fisici), cosi' le scritture passano da pm_write -> code_write,
 * che butta i blocchi della pagina e toglie PM_CODE. Un cambio della mappa
 * fisica (pm->map_gen) svuota tutto.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include "cpu/r4k_internal.h"
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define JIT_BUF_SIZE    (64u << 20)
#define JIT_MAX_BLOCKS  (1 << 18)
#define HASH_BITS       18
#define PAGE_BITS       14
#define DEFAULT_BLOCK   64
#define MAX_BLOCK       256
#define MAX_STUBS       (2 * MAX_BLOCK + 8)
#define HOT_SIZE        4096
#define JIT_MAX_EXITS   (4 * JIT_MAX_BLOCKS)
#define JIT_MAX_RETS    (1 << 18)
#define JC_BITS         16
#define JC_SIZE         (1u << JC_BITS)

struct JitBlock;

/* Uscita verso un pc costante che si puo' concatenare: il blocco salta
   direttamente al successivo se ci sta prima di c->limit. */
typedef struct JitExit {
    uint64_t  target;           /* pc virtuale di destinazione */
    struct JitBlock *from, *to; /* to = NULL: non collegata */
    uint8_t  *imm;              /* add rax, ninsn del blocco di arrivo */
    uint8_t  *jmp;              /* rel32 del salto al blocco di arrivo */
    uint8_t  *unlinked;         /* dove salta quando non e' collegata */
    struct JitExit *next_in;    /* altre uscite verso lo stesso blocco */
} JitExit;

typedef struct JitBlock {
    uint64_t  va;
    uintptr_t host;             /* prima istruzione */
    uintptr_t hpage;            /* inizio della pagina host */
    const uint8_t *code;        /* NULL = non traducibile: interprete */
    struct JitBlock *next;      /* catena della tabella hash */
    struct JitBlock *pnext;     /* catena per pagina */
    JitExit  *in;               /* uscite collegate a questo blocco */
    uint32_t  flags, ninsn;
} JitBlock;

/* jump cache: blocco per puntatore host, per i salti a registro */
typedef struct JitJc {
    uintptr_t host;
    uint64_t  va;
    const uint8_t *code;
    uint32_t  ninsn, flags;
} JitJc;

/* ultima destinazione di un jr/jalr in kseg0/kseg1 (kernel): valida se
   gen == c->jit_gen (cambia a ogni invalidazione o svuotamento) */
typedef struct JitRet {
    uint64_t  va;
    const uint8_t *code;
    uint32_t  ninsn, gen;
} JitRet;

typedef struct R4kJit R4kJit;
struct R4kJit {
    uint8_t  *buf, *code0, *p, *end;
    void    (*enter)(R4k *c, const uint8_t *code);
    const uint8_t *exit;
    const uint8_t *dyn;         /* cerca il blocco di c->pc e ci salta, o exit */
    const uint8_t *dyn_ret;     /* come dyn, e riempie il JitRet in r10 */
    JitRet   *rets;
    int       nrets;
    JitBlock *blocks;
    JitExit  *exits;
    int       nblocks, nexits, max_block;
    uint32_t  map_gen;
    JitBlock *hash[1 << HASH_BITS];
    JitBlock *page[1 << PAGE_BITS];
    JitJc     jc[JC_SIZE];
    uint32_t  mxcsr[5];
    int       no_regs, no_fpu;  /* debug: R4K_JIT_NOREGS, R4K_JIT_NOFPU */         /* pulito per RM 0..3, [4] = quello del C */
    /* statistiche */
    uint64_t  compiled, invalidated, flushes, runs, helpers, code_bytes, links;
    uint64_t  compile_ticks;
    uint64_t  f_nofetch, f_chain, f_jchit, f_nofit, f_nocode;   /* perche' si e' tornati in C */
    uint64_t  fb_op[64], fb_special[64], fb_cop1[32];
    struct { uint64_t pc, n; uint32_t insn; } hot[HOT_SIZE];   /* pc con piu' chiamate */
};

/* ================================================================ */
/* emettitore x86-64                                                 */
/* ================================================================ */

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { A_ADD = 0, A_OR = 1, A_AND = 4, A_SUB = 5, A_XOR = 6, A_CMP = 7 };
enum { S_ROL = 0, S_SHL = 4, S_SHR = 5, S_SAR = 7 };
enum { CC_O = 0, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A,
       CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G, CC_ALWAYS = 16 };

typedef struct X64 { uint8_t *p, *end; int full; } X64;

static void b1(X64 *e, unsigned v) { if (e->p < e->end) *e->p++ = (uint8_t)v; else e->full = 1; }
static void b4(X64 *e, uint32_t v) { for (int i = 0; i < 4; i++) b1(e, (v >> (8 * i)) & 0xFF); }
static void b8(X64 *e, uint64_t v) { b4(e, (uint32_t)v); b4(e, (uint32_t)(v >> 32)); }

static int fits8(int64_t v)  { return v >= -128 && v <= 127; }
static int fits32(uint64_t v) { return (int64_t)v == (int32_t)v; }

/* REX; index < 0 = nessun indice */
static void rex(X64 *e, int w, int reg, int index, int base, int force)
{
    unsigned r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0)
               | ((index >= 0 && (index & 8)) ? 2 : 0) | ((base & 8) ? 1 : 0);
    if (r != 0x40 || force) b1(e, r);
}

static void opc(X64 *e, unsigned op)
{
    if (op > 0xFF) b1(e, op >> 8);
    b1(e, op & 0xFF);
}

static void modrm_m(X64 *e, int reg, int base, int index, int32_t disp)
{
    int mod = (disp == 0 && (base & 7) != RBP) ? 0 : fits8(disp) ? 1 : 2;
    if (index < 0 && (base & 7) != RSP) {
        b1(e, (unsigned)(mod << 6 | (reg & 7) << 3 | (base & 7)));
    } else {
        b1(e, (unsigned)(mod << 6 | (reg & 7) << 3 | 4));
        b1(e, (unsigned)((index < 0 ? 4 : (index & 7)) << 3 | (base & 7)));
    }
    if (mod == 1) b1(e, (uint8_t)disp);
    else if (mod == 2) b4(e, (uint32_t)disp);
}

/* op reg, [base + index + disp] */
static void x_m(X64 *e, int p66, int w, unsigned op, int reg, int base, int index, int32_t disp)
{
    if (p66) b1(e, 0x66);
    rex(e, w, reg, index, base, 0);
    opc(e, op);
    modrm_m(e, reg, base, index, disp);
}

/* op reg, rm (registri) */
static void x_r(X64 *e, int p66, int w, unsigned op, int reg, int rm)
{
    if (p66) b1(e, 0x66);
    rex(e, w, reg, -1, rm, 0);
    opc(e, op);
    b1(e, 0xC0u | (unsigned)(reg & 7) << 3 | (unsigned)(rm & 7));
}

#define CTX RBX
#if defined(_WIN32)
enum { ARG0 = RCX, ARG1 = RDX, ARG2 = R8, ARG3 = R9 };
#else
enum { ARG0 = RDI, ARG1 = RSI, ARG2 = RDX, ARG3 = RCX };
#endif
#define OFF(f)  ((int32_t)offsetof(R4k, f))
#define REG(n)  (OFF(r) + 8 * (int32_t)(n))

static void ld_q(X64 *e, int r, int32_t off) { x_m(e, 0, 1, 0x8B, r, CTX, -1, off); }
static void ld_d(X64 *e, int r, int32_t off) { x_m(e, 0, 0, 0x8B, r, CTX, -1, off); }
static void st_q(X64 *e, int r, int32_t off) { x_m(e, 0, 1, 0x89, r, CTX, -1, off); }
static void mov_rr(X64 *e, int dst, int src) { x_r(e, 0, 1, 0x89, src, dst); }
static void movsxd(X64 *e, int dst, int src) { x_r(e, 0, 1, 0x63, dst, src); }
static void movsxd_m(X64 *e, int r, int32_t off) { x_m(e, 0, 1, 0x63, r, CTX, -1, off); }

/* op r, [ctx + off] */
static void alu_m(X64 *e, int w, int op, int r, int32_t off) { x_m(e, 0, w, (unsigned)(op * 8 + 3), r, CTX, -1, off); }

static void alu_ri(X64 *e, int w, int op, int r, int32_t imm)
{
    if (fits8(imm)) { x_r(e, 0, w, 0x83, op, r); b1(e, (uint8_t)imm); }
    else            { x_r(e, 0, w, 0x81, op, r); b4(e, (uint32_t)imm); }
}

/* op qword/dword [ctx + off], imm */
static void alu_mi(X64 *e, int w, int op, int32_t off, int32_t imm)
{
    if (fits8(imm)) { x_m(e, 0, w, 0x83, op, CTX, -1, off); b1(e, (uint8_t)imm); }
    else            { x_m(e, 0, w, 0x81, op, CTX, -1, off); b4(e, (uint32_t)imm); }
}

static void shift_ri(X64 *e, int w, int op, int r, int n) { x_r(e, 0, w, 0xC1, op, r); b1(e, (unsigned)n); }
static void shift_cl(X64 *e, int w, int op, int r)        { x_r(e, 0, w, 0xD3, op, r); }

static void bswap(X64 *e, int w, int r)
{
    rex(e, w, 0, -1, r, 0);
    b1(e, 0x0F);
    b1(e, 0xC8u + (unsigned)(r & 7));
}

static void setcc(X64 *e, int cc, int r)
{
    rex(e, 0, 0, -1, r, r >= 4);
    b1(e, 0x0F); b1(e, 0x90u + (unsigned)cc); b1(e, 0xC0u | (unsigned)(r & 7));
}

static void mov_ri(X64 *e, int r, uint64_t v)
{
    if (v <= 0xFFFFFFFFu) { rex(e, 0, 0, -1, r, 0); b1(e, 0xB8u + (unsigned)(r & 7)); b4(e, (uint32_t)v); }
    else if (fits32(v))   { x_r(e, 0, 1, 0xC7, 0, r); b4(e, (uint32_t)v); }
    else                  { rex(e, 1, 0, -1, r, 0); b1(e, 0xB8u + (unsigned)(r & 7)); b8(e, v); }
}

/* mov qword [ctx + off], v (tmp se v non sta in 32 bit con segno) */
static void st_qi(X64 *e, int32_t off, uint64_t v, int tmp)
{
    if (fits32(v)) { x_m(e, 0, 1, 0xC7, 0, CTX, -1, off); b4(e, (uint32_t)v); }
    else           { mov_ri(e, tmp, v); st_q(e, tmp, off); }
}

static void st_di(X64 *e, int32_t off, uint32_t v) { x_m(e, 0, 0, 0xC7, 0, CTX, -1, off); b4(e, v); }

static uint8_t *jcc(X64 *e, int cc)
{
    b1(e, 0x0F); b1(e, 0x80u + (unsigned)cc); b4(e, 0);
    return e->p - 4;
}

static uint8_t *jmp32(X64 *e)
{
    b1(e, 0xE9); b4(e, 0);
    return e->p - 4;
}

static void patch(X64 *e, uint8_t *at, const uint8_t *target)
{
    if (e->full) return;
    int32_t rel = (int32_t)(target - (at + 4));
    memcpy(at, &rel, 4);
}

static void jmp_to(X64 *e, const uint8_t *target) { uint8_t *p = jmp32(e); patch(e, p, target); }
static void jcc_to(X64 *e, int cc, const uint8_t *target) { uint8_t *p = jcc(e, cc); patch(e, p, target); }

static void call_abs(X64 *e, const void *fn)
{
    mov_ri(e, RAX, (uint64_t)(uintptr_t)fn);
    b1(e, 0xFF); b1(e, 0xD0);                           /* call rax */
}

/* ================================================================ */
/* helper chiamato dal codice tradotto                               */
/* ================================================================ */

/* Esegue insn con l'interprete. kd = istruzioni del blocco fino a questa
   compresa | delay slot << 16. Ritorna 1 se il blocco deve uscire (pc gia'
   scritto, icount comprende questa istruzione). */
static int jit_helper(R4k *c, uint32_t insn, uint64_t pc, uint32_t kd)
{
    uint32_t k = kd & 0xFFFF;
    R4kJit *j = c->jit;
    j->helpers++;
    switch (insn >> 26) {
    case 0x00: j->fb_special[insn & 63]++; break;
    case 0x11: j->fb_cop1[(insn >> 21) & 31]++; break;
    default:   j->fb_op[insn >> 26]++; break;
    }
    {
        uint32_t h = (uint32_t)(pc >> 2) & (HOT_SIZE - 1);
        if (j->hot[h].pc != pc) { if (j->hot[h].n > 16) j->hot[h].n -= 16; else { j->hot[h].pc = pc; j->hot[h].n = 0; j->hot[h].insn = insn; } }
        else j->hot[h].n++;
    }
    c->pc = pc;
    c->in_ds = (int)(kd >> 16);
    c->icount += k;
    c->pc_set = 0;
    r4k_exec(c, insn);
    c->r[0] = 0;
    if (c->pm->map_gen != j->map_gen) c->jit_exit = 1;  /* dyn torna a r4k_jit_find */
    if (c->pc_set) return 1;
    /* kick, codice riscritto, mappa fisica cambiata (i blocchi concatenati
       non ripassano da r4k_jit_find) */
    if (c->limit == 0 || c->jit_exit) {
        c->pc = c->in_ds ? c->branch_target : pc + 4;
        return 1;
    }
    c->icount -= k;
    return 0;
}

/* ================================================================ */
/* compilatore                                                       */
/* ================================================================ */

typedef struct Stub {
    int       kind;             /* 0 = helper, 1 = uscita a pc fisso */
    uint32_t  dirty;            /* registri host da riscrivere prima */
    uint8_t  *from;             /* rel32 del salto verso lo stub */
    uint8_t  *resume;           /* helper: dove tornare */
    uint32_t  insn, kd;
    uint64_t  pc;
} Stub;

typedef struct Comp {
    X64      e;
    R4k     *c;
    R4kJit  *j;
    JitBlock *b;                /* il blocco che si sta traducendo */
    int      allow64, kernel;
    int      fr, cu1, rm;       /* Status.FR, Status.CU1, FCSR.RM (nella chiave) */
    int      unmapped;          /* blocco in kseg0/kseg1 in modo kernel */
    int      nochain;           /* il delay slot puo' aver cambiato la mappa */
    int32_t  fast_off;          /* c->fast[mode] */
    Stub     stubs[MAX_STUBS];
    int      nstub;
    Stub    *fp_stub2;          /* secondo stub di fp_check_result */
    /* registri MIPS nei registri host (callee-saved, salvati dal thunk) */
    int      counting;          /* prima passata: conta gli usi, niente di definitivo */
    uint16_t uses[32], writes[32];
    int8_t   hreg[32];          /* registro host o -1 */
    uint32_t cached, dirty;     /* bit per registro MIPS */
} Comp;

#if defined(_WIN32)
#define NCACHE 7
static const int cache_host[NCACHE] = { RBP, RSI, RDI, R12, R13, R14, R15 };
#else                                   /* System V: rsi e rdi non si conservano */
#define NCACHE 5
static const int cache_host[NCACHE] = { RBP, R12, R13, R14, R15 };
#endif

#define RS(i)  ((int)((i) >> 21) & 31)
#define RT(i)  ((int)((i) >> 16) & 31)
#define RD(i)  ((int)((i) >> 11) & 31)
#define SA(i)  ((int)((i) >> 6) & 31)

static Stub *add_stub(Comp *k, int kind, uint8_t *from, uint32_t insn, uint64_t pc, uint32_t kd)
{
    if (k->nstub >= MAX_STUBS) { k->e.full = 1; return &k->stubs[0]; }
    Stub *s = &k->stubs[k->nstub++];
    s->kind = kind; s->from = from; s->resume = NULL;
    s->insn = insn; s->pc = pc; s->kd = kd;
    s->dirty = k->dirty;
    return s;
}

/* ---- accesso ai registri MIPS: nel registro host se il blocco lo tiene li' ---- */

static int hreg_of(Comp *k, int n) { return n ? k->hreg[n] : -1; }

static void gpr_ld(Comp *k, int w, int r, int n)            /* mov r, gpr (32 bit: zero-extend) */
{
    int h = hreg_of(k, n);
    if (k->counting) k->uses[n]++;
    if (h >= 0) x_r(&k->e, 0, w, 0x89, h, r);
    else x_m(&k->e, 0, w, 0x8B, r, CTX, -1, REG(n));
}

static void gpr_ldsx(Comp *k, int r, int n)                 /* movsxd r, gpr32 */
{
    int h = hreg_of(k, n);
    if (k->counting) k->uses[n]++;
    if (h >= 0) x_r(&k->e, 0, 1, 0x63, r, h);
    else x_m(&k->e, 0, 1, 0x63, r, CTX, -1, REG(n));
}

static void gpr_alu(Comp *k, int w, int op, int r, int n)   /* op r, gpr */
{
    int h = hreg_of(k, n);
    if (k->counting) k->uses[n]++;
    if (h >= 0) x_r(&k->e, 0, w, (unsigned)(op * 8 + 3), r, h);
    else x_m(&k->e, 0, w, (unsigned)(op * 8 + 3), r, CTX, -1, REG(n));
}

static void gpr_st(Comp *k, int r, int n)                   /* gpr = r (n != 0) */
{
    int h = hreg_of(k, n);
    if (k->counting) { k->uses[n]++; k->writes[n]++; }
    if (h >= 0) { x_r(&k->e, 0, 1, 0x89, r, h); k->dirty |= 1u << n; }
    else st_q(&k->e, r, REG(n));
}

static void gpr_sti(Comp *k, int n, uint64_t v, int tmp)    /* gpr = costante (non tocca i flag) */
{
    int h = hreg_of(k, n);
    if (k->counting) { k->uses[n]++; k->writes[n]++; }
    if (h >= 0) { mov_ri(&k->e, h, v); k->dirty |= 1u << n; }
    else st_qi(&k->e, REG(n), v, tmp);
}

/* riscrive in memoria i registri host modificati (non tocca i flag) */
static void gpr_writeback(Comp *k, uint32_t mask)
{
    mask &= k->cached;
    for (int n = 1; n < 32; n++)
        if (mask & (1u << n)) st_q(&k->e, k->hreg[n], REG(n));
}

/* dopo l'interprete: puo' aver scritto solo rt o rd */
static void gpr_reload(Comp *k, uint32_t insn)
{
    int v[2] = { RT(insn), RD(insn) };
    for (int i = 0; i < 2; i++) {
        int h = hreg_of(k, v[i]);
        if (h >= 0 && (i == 0 || v[1] != v[0])) ld_q(&k->e, h, REG(v[i]));
    }
}

static void emit_call(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd)
{
    X64 *e = &k->e;
    mov_rr(e, ARG0, CTX);
    mov_ri(e, ARG1, insn);
    mov_ri(e, ARG2, pc);
    mov_ri(e, ARG3, kd);
    call_abs(e, (const void *)jit_helper);
    x_r(e, 0, 0, 0x85, RAX, RAX);                       /* test eax, eax */
    jcc_to(e, CC_NE, k->j->dyn);
}

/* chiamata all'interprete per questa istruzione, esce se lui lo chiede */
static void emit_helper_call(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd)
{
    gpr_writeback(k, k->dirty);
    k->dirty = 0;
    emit_call(k, insn, pc, kd);
    gpr_reload(k, insn);
}

/* uscita dal blocco con pc costante; n = istruzioni eseguite */
static void emit_exit_pc(Comp *k, uint64_t pc, int n)
{
    X64 *e = &k->e;
    if (n) alu_mi(e, 1, A_ADD, OFF(icount), n);
    st_qi(e, OFF(pc), pc, RAX);
    jmp_to(e, k->j->exit);
}

static int kseg01(uint64_t va) { return va - 0xFFFFFFFF80000000ull < 0x40000000u; }

static uint32_t jc_index(uintptr_t host) { return (uint32_t)(host >> 2) & (JC_SIZE - 1); }

/* rax = pc di destinazione (icount gia' aggiornato): prosegue in dyn */
static void emit_lookup(Comp *k)
{
    st_q(&k->e, RAX, OFF(pc));
    jmp_to(&k->e, k->j->dyn);
}

/* rax = destinazione di un jr/jalr in un blocco kernel in kseg0/kseg1:
   prima l'ultima destinazione di questo salto, poi dyn_ret */
static void emit_ret_cache(Comp *k)
{
    X64 *e = &k->e;
    R4kJit *j = k->j;
    JitRet *r = &j->rets[j->nrets++];
    memset(r, 0, sizeof *r);
    r->gen = ~0u;
    uint8_t *miss[4];
    mov_ri(e, R10, (uint64_t)(uintptr_t)r);
    x_m(e, 0, 1, 0x3B, RAX, R10, -1, (int32_t)offsetof(JitRet, va));
    miss[0] = jcc(e, CC_NE);
    x_m(e, 0, 0, 0x8B, RDX, R10, -1, (int32_t)offsetof(JitRet, gen));
    alu_m(e, 0, A_CMP, RDX, OFF(jit_gen));
    miss[1] = jcc(e, CC_NE);
    ld_q(e, RDX, OFF(icount));
    x_m(e, 0, 0, 0x8B, RCX, R10, -1, (int32_t)offsetof(JitRet, ninsn));
    x_r(e, 0, 1, 0x03, RDX, RCX);
    alu_m(e, 1, A_CMP, RDX, OFF(limit));
    miss[2] = jcc(e, CC_A);
    x_m(e, 0, 0, 0xFF, 4, R10, -1, (int32_t)offsetof(JitRet, code));   /* jmp [r10 + code] */
    for (int i = 0; i < 3; i++) patch(e, miss[i], e->p);
    (void)miss[3];
    st_q(e, RAX, OFF(pc));
    jmp_to(e, j->dyn_ret);
}

/* Si puo' saltare direttamente al blocco di pc se la sua traduzione non
   puo' essere cambiata: stessa pagina virtuale del blocco (entrato con la
   mappatura attuale) o kseg0/kseg1 in modo kernel. Le uscite dopo MTC0,
   TLBW*, ERET... non si concatenano (nochain o emit_exit_pc). */
static int can_chain(Comp *k, uint64_t pc)
{
    if (k->nochain) return 0;
    if (((pc ^ k->b->va) & ~(uint64_t)PM_PAGE_MASK) == 0) return 1;
    return k->kernel && kseg01(pc) && kseg01(k->b->va);
}

/* uscita verso pc costante, concatenabile: se il blocco di arrivo e'
   collegato e ci sta prima di c->limit ci si salta direttamente */
static void emit_exit_chain(Comp *k, uint64_t pc, int n)
{
    R4kJit *j = k->j;
    X64 *e = &k->e;
    if (k->counting || !can_chain(k, pc) || j->nexits >= JIT_MAX_EXITS) {
        if (n) alu_mi(e, 1, A_ADD, OFF(icount), n);
        mov_ri(e, RAX, pc);
        emit_lookup(k);
        return;
    }
    JitExit *x = &j->exits[j->nexits++];
    memset(x, 0, sizeof *x);
    x->target = pc;
    x->from = k->b;
    if (n) alu_mi(e, 1, A_ADD, OFF(icount), n);
    ld_q(e, RAX, OFF(icount));
    b1(e, 0x48); b1(e, 0x05);                           /* add rax, imm32 */
    x->imm = e->p;
    b4(e, 0);
    alu_m(e, 1, A_CMP, RAX, OFF(limit));
    uint8_t *ja = jcc(e, CC_A);
    x->jmp = jmp32(e);
    x->unlinked = e->p;
    patch(e, ja, e->p);
    patch(e, x->jmp, e->p);
    st_qi(e, OFF(pc), pc, RAX);
    mov_ri(e, RAX, (uint64_t)(uintptr_t)x);
    st_q(e, RAX, OFF(jit_last));
    jmp_to(e, j->exit);
}

static void link_exit(R4kJit *j, JitExit *x, JitBlock *b)
{
    int32_t n = (int32_t)b->ninsn, rel = (int32_t)(b->code - (x->jmp + 4));
    memcpy(x->imm, &n, 4);
    memcpy(x->jmp, &rel, 4);
    x->to = b;
    x->next_in = b->in;
    b->in = x;
    j->links++;
}

static void unlink_all(JitBlock *b)
{
    for (JitExit *x = b->in, *nx; x; x = nx) {
        nx = x->next_in;
        int32_t rel = (int32_t)(x->unlinked - (x->jmp + 4));
        memcpy(x->jmp, &rel, 4);
        x->to = NULL;
        x->next_in = NULL;
    }
    b->in = NULL;
}

/* rax = r[rs] + simm */
static void emit_ea(Comp *k, int rs, int32_t simm)
{
    gpr_ld(k, 1, RAX, rs);
    if (simm) alu_ri(&k->e, 1, A_ADD, RAX, simm);
}

/* confronto con il tag della MMU veloce: salta allo stub se non c'e';
   poi rax = puntatore host. Usa rcx, rdx. */
static Stub *emit_fast(Comp *k, int size, int store, uint32_t insn, uint64_t pc, uint32_t kd)
{
    X64 *e = &k->e;
    int32_t tag = k->fast_off + (store ? (int32_t)offsetof(R4kFast, tag_w) : (int32_t)offsetof(R4kFast, tag_r));
    x_r(e, 0, 0, 0x89, RAX, RCX);                       /* mov ecx, eax */
    shift_ri(e, 0, S_SHR, RCX, PM_PAGE_SHIFT - 5);      /* ((va >> 12) & 1023) * 32 */
    alu_ri(e, 0, A_AND, RCX, (R4K_FAST_SIZE - 1) << 5);
    mov_rr(e, RDX, RAX);
    alu_ri(e, 1, A_AND, RDX, (int32_t)~(0xFFFu & ~(uint32_t)(size - 1)));
    x_m(e, 0, 1, 0x3B, RDX, CTX, RCX, tag);             /* cmp rdx, tag */
    Stub *s = add_stub(k, 0, jcc(e, CC_NE), insn, pc, kd);
    x_m(e, 0, 1, 0x03, RAX, CTX, RCX, k->fast_off + (int32_t)offsetof(R4kFast, addend));
    return s;
}

static void emit_load(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd, int size, int sign)
{
    X64 *e = &k->e;
    int rt = RT(insn);
    emit_ea(k, RS(insn), (int16_t)insn);
    Stub *s = emit_fast(k, size, 0, insn, pc, kd);
    switch (size) {
    case 1:
        if (sign) x_m(e, 0, 1, 0x0FBE, RAX, RAX, -1, 0);    /* movsx rax, byte [rax] */
        else      x_m(e, 0, 0, 0x0FB6, RAX, RAX, -1, 0);    /* movzx eax, byte [rax] */
        break;
    case 2:
        x_m(e, 0, 0, 0x0FB7, RAX, RAX, -1, 0);              /* movzx eax, word [rax] */
        bswap(e, 0, RAX);
        if (sign) { shift_ri(e, 0, S_SAR, RAX, 16); movsxd(e, RAX, RAX); }
        else shift_ri(e, 0, S_SHR, RAX, 16);
        break;
    case 4:
        x_m(e, 0, 0, 0x8B, RAX, RAX, -1, 0);                /* mov eax, [rax] */
        bswap(e, 0, RAX);
        if (sign) movsxd(e, RAX, RAX);
        break;
    default:
        x_m(e, 0, 1, 0x8B, RAX, RAX, -1, 0);
        bswap(e, 1, RAX);
        break;
    }
    if (rt) gpr_st(k, RAX, rt);
    s->resume = e->p;
}

static void emit_store(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd, int size)
{
    X64 *e = &k->e;
    int rt = RT(insn);
    emit_ea(k, RS(insn), (int16_t)insn);
    Stub *s = emit_fast(k, size, 1, insn, pc, kd);
    switch (size) {
    case 1:
        gpr_ld(k, 0, RCX, rt);
        x_m(e, 0, 0, 0x88, RCX, RAX, -1, 0);                /* mov [rax], cl */
        break;
    case 2:
        gpr_ld(k, 0, RCX, rt);
        x_r(e, 1, 0, 0xC1, S_ROL, RCX); b1(e, 8);           /* rol cx, 8 */
        x_m(e, 1, 0, 0x89, RCX, RAX, -1, 0);                /* mov [rax], cx */
        break;
    case 4:
        gpr_ld(k, 0, RCX, rt);
        bswap(e, 0, RCX);
        x_m(e, 0, 0, 0x89, RCX, RAX, -1, 0);
        break;
    default:
        gpr_ld(k, 1, RCX, rt);
        bswap(e, 1, RCX);
        x_m(e, 0, 1, 0x89, RCX, RAX, -1, 0);
        break;
    }
    s->resume = e->p;
}

static int is_branch(uint32_t insn)
{
    switch (insn >> 26) {
    case 0x00: return (insn & 63) == 0x08 || (insn & 63) == 0x09;
    case 0x01: { int rt = RT(insn); return rt <= 3 || (rt >= 0x10 && rt <= 0x13); }
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
    case 0x14: case 0x15: case 0x16: case 0x17:
        return 1;
    case 0x11: return RS(insn) == 8;
    default:   return 0;
    }
}

enum { CONT = 0, END = 1 };

/* LWL LWR LDL LDR: la parola allineata con la MMU veloce, poi l'unione
   con il registro; la via lenta (e le eccezioni) all'interprete */
static void emit_load_lr(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd, int size, int left)
{
    X64 *e = &k->e;
    int rt = RT(insn), w = size == 8;
    emit_ea(k, RS(insn), (int16_t)insn);
    x_r(e, 0, 0, 0x89, RAX, R9);                        /* mov r9d, eax: byte nella parola */
    alu_ri(e, 1, A_AND, RAX, -size);
    Stub *s = emit_fast(k, size, 0, insn, pc, kd);
    x_m(e, 0, w, 0x8B, RAX, RAX, -1, 0);
    bswap(e, w, RAX);
    if (rt) {
        x_r(e, 0, 0, 0x89, R9, RCX);                    /* mov ecx, r9d */
        alu_ri(e, 0, A_AND, RCX, size - 1);
        if (!left) alu_ri(e, 0, A_XOR, RCX, size - 1);
        shift_ri(e, 0, S_SHL, RCX, 3);                  /* cl = sh */
        if (left) {                                     /* (w << sh) | (rt & ((1 << sh) - 1)) */
            shift_cl(e, w, S_SHL, RAX);
            mov_ri(e, R8, 1);
            shift_cl(e, w, S_SHL, R8);
            alu_ri(e, w, A_SUB, R8, 1);
        } else {                                        /* (w >> sh) | (rt & ~(~0 >> sh)) */
            shift_cl(e, w, S_SHR, RAX);
            if (w) x_r(e, 0, 1, 0xC7, 0, R8), b4(e, 0xFFFFFFFFu);   /* mov r8, -1 */
            else mov_ri(e, R8, 0xFFFFFFFFu);
            shift_cl(e, w, S_SHR, R8);
            x_r(e, 0, w, 0xF7, 2, R8);                  /* not r8 */
        }
        gpr_ld(k, w, RDX, rt);
        x_r(e, 0, w, 0x23, RDX, R8);                    /* and edx, r8d */
        x_r(e, 0, w, 0x0B, RAX, RDX);                   /* or eax, edx */
        if (!w) movsxd(e, RAX, RAX);
        gpr_st(k, RAX, rt);
    }
    s->resume = e->p;
}

/* SC: senza ll_bit rt = 0, altrimenti la store e rt = 1 */
static void emit_sc(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd)
{
    X64 *e = &k->e;
    int rt = RT(insn);
    alu_mi(e, 0, A_CMP, OFF(ll_bit), 0);
    uint8_t *has = jcc(e, CC_NE);
    if (rt) gpr_sti(k, rt, 0, RAX);
    uint8_t *done = jmp32(e);
    patch(e, has, e->p);
    emit_ea(k, RS(insn), (int16_t)insn);
    Stub *s = emit_fast(k, 4, 1, insn, pc, kd);
    gpr_ld(k, 0, RCX, rt);
    bswap(e, 0, RCX);
    x_m(e, 0, 0, 0x89, RCX, RAX, -1, 0);
    if (rt) gpr_sti(k, rt, 1, RAX);
    s->resume = e->p;
    patch(e, done, e->p);
}

/* ---- FPU ---- */

/* registri FPU come li vede il programma (FR nella chiave del blocco) */
static int32_t fw_off(Comp *k, int n) { return OFF(fpr) + (k->fr ? 8 * n : 8 * (n & ~1) + 4 * (n & 1)); }
static int32_t fl_off(Comp *k, int n) { return OFF(fpr) + 8 * (k->fr ? n : n & ~1); }

/* istruzione SSE: prefisso obbligatorio, 0F op, xmm/reg con [ctx + off] */
static void sse_m(X64 *e, unsigned prefix, int w, unsigned op, int reg, int32_t off)
{
    if (prefix) b1(e, prefix);
    rex(e, w, reg, -1, CTX, 0);
    b1(e, 0x0F); b1(e, op);
    modrm_m(e, reg, CTX, -1, off);
}

static void sse_r(X64 *e, unsigned prefix, int w, unsigned op, int reg, int rm)
{
    if (prefix) b1(e, prefix);
    rex(e, w, reg, -1, rm, 0);
    b1(e, 0x0F); b1(e, op);
    b1(e, 0xC0u | (unsigned)(reg & 7) << 3 | (unsigned)(rm & 7));
}

static void ldmxcsr_tab(Comp *k, int i)
{
    X64 *e = &k->e;
    mov_ri(e, R8, (uint64_t)(uintptr_t)&k->j->mxcsr[i]);
    rex(e, 0, 0, -1, R8, 0);
    b1(e, 0x0F); b1(e, 0xAE); modrm_m(e, 2, R8, -1, 0);       /* ldmxcsr [r8] */
}

/* dopo l'operazione: flag in jit_mxcsr, MXCSR di nuovo quello del C */
static void fp_after(Comp *k)
{
    X64 *e = &k->e;
    b1(e, 0x0F); b1(e, 0xAE); modrm_m(e, 3, CTX, -1, OFF(jit_mxcsr));   /* stmxcsr */
    if (k->rm) ldmxcsr_tab(k, 4);
}

/* salta allo stub se l'operazione ha dato qualcosa oltre a inexact */
static void fp_check_flags(Comp *k, Stub **st, uint32_t insn, uint64_t pc, uint32_t kd)
{
    X64 *e = &k->e;
    x_m(e, 0, 0, 0xF7, 0, CTX, -1, OFF(jit_mxcsr)); b4(e, 0x1D);    /* IE ZE OE UE */
    *st = add_stub(k, 0, jcc(e, CC_NE), insn, pc, kd);
}

/* risultato in xmm0: NaN, infinito o denormale vanno all'interprete */
static void fp_check_result(Comp *k, int dbl, Stub *st)
{
    X64 *e = &k->e;
    if (dbl) {
        sse_r(e, 0x66, 1, 0x7E, 0, RAX);                          /* movq rax, xmm0 */
        mov_rr(e, RDX, RAX);
        shift_ri(e, 1, S_SHR, RDX, 52);
        alu_ri(e, 0, A_AND, RDX, 0x7FF);
    } else {
        sse_r(e, 0x66, 0, 0x7E, 0, RAX);                          /* movd eax, xmm0 */
        x_r(e, 0, 0, 0x89, RAX, RDX);
        shift_ri(e, 0, S_SHR, RDX, 23);
        alu_ri(e, 0, A_AND, RDX, 0xFF);
    }
    x_m(e, 0, 0, 0x8D, RCX, RDX, -1, -1);                         /* lea ecx, [rdx - 1] */
    alu_ri(e, 0, A_CMP, RCX, dbl ? 0x7FE : 0xFE);
    uint8_t *ok = jcc(e, CC_B);                                   /* esponente normale */
    mov_rr(e, RCX, RAX);
    x_r(e, 0, dbl, 0x03, RCX, RCX);                               /* add rcx, rcx: via il segno */
    Stub *s2 = add_stub(k, 0, jcc(e, CC_NE), st->insn, st->pc, st->kd);
    patch(e, ok, e->p);
    st->resume = s2->resume = NULL;                               /* li fissa il chiamante */
    k->fp_stub2 = s2;
}

/* FCSR: Cause = inexact o niente, flag sticky; inexact abilitato va
   all'interprete. inexact = 0: l'operazione non puo' esserlo. */
static void fp_finish(Comp *k, int inexact, Stub **st2, uint32_t insn, uint64_t pc, uint32_t kd)
{
    X64 *e = &k->e;
    ld_d(e, RDX, OFF(fcr31));
    alu_ri(e, 0, A_AND, RDX, (int32_t)~FCSR_CAUSE);
    *st2 = NULL;
    if (inexact) {
        x_m(e, 0, 0, 0xF7, 0, CTX, -1, OFF(jit_mxcsr)); b4(e, 0x20);   /* PE */
        uint8_t *no = jcc(e, CC_E);
        x_r(e, 0, 0, 0xF7, 0, RDX); b4(e, 0x80);                        /* enable I */
        *st2 = add_stub(k, 0, jcc(e, CC_NE), insn, pc, kd);
        alu_ri(e, 0, A_OR, RDX, 0x1004);                                /* Cause I, flag I */
        patch(e, no, e->p);
    }
    x_m(e, 0, 0, 0x89, RDX, CTX, -1, OFF(fcr31));
}

static void set_resume(Stub **v, int n, uint8_t *p)
{
    for (int i = 0; i < n; i++) if (v[i]) v[i]->resume = p;
}

/* S e D: ADD SUB MUL DIV SQRT MOV ABS NEG C.cond CVT; 0 = non tradotta */
static int emit_fpu_arith(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd)
{
    X64 *e = &k->e;
    int fmt = RS(insn), fn = (int)(insn & 63);
    int ft = RT(insn), fs = RD(insn), fd = SA(insn);
    int dbl = fmt == 17;
    unsigned pfx = dbl ? 0xF2 : 0xF3;
    Stub *st[4] = { 0 };

    if (fmt == 16 || fmt == 17) {
        if (fn >= 5 && fn <= 7) {                                 /* ABS MOV NEG */
            if (dbl) {
                ld_q(e, RAX, fl_off(k, fs));
                if (fn != 6) { x_r(e, 0, 1, 0x0FBA, fn == 5 ? 6 : 7, RAX); b1(e, 63); }  /* btr / btc */
                st_q(e, RAX, fl_off(k, fd));
            } else {
                ld_d(e, RAX, fw_off(k, fs));
                if (fn == 5) alu_ri(e, 0, A_AND, RAX, 0x7FFFFFFF);
                if (fn == 7) alu_ri(e, 0, A_XOR, RAX, (int32_t)0x80000000u);
                x_m(e, 0, 0, 0x89, RAX, CTX, -1, fw_off(k, fd));
            }
            return 1;
        }
        if (fn <= 4) {                                            /* ADD SUB MUL DIV SQRT */
            static const unsigned sop[5] = { 0x58, 0x5C, 0x59, 0x5E, 0x51 };
            ldmxcsr_tab(k, k->rm);
            sse_m(e, pfx, 0, 0x10, 0, dbl ? fl_off(k, fs) : fw_off(k, fs));   /* movs xmm0, fs */
            sse_m(e, pfx, 0, sop[fn], 0, fn == 4 ? (dbl ? fl_off(k, fs) : fw_off(k, fs))
                                                 : (dbl ? fl_off(k, ft) : fw_off(k, ft)));
            fp_after(k);
            fp_check_flags(k, &st[0], insn, pc, kd);
            fp_check_result(k, dbl, st[0]);
            st[1] = k->fp_stub2;
            fp_finish(k, 1, &st[2], insn, pc, kd);
            sse_m(e, pfx, 0, 0x11, 0, dbl ? fl_off(k, fd) : fw_off(k, fd));   /* movs fd, xmm0 */
            set_resume(st, 3, e->p);
            return 1;
        }
        if (fn >= 48) {                                           /* C.cond senza NaN */
            if (k->c->mips4 && ((insn >> 8) & 7)) return 0;      /* cc 1..7: interprete */
            int cc = fn & 15;
            sse_m(e, pfx, 0, 0x10, 0, dbl ? fl_off(k, fs) : fw_off(k, fs));
            sse_m(e, dbl ? 0x66 : 0, 0, 0x2E, 0, dbl ? fl_off(k, ft) : fw_off(k, ft));  /* ucomis */
            st[0] = add_stub(k, 0, jcc(e, CC_P), insn, pc, kd);  /* non ordinati */
            int ccx = (cc & 6) == 6 ? CC_BE : (cc & 2) ? CC_E : (cc & 4) ? CC_B : -1;
            if (ccx >= 0) { setcc(e, ccx, RAX); x_r(e, 0, 0, 0x0FB6, RAX, RAX); shift_ri(e, 0, S_SHL, RAX, 23); }
            ld_d(e, RDX, OFF(fcr31));
            alu_ri(e, 0, A_AND, RDX, (int32_t)~(FCSR_CAUSE | FCSR_C));
            if (ccx >= 0) x_r(e, 0, 0, 0x0B, RDX, RAX);            /* or edx, eax */
            x_m(e, 0, 0, 0x89, RDX, CTX, -1, OFF(fcr31));
            set_resume(st, 1, e->p);
            return 1;
        }
        if (fn == 33 && !dbl) {                                   /* CVT.D.S: esatta */
            ld_d(e, RAX, fw_off(k, fs));
            alu_ri(e, 0, A_AND, RAX, 0x7F800000);
            alu_ri(e, 0, A_CMP, RAX, 0x7F800000);
            st[0] = add_stub(k, 0, jcc(e, CC_E), insn, pc, kd);  /* NaN e infiniti */
            sse_m(e, 0xF3, 0, 0x5A, 0, fw_off(k, fs));            /* cvtss2sd */
            fp_finish(k, 0, &st[1], insn, pc, kd);
            sse_m(e, 0xF2, 0, 0x11, 0, fl_off(k, fd));
            set_resume(st, 1, e->p);
            return 1;
        }
        if (fn == 32 && dbl) {                                    /* CVT.S.D */
            ldmxcsr_tab(k, k->rm);
            sse_m(e, 0xF2, 0, 0x5A, 0, fl_off(k, fs));            /* cvtsd2ss */
            fp_after(k);
            fp_check_flags(k, &st[0], insn, pc, kd);
            fp_check_result(k, 0, st[0]);
            st[1] = k->fp_stub2;
            fp_finish(k, 1, &st[2], insn, pc, kd);
            sse_m(e, 0xF3, 0, 0x11, 0, fw_off(k, fd));
            set_resume(st, 3, e->p);
            return 1;
        }
        if (fn == 36 || fn == 13) {                               /* CVT.W, TRUNC.W */
            ldmxcsr_tab(k, k->rm);
            sse_m(e, pfx, 0, fn == 13 ? 0x2C : 0x2D, RAX, dbl ? fl_off(k, fs) : fw_off(k, fs));
            fp_after(k);
            fp_check_flags(k, &st[0], insn, pc, kd);             /* NaN, fuori scala */
            fp_finish(k, 1, &st[1], insn, pc, kd);
            x_m(e, 0, 0, 0x89, RAX, CTX, -1, fw_off(k, fd));
            set_resume(st, 2, e->p);
            return 1;
        }
        return 0;
    }
    if (fmt == 20 && (fn == 32 || fn == 33)) {                    /* CVT.S.W CVT.D.W */
        int todbl = fn == 33;
        if (!todbl) ldmxcsr_tab(k, k->rm);
        sse_m(e, todbl ? 0xF2 : 0xF3, 0, 0x2A, 0, fw_off(k, fs));  /* cvtsi2s xmm0, dword */
        if (todbl) {
            fp_finish(k, 0, &st[0], insn, pc, kd);
            sse_m(e, 0xF2, 0, 0x11, 0, fl_off(k, fd));
        } else {
            fp_after(k);
            fp_finish(k, 1, &st[0], insn, pc, kd);
            sse_m(e, 0xF3, 0, 0x11, 0, fw_off(k, fd));
        }
        set_resume(st, 1, e->p);
        return 1;
    }
    return 0;
}

/* COP1 (CU1 = 1 nella chiave); 0 = all'interprete, 2 = chiude il blocco */
static int emit_cop1(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd)
{
    X64 *e = &k->e;
    int rt = RT(insn), fs = RD(insn);
    switch (RS(insn)) {
    case 0:                                                       /* MFC1 */
        if (rt) { movsxd_m(e, RAX, fw_off(k, fs)); gpr_st(k, RAX, rt); }
        return 1;
    case 1:                                                       /* DMFC1 */
        if (!k->allow64) return 0;
        if (rt) { ld_q(e, RAX, fl_off(k, fs)); gpr_st(k, RAX, rt); }
        return 1;
    case 2:                                                       /* CFC1 */
        if (!rt) return 1;
        if (fs == 0 || fs == 31) movsxd_m(e, RAX, fs ? OFF(fcr31) : OFF(fcr0));
        else x_r(e, 0, 0, 0x33, RAX, RAX);                        /* xor eax, eax */
        gpr_st(k, RAX, rt);
        return 1;
    case 4:                                                       /* MTC1 */
        gpr_ld(k, 0, RAX, rt);
        x_m(e, 0, 0, 0x89, RAX, CTX, -1, fw_off(k, fs));
        return 1;
    case 5:                                                       /* DMTC1 */
        if (!k->allow64) return 0;
        gpr_ld(k, 1, RAX, rt);
        st_q(e, RAX, fl_off(k, fs));
        return 1;
    case 6:                                                       /* CTC1: RM e' nella chiave */
        return 2;
    default:
        return emit_fpu_arith(k, insn, pc, kd);
    }
}

/* LWC1 LDC1 SWC1 SDC1 */
static void emit_fmem(Comp *k, uint32_t insn, uint64_t pc, uint32_t kd, int size, int store)
{
    X64 *e = &k->e;
    int ft = RT(insn);
    int32_t off = size == 4 ? fw_off(k, ft) : fl_off(k, ft);
    emit_ea(k, RS(insn), (int16_t)insn);
    Stub *s = emit_fast(k, size, store, insn, pc, kd);
    if (store) {
        x_m(e, 0, size == 8, 0x8B, RCX, CTX, -1, off);
        bswap(e, size == 8, RCX);
        x_m(e, 0, size == 8, 0x89, RCX, RAX, -1, 0);
    } else {
        x_m(e, 0, size == 8, 0x8B, RAX, RAX, -1, 0);
        bswap(e, size == 8, RAX);
        x_m(e, 0, size == 8, 0x89, RAX, CTX, -1, off);
    }
    s->resume = e->p;
}

/* un'istruzione che non e' un salto; END = il blocco finisce dopo */
static int emit_insn(Comp *k, uint32_t insn, uint64_t pc, int idx, int ds)
{
    X64 *e = &k->e;
    int rs = RS(insn), rt = RT(insn), rd = RD(insn), sa = SA(insn);
    int32_t simm = (int16_t)insn;
    uint32_t uimm = (uint16_t)insn;
    uint32_t kd = (uint32_t)(idx + 1) | (uint32_t)ds << 16;
    Stub *s;

    switch (insn >> 26) {
    case 0x00:
        switch (insn & 63) {
        case 0x00: case 0x02: case 0x03: {                          /* SLL SRL SRA */
            static const int sop[4] = { S_SHL, 0, S_SHR, S_SAR };
            if (!rd) return CONT;
            gpr_ld(k, 0, RAX, rt);
            if (sa) shift_ri(e, 0, sop[insn & 3], RAX, sa);
            movsxd(e, RAX, RAX);
            gpr_st(k, RAX, rd);
            return CONT;
        }
        case 0x04: case 0x06: case 0x07: {                          /* SLLV SRLV SRAV */
            static const int sop[4] = { S_SHL, 0, S_SHR, S_SAR };
            if (!rd) return CONT;
            gpr_ld(k, 0, RCX, rs);
            gpr_ld(k, 0, RAX, rt);
            shift_cl(e, 0, sop[insn & 3], RAX);
            movsxd(e, RAX, RAX);
            gpr_st(k, RAX, rd);
            return CONT;
        }
        case 0x14: case 0x16: case 0x17: {                          /* DSLLV DSRLV DSRAV */
            static const int sop[4] = { S_SHL, 0, S_SHR, S_SAR };
            if (!k->allow64) break;
            if (!rd) return CONT;
            gpr_ld(k, 0, RCX, rs);
            gpr_ld(k, 1, RAX, rt);
            shift_cl(e, 1, sop[insn & 3], RAX);
            gpr_st(k, RAX, rd);
            return CONT;
        }
        case 0x38: case 0x3A: case 0x3B:                            /* DSLL DSRL DSRA */
        case 0x3C: case 0x3E: case 0x3F: {                          /* ...32 */
            static const int sop[4] = { S_SHL, 0, S_SHR, S_SAR };
            if (!k->allow64) break;
            if (!rd) return CONT;
            int n = sa + ((insn & 4) ? 32 : 0);
            gpr_ld(k, 1, RAX, rt);
            if (n) shift_ri(e, 1, sop[insn & 3], RAX, n);
            gpr_st(k, RAX, rd);
            return CONT;
        }
        case 0x0C: case 0x0D:                                       /* SYSCALL BREAK */
            emit_helper_call(k, insn, pc, kd);
            return END;
        case 0x0F: return CONT;                                     /* SYNC */
        case 0x10: case 0x12:                                       /* MFHI MFLO */
            if (!rd) return CONT;
            ld_q(e, RAX, (insn & 2) ? OFF(lo) : OFF(hi));
            gpr_st(k, RAX, rd);
            return CONT;
        case 0x11: case 0x13:                                       /* MTHI MTLO */
            gpr_ld(k, 1, RAX, rs);
            st_q(e, RAX, (insn & 2) ? OFF(lo) : OFF(hi));
            return CONT;
        case 0x18:                                                  /* MULT */
            gpr_ldsx(k, RAX, rs);
            gpr_ldsx(k, RCX, rt);
            x_r(e, 0, 1, 0x0FAF, RAX, RCX);                         /* imul rax, rcx */
            mov_rr(e, RDX, RAX);
            shift_ri(e, 1, S_SAR, RDX, 32);
            movsxd(e, RAX, RAX);
            st_q(e, RAX, OFF(lo));
            st_q(e, RDX, OFF(hi));
            return CONT;
        case 0x19:                                                  /* MULTU */
            gpr_ld(k, 0, RAX, rs);
            gpr_ld(k, 0, RCX, rt);
            x_r(e, 0, 1, 0x0FAF, RAX, RCX);
            mov_rr(e, RDX, RAX);
            shift_ri(e, 1, S_SHR, RDX, 32);
            movsxd(e, RDX, RDX);
            movsxd(e, RAX, RAX);
            st_q(e, RAX, OFF(lo));
            st_q(e, RDX, OFF(hi));
            return CONT;
        case 0x1A: case 0x1B: {                                     /* DIV DIVU */
            /* divisore 0 (e INT_MIN / -1) come l'interprete: diviso 1 */
            gpr_ld(k, 0, RAX, rs);
            gpr_ld(k, 0, RCX, rt);
            x_r(e, 0, 0, 0x85, RCX, RCX);                           /* test ecx, ecx */
            uint8_t *fix = jcc(e, CC_E), *ok1 = NULL, *ok2 = NULL;
            if (!(insn & 1)) {
                alu_ri(e, 0, A_CMP, RCX, -1);
                ok1 = jcc(e, CC_NE);
                alu_ri(e, 0, A_CMP, RAX, (int32_t)0x80000000u);
                ok2 = jcc(e, CC_NE);
            } else {
                ok1 = jmp32(e);
            }
            patch(e, fix, e->p);
            mov_ri(e, RCX, 1);
            patch(e, ok1, e->p);
            if (ok2) patch(e, ok2, e->p);
            if (insn & 1) { x_r(e, 0, 0, 0x33, RDX, RDX); x_r(e, 0, 0, 0xF7, 6, RCX); }   /* div ecx */
            else          { b1(e, 0x99); x_r(e, 0, 0, 0xF7, 7, RCX); }                   /* cdq; idiv ecx */
            movsxd(e, RAX, RAX);
            movsxd(e, RDX, RDX);
            st_q(e, RAX, OFF(lo));
            st_q(e, RDX, OFF(hi));
            return CONT;
        }
        case 0x20: case 0x22:                                       /* ADD SUB */
            gpr_ld(k, 0, RAX, rs);
            gpr_alu(k, 0, (insn & 2) ? A_SUB : A_ADD, RAX, rt);
            s = add_stub(k, 0, jcc(e, CC_O), insn, pc, kd);
            if (rd) { movsxd(e, RAX, RAX); gpr_st(k, RAX, rd); }
            s->resume = e->p;
            return CONT;
        case 0x21: case 0x23:                                       /* ADDU SUBU */
            if (!rd) return CONT;
            gpr_ld(k, 0, RAX, rs);
            gpr_alu(k, 0, (insn & 2) ? A_SUB : A_ADD, RAX, rt);
            movsxd(e, RAX, RAX);
            gpr_st(k, RAX, rd);
            return CONT;
        case 0x24: case 0x25: case 0x26: case 0x27: {               /* AND OR XOR NOR */
            static const int aop[4] = { A_AND, A_OR, A_XOR, A_OR };
            if (!rd) return CONT;
            gpr_ld(k, 1, RAX, rs);
            gpr_alu(k, 1, aop[insn & 3], RAX, rt);
            if ((insn & 63) == 0x27) x_r(e, 0, 1, 0xF7, 2, RAX);    /* not rax */
            gpr_st(k, RAX, rd);
            return CONT;
        }
        case 0x2A: case 0x2B:                                       /* SLT SLTU */
            if (!rd) return CONT;
            gpr_ld(k, 1, RAX, rs);
            gpr_alu(k, 1, A_CMP, RAX, rt);
            setcc(e, (insn & 1) ? CC_B : CC_L, RAX);
            x_r(e, 0, 0, 0x0FB6, RAX, RAX);                         /* movzx eax, al */
            gpr_st(k, RAX, rd);
            return CONT;
        case 0x2D: case 0x2F:                                       /* DADDU DSUBU */
            if (!k->allow64) break;
            if (!rd) return CONT;
            gpr_ld(k, 1, RAX, rs);
            gpr_alu(k, 1, (insn & 2) ? A_SUB : A_ADD, RAX, rt);
            gpr_st(k, RAX, rd);
            return CONT;
        default: break;
        }
        break;
    case 0x08:                                                      /* ADDI */
        gpr_ld(k, 0, RAX, rs);
        alu_ri(e, 0, A_ADD, RAX, simm);
        s = add_stub(k, 0, jcc(e, CC_O), insn, pc, kd);
        if (rt) { movsxd(e, RAX, RAX); gpr_st(k, RAX, rt); }
        s->resume = e->p;
        return CONT;
    case 0x09:                                                      /* ADDIU */
        if (!rt) return CONT;
        gpr_ld(k, 0, RAX, rs);
        if (simm) alu_ri(e, 0, A_ADD, RAX, simm);
        movsxd(e, RAX, RAX);
        gpr_st(k, RAX, rt);
        return CONT;
    case 0x0A: case 0x0B:                                           /* SLTI SLTIU */
        if (!rt) return CONT;
        gpr_ld(k, 1, RAX, rs);
        alu_ri(e, 1, A_CMP, RAX, simm);
        setcc(e, (insn >> 26) & 1 ? CC_B : CC_L, RAX);
        x_r(e, 0, 0, 0x0FB6, RAX, RAX);
        gpr_st(k, RAX, rt);
        return CONT;
    case 0x0C:                                                      /* ANDI */
        if (!rt) return CONT;
        gpr_ld(k, 0, RAX, rs);
        alu_ri(e, 0, A_AND, RAX, (int32_t)uimm);
        gpr_st(k, RAX, rt);
        return CONT;
    case 0x0D: case 0x0E:                                           /* ORI XORI */
        if (!rt) return CONT;
        gpr_ld(k, 1, RAX, rs);
        if (uimm) alu_ri(e, 1, (insn >> 26) == 0x0D ? A_OR : A_XOR, RAX, (int32_t)uimm);
        gpr_st(k, RAX, rt);
        return CONT;
    case 0x0F:                                                      /* LUI */
        if (rt) gpr_sti(k, rt, SX32(uimm << 16), RAX);
        return CONT;
    case 0x10:                                                      /* COP0 */
        /* MFC0/DMFC0 in modo kernel dei registri che sono solo memoria */
        if (k->kernel && (rs == 0 || rs == 1) && rd != CP0_COUNT && rd != CP0_RANDOM &&
            (insn & 0x7FF) == 0) {
            if (!rt) return CONT;
            if (rs == 0) movsxd_m(e, RAX, OFF(cp0) + 8 * rd);
            else ld_q(e, RAX, OFF(cp0) + 8 * rd);
            gpr_st(k, RAX, rt);
            return CONT;
        }
        /* MTC0/DMTC0 di EntryLo0/1 e PageMask: solo memoria (TLB refill) */
        /* (sull'R10000 EntryLo ha il PFN fino al bit 33 e MTC0 estende il
           segno: all'interprete) */
        if (k->kernel && (rs == 4 || rs == 5) && (insn & 0x7FF) == 0 && k->c->model != R4K_R10000 &&
            (rd == CP0_ENTRYLO0 || rd == CP0_ENTRYLO1 || rd == CP0_PAGEMASK)) {
            gpr_ld(k, 0, RAX, rt);
            alu_ri(e, 0, A_AND, RAX, rd == CP0_PAGEMASK ? 0x01FFE000 : 0x3FFFFFFF);
            st_q(e, RAX, OFF(cp0) + 8 * rd);
            return CONT;
        }
        emit_helper_call(k, insn, pc, kd);
        /* Un blocco in kseg0/kseg1 non dipende dal TLB, e le scritture che
           cambiano modo o interrupt (Status, Cause, Compare, ERET) fanno
           comunque uscire l'helper. Altrove TLBW*, EntryHi... chiudono. */
        return (rs == 0 || rs == 1 || k->unmapped) ? CONT : END;
    case 0x19:                                                      /* DADDIU */
        if (!k->allow64) break;
        if (!rt) return CONT;
        gpr_ld(k, 1, RAX, rs);
        if (simm) alu_ri(e, 1, A_ADD, RAX, simm);
        gpr_st(k, RAX, rt);
        return CONT;
    case 0x20: emit_load(k, insn, pc, kd, 1, 1); return CONT;       /* LB */
    case 0x21: emit_load(k, insn, pc, kd, 2, 1); return CONT;       /* LH */
    case 0x23: emit_load(k, insn, pc, kd, 4, 1); return CONT;       /* LW */
    case 0x24: emit_load(k, insn, pc, kd, 1, 0); return CONT;       /* LBU */
    case 0x25: emit_load(k, insn, pc, kd, 2, 0); return CONT;       /* LHU */
    case 0x27: if (!k->allow64) break; emit_load(k, insn, pc, kd, 4, 0); return CONT; /* LWU */
    case 0x37: if (!k->allow64) break; emit_load(k, insn, pc, kd, 8, 0); return CONT; /* LD */
    case 0x28: emit_store(k, insn, pc, kd, 1); return CONT;         /* SB */
    case 0x29: emit_store(k, insn, pc, kd, 2); return CONT;         /* SH */
    case 0x2B: emit_store(k, insn, pc, kd, 4); return CONT;         /* SW */
    case 0x3F: if (!k->allow64) break; emit_store(k, insn, pc, kd, 8); return CONT;   /* SD */
    case 0x22: emit_load_lr(k, insn, pc, kd, 4, 1); return CONT;    /* LWL */
    case 0x26: emit_load_lr(k, insn, pc, kd, 4, 0); return CONT;    /* LWR */
    case 0x1A: if (!k->allow64) break; emit_load_lr(k, insn, pc, kd, 8, 1); return CONT; /* LDL */
    case 0x1B: if (!k->allow64) break; emit_load_lr(k, insn, pc, kd, 8, 0); return CONT; /* LDR */
    case 0x38: emit_sc(k, insn, pc, kd); return CONT;               /* SC */
    case 0x11:                                                      /* COP1 */
        if (!k->cu1 || k->j->no_fpu) break;
        {
            int r = emit_cop1(k, insn, pc, kd);
            if (r == 1) return CONT;
            if (r == 2) { emit_helper_call(k, insn, pc, kd); return END; }
        }
        break;
    case 0x31: if (!k->cu1) break; emit_fmem(k, insn, pc, kd, 4, 0); return CONT;   /* LWC1 */
    case 0x35: if (!k->cu1) break; emit_fmem(k, insn, pc, kd, 8, 0); return CONT;   /* LDC1 */
    case 0x39: if (!k->cu1) break; emit_fmem(k, insn, pc, kd, 4, 1); return CONT;   /* SWC1 */
    case 0x3D: if (!k->cu1) break; emit_fmem(k, insn, pc, kd, 8, 1); return CONT;   /* SDC1 */
    default: break;
    }
    emit_helper_call(k, insn, pc, kd);
    return CONT;
}

/* salto: calcola c->branch_target (e il link) prima del delay slot.
   Ritorna BR_*; *tgt = destinazione se costante. */
enum { BR_REG, BR_ALWAYS, BR_COND, BR_LIKELY };

static int emit_branch(Comp *k, uint32_t insn, uint64_t pc, int idx, uint64_t *tgt)
{
    X64 *e = &k->e;
    int op = (int)(insn >> 26), rs = RS(insn), rt = RT(insn);
    uint64_t target = pc + 4 + ((uint64_t)(int64_t)(int16_t)insn << 2);
    int cc = CC_ALWAYS, likely = 0, link = 0;

    switch (op) {
    case 0x00:                                                      /* JR JALR */
        gpr_ld(k, 1, RAX, rs);
        st_q(e, RAX, OFF(branch_target));
        if ((insn & 63) == 0x09 && RD(insn)) gpr_sti(k, RD(insn), pc + 8, RAX);
        return BR_REG;
    case 0x02: case 0x03:                                           /* J JAL */
        target = ((pc + 4) & ~(uint64_t)0x0FFFFFFF) | ((uint64_t)(insn & 0x03FFFFFF) << 2);
        link = op == 3;
        break;
    case 0x04: case 0x05: case 0x14: case 0x15:                     /* BEQ BNE BEQL BNEL */
        gpr_ld(k, 1, RAX, rs);
        gpr_alu(k, 1, A_CMP, RAX, rt);
        cc = (op & 1) ? CC_NE : CC_E;
        likely = op >= 0x14;
        break;
    case 0x06: case 0x07: case 0x16: case 0x17:                     /* BLEZ BGTZ (L) */
        gpr_ld(k, 1, RAX, rs);
        x_r(e, 0, 1, 0x85, RAX, RAX);                               /* test rax, rax */
        cc = (op & 1) ? CC_G : CC_LE;
        likely = op >= 0x16;
        break;
    case 0x01:                                                      /* BLTZ BGEZ (L)(AL) */
        gpr_ld(k, 1, RAX, rs);
        x_r(e, 0, 1, 0x85, RAX, RAX);
        cc = (rt & 1) ? CC_GE : CC_L;
        likely = (rt & 2) != 0;
        link = (rt & 0x10) != 0;
        break;
    case 0x11: {                                                    /* BC1F BC1T (L) */
        uint32_t kd = (uint32_t)(idx + 1);
        x_m(e, 0, 0, 0xF7, 0, CTX, -1, OFF(cp0) + 8 * CP0_STATUS); b4(e, ST_CU1);
        Stub *s = add_stub(k, 0, jcc(e, CC_E), insn, pc, kd);      /* CpU dall'interprete */
        s->resume = e->p;
        {
            int fcc = k->c->mips4 ? (int)(insn >> 18) & 7 : 0;    /* MIPS IV: cc 0..7 */
            x_m(e, 0, 0, 0xF7, 0, CTX, -1, OFF(fcr31)); b4(e, fcc ? 1u << (24 + fcc) : FCSR_C);
        }
        cc = (insn >> 16) & 1 ? CC_NE : CC_E;
        likely = (insn >> 17) & 1;
        break;
    }
    }

    *tgt = target;
    if (cc == CC_ALWAYS) {
        st_qi(e, OFF(branch_target), target, RAX);
    } else if (likely) {
        /* le mov non toccano i flag */
        if (link) gpr_sti(k, 31, pc + 8, RDX);
        add_stub(k, 1, jcc(e, cc ^ 1), 0, pc + 8, (uint32_t)(idx + 1));
        st_qi(e, OFF(branch_target), target, RAX);
        return BR_LIKELY;
    } else {
        mov_ri(e, RAX, target);
        mov_ri(e, RCX, pc + 8);
        x_r(e, 0, 1, 0x0F40u + (unsigned)(cc ^ 1), RAX, RCX);      /* cmov se non preso */
        st_q(e, RAX, OFF(branch_target));
    }
    if (link) gpr_sti(k, 31, pc + 8, RDX);
    return cc == CC_ALWAYS ? BR_ALWAYS : BR_COND;
}

/* ================================================================ */
/* cache dei blocchi                                                 */
/* ================================================================ */

/* Chiave dei blocchi: modo | allow64 << 2 | FR << 3 | CU1 << 4 | RM << 5.
   Status cambia solo con uscita dal blocco (kick, eccezione, ERET), RM solo
   con CTC1, che chiude il blocco. dyn la calcola allo stesso modo. */
static uint32_t cur_flags(const R4k *c)
{
    uint32_t st = (uint32_t)c->cp0[CP0_STATUS];
    return (uint32_t)c->mode | (uint32_t)c->allow64 << 2 | ((st >> 26) & 1) << 3
         | ((st >> 29) & 1) << 4 | (c->fcr31 & 3) << 5;
}

static uint32_t hash_of(uintptr_t host)
{
    uint64_t h = (uint64_t)host >> 2;
    return (uint32_t)(h ^ (h >> HASH_BITS)) & ((1u << HASH_BITS) - 1);
}

static uint32_t phash_of(uintptr_t hpage)
{
    uint64_t h = (uint64_t)hpage >> PM_PAGE_SHIFT;
    return (uint32_t)(h ^ (h >> PAGE_BITS)) & ((1u << PAGE_BITS) - 1);
}

static void flush_all(R4k *c)
{
    R4kJit *j = c->jit;
    j->p = j->code0;
    j->nblocks = 0;
    j->nexits = 0;
    j->nrets = 0;
    c->jit_gen++;
    c->jit_last = NULL;
    memset(j->hash, 0, sizeof j->hash);
    memset(j->page, 0, sizeof j->page);
    memset(j->jc, 0, sizeof j->jc);
    j->flushes++;
    c->jit_exit = 1;
}

void r4k_jit_flush(R4k *c)
{
    if (c->jit) flush_all(c);
}

static void unhash(R4kJit *j, JitBlock *b)
{
    JitBlock **pp = &j->hash[hash_of(b->host)];
    while (*pp && *pp != b) pp = &(*pp)->next;
    if (*pp) *pp = b->next;
}

static void invalidate_page(R4kJit *j, uintptr_t hpage)
{
    JitBlock **pp = &j->page[phash_of(hpage)];
    while (*pp) {
        JitBlock *b = *pp;
        if (b->hpage == hpage) {
            *pp = b->pnext;
            unhash(j, b);
            unlink_all(b);
            JitJc *jc = &j->jc[jc_index(b->host)];
            if (jc->host == b->host) memset(jc, 0, sizeof *jc);
            j->invalidated++;
        } else {
            pp = &b->pnext;
        }
    }
}

/* pm_write/DMA su una pagina PM_CODE */
static void jit_code_write(void *ctx, uint32_t pa)
{
    R4k *c = (R4k *)ctx;
    uint8_t *h = c->pm->host[pa >> PM_PAGE_SHIFT];
    pm_set_code(c->pm, pa, 0);
    if (h) invalidate_page(c->jit, (uintptr_t)h);
    c->jit_gen++;
    c->jit_exit = 1;
}

static JitBlock *new_block(R4k *c)
{
    R4kJit *j = c->jit;
    if (j->nblocks >= JIT_MAX_BLOCKS || j->nexits > JIT_MAX_EXITS - 4 * MAX_BLOCK ||
        j->nrets > JIT_MAX_RETS - 4 ||
        j->end - j->p < 64 * 1024)
        flush_all(c);
    JitBlock *b = &j->blocks[j->nblocks++];
    memset(b, 0, sizeof *b);
    return b;
}

static void link_block(R4k *c, JitBlock *b)
{
    R4kJit *j = c->jit;
    uint32_t h = hash_of(b->host), ph = phash_of(b->hpage);
    b->next = j->hash[h];
    j->hash[h] = b;
    b->pnext = j->page[ph];
    j->page[ph] = b;
}

/* traduce le istruzioni da va (host = dove stanno); n = 0 se nessuna */
static int translate(Comp *k, uint64_t va, const uint8_t *host, int maxn)
{
    X64 *e = &k->e;
    int n = 0;
    for (int i = 0; i < maxn; i++) {
        uint32_t insn = be32(host + 4 * i);
        uint64_t pc = va + 4 * (uint64_t)i;
        if (is_branch(insn)) {
            if (i + 1 >= maxn) break;                  /* il delay slot non ci sta */
            uint32_t slot = be32(host + 4 * i + 4);
            if (is_branch(slot)) break;                /* salto nel delay slot: interprete */
            uint64_t target = 0;
            int kind = emit_branch(k, insn, pc, i, &target);
            if (emit_insn(k, slot, pc + 4, i + 1, 1) == END) k->nochain = 1;
            gpr_writeback(k, k->dirty);
            switch (kind) {
            case BR_REG:
                alu_mi(e, 1, A_ADD, OFF(icount), i + 2);
                ld_q(e, RAX, OFF(branch_target));
                if (k->unmapped && !k->counting && k->j->nrets < JIT_MAX_RETS) emit_ret_cache(k);
                else emit_lookup(k);
                break;
            case BR_COND: {                            /* preso se branch_target == target */
                ld_q(e, RAX, OFF(branch_target));
                if (fits32(target)) alu_ri(e, 1, A_CMP, RAX, (int32_t)target);
                else { mov_ri(e, RCX, target); x_r(e, 0, 1, 0x3B, RAX, RCX); }
                uint8_t *nt = jcc(e, CC_NE);
                emit_exit_chain(k, target, i + 2);
                patch(e, nt, e->p);
                emit_exit_chain(k, pc + 8, i + 2);
                break;
            }
            default:                                   /* J, JAL, likely preso */
                emit_exit_chain(k, target, i + 2);
                break;
            }
            return i + 2;
        }
        n = i + 1;
        if (emit_insn(k, insn, pc, i, 0) == END) {
            gpr_writeback(k, k->dirty);
            emit_exit_pc(k, pc + 4, n);
            return n;
        }
    }
    if (n) {
        gpr_writeback(k, k->dirty);
        emit_exit_chain(k, va + 4 * (uint64_t)n, n);
    }
    return n;
}

static void emit_stubs(Comp *k)
{
    X64 *e = &k->e;
    for (int i = 0; i < k->nstub; i++) {
        Stub *s = &k->stubs[i];
        patch(e, s->from, e->p);
        gpr_writeback(k, s->dirty);
        if (s->kind == 0) {
            emit_call(k, s->insn, s->pc, s->kd);
            gpr_reload(k, s->insn);
            jmp_to(e, s->resume);
        } else {
            emit_exit_chain(k, s->pc, (int)s->kd);     /* likely non preso */
        }
    }
}

static JitBlock *compile(R4k *c, uint64_t va, uintptr_t host, uint32_t flags)
{
    R4kJit *j = c->jit;
    int maxn = j->max_block;
    int room = (int)((PM_PAGE_SIZE - (va & PM_PAGE_MASK)) >> 2);
    if (maxn > room) maxn = room;

    for (int attempt = 0; attempt < 2; attempt++) {
        JitBlock *b = new_block(c);
        b->va = va; b->host = host; b->flags = flags;
        b->hpage = host - (uintptr_t)(va & PM_PAGE_MASK);

        static Comp k;
        k.c = c; k.j = j; k.b = b; k.nstub = 0; k.nochain = 0;
        k.allow64 = (int)(flags >> 2) & 1;
        k.kernel = (flags & 3) == R4K_MODE_KERNEL;
        k.fr = (int)(flags >> 3) & 1;
        k.cu1 = (int)(flags >> 4) & 1;
        k.rm = (int)(flags >> 5) & 3;
        k.unmapped = k.kernel && kseg01(va);
        k.fast_off = OFF(fast) + (int32_t)((flags & 3) * R4K_FAST_SIZE * sizeof(R4kFast));
        uint8_t *start = j->p;

        /* prima passata: quali registri MIPS usa il blocco */
        k.counting = 1;
        k.cached = k.dirty = 0;
        memset(k.uses, 0, sizeof k.uses);
        memset(k.writes, 0, sizeof k.writes);
        memset(k.hreg, -1, sizeof k.hreg);
        k.e.p = j->p; k.e.end = j->end; k.e.full = 0;
        translate(&k, va, (const uint8_t *)host, maxn);
        emit_stubs(&k);
        int full = k.e.full;

        /* i piu' usati nei registri host: conviene da 2 letture, o 3 usi
           se il registro viene anche scritto (va riscritto all'uscita) */
        for (int h = 0; h < NCACHE && !j->no_regs; h++) {
            int best = 0, bestv = 0;
            for (int r = 1; r < 32; r++) {
                if (k.cached & (1u << r)) continue;
                int v = (int)k.uses[r] - 1 - (k.writes[r] ? 1 : 0);
                if (v > bestv) { bestv = v; best = r; }
            }
            if (!best) break;
            k.cached |= 1u << best;
            k.hreg[best] = (int8_t)cache_host[h];
        }

        /* seconda passata: il codice vero */
        k.counting = 0;
        k.nstub = 0; k.nochain = 0; k.dirty = 0;
        k.e.p = j->p; k.e.end = j->end; k.e.full = full;
        for (int r = 1; r < 32; r++)
            if (k.cached & (1u << r)) ld_q(&k.e, k.hreg[r], REG(r));
        int n = translate(&k, va, (const uint8_t *)host, maxn);
        emit_stubs(&k);
        if (k.e.full) { j->nblocks--; flush_all(c); continue; }

        b->ninsn = (uint32_t)n;
        if (n) {
            b->code = start;
            j->p = k.e.p;
            j->code_bytes += (uint64_t)(k.e.p - start);
            /* le scritture su questa pagina devono passare da code_write */
            uint64_t pa;
            if (r4k_translate_debug(c, va, 0, &pa) && pa < ((uint64_t)1 << 32) &&
                !(c->pm->flags[pa >> PM_PAGE_SHIFT] & PM_CODE)) {
                pm_set_code(c->pm, pa, 1);
                r4k_fast_protect(c, b->hpage);
            }
        }
        j->compiled++;
        link_block(c, b);
        return b;
    }
    return NULL;
}

const uint8_t *r4k_jit_find(R4k *c)
{
    R4kJit *j = c->jit;
    if (j->map_gen != c->pm->map_gen) {
        j->map_gen = c->pm->map_gen;
        flush_all(c);
    }
    uint64_t pc = c->pc;
    R4kFast *f = r4k_fast(c, pc);
    if (f->tag_x != (pc & ~(uint64_t)0xFFC)) {
        /* eccezioni e I/O restano all'interprete */
        if (!r4k_fetch_fill(c, pc) || f->tag_x != (pc & ~(uint64_t)0xFFC)) { j->f_nofetch++; return NULL; }
    }
    uintptr_t host = (uintptr_t)FAST_HOST(f, pc);
    uint32_t flags = cur_flags(c);
    JitExit *x = (JitExit *)c->jit_last;           /* uscita concatenabile appena presa */
    c->jit_last = NULL;
    if (x) j->f_chain++;
    {
        const JitJc *jc = &j->jc[jc_index(host)];
        if (jc->host == host && jc->va == pc && jc->flags == flags) j->f_jchit++;
    }
    JitBlock *b;
    for (b = j->hash[hash_of(host)]; b; b = b->next)
        if (b->host == host && b->va == pc && b->flags == flags) break;
    if (!b) {
        uint32_t nb = (uint32_t)j->flushes;
        uint64_t t0 = __rdtsc();
        b = compile(c, pc, host, flags);
        j->compile_ticks += __rdtsc() - t0;
        if (!b) return NULL;
        if (nb != (uint32_t)j->flushes) x = NULL;     /* le uscite non esistono piu' */
    }
    if (b->code) {
        JitJc *jc = &j->jc[jc_index(host)];
        jc->host = host; jc->va = pc; jc->code = b->code; jc->ninsn = b->ninsn; jc->flags = b->flags;
    }
    if (x && b->code && !x->to && x->target == pc && x->from->flags == b->flags &&
        (x->from->hpage == b->hpage || (kseg01(pc) && kseg01(x->from->va))))
        link_exit(j, x, b);
    if (!b->code) { j->f_nocode++; return NULL; }
    if (b->ninsn > c->limit - c->icount) { j->f_nofit++; return R4K_JIT_NOFIT; }
    return b->code;
}

void r4k_jit_run(R4k *c, const uint8_t *code)
{
    R4kJit *j = c->jit;
    c->jit_exit = 0;
    j->runs++;
    j->enter(c, code);
}

/* ================================================================ */
/* inizializzazione                                                  */
/* ================================================================ */

/* void enter(R4k *c, const uint8_t *code): salva i registri da preservare,
   rbx = c, salta al blocco; i blocchi tornano a exit. */
static void emit_thunk(R4kJit *j)
{
    X64 e = { j->buf, j->buf + 1024, 0 };
    static const int saved[8] = { RBX, RBP, RSI, RDI, R12, R13, R14, R15 };
    const int arg0 = ARG0, arg1 = ARG1;
    j->enter = (void (*)(R4k *, const uint8_t *))(void *)e.p;
    for (int i = 0; i < 8; i++) { rex(&e, 0, 0, -1, saved[i], 0); b1(&e, 0x50u + (saved[i] & 7)); }
    alu_ri(&e, 1, A_SUB, RSP, 40);                      /* 32 di shadow space + allineamento */
    mov_rr(&e, CTX, arg0);
    rex(&e, 0, 0, -1, arg1, 0); b1(&e, 0xFF); b1(&e, 0xE0u | (arg1 & 7));   /* jmp arg1 */
    j->exit = e.p;
    alu_ri(&e, 1, A_ADD, RSP, 40);
    for (int i = 7; i >= 0; i--) { rex(&e, 0, 0, -1, saved[i], 0); b1(&e, 0x58u + (saved[i] & 7)); }
    b1(&e, 0xC3);

    /* dyn: il blocco per c->pc (dopo un salto a registro, un'eccezione, un
       ERET) dalla MMU veloce del modo attuale e dalla jump cache */
    typedef char fast_table_is_32k[R4K_FAST_SIZE * sizeof(R4kFast) == 1u << 15 ? 1 : -1];
    (void)sizeof(fast_table_is_32k);
    uint8_t *miss[7];
    j->dyn = e.p;
    x_r(&e, 0, 0, 0x33, R10, R10);                      /* xor r10d, r10d: nessun JitRet */
    j->dyn_ret = e.p;
    alu_mi(&e, 0, A_CMP, OFF(jit_exit), 0);
    miss[0] = jcc(&e, CC_NE);
    ld_q(&e, RAX, OFF(pc));
    ld_d(&e, RDX, OFF(mode));
    ld_d(&e, R9, OFF(allow64));
    shift_ri(&e, 0, S_SHL, R9, 2);
    x_r(&e, 0, 0, 0x0B, R9, RDX);                       /* or r9d, edx */
    static const int fbit[3][3] = { { 26, 3, 1 }, { 29, 4, 1 }, { 0, 5, 3 } };  /* FR, CU1, RM */
    for (int i = 0; i < 3; i++) {
        ld_d(&e, RCX, i < 2 ? OFF(cp0) + 8 * CP0_STATUS : OFF(fcr31));
        if (fbit[i][0]) shift_ri(&e, 0, S_SHR, RCX, fbit[i][0]);
        alu_ri(&e, 0, A_AND, RCX, fbit[i][2]);
        shift_ri(&e, 0, S_SHL, RCX, fbit[i][1]);
        x_r(&e, 0, 0, 0x0B, R9, RCX);                   /* or r9d, ecx = flags */
    }
    shift_ri(&e, 0, S_SHL, RDX, 15);                    /* mode * sizeof fast[0] */
    x_r(&e, 0, 0, 0x89, RAX, RCX);                      /* mov ecx, eax */
    shift_ri(&e, 0, S_SHR, RCX, PM_PAGE_SHIFT - 5);
    alu_ri(&e, 0, A_AND, RCX, (R4K_FAST_SIZE - 1) << 5);
    x_r(&e, 0, 0, 0x03, RCX, RDX);                      /* add ecx, edx */
    mov_rr(&e, RDX, RAX);
    alu_ri(&e, 1, A_AND, RDX, (int32_t)~0xFFCu);        /* pagina e allineamento */
    x_m(&e, 0, 1, 0x3B, RDX, CTX, RCX, OFF(fast) + (int32_t)offsetof(R4kFast, tag_x));
    miss[1] = jcc(&e, CC_NE);
    mov_rr(&e, RDX, RAX);
    x_m(&e, 0, 1, 0x03, RDX, CTX, RCX, OFF(fast) + (int32_t)offsetof(R4kFast, addend));   /* host */
    x_r(&e, 0, 0, 0x89, RDX, RCX);                      /* mov ecx, edx */
    shift_ri(&e, 0, S_SHR, RCX, 2);
    alu_ri(&e, 0, A_AND, RCX, JC_SIZE - 1);
    shift_ri(&e, 0, S_SHL, RCX, 5);
    mov_ri(&e, R8, (uint64_t)(uintptr_t)j->jc);
    x_r(&e, 0, 1, 0x03, R8, RCX);                       /* add r8, rcx */
    x_m(&e, 0, 1, 0x3B, RDX, R8, -1, (int32_t)offsetof(JitJc, host));
    miss[2] = jcc(&e, CC_NE);
    x_m(&e, 0, 1, 0x3B, RAX, R8, -1, (int32_t)offsetof(JitJc, va));
    miss[3] = jcc(&e, CC_NE);
    x_m(&e, 0, 0, 0x3B, R9, R8, -1, (int32_t)offsetof(JitJc, flags));
    miss[4] = jcc(&e, CC_NE);
    ld_q(&e, RDX, OFF(icount));
    x_m(&e, 0, 0, 0x8B, RCX, R8, -1, (int32_t)offsetof(JitJc, ninsn));
    x_r(&e, 0, 1, 0x03, RDX, RCX);
    alu_m(&e, 1, A_CMP, RDX, OFF(limit));
    miss[5] = jcc(&e, CC_A);
    /* dyn_ret: ricorda la destinazione se e' in kseg0/kseg1 */
    x_r(&e, 0, 1, 0x85, R10, R10);                      /* test r10, r10 */
    uint8_t *go1 = jcc(&e, CC_E);
    mov_rr(&e, RDX, RAX);
    shift_ri(&e, 1, S_SAR, RDX, 30);
    alu_ri(&e, 1, A_CMP, RDX, -2);
    uint8_t *go2 = jcc(&e, CC_NE);
    x_m(&e, 0, 1, 0x89, RAX, R10, -1, (int32_t)offsetof(JitRet, va));
    x_m(&e, 0, 1, 0x8B, RDX, R8, -1, (int32_t)offsetof(JitJc, code));
    x_m(&e, 0, 1, 0x89, RDX, R10, -1, (int32_t)offsetof(JitRet, code));
    x_m(&e, 0, 0, 0x8B, RDX, R8, -1, (int32_t)offsetof(JitJc, ninsn));
    x_m(&e, 0, 0, 0x89, RDX, R10, -1, (int32_t)offsetof(JitRet, ninsn));
    ld_d(&e, RDX, OFF(jit_gen));
    x_m(&e, 0, 0, 0x89, RDX, R10, -1, (int32_t)offsetof(JitRet, gen));
    patch(&e, go1, e.p);
    patch(&e, go2, e.p);
    x_m(&e, 0, 0, 0xFF, 4, R8, -1, (int32_t)offsetof(JitJc, code));     /* jmp [r8 + code] */
    for (int i = 0; i < 6; i++) patch(&e, miss[i], j->exit);
    (void)miss[6];
    j->code0 = j->buf + 1024;
    j->p = j->code0;
}

int r4k_jit_init(R4k *c, int max_block)
{
    typedef char fast_is_32_bytes[sizeof(R4kFast) == 32 ? 1 : -1];
    (void)sizeof(fast_is_32_bytes);
    if (c->jit) return 0;
    R4kJit *j = (R4kJit *)calloc(1, sizeof *j);
    if (!j) return -1;
    j->blocks = (JitBlock *)calloc(JIT_MAX_BLOCKS, sizeof *j->blocks);
    j->exits = (JitExit *)calloc(JIT_MAX_EXITS, sizeof *j->exits);
    j->rets = (JitRet *)calloc(JIT_MAX_RETS, sizeof *j->rets);
#if defined(_WIN32)
    j->buf = (uint8_t *)VirtualAlloc(NULL, JIT_BUF_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    j->buf = (uint8_t *)mmap(NULL, JIT_BUF_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (j->buf == (uint8_t *)MAP_FAILED) j->buf = NULL;
#endif
    if (!j->blocks || !j->exits || !j->rets || !j->buf) {
        free(j->blocks); free(j->exits); free(j->rets); free(j);
        return -1;
    }
    j->end = j->buf + JIT_BUF_SIZE;
    j->max_block = max_block <= 0 ? DEFAULT_BLOCK : max_block > MAX_BLOCK ? MAX_BLOCK : max_block;
    /* MXCSR: eccezioni mascherate, flag a zero, arrotondamento da FCSR.RM
       (0 vicino, 1 verso zero, 2 verso +inf, 3 verso -inf) */
    static const uint32_t rc[4] = { 0x0000, 0x6000, 0x4000, 0x2000 };
    for (int i = 0; i < 4; i++) j->mxcsr[i] = 0x1F80 | rc[i];
    j->mxcsr[4] = 0x1F80;
    j->no_regs = getenv("R4K_JIT_NOREGS") != NULL;
    j->no_fpu = getenv("R4K_JIT_NOFPU") != NULL;
    emit_thunk(j);
    c->jit = j;
    j->map_gen = c->pm->map_gen;
    c->pm->code_write = jit_code_write;
    c->pm->code_ctx = c;
    return 0;
}

void r4k_jit_free(R4k *c)
{
    R4kJit *j = c->jit;
    if (!j) return;
#if defined(_WIN32)
    VirtualFree(j->buf, 0, MEM_RELEASE);
#else
    munmap(j->buf, JIT_BUF_SIZE);
#endif
    free(j->blocks);
    free(j->exits);
    free(j->rets);
    free(j);
    c->jit = NULL;
    if (c->pm->code_ctx == c) { c->pm->code_write = NULL; c->pm->code_ctx = NULL; }
}

static void top(const char *what, const uint64_t *v, int n, uint64_t total)
{
    for (int shown = 0; shown < 8; shown++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (v[i] && (best < 0 || v[i] > v[best])) best = i;
        if (best < 0 || v[best] * 1000 < total) break;
        printf("  %s %02X: %llu\n", what, best, (unsigned long long)v[best]);
        ((uint64_t *)v)[best] = 0;
    }
}

int r4k_jit_owner(const R4k *c, const void *host_pc, uint64_t *va)
{
    const R4kJit *j = c->jit;
    const uint8_t *p = (const uint8_t *)host_pc;
    if (!j || p < j->buf || p >= j->end) return 0;
    /* i blocchi sono in ordine di indirizzo del codice (quelli senza codice
       in mezzo non contano) */
    int lo = 0, hi = j->nblocks - 1, found = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, m = mid;
        while (m >= lo && !j->blocks[m].code) m--;
        if (m < lo) { lo = mid + 1; continue; }
        if (j->blocks[m].code <= p) { found = m; lo = mid + 1; }
        else hi = m - 1;
    }
    if (found < 0 || p >= j->p) return 2;
    *va = j->blocks[found].va;
    return 1;
}

void r4k_jit_print_stats(const R4k *c)
{
    R4kJit *j = c->jit;
    if (!j) return;
    printf("JIT: %llu blocchi tradotti (%llu KB), %llu invalidati, %llu svuotamenti, "
           "%llu ingressi, %llu collegamenti, %llu chiamate all'interprete\n",
           (unsigned long long)j->compiled, (unsigned long long)(j->code_bytes >> 10),
           (unsigned long long)j->invalidated, (unsigned long long)j->flushes,
           (unsigned long long)j->runs, (unsigned long long)j->links,
           (unsigned long long)j->helpers);
    printf("  traduzione: %.2f Gcicli\n", (double)j->compile_ticks / 1e9);
    printf("  in C: %llu da uscite da collegare, %llu gia' in jump cache, %llu senza fetch veloce, "
           "%llu non traducibili, %llu oltre il limite\n",
           (unsigned long long)j->f_chain, (unsigned long long)j->f_jchit, (unsigned long long)j->f_nofetch,
           (unsigned long long)j->f_nocode, (unsigned long long)j->f_nofit);
    top("op", j->fb_op, 64, j->helpers);
    top("special", j->fb_special, 64, j->helpers);
    top("cop1 fmt", j->fb_cop1, 32, j->helpers);
    for (int shown = 0; shown < 10; shown++) {                  /* i pc piu' caldi */
        int best = -1;
        for (int i = 0; i < HOT_SIZE; i++)
            if (j->hot[i].n && (best < 0 || j->hot[i].n > j->hot[best].n)) best = i;
        if (best < 0) break;
        printf("  pc %016llX %08X: ~%llu\n", (unsigned long long)j->hot[best].pc,
               j->hot[best].insn, (unsigned long long)j->hot[best].n);
        j->hot[best].n = 0;
    }
}
