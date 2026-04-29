/*
 * EnlilOS - GPU Driver Internal Header (M3-04)
 *
 * Condiviso da gpu_agx.c, gpu_virtio.c, gpu_sw.c e kernel/gpu_syscall.c.
 */

#ifndef GPU_INTERNAL_H
#define GPU_INTERNAL_H

#include "gpu.h"
#include "types.h"

/* ── Dimensioni pool ────────────────────────────────────────────────── */

#define GPU_MAX_BUFS      32
#define GPU_MAX_QUEUES     8
#define GPU_MAX_CMDBUFS   16
#define GPU_MAX_FENCES    64
#define GPU_CMDBUF_CAP   256   /* uint32_t per command buffer = 1 KB */

/* ── Voci dei pool ──────────────────────────────────────────────────── */

typedef struct {
    uint64_t phys;     /* indirizzo fisico == virtuale (identity map) */
    uint64_t iova;     /* IOVA GPU; identity-mapped finche' manca IOMMU */
    uint32_t size;     /* dimensione in byte                          */
    uint32_t capacity; /* capacita' reale dello slot preallocato      */
    uint32_t type;     /* GPU_BUF_*                                   */
    uint16_t pool_kind;
    uint16_t pool_slot;
    uint16_t next_free;
    bool     mapped;   /* CPU mapping attivo                          */
    bool     cpu_dirty;/* il buffer va flushato prima del DMA GPU     */
    bool     pinned;   /* slot non-evictable / riservato              */
    bool     in_use;
} gpu_buf_entry_t;

typedef struct {
    uint32_t type;     /* GPU_QUEUE_RENDER / COMPUTE / BLIT           */
    uint32_t depth;    /* profondità ring buffer (num entry)          */
    bool     in_use;
} gpu_queue_entry_t;

/*
 * gpu_cmdbuf_entry_t — pool entry per command buffer.
 * pub.cmds punta sempre a storage[] del pool entry.
 * Per trovare l'entry da un puntatore pub: cast diretto (pub è il primo campo).
 */
typedef struct {
    gpu_cmdbuf_t pub;                    /* struttura pubblica (primo campo) */
    uint32_t     storage[GPU_CMDBUF_CAP]; /* storage statico 1 KB            */
    uint32_t     queue_handle;
    bool         in_use;
} gpu_cmdbuf_entry_t;

typedef struct {
    uint64_t id;         /* fence ID monotono (0 = invalido)          */
    uint32_t state;      /* GPU_FENCE_PENDING / SIGNALED / ERROR       */
    uint64_t submit_ns;
    uint64_t done_ns;
    bool     in_use;
} gpu_fence_entry_t;

/* ── Pool globali (definiti in gpu_syscall.c) ───────────────────────── */

extern gpu_buf_entry_t    gpu_buf_pool   [GPU_MAX_BUFS];
extern gpu_queue_entry_t  gpu_queue_pool [GPU_MAX_QUEUES];
extern gpu_cmdbuf_entry_t gpu_cmdbuf_pool[GPU_MAX_CMDBUFS];
extern gpu_fence_entry_t  gpu_fence_pool [GPU_MAX_FENCES];
extern uint64_t           gpu_next_fence_id;   /* contatore monotono */

/* ── Backend ops vtable ─────────────────────────────────────────────── */

typedef struct {
    void (*query_caps)        (gpu_caps_t *out);
    void (*query_scanout)     (gpu_scanout_info_t *out);
    /*
     * execute_cmdbuf: esegue/sottomette il command buffer.
     * Imposta la fence come SIGNALED (SW) o sottomette all'HW.
     * Ritorna 0 = OK, <0 = errore.
     */
    int  (*execute_cmdbuf)    (gpu_cmdbuf_entry_t *cb, gpu_fence_entry_t *fence);
    /*
     * present: page-flip del buffer di scanout.
     * Su SW: flush/copia. Su HW: page-flip sul display engine.
     * La fence puo' restare pending fino al prossimo vsync.
     */
    int  (*present)           (gpu_buf_entry_t *scanout, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h, gpu_fence_entry_t *fence);
} gpu_backend_ops_t;

/* Backend registrati */
extern const gpu_backend_ops_t gpu_sw_backend;
extern const gpu_backend_ops_t gpu_agx_backend;
extern const gpu_backend_ops_t gpu_virtio_backend;

/* Backend attivo */
extern const gpu_backend_ops_t *gpu_active_backend;

/* Seleziona il backend appropriato */
void gpu_backend_init(void);

/*
 * virtio_gpu_init() — probe bus virtio-mmio + init trasporto.
 * Definita in gpu_virtio.c. Ritorna true se GPU trovata e pronta.
 * Chiamare prima di controllare Apple Silicon.
 */
bool virtio_gpu_init(void);

/*
 * Buffer scanout effettivamente visibile lato host/QEMU.
 * Utile per selftest visuali e debug di present.
 */
uint32_t *virtio_gpu_visible_scanout_ptr(void);

#endif /* GPU_INTERNAL_H */
