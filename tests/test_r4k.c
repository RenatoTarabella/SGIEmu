/*
 * test_r4k.c - Test della CPU R4400 per quello che Unicorn non controlla:
 * TLB, vettori ed EPC/BD delle eccezioni, delay slot, modo utente,
 * interrupt e timer, xkphys, FPU in formato L, FR = 0, trap FPU, CACHE,
 * invalidazione della MMU veloce e pagine di codice.
 *
 *   test_r4k            tutti i test
 *   test_r4k --bench    velocita' dell'interprete (milioni di istruzioni al secondo)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cpu/r4k.h"
#include "cpu/r4k_disasm.h"

#define RAM_SIZE (8u << 20)
#define KSEG0    0xFFFFFFFF80000000ull
#define KSEG1    0xFFFFFFFFA0000000ull
#define CODE_PA  0x1000u

/* ---- codifica ---- */
#define R_(fn, rs, rt, rd, sa) ((uint32_t)(((rs) << 21) | ((rt) << 16) | ((rd) << 11) | ((sa) << 6) | (fn)))
#define I_(op, rs, rt, imm)    ((uint32_t)(((op) << 26) | ((rs) << 21) | ((rt) << 16) | ((imm) & 0xFFFF)))
#define COP0(sub, rt, rd)      ((uint32_t)((0x10u << 26) | ((sub) << 21) | ((rt) << 16) | ((rd) << 11)))
#define CO0(fn)                ((uint32_t)((0x10u << 26) | (1u << 25) | (fn)))
#define COP1(fmt, ft, fs, fd, fn) ((uint32_t)((0x11u << 26) | ((fmt) << 21) | ((ft) << 16) | ((fs) << 11) | ((fd) << 6) | (fn)))

enum { ZERO = 0, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3, T4, T5, T6, T7,
       S0, S1, S2, S3, S4, S5, S6, S7, T8, T9, K0, K1, GP, SP, S8, RA };

#define NOP        0u
#define ADDIU(t, s, i) I_(0x09, s, t, i)
#define DADDIU(t, s, i) I_(0x19, s, t, i)
#define LUI(t, i)  I_(0x0F, 0, t, i)
#define ORI(t, s, i) I_(0x0D, s, t, i)
#define LW(t, i, s) I_(0x23, s, t, i)
#define LD(t, i, s) I_(0x37, s, t, i)
#define SW(t, i, s) I_(0x2B, s, t, i)
#define LWL(t, i, s) I_(0x22, s, t, i)
#define LWR(t, i, s) I_(0x26, s, t, i)
#define SWL(t, i, s) I_(0x2A, s, t, i)
#define SWR(t, i, s) I_(0x2E, s, t, i)
#define LL(t, i, s) I_(0x30, s, t, i)
#define SC(t, i, s) I_(0x38, s, t, i)
#define BEQ(s, t, o) I_(0x04, s, t, o)
#define BNE(s, t, o) I_(0x05, s, t, o)
#define BNEL(s, t, o) I_(0x15, s, t, o)
#define ADD(d, s, t) R_(0x20, s, t, d, 0)
#define DADDU(d, s, t) R_(0x2D, s, t, d, 0)
#define JR(s)      R_(0x08, s, 0, 0, 0)
#define SYSCALL    R_(0x0C, 0, 0, 0, 0)
#define MFC0(t, d) COP0(0, t, d)
#define MTC0(t, d) COP0(4, t, d)
#define TLBR       CO0(0x01)
#define TLBWI      CO0(0x02)
#define TLBWR      CO0(0x06)
#define TLBP       CO0(0x08)
#define ERET       CO0(0x18)
#define CACHE(op, i, s) I_(0x2F, s, op, i)
#define MTC1(t, s) ((uint32_t)((0x11u << 26) | (4u << 21) | ((t) << 16) | ((s) << 11)))
#define DMFC1(t, s) ((uint32_t)((0x11u << 26) | (1u << 21) | ((t) << 16) | ((s) << 11)))
#define DMTC1(t, s) ((uint32_t)((0x11u << 26) | (5u << 21) | ((t) << 16) | ((s) << 11)))
#define CTC1(t, s) ((uint32_t)((0x11u << 26) | (6u << 21) | ((t) << 16) | ((s) << 11)))

static PhysMem pm;
static R4k cpu;
static R4k *c = &cpu;
static uint8_t *ram;
static int failures, checks;
static int last_exc;
static int stop_on_exc;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("  FALLITO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_EQ(a, b) do { uint64_t a_ = (uint64_t)(a), b_ = (uint64_t)(b); \
    CHECK(a_ == b_, "%s = %016llX, atteso %016llX", #a, (unsigned long long)a_, (unsigned long long)b_); } while (0)

static void on_exc(R4k *cc, int code, void *user)
{
    (void)user;
    last_exc = code;
    if (stop_on_exc) r4k_stop(cc);
}

/* CPU in kernel (EXL = ERL = 0, BEV = 0), codice a kseg0 + CODE_PA */
static void setup(void)
{
    memset(ram, 0, RAM_SIZE);
    r4k_reset(c);
    memset(c->fpr, 0, sizeof c->fpr);
    r4k_cp0_write(c, CP0_STATUS, ST_CU1 | ST_FR);
    c->pc = KSEG0 + CODE_PA;
    last_exc = -1;
    stop_on_exc = 1;
}

static void put(uint32_t pa, const uint32_t *w, int n)
{
    for (int k = 0; k < n; k++) wbe32(ram + pa + 4 * k, w[k]);
}

#define CODE(...) do { const uint32_t code_[] = { __VA_ARGS__ }; \
    put(CODE_PA, code_, (int)(sizeof code_ / sizeof code_[0])); } while (0)

static void run(int n) { r4k_run(c, (uint64_t)n); }

static uint64_t exc_code(void) { return (c->cp0[CP0_CAUSE] >> 2) & 31; }

/* voce del TLB via registri CP0 */
static void tlb_put(int idx, uint64_t mask, uint64_t hi, uint64_t lo0, uint64_t lo1)
{
    r4k_cp0_write(c, CP0_INDEX, (uint64_t)idx);
    r4k_cp0_write(c, CP0_PAGEMASK, mask);
    r4k_cp0_write(c, CP0_ENTRYHI, hi);
    r4k_cp0_write(c, CP0_ENTRYLO0, lo0);
    r4k_cp0_write(c, CP0_ENTRYLO1, lo1);
    wbe32(ram + 0x100, TLBWI);          /* eseguito dalla CPU, a kseg0+0x100 */
    wbe32(ram + 0x104, NOP);
    uint64_t pc = c->pc;
    c->pc = KSEG0 + 0x100;
    r4k_step(c);
    c->pc = pc;
}

#define LO(pfn, flags) ((((uint64_t)(pfn)) << 6) | (flags))   /* flags: D=4 V=2 G=1 */

/* ---------------------------------------------------------------- */

static void test_delay_slot(void)
{
    printf("delay slot e branch likely\n");
    setup();
    CODE(BEQ(ZERO, ZERO, 2), ADDIU(T0, T0, 1), ADDIU(T0, T0, 2), ADDIU(T0, T0, 4));
    run(3);
    CHECK_EQ(c->r[T0], 5);
    CHECK_EQ(c->pc, KSEG0 + CODE_PA + 16);

    setup();
    CODE(BNEL(ZERO, ZERO, 2), ADDIU(T0, T0, 1), ADDIU(T0, T0, 2), ADDIU(T0, T0, 4));
    run(3);                                            /* il delay slot non conta */
    CHECK_EQ(c->r[T0], 6);

    /* JR con l'indirizzo nel delay slot che cambia il registro */
    setup();
    c->r[T1] = KSEG0 + CODE_PA + 12;
    CODE(JR(T1), ADDIU(T1, T1, 100), ADDIU(T0, T0, 2), ADDIU(T0, T0, 4));
    run(3);
    CHECK_EQ(c->r[T0], 4);
    CHECK_EQ(c->r[T1], KSEG0 + CODE_PA + 112);
}

static void test_exceptions(void)
{
    printf("eccezioni: EPC, BD, vettori\n");
    /* overflow: niente scrittura, EPC sull'istruzione */
    setup();
    c->r[T0] = 0x7FFFFFFF; c->r[T1] = 1; c->r[T2] = 0x1234;
    CODE(NOP, ADD(T2, T0, T1));
    run(5);
    CHECK_EQ(last_exc, EXC_OV);
    CHECK_EQ(c->r[T2], 0x1234);
    CHECK_EQ(c->cp0[CP0_EPC], KSEG0 + CODE_PA + 4);
    CHECK_EQ(c->cp0[CP0_CAUSE] & CAUSE_BD, 0);
    CHECK_EQ(c->pc, KSEG0 + 0x180);
    CHECK(c->cp0[CP0_STATUS] & ST_EXL, "EXL non impostato");

    /* TLB miss nel delay slot: EPC sul salto, BD, vettore di refill */
    setup();
    r4k_cp0_write(c, CP0_ENTRYHI, 0x42);
    c->r[A0] = 0x00402468;
    CODE(BEQ(ZERO, ZERO, 4), LW(T0, 0, A0));
    run(5);
    CHECK_EQ(last_exc, EXC_TLBL);
    CHECK_EQ(c->cp0[CP0_EPC], KSEG0 + CODE_PA);
    CHECK(c->cp0[CP0_CAUSE] & CAUSE_BD, "BD non impostato");
    CHECK_EQ(c->pc, KSEG0);
    CHECK_EQ(c->cp0[CP0_BADVADDR], 0x00402468);
    CHECK_EQ(c->cp0[CP0_ENTRYHI], 0x00402000 | 0x42);
    CHECK_EQ(c->cp0[CP0_CONTEXT] & 0x7FFFF0, (0x00402468 >> 13) << 4);

    /* con EXL gia' a 1: vettore generale, EPC invariato */
    setup();
    r4k_cp0_write(c, CP0_STATUS, ST_EXL);
    c->cp0[CP0_EPC] = 0xDEAD;
    c->r[A0] = 0x1000;
    CODE(LW(T0, 0, A0));
    run(2);
    CHECK_EQ(last_exc, EXC_TLBL);
    CHECK_EQ(c->pc, KSEG0 + 0x180);
    CHECK_EQ(c->cp0[CP0_EPC], 0xDEAD);

    /* BEV = 1: SYSCALL va a 0xBFC00380 */
    setup();
    r4k_cp0_write(c, CP0_STATUS, ST_BEV);
    CODE(SYSCALL);
    run(1);
    CHECK_EQ(last_exc, EXC_SYS);
    CHECK_EQ(c->pc, 0xFFFFFFFFBFC00380ull);

    /* load non allineata: AdEL con BadVAddr */
    setup();
    c->r[A0] = KSEG0 + 0x2002;
    CODE(LW(T0, 0, A0));
    run(1);
    CHECK_EQ(last_exc, EXC_ADEL);
    CHECK_EQ(c->cp0[CP0_BADVADDR], KSEG0 + 0x2002);

    /* salto a un indirizzo non allineato: AdEL sul fetch, EPC = destinazione */
    setup();
    c->r[T1] = KSEG0 + 0x3002;
    CODE(JR(T1), NOP);
    run(3);
    CHECK_EQ(last_exc, EXC_ADEL);
    CHECK_EQ(c->cp0[CP0_EPC], KSEG0 + 0x3002);
    CHECK_EQ(c->cp0[CP0_BADVADDR], KSEG0 + 0x3002);

    /* bus error su un indirizzo fisico vuoto */
    setup();
    c->r[A0] = KSEG1 + 0x1F000000;
    CODE(LW(T0, 0, A0));
    run(1);
    CHECK_EQ(last_exc, EXC_DBE);

    /* ERET: torna a EPC, azzera EXL e il bit di LL */
    setup();
    c->r[A0] = KSEG0 + 0x2000;
    c->cp0[CP0_EPC] = KSEG0 + CODE_PA + 12;
    r4k_cp0_write(c, CP0_STATUS, ST_EXL);
    CODE(LL(T0, 0, A0), ERET, NOP, SC(T1, 0, A0));
    c->r[T1] = 77;
    run(3);
    CHECK_EQ(c->r[T1], 0);
    CHECK_EQ(c->cp0[CP0_STATUS] & ST_EXL, 0);
    /* LL/SC riuscita */
    setup();
    c->r[A0] = KSEG0 + 0x2000; c->r[T1] = 99;
    CODE(LL(T0, 0, A0), SC(T1, 0, A0));
    run(2);
    CHECK_EQ(c->r[T1], 1);
    CHECK_EQ(be32(ram + 0x2000), 99);
}

static void test_tlb(void)
{
    printf("TLB: mappatura, ASID, globali, Mod, invalidi, TLBP/TLBR, MMU veloce\n");
    setup();
    wbe32(ram + 0x200010, 0x11111111);
    wbe32(ram + 0x300010, 0x22222222);
    tlb_put(3, 0, 0x00400000 | 5, LO(0x200, 2 | 4), LO(0x300, 2));
    r4k_cp0_write(c, CP0_ENTRYHI, 5);
    c->r[A0] = 0x00400010; c->r[A1] = 0x00401010;
    CODE(LW(T0, 0, A0), LW(T1, 0, A1), SW(T0, 4, A0), SW(T0, 4, A1));
    run(4);
    CHECK_EQ(c->r[T0], 0x11111111);
    CHECK_EQ(c->r[T1], 0x22222222);
    CHECK_EQ(be32(ram + 0x200014), 0x11111111);
    CHECK_EQ(last_exc, EXC_MOD);                       /* pagina dispari senza D */
    CHECK_EQ(c->cp0[CP0_BADVADDR], 0x00401014);
    CHECK_EQ(c->pc, KSEG0 + 0x180);

    /* cambio di ASID: la stessa pagina (gia' nella MMU veloce) ora manca */
    setup();
    tlb_put(3, 0, 0x00400000 | 5, LO(0x200, 2 | 4), LO(0x300, 2));
    r4k_cp0_write(c, CP0_ENTRYHI, 5);
    c->r[A0] = 0x00400010;
    CODE(LW(T0, 0, A0), MTC0(A1, CP0_ENTRYHI), LW(T1, 0, A0));
    c->r[A1] = 6;
    run(3);
    CHECK_EQ(last_exc, EXC_TLBL);
    CHECK_EQ(c->pc, KSEG0);                            /* refill */

    /* voce globale: vale con qualsiasi ASID */
    setup();
    wbe32(ram + 0x200010, 0x33333333);
    tlb_put(7, 0, 0x00400000 | 9, LO(0x200, 2 | 1), LO(0x300, 2 | 1));
    r4k_cp0_write(c, CP0_ENTRYHI, 0x77);
    c->r[A0] = 0x00400010;
    CODE(LW(T0, 0, A0));
    run(1);
    CHECK_EQ(last_exc, -1);
    CHECK_EQ(c->r[T0], 0x33333333);

    /* voce non valida: TLBL al vettore generale */
    setup();
    tlb_put(1, 0, 0x00400000, LO(0x200, 0), LO(0x300, 0));
    c->r[A0] = 0x00400010;
    CODE(LW(T0, 0, A0));
    run(1);
    CHECK_EQ(last_exc, EXC_TLBL);
    CHECK_EQ(c->pc, KSEG0 + 0x180);

    /* riscrivere la voce cambia la traduzione anche se era nella MMU veloce */
    setup();
    wbe32(ram + 0x200010, 0xAAAA);
    wbe32(ram + 0x500010, 0xBBBB);
    tlb_put(2, 0, 0x00400000, LO(0x200, 6), LO(0x201, 6));
    c->r[A0] = 0x00400010;
    CODE(LW(T0, 0, A0));
    run(1);
    CHECK_EQ(c->r[T0], 0xAAAA);
    tlb_put(2, 0, 0x00400000, LO(0x500, 6), LO(0x501, 6));
    c->pc = KSEG0 + CODE_PA;
    run(1);
    CHECK_EQ(c->r[T0], 0xBBBB);

    /* pagine da 16 KB */
    setup();
    wbe32(ram + 0x100000 + 0x3000, 0x16161616);
    wbe32(ram + 0x140000 + 0x0004, 0x61616161);
    tlb_put(4, 0x6000, 0x01000000, LO(0x100, 6), LO(0x140, 6));
    c->r[A0] = 0x01003000; c->r[A1] = 0x01004004;
    CODE(LW(T0, 0, A0), LW(T1, 0, A1));
    run(2);
    CHECK_EQ(c->r[T0], 0x16161616);
    CHECK_EQ(c->r[T1], 0x61616161);

    /* TLBP e TLBR */
    setup();
    tlb_put(11, 0, 0x00800000 | 3, LO(0x123, 6), LO(0x124, 7));
    r4k_cp0_write(c, CP0_ENTRYHI, 0x00800000 | 3);
    r4k_cp0_write(c, CP0_INDEX, 0);
    CODE(TLBP, NOP);
    run(1);
    CHECK_EQ(c->cp0[CP0_INDEX], 11);
    r4k_cp0_write(c, CP0_ENTRYHI, 0x00A00000 | 3);
    c->pc = KSEG0 + CODE_PA;
    run(1);
    CHECK(c->cp0[CP0_INDEX] & 0x80000000u, "TLBP: manca il bit P");
    setup();
    tlb_put(11, 0x1E000, 0x00800000 | 3, LO(0x123, 6), LO(0x124, 7));
    r4k_cp0_write(c, CP0_INDEX, 11);
    r4k_cp0_write(c, CP0_ENTRYHI, 0);
    CODE(TLBR);
    run(1);
    CHECK_EQ(c->cp0[CP0_ENTRYHI], 0x00800000 | 3);
    CHECK_EQ(c->cp0[CP0_PAGEMASK], 0x1E000);
    CHECK_EQ(c->cp0[CP0_ENTRYLO0], LO(0x123, 6));    /* G = G0 & G1 = 0 */
    CHECK_EQ(c->cp0[CP0_ENTRYLO1], LO(0x124, 6));

    /* TLBWR non tocca le voci sotto Wired */
    setup();
    for (int i = 0; i < 48; i++) tlb_put(i, 0, 0x10000000 + ((uint64_t)i << 13), 0, 0);
    r4k_cp0_write(c, CP0_WIRED, 40);
    uint32_t prog[400];
    for (int k = 0; k < 200; k++) { prog[2 * k] = TLBWR; prog[2 * k + 1] = NOP; }
    put(CODE_PA, prog, 400);
    r4k_cp0_write(c, CP0_ENTRYHI, 0x20000000);
    run(400);
    int ok = 1, hit = 0;
    for (int i = 0; i < 40; i++) ok &= c->tlb[i].hi == 0x10000000 + ((uint64_t)i << 13);
    for (int i = 40; i < 48; i++) hit += c->tlb[i].hi == 0x20000000;
    CHECK(ok, "TLBWR ha scritto una voce sotto Wired");
    CHECK(hit == 8, "TLBWR ha usato %d voci su 8", hit);

    /* xkseg con KX: refill al vettore XTLB (0x080) */
    setup();
    r4k_cp0_write(c, CP0_STATUS, ST_KX);
    c->r[A0] = 0xC000000000004000ull;
    CODE(LW(T0, 0, A0));
    run(1);
    CHECK_EQ(last_exc, EXC_TLBL);
    CHECK_EQ(c->pc, KSEG0 + 0x080);
    CHECK_EQ(c->cp0[CP0_XCONTEXT] & 0x1FFFFFFF0ull, (3ull << 31) | ((0x4000ull >> 13) << 4));
}

static void test_segments(void)
{
    printf("segmenti: xkphys, kseg1, modo utente e supervisor\n");
    setup();
    wbe64(ram + 0x1000 + 0x800, 0x0123456789ABCDEFull);
    r4k_cp0_write(c, CP0_STATUS, ST_KX);
    c->r[A0] = 0x9000000000001800ull;
    c->r[A1] = KSEG1 + 0x1800;
    CODE(LD(T0, 0, A0), LD(T1, 0, A1));
    run(2);
    CHECK_EQ(c->r[T0], 0x0123456789ABCDEFull);
    CHECK_EQ(c->r[T1], 0x0123456789ABCDEFull);

    setup();                                           /* xkphys senza KX */
    c->r[A0] = 0x9000000000001800ull;
    CODE(LD(T0, 0, A0));
    run(1);
    CHECK_EQ(last_exc, EXC_ADEL);

    /* modo utente: codice mappato a 0x10000 */
    setup();
    tlb_put(0, 0, 0x00010000, LO(CODE_PA >> 12, 2 | 1), LO(0, 0));
    r4k_cp0_write(c, CP0_STATUS, 2u << 3);             /* KSU = utente */
    c->pc = 0x10000;
    c->r[A0] = KSEG0 + 0x100;
    CODE(LW(T0, 0, A0));
    run(1);
    CHECK_EQ(last_exc, EXC_ADEL);
    CHECK_EQ(c->cp0[CP0_EPC], 0x10000);

    setup();
    tlb_put(0, 0, 0x00010000, LO(CODE_PA >> 12, 2 | 1), LO(0, 0));
    r4k_cp0_write(c, CP0_STATUS, 2u << 3);
    c->pc = 0x10000;
    CODE(MFC0(T0, CP0_STATUS));
    run(1);
    CHECK_EQ(last_exc, EXC_CPU);
    CHECK_EQ((c->cp0[CP0_CAUSE] >> 28) & 3, 0);

    setup();                                           /* 64 bit in utente con UX = 0 */
    tlb_put(0, 0, 0x00010000, LO(CODE_PA >> 12, 2 | 1), LO(0, 0));
    r4k_cp0_write(c, CP0_STATUS, 2u << 3);
    c->pc = 0x10000;
    CODE(DADDU(T0, T1, T2));
    run(1);
    CHECK_EQ(last_exc, EXC_RI);

    setup();                                           /* ... e con UX = 1 */
    tlb_put(0, 0, 0x00010000, LO(CODE_PA >> 12, 2 | 1), LO(0, 0));
    r4k_cp0_write(c, CP0_STATUS, (2u << 3) | ST_UX);
    c->pc = 0x10000;
    c->r[T1] = 1ull << 40; c->r[T2] = 5;
    CODE(DADDU(T0, T1, T2));
    run(1);
    CHECK_EQ(last_exc, -1);
    CHECK_EQ(c->r[T0], (1ull << 40) + 5);

    setup();                                           /* FPU senza CU1 */
    r4k_cp0_write(c, CP0_STATUS, 0);
    CODE(MTC1(T0, 2));
    run(1);
    CHECK_EQ(last_exc, EXC_CPU);
    CHECK_EQ((c->cp0[CP0_CAUSE] >> 28) & 3, 1);
}

static void test_interrupts(void)
{
    printf("interrupt esterni e Count/Compare\n");
    setup();
    r4k_cp0_write(c, CP0_STATUS, ST_IE | 0x0400);      /* IM2 */
    uint32_t loop[2] = { BEQ(ZERO, ZERO, -1), NOP };
    put(CODE_PA, loop, 2);
    run(10);
    CHECK_EQ(last_exc, -1);
    r4k_set_irq(c, 0, 1);
    run(10);
    CHECK_EQ(last_exc, EXC_INT);
    CHECK(c->cp0[CP0_CAUSE] & 0x0400, "IP2 non visibile in Cause");
    CHECK_EQ(c->pc, KSEG0 + 0x180);
    CHECK_EQ(c->cp0[CP0_EPC], KSEG0 + CODE_PA);        /* sul salto, anche dal delay slot */
    r4k_set_irq(c, 0, 0);
    CHECK_EQ(c->cp0[CP0_CAUSE] & 0x0400, 0);

    /* interrupt mascherato: non si prende */
    setup();
    r4k_cp0_write(c, CP0_STATUS, ST_IE | 0x0800);
    put(CODE_PA, loop, 2);
    r4k_set_irq(c, 0, 1);
    run(10);
    CHECK_EQ(last_exc, -1);

    /* timer: Count avanza di 1 ogni 2 istruzioni */
    setup();
    put(CODE_PA, loop, 2);
    r4k_cp0_write(c, CP0_COUNT, 0);
    r4k_cp0_write(c, CP0_COMPARE, 100);
    r4k_cp0_write(c, CP0_STATUS, ST_IE | 0x8000);      /* IM7 */
    uint64_t start = c->icount;
    run(1000);
    CHECK_EQ(last_exc, EXC_INT);
    CHECK(c->icount - start >= 199 && c->icount - start <= 202,
          "timer dopo %llu istruzioni", (unsigned long long)(c->icount - start));
    CHECK(c->cp0[CP0_CAUSE] & 0x8000, "IP7 non impostato");
    r4k_cp0_write(c, CP0_COMPARE, 5000);
    CHECK_EQ(c->cp0[CP0_CAUSE] & 0x8000, 0);
    CHECK_EQ(r4k_cp0_read(c, CP0_COUNT), 100);
}

static void test_unaligned(void)
{
    printf("LWL/LWR/SWL/SWR\n");
    setup();
    static const uint8_t b[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    memcpy(ram + 0x2000, b, 8);
    c->r[A0] = KSEG0 + 0x2001;
    c->r[A1] = 0xAABBCCDD11223344ull;
    CODE(LWL(T0, 0, A0), LWR(T0, 3, A0), SWL(A1, 0, A0), SWR(A1, 3, A0));
    run(4);
    CHECK_EQ(c->r[T0], 0x22334455);
    CHECK_EQ(be32(ram + 0x2000), 0x11112233);
    CHECK_EQ(be32(ram + 0x2004), 0x44667788);
}

static void test_fpu(void)
{
    printf("FPU: formato L, FR = 0, trap\n");
    setup();
    double d = 1e12 + 0.5;
    uint64_t bits; memcpy(&bits, &d, 8);
    r4k_set_fpr_l(c, 2, bits);
    r4k_set_fpr_l(c, 4, 9007199254740993ull);          /* 2^53 + 1 */
    CODE(COP1(17, 0, 2, 6, 37),                        /* cvt.l.d f6, f2 */
         COP1(17, 0, 2, 8, 10),                        /* ceil.l.d f8, f2 */
         COP1(21, 0, 4, 10, 33),                       /* cvt.d.l f10, f4 */
         COP1(17, 0, 2, 12, 9));                       /* trunc.l.d */
    run(4);
    CHECK_EQ(r4k_fpr_l(c, 6), 1000000000000ull);       /* pari piu' vicino */
    CHECK_EQ(r4k_fpr_l(c, 8), 1000000000001ull);
    memcpy(&d, &c->fpr[10], 8);
    CHECK(d == 9007199254740992.0, "cvt.d.l arrotondamento");
    CHECK(c->fcr31 & (FPX_I << 2), "cvt.d.l: manca Inexact");
    CHECK_EQ(r4k_fpr_l(c, 12), 1000000000000ull);

    /* FR = 0: i registri dispari sono la meta' alta dei pari */
    setup();
    r4k_cp0_write(c, CP0_STATUS, ST_CU1);
    c->r[T0] = 0x11111111; c->r[T1] = 0x22222222;
    CODE(MTC1(T0, 0), MTC1(T1, 1), DMFC1(T2, 0));
    run(3);
    CHECK_EQ(c->r[T2], 0x2222222211111111ull);

    /* Invalid abilitato: sqrt(-1) prende FPE e non scrive */
    setup();
    c->r[T0] = 0x800;                                  /* enable V */
    r4k_set_fpr_w(c, 2, 0xBF800000u);
    r4k_set_fpr_w(c, 4, 0x12345678u);
    CODE(CTC1(T0, 31), COP1(16, 0, 2, 4, 4));
    run(2);
    CHECK_EQ(last_exc, EXC_FPE);
    CHECK_EQ(r4k_fpr_w(c, 4), 0x12345678u);
    CHECK_EQ((c->fcr31 >> 12) & 63, FPX_V);

    /* CTC1 con una causa abilitata prende subito FPE */
    setup();
    c->r[T0] = 0x10000 | 0x800;
    CODE(CTC1(T0, 31));
    run(1);
    CHECK_EQ(last_exc, EXC_FPE);
}

static void test_cache(void)
{
    printf("CACHE: tag della secondaria, con il giro a 1 MB\n");
    setup();
    c->r[A0] = KSEG0;
    c->r[A1] = KSEG0 + (1u << 20);
    c->r[T0] = 0x12345600;
    CODE(MTC0(T0, CP0_TAGLO), CACHE(0x0B, 0x80, A0),        /* Index_Store_Tag SD */
         MTC0(ZERO, CP0_TAGLO), CACHE(0x07, 0x80, A1),      /* Index_Load_Tag SD */
         MFC0(T1, CP0_TAGLO));
    run(5);
    CHECK_EQ(c->r[T1], 0x12345600);
}

static int code_writes;
static void on_code_write(void *ctx, uint32_t pa) { (void)ctx; (void)pa; code_writes++; }

static void test_code_pages(void)
{
    printf("pagine di codice: le scritture non passano dalla via veloce\n");
    setup();
    pm.code_write = on_code_write;
    code_writes = 0;
    c->r[A0] = KSEG0 + 0x5000;
    CODE(SW(T0, 0, A0), SW(T0, 4, A0), SW(T0, 8, A0));
    run(1);                                            /* la pagina entra nella MMU veloce */
    pm.flags[5] |= PM_CODE;
    r4k_fast_flush(c);                                 /* come fara' il JIT */
    run(2);
    CHECK_EQ(code_writes, 2);
    pm.flags[5] &= ~PM_CODE;
    pm.code_write = NULL;
}

static void test_disasm(void)
{
    printf("disassemblatore\n");
    char buf[64];
    r4k_disasm(0xFFFFFFFFBFC00000ull, LUI(T0, 0xBFC0), buf, sizeof buf);
    CHECK(!strcmp(buf, "lui t0,0xbfc0"), "%s", buf);
    r4k_disasm(0xFFFFFFFFBFC00000ull, BNE(T0, ZERO, -1), buf, sizeof buf);
    CHECK(!strcmp(buf, "bne t0,zero,0xffffffffbfc00000"), "%s", buf);
    r4k_disasm(0, MTC0(T0, CP0_STATUS), buf, sizeof buf);
    CHECK(!strcmp(buf, "mtc0 t0,Status"), "%s", buf);
}

/* ---- velocita' ---- */
static void bench(void)
{
    setup();
    stop_on_exc = 0;
    /* ciclo con load, store, ALU e salto: 8 istruzioni */
    c->r[A0] = KSEG0 + 0x10000;
    CODE(LW(T0, 0, A0), ADDIU(T0, T0, 1), SW(T0, 0, A0), ADDIU(T1, T1, 3),
         DADDU(T2, T2, T1), LW(T3, 4, A0), BNE(T0, ZERO, -7), ADDIU(T4, T4, 1));
    const uint64_t n = 300000000;
    clock_t t0 = clock();
    r4k_run(c, n);
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%llu istruzioni in %.2f s: %.0f MIPS\n", (unsigned long long)n, s, n / s / 1e6);
}

int main(int argc, char **argv)
{
    ram = calloc(1, RAM_SIZE);
    if (!ram || pm_init(&pm) || r4k_init(c, &pm)) { printf("memoria\n"); return 1; }
    pm_map_ram(&pm, 0, RAM_SIZE, ram, PM_RAM);
    c->exc_hook = on_exc;

    if (argc > 1 && !strcmp(argv[1], "--bench")) { bench(); return 0; }

    test_delay_slot();
    test_exceptions();
    test_tlb();
    test_segments();
    test_interrupts();
    test_unaligned();
    test_fpu();
    test_cache();
    test_code_pages();
    test_disasm();
    printf("%d controlli, %d falliti\n", checks, failures);
    return failures != 0;
}
