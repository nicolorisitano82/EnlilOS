/*
 * EnlilOS - GPU Software Fallback Backend (M3-04)
 *
 * Emula la GPU sulla CPU. Attivo su QEMU (MIDR implementer != 0x61).
 *
 * Comportamento:
 *   execute_cmdbuf: marca la fence SIGNALED immediatamente — i comandi
 *                   GPU sono opachi al kernel, non vengono interpretati.
 *   present:        copia il buffer di scanout nel framebuffer condiviso
 *                   e chiama fb_flush() per renderlo visibile su QEMU.
 */

#include "gpu_internal.h"
#include "framebuffer.h"
#include "timer.h"
#include "uart.h"

static gpu_scanout_info_t sw_scanout = {
    .width        = FB_WIDTH,
    .height       = FB_HEIGHT,
    .refresh_hz   = 60U,
    .flags        = GPU_SCANOUT_ACTIVE,
    .front_index  = 0U,
    .back_index   = 0U,
    .frame_counter = 0U,
};

/* ── query_caps ─────────────────────────────────────────────────────── */

static void sw_query_caps(gpu_caps_t *out)
{
    out->vendor          = GPU_VENDOR_SW;
    out->device_id       = 0x0000;
    out->vram_bytes      = 0;
    out->max_texture_dim = 4096;
    out->compute_units   = 0;
    out->flags           = GPU_CAP_SWFALLBACK | GPU_CAP_SCANOUT | GPU_CAP_COMPUTE;
}

static void sw_query_scanout(gpu_scanout_info_t *out)
{
    if (!out) return;
    *out = sw_scanout;
}

/* ── execute_cmdbuf ─────────────────────────────────────────────────── */

/*
 * Il SW backend non interpreta i comandi GPU: li accetta e segnala
 * subito la fence. Questo rende l'API corretta per lo sviluppo
 * anche senza hardware reale.
 */
static int sw_execute_cmdbuf(gpu_cmdbuf_entry_t *cb, gpu_fence_entry_t *fence)
{
    (void)cb;
    fence->done_ns = timer_now_ns();
    fence->state   = GPU_FENCE_SIGNALED;
    return 0;
}

/* ── present ────────────────────────────────────────────────────────── */

/*
 * Copia il scanout buffer nel framebuffer del kernel e chiama fb_flush().
 *
 * Supporta due casi:
 *   1. Il buffer IS il framebuffer (stessa PA): solo flush.
 *   2. Buffer distinto: blit nella regione (x,y,w,h) del framebuffer.
 *
 * La copia è pixel per pixel (uint32_t XRGB8888).
 * Su hardware reale questo avviene via DMA — qui è per sviluppo.
 */
static int sw_present(gpu_buf_entry_t *scanout, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, gpu_fence_entry_t *fence)
{
    /* Ottieni il puntatore al framebuffer del kernel */
    extern uint32_t *fb_get_ptr(void);
    uint32_t *fb = fb_get_ptr();

    if (fb == NULL) {
        fence->state = GPU_FENCE_ERROR;
        return -1;
    }

    uint32_t *src = (uint32_t *)(uintptr_t)scanout->phys;

    if ((uintptr_t)src == (uintptr_t)fb) {
        /* Caso 1: buffer è già il framebuffer */
        fb_flush();
    } else {
        /* Caso 2: blit regione (x,y,w,h) */
        uint32_t fb_w = FB_WIDTH;
        uint32_t blit_w = (x + w > fb_w) ? (fb_w - x) : w;
        uint32_t blit_h = (y + h > FB_HEIGHT) ? (FB_HEIGHT - y) : h;

        for (uint32_t row = 0; row < blit_h; row++) {
            uint32_t *dst_row = fb  + (y + row) * fb_w + x;
            uint32_t *src_row = src + row * w;
            for (uint32_t col = 0; col < blit_w; col++)
                dst_row[col] = src_row[col];
        }
        fb_flush();
    }

    sw_scanout.frame_counter++;
    fence->done_ns = timer_now_ns();
    fence->state   = GPU_FENCE_SIGNALED;
    return 0;
}

/* ── Esportazione backend ───────────────────────────────────────────── */

const gpu_backend_ops_t gpu_sw_backend = {
    .query_caps     = sw_query_caps,
    .query_scanout  = sw_query_scanout,
    .execute_cmdbuf = sw_execute_cmdbuf,
    .present        = sw_present,
};
