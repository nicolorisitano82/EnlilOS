#include "signal.h"
#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef signed long    s64;

static volatile u64 g_cont_seen;
static volatile u64 g_term_seen;

static long sys_call0(long nr)
{
    return user_svc0(nr);
}

static long sys_call1(long nr, long a0)
{
    return user_svc1(nr, a0);
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

static long sys_fork_now(void)
{
    return sys_call0(SYS_FORK);
}

static long sys_yield_now(void)
{
    return sys_call0(SYS_YIELD);
}

static long sys_kill_now(long pid, int sig)
{
    return sys_call2(SYS_KILL, pid, sig);
}

static long sys_sigaction_now(int sig, const sigaction_t *act)
{
    return sys_call3(SYS_SIGACTION, sig, (long)act, 0);
}

static long sys_waitpid_raw(long pid, int *status, uint32_t options, u64 timeout_ms)
{
    return sys_call4(SYS_WAITPID, pid, (long)status, options, (long)timeout_ms);
}

static long sys_setpgid_now(long pid, long pgid)
{
    return sys_call2(SYS_SETPGID, pid, pgid);
}

static long sys_getpgid_now(long pid)
{
    return sys_call1(SYS_GETPGID, pid);
}

static long sys_setsid_now(void)
{
    return sys_call0(SYS_SETSID);
}

static long sys_getsid_now(long pid)
{
    return sys_call1(SYS_GETSID, pid);
}

static long sys_tcsetpgrp_now(uint32_t pgid)
{
    return sys_call1(SYS_TCSETPGRP, (long)pgid);
}

static long sys_tcgetpgrp_now(void)
{
    return sys_call0(SYS_TCGETPGRP);
}

static u64 demo_strlen(const char *s)
{
    u64 n = 0ULL;

    while (s && s[n] != '\0')
        n++;
    return n;
}

static void demo_puts(const char *text)
{
    (void)sys_write_fd(1, text, demo_strlen(text));
}

static long demo_write_line(const char *path, u64 flags, const char *text)
{
    long fd = sys_open_path(path, flags);

    if (fd < 0)
        return fd;
    if (sys_write_fd(fd, text, demo_strlen(text)) < 0) {
        (void)sys_close_fd(fd);
        return -1;
    }
    if (sys_close_fd(fd) < 0)
        return -1;
    return 0;
}

static void on_sigcont(int sig)
{
    (void)sig;
    g_cont_seen = 1ULL;
}

static void on_sigterm(int sig)
{
    (void)sig;
    g_term_seen = 1ULL;
}

int main(void)
{
    static const char result_path[] = "/data/JOBCTRL.TXT";
    static const char ready_path[]  = "/data/JOBREADY.TXT";
    static const char cont_path[]   = "/data/JOBCONT.TXT";
    sigaction_t       act;
    long              leader_sid;
    long              leader_pgid;
    long              helper_pid;
    long              sid;
    long              pid;
    int               status = 0;

    leader_sid = sys_getsid_now(0);
    leader_pgid = sys_getpgid_now(0);
    if (leader_sid <= 0 || leader_pgid <= 0)
        sys_exit_now(1);
    if (leader_sid != leader_pgid)
        sys_exit_now(2);
    if (sys_setsid_now() >= 0)
        sys_exit_now(3);

    helper_pid = sys_fork_now();
    if (helper_pid < 0)
        sys_exit_now(4);
    if (helper_pid == 0) {
        sid = sys_setsid_now();
        if (sid < 0)
            sys_exit_now(5);
        if (sys_getsid_now(0) != sid || sys_getpgid_now(0) != sid)
            sys_exit_now(6);
        sys_exit_now(0);
    }
    if (sys_waitpid_raw(helper_pid, &status, 0U, 2000ULL) != helper_pid)
        sys_exit_now(7);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        sys_exit_now(8);

    if (sys_tcsetpgrp_now((uint32_t)leader_pgid) < 0)
        sys_exit_now(9);
    if (sys_tcgetpgrp_now() != leader_pgid)
        sys_exit_now(10);

    pid = sys_fork_now();
    if (pid < 0)
        sys_exit_now(11);

    if (pid == 0) {
        u64 cont_logged = 0ULL;

        if (sys_setpgid_now(0, 0) < 0)
            sys_exit_now(20);
        if (sys_getpgid_now(0) <= 0)
            sys_exit_now(21);

        act.sa_handler = on_sigcont;
        act.sa_mask = 0ULL;
        act.sa_flags = 0U;
        act._pad = 0U;
        if (sys_sigaction_now(SIGCONT, &act) < 0)
            sys_exit_now(22);

        act.sa_handler = on_sigterm;
        act.sa_mask = 0ULL;
        act.sa_flags = 0U;
        act._pad = 0U;
        if (sys_sigaction_now(SIGTERM, &act) < 0)
            sys_exit_now(23);

        if (demo_write_line(ready_path, O_CREAT | O_TRUNC | O_WRONLY, "ready\n") < 0)
            sys_exit_now(24);

        while (!g_term_seen) {
            if (g_cont_seen && !cont_logged) {
                if (demo_write_line(result_path, O_CREAT | O_WRONLY | O_APPEND,
                                    "child-cont\n") < 0)
                    sys_exit_now(25);
                if (demo_write_line(cont_path, O_CREAT | O_TRUNC | O_WRONLY,
                                    "cont\n") < 0)
                    sys_exit_now(26);
                cont_logged = 1ULL;
            }
            (void)sys_yield_now();
        }

        if (demo_write_line(result_path, O_CREAT | O_WRONLY | O_APPEND,
                            "child-term\n") < 0)
            sys_exit_now(27);
        sys_exit_now(0);
    }

    if (sys_setpgid_now(pid, pid) < 0)
        sys_exit_now(30);
    if (sys_getpgid_now(pid) != pid)
        sys_exit_now(31);
    if (sys_getsid_now(pid) != leader_sid)
        sys_exit_now(32);

    for (;;) {
        long fd = sys_open_path(ready_path, O_RDONLY);

        if (fd >= 0) {
            (void)sys_close_fd(fd);
            break;
        }
        (void)sys_yield_now();
    }

    if (sys_tcsetpgrp_now((uint32_t)pid) < 0)
        sys_exit_now(33);
    if (sys_tcgetpgrp_now() != pid)
        sys_exit_now(34);
    if (sys_kill_now(pid, SIGTSTP) < 0)
        sys_exit_now(35);

    if (sys_waitpid_raw(pid, &status, WUNTRACED, 2000ULL) != pid)
        sys_exit_now(36);
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGTSTP)
        sys_exit_now(37);

    if (sys_tcsetpgrp_now((uint32_t)leader_pgid) < 0)
        sys_exit_now(38);
    if (sys_tcgetpgrp_now() != leader_pgid)
        sys_exit_now(39);

    if (sys_kill_now(-pid, SIGCONT) < 0)
        sys_exit_now(40);

    for (;;) {
        long fd = sys_open_path(cont_path, O_RDONLY);

        if (fd >= 0) {
            (void)sys_close_fd(fd);
            break;
        }
        (void)sys_yield_now();
    }

    if (sys_kill_now(-pid, SIGTERM) < 0)
        sys_exit_now(41);
    if (sys_waitpid_raw(pid, &status, 0U, 2000ULL) != pid)
        sys_exit_now(42);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        sys_exit_now(43);

    if (demo_write_line(result_path, O_CREAT | O_WRONLY | O_APPEND,
                        "parent-ok\n") < 0)
        sys_exit_now(44);

    demo_puts("[JOB] ALL TESTS PASSED\n");
    sys_exit_now(0);
}

void _start(void)
{
    sys_exit_now(main());
}
