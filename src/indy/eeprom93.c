/*
 * eeprom93.c - EEPROM 93CS56 a livello di bit
 */
#include "indy/eeprom93.h"

enum { S_IDLE, S_CMD, S_READ, S_WRITE, S_DONE };

void ee_reset(Eeprom93 *e)
{
    e->cs = e->clk = 0;
    e->dout = 1;
    e->state = S_IDLE;
    e->wen = 0;
}

static void finish_write(Eeprom93 *e)
{
    uint16_t v = (uint16_t)e->shift;
    if (!e->wen) return;
    if (e->op == 1) { e->data[e->addr] = v; e->dirty = 1; }                   /* WRITE */
    else if (e->op == 0) {                                                  /* WRAL */
        for (int k = 0; k < 128; k++) e->data[k] = v;
        e->dirty = 1;
    }
}

static void command(Eeprom93 *e)
{
    int op = (int)(e->shift >> 8) & 3, a8 = (int)e->shift & 0xFF, addr = a8 & 0x7F;
    e->op = op; e->addr = addr;
    switch (op) {
    case 2:                                                  /* READ */
        e->state = S_READ; e->nbits = 0; e->dout = 0;         /* bit fittizio a 0 */
        break;
    case 1:                                                  /* WRITE */
        e->state = S_WRITE; e->nbits = 0; e->shift = 0;
        break;
    case 3:                                                  /* ERASE */
        if (e->wen) { e->data[addr] = 0xFFFF; e->dirty = 1; }
        e->state = S_DONE;
        break;
    default:
        switch (a8 >> 6) {                               /* i due bit alti dell'indirizzo */
        case 3: e->wen = 1; e->state = S_DONE; break;       /* EWEN */
        case 0: e->wen = 0; e->state = S_DONE; break;       /* EWDS */
        case 2:                                              /* ERAL */
            if (e->wen) { for (int k = 0; k < 128; k++) e->data[k] = 0xFFFF; e->dirty = 1; }
            e->state = S_DONE;
            break;
        default: e->state = S_WRITE; e->nbits = 0; e->shift = 0; break;   /* WRAL */
        }
    }
}

void ee_pins(Eeprom93 *e, int cs, int clk, int di)
{
    if (!cs) {
        if (e->cs && e->state == S_WRITE && e->nbits >= 16) finish_write(e);
        e->cs = 0; e->clk = clk;
        e->state = S_IDLE;
        e->dout = 1;                                         /* pronta */
        return;
    }
    int rising = clk && !e->clk;
    e->cs = 1; e->clk = clk;
    if (!rising) return;
    switch (e->state) {
    case S_IDLE:
        if (di) { e->state = S_CMD; e->shift = 0; e->nbits = 0; }
        break;
    case S_CMD:
        e->shift = (e->shift << 1) | (di & 1);
        if (++e->nbits == 10) command(e);
        break;
    case S_READ: {
        int bit = 15 - (e->nbits & 15);
        e->dout = (e->data[e->addr] >> bit) & 1;
        if (++e->nbits % 16 == 0) e->addr = (e->addr + 1) & 0x7F;      /* lettura sequenziale */
        break;
    }
    case S_WRITE:
        if (e->nbits < 16) { e->shift = (e->shift << 1) | (di & 1); e->nbits++; }
        break;
    default:
        break;
    }
}
