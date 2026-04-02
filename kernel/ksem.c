/*
 * EnlilOS - Kernel semaphores (M8-06)
 *
 * Implementazione v1:
 *   - pool statico di semafori / waiter / handle
 *   - namespace named lineare su pool statico
 *   - wait queue FIFO con wake diretto del primo waiter
 *   - timedwait bounded via timer_now_ms() + sched_yield()
 *   - priority inheritance best-effort riusando il donation slot dello scheduler
 */

#include "ksem.h"

#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer.h"
#include "uart.h"

extern void *memset(void *dst, int value, size_t n);

#define KSEM_MAX                 64U
#define KSEM_MAX_WAITERS         64U
#define KSEM_MAX_HANDLE_REFS     (SCHED_MAX_TASKS * 16U)

typedef struct ksem_entry  ksem_entry_t;
typedef struct ksem_waiter ksem_waiter_t;

struct ksem_waiter {
    uint8_t        active;
    uint8_t        granted;
    uint8_t        _pad0;
    uint8_t        _pad1;
    sched_tcb_t   *task;
    ksem_entry_t  *sem;
    ksem_waiter_t *next;
};

struct ksem_entry {
    uint32_t       id;
    int32_t        value;
    uint32_t       flags;
    uint32_t       refcount;
    uint8_t        active;
    uint8_t        named;
    uint8_t        unlinked;
    uint8_t        oneshot_fired;
    char           name[KSEM_NAME_MAX];
    sched_tcb_t   *owner_hint;
    ksem_waiter_t *wait_head;
    ksem_waiter_t *wait_tail;
};

typedef struct {
    uint8_t       active;
    uint8_t       _pad0;
    uint16_t      _pad1;
    uint32_t      owner_pid;
    uint32_t      handle;
    ksem_entry_t *sem;
} ksem_ref_t;

static ksem_entry_t  ksem_pool[KSEM_MAX];
static ksem_waiter_t ksem_waiters[KSEM_MAX_WAITERS];
static ksem_ref_t    ksem_refs[KSEM_MAX_HANDLE_REFS];
static uint32_t      ksem_next_id = 1U;
static uint32_t      ksem_next_handle = 1U;

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

static int ksem_has_current_task(void)
{
    return current_task != NULL;
}

static uint64_t ksem_timeout_to_ms(uint64_t timeout_ns)
{
    if (timeout_ns == 0ULL)
        return 0ULL;
    return (timeout_ns + 999999ULL) / 1000000ULL;
}

static int ksem_name_equal(const char *a, const char *b)
{
    uint32_t i = 0U;

    if (!a || !b)
        return 0;

    while (i < KSEM_NAME_MAX) {
        if (a[i] != b[i])
            return 0;
        if (a[i] == '\0')
            return 1;
        i++;
    }

    return 1;
}

static int ksem_name_copy(char dst[KSEM_NAME_MAX], const char *src)
{
    uint32_t i = 0U;

    if (!dst || !src || src[0] == '\0')
        return -EINVAL;

    while (src[i] != '\0') {
        if (i + 1U >= KSEM_NAME_MAX)
            return -ENAMETOOLONG;
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
    return 0;
}

static ksem_entry_t *ksem_find_named_locked(const char *name)
{
    for (uint32_t i = 0U; i < KSEM_MAX; i++) {
        ksem_entry_t *sem = &ksem_pool[i];

        if (!sem->active || !sem->named || sem->unlinked)
            continue;
        if (ksem_name_equal(sem->name, name))
            return sem;
    }

    return NULL;
}

static ksem_entry_t *ksem_alloc_entry_locked(void)
{
    for (uint32_t i = 0U; i < KSEM_MAX; i++) {
        if (!ksem_pool[i].active) {
            memset(&ksem_pool[i], 0, sizeof(ksem_pool[i]));
            ksem_pool[i].active = 1U;
            ksem_pool[i].id = ksem_next_id++;
            if (ksem_next_id == 0U)
                ksem_next_id = 1U;
            return &ksem_pool[i];
        }
    }

    return NULL;
}

static ksem_ref_t *ksem_alloc_ref_locked(ksem_entry_t *sem)
{
    if (!current_task || !sem)
        return NULL;

    for (uint32_t i = 0U; i < KSEM_MAX_HANDLE_REFS; i++) {
        ksem_ref_t *ref = &ksem_refs[i];

        if (ref->active)
            continue;

        memset(ref, 0, sizeof(*ref));
        ref->active = 1U;
        ref->owner_pid = current_task->pid;
        ref->handle = ksem_next_handle++;
        ref->sem = sem;
        if (ksem_next_handle == 0U)
            ksem_next_handle = 1U;
        sem->refcount++;
        return ref;
    }

    return NULL;
}

static ksem_ref_t *ksem_find_ref_locked(uint32_t owner_pid, ksem_t handle)
{
    for (uint32_t i = 0U; i < KSEM_MAX_HANDLE_REFS; i++) {
        if (ksem_refs[i].active &&
            ksem_refs[i].owner_pid == owner_pid &&
            ksem_refs[i].handle == handle)
            return &ksem_refs[i];
    }

    return NULL;
}

static ksem_waiter_t *ksem_alloc_waiter_locked(sched_tcb_t *task, ksem_entry_t *sem)
{
    for (uint32_t i = 0U; i < KSEM_MAX_WAITERS; i++) {
        ksem_waiter_t *waiter = &ksem_waiters[i];

        if (waiter->active)
            continue;

        memset(waiter, 0, sizeof(*waiter));
        waiter->active = 1U;
        waiter->task = task;
        waiter->sem = sem;
        return waiter;
    }

    return NULL;
}

static void ksem_free_waiter_locked(ksem_waiter_t *waiter)
{
    if (!waiter)
        return;
    memset(waiter, 0, sizeof(*waiter));
}

static void ksem_waitq_push_locked(ksem_entry_t *sem, ksem_waiter_t *waiter)
{
    waiter->next = NULL;
    if (!sem->wait_head) {
        sem->wait_head = sem->wait_tail = waiter;
    } else {
        sem->wait_tail->next = waiter;
        sem->wait_tail = waiter;
    }
}

static void ksem_waitq_remove_locked(ksem_entry_t *sem, ksem_waiter_t *waiter)
{
    ksem_waiter_t *prev = NULL;
    ksem_waiter_t *cur = sem ? sem->wait_head : NULL;

    while (cur) {
        if (cur == waiter) {
            if (prev)
                prev->next = cur->next;
            else
                sem->wait_head = cur->next;

            if (sem->wait_tail == cur)
                sem->wait_tail = prev;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static ksem_waiter_t *ksem_waitq_pop_locked(ksem_entry_t *sem)
{
    ksem_waiter_t *waiter;

    if (!sem || !sem->wait_head)
        return NULL;

    waiter = sem->wait_head;
    sem->wait_head = waiter->next;
    if (!sem->wait_head)
        sem->wait_tail = NULL;
    waiter->next = NULL;
    return waiter;
}

static void ksem_update_pi_locked(ksem_entry_t *sem)
{
    uint8_t best_prio = 0xFFU;

    if (!sem || !(sem->flags & KSEM_RT))
        return;
    if (!sem->owner_hint)
        return;

    for (ksem_waiter_t *waiter = sem->wait_head; waiter; waiter = waiter->next) {
        if (waiter->task) {
            uint8_t prio = sched_task_effective_priority(waiter->task);
            if (prio < best_prio)
                best_prio = prio;
        }
    }

    if (best_prio != 0xFFU)
        (void)sched_task_donate_priority(sem->owner_hint, best_prio);
    else
        sched_task_clear_donation(sem->owner_hint);
}

static void ksem_mark_oneshot_locked(ksem_entry_t *sem)
{
    if (!sem || !(sem->flags & KSEM_ONESHOT) || sem->oneshot_fired)
        return;

    sem->oneshot_fired = 1U;
    sem->unlinked = 1U;
    sem->named = 0U;
    sem->name[0] = '\0';
}

static int ksem_can_destroy_locked(const ksem_entry_t *sem)
{
    if (!sem || !sem->active)
        return 0;
    if (sem->wait_head != NULL)
        return 0;
    if (sem->refcount != 0U)
        return 0;
    if (sem->named && !sem->unlinked)
        return 0;
    return 1;
}

static void ksem_destroy_locked(ksem_entry_t *sem)
{
    if (!sem || !sem->active)
        return;

    if ((sem->flags & KSEM_RT) && sem->owner_hint)
        sched_task_clear_donation(sem->owner_hint);
    memset(sem, 0, sizeof(*sem));
}

static void ksem_drop_ref_locked(ksem_ref_t *ref)
{
    ksem_entry_t *sem;

    if (!ref || !ref->active)
        return;

    sem = ref->sem;
    memset(ref, 0, sizeof(*ref));

    if (!sem)
        return;

    if (sem->refcount != 0U)
        sem->refcount--;
    if (ksem_can_destroy_locked(sem))
        ksem_destroy_locked(sem);
}

static int ksem_wait_common_current(ksem_t handle, uint64_t timeout_ns, int try_only)
{
    ksem_ref_t    *ref;
    ksem_entry_t  *sem;
    ksem_waiter_t *waiter = NULL;
    uint64_t       deadline_ms = 0ULL;
    uint64_t       flags;

    if (!ksem_has_current_task())
        return -EPERM;

    flags = irq_save();
    ref = ksem_find_ref_locked(current_task->pid, handle);
    if (!ref) {
        irq_restore(flags);
        return -ENOENT;
    }

    sem = ref->sem;
    if (!sem || !sem->active) {
        irq_restore(flags);
        return -ENOENT;
    }

    if (sem->value > 0) {
        if ((sem->flags & KSEM_RT) && sem->owner_hint &&
            sem->owner_hint != current_task) {
            sched_task_clear_donation(sem->owner_hint);
        }
        sem->value--;
        if (sem->flags & KSEM_RT)
            sem->owner_hint = current_task;
        ksem_update_pi_locked(sem);
        ksem_mark_oneshot_locked(sem);
        if (ksem_can_destroy_locked(sem))
            ksem_destroy_locked(sem);
        irq_restore(flags);
        return 0;
    }

    if (try_only) {
        irq_restore(flags);
        return -EAGAIN;
    }

    waiter = ksem_alloc_waiter_locked(current_task, sem);
    if (!waiter) {
        irq_restore(flags);
        return -ENOSPC;
    }

    ksem_waitq_push_locked(sem, waiter);
    ksem_update_pi_locked(sem);
    irq_restore(flags);

    if (timeout_ns != KSEM_WAIT_FOREVER_NS)
        deadline_ms = timer_now_ms() + ksem_timeout_to_ms(timeout_ns);

    for (;;) {
        if (waiter->granted) {
            flags = irq_save();
            ksem_free_waiter_locked(waiter);
            if (ksem_can_destroy_locked(sem))
                ksem_destroy_locked(sem);
            irq_restore(flags);
            return 0;
        }

        if (signal_has_unblocked_pending(current_task)) {
            flags = irq_save();
            if (waiter->active && !waiter->granted) {
                ksem_waitq_remove_locked(sem, waiter);
                ksem_update_pi_locked(sem);
                ksem_free_waiter_locked(waiter);
                if (ksem_can_destroy_locked(sem))
                    ksem_destroy_locked(sem);
            }
            irq_restore(flags);
            return -EINTR;
        }

        if (timeout_ns != KSEM_WAIT_FOREVER_NS &&
            timer_now_ms() >= deadline_ms) {
            flags = irq_save();
            if (waiter->active && !waiter->granted) {
                ksem_waitq_remove_locked(sem, waiter);
                ksem_update_pi_locked(sem);
                ksem_free_waiter_locked(waiter);
                if (ksem_can_destroy_locked(sem))
                    ksem_destroy_locked(sem);
            }
            irq_restore(flags);
            return -ETIMEDOUT;
        }

        sched_yield();
    }
}

void ksem_init(void)
{
    memset(ksem_pool, 0, sizeof(ksem_pool));
    memset(ksem_waiters, 0, sizeof(ksem_waiters));
    memset(ksem_refs, 0, sizeof(ksem_refs));
    ksem_next_id = 1U;
    ksem_next_handle = 1U;
    uart_puts("[KSEM] Semafori kernel v1 pronti\n");
}

void ksem_task_cleanup(sched_tcb_t *task)
{
    uint64_t flags;

    if (!task)
        return;

    flags = irq_save();

    for (uint32_t i = 0U; i < KSEM_MAX_WAITERS; i++) {
        ksem_waiter_t *waiter = &ksem_waiters[i];

        if (!waiter->active || waiter->task != task || !waiter->sem)
            continue;

        ksem_waitq_remove_locked(waiter->sem, waiter);
        ksem_update_pi_locked(waiter->sem);
        if (ksem_can_destroy_locked(waiter->sem))
            ksem_destroy_locked(waiter->sem);
        ksem_free_waiter_locked(waiter);
    }

    for (uint32_t i = 0U; i < KSEM_MAX; i++) {
        if (ksem_pool[i].active && ksem_pool[i].owner_hint == task) {
            if (ksem_pool[i].flags & KSEM_RT)
                sched_task_clear_donation(task);
            ksem_pool[i].owner_hint = NULL;
            ksem_update_pi_locked(&ksem_pool[i]);
        }
    }

    for (uint32_t i = 0U; i < KSEM_MAX_HANDLE_REFS; i++) {
        if (ksem_refs[i].active && ksem_refs[i].owner_pid == task->pid)
            ksem_drop_ref_locked(&ksem_refs[i]);
    }

    irq_restore(flags);
}

int ksem_create_current(const char *name, uint32_t value, uint32_t flags,
                        ksem_t *handle_out)
{
    ksem_entry_t *sem;
    ksem_ref_t   *ref;
    uint64_t      irqf;
    int           rc;

    if (!ksem_has_current_task())
        return -EPERM;
    if (!name || !handle_out)
        return -EINVAL;
    if ((flags & KSEM_PRIVATE) != 0U)
        flags &= ~KSEM_PRIVATE;
    flags |= KSEM_SHARED;

    irqf = irq_save();
    sem = ksem_find_named_locked(name);
    if (sem) {
        irq_restore(irqf);
        return -EEXIST;
    }

    sem = ksem_alloc_entry_locked();
    if (!sem) {
        irq_restore(irqf);
        return -ENOSPC;
    }

    rc = ksem_name_copy(sem->name, name);
    if (rc < 0) {
        memset(sem, 0, sizeof(*sem));
        irq_restore(irqf);
        return rc;
    }

    sem->named = 1U;
    sem->flags = flags;
    sem->value = (int32_t)value;

    ref = ksem_alloc_ref_locked(sem);
    if (!ref) {
        memset(sem, 0, sizeof(*sem));
        irq_restore(irqf);
        return -ENFILE;
    }

    *handle_out = ref->handle;
    irq_restore(irqf);
    return 0;
}

int ksem_open_current(const char *name, uint32_t flags, ksem_t *handle_out)
{
    ksem_entry_t *sem;
    ksem_ref_t   *ref;
    uint64_t      irqf;

    (void)flags;

    if (!ksem_has_current_task())
        return -EPERM;
    if (!name || !handle_out)
        return -EINVAL;

    irqf = irq_save();
    sem = ksem_find_named_locked(name);
    if (!sem) {
        irq_restore(irqf);
        return -ENOENT;
    }

    ref = ksem_alloc_ref_locked(sem);
    if (!ref) {
        irq_restore(irqf);
        return -ENFILE;
    }

    *handle_out = ref->handle;
    irq_restore(irqf);
    return 0;
}

int ksem_close_current(ksem_t handle)
{
    ksem_ref_t *ref;
    uint64_t    irqf;

    if (!ksem_has_current_task())
        return -EPERM;

    irqf = irq_save();
    ref = ksem_find_ref_locked(current_task->pid, handle);
    if (!ref) {
        irq_restore(irqf);
        return -ENOENT;
    }

    ksem_drop_ref_locked(ref);
    irq_restore(irqf);
    return 0;
}

int ksem_unlink_current(const char *name)
{
    ksem_entry_t *sem;
    uint64_t      irqf;

    if (!ksem_has_current_task())
        return -EPERM;
    if (!name)
        return -EINVAL;

    irqf = irq_save();
    sem = ksem_find_named_locked(name);
    if (!sem) {
        irq_restore(irqf);
        return -ENOENT;
    }

    sem->unlinked = 1U;
    sem->named = 0U;
    sem->name[0] = '\0';
    if (ksem_can_destroy_locked(sem))
        ksem_destroy_locked(sem);
    irq_restore(irqf);
    return 0;
}

int ksem_post_current(ksem_t handle)
{
    ksem_ref_t    *ref;
    ksem_entry_t  *sem;
    ksem_waiter_t *waiter;
    uint64_t       irqf;

    if (!ksem_has_current_task())
        return -EPERM;

    irqf = irq_save();
    ref = ksem_find_ref_locked(current_task->pid, handle);
    if (!ref) {
        irq_restore(irqf);
        return -ENOENT;
    }

    sem = ref->sem;
    if (!sem || !sem->active) {
        irq_restore(irqf);
        return -ENOENT;
    }

    waiter = ksem_waitq_pop_locked(sem);
    if (waiter) {
        if ((sem->flags & KSEM_RT) && sem->owner_hint &&
            sem->owner_hint != waiter->task) {
            sched_task_clear_donation(sem->owner_hint);
        }
        waiter->granted = 1U;
        if (sem->flags & KSEM_RT)
            sem->owner_hint = waiter->task;
        ksem_update_pi_locked(sem);
    } else {
        if ((sem->flags & KSEM_RT) && sem->owner_hint)
            sched_task_clear_donation(sem->owner_hint);
        sem->owner_hint = NULL;
        sem->value++;
    }

    ksem_mark_oneshot_locked(sem);
    if (ksem_can_destroy_locked(sem))
        ksem_destroy_locked(sem);
    irq_restore(irqf);
    return 0;
}

int ksem_wait_current(ksem_t handle)
{
    return ksem_wait_common_current(handle, KSEM_WAIT_FOREVER_NS, 0);
}

int ksem_timedwait_current(ksem_t handle, uint64_t timeout_ns)
{
    return ksem_wait_common_current(handle, timeout_ns, 0);
}

int ksem_trywait_current(ksem_t handle)
{
    return ksem_wait_common_current(handle, 1ULL, 1);
}

int ksem_getvalue_current(ksem_t handle, int32_t *value_out)
{
    ksem_ref_t *ref;
    uint64_t    irqf;

    if (!ksem_has_current_task())
        return -EPERM;
    if (!value_out)
        return -EINVAL;

    irqf = irq_save();
    ref = ksem_find_ref_locked(current_task->pid, handle);
    if (!ref) {
        irq_restore(irqf);
        return -ENOENT;
    }

    if (!ref->sem || !ref->sem->active) {
        irq_restore(irqf);
        return -ENOENT;
    }

    *value_out = ref->sem->value;
    irq_restore(irqf);
    return 0;
}

int ksem_anon_current(uint32_t value, uint32_t flags, ksem_t *handle_out)
{
    ksem_entry_t *sem;
    ksem_ref_t   *ref;
    uint64_t      irqf;

    if (!ksem_has_current_task())
        return -EPERM;
    if (!handle_out)
        return -EINVAL;

    irqf = irq_save();
    sem = ksem_alloc_entry_locked();
    if (!sem) {
        irq_restore(irqf);
        return -ENOSPC;
    }

    sem->named = 0U;
    sem->unlinked = 1U;
    sem->flags = (flags | KSEM_PRIVATE) & ~KSEM_SHARED;
    sem->value = (int32_t)value;

    ref = ksem_alloc_ref_locked(sem);
    if (!ref) {
        memset(sem, 0, sizeof(*sem));
        irq_restore(irqf);
        return -ENFILE;
    }

    *handle_out = ref->handle;
    irq_restore(irqf);
    return 0;
}
