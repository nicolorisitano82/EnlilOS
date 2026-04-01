/*
 * EnlilOS Microkernel - VirtIO Block Device Driver (M5-01)
 *
 * Supporta virtio-blk su virtio-mmio (QEMU virt).
 *
 * Protocollo virtio-blk (spec. virtio v1.2, sez. 5.2):
 *   Ogni richiesta usa 3 descriptor in catena:
 *     [0] READABLE → blk_req_hdr_t  (tipo, settore)
 *     [1] READ/WRITE → data buffer  (count × 512 B)
 *     [2] WRITABLE → uint8_t status (0=OK, 1=ERR, 2=UNSUP)
 *
 * Transport: virtio-mmio v2 (non-legacy).
 * Queue: requestq (id 0), profondità BLK_QUEUE_DEPTH = 16.
 *
 * RT compliance:
 *   - Tutta la memoria VQ è statica (no kmalloc).
 *   - blk_read/write_sync usa busy-wait con timeout bounded.
 *   - Nessuna IRQ registrata: il driver usa polling sul used ring.
 *     L'assenza di IRQ è intenzionale: il server blk gira a bassa
 *     priorità e il busy-wait non interferisce con task RT.
 */

#include "blk.h"
#include "virtio_mmio.h"
#include "mmu.h"
#include "uart.h"

/* ── Strutture vring per la request queue ────────────────────────── */

#define BVQ_SIZE    BLK_QUEUE_DEPTH

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[BVQ_SIZE];
    uint16_t used_event;
} __attribute__((packed)) bvring_avail_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[BVQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) bvring_used_t;

/* ── Strutture virtio-blk ─────────────────────────────────────────── */

/* Tipi di richiesta */
#define VIRTIO_BLK_T_IN         0U    /* read  */
#define VIRTIO_BLK_T_OUT        1U    /* write */
#define VIRTIO_BLK_T_FLUSH      4U    /* flush cache */

/* Status byte risposta */
#define VIRTIO_BLK_S_OK         0U
#define VIRTIO_BLK_S_IOERR      1U
#define VIRTIO_BLK_S_UNSUPP     2U

/* Header richiesta (16 byte) */
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} blk_req_hdr_t;

/* Config space virtio-blk (offset 0x100 nel MMIO) */
typedef struct __attribute__((packed)) {
    uint64_t capacity;          /* settori totali */
    uint32_t size_max;
    uint32_t seg_max;
    uint16_t cylinders;
    uint8_t  heads;
    uint8_t  sectors;
    uint32_t blk_size;
    uint8_t  physical_block_exp;
    uint8_t  alignment_offset;
    uint16_t min_io_size;
    uint32_t opt_io_size;
} blk_config_t;

/* ── Memoria VQ statica ───────────────────────────────────────────── */

/*
 * Layout pagina 4KB:
 *   offset   0 – 255 : vring_desc_t[16]    (16 × 16B = 256B)
 *   offset 256 – 295 : bvring_avail_t       (38B)
 *   offset 512 – 647 : bvring_used_t        (134B)
 */
static uint8_t bvq_mem[4096] __attribute__((aligned(4096)));

#define BVQ_DESC  ((vring_desc_t  *)(bvq_mem + 0))
#define BVQ_AVAIL ((bvring_avail_t *)(bvq_mem + 256))
#define BVQ_USED  ((bvring_used_t  *)(bvq_mem + 512))

/*
 * Per ogni slot in-flight: header richiesta + status byte.
 * I buffer dati vengono passati direttamente dal chiamante.
 * Allineati a 16B per efficienza DMA.
 */
static blk_req_hdr_t bvq_hdrs  [BVQ_SIZE] __attribute__((aligned(16)));
static uint8_t       bvq_status[BVQ_SIZE] __attribute__((aligned(4)));

/* ── Stato driver ─────────────────────────────────────────────────── */

static uintptr_t bvq_base;        /* MMIO base; 0 = non trovato       */
static uint64_t  bvq_capacity;    /* settori totali da config space    */
static uint16_t  bvq_queue_size;  /* profondità effettiva negoziata    */
static uint16_t  bvq_next_avail;  /* prossimo idx avail ring           */
static uint16_t  bvq_last_used;   /* ultimo idx used ring consumato    */

/* Descriptor liberi: free list semplice (ring circolare) */
static uint16_t  bvq_free_head;   /* primo descriptor libero           */
static uint16_t  bvq_free_count;  /* quanti descriptor liberi          */

/* ── MMIO helpers ─────────────────────────────────────────────────── */

static inline uint32_t bv_rd(uint32_t off)
{
    return *(volatile uint32_t *)(bvq_base + off);
}

static inline void bv_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(bvq_base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint64_t bv_cfg_read64(uint32_t off)
{
    uint32_t lo = *(volatile uint32_t *)(bvq_base + VMMIO_CONFIG + off);
    uint32_t hi = *(volatile uint32_t *)(bvq_base + VMMIO_CONFIG + off + 4U);
    return ((uint64_t)hi << 32) | lo;
}

/* ── Descriptor free list (init lineare, poi LIFO) ───────────────── */

static void bvq_free_list_init(void)
{
    /* Chain: desc[0]→desc[1]→...→desc[N-1]→0xFFFF */
    for (uint16_t i = 0U; i < bvq_queue_size - 1U; i++)
        BVQ_DESC[i].next = (uint16_t)(i + 1U);
    BVQ_DESC[bvq_queue_size - 1U].next = 0xFFFFU;
    bvq_free_head  = 0U;
    bvq_free_count = bvq_queue_size;
}

/* Alloca 3 descriptor contigui dalla free list. Ritorna head o 0xFFFF. */
static uint16_t bvq_alloc3(void)
{
    if (bvq_free_count < 3U)
        return 0xFFFFU;

    uint16_t d0 = bvq_free_head;
    uint16_t d1 = BVQ_DESC[d0].next;
    uint16_t d2 = BVQ_DESC[d1].next;
    bvq_free_head   = BVQ_DESC[d2].next;
    bvq_free_count -= 3U;

    /* Azzera il campo next del terzo (termina la catena) */
    BVQ_DESC[d2].next = 0xFFFFU;
    return d0;
}

/* Rimette in testa alla free list i 3 descriptor usati. */
static void bvq_free3(uint16_t d0)
{
    uint16_t d1 = BVQ_DESC[d0].next;
    uint16_t d2 = BVQ_DESC[d1].next;

    BVQ_DESC[d2].next = bvq_free_head;
    bvq_free_head  = d0;
    bvq_free_count += 3U;
}

/* ── Sottomissione richiesta ──────────────────────────────────────── */

/*
 * bvq_submit() — alloca 3 descriptor, compila la catena e notifica il device.
 * Ritorna l'indice del descriptor head, o 0xFFFF su errore.
 *
 * Struttura catena:
 *   desc[d0] → hdr   (READONLY, NEXT)
 *   desc[d1] → data  (READ/WRITE secondo tipo, NEXT)
 *   desc[d2] → status (WRITE, no NEXT)
 */
static uint16_t bvq_submit(uint32_t type, uint64_t sector,
                           void *data_buf, uint32_t data_len,
                           uint16_t *out_d0)
{
    uint16_t d0 = bvq_alloc3();
    if (d0 == 0xFFFFU)
        return 0xFFFFU;

    uint16_t d1 = BVQ_DESC[d0].next;
    uint16_t d2 = BVQ_DESC[d1].next;

    /* d0: header richiesta (device legge) */
    bvq_hdrs[d0].type     = type;
    bvq_hdrs[d0].reserved = 0U;
    bvq_hdrs[d0].sector   = sector;

    BVQ_DESC[d0].addr  = (uint64_t)(uintptr_t)&bvq_hdrs[d0];
    BVQ_DESC[d0].len   = (uint32_t)sizeof(blk_req_hdr_t);
    BVQ_DESC[d0].flags = VRING_DESC_F_NEXT;   /* READABLE, chain */
    BVQ_DESC[d0].next  = d1;

    /* d1: buffer dati */
    BVQ_DESC[d1].addr  = (uint64_t)(uintptr_t)data_buf;
    BVQ_DESC[d1].len   = data_len;
    BVQ_DESC[d1].flags = (uint16_t)(VRING_DESC_F_NEXT |
                          (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0U));
    BVQ_DESC[d1].next  = d2;

    /* d2: status byte (device scrive) */
    bvq_status[d0] = 0xFFU;   /* valore sentinella pre-completamento */
    BVQ_DESC[d2].addr  = (uint64_t)(uintptr_t)&bvq_status[d0];
    BVQ_DESC[d2].len   = 1U;
    BVQ_DESC[d2].flags = VRING_DESC_F_WRITE;
    BVQ_DESC[d2].next  = 0U;

    /* Flush descriptors, header e buf dati in memoria fisica */
    cache_flush_range((uintptr_t)&BVQ_DESC[d0], sizeof(vring_desc_t) * 3U);
    cache_flush_range((uintptr_t)&bvq_hdrs[d0], sizeof(blk_req_hdr_t));
    if (type == VIRTIO_BLK_T_OUT)
        cache_flush_range((uintptr_t)data_buf, data_len);

    /* Pubblica sul avail ring */
    BVQ_AVAIL->ring[bvq_next_avail % bvq_queue_size] = d0;
    __asm__ volatile("dmb sy" ::: "memory");
    bvq_next_avail++;
    BVQ_AVAIL->idx = bvq_next_avail;
    cache_flush_range((uintptr_t)BVQ_AVAIL, sizeof(*BVQ_AVAIL));

    bv_wr(VMMIO_QUEUE_NOTIFY, 0U);

    if (out_d0) *out_d0 = d0;
    return d0;
}

/* ── Polling completamento ────────────────────────────────────────── */

/*
 * bvq_poll_complete(d0) — attende che il descriptor d0 appaia nel used ring.
 * Busy-wait bounded a BLK_POLL_TIMEOUT cicli.
 * Ritorna il codice di status virtio-blk (0=OK) o BLK_ERR_TIMEOUT.
 */
static int bvq_poll_complete(uint16_t d0)
{
    uint32_t timeout = BLK_POLL_TIMEOUT;

    while (timeout-- > 0U) {
        cache_invalidate_range((uintptr_t)BVQ_USED, sizeof(*BVQ_USED));

        if (BVQ_USED->idx != bvq_last_used) {
            /* Avanza il cursore used: cerca il nostro d0 */
            while (bvq_last_used != BVQ_USED->idx) {
                vring_used_elem_t elem =
                    BVQ_USED->ring[bvq_last_used % bvq_queue_size];
                bvq_last_used++;

                if ((uint16_t)elem.id == d0) {
                    /* Leggi status: invalida prima il byte in cache */
                    cache_invalidate_range(
                        (uintptr_t)&bvq_status[d0], 1U);
                    uint8_t st = bvq_status[d0];
                    bvq_free3(d0);

                    if (st == VIRTIO_BLK_S_OK)    return BLK_OK;
                    if (st == VIRTIO_BLK_S_IOERR)  return BLK_ERR_IO;
                    return BLK_ERR_IO;
                }
            }
        }

        /* Throttle: controlla IRQ_STATUS per non perdere eventi */
        if (bv_rd(VMMIO_IRQ_STATUS) != 0U)
            bv_wr(VMMIO_IRQ_ACK, bv_rd(VMMIO_IRQ_STATUS));
    }

    bvq_free3(d0);
    return BLK_ERR_TIMEOUT;
}

/* ── Probe e inizializzazione ─────────────────────────────────────── */

static uintptr_t bvq_find_device(void)
{
    for (uint32_t slot = 0U; slot < VMMIO_MAX_SLOTS; slot++) {
        uintptr_t base = VMMIO_BASE + (uintptr_t)(slot * VMMIO_SLOT_SIZE);
        volatile uint32_t *p = (volatile uint32_t *)base;

        if (p[VMMIO_MAGIC / 4]     != VMMIO_MAGIC_VALUE) continue;
        if (p[VMMIO_VERSION / 4]   != 2U)                 continue;
        if (p[VMMIO_DEVICE_ID / 4] != VIRTIO_DEVICE_BLOCK) continue;

        return base;
    }
    return 0U;
}

static int bvq_transport_init(void)
{
    /* Reset device */
    bv_wr(VMMIO_STATUS, 0U);
    bv_wr(VMMIO_STATUS, VSTAT_ACKNOWLEDGE);
    bv_wr(VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    /* Negozia feature: accettiamo solo VIRTIO_F_VERSION_1 */
    bv_wr(VMMIO_DRV_FEAT_SEL, 0U);
    bv_wr(VMMIO_DRV_FEATURES, 0U);
    bv_wr(VMMIO_DRV_FEAT_SEL, 1U);
    bv_wr(VMMIO_DRV_FEATURES, VIRTIO_F_VERSION_1);

    bv_wr(VMMIO_STATUS,
          VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    if (!(bv_rd(VMMIO_STATUS) & VSTAT_FEATURES_OK)) {
        bv_wr(VMMIO_STATUS, VSTAT_FAILED);
        uart_puts("[BLK] ERR: FEATURES_OK non accettato\n");
        return 0;
    }

    /* Negozia dimensione queue */
    bv_wr(VMMIO_QUEUE_SEL, 0U);
    {
        uint32_t qmax = bv_rd(VMMIO_QUEUE_NUM_MAX);
        if (qmax == 0U) {
            bv_wr(VMMIO_STATUS, VSTAT_FAILED);
            uart_puts("[BLK] ERR: QUEUE_NUM_MAX=0\n");
            return 0;
        }
        bvq_queue_size = (qmax < BVQ_SIZE) ? (uint16_t)qmax : (uint16_t)BVQ_SIZE;
    }
    bv_wr(VMMIO_QUEUE_NUM, bvq_queue_size);

    /* Configura indirizzi fisici VQ */
    {
        uint64_t desc_pa  = (uint64_t)(uintptr_t)(bvq_mem + 0);
        uint64_t avail_pa = (uint64_t)(uintptr_t)(bvq_mem + 256);
        uint64_t used_pa  = (uint64_t)(uintptr_t)(bvq_mem + 512);

        bv_wr(VMMIO_QUEUE_DESC_LO, (uint32_t)desc_pa);
        bv_wr(VMMIO_QUEUE_DESC_HI, (uint32_t)(desc_pa  >> 32));
        bv_wr(VMMIO_QUEUE_DRV_LO,  (uint32_t)avail_pa);
        bv_wr(VMMIO_QUEUE_DRV_HI,  (uint32_t)(avail_pa >> 32));
        bv_wr(VMMIO_QUEUE_DEV_LO,  (uint32_t)used_pa);
        bv_wr(VMMIO_QUEUE_DEV_HI,  (uint32_t)(used_pa  >> 32));
    }

    /* Inizializza free list e avail ring */
    bvq_free_list_init();
    bvq_next_avail = 0U;
    bvq_last_used  = 0U;
    BVQ_AVAIL->flags = 1U;   /* VRING_AVAIL_F_NO_INTERRUPT — no IRQ */
    BVQ_AVAIL->idx   = 0U;
    BVQ_USED->flags  = 0U;
    BVQ_USED->idx    = 0U;

    /* Flush iniziale dell'intera pagina VQ */
    cache_flush_range((uintptr_t)bvq_mem, sizeof(bvq_mem));
    cache_flush_range((uintptr_t)bvq_hdrs, sizeof(bvq_hdrs));
    cache_flush_range((uintptr_t)bvq_status, sizeof(bvq_status));

    bv_wr(VMMIO_QUEUE_READY, 1U);
    bv_wr(VMMIO_STATUS,
          VSTAT_ACKNOWLEDGE | VSTAT_DRIVER |
          VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);

    return 1;
}

/* ── API pubblica ─────────────────────────────────────────────────── */

int blk_init(void)
{
    bvq_base = bvq_find_device();
    if (!bvq_base) {
        uart_puts("[BLK] Nessun virtio-blk trovato\n");
        return 0;
    }

    if (!bvq_transport_init()) {
        bvq_base = 0U;
        return 0;
    }

    /* Legge la capacità dal config space */
    bvq_capacity = bv_cfg_read64(0U);   /* offset 0 = capacity */

    uart_puts("[BLK] VirtIO-blk pronto: ");
    {
        /* Stampa capacity in settori */
        static const char hex[] = "0123456789ABCDEF";
        uint64_t v = bvq_capacity;
        char buf[20];
        int pos = 0;
        if (v == 0U) { buf[pos++] = '0'; }
        else {
            while (v > 0ULL && pos < (int)sizeof(buf) - 1) {
                buf[pos++] = '0' + (char)(v % 10ULL);
                v /= 10ULL;
            }
        }
        for (int i = pos - 1; i >= 0; i--)
            uart_putc(buf[i]);
        (void)hex;
    }
    uart_puts(" settori × 512B, queue depth=");
    uart_putc('0' + (char)(bvq_queue_size / 10U % 10U));
    uart_putc('0' + (char)(bvq_queue_size % 10U));
    uart_puts("\n");

    return 1;
}

int blk_is_ready(void)
{
    return bvq_base != 0U;
}

uint64_t blk_sector_count(void)
{
    return bvq_base ? bvq_capacity : 0ULL;
}

int blk_read_sync(uint64_t sector, void *buf, uint32_t count)
{
    if (!bvq_base)           return BLK_ERR_NOT_READY;
    if (!buf || count == 0U) return BLK_ERR_RANGE;
    if (sector + count > bvq_capacity) return BLK_ERR_RANGE;

    uint16_t d0;
    if (bvq_submit(VIRTIO_BLK_T_IN, sector, buf,
                   count * BLK_SECTOR_SIZE, &d0) == 0xFFFFU)
        return BLK_ERR_BUSY;

    int rc = bvq_poll_complete(d0);

    /* Invalida il buffer dati: il device ha scritto in RAM */
    if (rc == BLK_OK)
        cache_invalidate_range((uintptr_t)buf, count * BLK_SECTOR_SIZE);

    return rc;
}

int blk_write_sync(uint64_t sector, const void *buf, uint32_t count)
{
    if (!bvq_base)           return BLK_ERR_NOT_READY;
    if (!buf || count == 0U) return BLK_ERR_RANGE;
    if (sector + count > bvq_capacity) return BLK_ERR_RANGE;

    uint16_t d0;
    if (bvq_submit(VIRTIO_BLK_T_OUT, sector, (void *)(uintptr_t)buf,
                   count * BLK_SECTOR_SIZE, &d0) == 0xFFFFU)
        return BLK_ERR_BUSY;

    return bvq_poll_complete(d0);
}

int blk_flush_sync(void)
{
    static uint8_t dummy;
    uint16_t d0;

    if (!bvq_base) return BLK_ERR_NOT_READY;

    if (bvq_submit(VIRTIO_BLK_T_FLUSH, 0ULL, &dummy, 1U, &d0) == 0xFFFFU)
        return BLK_ERR_BUSY;

    return bvq_poll_complete(d0);
}
