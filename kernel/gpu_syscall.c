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
#include "pmm.h"
#include "uart.h"

/* ════════════════════════════════════════════════════════════════════
 * Pool globali
 * ════════════════════════════════════════════════════════════════════ */

gpu_buf_entry_t    gpu_buf_pool   [GPU_MAX_BUFS];
gpu_queue_entry_t  gpu_queue_pool [GPU_MAX_QUEUES];
gpu_cmdbuf_entry_t gpu_cmdbuf_pool[GPU_MAX_CMDBUFS];
gpu_fence_entry_t  gpu_fence_pool [GPU_MAX_FENCES];
uint64_t           gpu_next_fence_id = 1;

/* ── Allocatori di slot (1-based per buf/queue; cerca ID per fence) ── */

static uint32_t buf_slot_alloc(void)
{
    for (int i = 0; i < GPU_MAX_BUFS; i++)
        if (!gpu_buf_pool[i].in_use) return (uint32_t)(i + 1);
    return 0;
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

    if (size == 0 || size > 256u * 1024u * 1024u) return GPU_INVALID_BUF;

    uint32_t h = buf_slot_alloc();
    if (h == 0) return GPU_INVALID_BUF;

    /* Buddy order */
    uint64_t pages = ((uint64_t)size + 4095u) / 4096u;
    uint32_t order = 0;
    uint64_t p = pages;
    while (p > 1) { p >>= 1; order++; }
    if ((1ull << order) < pages) order++;
    if (order > 10) order = 10;

    uint64_t pa = phys_alloc_pages(order);
    if (pa == 0) return GPU_INVALID_BUF;

    /* Zero-fill */
    uint8_t *ptr = (uint8_t *)(uintptr_t)pa;
    for (uint64_t i = 0; i < (1ull << order) * 4096u; i++) ptr[i] = 0;

    gpu_buf_pool[h-1].phys   = pa;
    gpu_buf_pool[h-1].size   = (uint32_t)((1ull << order) * 4096u);
    gpu_buf_pool[h-1].type   = type;
    gpu_buf_pool[h-1].mapped = false;
    gpu_buf_pool[h-1].in_use = true;
    return (uint64_t)h;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 122 — gpu_buf_free
 * args[0] = handle
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_gpu_buf_free(uint64_t args[6])
{
    uint32_t h = (uint32_t)args[0];
    if (!buf_valid(h)) return ERR(EINVAL);

    gpu_buf_entry_t *e = &gpu_buf_pool[h-1];
    uint64_t pages = ((uint64_t)e->size + 4095u) / 4096u;
    uint32_t order = 0;
    uint64_t p = pages;
    while (p > 1) { p >>= 1; order++; }
    if ((1ull << order) < pages) order++;
    if (order > 10) order = 10;

    phys_free_pages(e->phys, order);
    e->in_use = false;
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

    /* Flush D-cache: garantisce coerenza CPU → GPU DMA */
    uintptr_t base = (uintptr_t)e->phys;
    uintptr_t end  = base + e->size;
    for (uintptr_t addr = base & ~63u; addr < end; addr += 64) {
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");

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

    /* Polling immediato */
    if (timeout_ns == 0)
        return (f->state != GPU_FENCE_PENDING) ? 0 : ERR(EAGAIN);

    /* Attesa con deadline; UINT64_MAX = infinito */
    bool     infinite    = (timeout_ns == (uint64_t)-1ULL);
    uint64_t deadline_ns = infinite ? 0 : timer_now_ns() + timeout_ns;

    while (f->state == GPU_FENCE_PENDING) {
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
