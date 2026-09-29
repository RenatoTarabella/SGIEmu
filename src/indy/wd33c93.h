/*
 * wd33c93.h - Controller SCSI Western Digital WD33C93 (sull'HPC3)
 *
 * Due porte: SASR (indirizzo del registro; in lettura lo stato ausiliario)
 * e SCMD (il registro puntato, con autoincremento). Per ora: reset,
 * selezione (timeout se al target non c'e' niente), interrupt.
 * I dati dei dispositivi arriveranno con i dischi.
 */
#ifndef WD33C93_H
#define WD33C93_H

#include <stdint.h>

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

typedef struct Wd33c93 {
    uint8_t reg[32];
    uint8_t addr;
    uint8_t aux;
    int     irq;
    /* dispositivi presenti sul bus (bit per ID) */
    uint8_t present;
} Wd33c93;

void    wd_reset(Wd33c93 *w);
uint8_t wd_read(Wd33c93 *w, int port);                /* port 0 = SASR, 1 = SCMD */
void    wd_write(Wd33c93 *w, int port, uint8_t v);

#endif
