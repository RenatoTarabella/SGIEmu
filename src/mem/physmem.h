/*
 * physmem.h - Spazio degli indirizzi fisici
 *
 * Tabella a pagine da 4 KB su 4 GB fisici (all'Indy bastano: la RAM sta
 * sotto 0x40000000). Ogni pagina e' RAM/ROM, con un puntatore diretto alla
 * memoria host, oppure I/O, servita da un dispositivo con le sue callback.
 *
 * La memoria host e' in ordine big-endian, come la vede la CPU: una load
 * da 32 bit e' un bswap della parola host. Cosi' i DMA e le istruzioni
 * LWL/LWR lavorano sui byte nell'ordine naturale.
 *
 * Il flag PM_CODE marca le pagine fisiche che contengono codice gia'
 * tradotto (interprete pre-decodificato, JIT): le scritture su quelle
 * pagine non passano dalla via veloce e chiamano code_write.
 */
#ifndef PHYSMEM_H
#define PHYSMEM_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PM_PAGE_SHIFT 12
#define PM_PAGE_SIZE  4096u
#define PM_PAGE_MASK  0xFFFu
#define PM_NPAGES     (1u << 20)

enum {
    PM_RAM  = 1,    /* leggibile e scrivibile                     */
    PM_ROM  = 2,    /* sola lettura: le scritture vanno a unmapped_write */
    PM_CODE = 4     /* contiene codice tradotto                   */
};

/* Callback dei dispositivi: size = 1, 2, 4, 8; il valore e' allineato a
   destra. Ritornano 0 se va bene, altro per un bus error. */
typedef int (*PmIoRead) (void *ctx, uint32_t pa, int size, uint64_t *val);
typedef int (*PmIoWrite)(void *ctx, uint32_t pa, int size, uint64_t val);

typedef struct PmIo {
    uint32_t    base, size;
    void       *ctx;
    PmIoRead    read;
    PmIoWrite   write;
    const char *name;
} PmIo;

#define PM_MAX_IO 255

typedef struct PhysMem {
    uint8_t **host;             /* PM_NPAGES puntatori all'inizio pagina */
    uint8_t  *flags;            /* PM_RAM / PM_ROM / PM_CODE             */
    uint8_t  *io;               /* indice in ios[] + 1, 0 = niente       */
    PmIo      ios[PM_MAX_IO];
    int       nio;

    /* accessi dove non c'e' niente: di default bus error in lettura,
       scrittura ignorata */
    PmIoRead  unmapped_read;
    PmIoWrite unmapped_write;
    void     *unmapped_ctx;

    /* scrittura su una pagina PM_CODE (dopo averla eseguita) */
    void    (*code_write)(void *ctx, uint32_t pa);
    void     *code_ctx;
} PhysMem;

int  pm_init(PhysMem *pm);
void pm_free(PhysMem *pm);

/* Mappa memoria host (big-endian) su [base, base+size); size multiplo di 4 KB. */
void pm_map_ram(PhysMem *pm, uint32_t base, uint32_t size, uint8_t *host, int flags);
void pm_unmap(PhysMem *pm, uint32_t base, uint32_t size);
int  pm_map_io(PhysMem *pm, uint32_t base, uint32_t size, void *ctx,
               PmIoRead rd, PmIoWrite wr, const char *name);

/* Accesso generico (RAM, ROM o I/O) per DMA e vie lente. 0 = ok. */
int  pm_read (PhysMem *pm, uint64_t pa, int size, uint64_t *val);
int  pm_write(PhysMem *pm, uint64_t pa, int size, uint64_t val);

/* Blocchi per il DMA dei dispositivi (big-endian = ordine dei byte in
   memoria). Ritornano i byte trasferiti; si fermano dove non c'e' RAM. */
uint32_t pm_dma_read (PhysMem *pm, uint64_t pa, uint8_t *dst, uint32_t len);
uint32_t pm_dma_write(PhysMem *pm, uint64_t pa, const uint8_t *src, uint32_t len);

/* Nome del dispositivo che serve pa (per i log), o NULL. */
const char *pm_io_name(const PhysMem *pm, uint64_t pa);

static inline uint8_t *pm_host(const PhysMem *pm, uint64_t pa)
{
    if (pa >> 32) return NULL;
    uint8_t *p = pm->host[pa >> PM_PAGE_SHIFT];
    return p ? p + (pa & PM_PAGE_MASK) : NULL;
}

/* ---- ordine dei byte: memoria big-endian su host little-endian ---- */
#if defined(_MSC_VER)
#include <stdlib.h>
#define BSWAP16(x) _byteswap_ushort(x)
#define BSWAP32(x) _byteswap_ulong(x)
#define BSWAP64(x) _byteswap_uint64(x)
#else
#define BSWAP16(x) __builtin_bswap16(x)
#define BSWAP32(x) __builtin_bswap32(x)
#define BSWAP64(x) __builtin_bswap64(x)
#endif

static inline uint16_t be16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return BSWAP16(v); }
static inline uint32_t be32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return BSWAP32(v); }
static inline uint64_t be64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return BSWAP64(v); }
static inline void wbe16(uint8_t *p, uint16_t v) { v = BSWAP16(v); memcpy(p, &v, 2); }
static inline void wbe32(uint8_t *p, uint32_t v) { v = BSWAP32(v); memcpy(p, &v, 4); }
static inline void wbe64(uint8_t *p, uint64_t v) { v = BSWAP64(v); memcpy(p, &v, 8); }

#endif
