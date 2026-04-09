/*
 * EnlilOS Microkernel - Fixed-Priority Preemptive Scheduler (M2-03)
 *
 * Implementazione FPP con:
 *   - ready_bitmap[4] (256 bit): trova la priorità massima in O(1)
 *   - run_queue[256]:  FIFO intrusive singly-linked per priorità
 *   - task_pool[64]:   pool statico di sched_tcb_t (no kmalloc nel scheduler)
 *   - sched_context_switch: assembly in sched_switch.S
 *   - Preemption: need_resched settato da sched_tick(), letto da vectors.S
 */

#include "sched.h"
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
    uint8_t     is_user;
    uint8_t     resume_from_frame;
    uint8_t     _pad[6];
    int32_t     exit_code;
    uint32_t    parent_pid;
    uint32_t    pgid;
    uint32_t    sid;
    exception_frame_t start_frame;
} sched_task_ctx_t;

static sched_task_ctx_t task_ctx[SCHED_MAX_TASKS];

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

static inline uint32_t pgid_of(const sched_tcb_t *t)
{
    sched_task_ctx_t *ctx = ctx_of(t);
    return ctx ? ctx->pgid : 0U;
}

static inline uint32_t sid_of(const sched_tcb_t *t)
{
    sched_task_ctx_t *ctx = ctx_of(t);
    return ctx ? ctx->sid : 0U;
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
    task_ctx[slot].is_user         = 0U;
    task_ctx[slot].resume_from_frame = 0U;
    task_ctx[slot].exit_code       = 0;
    task_ctx[slot].parent_pid      = 0U;
    task_ctx[slot].pgid            = t->pid;
    task_ctx[slot].sid             = t->pid;
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
    ctx->mm = mmu_kernel_space();
    ctx->kernel_stack_pa = stack_pa;
    ctx->is_user = 0U;
    ctx->parent_pid = current_task ? current_task->pid : 0U;
    ctx->pgid = (current_task && pgid_of(current_task) != 0U) ? pgid_of(current_task) : t->pid;
    ctx->sid = (current_task && sid_of(current_task) != 0U) ? sid_of(current_task) : t->pid;
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
    ctx->is_user         = 1U;
    ctx->resume_from_frame = 0U;
    ctx->tpidr_el0       = 0ULL;
    ctx->parent_pid      = current_task ? current_task->pid : 0U;
    ctx->pgid            = (current_task && pgid_of(current_task) != 0U) ? pgid_of(current_task) : t->pid;
    ctx->sid             = (current_task && sid_of(current_task) != 0U) ? sid_of(current_task) : t->pid;
    signal_task_init(t, (current_task && sched_task_is_user(current_task)) ?
                        current_task->pid : 0U);

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
    ctx->is_user           = 1U;
    ctx->resume_from_frame = 1U;
    /* Il figlio eredita il thread pointer del parent.  musl non re-inizializza
     * TPIDR_EL0 nel figlio di fork() perche' il TLS statico e' gia' valido. */
    ctx->tpidr_el0         = ctx_of(current_task) ? ctx_of(current_task)->tpidr_el0 : 0ULL;
    ctx->parent_pid        = current_task ? current_task->pid : 0U;
    ctx->pgid              = (current_task && pgid_of(current_task) != 0U) ? pgid_of(current_task) : t->pid;
    ctx->sid               = (current_task && sid_of(current_task) != 0U) ? sid_of(current_task) : t->pid;
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
    sched_task_ctx_t *ctx = ctx_of(t);
    if (!ctx || !ctx->mm)
        return mmu_kernel_space();
    return ctx->mm;
}

int sched_task_rebind_user(sched_tcb_t *t, mm_space_t *mm,
                           uintptr_t entry, uintptr_t user_sp,
                           uintptr_t argc, uintptr_t argv,
                           uintptr_t envp, uintptr_t auxv)
{
    sched_task_ctx_t *ctx = ctx_of(t);

    if (!ctx || !mm)
        return -1;

    t->flags &= (uint8_t)~TCB_FLAG_KERNEL;
    t->flags |= TCB_FLAG_USER;
    ctx->mm         = mm;
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
    sched_task_ctx_t *ctx = ctx_of(t);
    return ctx ? ctx->parent_pid : 0U;
}

uint32_t sched_task_pgid(const sched_tcb_t *t)
{
    return pgid_of(t);
}

uint32_t sched_task_sid(const sched_tcb_t *t)
{
    return sid_of(t);
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
    sched_task_ctx_t       *target_ctx;
    const sched_task_ctx_t *caller_ctx;

    if (!caller || !target)
        return -EINVAL;
    if (!sched_task_is_user(caller) || !sched_task_is_user(target))
        return -EPERM;

    caller_ctx = ctx_of(caller);
    target_ctx = ctx_of(target);
    if (!caller_ctx || !target_ctx)
        return -EINVAL;
    if (target != caller && target_ctx->parent_pid != caller->pid)
        return -EPERM;
    if (target_ctx->sid == target->pid)
        return -EPERM;

    if (pgid == 0U)
        pgid = target->pid;

    if (target_ctx->sid != caller_ctx->sid)
        return -EPERM;
    if (pgid != target->pid && !sched_task_has_pgrp(target_ctx->sid, pgid))
        return -EPERM;

    target_ctx->pgid = pgid;
    return 0;
}

int sched_task_setsid(sched_tcb_t *task, uint32_t *out_sid)
{
    sched_task_ctx_t *ctx = ctx_of(task);

    if (!task || !ctx)
        return -EINVAL;
    if (!sched_task_is_user(task))
        return -EPERM;
    if (ctx->pgid == task->pid)
        return -EPERM;

    ctx->sid = task->pid;
    ctx->pgid = task->pid;
    if (out_sid)
        *out_sid = ctx->sid;
    return (int)ctx->sid;
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

uint64_t sched_task_get_tpidr(const sched_tcb_t *t)
{
    const sched_task_ctx_t *ctx = ctx_of(t);
    return ctx ? ctx->tpidr_el0 : 0ULL;
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
    sched_task_ctx_t *ctx = ctx_of(current_task);

    if (current_task) {
        if (ctx)
            ctx->exit_code = code;
        kmon_task_cleanup(current_task);
        ksem_task_cleanup(current_task);
        mreact_task_cleanup(current_task);
        vmm_cleanup_task(current_task->pid);
        syscall_task_cleanup(current_task);
        signal_task_exit(current_task);
        current_task->state = TCB_STATE_ZOMBIE;
    }

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
