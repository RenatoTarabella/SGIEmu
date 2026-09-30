/*
 * r4k.h - Modulo CPU: MIPS R4400 (MIPS III, 64 bit, big-endian)
 *
 * Interprete con CP0 (TLB a 48 voci, eccezioni precise con delay slot,
 * Count/Compare), FPU (CP1) e istruzione CACHE con i soli tag.
 *
 * Due scelte pensate per il JIT che verra' dopo:
 *
 *  - MMU veloce: per ogni modo (kernel, supervisor, utente) una tabella a
 *    mappatura diretta indirizzo virtuale -> puntatore host, come la softmmu
 *    di QEMU. Il tag contiene la pagina virtuale; confrontandolo con
 *    va & ~(0xFFF & ~(size-1)) si controllano insieme pagina e allineamento.
 *    Ci entrano solo pagine di RAM/ROM; l'I/O passa sempre dalla via lenta.
 *    Si invalida quando cambia la mappatura (TLBWI/TLBWR, ASID, bit di
 *    Status che cambiano gli spazi di indirizzi), non al cambio di modo.
 *
 *  - Codice indicizzato per indirizzo fisico: la traduzione va sempre fino
 *    all'indirizzo fisico e la memoria fisica (physmem) marca le pagine con
 *    codice tradotto (PM_CODE). Le scritture su quelle pagine prendono la
 *    via lenta: la cache dei blocchi sopravvive ai cambi di TLB e di ASID.
 *
 * La CPU non conosce la macchina: vede solo PhysMem e le linee di
 * interrupt (r4k_set_irq).
 */
#ifndef R4K_H
#define R4K_H

#include <stdint.h>
#include "mem/physmem.h"

#define R4K_TLB_ENTRIES   48
#define R4K_FAST_BITS     10
#define R4K_FAST_SIZE     (1u << R4K_FAST_BITS)
#define R4K_FAST_INVALID  (~(uint64_t)0)

enum { R4K_MODE_KERNEL = 0, R4K_MODE_SUPER = 1, R4K_MODE_USER = 2 };

/* ---- registri del CP0 ---- */
enum {
    CP0_INDEX = 0, CP0_RANDOM, CP0_ENTRYLO0, CP0_ENTRYLO1, CP0_CONTEXT,
    CP0_PAGEMASK, CP0_WIRED, CP0_BADVADDR = 8, CP0_COUNT, CP0_ENTRYHI,
    CP0_COMPARE, CP0_STATUS, CP0_CAUSE, CP0_EPC, CP0_PRID, CP0_CONFIG,
    CP0_LLADDR, CP0_WATCHLO, CP0_WATCHHI, CP0_XCONTEXT, CP0_ECC = 26,
    CP0_CACHEERR, CP0_TAGLO, CP0_TAGHI, CP0_ERROREPC
};

/* ---- Status ---- */
#define ST_IE    0x00000001u
#define ST_EXL   0x00000002u
#define ST_ERL   0x00000004u
#define ST_KSU   0x00000018u
#define ST_UX    0x00000020u
#define ST_SX    0x00000040u
#define ST_KX    0x00000080u
#define ST_IM    0x0000FF00u
#define ST_DE    0x00010000u
#define ST_CE    0x00020000u
#define ST_CH    0x00040000u
#define ST_SR    0x00100000u
#define ST_TS    0x00200000u
#define ST_BEV   0x00400000u
#define ST_RE    0x02000000u
#define ST_FR    0x04000000u
#define ST_RP    0x08000000u
#define ST_CU0   0x10000000u
#define ST_CU1   0x20000000u

/* ---- Cause ---- */
#define CAUSE_BD      0x80000000u
#define CAUSE_CE_SHIFT 28
#define CAUSE_IP      0x0000FF00u
#define CAUSE_EXC     0x0000007Cu

/* codici delle eccezioni (Cause.ExcCode) */
enum {
    EXC_INT = 0, EXC_MOD = 1, EXC_TLBL = 2, EXC_TLBS = 3, EXC_ADEL = 4,
    EXC_ADES = 5, EXC_IBE = 6, EXC_DBE = 7, EXC_SYS = 8, EXC_BP = 9,
    EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12, EXC_TR = 13, EXC_VCEI = 14,
    EXC_FPE = 15, EXC_WATCH = 23, EXC_VCED = 31
};

/* ---- FCSR (FCR31) ---- */
#define FCSR_RM       0x00000003u
#define FCSR_FLAGS    0x0000007Cu
#define FCSR_ENABLES  0x00000F80u
#define FCSR_CAUSE    0x0003F000u
#define FCSR_C        0x00800000u
#define FCSR_FS       0x01000000u
#define FCSR_MASK     0x0183FFFFu
/* bit dei singoli eccezioni FPU, nella posizione dei flag (>> 2) */
enum { FPX_I = 1, FPX_U = 2, FPX_O = 4, FPX_Z = 8, FPX_V = 16, FPX_E = 32 };

typedef struct R4kTlb {
    uint64_t mask;      /* PageMask (bit 24:13)                         */
    uint64_t hi;        /* EntryHi: R, VPN2, ASID                       */
    uint64_t lo[2];     /* EntryLo0/1 senza G                           */
    /* precalcolati */
    uint64_t cmp;       /* bit dell'indirizzo che contano nel confronto */
    uint64_t vpn;       /* hi & cmp                                     */
    uint64_t odd;       /* bit che sceglie tra pagina pari e dispari    */
    uint8_t  asid, g;
} R4kTlb;

/* Voce della MMU veloce: host = va + addend */
typedef struct R4kFast {
    uint64_t  tag_r, tag_w, tag_x;
    uintptr_t addend;
} R4kFast;

struct R4k;
struct R4kJit;
typedef void (*R4kExcHook)(struct R4k *c, int code, void *user);

typedef struct R4k {
    /* ---- stato dell'interprete, in testa per il JIT ---- */
    uint64_t r[32];
    uint64_t hi, lo;
    uint64_t pc;               /* istruzione in esecuzione            */
    uint64_t branch_target;    /* dove andare dopo il delay slot      */
    int      branch_pending;   /* la prossima e' un delay slot        */
    int      in_ds;            /* l'istruzione corrente e' in un delay slot */
    int      pc_set;           /* il pc e' gia' stato scritto (eccezione, ERET, likely) */
    int      mode;             /* R4K_MODE_*: tabella della MMU veloce */
    int      allow64;          /* istruzioni a 64 bit permesse        */
    int      ll_bit;

    uint64_t icount;           /* istruzioni eseguite                 */
    uint64_t limit;            /* il ciclo interno si ferma qui       */
    uint64_t run_end;
    int      stop;

    /* ---- FPU ---- */
    uint64_t fpr[32];          /* con FR=0 i registri dispari sono la meta' alta dei pari */
    uint32_t fcr31, fcr0;

    /* ---- CP0 ---- */
    uint64_t cp0[32];
    R4kTlb   tlb[R4K_TLB_ENTRIES];
    uint32_t ip_ext;           /* linee di interrupt esterne, gia' in posizione IP2..IP7 */
    uint32_t ip_timer;         /* Count == Compare (IP7)              */
    uint64_t count_base, count_icount, timer_event;
    int      count_shift;      /* Count avanza di 1 ogni 2^count_shift istruzioni */
    uint32_t rnd_state;

    /* tag delle cache, per le istruzioni CACHE sugli indici */
    uint32_t *ictag, *dctag, *sctag;
    uint32_t icache_size, dcache_size, scache_size;
    uint32_t icache_line, dcache_line, scache_line;

    PhysMem *pm;

    /* chiamata a ogni eccezione presa (tracce, oracolo dei test) */
    R4kExcHook exc_hook;
    void      *hook_user;

    struct R4kJit *jit;        /* NULL = solo interprete              */
    int      jit_exit;         /* il blocco in esecuzione deve uscire (codice riscritto) */
    void    *jit_last;         /* ultima uscita concatenabile presa (JitExit) */
    uint32_t jit_mxcsr;        /* MXCSR dopo un'operazione FPU tradotta */
    uint32_t jit_gen;          /* cambia quando si invalidano blocchi (JitRet) */

    uint64_t exc_count[32];
    uint64_t flushes;                   /* svuotamenti completi della MMU veloce */

    /* ---- MMU veloce: 3 x 1024 voci ---- */
    R4kFast fast[3][R4K_FAST_SIZE];
} R4k;

/* ciclo di vita */
int  r4k_init(R4k *c, PhysMem *pm);
void r4k_free(R4k *c);
void r4k_reset(R4k *c);                 /* reset a freddo: pc = 0xBFC00000 */

/* Esegue fino a n istruzioni; ritorna quante ne ha eseguite. */
uint64_t r4k_run(R4k *c, uint64_t n);
void r4k_step(R4k *c);                  /* una sola istruzione (o un interrupt) */
void r4k_stop(R4k *c);                  /* da una callback: esci da r4k_run */

/* linee di interrupt esterne 0..5 = IP2..IP7 */
void r4k_set_irq(R4k *c, int line, int level);

/* accesso al CP0 come MFC0/DMTC0 (usato anche dai test e dai tool) */
uint64_t r4k_cp0_read(R4k *c, int reg);
void     r4k_cp0_write(R4k *c, int reg, uint64_t v);

/* traduzione senza eccezioni (debugger, tool): 1 = ok */
int  r4k_translate_debug(R4k *c, uint64_t va, int store, uint64_t *pa);
/* lettura/scrittura virtuale senza eccezioni (tool): 1 = ok */
int  r4k_peek(R4k *c, uint64_t va, int size, uint64_t *val);

/* invalidazione della MMU veloce (es. dopo aver cambiato la mappa fisica) */
void r4k_fast_flush(R4k *c);
/* mette la pagina di va nella MMU veloce senza eccezioni (test del JIT: le
   load/store tradotte passano dalla via veloce solo se c'e'): 1 = fatto */
int  r4k_fast_prefill(R4k *c, uint64_t va);

/* JIT verso x86-64 (r4k_jit.c). Con il JIT attivo r4k_run esegue blocchi
   tradotti, ma esattamente le stesse istruzioni dell'interprete: un blocco
   parte solo se finisce prima del limite, e le eccezioni, gli interrupt
   chiesti da un accesso e il codice riscritto lo fanno uscire subito.
   max_block = istruzioni massime per blocco (0 = default). */
int  r4k_jit_init(R4k *c, int max_block);
void r4k_jit_free(R4k *c);
void r4k_jit_flush(R4k *c);
void r4k_jit_print_stats(const R4k *c);
/* per i profiler: 1 = host_pc e' nel blocco che inizia a *va, 2 = nel
   codice del JIT ma fuori dai blocchi, 0 = altrove */
int  r4k_jit_owner(const R4k *c, const void *host_pc, uint64_t *va);

/* registri FPU come li vede il programma (FR decide la disposizione) */
uint32_t r4k_fpr_w(const R4k *c, int n);
uint64_t r4k_fpr_l(const R4k *c, int n);
void     r4k_set_fpr_w(R4k *c, int n, uint32_t v);
void     r4k_set_fpr_l(R4k *c, int n, uint64_t v);

extern const char *const r4k_exc_names[32];

#endif
