/*
 * EnlilOS EL0 demo - fork() + COW verification (M8-01)
 */

#include "syscall.h"

typedef unsigned long  u64;
typedef signed long    s64;

static volatile u64 g_counter = 1ULL;

static long sys_call0(long nr)
{
    register long x0 asm("x0");
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "=r"(x0)
                 : "r"(x8)
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

static long sys_write_fd(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_open_path(const char *path, u64 flags)
{
    return sys_call3(SYS_OPEN, (long)path, (long)flags, 0);
}

static long sys_close_fd(long fd)
{
    return sys_call3(SYS_CLOSE, fd, 0, 0);
}

static long sys_fork_now(void)
{
    return sys_call0(SYS_FORK);
}

static long sys_wait_pid(long pid)
{
    return sys_call4(SYS_WAITPID, pid, 0, 0, 0);
}

static u64 demo_strlen(const char *s)
{
    u64 n = 0U;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static void demo_append(char *dst, u64 cap, const char *src)
{
    u64 len = demo_strlen(dst);
    u64 i = 0U;

    if (len >= cap - 1U)
        return;

    while (src[i] != '\0' && len + 1U < cap)
        dst[len++] = src[i++];
    dst[len] = '\0';
}

static void demo_append_u64(char *dst, u64 cap, u64 value)
{
    char tmp[32];
    u64  len = 0U;

    if (value == 0ULL) {
        demo_append(dst, cap, "0");
        return;
    }

    while (value != 0ULL && len < sizeof(tmp)) {
        tmp[len++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (len > 0U) {
        char one[2];

        one[0] = tmp[--len];
        one[1] = '\0';
        demo_append(dst, cap, one);
    }
}

static void demo_write_line(long fd, const char *role, u64 global_value, u64 local_value)
{
    char line[96];

    line[0] = '\0';
    demo_append(line, sizeof(line), role);
    demo_append(line, sizeof(line), " global=");
    demo_append_u64(line, sizeof(line), global_value);
    demo_append(line, sizeof(line), " local=");
    demo_append_u64(line, sizeof(line), local_value);
    demo_append(line, sizeof(line), "\n");
    (void)sys_write_fd(fd, line, demo_strlen(line));
}

void _start(void)
{
    static const char path[] = "/data/FORK.TXT";
    long              pid;
    long              fd;
    u64               local_value = 11ULL;

    pid = sys_fork_now();
    if (pid < 0) {
        static const char msg[] = "[EL0] fork fallita\n";

        (void)sys_write_fd(1, msg, sizeof(msg) - 1U);
        sys_exit_now(1);
    }

    if (pid == 0) {
        g_counter = 7ULL;
        local_value = 99ULL;

        fd = sys_open_path(path, O_WRONLY | O_CREAT | O_TRUNC);
        if (fd >= 0) {
            demo_write_line(fd, "child", g_counter, local_value);
            (void)sys_close_fd(fd);
        }
        sys_exit_now(0);
    }

    (void)sys_wait_pid(pid);

    fd = sys_open_path(path, O_WRONLY | O_APPEND);
    if (fd >= 0) {
        demo_write_line(fd, "parent", g_counter, local_value);
        (void)sys_close_fd(fd);
    }

    sys_exit_now(0);
}
