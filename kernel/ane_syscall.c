/*
 * EnlilOS Microkernel - ANE Syscall Layer (M3-03)
 *
 * Implementa le syscall 100-109 per l'Apple Neural Engine.
 *
 * Pool statici (zero allocazione nel path di inferenza):
 *   ane_buf_pool  [64] — buffer DMA
 *   ane_model_pool[16] — modelli .hwx caricati
 *   ane_job_pool  [32] — job in coda / completati
 *
 * Handle: 1-based (0 / ANE_INVALID_HANDLE = invalido).
 * Tutti gli handle sono uint32_t opachi per user-space.
 *
 * RT path: submit (105) e wait (106) sono O(1) / O(bounded).
 * Setup path: buf_alloc (101), model_load (103) possono allocare.
 */

#include "ane_internal.h"
#include "syscall.h"
#include "sched.h"
#include "timer.h"
#include "pmm.h"
#include "uart.h"

/* ════════════════════════════════════════════════════════════════════
 * Pool globali (condivisi con i backend)
 * ════════════════════════════════════════════════════════════════════ */

ane_buf_entry_t   ane_buf_pool  [ANE_MAX_BUFS];
ane_model_entry_t ane_model_pool[ANE_MAX_MODELS];
ane_job_entry_t   ane_job_pool  [ANE_MAX_JOBS];

/* ── Allocatori di slot dal pool ────────────────────────────────────── */

/* Ritorna handle 1-based, 0 se pool esaurito */
static uint32_t buf_slot_alloc(void)
{
    for (int i = 0; i < ANE_MAX_BUFS; i++)
        if (!ane_buf_pool[i].in_use) return (uint32_t)(i + 1);
    return 0;
}

static uint32_t model_slot_alloc(void)
{
    for (int i = 0; i < ANE_MAX_MODELS; i++)
        if (!ane_model_pool[i].in_use) return (uint32_t)(i + 1);
    return 0;
}

static uint32_t job_slot_alloc(void)
{
    for (int i = 0; i < ANE_MAX_JOBS; i++)
        if (!ane_job_pool[i].in_use) return (uint32_t)(i + 1);
    return 0;
}

/* Validazione handle (0 e > MAX sono invalidi) */
static bool buf_handle_valid(uint32_t h)
{
    return h >= 1 && h <= ANE_MAX_BUFS && ane_buf_pool[h-1].in_use;
}
static bool model_handle_valid(uint32_t h)
{
    return h >= 1 && h <= ANE_MAX_MODELS && ane_model_pool[h-1].in_use;
}
static bool job_handle_valid(uint32_t h)
{
    return h >= 1 && h <= ANE_MAX_JOBS && ane_job_pool[h-1].in_use;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 100 — ane_query_caps
 *
 * args[0] = puntatore a ane_caps_t
 * RT-safe O(1): legge MIDR_EL1 (già letto al boot, cacheable).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_query_caps(uint64_t args[6])
{
    ane_caps_t *out = (ane_caps_t *)(uintptr_t)args[0];
    if (!out) return ERR(EFAULT);

    ane_active_backend->query_caps(out);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 101 — ane_buf_alloc
 *
 * args[0] = size (byte)
 * args[1] = flags (ANE_BUF_*)
 * Ritorna: handle (uint32_t) oppure ANE_INVALID_HANDLE su errore
 *
 * Alloca pagine fisiche contigue via buddy, zero-fill.
 * Non chiamare nel hot path RT.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_buf_alloc(uint64_t args[6])
{
    uint32_t size  = (uint32_t)args[0];
    uint32_t flags = (uint32_t)args[1];

    if (size == 0 || size > 64u * 1024u * 1024u) return ANE_INVALID_HANDLE;

    uint32_t h = buf_slot_alloc();
    if (h == 0) return ANE_INVALID_HANDLE;

    /* Calcola ordine buddy */
    uint64_t pages = ((uint64_t)size + 4095u) / 4096u;
    uint32_t order = 0;
    uint64_t p = pages;
    while (p > 1) { p >>= 1; order++; }
    if ((1ull << order) < pages) order++;
    if (order > 10) order = 10;

    uint64_t pa = phys_alloc_pages(order);
    if (pa == 0) return ANE_INVALID_HANDLE;

    /* Zero-fill: evita leakage */
    uint8_t *ptr = (uint8_t *)(uintptr_t)pa;
    for (uint64_t i = 0; i < ((uint64_t)1 << order) * 4096u; i++) ptr[i] = 0;

    ane_buf_entry_t *e = &ane_buf_pool[h - 1];
    e->phys   = pa;
    e->size   = size;
    e->flags  = flags;
    e->in_use = true;

    return (uint64_t)h;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 102 — ane_buf_free
 *
 * args[0] = handle
 * RT-safe O(1): rilascio al pool.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_buf_free(uint64_t args[6])
{
    uint32_t h = (uint32_t)args[0];
    if (!buf_handle_valid(h)) return ERR(EINVAL);

    ane_buf_entry_t *e = &ane_buf_pool[h - 1];

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
 * Syscall 103 — ane_model_load
 *
 * args[0] = puntatore al buffer .hwx già caricato in memoria
 * args[1] = dimensione in byte
 * Ritorna: model_handle oppure ANE_INVALID_HANDLE
 *
 * Valida l'header HWX, alloca un buffer DMA, copia il modello.
 * Solo per uso setup — non chiamare in task hard-RT.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_model_load(uint64_t args[6])
{
    const void *hwx_ptr  = (const void *)(uintptr_t)args[0];
    uint32_t    hwx_size = (uint32_t)args[1];

    if (!hwx_ptr || hwx_size < sizeof(ane_hwx_header_t)) return ANE_INVALID_HANDLE;
    if (hwx_size > 64u * 1024u * 1024u) return ANE_INVALID_HANDLE;

    /* Valida header */
    const ane_hwx_header_t *hdr = (const ane_hwx_header_t *)hwx_ptr;
    if (hdr->magic != ANE_HWX_MAGIC) return ANE_INVALID_HANDLE;

    /* Alloca slot modello */
    uint32_t mh = model_slot_alloc();
    if (mh == 0) return ANE_INVALID_HANDLE;

    /* Alloca buffer DMA per i dati del modello */
    uint64_t buf_args[6] = { hwx_size, ANE_BUF_PINNED, 0, 0, 0, 0 };
    uint64_t bh = sys_ane_buf_alloc(buf_args);
    if (bh == ANE_INVALID_HANDLE) return ANE_INVALID_HANDLE;

    /* Copia il modello nel buffer DMA */
    uint8_t       *dst = (uint8_t *)(uintptr_t)ane_buf_pool[bh - 1].phys;
    const uint8_t *src = (const uint8_t *)hwx_ptr;
    for (uint32_t i = 0; i < hwx_size; i++) dst[i] = src[i];

    ane_model_entry_t *m = &ane_model_pool[mh - 1];
    m->buf_handle  = (uint32_t)bh;
    m->input_size  = hdr->input_size;
    m->output_size = hdr->output_size;
    m->in_use      = true;

    return (uint64_t)mh;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 104 — ane_model_unload
 *
 * args[0] = model_handle
 * RT-safe O(1).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_model_unload(uint64_t args[6])
{
    uint32_t mh = (uint32_t)args[0];
    if (!model_handle_valid(mh)) return ERR(EINVAL);

    ane_model_entry_t *m = &ane_model_pool[mh - 1];

    /* Libera il buffer DMA del modello */
    uint64_t free_args[6] = { m->buf_handle, 0, 0, 0, 0, 0 };
    sys_ane_buf_free(free_args);

    m->in_use = false;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 105 — ane_inference_submit
 *
 * args[0] = model_handle
 * args[1] = in_buf_handle
 * args[2] = out_buf_handle
 * args[3] = priority (0 = eredita da current_task)
 * Ritorna: job_handle oppure ANE_INVALID_HANDLE
 *
 * RT-safe: O(1). Non-blocking.
 * Su SW backend: l'inferenza viene eseguita immediatamente (sincrona)
 * e il job risulta DONE al ritorno. L'API è comunque corretta.
 * Su HW backend (futuro): submette al ring e ritorna subito.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_inference_submit(uint64_t args[6])
{
    uint32_t mh   = (uint32_t)args[0];
    uint32_t ibh  = (uint32_t)args[1];
    uint32_t obh  = (uint32_t)args[2];
    uint8_t  prio = (uint8_t) args[3];

    if (!model_handle_valid(mh)) return ANE_INVALID_HANDLE;
    if (!buf_handle_valid(ibh))  return ANE_INVALID_HANDLE;
    if (!buf_handle_valid(obh))  return ANE_INVALID_HANDLE;

    uint32_t jh = job_slot_alloc();
    if (jh == 0) return ANE_INVALID_HANDLE;

    /* Eredita priorità dal task se non specificata */
    if (prio == 0 && current_task)
        prio = current_task->priority;

    ane_job_entry_t *job = &ane_job_pool[jh - 1];
    job->model_handle   = mh;
    job->in_buf_handle  = ibh;
    job->out_buf_handle = obh;
    job->priority       = prio;
    job->state          = ANE_JOB_PENDING;
    job->error_code     = 0;
    job->submit_ns      = timer_now_ns();
    job->done_ns        = 0;
    job->cycles         = 0;
    job->in_use         = true;

    /* Esegue l'inferenza tramite il backend attivo */
    job->state = ANE_JOB_RUNNING;
    int ret = ane_active_backend->inference_execute(job);
    if (ret != 0) {
        job->state      = ANE_JOB_ERROR;
        job->error_code = (uint16_t)(-ret);
    }

    return (uint64_t)jh;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 106 — ane_inference_wait
 *
 * args[0] = job_handle
 * args[1] = timeout_ns (0 = polling immediato)
 * Ritorna: 0 OK, -EAGAIN timeout, -EINVAL handle non valido
 *
 * RT-safe con timeout > 0: bounded wait.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_inference_wait(uint64_t args[6])
{
    uint32_t jh         = (uint32_t)args[0];
    uint64_t timeout_ns = args[1];

    if (!job_handle_valid(jh)) return ERR(EINVAL);

    ane_job_entry_t *job = &ane_job_pool[jh - 1];

    /* Polling immediato */
    if (timeout_ns == 0) {
        return (job->state == ANE_JOB_DONE || job->state == ANE_JOB_ERROR)
               ? 0 : ERR(EAGAIN);
    }

    /* Attesa con deadline.
     * timeout_ns == UINT64_MAX → attesa illimitata (usata da inference_run). */
    bool infinite = (timeout_ns == (uint64_t)-1ULL);
    uint64_t deadline_ns = infinite ? 0 : timer_now_ns() + timeout_ns;

    while (job->state != ANE_JOB_DONE && job->state != ANE_JOB_ERROR) {
        if (!infinite && timer_now_ns() >= deadline_ns) return ERR(EAGAIN);
        sched_yield();
    }

    return (job->state == ANE_JOB_ERROR) ? ERR(job->error_code) : 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 107 — ane_inference_run
 *
 * args[0] = model_handle, args[1] = in_buf, args[2] = out_buf
 * Submit sincrono: non ritorna finché l'inferenza non è completata.
 * Non chiamare da task hard-RT (nessun timeout).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_inference_run(uint64_t args[6])
{
    uint64_t submit_args[6] = { args[0], args[1], args[2], 0, 0, 0 };
    uint64_t jh = sys_ane_inference_submit(submit_args);
    if (jh == ANE_INVALID_HANDLE) return ERR(EINVAL);

    /* Attesa senza timeout */
    uint64_t wait_args[6] = { jh, (uint64_t)-1ULL, 0, 0, 0, 0 };
    uint64_t ret = sys_ane_inference_wait(wait_args);

    /* Rilascia il job dopo l'uso sincrono */
    ane_job_pool[jh - 1].in_use = false;

    return ret;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 108 — ane_job_cancel
 *
 * args[0] = job_handle
 * RT-safe O(1): marca CANCELLED se il job è ancora PENDING.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_job_cancel(uint64_t args[6])
{
    uint32_t jh = (uint32_t)args[0];
    if (!job_handle_valid(jh)) return ERR(EINVAL);

    ane_job_entry_t *job = &ane_job_pool[jh - 1];

    if (job->state == ANE_JOB_PENDING || job->state == ANE_JOB_RUNNING) {
        job->state   = ANE_JOB_CANCELLED;
        job->done_ns = timer_now_ns();
    }
    /* Se già DONE/ERROR: non fa nulla, ritorna comunque 0 */
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 109 — ane_job_status
 *
 * args[0] = job_handle
 * args[1] = puntatore a ane_job_stat_t
 * RT-safe O(1): lettura diretta dal pool.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_ane_job_status(uint64_t args[6])
{
    uint32_t       jh  = (uint32_t)args[0];
    ane_job_stat_t *out = (ane_job_stat_t *)(uintptr_t)args[1];

    if (!job_handle_valid(jh)) return ERR(EINVAL);
    if (!out)                  return ERR(EFAULT);

    ane_job_entry_t *job = &ane_job_pool[jh - 1];
    out->state      = job->state;
    out->error_code = job->error_code;
    out->submit_ns  = job->submit_ns;
    out->done_ns    = job->done_ns;
    out->cycles     = job->cycles;

    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * ane_init — rileva backend e registra syscall 100-109
 * ════════════════════════════════════════════════════════════════════ */
void ane_init(void)
{
    /* 1. Seleziona backend HW o SW */
    ane_backend_init();

    /* 2. Stampa capacità */
    ane_caps_t caps;
    ane_active_backend->query_caps(&caps);

    uart_puts("[ANE] Capacità: ");
    if (caps.flags & ANE_CAP_HW) {
        uart_puts("HW v0x");
        uart_putc("0123456789ABCDEF"[(caps.version >> 4) & 0xF]);
        uart_putc("0123456789ABCDEF"[caps.version & 0xF]);
        uart_puts(" | ");
        /* Stampa TOPS (tops_x100 / 100 con un decimale) */
        uint32_t tops_int  = caps.tops_x100 / 100;
        uint32_t tops_frac = (caps.tops_x100 % 100) / 10;
        uart_putc('0' + (char)(tops_int / 10 % 10));
        uart_putc('0' + (char)(tops_int % 10));
        uart_putc('.');
        uart_putc('0' + (char)tops_frac);
        uart_puts(" TOPS");
    } else {
        uart_puts("SW fallback (CPU inference)");
    }
    uart_puts(" | bufs=");
    uart_putc('0' + ANE_MAX_BUFS / 10);
    uart_putc('0' + ANE_MAX_BUFS % 10);
    uart_puts(" models=");
    uart_putc('0' + ANE_MAX_MODELS / 10);
    uart_putc('0' + ANE_MAX_MODELS % 10);
    uart_puts(" jobs=");
    uart_putc('0' + ANE_MAX_JOBS / 10);
    uart_putc('0' + ANE_MAX_JOBS % 10);
    uart_puts("\n");

    /* 3. Registra syscall 100-109 nella tabella globale */
    extern syscall_entry_t syscall_table[256];

    syscall_table[SYS_ANE_QUERY_CAPS] = (syscall_entry_t){
        sys_ane_query_caps, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "ane_query_caps"
    };
    syscall_table[SYS_ANE_BUF_ALLOC] = (syscall_entry_t){
        sys_ane_buf_alloc, 0, "ane_buf_alloc"
    };
    syscall_table[SYS_ANE_BUF_FREE] = (syscall_entry_t){
        sys_ane_buf_free, SYSCALL_FLAG_RT, "ane_buf_free"
    };
    syscall_table[SYS_ANE_MODEL_LOAD] = (syscall_entry_t){
        sys_ane_model_load, 0, "ane_model_load"
    };
    syscall_table[SYS_ANE_MODEL_UNLOAD] = (syscall_entry_t){
        sys_ane_model_unload, SYSCALL_FLAG_RT, "ane_model_unload"
    };
    syscall_table[SYS_ANE_INFERENCE_SUBMIT] = (syscall_entry_t){
        sys_ane_inference_submit, SYSCALL_FLAG_RT, "ane_inference_submit"
    };
    syscall_table[SYS_ANE_INFERENCE_WAIT] = (syscall_entry_t){
        sys_ane_inference_wait, SYSCALL_FLAG_RT, "ane_inference_wait"
    };
    syscall_table[SYS_ANE_INFERENCE_RUN] = (syscall_entry_t){
        sys_ane_inference_run, 0, "ane_inference_run"
    };
    syscall_table[SYS_ANE_JOB_CANCEL] = (syscall_entry_t){
        sys_ane_job_cancel, SYSCALL_FLAG_RT, "ane_job_cancel"
    };
    syscall_table[SYS_ANE_JOB_STATUS] = (syscall_entry_t){
        sys_ane_job_status, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "ane_job_status"
    };

    uart_puts("[ANE] Syscall 100-109 registrate\n");
}
