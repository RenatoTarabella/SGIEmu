/*
 * scsi_dev.c - Disco e CD-ROM SCSI su file immagine (blocchi da 512 byte)
 */
#include <string.h>
#include "indy/scsi_dev.h"

#if defined(_MSC_VER)
#define fseek64 _fseeki64
#define ftell64 _ftelli64
#else
#define fseek64 fseeko
#define ftell64 ftello
#endif

#define BLK 512u
#define HEADS 16u
#define SECTORS 63u

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

static void pad(char *dst, const char *src, int n)
{
    int k = 0;
    for (; k < n && src[k]; k++) dst[k] = src[k];
    for (; k < n; k++) dst[k] = ' ';
    dst[n] = 0;
}

int scsi_dev_open(ScsiDev *d, int type, const char *path, int readonly)
{
    memset(d, 0, sizeof *d);
    d->type = type;
    d->readonly = readonly || type == SCSI_CDROM;
    d->f = fopen(path, d->readonly ? "rb" : "r+b");
    if (!d->f) return -1;
    fseek64(d->f, 0, SEEK_END);
    d->blocks = (uint64_t)ftell64(d->f) / BLK;
    if (type == SCSI_CDROM) {
        pad(d->vendor, "TOSHIBA", 8); pad(d->product, "CD-ROM XM-3401TA", 16); pad(d->revision, "0283", 4);
    } else {
        pad(d->vendor, "SGI", 8); pad(d->product, "SEAGATE ST39173N", 16); pad(d->revision, "6264", 4);
    }
    return 0;
}

void scsi_dev_close(ScsiDev *d)
{
    if (d->f) fclose(d->f);
    d->f = NULL;
}

static int check(ScsiDev *d, uint8_t key, uint8_t asc)
{
    d->sense_key = key; d->asc = asc; d->ascq = 0;
    return SCSI_CHECK_CONDITION;
}

/* LBA e numero di blocchi dei comandi di lettura/scrittura */
static int rw_args(const uint8_t *c, uint64_t *lba, uint32_t *n)
{
    switch (c[0]) {
    case 0x08: case 0x0A:
        *lba = ((uint32_t)(c[1] & 0x1F) << 16) | get16(c + 2);
        *n = c[4] ? c[4] : 256;
        return 1;
    case 0x28: case 0x2A: case 0x2E: case 0x2F:
        *lba = get32(c + 2);
        *n = get16(c + 7);
        return 1;
    case 0xA8: case 0xAA:
        *lba = get32(c + 2);
        *n = get32(c + 6);
        return 1;
    default:
        return 0;
    }
}

uint32_t scsi_dev_data_len(ScsiDev *d, const uint8_t *c, int *to_dev)
{
    uint64_t lba; uint32_t n;
    *to_dev = 0;
    switch (c[0]) {
    case 0x03: case 0x12: case 0x1A: return c[4];
    case 0x5A: case 0x43: case 0x42: case 0x37: case 0x3C: return get16(c + 7);
    case 0x25: return 8;
    case 0x15: *to_dev = 1; return c[4];
    case 0x55: *to_dev = 1; return get16(c + 7);
    case 0x3B: *to_dev = 1; return get16(c + 7);
    case 0x08: case 0x28: case 0xA8:
        rw_args(c, &lba, &n);
        return n * BLK;
    case 0x0A: case 0x2A: case 0x2E: case 0xAA:
        rw_args(c, &lba, &n);
        *to_dev = 1;
        return n * BLK;
    default:
        (void)d;
        return 0;
    }
}

static uint32_t mode_pages(ScsiDev *d, int page, uint8_t *p)
{
    uint32_t len = 0;
    uint32_t cyl = (uint32_t)(d->blocks / (HEADS * SECTORS));
    if (page == 0x01 || page == 0x3F) {                 /* recupero errori */
        uint8_t *q = p + len; memset(q, 0, 12); q[0] = 0x01; q[1] = 10; len += 12;
    }
    if (page == 0x02 || page == 0x3F) {                 /* disconnessione */
        uint8_t *q = p + len; memset(q, 0, 16); q[0] = 0x02; q[1] = 14; len += 16;
    }
    if (page == 0x03 || page == 0x3F) {                 /* formato */
        uint8_t *q = p + len; memset(q, 0, 24); q[0] = 0x03; q[1] = 22;
        put16(q + 10, SECTORS); put16(q + 12, BLK); len += 24;
    }
    if (page == 0x04 || page == 0x3F) {                 /* geometria */
        uint8_t *q = p + len; memset(q, 0, 24); q[0] = 0x04; q[1] = 22;
        q[2] = (uint8_t)(cyl >> 16); q[3] = (uint8_t)(cyl >> 8); q[4] = (uint8_t)cyl; q[5] = HEADS;
        put16(q + 20, 5400); len += 24;
    }
    if (page == 0x08 || page == 0x3F) {                 /* cache */
        uint8_t *q = p + len; memset(q, 0, 12); q[0] = 0x08; q[1] = 10; q[2] = 0x04; len += 12;
    }
    return len;
}

int scsi_dev_exec(ScsiDev *d, const uint8_t *c, uint8_t *buf, uint32_t *len)
{
    uint32_t alloc = *len;
    uint64_t lba; uint32_t n;
    *len = 0;
    switch (c[0]) {
    case 0x00: case 0x01: case 0x04: case 0x0B: case 0x16: case 0x17:
    case 0x1B: case 0x1D: case 0x1E: case 0x2B: case 0x2F: case 0x35:
        break;                                            /* niente dati: tutto bene */
    case 0x03: {                                          /* REQUEST SENSE */
        uint8_t s[18] = { 0x70, 0, d->sense_key, 0, 0, 0, 0, 10, 0, 0, 0, 0, d->asc, d->ascq };
        *len = alloc < 18 ? alloc : 18;
        memcpy(buf, s, *len);
        d->sense_key = d->asc = d->ascq = 0;
        return SCSI_GOOD;
    }
    case 0x12: {                                          /* INQUIRY */
        uint8_t q[36];
        memset(q, 0, sizeof q);
        if (c[1] & 1) return check(d, 5, 0x24);           /* pagine VPD: no */
        q[0] = (uint8_t)d->type;
        q[1] = d->type == SCSI_CDROM ? 0x80 : 0;
        q[2] = 2; q[3] = 2; q[4] = 31;
        q[7] = 0x10;                                      /* sincrono */
        memcpy(q + 8, d->vendor, 8);
        memcpy(q + 16, d->product, 16);
        memcpy(q + 32, d->revision, 4);
        *len = alloc < 36 ? alloc : 36;
        memcpy(buf, q, *len);
        break;
    }
    case 0x1A: case 0x5A: {                               /* MODE SENSE (6) e (10) */
        uint8_t m[256];
        int ten = c[0] == 0x5A, h = ten ? 8 : 4;
        memset(m, 0, sizeof m);
        uint8_t *bd = m + h;
        int with_bd = !(c[1] & 0x08);
        if (with_bd) {
            uint32_t nb = d->blocks > 0xFFFFFF ? 0xFFFFFF : (uint32_t)d->blocks;
            bd[1] = (uint8_t)(nb >> 16); bd[2] = (uint8_t)(nb >> 8); bd[3] = (uint8_t)nb;
            bd[5] = 0; bd[6] = (uint8_t)((BLK >> 8) & 0xFF); bd[7] = (uint8_t)(BLK & 0xFF);
        }
        uint32_t pl = mode_pages(d, c[2] & 0x3F, bd + (with_bd ? 8 : 0));
        uint32_t total = (uint32_t)h + (with_bd ? 8 : 0) + pl;
        if (ten) { put16(m, total - 2); m[3] = d->readonly ? 0x80 : 0; put16(m + 6, with_bd ? 8 : 0); }
        else { m[0] = (uint8_t)(total - 1); m[2] = d->readonly ? 0x80 : 0; m[3] = with_bd ? 8 : 0; }
        *len = alloc < total ? alloc : total;
        memcpy(buf, m, *len);
        break;
    }
    case 0x15: case 0x55: case 0x3B:                      /* MODE SELECT, WRITE BUFFER */
        break;
    case 0x25: {                                          /* READ CAPACITY */
        put32(buf, (uint32_t)(d->blocks ? d->blocks - 1 : 0));
        put32(buf + 4, BLK);
        *len = 8;
        break;
    }
    case 0x37: {                                          /* READ DEFECT DATA: lista vuota */
        uint8_t q[4] = { 0, c[2] & 0x1F, 0, 0 };
        *len = alloc < 4 ? alloc : 4;
        memcpy(buf, q, *len);
        break;
    }
    case 0x3C:                                            /* READ BUFFER */
        memset(buf, 0, alloc < 4 ? alloc : 4);
        *len = alloc < 4 ? alloc : 4;
        break;
    case 0x08: case 0x28: case 0xA8:                      /* READ */
        rw_args(c, &lba, &n);
        if (lba + n > d->blocks) return check(d, 5, 0x21);
        fseek64(d->f, (int64_t)(lba * BLK), SEEK_SET);
        *len = (uint32_t)fread(buf, 1, (size_t)n * BLK, d->f);
        if (*len < n * BLK) { memset(buf + *len, 0, n * BLK - *len); *len = n * BLK; }
        d->reads += n;
        break;
    case 0x0A: case 0x2A: case 0x2E: case 0xAA:           /* WRITE */
        rw_args(c, &lba, &n);
        if (d->readonly) return check(d, 7, 0x27);
        if (lba + n > d->blocks) return check(d, 5, 0x21);
        fseek64(d->f, (int64_t)(lba * BLK), SEEK_SET);
        fwrite(buf, 1, (size_t)n * BLK, d->f);
        d->writes += n;
        break;
    case 0x43: {                                          /* READ TOC: una traccia dati */
        uint8_t t[20];
        memset(t, 0, sizeof t);
        put16(t, 18); t[2] = 1; t[3] = 1;
        t[5] = 0x14; t[6] = 1;                            /* traccia 1, dati, da LBA 0 */
        t[13] = 0x14; t[14] = 0xAA;                       /* lead-out */
        put32(t + 16, (uint32_t)(d->blocks / 4));
        *len = alloc < 20 ? alloc : 20;
        memcpy(buf, t, *len);
        break;
    }
    default:
        return check(d, 5, 0x20);                         /* comando sconosciuto */
    }
    d->sense_key = d->asc = d->ascq = 0;
    return SCSI_GOOD;
}
