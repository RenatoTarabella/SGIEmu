/*
 * qlisp.c - QLogic ISP1040: mailbox e code dei comandi (firmware emulato)
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "octane/qlisp.h"

enum {                                     /* registri */
    R_ID_L = 0x00, R_ID_H = 0x02, R_CFG0 = 0x04, R_CFG1 = 0x06, R_ICTRL = 0x08,
    R_ISTATUS = 0x0A, R_SEMA = 0x0C, R_NVRAM = 0x0E, R_MBOX = 0x70, R_HCCR = 0xC0
};
#define ISTAT_RISC_INT 0x0004
#define MBS_OK         0x4000
#define MBS_INVALID    0x4001

static void logf_(QlIsp *q, const char *fmt, ...)
{
    if (!q->log) return;
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    q->log(q->ctx, b);
}

int ql_init(QlIsp *q)
{
    memset(q, 0, sizeof *q);
    q->buf_size = 1u << 20;
    q->buf = malloc(q->buf_size);
    q->initiator_id = 7;
    q->fw_major = 2; q->fw_minor = 55;
    if (!q->buf) return -1;
    ql_reset(q);
    return 0;
}

void ql_free(QlIsp *q)
{
    free(q->buf);
    q->buf = NULL;
}

void ql_reset(QlIsp *q)
{
    q->ictrl = 0;
    q->istatus = 0;
    q->semaphore = 0;
    memset(q->mb_in, 0, sizeof q->mb_in);
    memset(q->mb_out, 0, sizeof q->mb_out);    /* mailbox 0 = 0: pronto */
    q->risc_reset = 0;
    q->req_len = q->rsp_len = 0;
    q->req_out = q->rsp_in = 0;
}

/* ---- campi delle IOCB (little-endian, come nel driver Linux) ---- */

static uint16_t g16(const uint8_t *e, int o) { return (uint16_t)(e[o] | e[o + 1] << 8); }
static uint32_t g32(const uint8_t *e, int o) { return (uint32_t)(e[o] | e[o + 1] << 8 | e[o + 2] << 16 | (uint32_t)e[o + 3] << 24); }
static void p16(uint8_t *e, int o, uint16_t v) { e[o] = (uint8_t)v; e[o + 1] = (uint8_t)(v >> 8); }
static void p32(uint8_t *e, int o, uint32_t v) { for (int k = 0; k < 4; k++) e[o + k] = (uint8_t)(v >> (8 * k)); }

/* Ordine dei byte delle IOCB: il firmware che carica la PROM le vuole
   little-endian come Linux; quello che carica IRIX in parole da 32 bit
   big-endian (ogni gruppo di 4 byte rovesciato, CDB compreso). Si capisce
   dall'intestazione: tipo nel byte 0 e conteggio nel byte 1, o tipo nel
   byte 3 e conteggio nel 2. Le risposte seguono l'ordine dell'ultima
   richiesta; i dati SCSI non si toccano. */
static void swap_words(uint8_t *e, int n)
{
    for (int k = 0; k < n; k += 4) {
        uint8_t t = e[k]; e[k] = e[k + 3]; e[k + 3] = t;
        t = e[k + 1]; e[k + 1] = e[k + 2]; e[k + 2] = t;
    }
}

static int valid_type(uint8_t t) { return t == 1 || t == 2 || t == 4 || t == 5 || t == 9 || t == 0xA; }

static void entry_order(QlIsp *q, uint8_t *e)
{
    if (valid_type(e[0]) && e[1]) q->be = 0;
    else if (valid_type(e[3]) && e[2]) q->be = 1;
    if (q->be) swap_words(e, 64);
}

/* ---- comandi SCSI ---- */

typedef struct Seg { uint64_t addr; uint32_t len; } Seg;

/* segmenti di dati del comando (e delle continuazioni che lo seguono) */
static int read_segments(QlIsp *q, const uint8_t *e, Seg *seg, int max)
{
    int n = 0, a64 = e[0] == 9;
    int count = g16(e, 18);
    int per = a64 ? 2 : 4, base = a64 ? 40 : 32, step = a64 ? 12 : 8;
    for (int k = 0; k < per && n < count && n < max; k++, n++) {
        const uint8_t *s = e + base + step * k;
        seg[n].addr = g32(s, 0) | (a64 ? (uint64_t)g32(s, 4) << 32 : 0);
        seg[n].len = g32(s, a64 ? 8 : 4);
    }
    for (int c = 1; c < e[1] && n < count; c++) {          /* continuazioni */
        uint8_t ce[64];
        q->dma_read(q->ctx, q->req_base + 64ull * q->req_out, ce, 64);
        q->req_out = (uint16_t)((q->req_out + 1) % (q->req_len ? q->req_len : 1));
        if (q->be) swap_words(ce, 64);
        int ca64 = ce[0] == 0xA;
        int cper = ca64 ? 5 : 7, cbase = ca64 ? 4 : 8, cstep = ca64 ? 12 : 8;
        for (int k = 0; k < cper && n < count && n < max; k++, n++) {
            const uint8_t *s = ce + cbase + cstep * k;
            seg[n].addr = g32(s, 0) | (ca64 ? (uint64_t)g32(s, 4) << 32 : 0);
            seg[n].len = g32(s, ca64 ? 8 : 4);
        }
    }
    return n;
}

static void post_response(QlIsp *q, uint8_t *r)
{
    if (!q->rsp_len) return;
    if (q->be) swap_words(r, 64);
    q->dma_write(q->ctx, q->rsp_base + 64ull * q->rsp_in, r, 64);
    q->rsp_in = (uint16_t)((q->rsp_in + 1) % q->rsp_len);
    q->mb_out[5] = q->rsp_in;
}

static void scsi_command(QlIsp *q, const uint8_t *e)
{
    Seg seg[64];
    int nseg = read_segments(q, e, seg, 64);
    uint32_t want = 0;
    for (int k = 0; k < nseg; k++) want += seg[k].len;
    int target = e[9] & (QL_TARGETS - 1), lun = e[8];
    const uint8_t *cdb = e + 20;
    uint8_t r[64];
    memset(r, 0, sizeof r);
    r[0] = 3; r[1] = 1;                                      /* STATUS_TYPE */
    p32(r, 4, g32(e, 4));                                    /* handle */
    q->cmds++;

    ScsiDev *d = q->dev[target];
    if (!d || lun) {                                         /* nessuno risponde */
        logf_(q, "ql%d: id %d lun %d: selezione scaduta (%02X)", q->bus, target, lun, cdb[0]);
        p16(r, 10, 6);                                       /* CS_TIMEOUT */
        p16(r, 12, 0x0100);                                  /* SF_GOT_BUS */
        post_response(q, r);
        return;
    }
    int to_dev = 0;
    uint32_t len = scsi_dev_data_len(d, cdb, &to_dev);
    if (len > q->buf_size) len = q->buf_size;
    if (len > want) len = want;
    if (to_dev) {                                            /* raccoglie i dati */
        uint32_t got = 0;
        for (int k = 0; k < nseg && got < len; k++) {
            uint32_t n = seg[k].len < len - got ? seg[k].len : len - got;
            q->dma_read(q->ctx, seg[k].addr, q->buf + got, n);
            got += n;
        }
    }
    uint32_t done = len;
    int st = scsi_dev_exec(d, cdb, q->buf, &done);
    if (to_dev && st == SCSI_GOOD) done = len;               /* scritti tutti quelli raccolti */
    if (!to_dev) {                                           /* sparge i dati */
        uint32_t put = 0;
        for (int k = 0; k < nseg && put < done; k++) {
            uint32_t n = seg[k].len < done - put ? seg[k].len : done - put;
            q->dma_write(q->ctx, seg[k].addr, q->buf + put, n);
            put += n;
        }
    }
    uint16_t flags = 0x0100 | 0x0200 | 0x0400 | 0x1000;     /* bus, target, CDB, stato */
    if (done) flags |= 0x0800 | 0x4000;                      /* dati trasferiti, completo */
    p16(r, 8, (uint16_t)st);
    p16(r, 10, done < want && !to_dev && st == SCSI_GOOD ? 0x15 : 0);   /* CS_DATA_UNDERRUN */
    p32(r, 20, want - done);                                 /* residuo */
    if (st == SCSI_CHECK_CONDITION) {                        /* richiesta del sense automatica */
        static const uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
        uint8_t sense[32];
        uint32_t sl = 18;
        memset(sense, 0, sizeof sense);
        scsi_dev_exec(d, rs, sense, &sl);
        memcpy(r + 32, sense, 18);
        p16(r, 18, 18);
        flags |= 0x2000;                                     /* SF_GOT_SENSE */
    }
    p16(r, 12, flags);
    logf_(q, "ql%d: id %d %02X %02X %02X %02X %02X %02X -> stato %d, %u/%u byte", q->bus, target,
          cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], st, done, want);
    post_response(q, r);
}

static void process_requests(QlIsp *q)
{
    if (!q->req_len) return;
    int posted = 0;
    while (q->req_out != (q->mb_in[4] % q->req_len)) {
        uint8_t e[64];
        q->dma_read(q->ctx, q->req_base + 64ull * q->req_out, e, 64);
        q->req_out = (uint16_t)((q->req_out + 1) % q->req_len);
        entry_order(q, e);
        if (q->log) {
            char hex[200]; int p = 0;
            for (int k = 0; k < 48; k++) p += snprintf(hex + p, sizeof hex - (size_t)p, "%02X", e[k]);
            logf_(q, "ql: iocb %s", hex);
        }
        switch (e[0]) {
        case 1: case 9: scsi_command(q, e); posted = 1; break;
        case 4: break;                                       /* marker */
        default:
            logf_(q, "ql: iocb di tipo %02X non gestita", e[0]);
            break;
        }
    }
    q->mb_out[4] = q->req_out;
    if (posted) q->istatus |= ISTAT_RISC_INT;                /* coda delle risposte aggiornata */
}

/* ---- mailbox ---- */

static void mailbox_command(QlIsp *q)
{
    uint16_t *in = q->mb_in, *out = q->mb_out;
    uint16_t st = MBS_OK;
    memcpy(out, in, sizeof q->mb_out);
    switch (in[0]) {
    case 0x00: break;                                        /* NOP */
    case 0x01: {                                             /* LOAD RAM */
        uint64_t host = (uint64_t)in[2] << 16 | in[3];
        for (uint32_t k = 0; k < in[4]; k++) {
            uint8_t w[2];
            q->dma_read(q->ctx, host + 2 * k, w, 2);
            q->ram[(uint16_t)(in[1] + k)] = (uint16_t)(w[0] | w[1] << 8);   /* parole PCI, little-endian */
        }
        break;
    }
    case 0x02: q->risc_reset = 0; break;                     /* EXECUTE FIRMWARE */
    case 0x03: {                                             /* DUMP RAM */
        uint64_t host = (uint64_t)in[2] << 16 | in[3];
        for (uint32_t k = 0; k < in[4]; k++) {
            uint16_t v = q->ram[(uint16_t)(in[1] + k)];
            uint8_t w[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
            q->dma_write(q->ctx, host + 2 * k, w, 2);
        }
        break;
    }
    case 0x04: q->ram[in[1]] = in[2]; break;                 /* WRITE RAM WORD */
    case 0x05: out[2] = q->ram[in[1]]; break;                /* READ RAM WORD */
    case 0x06: break;                                        /* MAILBOX TEST: eco */
    case 0x07: out[2] = 0; break;                            /* VERIFY CHECKSUM: il firmware somma a 0 */
    case 0x08:                                               /* ABOUT FIRMWARE */
        out[1] = q->fw_major; out[2] = q->fw_minor; out[3] = q->fw_sub;
        break;
    case 0x10: case 0x52:                                    /* INIT REQUEST QUEUE */
        q->req_len = in[1];
        q->req_base = (uint64_t)in[2] << 16 | in[3];
        if (in[0] == 0x52) q->req_base |= (uint64_t)in[6] << 48 | (uint64_t)in[7] << 32;
        q->req_out = 0;
        out[4] = 0;
        break;
    case 0x11: case 0x53:                                    /* INIT RESPONSE QUEUE */
        q->rsp_len = in[1];
        q->rsp_base = (uint64_t)in[2] << 16 | in[3];
        if (in[0] == 0x53) q->rsp_base |= (uint64_t)in[6] << 48 | (uint64_t)in[7] << 32;
        q->rsp_in = 0;
        out[5] = 0;
        break;
    case 0x30: q->initiator_id = (uint8_t)(in[1] & 15); break;   /* SET INITIATOR ID */
    case 0x20: out[1] = q->initiator_id; break;              /* GET INITIATOR ID */
    default:
        if (in[0] >= 0x80) st = MBS_INVALID;
        break;                                               /* parametri: accettati */
    }
    out[0] = st;
    logf_(q, "ql%d: mailbox %02X %04X %04X %04X %04X -> %04X", q->bus, in[0], in[1], in[2], in[3], in[4], st);
    q->semaphore |= 1;
    q->istatus |= ISTAT_RISC_INT;
}

uint16_t ql_read(QlIsp *q, uint32_t off)
{
    off &= 0xFE;
    if (off >= R_MBOX && off < R_MBOX + 16) return q->mb_out[(off - R_MBOX) >> 1];
    switch (off) {
    case R_ID_L: return 0x4953;                              /* "IS" */
    case R_ID_H: return 0x5020;                              /* "P " */
    case R_CFG0: return 5;                                   /* ISP1040B */
    case R_CFG1: return q->cfg1;
    case R_ICTRL: return q->ictrl;
    case R_ISTATUS: return q->istatus;
    case R_SEMA: return q->semaphore;
    case R_NVRAM: return q->nvram & ~0x0008;                 /* niente NVRAM: DATA_IN a 0 */
    /* HCCR: bit 5 RISC in pausa; il bit 7 (interrupt verso il RISC in
       attesa) e' sempre 0, i comandi sono accettati subito */
    case R_HCCR: return (uint16_t)(q->risc_reset ? 0x20 : 0);
    default: return 0;
    }
}

void ql_write(QlIsp *q, uint32_t off, uint16_t v)
{
    off &= 0xFE;
    if (off >= R_MBOX && off < R_MBOX + 16) {
        int n = (int)(off - R_MBOX) >> 1;
        q->mb_in[n] = v;
        if (n == 4 && !q->risc_reset) process_requests(q);  /* campanello della coda */
        return;
    }
    switch (off) {
    case R_CFG1: q->cfg1 = v; break;
    case R_ICTRL:
        if (v & 1) { ql_reset(q); v &= (uint16_t)~1; }      /* reset: il bit si azzera da solo */
        q->ictrl = v;
        break;
    case R_SEMA: q->semaphore = v & 1; break;
    case R_NVRAM: q->nvram = v; break;
    case R_HCCR:
        switch (v & 0xF000) {
        case 0x1000: q->risc_reset = 1; break;               /* RESET RISC */
        case 0x2000: q->risc_reset = 1; break;               /* PAUSE */
        case 0x3000: q->risc_reset = 0; break;               /* RELEASE */
        case 0x5000: mailbox_command(q); break;              /* SET HOST INT: un comando */
        case 0x7000: q->istatus &= (uint16_t)~ISTAT_RISC_INT; break;   /* CLR RISC INT */
        default: break;
        }
        break;
    default: break;
    }
}
