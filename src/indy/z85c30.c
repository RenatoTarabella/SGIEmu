/*
 * z85c30.c - Zilog 85C30 a livello di carattere
 */
#include <string.h>
#include "indy/z85c30.h"

static void reset_ch(Z85c30Ch *c)
{
    uint8_t wr2 = c->wr[2], wr9 = c->wr[9];
    memset(c->wr, 0, sizeof c->wr);
    c->wr[2] = wr2; c->wr[9] = wr9;
    c->tx_ip = c->rx_ip = c->ext_ip = 0;
}

void scc_reset(Z85c30 *s)
{
    void (*tx)(void *, int, uint8_t) = s->tx;
    void *user = s->user;
    memset(s, 0, sizeof *s);
    s->tx = tx; s->user = user;
}

static int rx_avail(const Z85c30Ch *c) { return c->rx_head != c->rx_tail; }

int scc_rx_room(const Z85c30 *s, int ch)
{
    const Z85c30Ch *c = &s->ch[ch];
    return SCC_RXQ - 1 - ((c->rx_head - c->rx_tail + SCC_RXQ) % SCC_RXQ);
}

void scc_rx(Z85c30 *s, int ch, uint8_t v)
{
    Z85c30Ch *c = &s->ch[ch];
    if (!scc_rx_room(s, ch)) return;
    c->rxq[c->rx_head] = v;
    c->rx_head = (c->rx_head + 1) % SCC_RXQ;
}

/* bit di RR3 (dal punto di vista del canale A) */
static uint8_t pending(const Z85c30 *s)
{
    uint8_t r = 0;
    for (int k = 0; k < 2; k++) {
        const Z85c30Ch *c = &s->ch[k];
        int rxmode = (c->wr[1] >> 3) & 3;
        int shift = k == 0 ? 3 : 0;
        if (c->ext_ip && (c->wr[1] & 1)) r |= 1 << shift;
        if (c->tx_ip && (c->wr[1] & 2)) r |= 2 << shift;
        if (rxmode && rx_avail(c)) r |= 4 << shift;
    }
    return r;
}

int scc_irq(const Z85c30 *s)
{
    return (s->ch[0].wr[9] & 0x08) && pending(s);
}

static uint8_t read_reg(Z85c30 *s, int ch, int reg)
{
    Z85c30Ch *c = &s->ch[ch];
    switch (reg) {
    case 0:
        return (uint8_t)((rx_avail(c) ? 0x01 : 0) | 0x04 | 0x08 | 0x20 | 0x40);
    case 1:  return 0x01;                                   /* tutto trasmesso */
    case 2: {
        uint8_t v = s->ch[0].wr[2];
        if (ch == 1) {                                      /* vettore con lo stato */
            uint8_t p = pending(s), st = 3 << 1;            /* nessuno: 011 */
            if (p & 0x20) st = 6 << 1; else if (p & 0x10) st = 4 << 1; else if (p & 0x08) st = 5 << 1;
            else if (p & 0x04) st = 2 << 1; else if (p & 0x02) st = 0 << 1; else if (p & 0x01) st = 1 << 1;
            v = (v & 0xF1) | st;
        }
        return v;
    }
    case 3:  return ch == 0 ? pending(s) : 0;
    case 8: {
        uint8_t v = 0;
        if (rx_avail(c)) { v = c->rxq[c->rx_tail]; c->rx_tail = (c->rx_tail + 1) % SCC_RXQ; }
        return v;
    }
    case 10: return 0;
    case 12: return c->wr[12];
    case 13: return c->wr[13];
    case 15: return c->wr[15];
    default: return c->wr[reg & 15];
    }
}

static void write_reg(Z85c30 *s, int ch, int reg, uint8_t v)
{
    Z85c30Ch *c = &s->ch[ch];
    switch (reg) {
    case 0:
        switch ((v >> 3) & 7) {
        case 1: s->ptr[ch] = (v & 7) | 8; return;          /* point high */
        case 2: c->ext_ip = 0; break;                                   /* reset ext/status */
        case 5: c->tx_ip = 0; break;                                    /* reset Tx int pending */
        default: break;
        }
        s->ptr[ch] = v & 7;
        return;
    case 2: s->ch[0].wr[2] = s->ch[1].wr[2] = v; return;
    case 8:
        if (s->tx) s->tx(s->user, ch, v);
        c->tx_ip = 1;
        return;
    case 9:
        s->ch[0].wr[9] = s->ch[1].wr[9] = v;
        switch (v >> 6) {
        case 1: reset_ch(&s->ch[1]); break;
        case 2: reset_ch(&s->ch[0]); break;
        case 3: reset_ch(&s->ch[0]); reset_ch(&s->ch[1]); break;
        default: break;
        }
        return;
    default:
        c->wr[reg] = v;
        return;
    }
}

uint8_t scc_read(Z85c30 *s, int ch, int data)
{
    if (data) return read_reg(s, ch, 8);
    int reg = s->ptr[ch];
    s->ptr[ch] = 0;
    return read_reg(s, ch, reg);
}

void scc_write(Z85c30 *s, int ch, int data, uint8_t v)
{
    if (data) { write_reg(s, ch, 8, v); return; }
    int reg = s->ptr[ch];
    s->ptr[ch] = 0;
    write_reg(s, ch, reg, v);
}
