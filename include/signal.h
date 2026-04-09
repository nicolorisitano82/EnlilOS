/*
 * EnlilOS - Signal handling minimale (M8-03)
 */

#ifndef ENLILOS_SIGNAL_H
#define ENLILOS_SIGNAL_H

#include "exception.h"
#include "types.h"

struct sched_tcb;
typedef struct sched_tcb sched_tcb_t;

#define ENLILOS_NSIG    64

typedef void (*sighandler_t)(int);

typedef struct {
    sighandler_t sa_handler;
    uint64_t     sa_mask;
    uint32_t     sa_flags;
    uint32_t     _pad;
} sigaction_t;

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19
#define SIGTSTP     20
#define SIGTTIN     21
#define SIGTTOU     22

#define SA_RESTART   (1U << 0)
#define SA_NODEFER   (1U << 1)
#define SA_RESETHAND (1U << 2)

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

void signal_init(void);
void signal_task_init(sched_tcb_t *task, uint32_t parent_pid);
void signal_task_fork(sched_tcb_t *child, const sched_tcb_t *parent);
void signal_task_clone_thread(sched_tcb_t *child, const sched_tcb_t *parent);
void signal_task_reset_for_exec(sched_tcb_t *task);
void signal_task_exit(sched_tcb_t *task);

int  signal_send_pid(uint32_t pid, int sig);
int  signal_send_tgkill(uint32_t tgid, uint32_t tid, int sig);
int  signal_send_pgrp(uint32_t pgid, int sig);
int  signal_has_unblocked_pending(const sched_tcb_t *task);
int  signal_deliver_pending(exception_frame_t *frame);
int  signal_handle_user_exception(exception_frame_t *frame, uint32_t ec);
int  signal_task_is_stopped(const sched_tcb_t *task);
int  signal_task_consume_stop_report(const sched_tcb_t *task, int *sig_out);

int  signal_sigaction_current(int sig, const sigaction_t *act, sigaction_t *old);
int  signal_sigprocmask_current(int how, const uint64_t *set, uint64_t *old);
int  signal_sigreturn_current(exception_frame_t *frame);

#endif
