/*
 * EnlilOS EL0 demo - mreact reactive memory subscriptions (M8-05)
 */

#include "mreact.h"
#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef signed long    s64;

static long sys_call2(long nr, long a0, long a1)
{
    return user_svc2(nr, a0, a1);
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    return user_svc3(nr, a0, a1, a2);
}

static long sys_call6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
{
    return user_svc6(nr, a0, a1, a2, a3, a4, a5);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

static long sys_open_path(const char *path, u32 flags)
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

static long sys_mmap_anon(u64 length, u32 prot, u32 flags)
{
    return sys_call6(SYS_MMAP, 0, (long)length, (long)prot, (long)flags, -1, 0);
}

static long sys_mreact_subscribe(void *addr, u64 size,
                                 mreact_pred_t pred, u64 value, u32 flags)
{
    register long x0 asm("x0") = (long)addr;
    register long x1 asm("x1") = (long)size;
    register long x2 asm("x2") = (long)pred;
    register long x3 asm("x3") = (long)value;
    register long x4 asm("x4") = (long)flags;
    register long x8 asm("x8") = SYS_MREACT_SUBSCRIBE;

    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4)
                 : "r"(x8)
                 : "x5", "x6", "x7",
                   "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "x18",
                   "memory", "cc");
    return x0;
}

static long sys_mreact_wait(long handle, u64 timeout_ns)
{
    return sys_call2(SYS_MREACT_WAIT, handle, (long)timeout_ns);
}

static u64 demo_strlen(const char *s)
{
    u64 n = 0ULL;

    while (s && s[n] != '\0')
        n++;
    return n;
}

static long demo_write_text(const char *path, u32 flags, const char *text)
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

int main(void)
{
    static const char ready_path[] = "/data/MREACT.READY";
    static const char out_path[]   = "/data/MREACT.TXT";
    volatile u32     *slot;
    long              map_rc;
    long              handle;
    long              wait_rc;
    long              ready_rc;

    map_rc = sys_mmap_anon(4096ULL, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS);
    if (map_rc < 0 || (u64)map_rc == MAP_FAILED_VA)
        sys_exit_now(1);
    demo_puts("[MREACT] mmap-ok\n");

    slot = (volatile u32 *)(unsigned long)map_rc;
    *slot = 0U;

    handle = sys_mreact_subscribe((void *)(unsigned long)slot,
                                  sizeof(*slot),
                                  MREACT_EQ,
                                  42ULL,
                                  MREACT_ONE_SHOT | MREACT_EDGE);
    if (handle < 0)
        sys_exit_now(2);
    demo_puts("[MREACT] subscribe-ok\n");

    ready_rc = demo_write_text(ready_path, O_CREAT | O_TRUNC | O_WRONLY, "ready\n");
    if (ready_rc == 0)
        demo_puts("[MREACT] ready-ok\n");
    else if (ready_rc <= -1000 && ready_rc > -2000)
        demo_puts("[MREACT] ready-open-fail\n");
    else if (ready_rc == -2000)
        demo_puts("[MREACT] ready-write-fail\n");
    else if (ready_rc == -3000)
        demo_puts("[MREACT] ready-close-fail\n");
    else
        demo_puts("[MREACT] ready-fail\n");

    wait_rc = sys_mreact_wait(handle, 2000000000ULL);
    if (wait_rc == 0 && *slot == 42U) {
        demo_puts("[MREACT] wait-ok\n");
        (void)demo_write_text(out_path, O_CREAT | O_TRUNC | O_WRONLY, "ok\n");
        sys_exit_now(0);
    }

    demo_puts("[MREACT] wait-fail\n");
    (void)demo_write_text(out_path, O_CREAT | O_TRUNC | O_WRONLY, "fail\n");
    sys_exit_now(3);
}

void _start(void)
{
    sys_exit_now(main());
}
