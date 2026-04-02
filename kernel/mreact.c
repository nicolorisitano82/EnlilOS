/*
 * EnlilOS - Reactive memory subscriptions (M8-05)
 *
 * Implementazione v1:
 *   - backend bounded per-tick nel kernel
 *   - nessun polling in user-space
 *   - wakeup al tick successivo quando il predicato diventa vero
 */

#include "mreact.h"

#include "mmu.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer.h"
#include "uart.h"

extern void *memset(void *dst, int value, size_t n);

#define MREACT_MAX_HANDLES   64U

typedef struct {
    uintptr_t      addr;
    size_t         size;
    mreact_pred_t  pred;
    uint64_t       value;
    uint64_t       last_raw;
    uint8_t        initialized;
    uint8_t        last_truth;
    uint8_t        sample_every;
    uint8_t        sample_count;
} mreact_part_state_t;

typedef struct {
    uint32_t           handle;
    uint32_t           owner_pid;
    mm_space_t        *space;
    sched_tcb_t       *waiter;
    uint64_t           deadline_ms;
    int32_t            wait_result;
    uint32_t           trigger_count;
    uint8_t            active;
    uint8_t            armed;
    uint8_t            persistent;
    uint8_t            level;
    uint8_t            require_all;
    uint8_t            part_count;
    uint8_t            last_truth;
    uint8_t            _pad0;
    mreact_part_state_t parts[MREACT_MAX_SUBS];
} mreact_handle_state_t;

static mreact_handle_state_t mreact_handles[MREACT_MAX_HANDLES];
static uint32_t              mreact_next_handle = 1U;

static int mreact_is_owner(const mreact_handle_state_t *st)
{
    return current_task && st && st->owner_pid == current_task->pid;
}

static uint64_t mreact_timeout_to_ms(uint64_t timeout_ns)
{
    if (timeout_ns == 0ULL)
        return 0ULL;
    return (timeout_ns + 999999ULL) / 1000000ULL;
}

static int mreact_read_raw(mm_space_t *space, uintptr_t addr, size_t size, uint64_t *out)
{
    void *ptr;

    if (!space || !out)
        return -EINVAL;
    if (!(size == 1U || size == 2U || size == 4U || size == 8U))
        return -EINVAL;
    if ((addr & (size - 1U)) != 0U)
        return -EINVAL;

    ptr = mmu_space_resolve_ptr(space, addr, size);
    if (!ptr)
        return -EFAULT;

    switch (size) {
    case 1U:
        *out = *(volatile const uint8_t *)ptr;
        break;
    case 2U:
        *out = *(volatile const uint16_t *)ptr;
        break;
    case 4U:
        *out = *(volatile const uint32_t *)ptr;
        break;
    case 8U:
        *out = *(volatile const uint64_t *)ptr;
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static int mreact_pred_eval(const mreact_part_state_t *part, uint64_t raw, int changed)
{
    uint64_t lo;
    uint64_t hi;

    switch (part->pred) {
    case MREACT_EQ:
        return raw == part->value;
    case MREACT_NEQ:
        return raw != part->value;
    case MREACT_GT:
        return raw > part->value;
    case MREACT_LT:
        return raw < part->value;
    case MREACT_BITMASK_SET:
        return (raw & part->value) == part->value;
    case MREACT_BITMASK_CLEAR:
        return (raw & part->value) == 0ULL;
    case MREACT_CHANGED:
        return changed;
    case MREACT_RANGE_IN:
        lo = (uint32_t)(part->value & 0xFFFFFFFFULL);
        hi = (uint32_t)(part->value >> 32);
        return raw >= lo && raw <= hi;
    case MREACT_RANGE_OUT:
        lo = (uint32_t)(part->value & 0xFFFFFFFFULL);
        hi = (uint32_t)(part->value >> 32);
        return raw < lo || raw > hi;
    default:
        return 0;
    }
}

static int mreact_part_truth(mreact_part_state_t *part, mm_space_t *space, int *truth_out)
{
    uint64_t raw;
    int      rc;
    int      changed;
    int      truth;

    rc = mreact_read_raw(space, part->addr, part->size, &raw);
    if (rc < 0)
        return rc;

    if (!part->initialized) {
        part->last_raw = raw;
        part->initialized = 1U;
        part->sample_count = 0U;
        truth = mreact_pred_eval(part, raw, 0);
        part->last_truth = (uint8_t)truth;
        *truth_out = truth;
        return 0;
    }

    changed = (raw != part->last_raw);
    if (changed && part->sample_every > 1U) {
        part->sample_count++;
        if (part->sample_count < part->sample_every) {
            part->last_raw = raw;
            *truth_out = 0;
            return 0;
        }
        part->sample_count = 0U;
    }

    truth = mreact_pred_eval(part, raw, changed);
    part->last_raw = raw;
    part->last_truth = (uint8_t)truth;
    *truth_out = truth;
    return 0;
}

static int mreact_handle_truth(mreact_handle_state_t *st, int *truth_out)
{
    uint32_t true_count = 0U;

    if (!st || !st->space || st->part_count == 0U)
        return -EINVAL;

    for (uint32_t i = 0U; i < st->part_count; i++) {
        int truth = 0;
        int rc = mreact_part_truth(&st->parts[i], st->space, &truth);
        if (rc < 0)
            return rc;
        if (truth)
            true_count++;
    }

    if (st->require_all)
        *truth_out = (true_count == st->part_count);
    else
        *truth_out = (true_count != 0U);
    return 0;
}

static void mreact_fire(mreact_handle_state_t *st)
{
    if (!st)
        return;

    st->trigger_count++;
    st->wait_result = 0;
    if (!st->persistent)
        st->armed = 0U;
}

static mreact_handle_state_t *mreact_find_handle(uint32_t handle)
{
    for (uint32_t i = 0U; i < MREACT_MAX_HANDLES; i++) {
        if (mreact_handles[i].active && mreact_handles[i].handle == handle)
            return &mreact_handles[i];
    }
    return NULL;
}

static mreact_handle_state_t *mreact_alloc_handle(void)
{
    for (uint32_t i = 0U; i < MREACT_MAX_HANDLES; i++) {
        if (!mreact_handles[i].active) {
            memset(&mreact_handles[i], 0, sizeof(mreact_handles[i]));
            mreact_handles[i].active = 1U;
            mreact_handles[i].armed = 1U;
            mreact_handles[i].handle = mreact_next_handle++;
            if (mreact_next_handle == 0U)
                mreact_next_handle = 1U;
            return &mreact_handles[i];
        }
    }
    return NULL;
}

static void mreact_free_handle(mreact_handle_state_t *st)
{
    if (!st)
        return;
    memset(st, 0, sizeof(*st));
}

void mreact_init(void)
{
    memset(mreact_handles, 0, sizeof(mreact_handles));
    mreact_next_handle = 1U;
    uart_puts("[MREACT] Reactive subscriptions v1 pronte\n");
}

void mreact_task_cleanup(sched_tcb_t *task)
{
    if (!task)
        return;

    for (uint32_t i = 0U; i < MREACT_MAX_HANDLES; i++) {
        mreact_handle_state_t *st = &mreact_handles[i];

        if (!st->active || st->owner_pid != task->pid)
            continue;
        if (st->waiter) {
            sched_unblock(st->waiter);
            st->waiter = NULL;
        }
        mreact_free_handle(st);
    }
}

void mreact_tick(uint64_t now_ms)
{
    (void)now_ms;
}

int mreact_subscribe_current(const mreact_sub_t *subs, uint32_t count,
                             int require_all, uint32_t common_flags,
                             mreact_handle_t *handle_out)
{
    mreact_handle_state_t *st;
    int                    initial_truth = 0;

    if (!current_task || !sched_task_is_user(current_task))
        return -EPERM;
    if (!subs || count == 0U || count > MREACT_MAX_SUBS || !handle_out)
        return -EINVAL;

    st = mreact_alloc_handle();
    if (!st)
        return -ENOSPC;

    st->owner_pid   = current_task->pid;
    st->space       = sched_task_space(current_task);
    st->require_all = require_all ? 1U : 0U;
    st->part_count  = (uint8_t)count;
    st->persistent  = (common_flags & MREACT_PERSISTENT) ? 1U : 0U;
    st->level       = (common_flags & MREACT_LEVEL) ? 1U : 0U;
    if ((common_flags & MREACT_PERSISTENT) == 0U &&
        (common_flags & MREACT_ONE_SHOT) == 0U) {
        st->persistent = 0U;
    }

    for (uint32_t i = 0U; i < count; i++) {
        const mreact_sub_t *src = &subs[i];
        mreact_part_state_t *dst = &st->parts[i];
        uint32_t sample;

        if (!(src->size == 1U || src->size == 2U || src->size == 4U || src->size == 8U)) {
            mreact_free_handle(st);
            return -EINVAL;
        }
        if (((uintptr_t)src->addr & (src->size - 1U)) != 0U) {
            mreact_free_handle(st);
            return -EINVAL;
        }

        sample = MREACT_SAMPLE_GET(src->flags ? src->flags : common_flags);
        if (sample == 0U)
            sample = 1U;

        dst->addr         = (uintptr_t)src->addr;
        dst->size         = src->size;
        dst->pred         = src->pred;
        dst->value        = src->value;
        dst->sample_every = (uint8_t)sample;
    }

    if (mreact_handle_truth(st, &initial_truth) < 0) {
        mreact_free_handle(st);
        return -EFAULT;
    }
    st->last_truth = (uint8_t)initial_truth;
    if (st->level && initial_truth) {
        st->trigger_count = 1U;
        if (!st->persistent)
            st->armed = 0U;
    }

    *handle_out = st->handle;
    return 0;
}

int mreact_wait_current(mreact_handle_t handle, uint64_t timeout_ns)
{
    mreact_handle_state_t *st;
    int                    truth = 0;
    uint64_t               deadline_ms = 0ULL;

    st = mreact_find_handle(handle);
    if (!st)
        return -ENOENT;
    if (!mreact_is_owner(st))
        return -EPERM;

    if (st->trigger_count != 0U) {
        st->trigger_count--;
        if (!st->persistent && !st->armed && st->trigger_count == 0U)
            mreact_free_handle(st);
        return 0;
    }

    if (st->level && st->armed && mreact_handle_truth(st, &truth) == 0 && truth)
        return 0;

    if (timeout_ns != MREACT_WAIT_FOREVER_NS)
        deadline_ms = timer_now_ms() + mreact_timeout_to_ms(timeout_ns);

    for (;;) {
        if (!st->active)
            return -ENOENT;
        if (signal_has_unblocked_pending(current_task))
            return -EINTR;
        if (st->armed && mreact_handle_truth(st, &truth) == 0 && truth) {
            mreact_fire(st);
            st->last_truth = 1U;
        }
        if (st->trigger_count != 0U) {
            st->trigger_count--;
            if (!st->persistent && !st->armed && st->trigger_count == 0U)
                mreact_free_handle(st);
            return 0;
        }
        if (timeout_ns != MREACT_WAIT_FOREVER_NS &&
            timer_now_ms() >= deadline_ms) {
            return -EAGAIN;
        }
        sched_yield();
    }
}

int mreact_cancel_current(mreact_handle_t handle)
{
    mreact_handle_state_t *st = mreact_find_handle(handle);

    if (!st)
        return -ENOENT;
    if (!mreact_is_owner(st))
        return -EPERM;

    mreact_free_handle(st);
    return 0;
}
