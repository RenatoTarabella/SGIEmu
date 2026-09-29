/*
 * kbc8042.c - Controller 8042 minimo
 */
#include <string.h>
#include "indy/kbc8042.h"

static void push(Kbc8042 *k, uint8_t v, int aux)
{
    int next = (k->head + 1) % KBC_Q;
    if (next == k->tail) return;
    k->q[k->head] = v;
    k->qaux[k->head] = (uint8_t)aux;
    k->head = next;
}

void kbc_reset(Kbc8042 *k)
{
    memset(k, 0, sizeof *k);
    k->cmdbyte = 0x47;                  /* interrupt tastiera e mouse, traduzione */
}

uint8_t kbc_read(Kbc8042 *k, int status)
{
    int full = k->head != k->tail;
    if (status)
        return (uint8_t)((full ? 0x01 : 0) | 0x04 | 0x10 | (full && k->qaux[k->tail] ? 0x20 : 0));
    if (!full) return 0;
    uint8_t v = k->q[k->tail];
    k->tail = (k->tail + 1) % KBC_Q;
    return v;
}

static void device_cmd(Kbc8042 *k, uint8_t v, int aux)
{
    switch (v) {
    case 0xFF:                                      /* reset */
        push(k, 0xFA, aux); push(k, 0xAA, aux);
        if (aux) push(k, 0x00, aux);
        break;
    case 0xF2:                                      /* identificazione */
        push(k, 0xFA, aux);
        if (aux) push(k, 0x00, aux); else { push(k, 0xAB, aux); push(k, 0x83, aux); }
        break;
    case 0xEE: push(k, 0xEE, aux); break;           /* eco */
    default:   push(k, 0xFA, aux); break;           /* ACK */
    }
}

void kbc_write(Kbc8042 *k, int command, uint8_t v)
{
    if (!command) {
        int p = k->pending;
        k->pending = 0;
        switch (p) {
        case 0x60: k->cmdbyte = v; return;
        case 0xD2: push(k, v, 0); return;          /* come se venisse dalla tastiera */
        case 0xD3: push(k, v, 1); return;
        case 0xD4: device_cmd(k, v, 1); return;    /* al mouse */
        default:   device_cmd(k, v, 0); return;    /* alla tastiera */
        }
    }
    switch (v) {
    case 0x20: push(k, k->cmdbyte, 0); break;
    case 0x60: case 0xD2: case 0xD3: case 0xD4: case 0xD1: k->pending = v; break;
    case 0xAA: push(k, 0x55, 0); break;            /* autotest */
    case 0xAB: case 0xA9: push(k, 0x00, 0); break; /* test interfacce */
    case 0xA7: k->cmdbyte |= 0x20; break;
    case 0xA8: k->cmdbyte &= ~0x20; break;
    case 0xAD: k->cmdbyte |= 0x10; break;
    case 0xAE: k->cmdbyte &= ~0x10; break;
    case 0xC0: push(k, 0x00, 0); break;
    default: break;
    }
}

void kbc_key(Kbc8042 *k, uint8_t sc) { push(k, sc, 0); }

int kbc_irq(const Kbc8042 *k)
{
    if (k->head == k->tail) return 0;
    return k->qaux[k->tail] ? (k->cmdbyte & 2) != 0 : (k->cmdbyte & 1) != 0;
}
