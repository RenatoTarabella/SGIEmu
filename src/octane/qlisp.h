/*
 * qlisp.h - Controller SCSI QLogic ISP1040 (PCI) dell'Octane, due nella BaseIO
 *
 * Si emula il comportamento del firmware, non il suo processore RISC: i
 * comandi a mailbox rispondono subito, e i comandi SCSI arrivano come
 * blocchi da 64 byte (IOCB) in una coda in memoria letta via DMA; per
 * ciascuno si scrive un blocco di stato nella coda delle risposte.
 *
 * Registri (a 16 bit, offset del chip): id 0x00, cfg_0 0x04, cfg_1 0x06,
 * ictrl 0x08, istatus 0x0A, semaphore 0x0C, nvram 0x0E, mailbox 0..7 a
 * 0x70..0x7E, host_cmd 0xC0. Le mailbox hanno due facce: l'host scrive
 * gli ingressi del comando, legge le uscite. Mailbox 4 e 5 sono anche gli
 * indici delle code: l'host scrive in 4 l'ingresso della coda dei comandi e
 * in 5 l'uscita di quella delle risposte; il chip ci mette la sua uscita
 * dei comandi e il suo ingresso delle risposte.
 *
 * Gli indirizzi DMA sono indirizzi PCI: li traduce chi ospita il chip.
 */
#ifndef QLISP_H
#define QLISP_H

#include <stdint.h>
#include "indy/scsi_dev.h"

#define QL_TARGETS 16

typedef struct QlIsp {
    uint16_t cfg1, ictrl, istatus, semaphore, nvram;
    uint16_t mb_in[8];                 /* scritte dall'host */
    uint16_t mb_out[8];                /* lette dall'host */
    int      risc_reset;               /* RISC in reset o in pausa */
    uint16_t ram[65536];               /* memoria del RISC (LOAD RAM, WRITE RAM WORD) */
    uint16_t fw_major, fw_minor, fw_sub;

    /* code: indirizzo PCI, voci, indici */
    uint64_t req_base, rsp_base;
    uint16_t req_len, rsp_len;
    uint16_t req_out, rsp_in;
    int      be;                       /* IOCB in big-endian (firmware SGI) */

    int      bus;                      /* numero del controller, per i log */
    uint8_t  initiator_id;
    ScsiDev *dev[QL_TARGETS];
    uint8_t *buf;                      /* dati di un comando */
    uint32_t buf_size;

    /* DMA verso la memoria: ritornano i byte trasferiti */
    uint32_t (*dma_read)(void *ctx, uint64_t pci, uint8_t *dst, uint32_t len);
    uint32_t (*dma_write)(void *ctx, uint64_t pci, const uint8_t *src, uint32_t len);
    void    *ctx;

    void   (*log)(void *ctx, const char *msg);  /* comandi e mailbox, se non NULL */
    uint64_t cmds;                     /* comandi SCSI eseguiti */
} QlIsp;

int      ql_init(QlIsp *q);
void     ql_free(QlIsp *q);
void     ql_reset(QlIsp *q);
uint16_t ql_read(QlIsp *q, uint32_t off);
void     ql_write(QlIsp *q, uint32_t off, uint16_t v);
/* linea di interrupt verso il PCI */
static inline int ql_irq(const QlIsp *q) { return (q->istatus & 4) && (q->ictrl & 2); }

#endif
