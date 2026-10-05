/*
 * scsi_dev.h - Dispositivi SCSI emulati: disco e CD-ROM su file immagine
 *
 * A livello di comando: il controller passa il CDB, il dispositivo dice
 * quanti dati si aspetta e in che direzione, poi li produce o li consuma.
 * I CD-ROM SGI lavorano a blocchi da 512 byte, come i lettori che IRIX
 * supportava (Toshiba con il ponticello): qui tutto va a 512.
 */
#ifndef SCSI_DEV_H
#define SCSI_DEV_H

#include <stdint.h>
#include <stdio.h>

enum { SCSI_DISK = 0, SCSI_CDROM = 5 };

/* byte di stato */
#define SCSI_GOOD            0x00
#define SCSI_CHECK_CONDITION 0x02

typedef struct ScsiDev {
    int      type;
    FILE    *f;
    uint64_t blocks;          /* blocchi da 512 byte */
    int      readonly;
    int      raw;             /* CD a settori grezzi MODE1/2352 (.bin di chdman) */
    uint8_t  sense_key, asc, ascq;
    char     vendor[9], product[17], revision[5];
    uint64_t reads, writes;   /* statistiche: blocchi */
    /* SCSI_SNAPSHOT: il file resta com'e', i blocchi scritti stanno qui */
    int      snapshot;
    uint64_t *cow_key;        /* lba + 1, 0 = libero (indirizzamento aperto) */
    uint8_t  *cow_data;
    uint32_t cow_n, cow_cap;
} ScsiDev;

/* readonly: 0 = lettura e scrittura, 1 = sola lettura, SCSI_SNAPSHOT =
   scritture tenute in memoria (per confrontare esecuzioni) */
#define SCSI_SNAPSHOT 2
int  scsi_dev_open(ScsiDev *d, int type, const char *path, int readonly);
void scsi_dev_close(ScsiDev *d);

/* Fase 1: lunghezza (in byte) e direzione dei dati del comando.
   *to_dev = 1 se i dati vanno dall'host al dispositivo (scrittura). */
uint32_t scsi_dev_data_len(ScsiDev *d, const uint8_t *cdb, int *to_dev);
/* Fase 2: esegue. buf contiene i dati da scrivere o riceve quelli letti;
   *len e' quanti ne sono stati prodotti. Ritorna il byte di stato. */
int scsi_dev_exec(ScsiDev *d, const uint8_t *cdb, uint8_t *buf, uint32_t *len);

static inline int scsi_cdb_len(uint8_t op)
{
    switch (op >> 5) {
    case 0: return 6;
    case 1: case 2: return 10;
    case 5: return 12;
    default: return 6;
    }
}

#endif
