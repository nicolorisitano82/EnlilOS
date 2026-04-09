/*
 * EnlilOS user-space demo - M11-02b
 *
 * Verifica:
 *   - set_tid_address()
 *   - tgkill() thread-directed
 *   - clear_child_tid azzerato alla thread-exit
 *   - exit_group() process-wide
 */

#include "syscall.h"
#include "user_svc.h"

typedef unsigned long u64;
typedef unsigned int  u32;

#define THREADLIFE_STACK_SIZE   (64UL * 1024UL)
#define THREADLIFE_OUT_PATH     "/data/THREADLIFE.TXT"

#define ROLE_JOIN       1
#define ROLE_TERM       2
#define ROLE_EXITGROUP  3

static volatile u32 g_sigusr1_tid;
static volatile u32 g_join_tid_slot;
static volatile u32 g_term_tid_slot;
static volatile u32 g_exit_tid_slot;
static volatile u32 g_settid_ret;
static volatile u32 g_stage;

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

static long sys_call6(long nr, long a0, long a1, long a2,
                      long a3, long a4, long a5)
{
    return user_svc6(nr, a0, a1, a2, a3, a4, a5);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

static __attribute__((noreturn)) void sys_exit_group_now(long code)
{
    (void)sys_call1(SYS_EXIT_GROUP, code);
    while (1)
        (void)sys_call0(SYS_YIELD);
}

static long sys_getpid_now(void)
{
    return sys_call0(SYS_GETPID);
}

static long sys_gettid_now(void)
{
    return sys_call0(SYS_GETTID);
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

static long sys_sigaction_now(int sig, const sigaction_t *act)
{
    return sys_call3(SYS_SIGACTION, sig, (long)act, 0);
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

static long sys_set_tid_address_now(volatile u32 *clear_tid)
{
    return sys_call1(SYS_SET_TID_ADDRESS, (long)clear_tid);
}

static long sys_tgkill_now(u32 tgid, u32 tid, int sig)
{
    return sys_call3(SYS_TGKILL, tgid, tid, sig);
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
    long fd = sys_open_now(THREADLIFE_OUT_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);

    if (fd < 0)
        return;
    (void)sys_write_now(fd, line, demo_strlen(line));
    (void)sys_close_now(fd);
}

static int demo_truncate_file(void)
{
    long fd = sys_open_now(THREADLIFE_OUT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
        return -1;
    return (sys_close_now(fd) < 0) ? -1 : 0;
}

static void demo_sigusr1_handler(int sig)
{
    (void)sig;
    g_sigusr1_tid = (u32)sys_gettid_now();
}

static __attribute__((noreturn)) void demo_thread_join(void)
{
    long tid = sys_gettid_now();

    g_settid_ret = (u32)sys_set_tid_address_now(&g_join_tid_slot);
    while (g_stage < 2U)
        (void)sys_yield_now();
    if (g_settid_ret != (u32)tid)
        sys_exit_now(41);
    sys_exit_now(0);
}

static __attribute__((noreturn)) void demo_thread_term(void)
{
    for (;;)
        (void)sys_yield_now();
}

static __attribute__((noreturn)) void demo_thread_exitgroup(void)
{
    while (g_stage < 4U)
        (void)sys_yield_now();
    sys_exit_group_now(17);
}

static long demo_spawn_thread(int role, volatile u32 *tid_slot)
{
    static const u32 clone_flags =
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
        CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
        CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    u64 stack_va;
    long tid;
    u64 tls;

    *tid_slot = 0U;
    stack_va = (u64)sys_mmap_now((void *)0, THREADLIFE_STACK_SIZE,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_va == MAP_FAILED_VA)
        return -1;

    tls = stack_va + 0x80UL;
    tid = sys_clone_now(clone_flags,
                        (void *)(stack_va + THREADLIFE_STACK_SIZE - 16UL),
                        0,
                        tls,
                        tid_slot);
    if (tid == 0) {
        if (role == ROLE_JOIN)
            demo_thread_join();
        if (role == ROLE_TERM)
            demo_thread_term();
        demo_thread_exitgroup();
    }

    return tid;
}

static int demo_wait_slot_eq(volatile u32 *slot, u32 expected, long loops)
{
    while (loops-- > 0L) {
        if (*slot == expected)
            return 0;
        (void)sys_yield_now();
    }
    return -1;
}

static int demo_wait_slot_zero(volatile u32 *slot, long loops)
{
    return demo_wait_slot_eq(slot, 0U, loops);
}

static int demo_wait_sigusr1(u32 expected_tid, long loops)
{
    while (loops-- > 0L) {
        if (g_sigusr1_tid == expected_tid)
            return 0;
        (void)sys_yield_now();
    }
    return -1;
}

static int demo_run(void)
{
    sigaction_t act;
    long        tid;

    if (demo_truncate_file() < 0)
        return 1;

    act.sa_handler = demo_sigusr1_handler;
    act.sa_mask    = 0ULL;
    act.sa_flags   = 0U;
    act._pad       = 0U;
    if (sys_sigaction_now(SIGUSR1, &act) < 0)
        return 2;

    g_stage = 0U;
    g_sigusr1_tid = 0U;
    g_settid_ret  = 0U;

    tid = demo_spawn_thread(ROLE_JOIN, &g_join_tid_slot);
    if (tid < 0)
        return 3;
    if (demo_wait_slot_eq(&g_join_tid_slot, (u32)tid, 200000L) < 0)
        return 4;
    if (demo_wait_slot_eq(&g_settid_ret, (u32)tid, 200000L) < 0)
        return 5;
    demo_append_line("settid-ok\n");

    g_stage = 1U;
    if (sys_tgkill_now((u32)sys_getpid_now(), (u32)tid, SIGUSR1) < 0)
        return 6;
    if (demo_wait_sigusr1((u32)tid, 200000L) < 0)
        return 7;
    demo_append_line("sigthread-ok\n");

    g_stage = 2U;
    if (demo_wait_slot_zero(&g_join_tid_slot, 200000L) < 0)
        return 8;
    demo_append_line("cleartid-ok\n");

    tid = demo_spawn_thread(ROLE_TERM, &g_term_tid_slot);
    if (tid < 0)
        return 9;
    if (demo_wait_slot_eq(&g_term_tid_slot, (u32)tid, 200000L) < 0)
        return 10;
    if (sys_tgkill_now((u32)sys_getpid_now(), (u32)tid, SIGTERM) < 0)
        return 11;
    if (demo_wait_slot_zero(&g_term_tid_slot, 200000L) < 0)
        return 12;
    demo_append_line("sigkill-thread-ok\n");

    tid = demo_spawn_thread(ROLE_EXITGROUP, &g_exit_tid_slot);
    if (tid < 0)
        return 13;
    if (demo_wait_slot_eq(&g_exit_tid_slot, (u32)tid, 200000L) < 0)
        return 14;
    demo_append_line("exit-group-ok\n");
    g_stage = 4U;

    for (long loops = 0; loops < 200000L; loops++)
        (void)sys_yield_now();
    return 15;
}

void _start(void)
{
    sys_exit_now(demo_run());
}
