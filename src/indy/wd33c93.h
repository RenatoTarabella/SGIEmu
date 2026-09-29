/*
 * wd33c93.h - Controller SCSI Western Digital WD33C93 (sull'HPC3)
 *
 * Due porte: SASR (indirizzo del registro; in lettura lo stato ausiliario)
 * e SCMD (il registro puntato, con autoincremento).
 *
 * A livello di transazione: "Select-and-Transfer" esegue tutto il comando
 * SCSI in un colpo (selezione, CDB, dati, stato, messaggio) e alla fine da'
 * l'interrupt con lo stato 0x16; il byte di stato del target va nel
 * registro Target LUN. I dati passano dal DMA dell'HPC3 (callback dma) o,
 * in modo polled, dal registro dati.
 */
#ifndef WD33C93_H
#define WD33C93_H

#include <stdint.h>
#include "indy/scsi_dev.h"

enum {
    WD_OWN_ID = 0x00, WD_CONTROL = 0x01, WD_TIMEOUT = 0x02, WD_CDB1 = 0x03,
    WD_TARGET_LUN = 0x0F, WD_PHASE = 0x10, WD_SYNC = 0x11, WD_COUNT_HI = 0x12,
    WD_COUNT_MID = 0x13, WD_COUNT_LO = 0x14, WD_DEST_ID = 0x15, WD_SRC_ID = 0x16,
    WD_STATUS = 0x17, WD_COMMAND = 0x18, WD_DATA = 0x19, WD_AUX = 0x1F
};

#define WD_AUX_INT 0x80
#define WD_AUX_BSY 0x20
#define WD_AUX_CIP 0x10
#define WD_AUX_DBR 0x01

#define WD_BUF (16u << 20)            /* trasferimento massimo per comando */

typedef struct Wd33c93 {
    uint8_t  reg[32];
    uint8_t  addr;
    uint8_t  aux;
    int      irq;
    ScsiDev *dev[8];                  /* dispositivi per ID */

    /* DMA verso la memoria: ritorna i byte trasferiti, -1 se il canale
       dell'HPC3 non e' attivo (allora si va in polled) */
    long   (*dma)(void *user, uint8_t *buf, uint32_t len, int to_mem);
    void    *user;

    /* comando in corso (polled) */
    uint8_t *buf;
    uint32_t len, pos;
    int      poll;                    /* 0 niente, 1 dati verso l'host, 2 dall'host */
    uint8_t  cdb[16];
    int      target;

    /* traccia dei comandi (per il debug) */
    void   (*log)(void *user, int id, const uint8_t *cdb, int status, uint32_t len);
} Wd33c93;

void    wd_init(Wd33c93 *w);
void    wd_reset(Wd33c93 *w);
uint8_t wd_read(Wd33c93 *w, int port);                /* port 0 = SASR, 1 = SCMD */
void    wd_write(Wd33c93 *w, int port, uint8_t v);

#endif
