/*
 * r4k_cp0.c - Coprocessore 0: registri, TLB, eccezioni, interrupt, timer,
 * istruzione CACHE.
 */
#include <stdlib.h>
#include "cpu/r4k_internal.h"

const char *const r4k_exc_names[32] = {
    "Int", "Mod", "TLBL", "TLBS", "AdEL", "AdES", "IBE", "DBE",
    "Sys", "Bp", "RI", "CpU", "Ov", "Tr", "VCEI", "FPE",
    "16", "17", "18", "19", "20", "21", "22", "WATCH",
    "24", "25", "26", "27", "28", "29", "30", "VCED"
};

/* ---- ciclo di vita ---- */

static void cache_free(R4k *c)
{
    free(c->ictag); c->ictag = NULL;
    free(c->dctag); c->dctag = NULL;
    free(c->sctag); c->sctag = NULL;
    free(c->ictag_hi); c->ictag_hi = NULL;
    free(c->dctag_hi); c->dctag_hi = NULL;
    free(c->sctag_hi); c->sctag_hi = NULL;
    free(c->icdata); c->icdata = NULL;
    free(c->dcdata); c->dcdata = NULL;
    free(c->scdata); c->scdata = NULL;
}

/* tag (e per l'R10000 TagHi e dati) delle tre cache */
static int cache_alloc(R4k *c)
{
    cache_free(c);
    c->ictag = calloc(c->icache_size / c->icache_line, sizeof *c->ictag);
    c->dctag = calloc(c->dcache_size / c->dcache_line, sizeof *c->dctag);
    c->sctag = calloc(c->scache_size / c->scache_line, sizeof *c->sctag);
    if (!c->ictag || !c->dctag || !c->sctag) return -1;
    if (c->model == R4K_R10000) {
        c->ictag_hi = calloc(c->icache_size / c->icache_line, sizeof *c->ictag_hi);
        c->dctag_hi = calloc(c->dcache_size / c->dcache_line, sizeof *c->dctag_hi);
        c->sctag_hi = calloc(c->scache_size / c->scache_line, sizeof *c->sctag_hi);
        c->icdata = calloc(c->icache_size / 2, sizeof *c->icdata);    /* dati ed ECC ogni 4 byte */
        c->dcdata = calloc(c->dcache_size / 2, sizeof *c->dcdata);
        c->scdata = calloc(c->scache_size / 2, sizeof *c->scdata);
        if (!c->ictag_hi || !c->dctag_hi || !c->sctag_hi || !c->icdata || !c->dcdata || !c->scdata)
            return -1;
    }
    return 0;
}

static void model_params(R4k *c, int model)
{
    c->model = model;
    if (model == R4K_R10000) {
        /* primarie da 32 KB a due vie (righe da 64 byte istruzioni, 32 dati),
           secondaria da 1 MB a due vie con righe da 128 byte */
        c->mips4 = 1;
        c->ntlb = 64;
        c->vpn2_mask = 0xC00007FFFFFFE000ull;          /* R (63:62), VPN2 (43:13) */
        c->pfn_mask = 0x00000003FFFFFFFEull;           /* PFN (33:6), C, D, V */
        c->pa_mask = 0x000000FFFFFFFFFFull;            /* 40 bit */
        c->seg_limit = 1ull << 44;
        c->icache_size = c->dcache_size = 32 * 1024;
        c->icache_line = 64; c->dcache_line = 32;
        c->scache_size = 1024 * 1024; c->scache_line = 128;
        c->cache_ways = 2;
    } else {
        /* R4400SC dell'Indy: primarie 16 KB con righe da 16 byte,
           secondaria unificata da 1 MB con righe da 128 byte */
        c->mips4 = 0;
        c->ntlb = 48;
        c->vpn2_mask = 0xC00000FFFFFFE000ull;          /* R (63:62), VPN2 (39:13) */
        c->pfn_mask = 0x3FFFFFFEull;
        c->pa_mask = 0x0000000FFFFFFFFFull;            /* 36 bit */
        c->seg_limit = 1ull << 40;
        c->icache_size = c->dcache_size = 16 * 1024;
        c->icache_line = c->dcache_line = 16;
        c->scache_size = 1024 * 1024; c->scache_line = 128;
        c->cache_ways = 1;
    }
}

int r4k_init(R4k *c, PhysMem *pm)
{
    memset(c, 0, sizeof *c);
    c->pm = pm;
    c->count_shift = 1;
    return r4k_set_model(c, R4K_R4400);
}

int r4k_set_model(R4k *c, int model)
{
    model_params(c, model);
    if (cache_alloc(c)) { r4k_free(c); return -1; }
    r4k_reset(c);
    return 0;
}

void r4k_free(R4k *c)
{
    r4k_jit_free(c);
    cache_free(c);
}

static void tlb_set(const R4k *c, R4kTlb *e, uint64_t mask, uint64_t hi, uint64_t lo0, uint64_t lo1)
{
    e->mask = mask & 0x01FFE000u;
    e->hi = hi & (c->vpn2_mask | 0xFF);
    e->g = (uint8_t)(lo0 & lo1 & 1);
    e->lo[0] = lo0 & c->pfn_mask;
    e->lo[1] = lo1 & c->pfn_mask;
    e->asid = (uint8_t)e->hi;
    e->cmp = c->vpn2_mask & ~e->mask;
    e->vpn = e->hi & e->cmp;
    e->odd = ((e->mask | 0x1FFF) + 1) >> 1;
}

void r4k_reset(R4k *c)
{
    memset(c->r, 0, sizeof c->r);
    c->hi = c->lo = 0;
    memset(c->cp0, 0, sizeof c->cp0);
    for (int i = 0; i < R4K_TLB_MAX; i++) {
        tlb_set(c, &c->tlb[i], 0, 0, 0, 0);
        c->tlb[i].cmp = 0; c->tlb[i].vpn = 1; c->tlb[i].odd = 0;   /* non coincide con niente */
    }
    c->cp0[CP0_STATUS] = ST_BEV | ST_ERL;
    if (c->model == R4K_R10000) {
        c->cp0[CP0_PRID] = 0x0934;              /* R10000, revisione 3.4 */
        /* Config: IC = DC = 32 KB, secondaria 1 MB (SS = 1) con righe da
           32 parole (SB), K0 = 3 */
        c->cp0[CP0_CONFIG] = 0x6C012003;
        c->fcr0 = 0x0900;                       /* FPU dell'R10000 */
    } else {
        c->cp0[CP0_PRID] = 0x0460;              /* R4400, revisione 6.0 */
        /* Config: SB = 32 parole, secondaria presente, big-endian,
           IC = DC = 16 KB, righe da 16 byte, K0 = 3 (cacheable) */
        c->cp0[CP0_CONFIG] = 0x00C08483;
        c->fcr0 = 0x0500;                       /* FPU R4000/R4400 */
    }
    c->fcr31 = 0;
    c->pc = 0xFFFFFFFFBFC00000ull;
    c->branch_pending = c->in_ds = c->pc_set = 0;
    c->ll_bit = 0;
    c->ip_ext = c->ip_timer = 0;
    c->count_base = 0; c->count_icount = c->icount;
    c->rnd_state = 0x2545F491u;
    r4k_update_mode(c);
    r4k_fast_flush(c);
    r4k_update_timer(c);
}

/* ---- modo e interrupt ---- */

void r4k_update_mode(R4k *c)
{
    uint32_t st = (uint32_t)c->cp0[CP0_STATUS];
    if (st & (ST_EXL | ST_ERL)) c->mode = R4K_MODE_KERNEL;
    else switch ((st >> 3) & 3) {
        case 0:  c->mode = R4K_MODE_KERNEL; break;
        case 1:  c->mode = R4K_MODE_SUPER; break;
        default: c->mode = R4K_MODE_USER; break;
    }
    c->allow64 = c->mode == R4K_MODE_KERNEL ? 1
               : c->mode == R4K_MODE_SUPER ? (st & ST_SX) != 0 : (st & ST_UX) != 0;
}

void r4k_update_ip(R4k *c)
{
    uint64_t cause = c->cp0[CP0_CAUSE] & ~(uint64_t)0xFC00;
    cause |= c->ip_ext | (c->ip_timer ? 0x8000u : 0);
    c->cp0[CP0_CAUSE] = cause;
    if (r4k_irq_ready(c)) r4k_kick(c);
}

void r4k_set_irq(R4k *c, int line, int level)
{
    uint32_t bit = 0x400u << line;
    if (level) c->ip_ext |= bit; else c->ip_ext &= ~bit;
    r4k_update_ip(c);
}

/* ---- Count / Compare, a istruzioni ---- */

static uint32_t count_now(const R4k *c)
{
    return (uint32_t)(c->count_base + ((c->icount - c->count_icount) >> c->count_shift));
}

void r4k_update_timer(R4k *c)
{
    uint64_t d = (uint32_t)((uint32_t)c->cp0[CP0_COMPARE] - count_now(c));
    if (!d) d = 1ull << 32;
    uint64_t elapsed = (c->icount - c->count_icount) >> c->count_shift;
    c->timer_event = c->count_icount + ((elapsed + d) << c->count_shift);
    r4k_kick(c);
}

void r4k_timer_fire(R4k *c)
{
    c->ip_timer = 1;
    r4k_update_ip(c);
    r4k_update_timer(c);
}

/* ---- eccezioni ---- */

void r4k_exception_at(R4k *c, int code, int ce, uint32_t offset)
{
    uint64_t st = c->cp0[CP0_STATUS];
    uint64_t cause = c->cp0[CP0_CAUSE];
    if (!(st & ST_EXL)) {
        if (c->in_ds) { c->cp0[CP0_EPC] = c->pc - 4; cause |= CAUSE_BD; }
        else          { c->cp0[CP0_EPC] = c->pc;     cause &= ~(uint64_t)CAUSE_BD; }
    }
    cause = (cause & ~(uint64_t)(CAUSE_EXC | 0x30000000u)) | ((uint32_t)code << 2) | ((uint32_t)ce << CAUSE_CE_SHIFT);
    c->cp0[CP0_CAUSE] = SX32(cause);
    c->cp0[CP0_STATUS] = st | ST_EXL;
    uint64_t base = (st & ST_BEV) ? 0xFFFFFFFFBFC00200ull : 0xFFFFFFFF80000000ull;
    c->pc = base + offset;
    c->branch_pending = 0;
    c->pc_set = 1;
    r4k_update_mode(c);
    c->exc_count[code & 31]++;
    if (c->exc_hook) c->exc_hook(c, code, c->hook_user);
}

void r4k_exception(R4k *c, int code, int ce)
{
    r4k_exception_at(c, code, ce, 0x180);
}

/* ---- registri ---- */

static uint32_t tlb_random(const R4k *c)
{
    uint32_t wired = (uint32_t)c->cp0[CP0_WIRED] & 63;
    uint32_t n = (uint32_t)c->ntlb;
    if (wired >= n - 1) return n - 1;
    return wired + c->rnd_state % (n - wired);
}

uint64_t r4k_cp0_read(R4k *c, int reg)
{
    switch (reg) {
    case CP0_RANDOM: return tlb_random(c);
    case CP0_COUNT:  return SX32(count_now(c));
    default:         return c->cp0[reg & 31];
    }
}

void r4k_cp0_write(R4k *c, int reg, uint64_t v)
{
    uint64_t old = c->cp0[reg & 31];
    switch (reg) {
    case CP0_INDEX:    c->cp0[reg] = SX32((old & 0x80000000u) | (v & 0x3F)); break;
    case CP0_ENTRYLO0:
    case CP0_ENTRYLO1: c->cp0[reg] = v & (c->pfn_mask | 1); break;
    case CP0_CONTEXT:  c->cp0[reg] = (old & 0x7FFFF0u) | (v & ~(uint64_t)0x7FFFFF); break;
    case CP0_PAGEMASK: c->cp0[reg] = v & 0x01FFE000u; break;
    case CP0_WIRED:    c->cp0[reg] = v & 0x3F; break;
    case CP0_COUNT:
        c->count_base = (uint32_t)v;
        c->count_icount = c->icount;
        r4k_update_timer(c);
        break;
    case CP0_ENTRYHI: {
        uint64_t nv = v & (c->vpn2_mask | 0xFF);
        c->cp0[reg] = nv;
        if ((nv ^ old) & 0xFF) r4k_fast_flush(c);
        break;
    }
    case CP0_COMPARE:
        c->cp0[reg] = SX32(v);
        c->ip_timer = 0;
        r4k_update_ip(c);
        r4k_update_timer(c);
        break;
    case CP0_STATUS: {
        const uint32_t mask = 0xFFD7FFFFu;             /* TS e il bit 19 sono di sola lettura */
        uint64_t nv = SX32(((uint32_t)old & ~mask) | ((uint32_t)v & mask));
        c->cp0[reg] = nv;
        if ((nv ^ old) & (ST_ERL | ST_KX | ST_SX | ST_UX)) r4k_fast_flush(c);
        r4k_update_mode(c);
        r4k_update_ip(c);
        r4k_kick(c);
        break;
    }
    case CP0_CAUSE:
        c->cp0[reg] = (old & ~(uint64_t)0x300) | (v & 0x300);
        r4k_update_ip(c);
        break;
    case CP0_CONFIG: {                                 /* K0 (R4400: anche CU) */
        uint64_t m = c->model == R4K_R10000 ? 0x7 : 0xF;
        c->cp0[reg] = (old & ~m) | (v & m);
        break;
    }
    case CP0_XCONTEXT: c->cp0[reg] = (old & 0x1FFFFFFFFull) | (v & ~0x1FFFFFFFFull); break;
    case CP0_ECC:      c->cp0[reg] = v & (c->model == R4K_R10000 ? 0x3FF : 0xFF); break;
    case CP0_EPC:
    case CP0_ERROREPC:
    case CP0_LLADDR:   c->cp0[reg] = v; break;
    case CP0_WATCHLO:
    case CP0_WATCHHI:
    case CP0_TAGLO:
    case CP0_TAGHI:    c->cp0[reg] = SX32(v); break;
    case 21: case 22: case 25:                         /* R10000: FrameMask, Diagnostic, contatori */
        if (c->model == R4K_R10000) c->cp0[reg] = v;
        break;
    default: break;                                    /* sola lettura o riservati */
    }
}

/* ---- istruzioni TLB ---- */

static void tlb_write(R4k *c, int idx)
{
    R4kTlb *e = &c->tlb[idx % c->ntlb];
    r4k_fast_flush_tlb_entry(c, e);                    /* la mappatura vecchia */
    tlb_set(c, e,c->cp0[CP0_PAGEMASK], c->cp0[CP0_ENTRYHI],
            c->cp0[CP0_ENTRYLO0], c->cp0[CP0_ENTRYLO1]);
    r4k_fast_flush_tlb_entry(c, e);                    /* eventuali doppioni */
}

static void tlb_read(R4k *c)
{
    const R4kTlb *e = &c->tlb[(c->cp0[CP0_INDEX] & 63) % c->ntlb];
    uint64_t old_asid = c->cp0[CP0_ENTRYHI] & 0xFF;
    c->cp0[CP0_PAGEMASK] = e->mask;
    c->cp0[CP0_ENTRYHI] = e->hi;
    c->cp0[CP0_ENTRYLO0] = e->lo[0] | e->g;
    c->cp0[CP0_ENTRYLO1] = e->lo[1] | e->g;
    if (old_asid != (e->hi & 0xFF)) r4k_fast_flush(c);
}

static void tlb_probe(R4k *c)
{
    uint64_t hi = c->cp0[CP0_ENTRYHI];
    uint8_t asid = (uint8_t)hi;
    for (int i = 0; i < c->ntlb; i++) {
        const R4kTlb *e = &c->tlb[i];
        if ((hi & e->cmp) == e->vpn && (e->g || e->asid == asid)) {
            c->cp0[CP0_INDEX] = (uint64_t)i;
            return;
        }
    }
    c->cp0[CP0_INDEX] = SX32((uint32_t)c->cp0[CP0_INDEX] | 0x80000000u);
}

static void eret(R4k *c)
{
    if (c->cp0[CP0_STATUS] & ST_ERL) {
        c->pc = c->cp0[CP0_ERROREPC];
        c->cp0[CP0_STATUS] &= ~(uint64_t)ST_ERL;
        r4k_fast_flush(c);                              /* kuseg torna mappato */
    } else {
        c->pc = c->cp0[CP0_EPC];
        c->cp0[CP0_STATUS] &= ~(uint64_t)ST_EXL;
    }
    c->ll_bit = 0;
    c->pc_set = 1;
    r4k_update_mode(c);
    r4k_kick(c);
}

void r4k_cop0(R4k *c, uint32_t insn)
{
    if (!r4k_cp0_usable(c)) { r4k_exception(c, EXC_CPU, 0); return; }
    int rt = (insn >> 16) & 31, rd = (insn >> 11) & 31;
    switch ((insn >> 21) & 31) {
    case 0:  c->r[rt] = SX32(r4k_cp0_read(c, rd)); return;              /* MFC0 */
    case 1:                                                              /* DMFC0 */
        if (!c->allow64) break;
        c->r[rt] = r4k_cp0_read(c, rd);
        return;
    case 4:  r4k_cp0_write(c, rd, SX32(c->r[rt])); return;             /* MTC0 */
    case 5:                                                              /* DMTC0 */
        if (!c->allow64) break;
        r4k_cp0_write(c, rd, c->r[rt]);
        return;
    default:
        if (!(insn & (1u << 25))) break;
        switch (insn & 0x3F) {
        case 0x01: tlb_read(c); return;
        case 0x02: tlb_write(c, (int)(c->cp0[CP0_INDEX] & 63)); return;
        case 0x06:
            tlb_write(c, (int)tlb_random(c));
            c->rnd_state ^= c->rnd_state << 13;
            c->rnd_state ^= c->rnd_state >> 17;
            c->rnd_state ^= c->rnd_state << 5;
            return;
        case 0x08: tlb_probe(c); return;
        case 0x18: eret(c); return;
        default: break;
        }
    }
    r4k_exception(c, EXC_RI, 0);
}

/* ---- CACHE: si tengono solo i tag, per le istruzioni sugli indici ---- */

/* R10000: cache a due vie, la via e' il bit 0 dell'indirizzo per le
   operazioni sugli indici. Si tengono TagLo/TagHi di ogni riga e, ogni 4
   byte, TagHi:TagLo e il registro ECC (10 bit) per Index Load/Store Data,
   cosi' i test della PROM rileggono quello che hanno scritto. */
static void r10k_cache_op(R4k *c, uint64_t va, int op, int which)
{
    uint32_t *tags, *tags_hi, size, line;
    uint64_t *data;
    switch (which) {
    case 0:  tags = c->ictag; tags_hi = c->ictag_hi; data = c->icdata; size = c->icache_size; line = c->icache_line; break;
    case 1:  tags = c->dctag; tags_hi = c->dctag_hi; data = c->dcdata; size = c->dcache_size; line = c->dcache_line; break;
    case 3:  tags = c->sctag; tags_hi = c->sctag_hi; data = c->scdata; size = c->scache_size; line = c->scache_line; break;
    default: return;                                               /* cache 2: riservata */
    }
    uint32_t way = (uint32_t)va & 1, off = (uint32_t)va & (size / 2 - 1);
    uint32_t idx = off / line * 2 + way, word = (off >> 2) * 2 + way;
    switch (op) {
    case 0: tags[idx] = tags_hi[idx] = 0; break;                   /* Index (WB) Invalidate */
    case 1:                                                        /* Index Load Tag */
        c->cp0[CP0_TAGLO] = tags[idx];
        c->cp0[CP0_TAGHI] = tags_hi[idx];
        break;
    case 2:                                                        /* Index Store Tag */
        tags[idx] = (uint32_t)c->cp0[CP0_TAGLO];
        tags_hi[idx] = (uint32_t)c->cp0[CP0_TAGHI];
        break;
    case 6:                                                        /* Index Load Data */
        c->cp0[CP0_TAGLO] = (uint32_t)data[2 * word];
        c->cp0[CP0_TAGHI] = (uint32_t)(data[2 * word] >> 32);
        c->cp0[CP0_ECC] = data[2 * word + 1];
        break;
    case 7:                                                        /* Index Store Data */
        data[2 * word] = (c->cp0[CP0_TAGHI] & 0xFFFFFFFFu) << 32 | (c->cp0[CP0_TAGLO] & 0xFFFFFFFFu);
        data[2 * word + 1] = c->cp0[CP0_ECC];
        break;
    case 4: case 5:                                                /* Hit_*, Cache Barrier */
        if (op == 5 && which == 0) break;
        {
            uint64_t pa;
            r4k_translate(c, va, ACC_LOAD, &pa);                   /* solo per le eccezioni */
        }
        break;
    default: break;
    }
}

void r4k_cache_op(R4k *c, uint32_t insn)
{
    if (!r4k_cp0_usable(c)) { r4k_exception(c, EXC_CPU, 0); return; }
    uint64_t va = c->r[(insn >> 21) & 31] + (uint64_t)(int64_t)(int16_t)insn;
    if (c->model == R4K_R10000) { r10k_cache_op(c, va, (insn >> 18) & 7, (insn >> 16) & 3); return; }
    int op = (insn >> 18) & 7, which = (insn >> 16) & 3;
    uint32_t *tags, size, line;
    switch (which) {
    case 0:  tags = c->ictag; size = c->icache_size; line = c->icache_line; break;
    case 1:  tags = c->dctag; size = c->dcache_size; line = c->dcache_line; break;
    default: tags = c->sctag; size = c->scache_size; line = c->scache_line; break;
    }
    uint32_t idx = (uint32_t)(va & (size - 1)) / line;
    switch (op) {
    case 0: tags[idx] = 0; break;                                  /* Index_Invalidate / WB_Inv */
    case 1:                                                        /* Index_Load_Tag */
        c->cp0[CP0_TAGLO] = SX32(tags[idx]);
        c->cp0[CP0_TAGHI] = 0;
        break;
    case 2: tags[idx] = (uint32_t)c->cp0[CP0_TAGLO]; break;        /* Index_Store_Tag */
    default: {                                                     /* Hit_*, Create_Dirty, Fill */
        uint64_t pa;
        r4k_translate(c, va, ACC_LOAD, &pa);                       /* solo per le eccezioni */
        break;
    }
    }
}
