/*
 * wd33c93.c - WD33C93 a livello di transazione
 */
#include <stdlib.h>
#include <string.h>
#include "indy/wd33c93.h"

/* codici di stato (registro 0x17) */
#define ST_RESET          0x00
#define ST_RESET_EAF      0x01
#define ST_SAT_OK         0x16          /* select-and-transfer completato */
#define ST_INVALID_CMD    0x40
#define ST_SEL_TIMEOUT    0x42

void wd_init(Wd33c93 *w)
{
    memset(w, 0, sizeof *w);
    w->buf = malloc(WD_BUF);
}

void wd_reset(Wd33c93 *w)
{
    memset(w->reg, 0, sizeof w->reg);
    w->addr = 0;
    w->aux = 0;
    w->irq = 0;
    w->poll = 0;
}

static void finish(Wd33c93 *w, uint8_t status)
{
    w->reg[WD_STATUS] = status;
    w->aux = (uint8_t)((w->aux & ~(WD_AUX_CIP | WD_AUX_BSY | WD_AUX_DBR)) | WD_AUX_INT);
    w->irq = 1;
    w->poll = 0;
}

static uint32_t get_count(const Wd33c93 *w)
{
    return (uint32_t)w->reg[WD_COUNT_HI] << 16 | (uint32_t)w->reg[WD_COUNT_MID] << 8 | w->reg[WD_COUNT_LO];
}

static void set_count(Wd33c93 *w, uint32_t c)
{
    w->reg[WD_COUNT_HI] = (uint8_t)(c >> 16);
    w->reg[WD_COUNT_MID] = (uint8_t)(c >> 8);
    w->reg[WD_COUNT_LO] = (uint8_t)c;
}

/* fine del comando: stato del target nel registro Target LUN */
static void complete(Wd33c93 *w, int status, uint32_t moved)
{
    uint32_t count = get_count(w);
    set_count(w, count > moved ? count - moved : 0);
    w->reg[WD_TARGET_LUN] = (uint8_t)status;
    w->reg[WD_PHASE] = 0x60;
    if (w->log) w->log(w->user, w->target, w->cdb, status, moved);
    finish(w, ST_SAT_OK);
}

/* Esegue il comando del target (i dati in uscita sono gia' in buf) */
static void run_command(Wd33c93 *w, uint32_t want, int to_dev)
{
    ScsiDev *d = w->dev[w->target];
    uint32_t len = to_dev ? want : (want < WD_BUF ? want : WD_BUF);
    int lun = w->reg[WD_TARGET_LUN] & 7;
    int status;
    if (lun) {                                        /* solo la LUN 0 esiste */
        if (w->cdb[0] == 0x12 && len) {
            memset(w->buf, 0, len);
            w->buf[0] = 0x7F;
            status = SCSI_GOOD;
            len = len < 36 ? len : 36;
        } else {
            status = SCSI_CHECK_CONDITION;
            len = 0;
        }
    } else {
        status = scsi_dev_exec(d, w->cdb, w->buf, &len);
    }
    if (to_dev) { complete(w, status, want); return; }
    uint32_t count = get_count(w);
    if (len > count) len = count;
    if (!len) { complete(w, status, 0); return; }
    long moved = w->dma ? w->dma(w->user, w->buf, len, 1) : -1;
    if (moved >= 0) { complete(w, status, (uint32_t)moved); return; }
    /* polled: i dati si leggono dal registro dati */
    w->len = len; w->pos = 0; w->poll = 1;
    w->reg[WD_TARGET_LUN] = (uint8_t)status;
    w->aux |= WD_AUX_DBR;
}

static void select_and_transfer(Wd33c93 *w)
{
    w->target = w->reg[WD_DEST_ID] & 7;
    ScsiDev *d = w->dev[w->target];
    if (!d) { finish(w, ST_SEL_TIMEOUT); return; }
    int n = scsi_cdb_len(w->reg[WD_CDB1]);
    memset(w->cdb, 0, sizeof w->cdb);
    memcpy(w->cdb, &w->reg[WD_CDB1], (size_t)n);
    w->aux |= WD_AUX_BSY | WD_AUX_CIP;
    int to_dev = 0;
    uint32_t want = scsi_dev_data_len(d, w->cdb, &to_dev);
    if (want > WD_BUF) want = WD_BUF;
    if (to_dev) {
        uint32_t count = get_count(w);
        if (want > count) want = count;
        if (want) {
            long moved = w->dma ? w->dma(w->user, w->buf, want, 0) : -1;
            if (moved < 0) {                          /* polled: si aspetta il registro dati */
                w->len = want; w->pos = 0; w->poll = 2;
                w->aux |= WD_AUX_DBR;
                return;
            }
            want = (uint32_t)moved;
        }
    }
    run_command(w, want, to_dev);
}

static void command(Wd33c93 *w, uint8_t cmd)
{
    w->reg[WD_COMMAND] = cmd;
    switch (cmd & 0x7F) {
    case 0x00: {                                          /* reset: l'own ID resta */
        uint8_t own = w->reg[WD_OWN_ID];
        memset(w->reg, 0, WD_COMMAND);
        w->reg[WD_OWN_ID] = own;
        w->poll = 0;
        finish(w, (own & 0x08) ? ST_RESET_EAF : ST_RESET);
        break;
    }
    case 0x01: case 0x02: case 0x03: case 0x04:          /* abort, ATN, ACK, disconnect */
        break;
    case 0x08: case 0x09:                                /* select-and-transfer */
        select_and_transfer(w);
        break;
    case 0x06: case 0x07:                                /* solo selezione: non ancora */
        if (!w->dev[w->reg[WD_DEST_ID] & 7]) finish(w, ST_SEL_TIMEOUT);
        else finish(w, ST_INVALID_CMD);
        break;
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
    case WD_DATA:
        v = 0;
        if (w->poll == 1 && w->pos < w->len) {
            v = w->buf[w->pos++];
            if (w->pos == w->len) complete(w, w->reg[WD_TARGET_LUN], w->len);
        }
        return v;
    default: v = w->reg[a]; break;
    }
    if (a != WD_COMMAND) w->addr = (uint8_t)((a + 1) & 0x1F);
    return v;
}

void wd_write(Wd33c93 *w, int port, uint8_t v)
{
    if (!port) { w->addr = v & 0x1F; return; }
    uint8_t a = w->addr & 0x1F;
    switch (a) {
    case WD_COMMAND: command(w, v); return;
    case WD_DATA:
        if (w->poll == 2 && w->pos < w->len) {
            w->buf[w->pos++] = v;
            if (w->pos == w->len) { w->poll = 0; run_command(w, w->len, 1); }
        }
        return;
    case WD_STATUS: case WD_AUX: break;                   /* sola lettura */
    default: w->reg[a] = v; break;
    }
    w->addr = (uint8_t)((a + 1) & 0x1F);
}
