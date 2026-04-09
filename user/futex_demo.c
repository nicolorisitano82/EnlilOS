/*
 * EnlilOS user-space demo - M11-02c
 *
 * Verifica:
 *   - FUTEX_WAIT / FUTEX_WAKE
 *   - FUTEX_REQUEUE
 *   - FUTEX_CMP_REQUEUE
 *   - join via clear_child_tid + futex wait
 */

#include "syscall.h"
#include "user_svc.h"

typedef unsigned long u64;
typedef unsigned int  u32;

#define FUTEXDEMO_STACK_SIZE   (64UL * 1024UL)
#define FUTEXDEMO_OUT_PATH     "/data/FUTEXDEMO.TXT"
#define FUTEXDEMO_LOOPS        20000L

#define ROLE_WAIT          1
#define ROLE_REQUEUE       2
#define ROLE_CMP_REQUEUE   3
#define ROLE_JOIN          4

static volatile u32 g_wait_futex;
static volatile u32 g_wait_tid_slot;
static volatile u32 g_wait_ready;
static volatile u32 g_wait_done;

static volatile u32 g_requeue_src;
static volatile u32 g_requeue_dst;
static volatile u32 g_requeue_tid_slot;
static volatile u32 g_requeue_ready;
static volatile u32 g_requeue_done;

static volatile u32 g_cmp_src;
static volatile u32 g_cmp_dst;
static volatile u32 g_cmp_tid_slot;
static volatile u32 g_cmp_ready;
static volatile u32 g_cmp_done;

static volatile u32 g_join_tid_slot;
static volatile u32 g_join_stage;
static volatile u32 g_join_ready;

static long sys_call0(long nr)
{
    return user_svc0(nr);
}

static long sys_call1(long nr, long a0)
{
    return user_svc1(nr, a0);
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    return user_svc3(nr, a0, a1, a2);
}

static long sys_call6(long nr, long a0, long a1, long a2,
                      long a3, long a4, long a5)
{
    return user_svc6(nr, a0, a1, a2, a3, a4, a5);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

static long sys_yield_now(void)
{
    return sys_call0(SYS_YIELD);
}

static long sys_open_now(const char *path, long flags, long mode)
{
    return sys_call3(SYS_OPEN, (long)path, flags, mode);
}

static long sys_write_now(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_close_now(long fd)
{
    return sys_call1(SYS_CLOSE, fd);
}

static long sys_mmap_now(void *addr, u64 len, u32 prot, u32 flags, long fd, u64 off)
{
    return sys_call6(SYS_MMAP, (long)addr, (long)len, (long)prot,
                     (long)flags, fd, (long)off);
}

static long sys_clone_now(u32 flags, void *child_stack,
                          volatile u32 *parent_tid,
                          u64 tls,
                          volatile u32 *child_tid)
{
    return sys_call6(SYS_CLONE, (long)flags, (long)child_stack,
                     (long)parent_tid, (long)tls, (long)child_tid, 0);
}

static long sys_futex_now(volatile u32 *uaddr, u32 op, u32 val,
                          long arg3, volatile u32 *uaddr2, u32 val3)
{
    return sys_call6(SYS_FUTEX, (long)uaddr, (long)op, (long)val,
                     arg3, (long)uaddr2, (long)val3);
}

static u32 demo_strlen(const char *s)
{
    u32 n = 0U;

    while (s && s[n] != '\0')
        n++;
    return n;
}

static void demo_append_line(const char *line)
{
    long fd = sys_open_now(FUTEXDEMO_OUT_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0)
        return;
    (void)sys_write_now(fd, line, demo_strlen(line));
    (void)sys_close_now(fd);
}

static int demo_truncate_file(void)
{
    long fd = sys_open_now(FUTEXDEMO_OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
        return -1;
    return (sys_close_now(fd) < 0) ? -1 : 0;
}

static int demo_wait_value(volatile u32 *slot, u32 expected, long loops)
{
    while (loops-- > 0L) {
        if (*slot == expected)
            return 0;
        (void)sys_yield_now();
    }
    return -1;
}

static int demo_wait_nonzero(volatile u32 *slot, long loops)
{
    while (loops-- > 0L) {
        if (*slot != 0U)
            return 0;
        (void)sys_yield_now();
    }
    return -1;
}

static int demo_retry_wake(volatile u32 *uaddr, u32 expected, long loops)
{
    while (loops-- > 0L) {
        long rc = sys_futex_now(uaddr, FUTEX_WAKE, 1U, 0L, 0, 0U);

        if (rc == (long)expected)
            return 0;
        (void)sys_yield_now();
    }
    return -1;
}

static int demo_retry_requeue(volatile u32 *src, volatile u32 *dst,
                              u32 wake_count, u32 requeue_count, long loops)
{
    while (loops-- > 0L) {
        long rc = sys_futex_now(src, FUTEX_REQUEUE, wake_count,
                                (long)requeue_count, dst, 0U);

        if (rc == 1L)
            return 0;
        (void)sys_yield_now();
    }
    return -1;
}

static int demo_retry_cmp_requeue(volatile u32 *src, volatile u32 *dst,
                                  u32 wake_count, u32 requeue_count,
                                  u32 cmp_value, long loops)
{
    while (loops-- > 0L) {
        long rc = sys_futex_now(src, FUTEX_CMP_REQUEUE, wake_count,
                                (long)requeue_count, dst, cmp_value);

        if (rc == 1L)
            return 0;
        (void)sys_yield_now();
    }
    return -1;
}

static __attribute__((noreturn)) void demo_role_wait(void)
{
    g_wait_ready = 1U;
    while (g_wait_futex == 0U) {
        long rc = sys_futex_now(&g_wait_futex, FUTEX_WAIT, 0U, 0L, 0, 0U);

        if (rc < 0 && rc != -EAGAIN)
            sys_exit_now(11);
    }
    g_wait_done = 1U;
    sys_exit_now(0);
}

static __attribute__((noreturn)) void demo_role_requeue(void)
{
    long rc;

    g_requeue_ready = 1U;
    rc = sys_futex_now(&g_requeue_src, FUTEX_WAIT, 0U, 0L, 0, 0U);
    if (rc < 0)
        sys_exit_now(21);
    if (g_requeue_dst != 1U)
        sys_exit_now(22);
    g_requeue_done = 1U;
    sys_exit_now(0);
}

static __attribute__((noreturn)) void demo_role_cmp_requeue(void)
{
    long rc;

    g_cmp_ready = 1U;
    rc = sys_futex_now(&g_cmp_src, FUTEX_WAIT, 0U, 0L, 0, 0U);
    if (rc < 0)
        sys_exit_now(31);
    if (g_cmp_dst != 1U)
        sys_exit_now(32);
    g_cmp_done = 1U;
    sys_exit_now(0);
}

static __attribute__((noreturn)) void demo_role_join(void)
{
    g_join_ready = 1U;
    while (g_join_stage == 0U) {
        long rc = sys_futex_now(&g_join_stage, FUTEX_WAIT, 0U, 0L, 0, 0U);

        if (rc < 0 && rc != -EAGAIN)
            sys_exit_now(41);
    }
    sys_exit_now(0);
}

static long demo_spawn_thread(int role, volatile u32 *tid_slot)
{
    static const u32 clone_flags =
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
        CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
        CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    u64 stack_va;
    u64 tls;
    long tid;

    *tid_slot = 0U;
    stack_va = (u64)sys_mmap_now((void *)0, FUTEXDEMO_STACK_SIZE,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_va == MAP_FAILED_VA)
        return -1;

    tls = stack_va + 0x80UL;
    tid = sys_clone_now(clone_flags,
                        (void *)(stack_va + FUTEXDEMO_STACK_SIZE - 16UL),
                        0,
                        tls,
                        tid_slot);
    if (tid == 0) {
        if (role == ROLE_WAIT)
            demo_role_wait();
        if (role == ROLE_REQUEUE)
            demo_role_requeue();
        if (role == ROLE_CMP_REQUEUE)
            demo_role_cmp_requeue();
        demo_role_join();
    }

    return tid;
}

static int demo_run(void)
{
    long tid;
    int  join_woke = 0;

    if (demo_truncate_file() < 0)
        return 1;

    g_wait_futex = 0U;
    g_wait_ready = 0U;
    g_wait_done = 0U;
    tid = demo_spawn_thread(ROLE_WAIT, &g_wait_tid_slot);
    if (tid < 0)
        return 2;
    if (demo_wait_nonzero(&g_wait_tid_slot, FUTEXDEMO_LOOPS) < 0)
        return 3;
    if (demo_wait_value(&g_wait_ready, 1U, FUTEXDEMO_LOOPS) < 0)
        return 4;
    g_wait_futex = 1U;
    if (demo_retry_wake(&g_wait_futex, 1U, FUTEXDEMO_LOOPS) < 0)
        return 5;
    if (demo_wait_value(&g_wait_done, 1U, FUTEXDEMO_LOOPS) < 0)
        return 6;
    if (demo_wait_value(&g_wait_tid_slot, 0U, FUTEXDEMO_LOOPS) < 0)
        return 7;
    demo_append_line("wait-wake-ok\n");

    g_requeue_src = 0U;
    g_requeue_dst = 0U;
    g_requeue_ready = 0U;
    g_requeue_done = 0U;
    tid = demo_spawn_thread(ROLE_REQUEUE, &g_requeue_tid_slot);
    if (tid < 0)
        return 8;
    if (demo_wait_nonzero(&g_requeue_tid_slot, FUTEXDEMO_LOOPS) < 0)
        return 9;
    if (demo_wait_value(&g_requeue_ready, 1U, FUTEXDEMO_LOOPS) < 0)
        return 10;
    if (demo_retry_requeue(&g_requeue_src, &g_requeue_dst, 0U, 1U,
                           FUTEXDEMO_LOOPS) < 0)
        return 11;
    if (g_requeue_done != 0U)
        return 12;
    g_requeue_dst = 1U;
    if (demo_retry_wake(&g_requeue_dst, 1U, FUTEXDEMO_LOOPS) < 0)
        return 13;
    if (demo_wait_value(&g_requeue_done, 1U, FUTEXDEMO_LOOPS) < 0)
        return 14;
    if (demo_wait_value(&g_requeue_tid_slot, 0U, FUTEXDEMO_LOOPS) < 0)
        return 15;
    demo_append_line("requeue-ok\n");

    g_cmp_src = 0U;
    g_cmp_dst = 0U;
    g_cmp_ready = 0U;
    g_cmp_done = 0U;
    tid = demo_spawn_thread(ROLE_CMP_REQUEUE, &g_cmp_tid_slot);
    if (tid < 0)
        return 16;
    if (demo_wait_nonzero(&g_cmp_tid_slot, FUTEXDEMO_LOOPS) < 0)
        return 17;
    if (demo_wait_value(&g_cmp_ready, 1U, FUTEXDEMO_LOOPS) < 0)
        return 18;
    if (demo_retry_cmp_requeue(&g_cmp_src, &g_cmp_dst, 0U, 1U, 0U,
                               FUTEXDEMO_LOOPS) < 0)
        return 19;
    if (g_cmp_done != 0U)
        return 20;
    g_cmp_dst = 1U;
    if (demo_retry_wake(&g_cmp_dst, 1U, FUTEXDEMO_LOOPS) < 0)
        return 21;
    if (demo_wait_value(&g_cmp_done, 1U, FUTEXDEMO_LOOPS) < 0)
        return 22;
    if (demo_wait_value(&g_cmp_tid_slot, 0U, FUTEXDEMO_LOOPS) < 0)
        return 23;
    demo_append_line("cmp-requeue-ok\n");

    g_join_stage = 0U;
    g_join_ready = 0U;
    tid = demo_spawn_thread(ROLE_JOIN, &g_join_tid_slot);
    if (tid < 0)
        return 24;
    if (demo_wait_nonzero(&g_join_tid_slot, FUTEXDEMO_LOOPS) < 0)
        return 25;
    if (demo_wait_value(&g_join_ready, 1U, FUTEXDEMO_LOOPS) < 0)
        return 26;
    g_join_stage = 1U;
    if (demo_retry_wake(&g_join_stage, 1U, FUTEXDEMO_LOOPS) < 0)
        return 27;
    while (g_join_tid_slot != 0U) {
        u32  expect = g_join_tid_slot;
        long rc;

        if (expect == 0U)
            break;
        rc = sys_futex_now(&g_join_tid_slot, FUTEX_WAIT, expect, 0L, 0, 0U);
        if (rc == 0)
            join_woke = 1;
        else if (rc != -EAGAIN)
            return 28;
    }
    if (!join_woke)
        return 29;
    demo_append_line("join-ok\n");
    return 0;
}

void _start(void)
{
    int rc = demo_run();

    sys_exit_now(rc);
}
