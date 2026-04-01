/*
 * EnlilOS Microkernel - GPU Syscall Layer (M3-04)
 *
 * Implementa le syscall 120-133 per Apple AGX / virtio-GPU / SW.
 *
 * Pool statici:
 *   gpu_buf_pool   [32] — GPU buffer objects (GBO)
 *   gpu_queue_pool  [8] — command queue
 *   gpu_cmdbuf_pool[16] — command buffer con storage inline (1 KB ciascuno)
 *   gpu_fence_pool [64] — fence con ID monotono
 *
 * Handle buf/queue: 1-based uint32_t (0 / ~0 = invalido).
 * Fence: uint64_t monotono ≥ 1 (0 = invalido).
 *
 * RT path: cmdbuf_begin (127), cmdbuf_submit (128), fence_wait (129),
 *          fence_query (130), present (132), compute_dispatch (133)
 *          sono tutti O(1) o O(bounded).
 */

#include "gpu_internal.h"
#include "framebuffer.h"
#include "syscall.h"
#include "sched.h"
#include "timer.h"
#include "uart.h"

/* ════════════════════════════════════════════════════════════════════
 * Pool globali
 * ════════════════════════════════════════════════════════════════════ */

gpu_buf_entry_t    gpu_buf_pool   [GPU_MAX_BUFS];
gpu_queue_entry_t  gpu_queue_pool [GPU_MAX_QUEUES];
gpu_cmdbuf_entry_t gpu_cmdbuf_pool[GPU_MAX_CMDBUFS];
gpu_fence_entry_t  gpu_fence_pool [GPU_MAX_FENCES];
uint64_t           gpu_next_fence_id = 1;

#define GPU_MM_INVALID_SLOT     0xFFFFU
#define GPU_MM_POOL_SCANOUT     1U
#define GPU_MM_POOL_GENERAL     2U
#define GPU_MM_POOL_CMD         3U

#define GPU_MM_SCANOUT_SLOTS    4U
#define GPU_MM_GENERAL_SLOTS    8U
#define GPU_MM_CMD_SLOTS        8U

#define GPU_MM_SCANOUT_BYTES    (FB_WIDTH * FB_HEIGHT * 4U)
#define GPU_MM_GENERAL_BYTES    (512U * 1024U)
#define GPU_MM_CMD_BYTES        (64U * 1024U)

static uint8_t gpu_mm_scanout[GPU_MM_SCANOUT_SLOTS][GPU_MM_SCANOUT_BYTES]
    __attribute__((aligned(4096)));
static uint8_t gpu_mm_general[GPU_MM_GENERAL_SLOTS][GPU_MM_GENERAL_BYTES]
    __attribute__((aligned(4096)));
static uint8_t gpu_mm_cmd[GPU_MM_CMD_SLOTS][GPU_MM_CMD_BYTES]
    __attribute__((aligned(4096)));

static uint16_t gpu_mm_scanout_next[GPU_MM_SCANOUT_SLOTS];
static uint16_t gpu_mm_general_next[GPU_MM_GENERAL_SLOTS];
static uint16_t gpu_mm_cmd_next[GPU_MM_CMD_SLOTS];

static uint16_t gpu_mm_handle_head = GPU_MM_INVALID_SLOT;
static uint16_t gpu_mm_scanout_head = GPU_MM_INVALID_SLOT;
static uint16_t gpu_mm_general_head = GPU_MM_INVALID_SLOT;
static uint16_t gpu_mm_cmd_head = GPU_MM_INVALID_SLOT;

static void gpu_mm_init(void);
static int gpu_buf_flush_entry(gpu_buf_entry_t *e);
static void gpu_flush_dirty_buffers(void);

static void gpu_fence_update(gpu_fence_entry_t *f)
{
    if (!f || !f->in_use) return;
    if (f->state != GPU_FENCE_PENDING) return;
    if (f->done_ns == 0U) return;

    if (timer_now_ns() >= f->done_ns)
        f->state = GPU_FENCE_SIGNALED;
}

/* ── Allocatori di slot (1-based per buf/queue; cerca ID per fence) ── */

static uint32_t buf_slot_alloc(void)
{
    uint16_t slot = gpu_mm_handle_head;
    if (slot == GPU_MM_INVALID_SLOT)
        return 0;
    gpu_mm_handle_head = gpu_buf_pool[slot].next_free;
    gpu_buf_pool[slot].next_free = GPU_MM_INVALID_SLOT;
    return (uint32_t)(slot + 1U);
}
static void buf_slot_free(uint32_t h)
{
    uint16_t slot = (uint16_t)(h - 1U);
    gpu_buf_pool[slot].next_free = gpu_mm_handle_head;
    gpu_mm_handle_head = slot;
}
static uint32_t queue_slot_alloc(void)
{
    for (int i = 0; i < GPU_MAX_QUEUES; i++)
        if (!gpu_queue_pool[i].in_use) return (uint32_t)(i + 1);
    return 0;
}
static uint32_t cmdbuf_slot_alloc(void)
{
    for (int i = 0; i < GPU_MAX_CMDBUFS; i++)
        if (!gpu_cmdbuf_pool[i].in_use) return (uint32_t)(i + 1);
    return 0;
}
static gpu_fence_entry_t *fence_alloc(uint64_t *out_id)
{
    for (int i = 0; i < GPU_MAX_FENCES; i++) {
        if (!gpu_fence_pool[i].in_use) {
            gpu_fence_pool[i].id     = gpu_next_fence_id++;
            gpu_fence_pool[i].state  = GPU_FENCE_PENDING;
            gpu_fence_pool[i].in_use = true;
            *out_id = gpu_fence_pool[i].id;
            return &gpu_fence_pool[i];
        }
    }
    return NULL;
}
static gpu_fence_entry_t *fence_find(uint64_t id)
{
    if (id == 0) return NULL;
    for (int i = 0; i < GPU_MAX_FENCES; i++)
        if (gpu_fence_pool[i].in_use && gpu_fence_pool[i].id == id)
            return &gpu_fence_pool[i];
    return NULL;
}

/* Validazione handle */
static bool buf_valid(uint32_t h)
{
    return h >= 1 && h <= GPU_MAX_BUFS && gpu_buf_pool[h-1].in_use;
}
static bool queue_valid(uint32_t h)
{
    return h >= 1 && h <= GPU_MAX_QUEUES && gpu_queue_pool[h-1].in_use;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 120 — gpu_query_caps
 * args[0] = gpu_caps_t *out
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_query_caps(uint64_t args[6])
{
    gpu_caps_t *out = (gpu_caps_t *)(uintptr_t)args[0];
    if (!out) return ERR(EFAULT);
    gpu_active_backend->query_caps(out);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 121 — gpu_buf_alloc
 * args[0] = size, args[1] = type (GPU_BUF_*)
 * Ritorna: handle oppure GPU_INVALID_BUF
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_buf_alloc(uint64_t args[6])
{
    uint32_t size = (uint32_t)args[0];
    uint32_t type = (uint32_t)args[1];
    uint16_t *pool_head;
    uint16_t *pool_next;
    uint16_t pool_slot;
    uint32_t pool_kind;
    uint32_t capacity;
    uintptr_t base;
    uint32_t h;

    if (size == 0U) return GPU_INVALID_BUF;

    if (type & GPU_BUF_SCANOUT) {
        pool_kind = GPU_MM_POOL_SCANOUT;
        capacity  = GPU_MM_SCANOUT_BYTES;
        pool_head = &gpu_mm_scanout_head;
        pool_next = gpu_mm_scanout_next;
    } else if ((type & (GPU_BUF_UNIFORM | GPU_BUF_SHADER)) != 0U &&
               size <= GPU_MM_CMD_BYTES) {
        pool_kind = GPU_MM_POOL_CMD;
        capacity  = GPU_MM_CMD_BYTES;
        pool_head = &gpu_mm_cmd_head;
        pool_next = gpu_mm_cmd_next;
    } else if (size <= GPU_MM_GENERAL_BYTES) {
        pool_kind = GPU_MM_POOL_GENERAL;
        capacity  = GPU_MM_GENERAL_BYTES;
        pool_head = &gpu_mm_general_head;
        pool_next = gpu_mm_general_next;
    } else if (size <= GPU_MM_CMD_BYTES) {
        pool_kind = GPU_MM_POOL_CMD;
        capacity  = GPU_MM_CMD_BYTES;
        pool_head = &gpu_mm_cmd_head;
        pool_next = gpu_mm_cmd_next;
    } else {
        return GPU_INVALID_BUF;
    }

    h = buf_slot_alloc();
    if (h == 0) return GPU_INVALID_BUF;
    pool_slot = *pool_head;
    if (pool_slot == GPU_MM_INVALID_SLOT) {
        buf_slot_free(h);
        return GPU_INVALID_BUF;
    }
    *pool_head = pool_next[pool_slot];

    if (pool_kind == GPU_MM_POOL_SCANOUT)
        base = (uintptr_t)&gpu_mm_scanout[pool_slot][0];
    else if (pool_kind == GPU_MM_POOL_CMD)
        base = (uintptr_t)&gpu_mm_cmd[pool_slot][0];
    else
        base = (uintptr_t)&gpu_mm_general[pool_slot][0];

    for (uint32_t i = 0U; i < capacity; i++)
        ((volatile uint8_t *)base)[i] = 0U;

    gpu_buf_pool[h-1].phys      = (uint64_t)base;
    gpu_buf_pool[h-1].iova      = (uint64_t)base;
    gpu_buf_pool[h-1].size      = size;
    gpu_buf_pool[h-1].capacity  = capacity;
    gpu_buf_pool[h-1].type      = type;
    gpu_buf_pool[h-1].pool_kind = (uint16_t)pool_kind;
    gpu_buf_pool[h-1].pool_slot = pool_slot;
    gpu_buf_pool[h-1].mapped    = false;
    gpu_buf_pool[h-1].cpu_dirty = true;
    gpu_buf_pool[h-1].pinned    = ((type & GPU_BUF_PINNED) != 0U);
    gpu_buf_pool[h-1].in_use    = true;
    return (uint64_t)h;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 122 — gpu_buf_free
 * args[0] = handle
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_buf_free(uint64_t args[6])
{
    uint32_t h = (uint32_t)args[0];
    gpu_buf_entry_t *e;
    uint16_t *pool_head;
    uint16_t *pool_next;
    if (!buf_valid(h)) return ERR(EINVAL);

    e = &gpu_buf_pool[h-1];
    if (e->pinned) return ERR(EBUSY);

    if (e->pool_kind == GPU_MM_POOL_SCANOUT) {
        pool_head = &gpu_mm_scanout_head;
        pool_next = gpu_mm_scanout_next;
    } else if (e->pool_kind == GPU_MM_POOL_CMD) {
        pool_head = &gpu_mm_cmd_head;
        pool_next = gpu_mm_cmd_next;
    } else if (e->pool_kind == GPU_MM_POOL_GENERAL) {
        pool_head = &gpu_mm_general_head;
        pool_next = gpu_mm_general_next;
    } else {
        return ERR(EINVAL);
    }

    pool_next[e->pool_slot] = *pool_head;
    *pool_head = e->pool_slot;
    e->phys = 0U;
    e->iova = 0U;
    e->size = 0U;
    e->capacity = 0U;
    e->type = 0U;
    e->pool_kind = 0U;
    e->pool_slot = GPU_MM_INVALID_SLOT;
    e->mapped = false;
    e->cpu_dirty = false;
    e->pinned = false;
    e->in_use = false;
    buf_slot_free(h);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 123 — gpu_buf_map_cpu
 * args[0] = handle
 * Ritorna: indirizzo virtuale (== fisico, identity map) oppure 0
 *
 * Con MMU identity-mapped PA==VA — la "mappatura" è un no-op.
 * Setta il flag mapped per validazione di unmap.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_buf_map_cpu(uint64_t args[6])
{
    uint32_t h = (uint32_t)args[0];
    if (!buf_valid(h)) return 0;

    gpu_buf_entry_t *e = &gpu_buf_pool[h-1];
    e->mapped = true;
    e->cpu_dirty = true;
    return e->phys;   /* PA == VA */
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 124 — gpu_buf_unmap_cpu
 * args[0] = handle
 *
 * Invalida la D-cache per il buffer (DC CIVAC) così la GPU DMA
 * vede i dati scritti dalla CPU. Poi rilascia il mapping.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_buf_unmap_cpu(uint64_t args[6])
{
    uint32_t h = (uint32_t)args[0];
    if (!buf_valid(h)) return ERR(EINVAL);

    gpu_buf_entry_t *e = &gpu_buf_pool[h-1];
    if (!e->mapped) return ERR(EINVAL);
    (void)gpu_buf_flush_entry(e);
    e->mapped = false;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 125 — gpu_cmdqueue_create
 * args[0] = type (GPU_QUEUE_*), args[1] = depth
 * Ritorna: queue_handle oppure GPU_INVALID_QUEUE
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_cmdqueue_create(uint64_t args[6])
{
    uint32_t type  = (uint32_t)args[0];
    uint32_t depth = (uint32_t)args[1];

    if (type > GPU_QUEUE_BLIT) return GPU_INVALID_QUEUE;
    if (depth == 0) depth = 64;
    if (depth > 1024) depth = 1024;

    uint32_t h = queue_slot_alloc();
    if (h == 0) return GPU_INVALID_QUEUE;

    gpu_queue_pool[h-1].type   = type;
    gpu_queue_pool[h-1].depth  = depth;
    gpu_queue_pool[h-1].in_use = true;
    return (uint64_t)h;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 126 — gpu_cmdqueue_destroy
 * args[0] = queue_handle
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_cmdqueue_destroy(uint64_t args[6])
{
    uint32_t h = (uint32_t)args[0];
    if (!queue_valid(h)) return ERR(EINVAL);
    gpu_queue_pool[h-1].in_use = false;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 127 — gpu_cmdbuf_begin
 * args[0] = queue_handle
 * Ritorna: puntatore a gpu_cmdbuf_t (nel pool statico) oppure NULL
 *
 * RT-safe: O(MAX_CMDBUFS) per trovare uno slot libero.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_cmdbuf_begin(uint64_t args[6])
{
    uint32_t qh = (uint32_t)args[0];
    if (!queue_valid(qh)) return 0;

    uint32_t slot = cmdbuf_slot_alloc();
    if (slot == 0) return 0;

    gpu_cmdbuf_entry_t *entry = &gpu_cmdbuf_pool[slot - 1];
    entry->pub.cmds      = entry->storage;
    entry->pub.capacity  = GPU_CMDBUF_CAP;
    entry->pub.count     = 0;
    entry->pub.queue_idx = qh - 1;
    entry->queue_handle  = qh;
    entry->in_use        = true;

    /* Azzera lo storage comandi */
    for (int i = 0; i < GPU_CMDBUF_CAP; i++) entry->storage[i] = 0;

    return (uint64_t)(uintptr_t)&entry->pub;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 128 — gpu_cmdbuf_submit
 * args[0] = puntatore a gpu_cmdbuf_t, args[1] = priority
 * Ritorna: gpu_fence_t (ID) oppure GPU_INVALID_FENCE
 *
 * RT-safe: O(1). Non-blocking.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_cmdbuf_submit(uint64_t args[6])
{
    gpu_cmdbuf_t *pub  = (gpu_cmdbuf_t *)(uintptr_t)args[0];
    /* uint32_t prio = (uint32_t)args[1]; usato dal HW scheduler GPU */

    if (!pub) return GPU_INVALID_FENCE;

    /* Risale all'entry del pool: pub è il primo campo di gpu_cmdbuf_entry_t */
    gpu_cmdbuf_entry_t *entry = (gpu_cmdbuf_entry_t *)pub;

    /* Valida che l'entry sia nel pool (bounds check) */
    if (entry < gpu_cmdbuf_pool ||
        entry >= gpu_cmdbuf_pool + GPU_MAX_CMDBUFS ||
        !entry->in_use)
        return GPU_INVALID_FENCE;

    /* Alloca fence */
    uint64_t fence_id;
    gpu_fence_entry_t *fence = fence_alloc(&fence_id);
    if (!fence) { entry->in_use = false; return GPU_INVALID_FENCE; }

    fence->submit_ns = timer_now_ns();
    gpu_flush_dirty_buffers();

    /* Esegue / sottomette tramite il backend */
    int ret = gpu_active_backend->execute_cmdbuf(entry, fence);
    if (ret != 0) {
        fence->state = GPU_FENCE_ERROR;
    }

    /* Libera il command buffer (non riusabile dopo submit) */
    entry->in_use = false;

    return fence_id;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 129 — gpu_fence_wait
 * args[0] = fence_id, args[1] = timeout_ns (0 = polling)
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_fence_wait(uint64_t args[6])
{
    uint64_t fence_id   = args[0];
    uint64_t timeout_ns = args[1];

    gpu_fence_entry_t *f = fence_find(fence_id);
    if (!f) return ERR(EINVAL);
    gpu_fence_update(f);

    /* Polling immediato */
    if (timeout_ns == 0)
        return (f->state != GPU_FENCE_PENDING) ? 0 : ERR(EAGAIN);

    /* Attesa con deadline; UINT64_MAX = infinito */
    bool     infinite    = (timeout_ns == (uint64_t)-1ULL);
    uint64_t deadline_ns = infinite ? 0 : timer_now_ns() + timeout_ns;

    while (f->state == GPU_FENCE_PENDING) {
        gpu_fence_update(f);
        if (!infinite && timer_now_ns() >= deadline_ns) return ERR(EAGAIN);
        sched_yield();
    }

    return (f->state == GPU_FENCE_ERROR) ? ERR(EIO) : 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 130 — gpu_fence_query
 * args[0] = fence_id
 * Ritorna: GPU_FENCE_PENDING / SIGNALED / ERROR, oppure -EINVAL
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_fence_query(uint64_t args[6])
{
    gpu_fence_entry_t *f = fence_find(args[0]);
    if (!f) return ERR(EINVAL);
    gpu_fence_update(f);
    return (uint64_t)f->state;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 131 — gpu_fence_destroy
 * args[0] = fence_id
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_fence_destroy(uint64_t args[6])
{
    gpu_fence_entry_t *f = fence_find(args[0]);
    if (!f) return ERR(EINVAL);
    f->in_use = false;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 132 — gpu_present
 * args[0] = scanout_buf_handle
 * args[1] = x, args[2] = y, args[3] = w, args[4] = h
 * Ritorna: gpu_fence_t segnalata dopo il page-flip
 *
 * RT-safe: O(1) su HW (DCP page-flip atomico).
 * Su SW: blit e fb_flush() — O(w×h).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_present(uint64_t args[6])
{
    uint32_t sbh = (uint32_t)args[0];
    uint32_t x   = (uint32_t)args[1];
    uint32_t y   = (uint32_t)args[2];
    uint32_t w   = (uint32_t)args[3];
    uint32_t h   = (uint32_t)args[4];

    if (!buf_valid(sbh)) return GPU_INVALID_FENCE;

    gpu_buf_entry_t *scanout = &gpu_buf_pool[sbh - 1];
    if (!(scanout->type & GPU_BUF_SCANOUT)) return GPU_INVALID_FENCE;

    uint64_t fence_id;
    gpu_fence_entry_t *fence = fence_alloc(&fence_id);
    if (!fence) return GPU_INVALID_FENCE;

    fence->submit_ns = timer_now_ns();
    gpu_flush_dirty_buffers();

    int ret = gpu_active_backend->present(scanout, x, y, w, h, fence);
    if (ret != 0) fence->state = GPU_FENCE_ERROR;

    return fence_id;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 133 — gpu_compute_dispatch
 * args[0] = queue_handle
 * args[1] = shader_buf_handle
 * args[2] = grid_x, args[3] = grid_y, args[4] = grid_z
 * args[5] = args_buf_handle (uniform/storage per lo shader)
 * Ritorna: gpu_fence_t
 *
 * RT-safe: non-blocking — encode il dispatch come command buffer interno
 * e lo submette tramite il backend.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_compute_dispatch(uint64_t args[6])
{
    uint32_t qh     = (uint32_t)args[0];
    uint32_t sh_buf = (uint32_t)args[1];
    uint32_t gx     = (uint32_t)args[2];
    uint32_t gy     = (uint32_t)args[3];
    uint32_t gz     = (uint32_t)args[4];
    uint32_t ab     = (uint32_t)args[5];

    if (!queue_valid(qh))   return GPU_INVALID_FENCE;
    if (!buf_valid(sh_buf)) return GPU_INVALID_FENCE;
    if (!buf_valid(ab))     return GPU_INVALID_FENCE;

    /* Crea un command buffer interno per il dispatch */
    uint32_t slot = cmdbuf_slot_alloc();
    if (slot == 0) return GPU_INVALID_FENCE;

    gpu_cmdbuf_entry_t *entry = &gpu_cmdbuf_pool[slot - 1];
    entry->pub.cmds      = entry->storage;
    entry->pub.capacity  = GPU_CMDBUF_CAP;
    entry->pub.count     = 0;
    entry->pub.queue_idx = qh - 1;
    entry->queue_handle  = qh;
    entry->in_use        = true;

    /*
     * Codifica il dispatch compute in formato opaco:
     *   word 0: opcode GPU_CMD_COMPUTE_DISPATCH = 0x01000000
     *   word 1: shader_buf PA (bassa 32 bit)
     *   word 2: shader_buf PA (alta 32 bit)
     *   word 3: args_buf PA (bassa 32 bit)
     *   word 4: args_buf PA (alta 32 bit)
     *   word 5: grid X
     *   word 6: grid Y
     *   word 7: grid Z
     */
    uint64_t sh_pa = gpu_buf_pool[sh_buf - 1].phys;
    uint64_t ab_pa = gpu_buf_pool[ab - 1].phys;
    entry->storage[0] = 0x01000000u;           /* opcode */
    entry->storage[1] = (uint32_t)(sh_pa);
    entry->storage[2] = (uint32_t)(sh_pa >> 32);
    entry->storage[3] = (uint32_t)(ab_pa);
    entry->storage[4] = (uint32_t)(ab_pa >> 32);
    entry->storage[5] = gx;
    entry->storage[6] = gy;
    entry->storage[7] = gz;
    entry->pub.count  = 8;

    /* Submit tramite il backend */
    uint64_t fence_id;
    gpu_fence_entry_t *fence = fence_alloc(&fence_id);
    if (!fence) { entry->in_use = false; return GPU_INVALID_FENCE; }

    fence->submit_ns = timer_now_ns();
    gpu_flush_dirty_buffers();

    int ret = gpu_active_backend->execute_cmdbuf(entry, fence);
    if (ret != 0) fence->state = GPU_FENCE_ERROR;

    entry->in_use = false;
    return fence_id;
}

/* ════════════════════════════════════════════════════════════════════
 * gpu_init — rileva backend e registra syscall 120-133
 * ════════════════════════════════════════════════════════════════════ */
void gpu_init(void)
{
    gpu_mm_init();

    /* 1. Seleziona backend */
    gpu_backend_init();

    /* 2. Stampa capacità */
    gpu_caps_t caps;
    gpu_active_backend->query_caps(&caps);

    uart_puts("[GPU] Capacità: ");
    static const char hx[] = "0123456789ABCDEF";
    if (caps.vendor == GPU_VENDOR_VIRTIO) {
        uart_puts("VirtIO-GPU");
    } else if (caps.flags & GPU_CAP_HW) {
        uart_puts("AGX device_id=0x");
        for (int s = 12; s >= 0; s -= 4)
            uart_putc(hx[(caps.device_id >> s) & 0xF]);
        uart_puts(" cu=");
        uart_putc('0' + (char)(caps.compute_units / 10));
        uart_putc('0' + (char)(caps.compute_units % 10));
    } else {
        uart_puts("SW fallback");
    }
    uart_puts(" | buf=");
    uart_putc('0' + GPU_MAX_BUFS / 10);
    uart_putc('0' + GPU_MAX_BUFS % 10);
    uart_puts(" queue=");
    uart_putc('0' + GPU_MAX_QUEUES);
    uart_puts(" fence=");
    uart_putc('0' + GPU_MAX_FENCES / 10);
    uart_putc('0' + GPU_MAX_FENCES % 10);
    uart_puts(" | mm scanout=");
    uart_putc('0' + GPU_MM_SCANOUT_SLOTS);
    uart_puts(" general=");
    uart_putc('0' + GPU_MM_GENERAL_SLOTS);
    uart_puts(" cmd=");
    uart_putc('0' + GPU_MM_CMD_SLOTS);
    uart_puts("\n");

    /* 3. Registra syscall 120-133 */
    extern syscall_entry_t syscall_table[256];

    syscall_table[SYS_GPU_QUERY_CAPS] = (syscall_entry_t){
        sys_gpu_query_caps, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "gpu_query_caps"
    };
    syscall_table[SYS_GPU_BUF_ALLOC] = (syscall_entry_t){
        sys_gpu_buf_alloc, 0, "gpu_buf_alloc"
    };
    syscall_table[SYS_GPU_BUF_FREE] = (syscall_entry_t){
        sys_gpu_buf_free, SYSCALL_FLAG_RT, "gpu_buf_free"
    };
    syscall_table[SYS_GPU_BUF_MAP_CPU] = (syscall_entry_t){
        sys_gpu_buf_map_cpu, 0, "gpu_buf_map_cpu"
    };
    syscall_table[SYS_GPU_BUF_UNMAP_CPU] = (syscall_entry_t){
        sys_gpu_buf_unmap_cpu, SYSCALL_FLAG_RT, "gpu_buf_unmap_cpu"
    };
    syscall_table[SYS_GPU_CMDQUEUE_CREATE] = (syscall_entry_t){
        sys_gpu_cmdqueue_create, 0, "gpu_cmdqueue_create"
    };
    syscall_table[SYS_GPU_CMDQUEUE_DESTROY] = (syscall_entry_t){
        sys_gpu_cmdqueue_destroy, 0, "gpu_cmdqueue_destroy"
    };
    syscall_table[SYS_GPU_CMDBUF_BEGIN] = (syscall_entry_t){
        sys_gpu_cmdbuf_begin, SYSCALL_FLAG_RT, "gpu_cmdbuf_begin"
    };
    syscall_table[SYS_GPU_CMDBUF_SUBMIT] = (syscall_entry_t){
        sys_gpu_cmdbuf_submit, SYSCALL_FLAG_RT, "gpu_cmdbuf_submit"
    };
    syscall_table[SYS_GPU_FENCE_WAIT] = (syscall_entry_t){
        sys_gpu_fence_wait, SYSCALL_FLAG_RT, "gpu_fence_wait"
    };
    syscall_table[SYS_GPU_FENCE_QUERY] = (syscall_entry_t){
        sys_gpu_fence_query, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "gpu_fence_query"
    };
    syscall_table[SYS_GPU_FENCE_DESTROY] = (syscall_entry_t){
        sys_gpu_fence_destroy, SYSCALL_FLAG_RT, "gpu_fence_destroy"
    };
    syscall_table[SYS_GPU_PRESENT] = (syscall_entry_t){
        sys_gpu_present, SYSCALL_FLAG_RT, "gpu_present"
    };
    syscall_table[SYS_GPU_COMPUTE_DISPATCH] = (syscall_entry_t){
        sys_gpu_compute_dispatch, SYSCALL_FLAG_RT, "gpu_compute_dispatch"
    };

    uart_puts("[GPU] Syscall 120-133 registrate\n");
}

/* ════════════════════════════════════════════════════════════════════
 * gpu_present_fullscreen — helper per main.c (non è una syscall)
 *
 * Esegue il present dell'intero framebuffer tramite il backend attivo.
 * Con virtio-GPU: fb_flush + TRANSFER_TO_HOST_2D + RESOURCE_FLUSH.
 * Con SW backend: equivalente a fb_flush() (già chiamato).
 * ════════════════════════════════════════════════════════════════════ */
void gpu_present_fullscreen(void)
{
    uint32_t *fb = fb_get_ptr();
    if (!fb) return;

    static gpu_buf_entry_t fb_entry = {
        .size   = FB_WIDTH * FB_HEIGHT * 4U,
        .type   = GPU_BUF_SCANOUT,
        .in_use = true,
    };
    fb_entry.phys = (uint64_t)(uintptr_t)fb;

    static gpu_fence_entry_t fence = {
        .id     = 0U,
        .in_use = true,
    };
    fence.state = GPU_FENCE_PENDING;

    gpu_active_backend->present(&fb_entry,
                                 0U, 0U, FB_WIDTH, FB_HEIGHT,
                                 &fence);
}

void gpu_get_caps(gpu_caps_t *out)
{
    if (!out || !gpu_active_backend) return;
    gpu_active_backend->query_caps(out);
}

void gpu_get_scanout_info(gpu_scanout_info_t *out)
{
    if (!out || !gpu_active_backend || !gpu_active_backend->query_scanout)
        return;
    gpu_active_backend->query_scanout(out);
}

int gpu_flush_cache(gpu_buf_handle_t handle)
{
    if (!buf_valid(handle)) return -EINVAL;
    return gpu_buf_flush_entry(&gpu_buf_pool[handle - 1U]);
}

static void gpu_mm_init(void)
{
    for (uint32_t i = 0U; i < GPU_MAX_BUFS; i++) {
        gpu_buf_pool[i].phys = 0U;
        gpu_buf_pool[i].iova = 0U;
        gpu_buf_pool[i].size = 0U;
        gpu_buf_pool[i].capacity = 0U;
        gpu_buf_pool[i].type = 0U;
        gpu_buf_pool[i].pool_kind = 0U;
        gpu_buf_pool[i].pool_slot = GPU_MM_INVALID_SLOT;
        gpu_buf_pool[i].next_free = (i + 1U < GPU_MAX_BUFS) ? (uint16_t)(i + 1U)
                                                            : GPU_MM_INVALID_SLOT;
        gpu_buf_pool[i].mapped = false;
        gpu_buf_pool[i].cpu_dirty = false;
        gpu_buf_pool[i].pinned = false;
        gpu_buf_pool[i].in_use = false;
    }
    gpu_mm_handle_head = 0U;

    for (uint32_t i = 0U; i < GPU_MM_SCANOUT_SLOTS; i++)
        gpu_mm_scanout_next[i] = (i + 1U < GPU_MM_SCANOUT_SLOTS)
                               ? (uint16_t)(i + 1U) : GPU_MM_INVALID_SLOT;
    gpu_mm_scanout_head = 0U;

    for (uint32_t i = 0U; i < GPU_MM_GENERAL_SLOTS; i++)
        gpu_mm_general_next[i] = (i + 1U < GPU_MM_GENERAL_SLOTS)
                               ? (uint16_t)(i + 1U) : GPU_MM_INVALID_SLOT;
    gpu_mm_general_head = 0U;

    for (uint32_t i = 0U; i < GPU_MM_CMD_SLOTS; i++)
        gpu_mm_cmd_next[i] = (i + 1U < GPU_MM_CMD_SLOTS)
                           ? (uint16_t)(i + 1U) : GPU_MM_INVALID_SLOT;
    gpu_mm_cmd_head = 0U;
}

static int gpu_buf_flush_entry(gpu_buf_entry_t *e)
{
    uintptr_t base;
    uintptr_t end;

    if (!e || !e->in_use) return -EINVAL;

    base = (uintptr_t)e->phys;
    end  = base + e->size;
    for (uintptr_t addr = base & ~(uintptr_t)63U; addr < end; addr += 64U)
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");

    e->cpu_dirty = false;
    return 0;
}

static void gpu_flush_dirty_buffers(void)
{
    for (uint32_t i = 0U; i < GPU_MAX_BUFS; i++) {
        if (!gpu_buf_pool[i].in_use) continue;
        if (!gpu_buf_pool[i].cpu_dirty) continue;
        (void)gpu_buf_flush_entry(&gpu_buf_pool[i]);
    }
}
