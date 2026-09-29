/*
 * wd33c93.c - WD33C93 a livello di fase
 *
 * Select-and-Transfer seleziona, manda il CDB ed esegue il comando del
 * dispositivo; poi la fase dati procede finche' ci sono dati e il
 * transfer count (TC) non e' a zero:
 *  - dati finiti: fase di stato, fine con 0x16 (stato del target nel
 *    registro Target LUN, fase 0x60);
 *  - TC a zero con dati ancora da passare: pausa con "fase inattesa, dati"
 *    (0x49 in lettura, 0x48 in scrittura). Il driver carica un altro pezzo
 *    (TC e DMA) e riprende con Select-and-Transfer (fase 0x41-0x45) o con
 *    Transfer Info; dopo Transfer Info, a dati finiti, si segnala 0x1B
 *    (trasferimento fatto, il target e' in fase di stato);
 *  - la catena di descrittori dell'HPC3 finisce prima del TC: si aspetta
 *    che il driver riavvii il DMA (wd_dma_resume).
 * I dati passano dal DMA (callback) o, senza DMA attivo, dal registro dati.
 */
#include <stdlib.h>
#include <string.h>
#include "indy/wd33c93.h"

/* codici di stato (registro 0x17) */
#define ST_RESET          0x00
#define ST_RESET_EAF      0x01
#define ST_SAT_OK         0x16          /* select-and-transfer completato */
#define ST_XFER_DONE      0x18          /* + fase: transfer info completato */
#define ST_INVALID_CMD    0x40
#define ST_SEL_TIMEOUT    0x42
#define ST_UNEXP          0x48          /* + fase: fase inattesa */
#define PHS_DATA_OUT      0
#define PHS_DATA_IN       1
#define PHS_STATUS        3

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
    w->connected = 0;
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

/* esegue il comando del dispositivo; per le scritture i dati sono in buf */
static int exec_device(Wd33c93 *w, uint32_t *len)
{
    ScsiDev *d = w->dev[w->target];
    if (w->lun) {                                     /* solo la LUN 0 esiste */
        if (w->cdb[0] == 0x12 && *len) {
            memset(w->buf, 0, *len);
            w->buf[0] = 0x7F;
            if (*len > 36) *len = 36;
            return SCSI_GOOD;
        }
        *len = 0;
        return SCSI_CHECK_CONDITION;
    }
    return scsi_dev_exec(d, w->cdb, w->buf, len);
}

/* fase di stato raggiunta: fine del comando */
static void status_phase(Wd33c93 *w)
{
    if (w->from_ti) {                                 /* dopo Transfer Info: lo dice e aspetta */
        w->in_status = 1;
        w->reg[WD_PHASE] = 0x46;
        finish(w, ST_XFER_DONE | PHS_STATUS);
        return;
    }
    w->connected = 0;
    w->reg[WD_TARGET_LUN] = (uint8_t)w->status;
    w->reg[WD_PHASE] = 0x60;
    if (w->log) w->log(w->user, w->target, w->cdb, w->status, w->dpos);
    finish(w, ST_SAT_OK);
}

/* la fase dati: avanza finche' puo' */
static void data_phase(Wd33c93 *w)
{
    while (w->dpos < w->dlen) {
        uint32_t tc = get_count(w), n = w->dlen - w->dpos;
        if (n > tc) n = tc;
        if (!n) {                                     /* TC finito: pausa in fase dati */
            w->reg[WD_PHASE] = 0x45;                  /* fase dati in corso */
            finish(w, (uint8_t)((w->from_ti ? ST_XFER_DONE : ST_UNEXP) | (w->dir_in ? PHS_DATA_IN : PHS_DATA_OUT)));
            return;
        }
        long moved = w->dma ? w->dma(w->user, w->buf + w->dpos, n, w->dir_in) : -1;
        if (moved < 0) {                              /* polled: registro dati */
            w->poll = w->dir_in ? 1 : 2;
            w->poll_end = w->dpos + n;
            w->aux |= WD_AUX_DBR;
            return;
        }
        w->dpos += (uint32_t)moved;
        set_count(w, tc - (uint32_t)moved);
        if ((uint32_t)moved < n) { w->poll = 3; return; }   /* catena DMA finita: si aspetta */
    }
    if (!w->dir_in && !w->executed) {                 /* scrittura: ora il comando ha i dati */
        uint32_t len = w->dlen;
        w->status = exec_device(w, &len);
        w->executed = 1;
    }
    status_phase(w);
}

static void select_and_transfer(Wd33c93 *w)
{
    w->aux |= WD_AUX_BSY | WD_AUX_CIP;
    w->from_ti = 0;
    uint8_t phase = w->reg[WD_PHASE];
    if (w->connected && phase >= 0x41 && phase < 0x46 && !w->in_status) { data_phase(w); return; }
    if (w->connected && (phase >= 0x46 || w->in_status)) { status_phase(w); return; }

    /* nuovo comando: selezione, CDB, esecuzione */
    w->target = w->reg[WD_DEST_ID] & 7;
    ScsiDev *d = w->dev[w->target];
    if (!d) { finish(w, ST_SEL_TIMEOUT); return; }
    int n = scsi_cdb_len(w->reg[WD_CDB1]);
    memset(w->cdb, 0, sizeof w->cdb);
    memcpy(w->cdb, &w->reg[WD_CDB1], (size_t)n);
    w->lun = w->reg[WD_TARGET_LUN] & 7;
    int to_dev = 0;
    uint32_t want = scsi_dev_data_len(d, w->cdb, &to_dev);
    if (want > WD_BUF) want = WD_BUF;
    w->connected = 1;
    w->in_status = 0;
    w->dir_in = !to_dev;
    w->dpos = 0;
    w->executed = 0;
    if (w->dir_in) {
        uint32_t len = want;
        w->status = exec_device(w, &len);
        w->dlen = len;
        w->executed = 1;
    } else {
        w->dlen = want;
        if (!want) { uint32_t len = 0; w->status = exec_device(w, &len); w->executed = 1; }
    }
    w->reg[WD_PHASE] = 0x30;
    data_phase(w);
}

static void transfer_info(Wd33c93 *w)
{
    if (!w->connected) { finish(w, ST_INVALID_CMD); return; }
    w->aux |= WD_AUX_BSY | WD_AUX_CIP;
    w->from_ti = 1;
    if (w->dpos < w->dlen) { data_phase(w); return; }
    status_phase(w);
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
        w->connected = 0;
        finish(w, (own & 0x08) ? ST_RESET_EAF : ST_RESET);
        break;
    }
    case 0x01:                                           /* abort */
        w->connected = 0; w->poll = 0;
        break;
    case 0x02: case 0x03: case 0x04:                     /* ATN, ACK, disconnect */
        break;
    case 0x08: case 0x09:                                /* select-and-transfer */
        select_and_transfer(w);
        break;
    case 0x20:                                           /* transfer info */
        transfer_info(w);
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

/* il driver ha ricaricato i descrittori e riattivato il DMA */
void wd_dma_resume(Wd33c93 *w)
{
    if (w->poll != 3) return;
    w->poll = 0;
    data_phase(w);
}

/* un byte passato dal registro dati in modo polled */
static void polled_step(Wd33c93 *w)
{
    w->dpos++;
    uint32_t tc = get_count(w);
    if (tc) set_count(w, tc - 1);
    if (w->dpos >= w->poll_end) {
        w->poll = 0;
        w->aux &= ~WD_AUX_DBR;
        data_phase(w);
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
        if (w->poll == 1 && w->dpos < w->poll_end) { v = w->buf[w->dpos]; polled_step(w); }
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
        if (w->poll == 2 && w->dpos < w->poll_end) { w->buf[w->dpos] = v; polled_step(w); }
        return;
    case WD_STATUS: case WD_AUX: break;                   /* sola lettura */
    default: w->reg[a] = v; break;
    }
    w->addr = (uint8_t)((a + 1) & 0x1F);
}
