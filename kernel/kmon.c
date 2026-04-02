/*
 * EnlilOS - Kernel monitor (M8-07)
 *
 * Implementazione v1:
 *   - pool statico di monitor e waiter
 *   - mutex implicito con PI/ceiling best-effort sul donation slot scheduler
 *   - condition variable Mesa-style con wait queue kernel-side
 *   - timedwait bounded cooperativo via timer_now_ms() + sched_yield()
 */

#include "kmon.h"

#include "kdebug.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer.h"
#include "uart.h"

extern void *memset(void *dst, int value, size_t n);

#define KMON_MAX         64U
#define KMON_MAX_WAITERS 96U

#define KMON_Q_NONE   0U
#define KMON_Q_ENTER  1U
#define KMON_Q_COND   2U
#define KMON_Q_URGENT 3U

#define KMON_WAKE_NONE 0U
#define KMON_WAKE_LOCK 1U

typedef struct kmon_waiter kmon_waiter_t;

typedef struct {
    kmon_waiter_t *head;
    kmon_waiter_t *tail;
    uint32_t       n_waiters;
} kmon_queue_t;

typedef struct {
    kmon_queue_t q;
} kmon_cond_t;

typedef struct {
    uint8_t     active;
    uint8_t     _pad0;
    uint16_t    _pad1;
    uint32_t    handle;
    sched_tcb_t *owner;
    uint32_t    prio_ceiling;
    uint32_t    entry_count;
    uint32_t    flags;
    kmon_queue_t enter_q;
    kmon_queue_t urgent_q;
    kmon_cond_t cond[KMON_MAX_COND];
} kmon_entry_t;

struct kmon_waiter {
    uint8_t        active;
    uint8_t        queue_kind;
    uint8_t        cond_idx;
    uint8_t        wake_reason;
    sched_tcb_t   *task;
    kmon_entry_t  *mon;
    kmon_waiter_t *next;
};

static kmon_entry_t  kmon_pool[KMON_MAX];
static kmon_waiter_t kmon_waiters[KMON_MAX_WAITERS];
static uint32_t      kmon_next_handle = 1U;

static inline uint64_t irq_save(void)
{
    uint64_t flags;

    __asm__ volatile(
        "mrs %0, daif\n"
        "msr daifset, #2\n"
        : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

static int kmon_has_current_task(void)
{
    return current_task != NULL;
}

static uint64_t kmon_timeout_to_ms(uint64_t timeout_ns)
{
    if (timeout_ns == 0ULL)
        return 0ULL;
    return (timeout_ns + 999999ULL) / 1000000ULL;
}

static kmon_entry_t *kmon_find_locked(kmon_t handle)
{
    for (uint32_t i = 0U; i < KMON_MAX; i++) {
        if (kmon_pool[i].active && kmon_pool[i].handle == handle)
            return &kmon_pool[i];
    }

    return NULL;
}

static kmon_entry_t *kmon_alloc_locked(void)
{
    for (uint32_t i = 0U; i < KMON_MAX; i++) {
        if (!kmon_pool[i].active) {
            memset(&kmon_pool[i], 0, sizeof(kmon_pool[i]));
            kmon_pool[i].active = 1U;
            kmon_pool[i].handle = kmon_next_handle++;
            if (kmon_next_handle == 0U)
                kmon_next_handle = 1U;
            return &kmon_pool[i];
        }
    }

    return NULL;
}

static kmon_waiter_t *kmon_waiter_alloc_locked(kmon_entry_t *mon,
                                               sched_tcb_t *task,
                                               uint8_t queue_kind,
                                               uint8_t cond_idx)
{
    for (uint32_t i = 0U; i < KMON_MAX_WAITERS; i++) {
        kmon_waiter_t *waiter = &kmon_waiters[i];

        if (waiter->active)
            continue;

        memset(waiter, 0, sizeof(*waiter));
        waiter->active = 1U;
        waiter->mon = mon;
        waiter->task = task;
        waiter->queue_kind = queue_kind;
        waiter->cond_idx = cond_idx;
        return waiter;
    }

    return NULL;
}

static void kmon_waiter_free_locked(kmon_waiter_t *waiter)
{
    if (!waiter)
        return;
    memset(waiter, 0, sizeof(*waiter));
}

static kmon_queue_t *kmon_waiter_queue(kmon_entry_t *mon, kmon_waiter_t *waiter)
{
    if (!mon || !waiter)
        return NULL;

    switch (waiter->queue_kind) {
    case KMON_Q_ENTER:
        return &mon->enter_q;
    case KMON_Q_URGENT:
        return &mon->urgent_q;
    case KMON_Q_COND:
        if (waiter->cond_idx < KMON_MAX_COND)
            return &mon->cond[waiter->cond_idx].q;
        break;
    default:
        break;
    }

    return NULL;
}

static void kmon_queue_push_tail_locked(kmon_queue_t *q, kmon_waiter_t *waiter)
{
    waiter->next = NULL;
    if (!q->head) {
        q->head = q->tail = waiter;
    } else {
        q->tail->next = waiter;
        q->tail = waiter;
    }
    q->n_waiters++;
}

static kmon_waiter_t *kmon_queue_pop_head_locked(kmon_queue_t *q)
{
    kmon_waiter_t *waiter;

    if (!q || !q->head)
        return NULL;

    waiter = q->head;
    q->head = waiter->next;
    if (!q->head)
        q->tail = NULL;
    waiter->next = NULL;
    if (q->n_waiters != 0U)
        q->n_waiters--;
    return waiter;
}

static void kmon_queue_remove_locked(kmon_queue_t *q, kmon_waiter_t *waiter)
{
    kmon_waiter_t *prev = NULL;
    kmon_waiter_t *cur;

    if (!q || !waiter)
        return;

    cur = q->head;
    while (cur) {
        if (cur == waiter) {
            if (prev)
                prev->next = cur->next;
            else
                q->head = cur->next;

            if (q->tail == cur)
                q->tail = prev;
            cur->next = NULL;
            if (q->n_waiters != 0U)
                q->n_waiters--;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static void kmon_refresh_owner_locked(kmon_entry_t *mon)
{
    uint8_t best = 0xFFU;

    if (!mon || !mon->owner || !(mon->flags & KMON_RT))
        return;

    if (mon->prio_ceiling != 0U)
        best = (uint8_t)mon->prio_ceiling;

    for (kmon_waiter_t *w = mon->enter_q.head; w; w = w->next) {
        uint8_t prio = sched_task_effective_priority(w->task);
        if (prio < best)
            best = prio;
    }
    for (kmon_waiter_t *w = mon->urgent_q.head; w; w = w->next) {
        uint8_t prio = sched_task_effective_priority(w->task);
        if (prio < best)
            best = prio;
    }

    if (best != 0xFFU)
        (void)sched_task_donate_priority(mon->owner, best);
    else
        sched_task_clear_donation(mon->owner);
}

static void kmon_assign_owner_locked(kmon_entry_t *mon, sched_tcb_t *owner)
{
    mon->owner = owner;
    mon->entry_count = owner ? 1U : 0U;
    if (owner)
        kmon_refresh_owner_locked(mon);
}

static void kmon_release_locked(kmon_entry_t *mon, sched_tcb_t *old_owner)
{
    kmon_waiter_t *next = NULL;
    sched_tcb_t   *wake_task = NULL;

    if (!mon)
        return;

    if ((mon->flags & KMON_RT) && old_owner)
        sched_task_clear_donation(old_owner);

    if (mon->urgent_q.head)
        next = kmon_queue_pop_head_locked(&mon->urgent_q);
    else if (mon->enter_q.head)
        next = kmon_queue_pop_head_locked(&mon->enter_q);

    if (next) {
        next->queue_kind = KMON_Q_NONE;
        next->wake_reason = KMON_WAKE_LOCK;
        kmon_assign_owner_locked(mon, next->task);
        wake_task = next->task;
    } else {
        mon->owner = NULL;
        mon->entry_count = 0U;
    }

    if (wake_task && wake_task->state == TCB_STATE_BLOCKED)
        sched_unblock(wake_task);
}

static int kmon_enter_common(kmon_t handle, int allow_interrupt)
{
    kmon_entry_t  *mon;
    kmon_waiter_t *waiter;
    uint64_t       flags;

    if (!kmon_has_current_task())
        return -EPERM;

    flags = irq_save();
    mon = kmon_find_locked(handle);
    if (!mon) {
        irq_restore(flags);
        return -ENOENT;
    }

    if (!mon->owner) {
        kmon_assign_owner_locked(mon, current_task);
        irq_restore(flags);
        return 0;
    }

    if (mon->owner == current_task)
        kdebug_panic("kmon: recursive enter");

    waiter = kmon_waiter_alloc_locked(mon, current_task, KMON_Q_ENTER, 0U);
    if (!waiter) {
        irq_restore(flags);
        return -ENOSPC;
    }

    kmon_queue_push_tail_locked(&mon->enter_q, waiter);
    kmon_refresh_owner_locked(mon);
    irq_restore(flags);

    if (!allow_interrupt || !sched_task_is_user(current_task)) {
        for (;;) {
            if (waiter->wake_reason == KMON_WAKE_LOCK) {
                flags = irq_save();
                kmon_waiter_free_locked(waiter);
                irq_restore(flags);
                return 0;
            }
            sched_block();
        }
    }

    for (;;) {
        if (waiter->wake_reason == KMON_WAKE_LOCK) {
            flags = irq_save();
            kmon_waiter_free_locked(waiter);
            irq_restore(flags);
            return 0;
        }

        if (allow_interrupt && signal_has_unblocked_pending(current_task)) {
            flags = irq_save();
            if (waiter->active && waiter->wake_reason == KMON_WAKE_NONE) {
                kmon_queue_remove_locked(&mon->enter_q, waiter);
                kmon_waiter_free_locked(waiter);
                if (mon->owner)
                    kmon_refresh_owner_locked(mon);
            }
            irq_restore(flags);
            return -EINTR;
        }

        sched_yield();
    }
}

void kmon_init(void)
{
    memset(kmon_pool, 0, sizeof(kmon_pool));
    memset(kmon_waiters, 0, sizeof(kmon_waiters));
    kmon_next_handle = 1U;
    uart_puts("[KMON] Monitor kernel v1 pronti\n");
}

void kmon_task_cleanup(sched_tcb_t *task)
{
    uint64_t flags;

    if (!task)
        return;

    flags = irq_save();

    for (uint32_t i = 0U; i < KMON_MAX_WAITERS; i++) {
        kmon_waiter_t *waiter = &kmon_waiters[i];
        kmon_queue_t  *q;

        if (!waiter->active || waiter->task != task || !waiter->mon)
            continue;

        q = kmon_waiter_queue(waiter->mon, waiter);
        kmon_queue_remove_locked(q, waiter);
        if (waiter->mon->owner)
            kmon_refresh_owner_locked(waiter->mon);
        kmon_waiter_free_locked(waiter);
    }

    for (uint32_t i = 0U; i < KMON_MAX; i++) {
        kmon_entry_t *mon = &kmon_pool[i];

        if (!mon->active || mon->owner != task)
            continue;

        mon->entry_count = 0U;
        kmon_release_locked(mon, task);
    }

    irq_restore(flags);
}

int kmon_create_current(uint32_t prio_ceiling, uint32_t flags, kmon_t *handle_out)
{
    kmon_entry_t *mon;
    uint64_t      irqf;

    if (!kmon_has_current_task())
        return -EPERM;
    if (!handle_out)
        return -EINVAL;
    if (prio_ceiling > 255U)
        return -EINVAL;

    irqf = irq_save();
    mon = kmon_alloc_locked();
    if (!mon) {
        irq_restore(irqf);
        return -ENOSPC;
    }

    mon->prio_ceiling = prio_ceiling;
    mon->flags = flags ? flags : KMON_HOARE;
    if ((mon->flags & (KMON_HOARE | KMON_MESA)) == 0U)
        mon->flags |= KMON_HOARE;
    *handle_out = mon->handle;
    irq_restore(irqf);
    return 0;
}

int kmon_destroy_current(kmon_t handle)
{
    kmon_entry_t *mon;
    uint64_t      irqf;

    if (!kmon_has_current_task())
        return -EPERM;

    irqf = irq_save();
    mon = kmon_find_locked(handle);
    if (!mon) {
        irq_restore(irqf);
        return -ENOENT;
    }

    if (mon->owner || mon->enter_q.n_waiters || mon->urgent_q.n_waiters) {
        irq_restore(irqf);
        return -EBUSY;
    }
    for (uint32_t i = 0U; i < KMON_MAX_COND; i++) {
        if (mon->cond[i].q.n_waiters != 0U) {
            irq_restore(irqf);
            return -EBUSY;
        }
    }

    memset(mon, 0, sizeof(*mon));
    irq_restore(irqf);
    return 0;
}

int kmon_enter_current(kmon_t handle)
{
    return kmon_enter_common(handle, 1);
}

int kmon_exit_current(kmon_t handle)
{
    kmon_entry_t *mon;
    uint64_t      irqf;

    if (!kmon_has_current_task())
        return -EPERM;

    irqf = irq_save();
    mon = kmon_find_locked(handle);
    if (!mon) {
        irq_restore(irqf);
        return -ENOENT;
    }
    if (mon->owner != current_task || mon->entry_count == 0U) {
        irq_restore(irqf);
        return -EPERM;
    }

    mon->entry_count = 0U;
    kmon_release_locked(mon, current_task);
    irq_restore(irqf);
    return 0;
}

int kmon_wait_current(kmon_t handle, uint8_t cond, uint64_t timeout_ns)
{
    kmon_entry_t  *mon;
    kmon_waiter_t *waiter;
    uint64_t       deadline_ms = 0ULL;
    uint64_t       flags;
    int            rc;

    if (!kmon_has_current_task())
        return -EPERM;
    if (cond >= KMON_MAX_COND)
        return -EINVAL;

    flags = irq_save();
    mon = kmon_find_locked(handle);
    if (!mon) {
        irq_restore(flags);
        return -ENOENT;
    }
    if (mon->owner != current_task || mon->entry_count == 0U) {
        irq_restore(flags);
        return -EPERM;
    }

    waiter = kmon_waiter_alloc_locked(mon, current_task, KMON_Q_COND, cond);
    if (!waiter) {
        irq_restore(flags);
        return -ENOSPC;
    }

    kmon_queue_push_tail_locked(&mon->cond[cond].q, waiter);
    mon->entry_count = 0U;
    kmon_release_locked(mon, current_task);
    irq_restore(flags);

    if (timeout_ns != KMON_WAIT_FOREVER_NS)
        deadline_ms = timer_now_ms() + kmon_timeout_to_ms(timeout_ns);

    for (;;) {
        if (waiter->wake_reason == KMON_WAKE_LOCK) {
            flags = irq_save();
            kmon_waiter_free_locked(waiter);
            irq_restore(flags);
            return 0;
        }

        if (signal_has_unblocked_pending(current_task)) {
            flags = irq_save();
            if (waiter->active && waiter->wake_reason == KMON_WAKE_NONE) {
                kmon_queue_t *q = kmon_waiter_queue(mon, waiter);
                kmon_queue_remove_locked(q, waiter);
                kmon_waiter_free_locked(waiter);
            }
            irq_restore(flags);
            rc = kmon_enter_common(handle, 0);
            return (rc < 0) ? rc : -EINTR;
        }

        if (timeout_ns != KMON_WAIT_FOREVER_NS &&
            timer_now_ms() >= deadline_ms) {
            flags = irq_save();
            if (waiter->active && waiter->wake_reason == KMON_WAKE_NONE) {
                kmon_queue_t *q = kmon_waiter_queue(mon, waiter);
                kmon_queue_remove_locked(q, waiter);
                kmon_waiter_free_locked(waiter);
            }
            irq_restore(flags);
            rc = kmon_enter_common(handle, 0);
            return (rc < 0) ? rc : -ETIMEDOUT;
        }

        sched_yield();
    }
}

int kmon_signal_current(kmon_t handle, uint8_t cond)
{
    kmon_entry_t  *mon;
    kmon_waiter_t *target;
    uint64_t       flags;

    if (!kmon_has_current_task())
        return -EPERM;
    if (cond >= KMON_MAX_COND)
        return -EINVAL;

    flags = irq_save();
    mon = kmon_find_locked(handle);
    if (!mon) {
        irq_restore(flags);
        return -ENOENT;
    }
    if (mon->owner != current_task || mon->entry_count == 0U) {
        irq_restore(flags);
        return -EPERM;
    }

    target = kmon_queue_pop_head_locked(&mon->cond[cond].q);
    if (!target) {
        irq_restore(flags);
        return 0;
    }

    target->queue_kind = KMON_Q_ENTER;
    target->wake_reason = KMON_WAKE_NONE;
    kmon_queue_push_tail_locked(&mon->enter_q, target);
    if (mon->owner)
        kmon_refresh_owner_locked(mon);
    irq_restore(flags);
    return 0;
}

int kmon_broadcast_current(kmon_t handle, uint8_t cond)
{
    kmon_entry_t  *mon;
    kmon_waiter_t *first;
    uint64_t       flags;

    if (!kmon_has_current_task())
        return -EPERM;
    if (cond >= KMON_MAX_COND)
        return -EINVAL;

    flags = irq_save();
    mon = kmon_find_locked(handle);
    if (!mon) {
        irq_restore(flags);
        return -ENOENT;
    }
    if (mon->owner != current_task || mon->entry_count == 0U) {
        irq_restore(flags);
        return -EPERM;
    }

    first = kmon_queue_pop_head_locked(&mon->cond[cond].q);
    if (!first) {
        irq_restore(flags);
        return 0;
    }

    for (;;) {
        kmon_waiter_t *extra = kmon_queue_pop_head_locked(&mon->cond[cond].q);

        if (!extra)
            break;
        extra->queue_kind = KMON_Q_ENTER;
        extra->wake_reason = KMON_WAKE_NONE;
        kmon_queue_push_tail_locked(&mon->enter_q, extra);
    }

    first->queue_kind = KMON_Q_ENTER;
    first->wake_reason = KMON_WAKE_NONE;
    kmon_queue_push_tail_locked(&mon->enter_q, first);
    if (mon->owner)
        kmon_refresh_owner_locked(mon);
    irq_restore(flags);
    return 0;
}
