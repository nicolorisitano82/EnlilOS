/*
 * EnlilOS - Signal handling minimale (M8-03)
 */

#include "signal.h"

#include "kmon.h"
#include "ksem.h"
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
    uint8_t     stop_reported;
    uint8_t     stop_sig;
    uint8_t     active_sig;
} signal_state_t;

static signal_state_t signal_state[SCHED_MAX_TASKS];

static void signal_uart_put_u32(uint32_t v)
{
    char buf[11];
    int  len = 0;

    if (v == 0U) {
        uart_putc('0');
        return;
    }

    while (v != 0U && len < (int)sizeof(buf)) {
        buf[len++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (len > 0)
        uart_putc(buf[--len]);
}

static void signal_uart_put_hex64(uint64_t v)
{
    static const char digits[] = "0123456789ABCDEF";

    uart_puts("0x");
    for (int shift = 60; shift >= 0; shift -= 4)
        uart_putc(digits[(v >> (uint32_t)shift) & 0xFULL]);
}

static sigaction_t    signal_actions[SCHED_MAX_TASKS][ENLILOS_NSIG];

static inline uint32_t signal_slot_pid(uint32_t pid)
{
    return pid % SCHED_MAX_TASKS;
}

static inline uint32_t signal_slot_task(const sched_tcb_t *task)
{
    return signal_slot_pid(task ? task->pid : 0U);
}

static inline uint32_t signal_proc_slot_task(const sched_tcb_t *task)
{
    return task ? sched_task_proc_slot(task) % SCHED_MAX_TASKS : 0U;
}

static inline sigaction_t *signal_actions_for_task(const sched_tcb_t *task)
{
    return signal_actions[signal_proc_slot_task(task)];
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
    return sig == SIGSTOP || sig == SIGTSTP || sig == SIGTTIN || sig == SIGTTOU;
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

static void signal_reset_actions(uint32_t proc_slot)
{
    for (uint32_t i = 0U; i < ENLILOS_NSIG; i++) {
        signal_actions[proc_slot][i].sa_handler = SIG_DFL;
        signal_actions[proc_slot][i].sa_mask    = 0ULL;
        signal_actions[proc_slot][i].sa_flags   = 0U;
        signal_actions[proc_slot][i]._pad       = 0U;
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

static int signal_has_deliverable_pending(const signal_state_t *st,
                                         const sched_tcb_t *task)
{
    uint64_t ready;

    if (!st)
        return 0;

    ready = st->pending & ~(signal_strip_unmaskable(st->blocked));
    ready |= st->pending & signal_unmaskable_bits();

    while (ready != 0ULL) {
        int sig = 1 + __builtin_ctzll(ready);
        sigaction_t act = signal_actions_for_task(task)[sig - 1];

        if (!(act.sa_handler == SIG_IGN ||
              (act.sa_handler == SIG_DFL && signal_default_ignore(sig))))
            return 1;
        ready &= ~signal_bit(sig);
    }

    return 0;
}

static uint64_t signal_stop_bits(void)
{
    return signal_bit(SIGSTOP) | signal_bit(SIGTSTP) |
           signal_bit(SIGTTIN) | signal_bit(SIGTTOU);
}

static void signal_stop_current(signal_state_t *st, int sig)
{
    if (!current_task || !st)
        return;

    st->stopped = 1U;
    st->stop_reported = 0U;
    st->stop_sig = (uint8_t)sig;
    if (st->parent_pid != 0U)
        (void)signal_send_pid(st->parent_pid, SIGCHLD);
    current_task->state = TCB_STATE_BLOCKED;
    schedule();
}

static void signal_terminate_current(int sig)
{
    if (!current_task)
        return;

    if (sig <= 0)
        sig = SIGTERM;
    sched_task_exit_with_code(128 + sig);
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
        signal_actions_for_task(current_task)[sig - 1].sa_handler = SIG_DFL;

    frame->sp   = sig_sp;
    frame->pc   = (uint64_t)(uintptr_t)act->sa_handler;
    frame->x[0] = (uint64_t)(uint32_t)sig;
    frame->x[1] = 0ULL;
    frame->x[2] = 0ULL;
    frame->x[30] = (sched_task_abi_mode(current_task) == SCHED_ABI_LINUX)
                   ? MMU_USER_LINUX_SIGTRAMP_VA
                   : MMU_USER_SIGTRAMP_VA;
    return 0;
}

void signal_init(void)
{
    memset(signal_state, 0, sizeof(signal_state));
    memset(signal_actions, 0, sizeof(signal_actions));
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
    signal_reset_actions(signal_proc_slot_task(task));
}

void signal_task_fork(sched_tcb_t *child, const sched_tcb_t *parent)
{
    signal_state_t       *dst;
    const signal_state_t *src;
    sigaction_t          *child_actions;
    sigaction_t          *parent_actions;

    if (!child || !parent)
        return;

    src = &signal_state[signal_slot_task(parent)];
    dst = &signal_state[signal_slot_task(child)];
    *dst = *src;
    dst->pending     = 0ULL;
    dst->in_handler  = 0U;
    dst->stopped     = 0U;
    dst->stop_reported = 0U;
    dst->stop_sig  = 0U;
    dst->active_sig  = 0U;
    dst->parent_pid  = sched_task_tgid(parent);
    child_actions = signal_actions_for_task(child);
    parent_actions = signal_actions_for_task(parent);
    memcpy(child_actions, parent_actions, sizeof(signal_actions[0]));
}

void signal_task_clone_thread(sched_tcb_t *child, const sched_tcb_t *parent)
{
    signal_state_t       *dst;
    const signal_state_t *src;

    if (!child || !parent)
        return;

    src = &signal_state[signal_slot_task(parent)];
    dst = &signal_state[signal_slot_task(child)];
    memset(dst, 0, sizeof(*dst));
    dst->blocked    = src->blocked;
    dst->parent_pid = src->parent_pid;
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
    signal_reset_actions(signal_proc_slot_task(task));
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
    st->stop_reported = 0U;
    st->stop_sig = 0U;
}

static int signal_send_task(sched_tcb_t *task, int sig)
{
    signal_state_t *st;

    if (!signal_valid(sig))
        return -EINVAL;
    if (!task)
        return -ESRCH;
    if (!sched_task_is_user(task))
        return -EPERM;
    if (task->state == TCB_STATE_ZOMBIE)
        return -ESRCH;

    st = &signal_state[signal_slot_task(task)];
    if (sig == SIGCONT) {
        st->pending &= ~signal_stop_bits();
        st->stopped = 0U;
        st->stop_reported = 0U;
        st->stop_sig = 0U;
        if (task->state == TCB_STATE_BLOCKED)
            sched_unblock(task);
        st->pending |= signal_bit(SIGCONT);
        if (st->parent_pid != 0U)
            (void)signal_send_pid(st->parent_pid, SIGCHLD);
        return 0;
    }

    st->pending |= signal_bit(sig);
    if (sig == SIGKILL && st->stopped && task->state == TCB_STATE_BLOCKED) {
        st->stopped = 0U;
        sched_unblock(task);
    }
    return 0;
}

int signal_send_pgrp(uint32_t pgid, int sig)
{
    int delivered = 0;

    if (pgid == 0U || !signal_valid(sig))
        return -EINVAL;

    for (uint32_t i = 0U; i < sched_task_count_total(); i++) {
        sched_tcb_t *task = sched_task_at(i);

        if (!task || !sched_task_is_user(task))
            continue;
        if (sched_task_pgid(task) != pgid)
            continue;
        if (task->state == TCB_STATE_ZOMBIE)
            continue;
        if (signal_send_task(task, sig) == 0)
            delivered = 1;
    }

    return delivered ? 0 : -ESRCH;
}

int signal_has_unblocked_pending(const sched_tcb_t *task)
{
    const signal_state_t *st;

    if (!task || !sched_task_is_user(task))
        return 0;

    st = &signal_state[signal_slot_task(task)];
    return signal_has_deliverable_pending(st, task);
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

        act = signal_actions_for_task(current_task)[sig - 1];
        if (act.sa_handler == SIG_IGN || (act.sa_handler == SIG_DFL && signal_default_ignore(sig)))
            continue;

        if (act.sa_handler == SIG_DFL) {
            if (sig == SIGCONT) {
                st->stopped = 0U;
                st->stop_reported = 0U;
                st->stop_sig = 0U;
                continue;
            }
            if (signal_default_stop(sig))
                signal_stop_current(st, sig);
            if (signal_default_terminate(sig))
                signal_terminate_current(sig);
            continue;
        }

        if (signal_install_user_frame(st, frame, sig, &act) < 0) {
            signal_terminate_current(sig);
        }
        return 1;
    }
}

int signal_handle_user_exception(exception_frame_t *frame, uint32_t ec)
{
    int sig;
    const char *path;

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

    path = sched_task_exec_path(current_task);
    uart_puts("[SIGNAL] user fault pid=");
    signal_uart_put_u32(current_task->pid);
    uart_puts(" sig=");
    signal_uart_put_u32((uint32_t)sig);
    uart_puts(" ec=");
    signal_uart_put_hex64((uint64_t)ec);
    uart_puts(" pc=");
    signal_uart_put_hex64(frame->pc);
    uart_puts(" lr=");
    signal_uart_put_hex64(frame->x[30]);
    uart_puts(" far=");
    signal_uart_put_hex64(frame->far);
    uart_puts(" x0=");
    signal_uart_put_hex64(frame->x[0]);
    uart_puts(" x1=");
    signal_uart_put_hex64(frame->x[1]);
    uart_puts(" x2=");
    signal_uart_put_hex64(frame->x[2]);
    uart_puts(" x3=");
    signal_uart_put_hex64(frame->x[3]);
    uart_puts(" path=");
    uart_puts((path && path[0] != '\0') ? path : "(unknown)");
    uart_puts("\n");

    (void)signal_send_tgkill(sched_task_tgid(current_task), current_task->pid, sig);
    (void)signal_deliver_pending(frame);
    return 0;
}

int signal_task_is_stopped(const sched_tcb_t *task)
{
    const signal_state_t *st;

    if (!task)
        return 0;
    st = &signal_state[signal_slot_task(task)];
    return st->stopped ? 1 : 0;
}

int signal_task_consume_stop_report(const sched_tcb_t *task, int *sig_out)
{
    signal_state_t *st;

    if (!task)
        return 0;

    st = &signal_state[signal_slot_task(task)];
    if (!st->stopped || st->stop_reported)
        return 0;

    st->stop_reported = 1U;
    if (sig_out)
        *sig_out = st->stop_sig ? st->stop_sig : SIGSTOP;
    return 1;
}

int signal_sigaction_current(int sig, const sigaction_t *act, sigaction_t *old)
{
    if (!current_task || !sched_task_is_user(current_task))
        return -EPERM;
    if (!signal_valid(sig))
        return -EINVAL;
    if (old)
        *old = signal_actions_for_task(current_task)[sig - 1];

    if (!act)
        return 0;

    if (sig == SIGKILL || sig == SIGSTOP)
        return -EINVAL;

    signal_actions_for_task(current_task)[sig - 1] = *act;
    signal_actions_for_task(current_task)[sig - 1].sa_mask = signal_strip_unmaskable(act->sa_mask);
    signal_actions_for_task(current_task)[sig - 1]._pad = 0U;
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

static sched_tcb_t *signal_find_process_target(uint32_t tgid)
{
    sched_tcb_t *leader;

    if (tgid == 0U)
        return NULL;

    leader = sched_task_find(tgid);
    if (leader && sched_task_is_user(leader) && leader->state != TCB_STATE_ZOMBIE &&
        sched_task_tgid(leader) == tgid)
        return leader;

    for (uint32_t i = 0U; i < sched_task_count_total(); i++) {
        sched_tcb_t *cur = sched_task_at(i);

        if (!cur || !sched_task_is_user(cur) || cur->state == TCB_STATE_ZOMBIE)
            continue;
        if (sched_task_tgid(cur) != tgid)
            continue;
        return cur;
    }

    return NULL;
}

int signal_send_pid(uint32_t pid, int sig)
{
    return signal_send_task(signal_find_process_target(pid), sig);
}

int signal_send_tgkill(uint32_t tgid, uint32_t tid, int sig)
{
    sched_tcb_t *task;

    if (!signal_valid(sig))
        return -EINVAL;
    if (tgid == 0U || tid == 0U)
        return -EINVAL;

    task = sched_task_find(tid);
    if (!task || !sched_task_is_user(task) || task->state == TCB_STATE_ZOMBIE)
        return -ESRCH;
    if (sched_task_tgid(task) != tgid)
        return -ESRCH;
    return signal_send_task(task, sig);
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
