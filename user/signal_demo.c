#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef signed long    s64;

static volatile u64 g_term_seen;
static volatile u64 g_chld_seen;

static long sys_call0(long nr)
{
    return user_svc0(nr);
}

static long sys_call2(long nr, long a0, long a1)
{
    return user_svc2(nr, a0, a1);
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    return user_svc3(nr, a0, a1, a2);
}

static long sys_call4(long nr, long a0, long a1, long a2, long a3)
{
    return user_svc4(nr, a0, a1, a2, a3);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
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

static long sys_yield_now(void)
{
    return sys_call0(SYS_YIELD);
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

static long demo_write_line(const char *path, u64 flags, const char *text)
{
    long fd = sys_open_path(path, flags);

    if (fd < 0)
        return -1000 + fd;
    if (sys_write_fd(fd, text, demo_strlen(text)) < 0) {
        (void)sys_close_fd(fd);
        return -2000;
    }
    if (sys_close_fd(fd) < 0)
        return -3000;
    return 0;
}

static void demo_puts(const char *text)
{
    (void)sys_write_fd(1, text, demo_strlen(text));
}

static void demo_put_num(long value)
{
    char          buf[32];
    unsigned long mag;
    u64           len = 0ULL;

    if (value < 0) {
        demo_puts("-");
        mag = (unsigned long)(-(value + 1L)) + 1UL;
    } else {
        mag = (unsigned long)value;
    }

    if (mag == 0UL) {
        demo_puts("0");
        return;
    }

    while (mag != 0UL && len < (u64)sizeof(buf)) {
        buf[len++] = (char)('0' + (mag % 10UL));
        mag /= 10UL;
    }
    while (len > 0ULL)
        (void)sys_write_fd(1, &buf[--len], 1ULL);
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
    long        kill_rc;
    long        ready_fd;

    act.sa_handler = on_sigchld;
    act.sa_mask = 0ULL;
    act.sa_flags = 0U;
    act._pad = 0U;
    if (sys_sigaction_now(SIGCHLD, &act) < 0)
        sys_exit_now(1);
    demo_puts("[SIG] parent-armed\n");

    pid = sys_fork_now();
    if (pid < 0)
        sys_exit_now(2);

    if (pid == 0) {
        long ready_rc;

        demo_puts("[SIG] child-start\n");
        act.sa_handler = on_sigterm;
        act.sa_mask = 0ULL;
        act.sa_flags = 0U;
        act._pad = 0U;
        if (sys_sigaction_now(SIGTERM, &act) < 0)
            sys_exit_now(3);
        demo_puts("[SIG] child-armed\n");

        ready_rc = demo_write_line("/data/SIGREADY.TXT", O_CREAT | O_TRUNC | O_WRONLY, "ready\n");
        if (ready_rc == 0)
            demo_puts("[SIG] child-ready-ok\n");
        else if (ready_rc <= -1000 && ready_rc > -2000)
            demo_puts("[SIG] child-ready-open-fail\n");
        else if (ready_rc == -2000)
            demo_puts("[SIG] child-ready-write-fail\n");
        else if (ready_rc == -3000)
            demo_puts("[SIG] child-ready-close-fail\n");
        else
            demo_puts("[SIG] child-ready-fail\n");
        while (!g_term_seen)
            (void)sys_yield_now();

        demo_puts("[SIG] child-term-seen\n");
        (void)demo_write_line("/data/SIGNAL.TXT", O_CREAT | O_WRONLY | O_APPEND, "child-term\n");
        sys_exit_now(0);
    }

    do {
        ready_fd = sys_open_path("/data/SIGREADY.TXT", O_RDONLY);
        if (ready_fd >= 0)
            (void)sys_close_fd(ready_fd);
        else
            (void)sys_yield_now();
    } while (ready_fd < 0);
    demo_puts("[SIG] parent-ready\n");

    kill_rc = sys_kill_now(pid, SIGTERM);
    if (kill_rc < 0) {
        demo_puts("[SIG] parent-kill-fail\n");
        demo_puts("[SIG] parent-kill-rc=");
        demo_put_num(kill_rc);
        demo_puts("\n");
    }
    if (kill_rc < 0)
        sys_exit_now(4);
    demo_puts("[SIG] parent-kill\n");

    while (!g_chld_seen)
        (void)sys_yield_now();
    demo_puts("[SIG] parent-chld-seen\n");

    if (sys_wait_pid(pid) < 0)
        sys_exit_now(5);

    demo_write_line("/data/SIGNAL.TXT", O_CREAT | O_WRONLY | O_APPEND, "parent-chld\n");
    sys_exit_now(0);
}

void _start(void)
{
    sys_exit_now(main());
}
