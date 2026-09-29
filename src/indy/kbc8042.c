/*
 * kbc8042.c - Controller 8042, tastiera e mouse PS/2
 */
#include <string.h>
#include "indy/kbc8042.h"

/* set 1 -> set 2, tasti senza prefisso (0 = nessuno) */
static const uint8_t set1_to_2[0x59] = {
    0x00, 0x76, 0x16, 0x1E, 0x26, 0x25, 0x2E, 0x36, 0x3D, 0x3E, 0x46, 0x45, 0x4E, 0x55, 0x66, 0x0D,
    0x15, 0x1D, 0x24, 0x2D, 0x2C, 0x35, 0x3C, 0x43, 0x44, 0x4D, 0x54, 0x5B, 0x5A, 0x14, 0x1C, 0x1B,
    0x23, 0x2B, 0x34, 0x33, 0x3B, 0x42, 0x4B, 0x4C, 0x52, 0x0E, 0x12, 0x5D, 0x1A, 0x22, 0x21, 0x2A,
    0x32, 0x31, 0x3A, 0x41, 0x49, 0x4A, 0x59, 0x7C, 0x11, 0x29, 0x58, 0x05, 0x06, 0x04, 0x0C, 0x03,
    0x0B, 0x83, 0x0A, 0x01, 0x09, 0x77, 0x7E, 0x6C, 0x75, 0x7D, 0x7B, 0x6B, 0x73, 0x74, 0x79, 0x69,
    0x72, 0x7A, 0x70, 0x71, 0x00, 0x00, 0x61, 0x78, 0x07
};

/* set 1 con E0 -> set 2 (sempre con E0) */
static uint8_t ext1_to_2(int sc)
{
    switch (sc) {
    case 0x1C: return 0x5A;  case 0x1D: return 0x14;  case 0x35: return 0x4A;
    case 0x38: return 0x11;  case 0x47: return 0x6C;  case 0x48: return 0x75;
    case 0x49: return 0x7D;  case 0x4B: return 0x6B;  case 0x4D: return 0x74;
    case 0x4F: return 0x69;  case 0x50: return 0x72;  case 0x51: return 0x7A;
    case 0x52: return 0x70;  case 0x53: return 0x71;  case 0x5B: return 0x1F;
    case 0x5C: return 0x27;  case 0x5D: return 0x2F;  case 0x37: return 0x7C;
    default:   return 0;
    }
}

/* set 3: dove differisce dal set 2 (per codice del set 1, ext = E0) */
static uint8_t set3(int sc, int ext)
{
    if (ext) {
        switch (sc) {
        case 0x1C: return 0x79;  case 0x1D: return 0x58;  case 0x35: return 0x77;
        case 0x38: return 0x39;  case 0x47: return 0x6E;  case 0x48: return 0x63;
        case 0x49: return 0x6F;  case 0x4B: return 0x61;  case 0x4D: return 0x6A;
        case 0x4F: return 0x65;  case 0x50: return 0x60;  case 0x51: return 0x6D;
        case 0x52: return 0x67;  case 0x53: return 0x64;  case 0x5B: return 0x8B;
        case 0x5C: return 0x8C;  case 0x5D: return 0x8D;  case 0x37: return 0x57;
        default:   return 0;
        }
    }
    switch (sc) {
    case 0x01: return 0x08;  case 0x3B: return 0x07;  case 0x3C: return 0x0F;
    case 0x3D: return 0x17;  case 0x3E: return 0x1F;  case 0x3F: return 0x27;
    case 0x40: return 0x2F;  case 0x41: return 0x37;  case 0x42: return 0x3F;
    case 0x43: return 0x47;  case 0x44: return 0x4F;  case 0x57: return 0x56;
    case 0x58: return 0x5E;  case 0x46: return 0x5F;  case 0x1D: return 0x11;
    case 0x38: return 0x19;  case 0x3A: return 0x14;  case 0x2B: return 0x5C;
    case 0x56: return 0x13;  case 0x45: return 0x76;  case 0x37: return 0x7E;
    case 0x4A: return 0x84;  case 0x4E: return 0x7C;
    default:   return sc < 0x59 ? set1_to_2[sc] : 0;
    }
}

static void push(Kbc8042 *k, uint8_t v, int aux)
{
    int next = (k->head + 1) % KBC_Q;
    if (next == k->tail) return;
    k->q[k->head] = v;
    k->qaux[k->head] = (uint8_t)aux;
    k->head = next;
}

int kbc_room(const Kbc8042 *k)
{
    return KBC_Q - 1 - ((k->head - k->tail + KBC_Q) % KBC_Q);
}

void kbc_reset(Kbc8042 *k)
{
    memset(k, 0, sizeof *k);
    k->cmdbyte = 0x47;                  /* interrupt tastiera e mouse, traduzione */
    k->set = 2;
    k->kbd_enabled = 1;
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

static void kbd_cmd(Kbc8042 *k, uint8_t v)
{
    if (k->kbd_pending) {                               /* dato di un comando */
        int c = k->kbd_pending;
        k->kbd_pending = 0;
        push(k, 0xFA, 0);
        if (c == 0xF0) {
            if (v == 0) push(k, (uint8_t)k->set, 0);
            else if (v <= 3) k->set = v;
        }
        return;
    }
    switch (v) {
    case 0xFF: push(k, 0xFA, 0); push(k, 0xAA, 0); k->set = 2; k->kbd_enabled = 1; break;
    case 0xF2: push(k, 0xFA, 0); push(k, 0xAB, 0); push(k, 0x83, 0); break;
    case 0xEE: push(k, 0xEE, 0); break;
    case 0xED: case 0xF0: case 0xF3: case 0xFB: case 0xFC: case 0xFD:
        k->kbd_pending = v; push(k, 0xFA, 0); break;
    case 0xF4: k->kbd_enabled = 1; push(k, 0xFA, 0); break;
    case 0xF5: k->kbd_enabled = 0; push(k, 0xFA, 0); break;
    case 0xF6: k->set = 2; push(k, 0xFA, 0); break;
    default:   push(k, 0xFA, 0); break;                 /* F7-FA e il resto: ACK */
    }
}

static void aux_cmd(Kbc8042 *k, uint8_t v)
{
    if (k->aux_pending) { k->aux_pending = 0; push(k, 0xFA, 1); return; }
    switch (v) {
    case 0xFF: push(k, 0xFA, 1); push(k, 0xAA, 1); push(k, 0x00, 1); k->aux_enabled = 0; break;
    case 0xF2: push(k, 0xFA, 1); push(k, 0x00, 1); break;
    case 0xE9: push(k, 0xFA, 1); push(k, 0x00, 1); push(k, 0x02, 1); push(k, 0x64, 1); break;
    case 0xE8: case 0xF3: k->aux_pending = v; push(k, 0xFA, 1); break;
    case 0xF4: k->aux_enabled = 1; push(k, 0xFA, 1); break;
    case 0xF5: case 0xF6: k->aux_enabled = 0; push(k, 0xFA, 1); break;
    default:   push(k, 0xFA, 1); break;
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
        case 0xD4: aux_cmd(k, v); return;          /* al mouse */
        case 0xD1: return;                         /* porta d'uscita */
        default:   kbd_cmd(k, v); return;          /* alla tastiera */
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

void kbc_key(Kbc8042 *k, int sc, int ext, int down)
{
    if (!k->kbd_enabled || kbc_room(k) < 8) return;
    if (k->cmdbyte & 0x40) {                       /* traduzione: il sistema vede il set 1 */
        if (ext) push(k, 0xE0, 0);
        push(k, (uint8_t)(down ? sc : sc | 0x80), 0);
        return;
    }
    if (k->set == 3) {
        uint8_t c = set3(sc, ext);
        if (!c) return;
        if (!down) push(k, 0xF0, 0);
        push(k, c, 0);
        return;
    }
    uint8_t c = ext ? ext1_to_2(sc) : (sc < 0x59 ? set1_to_2[sc] : 0);
    if (!c) return;
    if (ext) push(k, 0xE0, 0);
    if (!down) push(k, 0xF0, 0);
    push(k, c, 0);
}

void kbc_mouse(Kbc8042 *k, int dx, int dy, int buttons)
{
    if (!k->aux_enabled || kbc_room(k) < 3) return;
    if (dx > 255) dx = 255; else if (dx < -255) dx = -255;
    if (dy > 255) dy = 255; else if (dy < -255) dy = -255;
    uint8_t b0 = (uint8_t)(0x08 | (buttons & 7) | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0));
    push(k, b0, 1);
    push(k, (uint8_t)dx, 1);
    push(k, (uint8_t)dy, 1);
}

int kbc_irq(const Kbc8042 *k)
{
    if (k->head == k->tail) return 0;
    return k->qaux[k->tail] ? (k->cmdbyte & 2) != 0 : (k->cmdbyte & 1) != 0;
}
