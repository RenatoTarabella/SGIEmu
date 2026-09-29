/*
 * wd33c93.c - WD33C93: reset, selezione, interrupt
 */
#include <string.h>
#include "indy/wd33c93.h"

/* codici di stato (registro 0x17) */
#define ST_RESET          0x00
#define ST_RESET_EAF      0x01
#define ST_SEL_TIMEOUT    0x42
#define ST_INVALID_CMD    0x40

void wd_reset(Wd33c93 *w)
{
    uint8_t present = w->present;
    memset(w, 0, sizeof *w);
    w->present = present;
}

static void finish(Wd33c93 *w, uint8_t status)
{
    w->reg[WD_STATUS] = status;
    w->aux = (uint8_t)((w->aux & ~(WD_AUX_CIP | WD_AUX_BSY)) | WD_AUX_INT);
    w->irq = 1;
}

static void command(Wd33c93 *w, uint8_t cmd)
{
    w->reg[WD_COMMAND] = cmd;
    switch (cmd & 0x7F) {
    case 0x00: {                                          /* reset: l'own ID resta */
        uint8_t own = w->reg[WD_OWN_ID];
        memset(w->reg, 0, WD_COMMAND);
        w->reg[WD_OWN_ID] = own;
        finish(w, (own & 0x08) ? ST_RESET_EAF : ST_RESET);
        break;
    }
    case 0x01: case 0x02: case 0x03:                      /* abort, assert ATN, negate ACK */
        break;
    case 0x06: case 0x07: case 0x08: case 0x09:          /* selezioni */
    case 0x0A: case 0x0B: {
        /* nessun dispositivo ancora collegato: timeout di selezione */
        finish(w, ST_SEL_TIMEOUT);
        break;
    }
    default:
        finish(w, ST_INVALID_CMD);
        break;
    }
}

uint8_t wd_read(Wd33c93 *w, int port)
{
    if (!port) return w->aux;
    uint8_t a = w->addr & 0x1F, v;
    switch (a) {
    case WD_AUX:    return w->aux;
    case WD_STATUS:                                        /* la lettura spegne l'interrupt */
        v = w->reg[WD_STATUS];
        w->aux &= ~WD_AUX_INT;
        w->irq = 0;
        break;
    default: v = w->reg[a]; break;
    }
    if (a != WD_COMMAND && a != WD_DATA) w->addr = (uint8_t)((a + 1) & 0x1F);
    return v;
}

void wd_write(Wd33c93 *w, int port, uint8_t v)
{
    if (!port) { w->addr = v & 0x1F; return; }
    uint8_t a = w->addr & 0x1F;
    switch (a) {
    case WD_COMMAND: command(w, v); return;
    case WD_STATUS: case WD_AUX: break;                   /* sola lettura */
    default: w->reg[a] = v; break;
    }
    if (a != WD_DATA) w->addr = (uint8_t)((a + 1) & 0x1F);
}
