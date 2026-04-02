/*
 * EnlilOS - Signal handling minimale (M8-03)
 */

#include "signal.h"

#include "mreact.h"
#include "mmu.h"
#include "pmm.h"
#include "sched.h"
#include "syscall.h"
#include "uart.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

#define SIGFRAME_MAGIC 0x5349474652414D45ULL /* "SIGFRAME" */

typedef struct {
    uint64_t         magic;
    uint64_t         saved_mask;
    uint64_t         delivered_sig;
    uint64_t         _pad0;
    exception_frame_t saved_frame;
} signal_user_frame_t;

typedef struct {
    uint64_t    pending;
    uint64_t    blocked;
    uint32_t    parent_pid;
    uint8_t     in_handler;
    uint8_t     stopped;
    uint8_t     active_sig;
    uint8_t     _pad0;
    sigaction_t actions[ENLILOS_NSIG];
} signal_state_t;

static signal_state_t signal_state[SCHED_MAX_TASKS];

static inline uint32_t signal_slot_pid(uint32_t pid)
{
    return pid % SCHED_MAX_TASKS;
}

static inline uint32_t signal_slot_task(const sched_tcb_t *task)
{
    return signal_slot_pid(task ? task->pid : 0U);
}

static inline uint64_t signal_bit(int sig)
{
    return 1ULL << (uint32_t)(sig - 1);
}

static int signal_valid(int sig)
{
    return sig >= 1 && sig <= ENLILOS_NSIG;
}

static uint64_t signal_unmaskable_bits(void)
{
    return signal_bit(SIGKILL) | signal_bit(SIGSTOP);
}

static uint64_t signal_strip_unmaskable(uint64_t mask)
{
    return mask & ~signal_unmaskable_bits();
}

static int signal_default_ignore(int sig)
{
    return sig == SIGCHLD;
}

static int signal_default_stop(int sig)
{
    return sig == SIGSTOP;
}

static int signal_default_terminate(int sig)
{
    if (signal_default_ignore(sig) || signal_default_stop(sig) || sig == SIGCONT)
        return 0;
    return 1;
}

static int signal_copy_from_user(mm_space_t *space, uintptr_t uva, void *dst, size_t size)
{
    uint8_t *out = (uint8_t *)dst;
    size_t   copied = 0U;

    while (copied < size) {
        uintptr_t cur = uva + copied;
        size_t    off = (size_t)(cur & (PAGE_SIZE - 1ULL));
        size_t    chunk = PAGE_SIZE - off;
        void     *src;

        if (chunk > size - copied)
            chunk = size - copied;

        src = mmu_space_resolve_ptr(space, cur, chunk);
        if (!src)
            return -EFAULT;
        memcpy(out + copied, src, chunk);
        copied += chunk;
    }

    return 0;
}

static int signal_copy_to_user(mm_space_t *space, uintptr_t uva,
                               const void *src, size_t size)
{
    const uint8_t *in = (const uint8_t *)src;
    size_t         copied = 0U;

    while (copied < size) {
        uintptr_t cur = uva + copied;
        size_t    off = (size_t)(cur & (PAGE_SIZE - 1ULL));
        size_t    chunk = PAGE_SIZE - off;
        void     *dst;

        if (chunk > size - copied)
            chunk = size - copied;

        if (mmu_space_prepare_write(space, cur, chunk) < 0)
            return -EFAULT;
        dst = mmu_space_resolve_ptr(space, cur, chunk);
        if (!dst)
            return -EFAULT;
        memcpy(dst, in + copied, chunk);
        copied += chunk;
    }

    return 0;
}

static void signal_reset_actions(signal_state_t *st)
{
    for (uint32_t i = 0U; i < ENLILOS_NSIG; i++) {
        st->actions[i].sa_handler = SIG_DFL;
        st->actions[i].sa_mask    = 0ULL;
        st->actions[i].sa_flags   = 0U;
        st->actions[i]._pad       = 0U;
    }
}

static int signal_pick_pending(signal_state_t *st)
{
    uint64_t ready;

    if (!st)
        return 0;

    ready = st->pending & ~(signal_strip_unmaskable(st->blocked));
    ready |= st->pending & signal_unmaskable_bits();
    if (ready == 0ULL)
        return 0;

    return 1 + __builtin_ctzll(ready);
}

static void signal_stop_current(signal_state_t *st)
{
    if (!current_task || !st)
        return;

    st->stopped = 1U;
    current_task->state = TCB_STATE_BLOCKED;
    schedule();
    while (1)
        __asm__ volatile("wfe");
}

static void signal_terminate_current(void)
{
    if (!current_task)
        return;

    mreact_task_cleanup(current_task);
    signal_task_exit(current_task);
    current_task->state = TCB_STATE_ZOMBIE;
    schedule();
    while (1)
        __asm__ volatile("wfe");
}

static int signal_install_user_frame(signal_state_t *st, exception_frame_t *frame,
                                     int sig, const sigaction_t *act)
{
    signal_user_frame_t sigframe;
    mm_space_t         *space;
    uintptr_t           sig_sp;
    uint64_t            old_mask;

    if (!st || !frame || !act || act->sa_handler == SIG_DFL || act->sa_handler == SIG_IGN)
        return -EINVAL;

    space = sched_task_space(current_task);
    if (!space)
        return -EFAULT;

    sig_sp = (frame->sp - sizeof(sigframe)) & ~0xFULL;
    if (sig_sp < MMU_USER_BASE || sig_sp >= MMU_USER_LIMIT)
        return -EFAULT;

    old_mask = st->blocked;
    sigframe.magic         = SIGFRAME_MAGIC;
    sigframe.saved_mask    = old_mask;
    sigframe.delivered_sig = (uint64_t)(uint32_t)sig;
    sigframe._pad0         = 0ULL;
    sigframe.saved_frame   = *frame;

    if (signal_copy_to_user(space, sig_sp, &sigframe, sizeof(sigframe)) < 0)
        return -EFAULT;

    st->blocked = signal_strip_unmaskable(old_mask | act->sa_mask);
    if ((act->sa_flags & SA_NODEFER) == 0U)
        st->blocked |= signal_bit(sig);
    st->in_handler = 1U;
    st->active_sig = (uint8_t)sig;

    if (act->sa_flags & SA_RESETHAND)
        st->actions[sig - 1].sa_handler = SIG_DFL;

    frame->sp   = sig_sp;
    frame->pc   = (uint64_t)(uintptr_t)act->sa_handler;
    frame->x[0] = (uint64_t)(uint32_t)sig;
    frame->x[1] = 0ULL;
    frame->x[2] = 0ULL;
    frame->x[30] = MMU_USER_SIGTRAMP_VA;
    return 0;
}

void signal_init(void)
{
    memset(signal_state, 0, sizeof(signal_state));
    uart_puts("[SIGNAL] Core POSIX minimale pronto\n");
}

void signal_task_init(sched_tcb_t *task, uint32_t parent_pid)
{
    signal_state_t *st;

    if (!task)
        return;

    st = &signal_state[signal_slot_task(task)];
    memset(st, 0, sizeof(*st));
    st->parent_pid = parent_pid;
    signal_reset_actions(st);
}

void signal_task_fork(sched_tcb_t *child, const sched_tcb_t *parent)
{
    signal_state_t       *dst;
    const signal_state_t *src;

    if (!child || !parent)
        return;

    src = &signal_state[signal_slot_task(parent)];
    dst = &signal_state[signal_slot_task(child)];
    *dst = *src;
    dst->pending     = 0ULL;
    dst->in_handler  = 0U;
    dst->stopped     = 0U;
    dst->active_sig  = 0U;
    dst->parent_pid  = parent->pid;
}

void signal_task_reset_for_exec(sched_tcb_t *task)
{
    signal_state_t *st;
    uint64_t        blocked;
    uint32_t        parent_pid;

    if (!task)
        return;

    st = &signal_state[signal_slot_task(task)];
    blocked = st->blocked;
    parent_pid = st->parent_pid;
    memset(st, 0, sizeof(*st));
    st->blocked = signal_strip_unmaskable(blocked);
    st->parent_pid = parent_pid;
    signal_reset_actions(st);
}

void signal_task_exit(sched_tcb_t *task)
{
    signal_state_t *st;

    if (!task)
        return;

    st = &signal_state[signal_slot_task(task)];
    st->pending = 0ULL;
    st->in_handler = 0U;
    st->stopped = 0U;
    if (st->parent_pid != 0U)
        (void)signal_send_pid(st->parent_pid, SIGCHLD);
}

int signal_send_pid(uint32_t pid, int sig)
{
    sched_tcb_t    *task;
    signal_state_t *st;

    if (!signal_valid(sig))
        return -EINVAL;

    task = sched_task_find(pid);
    if (!task)
        return -ESRCH;
    if (!sched_task_is_user(task))
        return -EPERM;
    if (task->state == TCB_STATE_ZOMBIE)
        return -ESRCH;

    st = &signal_state[signal_slot_task(task)];
    if (sig == SIGCONT) {
        st->pending &= ~signal_bit(SIGSTOP);
        st->stopped = 0U;
        if (task->state == TCB_STATE_BLOCKED)
            sched_unblock(task);
        return 0;
    }

    st->pending |= signal_bit(sig);
    if (sig == SIGKILL && st->stopped && task->state == TCB_STATE_BLOCKED) {
        st->stopped = 0U;
        sched_unblock(task);
    }
    return 0;
}

int signal_has_unblocked_pending(const sched_tcb_t *task)
{
    const signal_state_t *st;

    if (!task || !sched_task_is_user(task))
        return 0;

    st = &signal_state[signal_slot_task(task)];
    return signal_pick_pending((signal_state_t *)st) != 0;
}

int signal_deliver_pending(exception_frame_t *frame)
{
    signal_state_t *st;

    if (!current_task || !frame || !sched_task_is_user(current_task))
        return 0;

    st = &signal_state[signal_slot_task(current_task)];
    if (st->in_handler)
        return 0;

    while (1) {
        int         sig = signal_pick_pending(st);
        sigaction_t act;

        if (sig == 0)
            return 0;

        st->pending &= ~signal_bit(sig);

        if (sig == SIGCONT) {
            st->stopped = 0U;
            continue;
        }

        act = st->actions[sig - 1];
        if (act.sa_handler == SIG_IGN || (act.sa_handler == SIG_DFL && signal_default_ignore(sig)))
            continue;

        if (act.sa_handler == SIG_DFL) {
            if (signal_default_stop(sig))
                signal_stop_current(st);
            if (signal_default_terminate(sig))
                signal_terminate_current();
            continue;
        }

        if (signal_install_user_frame(st, frame, sig, &act) < 0) {
            signal_terminate_current();
        }
        return 1;
    }
}

int signal_handle_user_exception(exception_frame_t *frame, uint32_t ec)
{
    int sig;

    if (!current_task || !sched_task_is_user(current_task) || !frame)
        return -1;

    switch (ec) {
    case EC_IABT_LOWER:
        sig = SIGSEGV;
        break;
    case EC_DABT_LOWER:
        sig = SIGSEGV;
        break;
    case EC_FP_AA64:
        sig = SIGFPE;
        break;
    case EC_PC_ALIGN:
    case EC_SP_ALIGN:
        sig = SIGBUS;
        break;
    case EC_UNKNOWN:
    case EC_BRK_AA64:
    default:
        sig = SIGILL;
        break;
    }

    (void)signal_send_pid(current_task->pid, sig);
    (void)signal_deliver_pending(frame);
    return 0;
}

int signal_sigaction_current(int sig, const sigaction_t *act, sigaction_t *old)
{
    signal_state_t *st;

    if (!current_task || !sched_task_is_user(current_task))
        return -EPERM;
    if (!signal_valid(sig))
        return -EINVAL;

    st = &signal_state[signal_slot_task(current_task)];
    if (old)
        *old = st->actions[sig - 1];

    if (!act)
        return 0;

    if (sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;

    st->actions[sig - 1] = *act;
    st->actions[sig - 1].sa_mask = signal_strip_unmaskable(act->sa_mask);
    st->actions[sig - 1]._pad = 0U;
    return 0;
}

int signal_sigprocmask_current(int how, const uint64_t *set, uint64_t *old)
{
    signal_state_t *st;

    if (!current_task || !sched_task_is_user(current_task))
        return -EPERM;

    st = &signal_state[signal_slot_task(current_task)];
    if (old)
        *old = st->blocked;
    if (!set)
        return 0;

    switch (how) {
    case SIG_BLOCK:
        st->blocked |= signal_strip_unmaskable(*set);
        break;
    case SIG_UNBLOCK:
        st->blocked &= ~signal_strip_unmaskable(*set);
        break;
    case SIG_SETMASK:
        st->blocked = signal_strip_unmaskable(*set);
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

int signal_sigreturn_current(exception_frame_t *frame)
{
    signal_state_t      *st;
    signal_user_frame_t  sigframe;
    mm_space_t          *space;

    if (!current_task || !frame || !sched_task_is_user(current_task))
        return -EPERM;

    st = &signal_state[signal_slot_task(current_task)];
    if (!st->in_handler)
        return -EINVAL;

    space = sched_task_space(current_task);
    if (!space)
        return -EFAULT;
    if (signal_copy_from_user(space, frame->sp, &sigframe, sizeof(sigframe)) < 0)
        return -EFAULT;
    if (sigframe.magic != SIGFRAME_MAGIC)
        return -EINVAL;

    st->blocked = signal_strip_unmaskable(sigframe.saved_mask);
    st->in_handler = 0U;
    st->active_sig = 0U;
    *frame = sigframe.saved_frame;
    return 0;
}
