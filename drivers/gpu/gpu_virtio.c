/*
 * EnlilOS - VirtIO-GPU Backend (M5b-01)
 *
 * Virtio-GPU via virtio-mmio su QEMU virt machine.
 *
 * Hardware:
 *   VirtIO-MMIO bus: 0x0a000000 + slot × 0x200  (32 slot max)
 *   IRQ: GIC SPI 16+n = GIC ID 48+n per slot n — non usato (polling)
 *   VirtIO GPU device ID = 16
 *
 * Protocollo: virtio v2 (non-legacy).
 * Queue: control queue (id 0), VQ_SIZE=16, split vring.
 *
 * VirtQueue memory layout (statica, 4KB allineata):
 *   offset   0 – 255 : vring_desc_t[16]   — descriptor table (16×16B)
 *   offset 256 – 293 : vring_avail_t      — available ring (driver→device)
 *   offset 512 – 645 : vring_used_t       — used ring (device→driver)
 *
 * Sequenza GPU (lazy, prima chiamata a present):
 *   RESOURCE_CREATE_2D  id=1 e id=2, B8G8R8X8_UNORM, FB_WIDTH×FB_HEIGHT
 *   RESOURCE_ATTACH_BACKING  per due scanout buffer CPU-writable
 *   SET_SCANOUT  iniziale sul front buffer
 *
 * present():
 *   copia la dirty rect dal framebuffer staging al back buffer
 *   TRANSFER_TO_HOST_2D sul back buffer
 *   SET_SCANOUT atomico sul back buffer
 *   RESOURCE_FLUSH e swap front/back
 *   Fence completata al prossimo tick vsync (60 Hz emulato).
 *
 * RT: vq_mem, vgpu_cmd, vgpu_resp sono statici (nessuna allocazione
 *     runtime). present() ha WCET noto = poll bound × ciclo_clock.
 */

#include "gpu_internal.h"
#include "virtio_mmio.h"
#include "framebuffer.h"
#include "mmu.h"
#include "timer.h"
#include "uart.h"

/* GPU-specific feature flag */
#define VIRTIO_GPU_FLAG_FENCE (1U << 0)

/* ── Split virtqueue structures per GPU (VQ_SIZE entries) ────────── */

#define VQ_SIZE  16U

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VQ_SIZE];
    uint16_t used_event;
} __attribute__((packed)) vring_avail_t;        /* 38 bytes */

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[VQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) vring_used_t;         /* 134 bytes */

/*
 * Tutta la memoria VQ in una pagina statica 4KB-allineata.
 * Offset 0: desc table  (256 B)
 * Offset 256: avail ring (38 B)
 * Offset 512: used ring  (134 B)
 */
static uint8_t vq_mem[4096] __attribute__((aligned(4096)));

#define VQ_DESC  ((vring_desc_t  *)(vq_mem + 0))
#define VQ_AVAIL ((vring_avail_t *)(vq_mem + 256))
#define VQ_USED  ((vring_used_t  *)(vq_mem + 512))

/*
 * Command e response buffer — 8-byte aligned.
 * La risposta piu' grande che usiamo e' GET_DISPLAY_INFO:
 * hdr (24 B) + 16 scanout * 24 B = 408 B.
 */
static uint8_t vgpu_cmd [64]  __attribute__((aligned(8)));
static uint8_t vgpu_resp[512] __attribute__((aligned(8)));

/* Stato driver */
static uintptr_t vgpu_base;        /* 0 = non trovato              */
static bool      vgpu_transport;   /* trasporto inizializzato       */
static bool      vgpu_device;      /* risorsa + scanout configurati */

/* ── MMIO helpers ────────────────────────────────────────────────── */

static inline uint32_t vmmio_rd(uint32_t off)
{
    return *(volatile uint32_t *)(vgpu_base + off);
}

static inline void vmmio_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(vgpu_base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void vgpu_pr_hex32(uint32_t v)
{
    static const char h[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int s = 28; s >= 0; s -= 4)
        uart_putc(h[(v >> s) & 0xFU]);
}

static void vgpu_pr_u32(uint32_t v)
{
    char buf[10];
    int  len = 0;

    if (v == 0U) {
        uart_putc('0');
        return;
    }

    while (v != 0U && len < (int)sizeof(buf)) {
        buf[len++] = (char)('0' + (v % 10U));
        v /= 10U;
    }

    while (len-- > 0)
        uart_putc(buf[len]);
}


/* ── VirtIO GPU command types & structures ──────────────────────── */

#define VGPU_CMD_CREATE_2D          0x0101U
#define VGPU_CMD_SET_SCANOUT        0x0103U
#define VGPU_CMD_RESOURCE_FLUSH     0x0104U
#define VGPU_CMD_TRANSFER_TO_HOST   0x0105U
#define VGPU_CMD_ATTACH_BACKING     0x0106U
#define VGPU_RESP_OK_NODATA         0x1100U
#define VGPU_CMD_GET_DISPLAY_INFO   0x0100U
#define VGPU_RESP_OK_DISPLAY_INFO   0x1101U

#define VGPU_FORMAT_B8G8R8X8_UNORM  2U   /* corrisponde al layout del FB */
#define VGPU_RESOURCE_FRONT_ID      1U
#define VGPU_RESOURCE_BACK_ID       2U
#define VGPU_SCANOUT_ID             0U
#define VGPU_MAX_SCANOUTS           16U
#define VGPU_VSYNC_NS               16666666ULL

static uint32_t vgpu_scanbuf[2][FB_WIDTH * FB_HEIGHT] __attribute__((aligned(4096)));
static uint32_t vgpu_scanout_id = VGPU_SCANOUT_ID;
static uint32_t vgpu_front_idx;
static uint32_t vgpu_host_w = FB_WIDTH;
static uint32_t vgpu_host_h = FB_HEIGHT;
static uint64_t vgpu_frame_counter;
static uint64_t vgpu_next_vsync_ns;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
} __attribute__((packed)) vgpu_hdr_t;                   /* 24 B */

typedef struct {
    vgpu_hdr_t hdr;
    uint32_t   resource_id;
    uint32_t   format;
    uint32_t   width;
    uint32_t   height;
} __attribute__((packed)) vgpu_create_2d_t;             /* 40 B */

/*
 * attach_backing + mem_entry[0] in un unico buffer.
 * Il protocollo vuole che le mem_entry seguano immediatamente
 * l'intestazione attach_backing nello stesso descriptor chain.
 */
typedef struct {
    vgpu_hdr_t hdr;
    uint32_t   resource_id;
    uint32_t   nr_entries;  /* = 1 */
    uint64_t   mem_addr;    /* mem_entry[0].addr   */
    uint32_t   mem_len;     /* mem_entry[0].length */
    uint32_t   mem_pad;     /* mem_entry[0].padding */
} __attribute__((packed)) vgpu_attach_backing_t;        /* 48 B */

typedef struct {
    uint32_t x, y, w, h;
} __attribute__((packed)) vgpu_rect_t;                  /* 16 B */

typedef struct {
    vgpu_rect_t r;
    uint32_t    enabled;
    uint32_t    flags;
} __attribute__((packed)) vgpu_display_one_t;          /* 24 B */

typedef struct {
    vgpu_hdr_t          hdr;
    vgpu_display_one_t  pmodes[VGPU_MAX_SCANOUTS];
} __attribute__((packed)) vgpu_display_info_resp_t;    /* 408 B */

typedef struct {
    vgpu_hdr_t  hdr;
    vgpu_rect_t r;
    uint32_t    scanout_id;
    uint32_t    resource_id;
} __attribute__((packed)) vgpu_set_scanout_t;           /* 48 B */

typedef struct {
    vgpu_hdr_t  hdr;
    vgpu_rect_t r;
    uint64_t    offset;
    uint32_t    resource_id;
    uint32_t    padding;
} __attribute__((packed)) vgpu_transfer_t;              /* 56 B */

typedef struct {
    vgpu_hdr_t  hdr;
    vgpu_rect_t r;
    uint32_t    resource_id;
    uint32_t    padding;
} __attribute__((packed)) vgpu_flush_t;                 /* 48 B */

/* ── VirtQ send + poll ───────────────────────────────────────────── */

static int vq_send_expect(uint32_t req_size, uint32_t resp_size,
                          uint32_t expected_type);
static int vgpu_create_resource(uint32_t resource_id, uint32_t *pixels);
static int vgpu_set_scanout_resource(uint32_t resource_id);
static int vgpu_transfer_and_flush(uint32_t resource_id,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h);

/*
 * vq_send() — invia req_size byte da vgpu_cmd[], riceve in vgpu_resp[].
 * Descriptor 0 = request (device-readable).
 * Descriptor 1 = response (device-writable).
 * Poll VQ_USED->idx, bounded a ~2M iterazioni.
 * Ritorna 0 = OK (risposta RESP_OK_NODATA), -1 = errore/timeout.
 */
static int vq_send(uint32_t req_size, uint32_t resp_size)
{
    return vq_send_expect(req_size, resp_size, VGPU_RESP_OK_NODATA);
}

static void vgpu_prepare_hdr(vgpu_hdr_t *hdr, uint32_t type)
{
    static uint64_t next_fence = 1U;

    *hdr = (vgpu_hdr_t){
        .type     = type,
        .flags    = VIRTIO_GPU_FLAG_FENCE,
        .fence_id = next_fence++,
    };
}

static int vq_send_expect(uint32_t req_size, uint32_t resp_size,
                          uint32_t expected_type)
{
    VQ_DESC[0].addr  = (uint64_t)(uintptr_t)vgpu_cmd;
    VQ_DESC[0].len   = req_size;
    VQ_DESC[0].flags = VRING_DESC_F_NEXT;
    VQ_DESC[0].next  = 1;

    VQ_DESC[1].addr  = (uint64_t)(uintptr_t)vgpu_resp;
    VQ_DESC[1].len   = resp_size;
    VQ_DESC[1].flags = VRING_DESC_F_WRITE;
    VQ_DESC[1].next  = 0;

    uint16_t avail_idx = VQ_AVAIL->idx;
    uint16_t used_snap = VQ_USED->idx;

    VQ_AVAIL->ring[avail_idx % VQ_SIZE] = 0;   /* head descriptor = 0 */

    __asm__ volatile("dmb sy" ::: "memory");    /* desc prima di avail.idx */

    VQ_AVAIL->idx = (uint16_t)(avail_idx + 1U);

    __asm__ volatile("dmb sy" ::: "memory");    /* avail.idx prima di notify */

    /*
     * VirtIO legge/scrive queste strutture dalla RAM fisica, non dalla D-cache
     * della CPU. Puliamo i dati prodotti dalla CPU e invalidiamo i buffer che
     * il device dovrà aggiornare.
     */
    cache_flush_range((uintptr_t)vq_mem, sizeof(vq_mem));
    cache_flush_range((uintptr_t)vgpu_cmd, req_size);
    cache_invalidate_range((uintptr_t)vgpu_resp, resp_size);

    vmmio_wr(VMMIO_QUEUE_NOTIFY, 0);            /* control queue = 0 */

    /*
     * Poll sia sul used ring sia sul bit IRQ MMIO.
     * Alcune combinazioni QEMU/backend sono piu' affidabili se il driver
     * controlla direttamente l'avanzamento della used queue invece di
     * dipendere esclusivamente dall'IRQ status.
     */
    uint32_t timeout = 2000000U;
    while (timeout-- > 0U) {
        cache_invalidate_range((uintptr_t)VQ_USED, sizeof(*VQ_USED));
        if (VQ_USED->idx != used_snap) break;

        if (vmmio_rd(VMMIO_IRQ_STATUS) != 0U) {
            cache_invalidate_range((uintptr_t)VQ_USED, sizeof(*VQ_USED));
            if (VQ_USED->idx != used_snap) break;
        }
    }

    uint32_t isr = vmmio_rd(VMMIO_IRQ_STATUS);
    if (isr)
        vmmio_wr(VMMIO_IRQ_ACK, isr);

    cache_invalidate_range((uintptr_t)VQ_USED, sizeof(*VQ_USED));
    cache_invalidate_range((uintptr_t)vgpu_resp, resp_size);
    if (VQ_USED->idx == used_snap) {
        uart_puts("[VGPU] WARN: timeout waiting control queue completion\n");
        return -1;
    }

    vgpu_hdr_t *req  = (vgpu_hdr_t *)(void *)vgpu_cmd;
    vgpu_hdr_t *resp = (vgpu_hdr_t *)(void *)vgpu_resp;
    if ((req->flags & VIRTIO_GPU_FLAG_FENCE) != 0U) {
        if ((resp->flags & VIRTIO_GPU_FLAG_FENCE) == 0U ||
            resp->fence_id != req->fence_id) {
            uart_puts("[VGPU] WARN: fence response non coerente\n");
            return -1;
        }
    }

    if (resp->type != expected_type) {
        uart_puts("[VGPU] WARN: response type inatteso ");
        vgpu_pr_hex32(resp->type);
        uart_puts("\n");
        return -1;
    }

    return 0;
}

static uint32_t virtio_gpu_pick_scanout(void)
{
    vgpu_hdr_t *req = (vgpu_hdr_t *)(void *)vgpu_cmd;
    vgpu_prepare_hdr(req, VGPU_CMD_GET_DISPLAY_INFO);

    if (vq_send_expect((uint32_t)sizeof(vgpu_hdr_t),
                       (uint32_t)sizeof(vgpu_display_info_resp_t),
                       VGPU_RESP_OK_DISPLAY_INFO) != 0) {
        uart_puts("[VGPU] WARN: GET_DISPLAY_INFO fallito, uso scanout 0\n");
        return VGPU_SCANOUT_ID;
    }

    vgpu_display_info_resp_t *info = (vgpu_display_info_resp_t *)(void *)vgpu_resp;
    uint32_t chosen = VGPU_SCANOUT_ID;
    int found_enabled = 0;
    int found_fallback = 0;

    for (uint32_t i = 0U; i < VGPU_MAX_SCANOUTS; i++) {
        vgpu_display_one_t *mode = &info->pmodes[i];
        if (mode->enabled != 0U && mode->r.w != 0U && mode->r.h != 0U) {
            chosen = i;
            found_enabled = 1;
            break;
        }
        if (!found_fallback &&
            mode->r.w != 0U && mode->r.h != 0U) {
            chosen = i;
            found_fallback = 1;
        }
    }

    vgpu_scanout_id = chosen;
    vgpu_host_w = info->pmodes[chosen].r.w ? info->pmodes[chosen].r.w : FB_WIDTH;
    vgpu_host_h = info->pmodes[chosen].r.h ? info->pmodes[chosen].r.h : FB_HEIGHT;

    uart_puts("[VGPU] Host scanout ");
    vgpu_pr_u32(chosen);
    uart_puts(found_enabled ? " attivo " : " fallback ");
    vgpu_pr_u32(info->pmodes[chosen].r.w);
    uart_putc('x');
    vgpu_pr_u32(info->pmodes[chosen].r.h);
    uart_puts("\n");

    return chosen;
}

/* ── Transport initialisation ────────────────────────────────────── */

static uintptr_t virtio_mmio_find_gpu(void)
{
    bool saw_legacy_gpu = false;

    for (uint32_t i = 0U; i < VMMIO_MAX_SLOTS; i++) {
        uintptr_t base = VMMIO_BASE + (uintptr_t)(i * VMMIO_SLOT_SIZE);
        volatile uint32_t *p = (volatile uint32_t *)base;

        if (p[VMMIO_MAGIC    / 4] != VMMIO_MAGIC_VALUE) continue;
        if (p[VMMIO_DEVICE_ID/ 4] != VIRTIO_DEVICE_GPU) continue;

        if (p[VMMIO_VERSION / 4] == 2U)
            return base;

        if (p[VMMIO_VERSION / 4] == 1U)
            saw_legacy_gpu = true;
    }

    if (saw_legacy_gpu) {
        uart_puts("[VGPU] WARN: rilevato virtio-gpu MMIO legacy (version 1)\n");
        uart_puts("[VGPU] WARN: avvia QEMU con -global virtio-mmio.force-legacy=false\n");
    }
    return 0;
}

static bool virtio_transport_init(void)
{
    /* 1. Reset */
    vmmio_wr(VMMIO_STATUS, 0U);

    /* 2-3. ACKNOWLEDGE + DRIVER */
    vmmio_wr(VMMIO_STATUS, VSTAT_ACKNOWLEDGE);
    vmmio_wr(VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    /* 4. Negozia feature: nessuna GPU feature specifica (solo 2D).
     *    Sel=0: device-specific feature bits → scrivi 0.
     *    Sel=1: generic bits → abilita VIRTIO_F_VERSION_1. */
    vmmio_wr(VMMIO_DRV_FEAT_SEL, 0U);
    vmmio_wr(VMMIO_DRV_FEATURES, 0U);

    vmmio_wr(VMMIO_DRV_FEAT_SEL, 1U);
    vmmio_wr(VMMIO_DRV_FEATURES, VIRTIO_F_VERSION_1);

    /* 5. FEATURES_OK */
    vmmio_wr(VMMIO_STATUS,
             VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);

    /* 6. Verifica che il device abbia accettato le feature */
    if (!(vmmio_rd(VMMIO_STATUS) & VSTAT_FEATURES_OK)) {
        vmmio_wr(VMMIO_STATUS, VSTAT_FAILED);
        return false;
    }

    /* 7. Configura la control queue (queue 0) */
    vmmio_wr(VMMIO_QUEUE_SEL, 0U);

    uint32_t qmax = vmmio_rd(VMMIO_QUEUE_NUM_MAX);
    if (qmax < VQ_SIZE) {
        vmmio_wr(VMMIO_STATUS, VSTAT_FAILED);
        return false;
    }
    vmmio_wr(VMMIO_QUEUE_NUM, VQ_SIZE);

    uint64_t desc_pa  = (uint64_t)(uintptr_t)(vq_mem + 0);
    uint64_t avail_pa = (uint64_t)(uintptr_t)(vq_mem + 256);
    uint64_t used_pa  = (uint64_t)(uintptr_t)(vq_mem + 512);

    vmmio_wr(VMMIO_QUEUE_DESC_LO, (uint32_t)(desc_pa));
    vmmio_wr(VMMIO_QUEUE_DESC_HI, (uint32_t)(desc_pa  >> 32));
    vmmio_wr(VMMIO_QUEUE_DRV_LO,  (uint32_t)(avail_pa));
    vmmio_wr(VMMIO_QUEUE_DRV_HI,  (uint32_t)(avail_pa >> 32));
    vmmio_wr(VMMIO_QUEUE_DEV_LO,  (uint32_t)(used_pa));
    vmmio_wr(VMMIO_QUEUE_DEV_HI,  (uint32_t)(used_pa  >> 32));

    vmmio_wr(VMMIO_QUEUE_READY, 1U);

    /* 8. DRIVER_OK */
    vmmio_wr(VMMIO_STATUS,
             VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK |
             VSTAT_DRIVER_OK);

    return true;
}

/* ── GPU device setup (lazy, prima chiamata a present) ──────────── */

/*
 * Crea la risorsa 2D, allega il framebuffer fisico, imposta lo scanout.
 * Deve essere chiamata dopo fb_init() — usa fb_get_ptr().
 */
static void virtio_gpu_device_init(void)
{
    virtio_gpu_pick_scanout();

    if (vgpu_create_resource(VGPU_RESOURCE_FRONT_ID, vgpu_scanbuf[0]) != 0)
        return;
    if (vgpu_create_resource(VGPU_RESOURCE_BACK_ID, vgpu_scanbuf[1]) != 0)
        return;

    vgpu_front_idx = 0U;
    vgpu_frame_counter = 0U;
    vgpu_next_vsync_ns = 0U;

    if (vgpu_set_scanout_resource(VGPU_RESOURCE_FRONT_ID) != 0)
        return;
    if (vgpu_transfer_and_flush(VGPU_RESOURCE_FRONT_ID, 0U, 0U,
                                FB_WIDTH, FB_HEIGHT) != 0)
        return;

    vgpu_device = true;
    uart_puts("[VGPU] Display engine: double buffer + page flip + vsync 60Hz\n");
    uart_puts("[VGPU] Scanout 800x600 BGRX attivo su display virtio\n");
}

static void vgpu_ensure_device(void)
{
    if (!vgpu_transport || vgpu_device) return;
    virtio_gpu_device_init();
}

/* ── Backend ops ─────────────────────────────────────────────────── */

static void virtio_query_caps(gpu_caps_t *out)
{
    out->vendor          = GPU_VENDOR_VIRTIO;
    out->device_id       = 0x0010U;     /* VirtIO standard GPU device ID */
    out->vram_bytes      = 0ULL;
    out->max_texture_dim = 4096U;
    out->compute_units   = 0U;
    out->flags           = GPU_CAP_SCANOUT | GPU_CAP_HW;
}

static void virtio_query_scanout(gpu_scanout_info_t *out)
{
    if (!out) return;

    out->width         = vgpu_host_w;
    out->height        = vgpu_host_h;
    out->refresh_hz    = 60U;
    out->flags         = GPU_SCANOUT_ACTIVE |
                         GPU_SCANOUT_DOUBLE_BUFFER |
                         GPU_SCANOUT_VSYNC;
    out->front_index   = vgpu_front_idx;
    out->back_index    = vgpu_front_idx ^ 1U;
    out->frame_counter = vgpu_frame_counter;
}

static int virtio_execute_cmdbuf(gpu_cmdbuf_entry_t *cb,
                                  gpu_fence_entry_t  *fence)
{
    /*
     * VirtIO-GPU 2D non interpreta command buffer GPU opachi.
     * Accetta e segnala la fence immediatamente (compat con SW).
     */
    (void)cb;
    fence->done_ns = timer_now_ns();
    fence->state   = GPU_FENCE_SIGNALED;
    return 0;
}

static int virtio_present(gpu_buf_entry_t   *scanout,
                           uint32_t           x, uint32_t y,
                           uint32_t           w, uint32_t h,
                           gpu_fence_entry_t *fence)
{
    uint32_t front_resource;
    uint32_t back_idx;
    uint32_t back_resource;
    uint32_t *src;
    uint32_t *front_dst;
    uint32_t *dst;

    vgpu_ensure_device();

    if (!vgpu_device) {
        fence->state = GPU_FENCE_ERROR;
        return -1;
    }

    if (!scanout || x >= FB_WIDTH || y >= FB_HEIGHT) {
        fence->state = GPU_FENCE_ERROR;
        return -1;
    }

    if (x + w > FB_WIDTH)  w = FB_WIDTH - x;
    if (y + h > FB_HEIGHT) h = FB_HEIGHT - y;

    front_resource = (vgpu_front_idx == 0U) ? VGPU_RESOURCE_FRONT_ID
                                            : VGPU_RESOURCE_BACK_ID;
    back_idx = vgpu_front_idx ^ 1U;
    back_resource = (back_idx == 0U) ? VGPU_RESOURCE_FRONT_ID
                                     : VGPU_RESOURCE_BACK_ID;
    src = (uint32_t *)(uintptr_t)scanout->phys;
    front_dst = vgpu_scanbuf[vgpu_front_idx];
    dst = vgpu_scanbuf[back_idx];

    for (uint32_t row = 0U; row < h; row++) {
        uint32_t base = (y + row) * FB_WIDTH + x;
        for (uint32_t col = 0U; col < w; col++) {
            front_dst[base + col] = src[base + col];
            dst[base + col] = src[base + col];
        }
    }

    cache_flush_range((uintptr_t)front_dst, FB_WIDTH * FB_HEIGHT * 4U);
    cache_flush_range((uintptr_t)dst, FB_WIDTH * FB_HEIGHT * 4U);

    /*
     * Aggiorna anche la risorsa attualmente visibile: su alcune sequenze
     * interattive il page-flip può arrivare tardi rispetto al primo present
     * Wayland, mentre il frontbuffer viene comunque mostrato subito.
     */
    if (vgpu_transfer_and_flush(front_resource, x, y, w, h) != 0) {
        fence->state = GPU_FENCE_ERROR;
        return -1;
    }
    if (vgpu_transfer_and_flush(back_resource, x, y, w, h) != 0) {
        fence->state = GPU_FENCE_ERROR;
        return -1;
    }
    if (vgpu_set_scanout_resource(back_resource) != 0) {
        fence->state = GPU_FENCE_ERROR;
        return -1;
    }
    if (vgpu_transfer_and_flush(back_resource, x, y, w, h) != 0) {
        fence->state = GPU_FENCE_ERROR;
        return -1;
    }

    vgpu_front_idx = back_idx;
    vgpu_frame_counter++;
    if (vgpu_next_vsync_ns == 0U || timer_now_ns() >= vgpu_next_vsync_ns) {
        vgpu_next_vsync_ns = timer_now_ns() + VGPU_VSYNC_NS;
    } else {
        vgpu_next_vsync_ns += VGPU_VSYNC_NS;
    }
    fence->done_ns = vgpu_next_vsync_ns;
    fence->state   = GPU_FENCE_PENDING;
    return 0;
}

/* ── Esportazione backend ────────────────────────────────────────── */

const gpu_backend_ops_t gpu_virtio_backend = {
    .query_caps     = virtio_query_caps,
    .query_scanout  = virtio_query_scanout,
    .execute_cmdbuf = virtio_execute_cmdbuf,
    .present        = virtio_present,
};

/* ── API pubblica ────────────────────────────────────────────────── */

/*
 * virtio_gpu_init() — probe del bus virtio-mmio + init trasporto.
 * Ritorna true se trovato un device GPU e il trasporto è pronto.
 * Chiamare da gpu_backend_init() prima del check Apple Silicon.
 */
bool virtio_gpu_init(void)
{
    vgpu_base = virtio_mmio_find_gpu();
    if (!vgpu_base) return false;

    if (!virtio_transport_init()) {
        uart_puts("[VGPU] ERR: init trasporto fallita\n");
        vgpu_base = 0;
        return false;
    }

    vgpu_transport = true;
    uart_puts("[VGPU] VirtIO-GPU: trasporto v2 pronto\n");
    return true;
}

uint32_t *virtio_gpu_visible_scanout_ptr(void)
{
    if (!vgpu_transport)
        return NULL;
    vgpu_ensure_device();
    if (!vgpu_device)
        return NULL;
    return vgpu_scanbuf[vgpu_front_idx];
}

static int vgpu_create_resource(uint32_t resource_id, uint32_t *pixels)
{
    vgpu_create_2d_t *c2d = (vgpu_create_2d_t *)(void *)vgpu_cmd;
    vgpu_attach_backing_t *ab;

    vgpu_prepare_hdr(&c2d->hdr, VGPU_CMD_CREATE_2D);
    c2d->resource_id = resource_id;
    c2d->format      = VGPU_FORMAT_B8G8R8X8_UNORM;
    c2d->width       = FB_WIDTH;
    c2d->height      = FB_HEIGHT;
    if (vq_send((uint32_t)sizeof(vgpu_create_2d_t),
                (uint32_t)sizeof(vgpu_hdr_t)) != 0) {
        uart_puts("[VGPU] WARN: create_2d fallito\n");
        return -1;
    }

    ab = (vgpu_attach_backing_t *)(void *)vgpu_cmd;
    vgpu_prepare_hdr(&ab->hdr, VGPU_CMD_ATTACH_BACKING);
    ab->resource_id = resource_id;
    ab->nr_entries  = 1U;
    ab->mem_addr    = (uint64_t)(uintptr_t)pixels;
    ab->mem_len     = FB_WIDTH * FB_HEIGHT * 4U;
    ab->mem_pad     = 0U;
    if (vq_send((uint32_t)sizeof(vgpu_attach_backing_t),
                (uint32_t)sizeof(vgpu_hdr_t)) != 0) {
        uart_puts("[VGPU] WARN: attach_backing fallito\n");
        return -1;
    }

    return 0;
}

static int vgpu_set_scanout_resource(uint32_t resource_id)
{
    vgpu_set_scanout_t *ss = (vgpu_set_scanout_t *)(void *)vgpu_cmd;
    vgpu_prepare_hdr(&ss->hdr, VGPU_CMD_SET_SCANOUT);
    ss->r           = (vgpu_rect_t){ 0U, 0U, FB_WIDTH, FB_HEIGHT };
    ss->scanout_id  = vgpu_scanout_id;
    ss->resource_id = resource_id;
    if (vq_send((uint32_t)sizeof(vgpu_set_scanout_t),
                (uint32_t)sizeof(vgpu_hdr_t)) != 0) {
        uart_puts("[VGPU] WARN: set_scanout fallito\n");
        return -1;
    }
    return 0;
}

static int vgpu_transfer_and_flush(uint32_t resource_id,
                                   uint32_t x, uint32_t y,
                                   uint32_t w, uint32_t h)
{
    vgpu_transfer_t *tr = (vgpu_transfer_t *)(void *)vgpu_cmd;
    vgpu_flush_t    *fl;

    vgpu_prepare_hdr(&tr->hdr, VGPU_CMD_TRANSFER_TO_HOST);
    tr->r           = (vgpu_rect_t){ x, y, w, h };
    tr->offset      = ((uint64_t)y * FB_WIDTH + x) * 4ULL;
    tr->resource_id = resource_id;
    tr->padding     = 0U;
    if (vq_send((uint32_t)sizeof(vgpu_transfer_t),
                (uint32_t)sizeof(vgpu_hdr_t)) != 0) {
        uart_puts("[VGPU] WARN: transfer_to_host fallito\n");
        return -1;
    }

    fl = (vgpu_flush_t *)(void *)vgpu_cmd;
    vgpu_prepare_hdr(&fl->hdr, VGPU_CMD_RESOURCE_FLUSH);
    fl->r           = (vgpu_rect_t){ x, y, w, h };
    fl->resource_id = resource_id;
    fl->padding     = 0U;
    if (vq_send((uint32_t)sizeof(vgpu_flush_t),
                (uint32_t)sizeof(vgpu_hdr_t)) != 0) {
        uart_puts("[VGPU] WARN: resource_flush fallito\n");
        return -1;
    }

    return 0;
}
