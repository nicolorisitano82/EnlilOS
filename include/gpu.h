/*
 * EnlilOS Microkernel - GPU Syscall Interface (M3-04)
 *
 * Accesso diretto ad Apple AGX (M-series) o virtio-GPU (QEMU).
 * Syscall range 120–139.
 *
 * RT design:
 *   - Submit command buffer non-blocking: O(1)
 *   - Fence wait con deadline: bounded, mai unbounded in task RT
 *   - Buffer GPU pre-allocati al boot: zero alloc nel hot path
 *   - gpu_present(): page-flip atomico, bypassa il compositor
 *
 * Rilevamento backend via MIDR_EL1:
 *   Apple (0x61) → AGX stub → SW per ora (driver MMIO in M5b-01)
 *   ARM/QEMU (0x41) → SW fallback (fence segnalate immediatamente)
 */

#ifndef ENLILOS_GPU_H
#define ENLILOS_GPU_H

#include "types.h"

/* ── Numeri syscall ─────────────────────────────────────────────────── */

#define SYS_GPU_QUERY_CAPS       120
#define SYS_GPU_BUF_ALLOC        121
#define SYS_GPU_BUF_FREE         122
#define SYS_GPU_BUF_MAP_CPU      123
#define SYS_GPU_BUF_UNMAP_CPU    124
#define SYS_GPU_CMDQUEUE_CREATE  125
#define SYS_GPU_CMDQUEUE_DESTROY 126
#define SYS_GPU_CMDBUF_BEGIN     127
#define SYS_GPU_CMDBUF_SUBMIT    128
#define SYS_GPU_FENCE_WAIT       129
#define SYS_GPU_FENCE_QUERY      130
#define SYS_GPU_FENCE_DESTROY    131
#define SYS_GPU_PRESENT          132
#define SYS_GPU_COMPUTE_DISPATCH 133

/* ── Capacità GPU ───────────────────────────────────────────────────── */

#define GPU_VENDOR_APPLE_AGX  0x106B
#define GPU_VENDOR_VIRTIO     0x1AF4
#define GPU_VENDOR_SW         0x0000

#define GPU_CAP_COMPUTE      (1u << 0)   /* compute shader supportato     */
#define GPU_CAP_RT_PIPELINE  (1u << 1)   /* ray tracing (M3+)             */
#define GPU_CAP_SWFALLBACK   (1u << 2)   /* running in CPU emulation      */
#define GPU_CAP_SCANOUT      (1u << 3)   /* scanout diretto al display    */
#define GPU_CAP_HW           (1u << 4)   /* hardware GPU presente         */

typedef struct {
    uint32_t vendor;           /* GPU_VENDOR_*                          */
    uint32_t device_id;        /* 0x6000=M1, 0x6020=M2, 0x6030=M3, ... */
    uint64_t vram_bytes;       /* VRAM (shared con sistema su M-series) */
    uint32_t max_texture_dim;  /* max texture 1D/2D (es. 16384)         */
    uint32_t compute_units;    /* shader core / compute unit            */
    uint32_t flags;            /* GPU_CAP_*                             */
} gpu_caps_t;

/* ── Handle ─────────────────────────────────────────────────────────── */

typedef uint32_t gpu_buf_handle_t;
typedef uint32_t gpu_queue_handle_t;
typedef uint64_t gpu_fence_t;      /* 0 = invalido */

#define GPU_INVALID_BUF    ((gpu_buf_handle_t)~0u)
#define GPU_INVALID_QUEUE  ((gpu_queue_handle_t)~0u)
#define GPU_INVALID_FENCE  ((gpu_fence_t)0u)

/* ── Tipi di GPU buffer ─────────────────────────────────────────────── */

#define GPU_BUF_VERTEX   (1u << 0)
#define GPU_BUF_TEXTURE  (1u << 1)
#define GPU_BUF_UNIFORM  (1u << 2)
#define GPU_BUF_STORAGE  (1u << 3)
#define GPU_BUF_SCANOUT  (1u << 4)   /* display scanout buffer */
#define GPU_BUF_SHADER   (1u << 5)   /* compiled shader binary */
#define GPU_BUF_PINNED   (1u << 6)   /* non evictabile (obbligatorio RT) */

/* ── Tipi di command queue ──────────────────────────────────────────── */

#define GPU_QUEUE_RENDER   0u   /* vertex + fragment */
#define GPU_QUEUE_COMPUTE  1u   /* compute shader    */
#define GPU_QUEUE_BLIT     2u   /* copia / fill      */

/* ── Stato fence ────────────────────────────────────────────────────── */

#define GPU_FENCE_PENDING  0u
#define GPU_FENCE_SIGNALED 1u
#define GPU_FENCE_ERROR    2u

/* ── Stato scanout / display engine ──────────────────────────────── */

#define GPU_SCANOUT_ACTIVE         (1u << 0)
#define GPU_SCANOUT_DOUBLE_BUFFER  (1u << 1)
#define GPU_SCANOUT_VSYNC          (1u << 2)

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t refresh_hz;
    uint32_t flags;
    uint32_t front_index;
    uint32_t back_index;
    uint64_t frame_counter;
} gpu_scanout_info_t;

/* ── Command buffer ─────────────────────────────────────────────────── */

/*
 * gpu_cmdbuf_t — struttura pubblica restituita da gpu_cmdbuf_begin().
 * Il chiamante scrive i comandi GPU encoded in cmds[count++].
 * Non allocare direttamente: usare solo tramite gpu_cmdbuf_begin().
 */
typedef struct {
    uint32_t *cmds;      /* puntatore allo storage statico nel pool   */
    uint32_t  capacity;  /* numero massimo comandi                    */
    uint32_t  count;     /* comandi scritti finora                    */
    uint32_t  queue_idx; /* indice nella gpu_queue_pool (interno)     */
} gpu_cmdbuf_t;

/* ── API pubblica ────────────────────────────────────────────────────── */

/*
 * gpu_init() — rileva backend, registra syscall 120-133.
 * Chiamare da main() dopo ane_init().
 */
void gpu_init(void);

/*
 * gpu_present_fullscreen() — esegue un present dell'intero framebuffer
 * tramite il backend attivo. Usa fb_get_ptr() come sorgente.
 * Chiama fb_flush() internamente — sicuro da chiamare in coppia con
 * il vecchio fb_flush() (il secondo è un no-op su virtio).
 * Usata da main() dopo l'inizializzazione grafica per sincronizzare
 * il display virtio con il contenuto disegnato nel framebuffer SW.
 */
void gpu_present_fullscreen(void);

/*
 * gpu_present_framebuffer() — presenta esplicitamente il framebuffer legacy
 * tramite il backend attivo, bypassando l'eventuale target 2D separato.
 * Utile per path che disegnano ancora con fb_put_pixel()/fb_clear().
 */
void gpu_present_framebuffer(void);
void gpu_set_2d_present_enabled(bool enabled);

/*
 * gpu_get_caps() — ritorna le capability del backend GPU attivo.
 * Utile per scegliere una UI di boot coerente tra SW fallback e VirtIO-GPU.
 */
void gpu_get_caps(gpu_caps_t *out);

/*
 * gpu_get_scanout_info() — stato del display engine attivo:
 * scanout size, vsync, double buffering, front/back index e frame count.
 */
void gpu_get_scanout_info(gpu_scanout_info_t *out);

/*
 * gpu_get_present_target_ptr() — ritorna il target CPU-writable usato dal
 * path di present attuale.
 *
 * In modalita' grafica preferisce il buffer scanout del renderer 2D; altrimenti
 * degrada al framebuffer locale del kernel.
 */
uint32_t *gpu_get_present_target_ptr(void);

/*
 * gpu_mark_present_target_dirty() — marca il target CPU-writable come sporco
 * in modo che il backend di present lo flushi correttamente al prossimo
 * gpu_present_fullscreen().
 */
void gpu_mark_present_target_dirty(void);

/*
 * gpu_get_visible_scanout_ptr() — ritorna il buffer che il backend display
 * considera attualmente visibile.
 *
 * Con VirtIO-GPU corrisponde al frontbuffer attivo sullo scanout; sui backend
 * piu' semplici degrada al framebuffer locale.
 */
uint32_t *gpu_get_visible_scanout_ptr(void);

/*
 * gpu_flush_cache() — pulisce la D-cache per un GBO e lo rende visibile
 * al DMA GPU. API kernel-side; le syscall continuano a usare map/unmap.
 */
int gpu_flush_cache(gpu_buf_handle_t handle);

/*
 * Renderer 2D batched (M5b-04).
 *
 * Le primitive vengono accodate in un command ring statico e applicate in
 * batch al prossimo gpu_present_fullscreen(). In modalita' grafica il target
 * e' un buffer di scanout GPU dedicato; altrove il fallback resta il FB CPU.
 */
int gpu_begin_2d_frame(uint32_t clear_color);
int gpu_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
int gpu_blit(uint32_t dst_x, uint32_t dst_y,
             uint32_t src_x, uint32_t src_y,
             uint32_t w, uint32_t h);
int gpu_draw_glyph(uint32_t x, uint32_t y, uint32_t codepoint,
                   uint32_t fg, uint32_t bg, bool transparent_bg);
int gpu_draw_string(uint32_t x, uint32_t y, const char *utf8_str,
                    uint32_t fg, uint32_t bg, bool transparent_bg);
int gpu_alpha_blend(uint32_t x, uint32_t y, const uint32_t *src,
                    uint32_t w, uint32_t h, uint8_t global_alpha);

#endif /* ENLILOS_GPU_H */
