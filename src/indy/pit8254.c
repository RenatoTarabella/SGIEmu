/*
 * pit8254.c - Timer 8254 calcolato dal tempo
 */
#include <string.h>
#include "indy/pit8254.h"

#define NS_PER_TICK2 1000u                      /* contatore 2 a 1 MHz */

void pit_reset(Pit8254 *p)
{
    memset(p, 0, sizeof *p);
    for (int k = 0; k < 3; k++) { p->ch[k].reload = 65536; p->ch[k].rw = 3; p->ch[k].wlow = p->ch[k].rlow = 1; }
}

static int periodic(const Pit8254Ch *c) { return c->mode == 2 || c->mode == 3; }

/* tick del clock del canale k al tempo now */
static uint64_t ticks(const Pit8254 *p, int k, uint64_t now_ns)
{
    uint64_t t2 = now_ns / NS_PER_TICK2;
    if (k == 2) return t2;
    const Pit8254Ch *c2 = &p->ch[2];
    if (!c2->running || !periodic(c2) || t2 < c2->load_tick) return 0;
    return (t2 - c2->load_tick) / c2->reload;
}

static uint64_t elapsed(const Pit8254 *p, int k, uint64_t now_ns)
{
    uint64_t t = ticks(p, k, now_ns);
    return t > p->ch[k].load_tick ? t - p->ch[k].load_tick : 0;
}

static uint16_t value(const Pit8254 *p, int k, uint64_t now_ns)
{
    const Pit8254Ch *c = &p->ch[k];
    if (!c->running) return (uint16_t)c->reload;
    uint64_t e = elapsed(p, k, now_ns);
    if (periodic(c)) return (uint16_t)(c->reload - e % c->reload);
    return (uint16_t)(c->reload - e);                           /* e poi continua da 0xFFFF */
}

static void load(Pit8254 *p, int k, uint32_t n, uint64_t now_ns)
{
    Pit8254Ch *c = &p->ch[k];
    c->reload = n ? n : 65536;
    c->running = 1;
    c->fired = 0;
    c->load_tick = 0;
    c->load_tick = ticks(p, k, now_ns);
}

uint8_t pit_read(Pit8254 *p, int port, uint64_t now_ns)
{
    if (port == 3) return 0xFF;
    Pit8254Ch *c = &p->ch[port];
    uint16_t v;
    if (c->latched) v = c->latch; else v = value(p, port, now_ns);
    uint8_t out;
    switch (c->rw) {
    case 1: out = (uint8_t)v; c->latched = 0; break;
    case 2: out = (uint8_t)(v >> 8); c->latched = 0; break;
    default:
        out = c->rlow ? (uint8_t)v : (uint8_t)(v >> 8);
        if (!c->rlow && c->latched) c->latched = 0;
        c->rlow = !c->rlow;
        break;
    }
    return out;
}

void pit_write(Pit8254 *p, int port, uint8_t v, uint64_t now_ns)
{
    if (port == 3) {
        int k = v >> 6;
        if (k == 3) {                                         /* read-back: latch dei conteggi */
            for (int j = 0; j < 3; j++)
                if ((v & (2 << j)) && !(v & 0x20)) {
                    p->ch[j].latch = value(p, j, now_ns);
                    p->ch[j].latched = 1;
                    p->ch[j].rlow = 1;
                }
            return;
        }
        Pit8254Ch *c = &p->ch[k];
        int rw = (v >> 4) & 3;
        if (rw == 0) {                                        /* latch */
            c->latch = value(p, k, now_ns);
            c->latched = 1;
            c->rlow = 1;
            return;
        }
        c->rw = rw;
        c->mode = (v >> 1) & 7;
        if (c->mode > 5) c->mode -= 4;
        c->bcd = v & 1;
        c->running = 0;
        c->wlow = c->rlow = 1;
        c->latched = 0;
        return;
    }
    Pit8254Ch *c = &p->ch[port];
    switch (c->rw) {
    case 1: load(p, port, v, now_ns); break;
    case 2: load(p, port, (uint32_t)v << 8, now_ns); break;
    default:
        if (c->wlow) { c->low_written = v; c->wlow = 0; }
        else { load(p, port, c->low_written | ((uint32_t)v << 8), now_ns); c->wlow = 1; }
        break;
    }
}

uint64_t pit_poll(Pit8254 *p, int k, uint64_t now_ns)
{
    Pit8254Ch *c = &p->ch[k];
    if (!c->running) return 0;
    uint64_t e = elapsed(p, k, now_ns);
    uint64_t total = periodic(c) ? e / c->reload : (e >= c->reload ? 1 : 0);
    uint64_t n = total > c->fired ? total - c->fired : 0;
    c->fired = total;
    return n;
}

uint64_t pit_next_ns(const Pit8254 *p, int k, uint64_t now_ns)
{
    const Pit8254Ch *c = &p->ch[k];
    if (!c->running) return UINT64_MAX;
    if (!periodic(c) && c->fired) return UINT64_MAX;
    /* scadenza in tick del canale, poi in ns */
    uint64_t e = elapsed(p, k, now_ns);
    uint64_t left = periodic(c) ? c->reload - e % c->reload : (e < c->reload ? c->reload - e : 1);
    uint64_t per_tick = NS_PER_TICK2;
    if (k != 2) {
        const Pit8254Ch *c2 = &p->ch[2];
        if (!c2->running || !periodic(c2)) return UINT64_MAX;
        per_tick = (uint64_t)NS_PER_TICK2 * c2->reload;
    }
    return left * per_tick;
}
