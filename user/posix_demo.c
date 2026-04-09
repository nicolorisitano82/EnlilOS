/*
 * EnlilOS user-space demo - M8-08a/b/c
 *
 * Verifica:
 *   - envp bootstrap + getcwd/chdir
 *   - pipe() + dup() + dup2()
 *   - tcgetattr/tcsetattr/isatty
 */

#include "syscall.h"
#include "termios.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef signed int     s32;

#define POSIX_MSG "pipe-through-stdout\n"

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

static long sys_write_fd(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_read_fd(long fd, void *buf, u64 len)
{
    return sys_call3(SYS_READ, fd, (long)buf, (long)len);
}

static long sys_close_fd(long fd)
{
    return sys_call1(SYS_CLOSE, fd);
}

static long sys_pipe_now(int fdv[2])
{
    return sys_call1(SYS_PIPE, (long)fdv);
}

static long sys_dup_now(long fd)
{
    return sys_call1(SYS_DUP, fd);
}

static long sys_dup2_now(long oldfd, long newfd)
{
    return sys_call2(SYS_DUP2, oldfd, newfd);
}

static long sys_fork_now(void)
{
    return sys_call1(SYS_FORK, 0);
}

static long sys_waitpid_now(long pid, int *status, u32 options, u64 timeout_ms)
{
    return sys_call4(SYS_WAITPID, pid, (long)status, options, (long)timeout_ms);
}

static long sys_chdir_now(const char *path)
{
    return sys_call1(SYS_CHDIR, (long)path);
}

static long sys_getcwd_now(char *buf, u32 size)
{
    return sys_call2(SYS_GETCWD, (long)buf, size);
}

static long sys_tcgetattr_now(int fd, termios_t *t)
{
    return sys_call2(SYS_TCGETATTR, fd, (long)t);
}

static long sys_tcsetattr_now(int fd, int action, const termios_t *t)
{
    return sys_call3(SYS_TCSETATTR, fd, action, (long)t);
}

static long sys_isatty_now(int fd)
{
    return sys_call1(SYS_ISATTY, fd);
}

static u32 demo_strlen(const char *s)
{
    u32 n = 0U;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static int demo_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int demo_startswith(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static int demo_env_has(const char *const *envp, const char *prefix)
{
    u32 i = 0U;

    if (!envp || !prefix)
        return 0;

    while (envp[i] != (const char *)0) {
        if (demo_startswith(envp[i], prefix))
            return 1;
        i++;
    }
    return 0;
}

static void demo_log(const char *msg)
{
    (void)sys_write_fd(1, msg, demo_strlen(msg));
}

static int demo_check_env(const char *const *envp)
{
    return demo_env_has(envp, "PATH=") &&
           demo_env_has(envp, "HOME=") &&
           demo_env_has(envp, "PWD=") &&
           demo_env_has(envp, "TERM=") &&
           demo_env_has(envp, "USER=");
}

static int demo_pipe_dup_termios(const char *const *envp)
{
    char      cwd[64];
    char      buf[64];
    int       fds[2];
    int       status = 0;
    int       dupfd;
    termios_t saved;
    termios_t raw;
    termios_t check;
    long      pid;
    long      n;

    if (!demo_check_env(envp))
        return 1;

    if (sys_getcwd_now(cwd, sizeof(cwd)) < 0 || !demo_streq(cwd, "/"))
        return 2;
    if (sys_chdir_now("/dev") < 0)
        return 3;
    if (sys_getcwd_now(cwd, sizeof(cwd)) < 0 || !demo_streq(cwd, "/dev"))
        return 4;
    if (sys_chdir_now("/") < 0)
        return 5;

    if (sys_isatty_now(0) != 1 || sys_isatty_now(1) != 1)
        return 6;

    dupfd = (int)sys_dup_now(0);
    if (dupfd < 3 || sys_isatty_now(dupfd) != 1)
        return 7;
    if (sys_close_fd(dupfd) < 0)
        return 8;

    if (sys_tcgetattr_now(0, &saved) < 0)
        return 9;
    raw = saved;
    raw.c_lflag &= (uint32_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1U;
    raw.c_cc[VTIME] = 0U;
    if (sys_tcsetattr_now(0, TCSANOW, &raw) < 0)
        return 10;
    if (sys_tcgetattr_now(0, &check) < 0)
        return 11;
    if ((check.c_lflag & (ICANON | ECHO)) != 0U)
        return 12;
    if (sys_tcsetattr_now(0, TCSANOW, &saved) < 0)
        return 13;

    if (sys_pipe_now(fds) < 0)
        return 14;
    if (sys_isatty_now(fds[0]) != 0 || sys_isatty_now(fds[1]) != 0)
        return 15;

    dupfd = (int)sys_dup_now(fds[0]);
    if (dupfd < 3)
        return 16;
    if (sys_close_fd(fds[0]) < 0)
        return 17;

    pid = sys_fork_now();
    if (pid < 0)
        return 18;

    if (pid == 0) {
        if (sys_close_fd(dupfd) < 0)
            sys_exit_now(30);
        if (sys_dup2_now(fds[1], 1) < 0)
            sys_exit_now(31);
        if (sys_close_fd(fds[1]) < 0)
            sys_exit_now(32);
        if (sys_write_fd(1, POSIX_MSG, demo_strlen(POSIX_MSG)) != (long)demo_strlen(POSIX_MSG))
            sys_exit_now(33);
        sys_exit_now(0);
    }

    if (sys_close_fd(fds[1]) < 0)
        return 19;

    n = sys_read_fd(dupfd, buf, sizeof(buf));
    if (n != (long)demo_strlen(POSIX_MSG))
        return 20;
    buf[(u32)n < sizeof(buf) ? (u32)n : (sizeof(buf) - 1U)] = '\0';
    if (!demo_streq(buf, POSIX_MSG))
        return 21;

    if (sys_waitpid_now(pid, &status, 0U, 2000ULL) != pid)
        return 22;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 23;

    n = sys_read_fd(dupfd, buf, sizeof(buf));
    if (n != 0)
        return 24;
    if (sys_close_fd(dupfd) < 0)
        return 25;

    demo_log("[POSIX] pipe/dup/cwd/termios OK\n");
    return 0;
}

void _start(long argc, const char **argv, const char **envp, const void *auxv)
{
    (void)argc;
    (void)argv;
    (void)auxv;
    sys_exit_now(demo_pipe_dup_termios(envp));
}
