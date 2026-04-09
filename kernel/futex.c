#include "futex.h"

#include "mmu.h"
#include "signal.h"
#include "syscall.h"
#include "uart.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

#define FUTEX_BUCKET_COUNT   256U

#define FUTEX_WAIT_REASON_NONE  0U
#define FUTEX_WAIT_REASON_WAKE  1U

typedef struct futex_waiter futex_waiter_t;

struct futex_waiter {
    futex_waiter_t *next;
    sched_tcb_t    *task;
    uintptr_t       uaddr;
    uint32_t        proc_slot;
    uint8_t         queued;
    uint8_t         wake_reason;
    uint16_t        _pad0;
};

typedef struct {
    futex_waiter_t *head;
    futex_waiter_t *tail;
} futex_bucket_t;

static futex_bucket_t futex_buckets[FUTEX_BUCKET_COUNT];
static futex_waiter_t futex_waiters[SCHED_MAX_TASKS];

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

static uint32_t futex_hash(uint32_t proc_slot, uintptr_t uaddr)
{
    return (uint32_t)(((uaddr >> 2) ^ ((uintptr_t)proc_slot * 131U)) &
                      (FUTEX_BUCKET_COUNT - 1U));
}

static int futex_task_index(const sched_tcb_t *task)
{
    uint32_t total = sched_task_count_total();

    for (uint32_t i = 0U; i < total; i++) {
        if (sched_task_at(i) == task)
            return (int)i;
    }
    return -1;
}

static futex_waiter_t *futex_waiter_of(const sched_tcb_t *task)
{
    int index = futex_task_index(task);

    if (index < 0 || index >= (int)SCHED_MAX_TASKS)
        return NULL;
    return &futex_waiters[index];
}

static int futex_validate_uaddr(const sched_tcb_t *task, uintptr_t uaddr)
{
    if (!task || !sched_task_is_user(task))
        return -EPERM;
    if ((uaddr & (sizeof(uint32_t) - 1U)) != 0U)
        return -EINVAL;
    if (uaddr < MMU_USER_BASE || uaddr + sizeof(uint32_t) > MMU_USER_LIMIT)
        return -EFAULT;
    return 0;
}

static int futex_load_u32_task(const sched_tcb_t *task, uintptr_t uaddr, uint32_t *out)
{
    mm_space_t *space;
    void       *src;
    int         rc;

    if (!out)
        return -EINVAL;

    rc = futex_validate_uaddr(task, uaddr);
    if (rc < 0)
        return rc;

    space = sched_task_space(task);
    if (!space)
        return -EFAULT;

    src = mmu_space_resolve_ptr(space, uaddr, sizeof(uint32_t));
    if (!src)
        return -EFAULT;

    memcpy(out, src, sizeof(*out));
    return 0;
}

static futex_bucket_t *futex_bucket_for(uint32_t proc_slot, uintptr_t uaddr)
{
    return &futex_buckets[futex_hash(proc_slot, uaddr)];
}

static int futex_waiter_matches(const futex_waiter_t *waiter,
                                uint32_t proc_slot, uintptr_t uaddr)
{
    return waiter &&
           waiter->queued != 0U &&
           waiter->proc_slot == proc_slot &&
           waiter->uaddr == uaddr;
}

static void futex_waiter_reset(futex_waiter_t *waiter)
{
    if (!waiter)
        return;

    waiter->next = NULL;
    waiter->task = NULL;
    waiter->uaddr = 0U;
    waiter->proc_slot = 0U;
    waiter->queued = 0U;
    waiter->wake_reason = FUTEX_WAIT_REASON_NONE;
}

static void futex_bucket_append(futex_bucket_t *bucket, futex_waiter_t *waiter)
{
    if (!bucket || !waiter)
        return;

    waiter->next = NULL;
    if (!bucket->head) {
        bucket->head = waiter;
        bucket->tail = waiter;
    } else {
        bucket->tail->next = waiter;
        bucket->tail = waiter;
    }
    waiter->queued = 1U;
}

static void futex_bucket_remove(futex_bucket_t *bucket, futex_waiter_t *waiter)
{
    futex_waiter_t *prev = NULL;
    futex_waiter_t *cur;

    if (!bucket || !waiter)
        return;

    cur = bucket->head;
    while (cur) {
        if (cur == waiter) {
            if (prev)
                prev->next = cur->next;
            else
                bucket->head = cur->next;

            if (bucket->tail == cur)
                bucket->tail = prev;

            waiter->next = NULL;
            waiter->queued = 0U;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static uint32_t futex_wake_key_locked(uint32_t proc_slot, uintptr_t uaddr,
                                      uint32_t count, sched_tcb_t **wake_list)
{
    futex_bucket_t *bucket = futex_bucket_for(proc_slot, uaddr);
    futex_waiter_t *prev = NULL;
    futex_waiter_t *cur = bucket->head;
    uint32_t        woke = 0U;

    while (cur && woke < count) {
        futex_waiter_t *next = cur->next;

        if (futex_waiter_matches(cur, proc_slot, uaddr)) {
            if (prev)
                prev->next = next;
            else
                bucket->head = next;
            if (bucket->tail == cur)
                bucket->tail = prev;

            cur->next = NULL;
            cur->queued = 0U;
            cur->wake_reason = FUTEX_WAIT_REASON_WAKE;
            if (wake_list)
                wake_list[woke] = cur->task;
            woke++;
        } else {
            prev = cur;
        }

        cur = next;
    }

    return woke;
}

static uint32_t futex_requeue_key_locked(uint32_t proc_slot,
                                         uintptr_t uaddr,
                                         uint32_t wake_count,
                                         uint32_t requeue_count,
                                         uintptr_t uaddr2,
                                         sched_tcb_t **wake_list)
{
    futex_bucket_t *src_bucket = futex_bucket_for(proc_slot, uaddr);
    futex_bucket_t *dst_bucket = futex_bucket_for(proc_slot, uaddr2);
    futex_waiter_t *prev = NULL;
    futex_waiter_t *cur = src_bucket->head;
    uint32_t        woke = 0U;
    uint32_t        moved = 0U;

    while (cur && (woke < wake_count || moved < requeue_count)) {
        futex_waiter_t *next = cur->next;

        if (!futex_waiter_matches(cur, proc_slot, uaddr)) {
            prev = cur;
            cur = next;
            continue;
        }

        if (woke < wake_count) {
            if (prev)
                prev->next = next;
            else
                src_bucket->head = next;
            if (src_bucket->tail == cur)
                src_bucket->tail = prev;

            cur->next = NULL;
            cur->queued = 0U;
            cur->wake_reason = FUTEX_WAIT_REASON_WAKE;
            if (wake_list)
                wake_list[woke] = cur->task;
            woke++;
        } else if (moved < requeue_count && uaddr2 != uaddr) {
            if (prev)
                prev->next = next;
            else
                src_bucket->head = next;
            if (src_bucket->tail == cur)
                src_bucket->tail = prev;

            cur->uaddr = uaddr2;
            cur->next = NULL;
            if (!dst_bucket->head) {
                dst_bucket->head = cur;
                dst_bucket->tail = cur;
            } else {
                dst_bucket->tail->next = cur;
                dst_bucket->tail = cur;
            }
            moved++;
        } else {
            prev = cur;
        }

        cur = next;
    }

    return woke + moved;
}

void futex_init(void)
{
    memset(futex_buckets, 0, sizeof(futex_buckets));
    memset(futex_waiters, 0, sizeof(futex_waiters));
    uart_puts("[FUTEX] Core WAIT/WAKE/REQUEUE pronto\n");
}

int futex_wait_current(uintptr_t uaddr, uint32_t expected, uintptr_t timeout_uva)
{
    futex_waiter_t *waiter;
    futex_bucket_t *bucket;
    uint32_t        current = 0U;
    uint32_t        proc_slot;
    uint64_t        flags;
    int             rc;

    if (!current_task)
        return -EPERM;
    if (timeout_uva != 0U)
        return -ENOSYS;

    rc = futex_load_u32_task(current_task, uaddr, &current);
    if (rc < 0)
        return rc;
    if (current != expected)
        return -EAGAIN;

    waiter = futex_waiter_of(current_task);
    if (!waiter)
        return -EINVAL;

    proc_slot = sched_task_proc_slot(current_task);
    flags = irq_save();
    if (waiter->queued) {
        futex_bucket_t *old_bucket = futex_bucket_for(waiter->proc_slot, waiter->uaddr);
        futex_bucket_remove(old_bucket, waiter);
    }
    waiter->task = current_task;
    waiter->uaddr = uaddr;
    waiter->proc_slot = proc_slot;
    waiter->wake_reason = FUTEX_WAIT_REASON_NONE;
    bucket = futex_bucket_for(proc_slot, uaddr);
    futex_bucket_append(bucket, waiter);
    irq_restore(flags);

    sched_block();

    flags = irq_save();
    if (waiter->queued) {
        futex_bucket_remove(futex_bucket_for(waiter->proc_slot, waiter->uaddr), waiter);
    }
    rc = (waiter->wake_reason == FUTEX_WAIT_REASON_WAKE) ? 0 : -EINTR;
    futex_waiter_reset(waiter);
    irq_restore(flags);
    return rc;
}

int futex_wake_current(uintptr_t uaddr, uint32_t count)
{
    sched_tcb_t *wake_list[SCHED_MAX_TASKS];
    uint32_t     proc_slot;
    uint64_t     flags;
    uint32_t     woke;
    int          rc;

    if (!current_task)
        return -EPERM;

    rc = futex_validate_uaddr(current_task, uaddr);
    if (rc < 0)
        return rc;
    if (count == 0U)
        return 0;

    proc_slot = sched_task_proc_slot(current_task);
    flags = irq_save();
    woke = futex_wake_key_locked(proc_slot, uaddr, count, wake_list);
    irq_restore(flags);

    for (uint32_t i = 0U; i < woke; i++) {
        if (wake_list[i] && wake_list[i]->state == TCB_STATE_BLOCKED)
            sched_unblock(wake_list[i]);
    }
    return (int)woke;
}

int futex_requeue_current(uintptr_t uaddr,
                          uint32_t wake_count,
                          uint32_t requeue_count,
                          uintptr_t uaddr2,
                          int cmp_enabled,
                          uint32_t cmp_value)
{
    sched_tcb_t *wake_list[SCHED_MAX_TASKS];
    uint32_t     proc_slot;
    uint32_t     current = 0U;
    uint64_t     flags;
    uint32_t     count;
    int          rc;

    if (!current_task)
        return -EPERM;

    rc = futex_validate_uaddr(current_task, uaddr);
    if (rc < 0)
        return rc;
    rc = futex_validate_uaddr(current_task, uaddr2);
    if (rc < 0)
        return rc;

    if (cmp_enabled) {
        rc = futex_load_u32_task(current_task, uaddr, &current);
        if (rc < 0)
            return rc;
        if (current != cmp_value)
            return -EAGAIN;
    }

    proc_slot = sched_task_proc_slot(current_task);
    flags = irq_save();
    count = futex_requeue_key_locked(proc_slot, uaddr, wake_count, requeue_count,
                                     uaddr2, wake_list);
    irq_restore(flags);

    if (wake_count > 0U) {
        uint32_t woke = (count < wake_count) ? count : wake_count;

        for (uint32_t i = 0U; i < woke; i++) {
            if (wake_list[i] && wake_list[i]->state == TCB_STATE_BLOCKED)
                sched_unblock(wake_list[i]);
        }
    }

    return (int)count;
}

int futex_wake_task_uaddr(const sched_tcb_t *task, uintptr_t uaddr, uint32_t count)
{
    sched_tcb_t *wake_list[SCHED_MAX_TASKS];
    uint32_t     proc_slot;
    uint64_t     flags;
    uint32_t     woke;

    if (!task || uaddr == 0U || count == 0U)
        return 0;

    proc_slot = sched_task_proc_slot(task);
    flags = irq_save();
    woke = futex_wake_key_locked(proc_slot, uaddr, count, wake_list);
    irq_restore(flags);

    for (uint32_t i = 0U; i < woke; i++) {
        if (wake_list[i] && wake_list[i]->state == TCB_STATE_BLOCKED)
            sched_unblock(wake_list[i]);
    }

    return (int)woke;
}

void futex_task_cleanup(sched_tcb_t *task)
{
    futex_waiter_t *waiter;
    uint64_t        flags;

    waiter = futex_waiter_of(task);
    if (!waiter)
        return;

    flags = irq_save();
    if (waiter->queued) {
        futex_bucket_remove(futex_bucket_for(waiter->proc_slot, waiter->uaddr), waiter);
    }
    futex_waiter_reset(waiter);
    irq_restore(flags);
}
