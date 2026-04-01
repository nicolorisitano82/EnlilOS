/*
 * EnlilOS Microkernel - Fixed-Priority Preemptive Scheduler (M2-03)
 *
 * Policy: Fixed-Priority Preemptive (FPP)
 *   - 256 livelli di priorità (0 = massima, 255 = minima/idle)
 *   - Preemption immediata: se task READY con priorità > corrente → switch
 *   - Time quantum: SCHED_TICK_QUANTUM tick (ms) per priorità uguale
 *   - Ready bitmap 256 bit (4 × uint64_t): find_first in O(1) via CLZ
 *   - Per-priority FIFO run queue (singly-linked intrusive)
 *
 * Struttura sched_tcb_t — esattamente 64 byte (entra in task_cache):
 *
 *   offset  0: sp            — SP salvato (DEVE essere a offset 0, vedi sched_switch.S)
 *   offset  8: pid           — ID processo
 *   offset 12: priority      — 0=massima, 255=idle
 *   offset 13: state         — TCB_STATE_*
 *   offset 14: flags         — TCB_FLAG_*
 *   offset 15: ticks_left    — quantum rimanente in tick
 *   offset 16: runtime_ns    — CPU time totale consumato
 *   offset 24: budget_ns     — budget rimanente nel periodo (0=illimitato)
 *   offset 32: period_ms     — periodo in ms (0=aperiodico)
 *   offset 40: deadline_ms   — deadline assoluta in jiffies (0=nessuna)
 *   offset 48: name          — puntatore stringa di debug
 *   offset 56: next          — link intrusive nella run queue
 *
 * Context switch (sched_switch.S):
 *   sched_context_switch(prev, next):
 *     - Salva x19-x28, x29, x30 di prev sullo stack → prev->sp
 *     - Carica next->sp → ripristina x19-x28, x29, x30 di next
 *     - ret → salta a x30 del next task
 *
 * Prima schedulazione di un task:
 *   - Stack frame iniziale: x19=entry_fn, x20-x28=0, x29=0, x30=trampoline
 *   - task_entry_trampoline abilita IRQ + chiama x19(entry_fn)
 *
 * Preemption hardware:
 *   In __exc_common (vectors.S), DOPO exception_handler():
 *     if (need_resched) { need_resched=0; schedule(); }
 *   Questo garantisce preemption al ritorno da ogni IRQ (ogni 1ms).
 */

#ifndef ENLILOS_SCHED_H
#define ENLILOS_SCHED_H

#include "types.h"

/* ── Costanti ───────────────────────────────────────────────────── */

#define SCHED_MAX_TASKS     32          /* pool statico di TCB           */
#define SCHED_TICK_QUANTUM  10          /* ms di time slice per task     */
#define TASK_STACK_SIZE     4096        /* 1 pagina per stack kernel task */

/* ── Stati del TCB ──────────────────────────────────────────────── */
#define TCB_STATE_RUNNING   0
#define TCB_STATE_READY     1
#define TCB_STATE_BLOCKED   2
#define TCB_STATE_ZOMBIE    3

/* ── Flag del TCB ───────────────────────────────────────────────── */
#define TCB_FLAG_KERNEL     (1 << 0)    /* task kernel (non user-space)  */
#define TCB_FLAG_IDLE       (1 << 1)    /* task idle                     */
#define TCB_FLAG_RT         (1 << 2)    /* hard real-time                */
#define TCB_FLAG_USER       (1 << 3)    /* task user-space (EL0)         */

/* ── Priorità predefinite ───────────────────────────────────────── */
#define PRIO_MAX            0           /* massima priorità              */
#define PRIO_KERNEL         0
#define PRIO_HIGH           32
#define PRIO_NORMAL         128
#define PRIO_LOW            200
#define PRIO_IDLE           255         /* task idle — sempre READY      */

/* ── Tipo entry function ────────────────────────────────────────── */
typedef void (*sched_fn)(void);
typedef struct mm_space mm_space_t;

/* ════════════════════════════════════════════════════════════════════
 * sched_tcb_t — Task Control Block, 64 byte esatti (fit in task_cache)
 *
 * IMPORTANTE: sp DEVE essere al byte 0 — sched_switch.S ne dipende.
 * ════════════════════════════════════════════════════════════════════ */
typedef struct sched_tcb {
    uint64_t    sp;           //  0: saved kernel SP
    uint32_t    pid;          //  8: process ID
    uint8_t     priority;     // 12: 0=max, 255=idle
    uint8_t     state;        // 13: TCB_STATE_*
    uint8_t     flags;        // 14: TCB_FLAG_*
    uint8_t     ticks_left;   // 15: quantum rimanente (tick)
    uint64_t    runtime_ns;   // 16: CPU time totale
    uint64_t    budget_ns;    // 24: budget rimanente (0=unlimited)
    uint64_t    period_ms;    // 32: periodo (0=aperiodico)
    uint64_t    deadline_ms;  // 40: deadline assoluta in jiffies
    const char *name;         // 48: nome debug
    struct sched_tcb *next;   // 56: link in run queue
} sched_tcb_t;                /* TOTALE: 64 byte */

/* ── Variabili globali esportate ────────────────────────────────── */

/* Task corrente in esecuzione */
extern sched_tcb_t *current_task;

/*
 * need_resched — flag di richiesta reschedule.
 * Scritto da sched_tick() nell'IRQ handler del timer.
 * Letto da __exc_common in vectors.S dopo ogni exception_handler().
 * volatile: il compilatore NON può ottimizzarlo via caching in registro.
 */
extern volatile uint32_t need_resched;

/* ════════════════════════════════════════════════════════════════════
 * API
 * ════════════════════════════════════════════════════════════════════ */

/*
 * sched_init() — inizializza lo scheduler.
 * Chiamare dopo timer_init().
 */
void sched_init(void);

/*
 * sched_task_create(name, entry, priority) — crea un nuovo kernel task.
 * WCET: O(1) — NON chiamare in sezioni RT.
 */
sched_tcb_t *sched_task_create(const char *name, sched_fn entry,
                                uint8_t priority);

/*
 * Crea un task user-space. Il codice entra a EL0 con stack/argv/envp/auxv gia'
 * preparati dal loader ELF. Il kernel stack resta separato e serve per IRQ e
 * syscall provenienti da EL0.
 */
sched_tcb_t *sched_task_create_user(const char *name, mm_space_t *mm,
                                    uintptr_t entry, uintptr_t user_sp,
                                    uintptr_t argc, uintptr_t argv,
                                    uintptr_t envp, uintptr_t auxv,
                                    uint8_t priority);

/*
 * schedule() — seleziona ed esegue il task con priorità massima. O(1).
 */
void schedule(void);

/*
 * sched_yield() — cede volontariamente la CPU. O(1).
 */
void sched_yield(void);

/*
 * sched_block() — blocca il task corrente fino a sched_unblock().
 */
void sched_block(void);

/*
 * sched_unblock(t) — sblocca il task 't', aggiunge alla run queue.
 */
void sched_unblock(sched_tcb_t *t);

/*
 * sched_tick(jiffies) — chiamato dall'IRQ timer ogni 1ms. WCET: O(1).
 */
void sched_tick(uint64_t jiffies);

/*
 * sched_stats() — stampa stato di tutti i task su UART.
 */
void sched_stats(void);

/*
 * sched_task_find(pid) — cerca un task per PID nel pool statico. O(N).
 * Ritorna NULL se nessun task attivo ha quel PID.
 */
sched_tcb_t *sched_task_find(uint32_t pid);

int         sched_task_is_user(const sched_tcb_t *t);
mm_space_t *sched_task_space(const sched_tcb_t *t);
int         sched_task_rebind_user(sched_tcb_t *t, mm_space_t *mm,
                                   uintptr_t entry, uintptr_t user_sp,
                                   uintptr_t argc, uintptr_t argv,
                                   uintptr_t envp, uintptr_t auxv);

/* Helper del trampoline assembly */
void sched_task_bootstrap(uint64_t entry_reg);
void sched_enter_user(uint64_t argc, uint64_t argv,
                      uint64_t envp, uint64_t auxv,
                      uint64_t user_sp, uint64_t entry);

/* Trampoline assembly — non chiamare direttamente */
extern void task_entry_trampoline(void);

/* Context switch assembly */
void sched_context_switch(sched_tcb_t *prev, sched_tcb_t *next);

#endif /* ENLILOS_SCHED_H */
