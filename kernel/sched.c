/*
 * EnlilOS Microkernel - Fixed-Priority Preemptive Scheduler (M2-03)
 *
 * Implementazione FPP con:
 *   - ready_bitmap[4] (256 bit): trova la priorità massima in O(1)
 *   - run_queue[256]:  FIFO intrusive singly-linked per priorità
 *   - task_pool[SCHED_MAX_TASKS]: pool statico di sched_tcb_t (no kmalloc nel scheduler)
 *   - sched_context_switch: assembly in sched_switch.S
 *   - Preemption: need_resched settato da sched_tick(), letto da vectors.S
 */

#include "sched.h"
#include "syscall.h"
#include "elf_loader.h"
#include "futex.h"
#include "kmon.h"
#include "ksem.h"
#include "mreact.h"
#include "mmu.h"
#include "signal.h"
#include "timer.h"
#include "pmm.h"
#include "uart.h"
#include "vmm.h"

extern void *memset(void *dst, int value, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);
extern void syscall_task_cleanup(sched_tcb_t *task);

/* ── Costanti interne ────────────────────────────────────────────── */

#define TICK_NS     (1000000ULL)    /* 1ms in nanosecondi               */

/* ── Pool statico di task ─────────────────────────────────────────── */

static sched_tcb_t    task_pool[SCHED_MAX_TASKS];
static uint32_t  task_count;
static uint32_t  next_pid;
static uint8_t   donated_priority[SCHED_MAX_TASKS];

/* Stack statici per idle (nessuna alloc da buddy necessaria al boot) */
static uint8_t   idle_stack[TASK_STACK_SIZE] __attribute__((aligned(16)));

typedef struct {
    mm_space_t *mm;
    uint64_t    kernel_stack_pa;
    uint64_t    user_entry;
    uint64_t    user_sp;
    uint64_t    argc;
    uint64_t    argv;
    uint64_t    envp;
    uint64_t    auxv;
    uint64_t    tpidr_el0;   /* thread pointer user-space (TPIDR_EL0) — M11-01b */
    uint64_t    child_set_tid_uva;
    uint64_t    clear_child_tid_uva;
    uint8_t     is_user;
    uint8_t     resume_from_frame;
    uint8_t     proc_slot;
    uint8_t     _pad[5];
    int32_t     exit_code;
    exception_frame_t start_frame;
} sched_task_ctx_t;

typedef struct {
    mm_space_t *mm;
    uint32_t    tgid;
    uint32_t    parent_pid;
    uint32_t    pgid;
    uint32_t    sid;
    int32_t     wait_exit_code;
    int32_t     group_exit_code;
    uint16_t    refcount;
    uint8_t     in_use;
    uint8_t     waitable;
    uint8_t     group_exiting;
    uint8_t     abi_mode;
    char        exec_path[SCHED_EXEC_PATH_MAX];
    rlimit64_t  rlimits[RLIMIT_NLIMITS];
} sched_proc_ctx_t;

static sched_task_ctx_t task_ctx[SCHED_MAX_TASKS];
static sched_proc_ctx_t proc_ctx[SCHED_MAX_TASKS];

static void sched_task_finish_exit(sched_tcb_t *task, int32_t code);

/* ── Scheduler state ─────────────────────────────────────────────── */

/*
 * ready_bitmap[4]: bit i settato ↔ esiste almeno un task READY a priorità i.
 * Parola 0 → priorità 0-63, parola 1 → 64-127, ecc.
 * Invariante mantenuta da rq_push/rq_pop.
 */
static uint64_t ready_bitmap[4];

/*
 * Run queue per priorità: singly-linked FIFO.
 * rq_head[p] → primo task da schedulare (front della coda)
 * rq_tail[p] → ultimo task inserito (back della coda)
 */
static sched_tcb_t *rq_head[256];
static sched_tcb_t *rq_tail[256];

/* Variabili globali esportate (usate da vectors.S e da altri moduli) */
sched_tcb_t          *current_task;
volatile uint32_t need_resched;

static inline uint32_t task_slot(const sched_tcb_t *t)
{
    return (uint32_t)(t - task_pool);
}

static inline sched_task_ctx_t *ctx_of(const sched_tcb_t *t)
{
    if (!t) return NULL;
    return &task_ctx[task_slot(t)];
}

static inline sched_proc_ctx_t *proc_of(const sched_tcb_t *t)
{
    sched_task_ctx_t *ctx = ctx_of(t);

    if (!ctx || ctx->proc_slot >= SCHED_MAX_TASKS)
        return NULL;
    return proc_ctx[ctx->proc_slot].in_use ? &proc_ctx[ctx->proc_slot] : NULL;
}

static inline uint32_t tgid_of(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);
    return proc ? proc->tgid : (t ? t->pid : 0U);
}

static inline uint32_t parent_pid_of(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);
    return proc ? proc->parent_pid : 0U;
}

static inline uint32_t pgid_of(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);
    return proc ? proc->pgid : 0U;
}

static inline uint32_t sid_of(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);
    return proc ? proc->sid : 0U;
}

static inline uint8_t eff_prio_of(const sched_tcb_t *t)
{
    uint8_t donated;

    if (!t) return PRIO_IDLE;
    donated = donated_priority[task_slot(t)];
    return (donated < t->priority) ? donated : t->priority;
}

/* ── Helpers UART ─────────────────────────────────────────────────── */

static void pr_dec(uint64_t v)
{
    if (v == 0) { uart_putc('0'); return; }
    char buf[20]; int len = 0;
    while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = len - 1; i >= 0; i--) uart_putc(buf[i]);
}

static void pr_hex64(uint64_t v)
{
    static const char h[] = "0123456789ABCDEF";
    uart_puts("0x");
    for (int s = 60; s >= 0; s -= 4) uart_putc(h[(v >> s) & 0xF]);
}

static void sched_strlcpy(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0U;

    if (!dst || cap == 0U)
        return;

    while (src && src[i] != '\0' && i + 1U < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* ── Ready bitmap: O(1) ──────────────────────────────────────────── */

static inline void bitmap_set(uint8_t prio)
{
    ready_bitmap[prio >> 6] |= (1ULL << (prio & 63));
}

static inline void bitmap_clear(uint8_t prio)
{
    ready_bitmap[prio >> 6] &= ~(1ULL << (prio & 63));
}

/*
 * bitmap_find_first — trova la priorità più alta (numero più basso) in O(1).
 * Usa CTZ (count trailing zeros) = RBIT+CLZ su AArch64.
 * Ritorna -1 se nessun bit è settato.
 */
static int bitmap_find_first(void)
{
    for (int w = 0; w < 4; w++) {
        if (ready_bitmap[w]) {
            /* __builtin_ctzll: numero di zeri dalla parte bassa
             * = indice del primo bit settato nella parola [O(1)] */
            return w * 64 + __builtin_ctzll(ready_bitmap[w]);
        }
    }
    return -1;
}

/* ── Run queue FIFO per priorità: O(1) enqueue/dequeue ──────────── */

/* Aggiunge task 't' in coda alla run queue della sua priorità */
static void rq_push(sched_tcb_t *t)
{
    uint8_t p = eff_prio_of(t);
    t->next = NULL;
    if (rq_head[p] == NULL) {
        rq_head[p] = rq_tail[p] = t;
    } else {
        rq_tail[p]->next = t;
        rq_tail[p] = t;
    }
    bitmap_set(p);
}

/* Estrae il primo task dalla run queue della priorità 'p' */
static sched_tcb_t *rq_pop(uint8_t p)
{
    sched_tcb_t *t = rq_head[p];
    if (t == NULL) return NULL;
    rq_head[p] = t->next;
    if (rq_head[p] == NULL) {
        rq_tail[p] = NULL;
        bitmap_clear(p);
    }
    t->next = NULL;
    return t;
}

static int rq_remove(sched_tcb_t *t, uint8_t p)
{
    sched_tcb_t *prev = NULL;
    sched_tcb_t *cur = rq_head[p];

    while (cur) {
        if (cur == t) {
            if (prev)
                prev->next = cur->next;
            else
                rq_head[p] = cur->next;

            if (rq_tail[p] == cur)
                rq_tail[p] = prev;
            if (rq_head[p] == NULL)
                bitmap_clear(p);

            cur->next = NULL;
            return 0;
        }
        prev = cur;
        cur = cur->next;
    }

    return -1;
}

/* ── IRQ save/restore per critical sections ─────────────────────── */

static inline uint64_t irq_save(void)
{
    uint64_t flags;
    __asm__ volatile(
        "mrs %0, daif\n"
        "msr daifset, #2\n"
        : "=r"(flags) :: "memory"
    );
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

static __attribute__((noinline)) void sched_restore_irq_flags(uint64_t flags)
{
    irq_restore(flags);
}

static __attribute__((noinline)) void sched_return_irqs_masked(void)
{
    __asm__ volatile("" ::: "memory");
}

static uint64_t sched_alloc_kernel_stack(void)
{
    return phys_alloc_pages(TASK_STACK_ORDER);
}

static void proc_rlimit_defaults(sched_proc_ctx_t *proc)
{
    for (uint32_t i = 0U; i < RLIMIT_NLIMITS; i++) {
        proc->rlimits[i].rlim_cur = RLIM64_INFINITY;
        proc->rlimits[i].rlim_max = RLIM64_INFINITY;
    }
    /* Stack: 8 MB soft, unlimited hard (Linux default) */
    proc->rlimits[RLIMIT_STACK].rlim_cur = 8U * 1024U * 1024U;
    proc->rlimits[RLIMIT_STACK].rlim_max = RLIM64_INFINITY;
    /* Core: no core dumps */
    proc->rlimits[RLIMIT_CORE].rlim_cur  = 0U;
    proc->rlimits[RLIMIT_CORE].rlim_max  = 0U;
    /* NOFILE: kernel FD limit */
    proc->rlimits[RLIMIT_NOFILE].rlim_cur = (uint64_t)MAX_FD;
    proc->rlimits[RLIMIT_NOFILE].rlim_max = (uint64_t)MAX_FD;
    /* NPROC: bounded by task pool */
    proc->rlimits[RLIMIT_NPROC].rlim_cur = (uint64_t)SCHED_MAX_TASKS;
    proc->rlimits[RLIMIT_NPROC].rlim_max = (uint64_t)SCHED_MAX_TASKS;
    /* MEMLOCK: 64 KB */
    proc->rlimits[RLIMIT_MEMLOCK].rlim_cur = 64U * 1024U;
    proc->rlimits[RLIMIT_MEMLOCK].rlim_max = 64U * 1024U;
    /* NICE / RTPRIO: 0 (non-RT) */
    proc->rlimits[RLIMIT_NICE].rlim_cur   = 0U;
    proc->rlimits[RLIMIT_NICE].rlim_max   = 0U;
    proc->rlimits[RLIMIT_RTPRIO].rlim_cur = 0U;
    proc->rlimits[RLIMIT_RTPRIO].rlim_max = 0U;
    /* MSGQUEUE: Linux default 819200 */
    proc->rlimits[RLIMIT_MSGQUEUE].rlim_cur = 819200U;
    proc->rlimits[RLIMIT_MSGQUEUE].rlim_max = 819200U;
}

static int proc_alloc(mm_space_t *mm, uint32_t tgid, uint32_t parent_pid,
                      uint32_t pgid, uint32_t sid)
{
    for (uint32_t i = 0U; i < SCHED_MAX_TASKS; i++) {
        sched_proc_ctx_t *proc = &proc_ctx[i];

        if (proc->in_use)
            continue;

        memset(proc, 0, sizeof(*proc));
        proc->mm        = mm ? mm : mmu_kernel_space();
        proc->tgid      = tgid;
        proc->parent_pid = parent_pid;
        proc->pgid      = pgid;
        proc->sid       = sid;
        proc->wait_exit_code = 0;
        proc->group_exit_code = 0;
        proc->refcount  = 1U;
        proc->in_use    = 1U;
        proc->waitable  = 0U;
        proc->group_exiting = 0U;
        proc->abi_mode  = SCHED_ABI_ENLILOS;
        proc_rlimit_defaults(proc);
        return (int)i;
    }

    return -1;
}

static int proc_retain(uint8_t slot)
{
    if (slot >= SCHED_MAX_TASKS || !proc_ctx[slot].in_use)
        return -1;
    proc_ctx[slot].refcount++;
    return 0;
}

static void proc_set_group_exit(uint8_t slot, int32_t code)
{
    sched_proc_ctx_t *proc;

    if (slot >= SCHED_MAX_TASKS || !proc_ctx[slot].in_use)
        return;

    proc = &proc_ctx[slot];
    proc->group_exiting = 1U;
    proc->group_exit_code = code;
}

static sched_tcb_t *proc_leader_task(const sched_proc_ctx_t *proc)
{
    if (!proc || proc->tgid == 0U)
        return NULL;
    return sched_task_find(proc->tgid);
}

static int proc_release(uint8_t slot, int32_t code, uint32_t *parent_pid_out)
{
    mm_space_t       *mm;
    sched_proc_ctx_t *proc;
    int32_t           final_code;
    sched_tcb_t      *leader;
    sched_task_ctx_t *leader_ctx;

    if (slot >= SCHED_MAX_TASKS || !proc_ctx[slot].in_use || proc_ctx[slot].refcount == 0U)
        return 0;

    proc = &proc_ctx[slot];
    proc->refcount--;
    if (proc->refcount != 0U)
        return 0;

    final_code = proc->group_exiting ? proc->group_exit_code : code;
    proc->waitable = 1U;
    proc->wait_exit_code = final_code;
    if (parent_pid_out)
        *parent_pid_out = proc->parent_pid;

    leader = proc_leader_task(proc);
    leader_ctx = ctx_of(leader);
    if (leader_ctx)
        leader_ctx->exit_code = final_code;

    mm = proc->mm;
    proc->mm = mmu_kernel_space();
    if (mm && mm != mmu_kernel_space())
        mmu_space_destroy(mm);
    return 1;
}

static int task_bind_new_process(sched_tcb_t *t, mm_space_t *mm,
                                 uint32_t parent_pid, uint32_t pgid,
                                 uint32_t sid)
{
    sched_task_ctx_t *ctx = ctx_of(t);
    int               slot;

    if (!t || !ctx)
        return -1;

    slot = proc_alloc(mm, t->pid, parent_pid,
                      (pgid != 0U) ? pgid : t->pid,
                      (sid != 0U) ? sid : t->pid);
    if (slot < 0)
        return -1;

    ctx->mm = mm ? mm : mmu_kernel_space();
    ctx->proc_slot = (uint8_t)slot;
    return 0;
}

static int task_bind_shared_process(sched_tcb_t *t, const sched_tcb_t *owner)
{
    sched_task_ctx_t *ctx;
    sched_proc_ctx_t *proc;
    uint8_t           slot;

    if (!t || !owner)
        return -1;

    ctx = ctx_of(owner);
    proc = proc_of(owner);
    if (!ctx || !proc)
        return -1;

    slot = ctx->proc_slot;
    if (proc_retain(slot) < 0)
        return -1;

    ctx = ctx_of(t);
    if (!ctx) {
        (void)proc_release(slot, 0, NULL);
        return -1;
    }

    ctx->mm = proc->mm;
    ctx->proc_slot = slot;
    return 0;
}

/* ── Allocazione task dal pool ───────────────────────────────────── */

static sched_tcb_t *task_alloc(void)
{
    uint32_t slot;

    if (task_count >= SCHED_MAX_TASKS) return NULL;
    sched_tcb_t *t = &task_pool[task_count++];
    slot = task_count - 1U;
    /* Zero-init già garantita dal BSS, ma azzeriamo esplicitamente
     * per i task re-usati (non in questa versione, ma per sicurezza) */
    t->sp          = 0;
    t->pid         = next_pid++;
    t->priority    = PRIO_NORMAL;
    t->state       = TCB_STATE_READY;
    t->flags       = TCB_FLAG_KERNEL;
    t->ticks_left  = SCHED_TICK_QUANTUM;
    t->runtime_ns  = 0;
    t->budget_ns   = 0;
    t->period_ms   = 0;
    t->deadline_ms = 0;
    t->name        = "(unnamed)";
    t->next        = NULL;

    task_ctx[slot].mm              = mmu_kernel_space();
    task_ctx[slot].kernel_stack_pa = 0ULL;
    task_ctx[slot].user_entry      = 0ULL;
    task_ctx[slot].user_sp         = 0ULL;
    task_ctx[slot].argc            = 0ULL;
    task_ctx[slot].argv            = 0ULL;
    task_ctx[slot].envp            = 0ULL;
    task_ctx[slot].auxv            = 0ULL;
    task_ctx[slot].child_set_tid_uva = 0ULL;
    task_ctx[slot].clear_child_tid_uva = 0ULL;
    task_ctx[slot].is_user         = 0U;
    task_ctx[slot].resume_from_frame = 0U;
    task_ctx[slot].proc_slot       = 0xFFU;
    task_ctx[slot].exit_code       = 0;
    memset(&task_ctx[slot].start_frame, 0, sizeof(task_ctx[slot].start_frame));
    donated_priority[slot]         = 0xFFU;
    return t;
}

/* ── Setup stack iniziale per un nuovo task ──────────────────────── */

/*
 * Prepara il frame iniziale per sched_context_switch sul task 't'.
 * Quando il task sarà schedulato per la prima volta:
 *   - sched_context_switch carica next->sp
 *   - ripristina x19-x29, x30
 *   - ret salta a x30 = task_entry_trampoline
 *   - trampoline abilita IRQ, chiama x19 = entry_fn
 *
 * Layout del frame (96 byte, crescita verso il basso):
 *   stack_top - 96  [x19=entry, x20=0] ← sp salvato in t->sp
 *   stack_top - 80  [x21=0, x22=0]
 *   stack_top - 64  [x23=0, x24=0]
 *   stack_top - 48  [x25=0, x26=0]
 *   stack_top - 32  [x27=0, x28=0]
 *   stack_top - 16  [x29=0, x30=trampoline]
 *   stack_top       ← cima dello stack
 */
static void task_setup_stack(sched_tcb_t *t, uint8_t *stack_top, sched_fn entry)
{
    /* Il frame ha 96 byte: 6 coppie di stp */
    uint64_t *frame = (uint64_t *)(uintptr_t)(stack_top - 96);

    frame[0] = (uint64_t)(uintptr_t)entry;          /* x19 = entry fn   */
    frame[1] = 0;                                    /* x20              */
    frame[2] = 0; frame[3] = 0;                     /* x21, x22         */
    frame[4] = 0; frame[5] = 0;                     /* x23, x24         */
    frame[6] = 0; frame[7] = 0;                     /* x25, x26         */
    frame[8] = 0; frame[9] = 0;                     /* x27, x28         */
    frame[10] = 0;                                   /* x29 (fp = 0)     */
    frame[11] = (uint64_t)(uintptr_t)task_entry_trampoline; /* x30 = LR */

    t->sp = (uint64_t)(uintptr_t)frame;
}

/* ══════════════════════════════════════════════════════════════════
 * sched_init — inizializzazione scheduler
 * ══════════════════════════════════════════════════════════════════ */
void sched_init(void)
{
    uart_puts("[SCHED] Inizializzazione scheduler FPP...\n");

    /* ── 1. Crea il task "kernel" per il contesto corrente ────────
     *
     * kernel_main è già in esecuzione su questo stack (boot stack).
     * Non allochiamo un nuovo stack — usiamo quello corrente.
     * t->sp = 0: verrà popolato alla prima chiamata di sched_context_switch.
     */
    sched_tcb_t *kern = task_alloc();
    kern->name      = "kernel";
    kern->priority  = PRIO_KERNEL;
    kern->state     = TCB_STATE_RUNNING;
    kern->flags     = TCB_FLAG_KERNEL;
    kern->ticks_left = SCHED_TICK_QUANTUM;
    if (task_bind_new_process(kern, mmu_kernel_space(), 0U, kern->pid, kern->pid) < 0)
        while (1) __asm__ volatile("wfe");
    /* sp = 0: non è ancora stato salvato; sched_context_switch lo salverà */
    current_task = kern;
    signal_task_init(kern, 0U);

    uart_puts("[SCHED]   task 'kernel' (pid=0, prio=");
    pr_dec(kern->priority);
    uart_puts(") — corrente\n");

    /* ── 2. Crea il task "idle" (priorità 255) ────────────────────
     *
     * Usa uno stack statico per evitare dipendenza dal buddy allocator.
     * Il task idle gira con WFE quando nessun altro task è READY.
     */
    sched_tcb_t *idle = task_alloc();
    idle->name      = "idle";
    idle->priority  = PRIO_IDLE;
    idle->state     = TCB_STATE_READY;
    idle->flags     = TCB_FLAG_KERNEL | TCB_FLAG_IDLE;
    idle->ticks_left = 0xFF; /* quantum illimitato per l'idle */
    if (task_bind_new_process(idle, mmu_kernel_space(), 0U, idle->pid, idle->pid) < 0)
        while (1) __asm__ volatile("wfe");
    signal_task_init(idle, 0U);

    uint8_t *idle_top = idle_stack + TASK_STACK_SIZE;
    task_setup_stack(idle, idle_top, (sched_fn)NULL); /* entry = NULL: trampoline gestisce */

    /* Per il task idle, usiamo una funzione speciale come entry */
    extern void sched_idle_fn(void);
    /* Sovrascrivi x19 nel frame con sched_idle_fn */
    uint64_t *frame = (uint64_t *)(uintptr_t)idle->sp;
    frame[0] = (uint64_t)(uintptr_t)sched_idle_fn;

    rq_push(idle);

    uart_puts("[SCHED]   task 'idle' (pid=1, prio=255) — ready\n");

    /* ── 3. Registra sched_tick come callback del timer ───────────*/
    timer_set_tick_callback(sched_tick);

    uart_puts("[SCHED] Scheduler FPP pronto — ");
    pr_dec(task_count);
    uart_puts(" task iniziali\n");
}

/* ── Funzione del task idle (esportata per sched_switch.S) ─────────
 *
 * Gira per sempre: WFE mette il core in sleep fino al prossimo evento
 * (IRQ, FIQ, SEV). Dopo WFE chiama schedule() per cedere la CPU se
 * un task a priorità più alta è diventato READY (wake-up event).
 */
void sched_idle_fn(void)
{
    while (1) {
        __asm__ volatile("wfe");
        schedule();
    }
}

/* ══════════════════════════════════════════════════════════════════
 * sched_task_create — crea un nuovo kernel task
 * ══════════════════════════════════════════════════════════════════ */
sched_tcb_t *sched_task_create(const char *name, sched_fn entry, uint8_t priority)
{
    sched_tcb_t *t = task_alloc();
    sched_task_ctx_t *ctx;
    if (!t) {
        uart_puts("[SCHED] PANIC: task pool esaurito\n");
        while (1) __asm__ volatile("wfe");
    }

    /* Alloca uno stack kernel dedicato multi-pagina per i path syscall/IPC. */
    uint64_t stack_pa = sched_alloc_kernel_stack();
    uint8_t *stack_top = (uint8_t *)(uintptr_t)(stack_pa + TASK_STACK_SIZE);

    t->name      = name;
    t->priority  = priority;
    t->state     = TCB_STATE_READY;
    t->flags     = TCB_FLAG_KERNEL;
    t->ticks_left = SCHED_TICK_QUANTUM;
    ctx = ctx_of(t);
    ctx->kernel_stack_pa = stack_pa;
    ctx->is_user = 0U;
    if (task_bind_new_process(t, mmu_kernel_space(), 0U,
                              (current_task && pgid_of(current_task) != 0U) ? pgid_of(current_task) : t->pid,
                              (current_task && sid_of(current_task) != 0U) ? sid_of(current_task) : t->pid) < 0) {
        uart_puts("[SCHED] PANIC: proc slot esauriti\n");
        while (1) __asm__ volatile("wfe");
    }
    signal_task_init(t, 0U);

    task_setup_stack(t, stack_top, entry);

    /* Aggiunge alla run queue */
    {
        uint64_t flags = irq_save();
        rq_push(t);
        /* Preemption immediata se il nuovo task ha priorità maggiore */
        if (current_task && eff_prio_of(t) < eff_prio_of(current_task))
            need_resched = 1;
        irq_restore(flags);
    }

    uart_puts("[SCHED] Task creato: '");
    uart_puts(name);
    uart_puts("' pid=");
    pr_dec(t->pid);
    uart_puts(" prio=");
    pr_dec(priority);
    uart_puts(" stack=");
    pr_hex64(stack_pa);
    uart_puts("\n");

    return t;
}

sched_tcb_t *sched_task_create_user(const char *name, mm_space_t *mm,
                                    uintptr_t entry, uintptr_t user_sp,
                                    uintptr_t argc, uintptr_t argv,
                                    uintptr_t envp, uintptr_t auxv,
                                    uint8_t priority)
{
    sched_tcb_t *t = task_alloc();
    sched_task_ctx_t *ctx;
    uint64_t stack_pa;
    uint8_t *stack_top;

    if (!t || !mm) {
        uart_puts("[SCHED] PANIC: impossibile creare task user\n");
        while (1) __asm__ volatile("wfe");
    }

    stack_pa = sched_alloc_kernel_stack();
    stack_top = (uint8_t *)(uintptr_t)(stack_pa + TASK_STACK_SIZE);

    t->name       = name;
    t->priority   = priority;
    t->state      = TCB_STATE_READY;
    t->flags      = TCB_FLAG_USER;
    t->ticks_left = SCHED_TICK_QUANTUM;

    ctx = ctx_of(t);
    ctx->mm              = mm;
    ctx->kernel_stack_pa = stack_pa;
    ctx->user_entry      = entry;
    ctx->user_sp         = user_sp;
    ctx->argc            = argc;
    ctx->argv            = argv;
    ctx->envp            = envp;
    ctx->auxv            = auxv;
    ctx->child_set_tid_uva = 0ULL;
    ctx->clear_child_tid_uva = 0ULL;
    ctx->is_user         = 1U;
    ctx->resume_from_frame = 0U;
    ctx->tpidr_el0       = 0ULL;
    if (task_bind_new_process(t, mm,
                              current_task ? tgid_of(current_task) : 0U,
                              (current_task && pgid_of(current_task) != 0U) ? pgid_of(current_task) : t->pid,
                              (current_task && sid_of(current_task) != 0U) ? sid_of(current_task) : t->pid) < 0) {
        uart_puts("[SCHED] PANIC: proc slot user esauriti\n");
        while (1) __asm__ volatile("wfe");
    }
    signal_task_init(t, (current_task && sched_task_is_user(current_task)) ?
                        tgid_of(current_task) : 0U);

    task_setup_stack(t, stack_top, (sched_fn)0);
    {
        uint64_t flags = irq_save();
        rq_push(t);
        if (current_task && eff_prio_of(t) < eff_prio_of(current_task))
            need_resched = 1;
        irq_restore(flags);
    }

    uart_puts("[SCHED] Task user creato: '");
    uart_puts(name);
    uart_puts("' pid=");
    pr_dec(t->pid);
    uart_puts(" prio=");
    pr_dec(priority);
    uart_puts(" kstack=");
    pr_hex64(stack_pa);
    uart_puts("\n");

    return t;
}

sched_tcb_t *sched_task_fork_user(const char *name, mm_space_t *mm,
                                  const exception_frame_t *frame,
                                  uint8_t priority)
{
    sched_tcb_t *t = task_alloc();
    sched_task_ctx_t *ctx;
    uint64_t stack_pa;
    uint8_t *stack_top;

    if (!t || !mm || !frame) {
        uart_puts("[SCHED] PANIC: impossibile forkare task user\n");
        while (1) __asm__ volatile("wfe");
    }

    stack_pa = sched_alloc_kernel_stack();
    stack_top = (uint8_t *)(uintptr_t)(stack_pa + TASK_STACK_SIZE);

    t->name       = name;
    t->priority   = priority;
    t->state      = TCB_STATE_READY;
    t->flags      = TCB_FLAG_USER;
    t->ticks_left = SCHED_TICK_QUANTUM;

    ctx = ctx_of(t);
    ctx->mm                = mm;
    ctx->kernel_stack_pa   = stack_pa;
    ctx->user_entry        = frame->pc;
    ctx->user_sp           = frame->sp;
    ctx->argc              = 0ULL;
    ctx->argv              = 0ULL;
    ctx->envp              = 0ULL;
    ctx->auxv              = 0ULL;
    ctx->child_set_tid_uva = 0ULL;
    ctx->clear_child_tid_uva = 0ULL;
    ctx->is_user           = 1U;
    ctx->resume_from_frame = 1U;
    /* Il figlio eredita il thread pointer del parent.  musl non re-inizializza
     * TPIDR_EL0 nel figlio di fork() perche' il TLS statico e' gia' valido. */
    ctx->tpidr_el0         = ctx_of(current_task) ? ctx_of(current_task)->tpidr_el0 : 0ULL;
    if (task_bind_new_process(t, mm,
                              current_task ? tgid_of(current_task) : 0U,
                              (current_task && pgid_of(current_task) != 0U) ? pgid_of(current_task) : t->pid,
                              (current_task && sid_of(current_task) != 0U) ? sid_of(current_task) : t->pid) < 0) {
        uart_puts("[SCHED] PANIC: proc slot fork esauriti\n");
        while (1) __asm__ volatile("wfe");
    }
    /* Eredita rlimits dal processo parent (POSIX: fork non azzera i limiti). */
    if (current_task) {
        sched_proc_ctx_t *parent_proc = proc_of(current_task);
        sched_proc_ctx_t *child_proc  = proc_of(t);
        if (parent_proc && child_proc)
            memcpy(child_proc->rlimits, parent_proc->rlimits,
                   sizeof(parent_proc->rlimits));
    }
    ctx->start_frame       = *frame;
    ctx->start_frame.x[0]  = 0ULL;

    task_setup_stack(t, stack_top, (sched_fn)0);
    {
        uint64_t flags = irq_save();
        rq_push(t);
        if (current_task && eff_prio_of(t) < eff_prio_of(current_task))
            need_resched = 1;
        irq_restore(flags);
    }

    uart_puts("[SCHED] Task forkato: '");
    uart_puts(name ? name : "user-fork");
    uart_puts("' pid=");
    pr_dec(t->pid);
    uart_puts(" prio=");
    pr_dec(priority);
    uart_puts(" kstack=");
    pr_hex64(stack_pa);
    uart_puts("\n");

    return t;
}

sched_tcb_t *sched_task_clone_user_thread(const char *name,
                                          const exception_frame_t *frame,
                                          uintptr_t child_sp,
                                          uint64_t child_tpidr,
                                          uintptr_t child_set_tid_uva,
                                          uintptr_t clear_child_tid_uva,
                                          uint8_t priority)
{
    sched_tcb_t       *t = task_alloc();
    sched_task_ctx_t  *ctx;
    uint64_t           stack_pa;
    uint8_t           *stack_top;

    if (!t || !frame || !current_task || !sched_task_is_user(current_task)) {
        uart_puts("[SCHED] PANIC: impossibile clonare thread user\n");
        while (1) __asm__ volatile("wfe");
    }

    stack_pa = sched_alloc_kernel_stack();
    stack_top = (uint8_t *)(uintptr_t)(stack_pa + TASK_STACK_SIZE);

    t->name       = name;
    t->priority   = priority;
    t->state      = TCB_STATE_READY;
    t->flags      = TCB_FLAG_USER | TCB_FLAG_THREAD;
    t->ticks_left = SCHED_TICK_QUANTUM;

    if (task_bind_shared_process(t, current_task) < 0) {
        uart_puts("[SCHED] PANIC: impossibile condividere proc slot\n");
        while (1) __asm__ volatile("wfe");
    }

    ctx = ctx_of(t);
    ctx->kernel_stack_pa   = stack_pa;
    ctx->user_entry        = frame->pc;
    ctx->user_sp           = child_sp ? child_sp : frame->sp;
    ctx->argc              = 0ULL;
    ctx->argv              = 0ULL;
    ctx->envp              = 0ULL;
    ctx->auxv              = 0ULL;
    ctx->child_set_tid_uva = child_set_tid_uva;
    ctx->clear_child_tid_uva = clear_child_tid_uva;
    ctx->is_user           = 1U;
    ctx->resume_from_frame = 1U;
    ctx->tpidr_el0         = child_tpidr;
    ctx->start_frame       = *frame;
    ctx->start_frame.x[0]  = 0ULL;
    ctx->start_frame.sp    = child_sp ? child_sp : frame->sp;

    task_setup_stack(t, stack_top, (sched_fn)0);
    {
        uint64_t flags = irq_save();
        rq_push(t);
        if (current_task && eff_prio_of(t) < eff_prio_of(current_task))
            need_resched = 1;
        irq_restore(flags);
    }

    uart_puts("[SCHED] Thread user clonato: '");
    uart_puts(name ? name : "user-thread");
    uart_puts("' tid=");
    pr_dec(t->pid);
    uart_puts(" tgid=");
    pr_dec(tgid_of(t));
    uart_puts(" kstack=");
    pr_hex64(stack_pa);
    uart_puts("\n");

    return t;
}

static __attribute__((noinline))
void schedule_locked(uint64_t flags, int reenable_irqs_after_switch)
{
    volatile uint64_t saved_flags = flags;
    sched_tcb_t *prev = current_task;
    sched_tcb_t *next = NULL;

    for (;;) {
        int next_prio = bitmap_find_first();

        if (next_prio < 0 || next_prio > 255) {
            sched_restore_irq_flags(saved_flags);
            return;
        }

        next = rq_pop((uint8_t)next_prio);
        if (!next)
            continue;

        /* Scarta entry stale: task non READY o duplicato del corrente. */
        if (next == prev || next->state != TCB_STATE_READY)
            continue;

        break;
    }

    if (prev->state == TCB_STATE_RUNNING) {
        prev->state = TCB_STATE_READY;
        rq_push(prev);
    }

    if (prev->ticks_left == 0)
        prev->ticks_left = SCHED_TICK_QUANTUM;

    next->state = TCB_STATE_RUNNING;
    next->ticks_left = next->ticks_left ? next->ticks_left : SCHED_TICK_QUANTUM;

    current_task = next;
    mmu_activate_space(sched_task_space(next));

    /*
     * Save/restore TPIDR_EL0 (thread pointer) around the context switch.
     * IRQs are already disabled here so this is race-free.
     * Only user tasks have a meaningful TP; kernel tasks always have 0.
     */
    {
        sched_task_ctx_t *prev_ctx = ctx_of(prev);
        if (prev_ctx)
            __asm__ volatile("mrs %0, tpidr_el0"
                             : "=r"(prev_ctx->tpidr_el0) :: "memory");
        sched_context_switch(prev, next);
        /*
         * Quando questa funzione riprende dopo sched_context_switch(), siamo
         * nel task appena tornato in esecuzione, non necessariamente nel
         * 'next' scelto da questa invocazione.  Ripristina quindi il TP del
         * task corrente reale.
         */
        if (current_task && ctx_of(current_task))
            __asm__ volatile("msr tpidr_el0, %0"
                             :: "r"(ctx_of(current_task)->tpidr_el0) : "memory");
    }
    if (reenable_irqs_after_switch) {
        /*
         * Ripristina il DAIF originale del caller kernel-side.
         * Non possiamo fidarci dei registri caller-saved dopo il
         * context switch: quando questo task riprende piu' tardi,
         * x0-x18 possono contenere valori arbitrari del task corrente.
         */
        sched_restore_irq_flags(saved_flags);
    } else {
        /*
         * schedule_from_exception() deve tornare in vectors.S con
         * gli IRQ ancora mascherati.  Saranno riabilitati solo dall'ERET
         * tramite SPSR_EL1 del task interrotto, evitando IRQ annidati
         * mentre il frame eccezione e' ancora sullo stack kernel.
         */
        sched_return_irqs_masked();
    }
}

/* ══════════════════════════════════════════════════════════════════
 * schedule — cuore dello scheduler FPP
 *
 * Seleziona il task READY a priorità massima ed esegue il switch.
 * WCET: O(1) — 4 CLZ + lookup + sched_context_switch (16 istruzioni)
 * ══════════════════════════════════════════════════════════════════ */
void schedule(void)
{
    uint64_t flags = irq_save();
    schedule_locked(flags, 1);
}

void schedule_from_exception(void)
{
    uint64_t flags = irq_save();
    schedule_locked(flags, 0);
}

/* ══════════════════════════════════════════════════════════════════
 * sched_yield — cede volontariamente la CPU
 * ══════════════════════════════════════════════════════════════════ */
void sched_yield(void)
{
    uint64_t flags = irq_save();
    if (current_task)
        current_task->ticks_left = 0;   /* forza re-insert in coda */
    schedule_locked(flags, 1);
}

/* ══════════════════════════════════════════════════════════════════
 * sched_block — blocca il task corrente
 * ══════════════════════════════════════════════════════════════════ */
void sched_block(void)
{
    uint64_t flags = irq_save();
    if (current_task)
        current_task->state = TCB_STATE_BLOCKED;
    schedule_locked(flags, 1);
}

/* ══════════════════════════════════════════════════════════════════
 * sched_unblock — sblocca un task
 * ══════════════════════════════════════════════════════════════════ */
void sched_unblock(sched_tcb_t *t)
{
    if (!t) return;
    uint64_t flags = irq_save();
    if (t->state == TCB_STATE_BLOCKED) {
        t->state = TCB_STATE_READY;
        t->ticks_left = SCHED_TICK_QUANTUM;
        rq_push(t);
        if (current_task && eff_prio_of(t) < eff_prio_of(current_task))
            need_resched = 1;
    }
    irq_restore(flags);
}

/* ══════════════════════════════════════════════════════════════════
 * sched_tick — IRQ handler del timer, chiamato ogni 1ms
 *
 * WCET: O(1) — no alloc, no lock, no uart.
 * ══════════════════════════════════════════════════════════════════ */
void sched_tick(uint64_t jiffies)
{
    if (!current_task) return;

    /* Aggiorna tempo CPU consumato */
    current_task->runtime_ns += TICK_NS;

    /* Decrementa il quantum */
    if (current_task->ticks_left > 0)
        current_task->ticks_left--;

    /* Quantum esaurito: richiedi reschedule */
    if (current_task->ticks_left == 0)
        need_resched = 1;

    /* Overrun detection: deadline superata */
    if (current_task->deadline_ms > 0 &&
        jiffies > current_task->deadline_ms &&
        !(current_task->flags & TCB_FLAG_IDLE)) {
        /* Non stampiamo nell'IRQ handler: troppo lento.
         * Settiamo solo un flag — il logging avviene in sched_stats(). */
        need_resched = 1;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * sched_stats
 * ══════════════════════════════════════════════════════════════════ */

static const char *state_str(uint8_t s)
{
    switch (s) {
    case TCB_STATE_RUNNING: return "RUNNING";
    case TCB_STATE_READY:   return "READY  ";
    case TCB_STATE_BLOCKED: return "BLOCKED";
    case TCB_STATE_ZOMBIE:  return "ZOMBIE ";
    default:                 return "???????";
    }
}

int sched_task_is_user(const sched_tcb_t *t)
{
    sched_task_ctx_t *ctx = ctx_of(t);
    return (ctx && ctx->is_user) ? 1 : 0;
}

uint8_t sched_task_effective_priority(const sched_tcb_t *t)
{
    return eff_prio_of(t);
}

int sched_task_donate_priority(sched_tcb_t *t, uint8_t donated_prio)
{
    uint64_t flags;
    uint8_t  old_eff;
    uint8_t  new_eff;
    uint32_t slot;

    if (!t) return -1;

    flags = irq_save();
    slot = task_slot(t);
    old_eff = eff_prio_of(t);

    if (donated_priority[slot] == 0xFFU || donated_prio < donated_priority[slot])
        donated_priority[slot] = donated_prio;

    new_eff = eff_prio_of(t);
    if (new_eff != old_eff && t->state == TCB_STATE_READY) {
        (void)rq_remove(t, old_eff);
        rq_push(t);
    }
    if (current_task && t != current_task && new_eff < eff_prio_of(current_task))
        need_resched = 1;

    irq_restore(flags);
    return 0;
}

void sched_task_clear_donation(sched_tcb_t *t)
{
    uint64_t flags;
    uint8_t  old_eff;
    uint8_t  new_eff;
    int      highest;

    if (!t) return;

    flags = irq_save();
    old_eff = eff_prio_of(t);
    donated_priority[task_slot(t)] = 0xFFU;
    new_eff = eff_prio_of(t);

    if (new_eff != old_eff && t->state == TCB_STATE_READY) {
        (void)rq_remove(t, old_eff);
        rq_push(t);
    }

    if (t == current_task) {
        highest = bitmap_find_first();
        if (highest >= 0 && highest < (int)new_eff)
            need_resched = 1;
    } else if (current_task && new_eff < eff_prio_of(current_task)) {
        need_resched = 1;
    }

    irq_restore(flags);
}

uint32_t sched_task_snapshot(sched_task_info_t *out, uint32_t max_entries)
{
    uint32_t count = 0U;

    if (!out || max_entries == 0U)
        return 0U;

    for (uint32_t i = 0U; i < task_count && count < max_entries; i++) {
        sched_tcb_t      *t = &task_pool[i];
        sched_task_info_t *dst = &out[count++];

        dst->pid         = t->pid;
        dst->priority    = eff_prio_of(t);
        dst->state       = t->state;
        dst->flags       = t->flags;
        dst->_reserved0  = 0U;
        dst->runtime_ns  = t->runtime_ns;
        dst->budget_ns   = t->budget_ns;
        dst->period_ms   = t->period_ms;
        dst->deadline_ms = t->deadline_ms;
        sched_strlcpy(dst->name, t->name ? t->name : "(unnamed)",
                      (uint32_t)sizeof(dst->name));
    }

    return count;
}

mm_space_t *sched_task_space(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);
    if (!proc || !proc->mm)
        return mmu_kernel_space();
    return proc->mm;
}

int sched_task_rebind_user(sched_tcb_t *t, mm_space_t *mm,
                           uintptr_t entry, uintptr_t user_sp,
                           uintptr_t argc, uintptr_t argv,
                           uintptr_t envp, uintptr_t auxv)
{
    sched_task_ctx_t  *ctx = ctx_of(t);
    sched_proc_ctx_t  *proc = proc_of(t);

    if (!ctx || !proc || !mm)
        return -1;

    t->flags &= (uint8_t)~TCB_FLAG_KERNEL;
    t->flags |= TCB_FLAG_USER;
    ctx->mm         = mm;
    proc->mm        = mm;
    ctx->user_entry = entry;
    ctx->user_sp    = user_sp;
    ctx->argc       = argc;
    ctx->argv       = argv;
    ctx->envp       = envp;
    ctx->auxv       = auxv;
    ctx->is_user    = 1U;
    return 0;
}

int sched_task_get_exit_code(const sched_tcb_t *t, int32_t *out)
{
    sched_task_ctx_t *ctx = ctx_of(t);

    if (!ctx || !out)
        return -1;

    *out = ctx->exit_code;
    return 0;
}

uint32_t sched_task_parent_pid(const sched_tcb_t *t)
{
    return parent_pid_of(t);
}

uint32_t sched_task_tgid(const sched_tcb_t *t)
{
    return tgid_of(t);
}

uint32_t sched_task_pgid(const sched_tcb_t *t)
{
    return pgid_of(t);
}

uint32_t sched_task_sid(const sched_tcb_t *t)
{
    return sid_of(t);
}

uint32_t sched_task_proc_slot(const sched_tcb_t *t)
{
    sched_task_ctx_t *ctx = ctx_of(t);
    return ctx ? (uint32_t)ctx->proc_slot : 0U;
}

uint32_t sched_task_proc_refcount(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);
    return proc ? (uint32_t)proc->refcount : 0U;
}

uint32_t sched_task_abi_mode(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);
    return proc ? (uint32_t)proc->abi_mode : (uint32_t)SCHED_ABI_ENLILOS;
}

int sched_task_set_abi_mode(sched_tcb_t *t, uint32_t abi_mode)
{
    sched_proc_ctx_t *proc = proc_of(t);

    if (!proc)
        return -1;
    if (abi_mode != SCHED_ABI_ENLILOS && abi_mode != SCHED_ABI_LINUX)
        return -1;

    proc->abi_mode = (uint8_t)abi_mode;
    return 0;
}

const char *sched_task_exec_path(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);

    if (!proc || proc->exec_path[0] == '\0')
        return NULL;
    return proc->exec_path;
}

int sched_task_set_exec_path(sched_tcb_t *t, const char *path)
{
    sched_proc_ctx_t *proc = proc_of(t);

    if (!proc)
        return -1;
    sched_strlcpy(proc->exec_path, path ? path : "", sizeof(proc->exec_path));
    return 0;
}

int sched_task_is_thread(const sched_tcb_t *t)
{
    return (t && (t->flags & TCB_FLAG_THREAD) != 0U) ? 1 : 0;
}

int sched_task_is_process_waitable(const sched_tcb_t *t)
{
    sched_proc_ctx_t *proc = proc_of(t);

    if (!t || !proc)
        return 0;
    if (t->pid != proc->tgid)
        return 0;
    return proc->waitable ? 1 : 0;
}

int sched_task_has_session(uint32_t sid)
{
    if (sid == 0U)
        return 0;

    for (uint32_t i = 0U; i < task_count; i++) {
        sched_tcb_t *t = &task_pool[i];

        if (t->state == TCB_STATE_ZOMBIE)
            continue;
        if (sid_of(t) == sid)
            return 1;
    }

    return 0;
}

int sched_task_has_pgrp(uint32_t sid, uint32_t pgid)
{
    if (pgid == 0U || sid == 0U)
        return 0;

    for (uint32_t i = 0U; i < task_count; i++) {
        sched_tcb_t *t = &task_pool[i];

        if (t->state == TCB_STATE_ZOMBIE)
            continue;
        if (sid_of(t) == sid && pgid_of(t) == pgid)
            return 1;
    }

    return 0;
}

int sched_task_setpgid(const sched_tcb_t *caller, sched_tcb_t *target, uint32_t pgid)
{
    sched_proc_ctx_t       *target_proc;
    sched_proc_ctx_t       *caller_proc;

    if (!caller || !target)
        return -EINVAL;
    if (!sched_task_is_user(caller) || !sched_task_is_user(target))
        return -EPERM;

    caller_proc = proc_of(caller);
    target_proc = proc_of(target);
    if (!caller_proc || !target_proc)
        return -EINVAL;
    if (target != caller && target_proc->parent_pid != caller_proc->tgid)
        return -EPERM;
    if (target->pid != target_proc->tgid)
        return -EPERM;
    if (target_proc->sid == target_proc->tgid)
        return -EPERM;

    if (pgid == 0U)
        pgid = target_proc->tgid;

    if (target_proc->sid != caller_proc->sid)
        return -EPERM;
    if (pgid != target_proc->tgid && !sched_task_has_pgrp(target_proc->sid, pgid))
        return -EPERM;

    target_proc->pgid = pgid;
    return 0;
}

int sched_task_setsid(sched_tcb_t *task, uint32_t *out_sid)
{
    sched_proc_ctx_t *proc = proc_of(task);

    if (!task || !proc)
        return -EINVAL;
    if (!sched_task_is_user(task))
        return -EPERM;
    if (proc->pgid == proc->tgid)
        return -EPERM;

    proc->sid = proc->tgid;
    proc->pgid = proc->tgid;
    if (out_sid)
        *out_sid = proc->sid;
    return (int)proc->sid;
}

sched_tcb_t *sched_task_at(uint32_t index)
{
    if (index >= task_count)
        return NULL;
    return &task_pool[index];
}

uint32_t sched_task_count_total(void)
{
    return task_count;
}

void sched_task_set_tpidr(sched_tcb_t *t, uint64_t tpidr)
{
    sched_task_ctx_t *ctx = ctx_of(t);
    if (ctx)
        ctx->tpidr_el0 = tpidr;
}

int sched_task_set_clear_tid(sched_tcb_t *t, uintptr_t clear_tid_uva)
{
    sched_task_ctx_t *ctx = ctx_of(t);

    if (!ctx || !ctx->is_user)
        return -1;

    ctx->clear_child_tid_uva = clear_tid_uva;
    return 0;
}

uint64_t sched_task_get_tpidr(const sched_tcb_t *t)
{
    const sched_task_ctx_t *ctx = ctx_of(t);
    return ctx ? ctx->tpidr_el0 : 0ULL;
}

int sched_task_begin_exit_group(int32_t code)
{
    sched_task_ctx_t *cur_ctx;
    uint8_t           proc_slot;

    if (!current_task)
        return -1;

    cur_ctx = ctx_of(current_task);
    if (!cur_ctx)
        return -1;

    proc_slot = cur_ctx->proc_slot;
    proc_set_group_exit(proc_slot, code);

    for (uint32_t i = 0U; i < task_count; i++) {
        sched_tcb_t      *task = &task_pool[i];
        sched_task_ctx_t *ctx  = &task_ctx[i];

        if (task == current_task || task->state == TCB_STATE_ZOMBIE)
            continue;
        if (ctx->proc_slot != proc_slot)
            continue;

        sched_task_finish_exit(task, code);
    }

    return 0;
}

static void sched_task_store_tid_uva(sched_task_ctx_t *ctx,
                                     uint64_t tid_uva, uint32_t value)
{
    void *dst;

    if (!ctx || !ctx->mm || tid_uva == 0ULL)
        return;
    if (tid_uva < MMU_USER_BASE || tid_uva + sizeof(uint32_t) > MMU_USER_LIMIT)
        return;
    if (mmu_space_prepare_write(ctx->mm, (uintptr_t)tid_uva, sizeof(uint32_t)) < 0)
        return;
    dst = mmu_space_resolve_ptr(ctx->mm, (uintptr_t)tid_uva, sizeof(uint32_t));
    if (!dst)
        return;
    memcpy(dst, &value, sizeof(value));
}

static void sched_task_store_set_tid(sched_task_ctx_t *ctx, uint32_t value)
{
    sched_task_store_tid_uva(ctx, ctx ? ctx->child_set_tid_uva : 0ULL, value);
}

static void sched_task_store_clear_tid(sched_task_ctx_t *ctx, uint32_t value)
{
    sched_task_store_tid_uva(ctx, ctx ? ctx->clear_child_tid_uva : 0ULL, value);
}

static void sched_task_finish_exit(sched_tcb_t *task, int32_t code)
{
    sched_task_ctx_t *ctx;
    uint32_t          proc_slot = 0U;
    uint32_t          parent_pid = 0U;
    int               last_thread = 0;
    uint64_t          flags;

    if (!task)
        return;

    ctx = ctx_of(task);
    if (ctx)
        ctx->exit_code = code;
    if (ctx)
        proc_slot = ctx->proc_slot;

    flags = irq_save();
    if (task != current_task && task->state == TCB_STATE_READY)
        (void)rq_remove(task, eff_prio_of(task));
    irq_restore(flags);

    futex_task_cleanup(task);

    if (ctx && ctx->clear_child_tid_uva != 0ULL) {
        sched_task_store_clear_tid(ctx, 0U);
        (void)futex_wake_task_uaddr(task, (uintptr_t)ctx->clear_child_tid_uva, 1U);
    }

    kmon_task_cleanup(task);
    ksem_task_cleanup(task);
    mreact_task_cleanup(task);
    signal_task_exit(task);

    if (ctx)
        last_thread = proc_release(ctx->proc_slot, code, &parent_pid);
    if (last_thread) {
        vmm_cleanup_task(proc_slot);
        elf64_dlreset_proc(proc_slot);
        syscall_task_cleanup(task);
        if (parent_pid != 0U)
            (void)signal_send_pid(parent_pid, SIGCHLD);
    }

    task->state = TCB_STATE_ZOMBIE;
}

void sched_task_bootstrap(uint64_t entry_reg)
{
    sched_task_ctx_t *ctx = ctx_of(current_task);

    if (ctx && ctx->resume_from_frame) {
        /*
         * Il fork child arriva qui alla sua prima schedulazione con gli IRQ
         * ancora mascherati dal context switch.  Manteniamoli disabilitati
         * fino alla sched_resume_user_frame(): la sequenza MSR→ERET deve essere
         * atomica, altrimenti un timer IRQ puo' preemptare il bootstrap e farci
         * riprendere il task con un frame EL1 incompleto (PC=0, x30=0).
         */
        __asm__ volatile("msr daifset, #2" ::: "memory");
        ctx->resume_from_frame = 0U;
        if (ctx->child_set_tid_uva != 0ULL)
            sched_task_store_set_tid(ctx, current_task->pid);
        __asm__ volatile("msr tpidr_el0, %0"
                         :: "r"(ctx->tpidr_el0) : "memory");
        sched_resume_user_frame(&ctx->start_frame);
        while (1) __asm__ volatile("wfe");
    }

    if (ctx && ctx->is_user) {
        __asm__ volatile("msr tpidr_el0, %0"
                         :: "r"(ctx->tpidr_el0) : "memory");
        sched_enter_user(ctx->argc, ctx->argv, ctx->envp, ctx->auxv,
                         ctx->user_sp, ctx->user_entry);
        while (1) __asm__ volatile("wfe");
    }

    if (entry_reg != 0ULL) {
        /* I task kernel possono riprendere con IRQ attivi solo qui. */
        __asm__ volatile("msr daifclr, #2" ::: "memory");
        ((sched_fn)(uintptr_t)entry_reg)();
    }

    sched_task_exit();
}

void sched_task_exit_with_code(int32_t code)
{
    if (current_task)
        sched_task_finish_exit(current_task, code);

    schedule();
    while (1) __asm__ volatile("wfe");
}

void sched_task_exit(void)
{
    sched_task_exit_with_code(0);
}

/* ══════════════════════════════════════════════════════════════════
 * sched_task_find — cerca un task per PID nel pool statico
 * ══════════════════════════════════════════════════════════════════ */
sched_tcb_t *sched_task_find(uint32_t pid)
{
    for (uint32_t i = 0; i < task_count; i++) {
        if (task_pool[i].pid == pid)
            return &task_pool[i];
    }
    return NULL;
}

void sched_stats(void)
{
    uart_puts("[SCHED] ── Task attivi ────────────────────────────────\n");
    uart_puts("[SCHED]  PID  PRI  STATE    TICKS   RUNTIME(ms)  NAME\n");
    uart_puts("[SCHED]  ───  ───  ───────  ──────  ───────────  ────────────\n");
    for (uint32_t i = 0; i < task_count; i++) {
        sched_tcb_t *t = &task_pool[i];
        uart_puts("[SCHED]  ");
        /* pid */
        uart_putc('0' + (int)(t->pid / 10));
        uart_putc('0' + (int)(t->pid % 10));
        uart_puts("   ");
        /* priority */
        if (t->priority < 100) uart_putc(' ');
        if (t->priority < 10)  uart_putc(' ');
        pr_dec(t->priority);
        uart_puts("  ");
        uart_puts(state_str(t->state));
        uart_puts("  ");
        /* ticks_left */
        uart_putc(' ');
        if (t->ticks_left < 10) uart_putc(' ');
        pr_dec(t->ticks_left);
        uart_puts("      ");
        /* runtime in ms */
        pr_dec(t->runtime_ns / 1000000ULL);
        uart_puts("          ");
        uart_puts(t->name);
        uart_puts("\n");
    }
    uart_puts("[SCHED] ─────────────────────────────────────────────\n");
}

/* ── Resource limits API ──────────────────────────────────────────── */

int sched_proc_get_rlimit(const sched_tcb_t *t, uint32_t resource,
                          rlimit64_t *out)
{
    const sched_proc_ctx_t *proc;

    if (!t || !out || resource >= RLIMIT_NLIMITS)
        return -1;
    proc = proc_of(t);
    if (!proc)
        return -1;
    *out = proc->rlimits[resource];
    return 0;
}

int sched_proc_set_rlimit(sched_tcb_t *t, uint32_t resource,
                          const rlimit64_t *in)
{
    sched_proc_ctx_t *proc;

    if (!t || !in || resource >= RLIMIT_NLIMITS)
        return -1;
    proc = proc_of(t);
    if (!proc)
        return -1;
    proc->rlimits[resource] = *in;
    return 0;
}
