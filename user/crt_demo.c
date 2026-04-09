#include "crt_runtime.h"
#include "syscall.h"
#include "user_svc.h"

typedef unsigned long u64;
typedef unsigned int  u32;

#define CRT_DEMO_PATH "/data/CRTDEMO.TXT"

static volatile int ctor_seen;
static volatile int dtor_seen;
static volatile int main_ready_for_dtor;

__thread int tls_seed = 0x12345678;
__thread int tls_zero;

static long sys_call1(long nr, long a0)
{
    return user_svc1(nr, a0);
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    return user_svc3(nr, a0, a1, a2);
}

static long sys_call4(long nr, long a0, long a1, long a2, long a3)
{
    return user_svc4(nr, a0, a1, a2, a3);
}

static long demo_open(const char *path, long flags, long mode)
{
    return sys_call4(SYS_OPENAT, AT_FDCWD, (long)path, flags, mode);
}

static long demo_write(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long demo_close(long fd)
{
    return sys_call1(SYS_CLOSE, fd);
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
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static void demo_append_line(const char *line)
{
    long fd = demo_open(CRT_DEMO_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0)
        return;
    (void)demo_write(fd, line, demo_strlen(line));
    (void)demo_close(fd);
}

static int demo_write_line_fd(long fd, const char *line)
{
    return demo_write(fd, line, demo_strlen(line)) == (long)demo_strlen(line);
}

static void crt_ctor(void) __attribute__((constructor));
static void crt_ctor(void)
{
    ctor_seen = 1;
    tls_seed += 0x22;
    tls_zero += 7;
}

static void crt_dtor(void) __attribute__((destructor));
static void crt_dtor(void)
{
    dtor_seen = 1;
    if (main_ready_for_dtor)
        demo_append_line("dtor-ok\n");
}

int main(int argc, char **argv, char **envp)
{
    long fd;

    fd = demo_open(CRT_DEMO_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 1;

    if (!ctor_seen)
        return 2;
    if (!demo_write_line_fd(fd, "ctor-ok\n"))
        return 8;

    if (argc != 1 || !argv || !argv[0] || !demo_streq(argv[0], "/CRTDEMO.ELF"))
        return 3;
    if (!demo_write_line_fd(fd, "argv-ok\n"))
        return 9;

    if (!envp || !envp[0] || !demo_startswith(envp[0], "PATH="))
        return 4;
    if (!demo_write_line_fd(fd, "env-ok\n"))
        return 10;

    if (environ != envp || __enlilos_auxv == (const void *)0)
        return 5;
    if (!demo_write_line_fd(fd, "crt-ok\n"))
        return 11;

    if (tls_seed != 0x1234569A || tls_zero != 7)
        return 6;
    if (!demo_write_line_fd(fd, "tls-ok\n")) {
        (void)demo_close(fd);
        return 12;
    }

    if (demo_close(fd) < 0)
        return 13;

    main_ready_for_dtor = 1;
    return dtor_seen ? 7 : 0;
}
