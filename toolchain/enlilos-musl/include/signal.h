#ifndef ENLILOS_MUSL_SIGNAL_H
#define ENLILOS_MUSL_SIGNAL_H

#include <sys/types.h>

typedef unsigned long long sigset_t;
typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;

struct sigaction {
    sighandler_t sa_handler;
    sigset_t     sa_mask;
    unsigned int sa_flags;
    unsigned int __pad;
};

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

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
#define SIGWINCH    28

#define SA_RESTART   (1U << 0)
#define SA_NODEFER   (1U << 1)
#define SA_RESETHAND (1U << 2)

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

static inline int sigemptyset(sigset_t *set)
{
    if (!set)
        return -1;
    *set = 0ULL;
    return 0;
}

static inline int sigfillset(sigset_t *set)
{
    if (!set)
        return -1;
    *set = ~0ULL;
    return 0;
}

static inline int sigaddset(sigset_t *set, int sig)
{
    if (!set || sig <= 0 || sig > 64)
        return -1;
    *set |= (1ULL << (unsigned)(sig - 1));
    return 0;
}

static inline int sigdelset(sigset_t *set, int sig)
{
    if (!set || sig <= 0 || sig > 64)
        return -1;
    *set &= ~(1ULL << (unsigned)(sig - 1));
    return 0;
}

static inline int sigismember(const sigset_t *set, int sig)
{
    if (!set || sig <= 0 || sig > 64)
        return -1;
    return ((*set & (1ULL << (unsigned)(sig - 1))) != 0ULL) ? 1 : 0;
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old);
int sigprocmask(int how, const sigset_t *set, sigset_t *old);
int kill(pid_t pid, int sig);
sighandler_t signal(int sig, sighandler_t handler);

#endif
