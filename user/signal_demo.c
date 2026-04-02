#include "syscall.h"

typedef unsigned long  u64;
typedef signed long    s64;

static volatile u64 g_term_seen;
static volatile u64 g_chld_seen;

static long sys_call0(long nr)
{
    register long x0 asm("x0");
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "=r"(x0)
                 : "r"(x8)
                 : "memory", "x1", "x2", "x3", "x4", "x5");
    return x0;
}

static long sys_call2(long nr, long a0, long a1)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x8)
                 : "memory");
    return x0;
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x8)
                 : "memory");
    return x0;
}

static long sys_call4(long nr, long a0, long a1, long a2, long a3)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                 : "memory");
    return x0;
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    register long x0 asm("x0") = code;
    register long x8 asm("x8") = SYS_EXIT;

    asm volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    for (;;)
        asm volatile("wfe");
}

static long sys_open_path(const char *path, u64 flags)
{
    return sys_call3(SYS_OPEN, (long)path, (long)flags, 0);
}

static long sys_close_fd(long fd)
{
    return sys_call3(SYS_CLOSE, fd, 0, 0);
}

static long sys_write_fd(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_wait_pid(long pid)
{
    return sys_call4(SYS_WAITPID, pid, 0, 0, 0);
}

static long sys_fork_now(void)
{
    return sys_call0(SYS_FORK);
}

static long sys_sigaction_now(int sig, const sigaction_t *act)
{
    return sys_call3(SYS_SIGACTION, sig, (long)act, 0);
}

static long sys_kill_now(long pid, int sig)
{
    return sys_call2(SYS_KILL, pid, sig);
}

static u64 demo_strlen(const char *s)
{
    u64 n = 0ULL;

    while (s && s[n] != '\0')
        n++;
    return n;
}

static void demo_write_line(const char *path, u64 flags, const char *text)
{
    long fd = sys_open_path(path, flags);

    if (fd < 0)
        return;
    (void)sys_write_fd(fd, text, demo_strlen(text));
    (void)sys_close_fd(fd);
}

static void on_sigterm(int sig)
{
    (void)sig;
    g_term_seen = 1ULL;
}

static void on_sigchld(int sig)
{
    (void)sig;
    g_chld_seen = 1ULL;
}

int main(void)
{
    sigaction_t act;
    long        pid;
    long        ready_fd;

    act.sa_handler = on_sigchld;
    act.sa_mask = 0ULL;
    act.sa_flags = 0U;
    act._pad = 0U;
    if (sys_sigaction_now(SIGCHLD, &act) < 0)
        sys_exit_now(1);

    pid = sys_fork_now();
    if (pid < 0)
        sys_exit_now(2);

    if (pid == 0) {
        act.sa_handler = on_sigterm;
        act.sa_mask = 0ULL;
        act.sa_flags = 0U;
        act._pad = 0U;
        if (sys_sigaction_now(SIGTERM, &act) < 0)
            sys_exit_now(3);

        demo_write_line("/data/SIGREADY.TXT", O_CREAT | O_TRUNC | O_WRONLY, "ready\n");
        while (!g_term_seen)
            asm volatile("" ::: "memory");

        demo_write_line("/data/SIGNAL.TXT", O_CREAT | O_WRONLY | O_APPEND, "child-term\n");
        sys_exit_now(0);
    }

    do {
        ready_fd = sys_open_path("/data/SIGREADY.TXT", O_RDONLY);
        if (ready_fd >= 0)
            (void)sys_close_fd(ready_fd);
    } while (ready_fd < 0);

    if (sys_kill_now(pid, SIGTERM) < 0)
        sys_exit_now(4);

    while (!g_chld_seen)
        asm volatile("" ::: "memory");

    if (sys_wait_pid(pid) < 0)
        sys_exit_now(5);

    demo_write_line("/data/SIGNAL.TXT", O_CREAT | O_WRONLY | O_APPEND, "parent-chld\n");
    sys_exit_now(0);
}

void _start(void)
{
    sys_exit_now(main());
}
