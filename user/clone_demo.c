/*
 * EnlilOS user-space demo - M11-02a
 *
 * Verifica:
 *   - clone() thread-group subset
 *   - getpid() -> tgid, gettid() -> tid
 *   - CLONE_VM / CLONE_FILES / CLONE_FS
 *   - CLONE_SETTLS / CLONE_PARENT_SETTID / CLONE_CHILD_SETTID / CLEARTID
 */

#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef unsigned int   u32;

#define CLONEDEMO_STACK_SIZE   (64UL * 1024UL)
#define CLONEDEMO_CHILD_READ   "Enli"
#define CLONEDEMO_PARENT_READ  "lOS "
#define CLONEDEMO_MAGIC        0xC10E020AUL

static volatile u32 g_stage;
static volatile u32 g_parent_tid_slot;
static volatile u32 g_child_tid_slot;
static volatile u32 g_child_pid_seen;
static volatile u32 g_child_tid_seen;
static volatile u32 g_shared_value;
static volatile u64 g_child_tp_seen;
static char         g_child_read[5];
static char         g_parent_read[5];

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

static long sys_open_now(const char *path, long flags, long mode)
{
    return sys_call3(SYS_OPEN, (long)path, flags, mode);
}

static long sys_close_now(long fd)
{
    return sys_call1(SYS_CLOSE, fd);
}

static long sys_read_now(long fd, void *buf, u64 len)
{
    return sys_call3(SYS_READ, fd, (long)buf, (long)len);
}

static long sys_mmap_now(void *addr, u64 len, u32 prot, u32 flags, long fd, u64 off)
{
    return sys_call6(SYS_MMAP, (long)addr, (long)len, (long)prot,
                     (long)flags, fd, (long)off);
}

static long sys_getcwd_now(char *buf, u32 size)
{
    return sys_call2(SYS_GETCWD, (long)buf, size);
}

static long sys_chdir_now(const char *path)
{
    return sys_call1(SYS_CHDIR, (long)path);
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

static long sys_clone_now(u32 flags, void *child_stack,
                          volatile u32 *parent_tid,
                          u64 tls,
                          volatile u32 *child_tid)
{
    return sys_call6(SYS_CLONE, (long)flags, (long)child_stack,
                     (long)parent_tid, (long)tls, (long)child_tid, 0);
}

static int demo_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static u64 demo_read_tp(void)
{
    u64 v;

    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(v));
    return v;
}

static int demo_child(long shared_fd, u64 expected_tp)
{
    long n;

    g_child_pid_seen = (u32)sys_getpid_now();
    g_child_tid_seen = (u32)sys_gettid_now();
    g_child_tp_seen  = demo_read_tp();
    if (g_child_tp_seen != expected_tp) {
        g_stage = 0xFFU;
        sys_exit_now(40);
    }

    if (sys_chdir_now("/dev") < 0) {
        g_stage = 0xFFU;
        sys_exit_now(41);
    }

    n = sys_read_now(shared_fd, g_child_read, 4U);
    if (n != 4) {
        g_stage = 0xFFU;
        sys_exit_now(42);
    }
    g_child_read[4] = '\0';
    g_shared_value = CLONEDEMO_MAGIC;
    g_stage = 1U;

    while (g_stage != 2U)
        (void)sys_yield_now();

    sys_exit_now(0);
}

static int demo_run(void)
{
    static const u32 clone_flags =
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND |
        CLONE_THREAD | CLONE_SYSVSEM | CLONE_SETTLS |
        CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    char cwd[64];
    long fd;
    long tid;
    long n;
    long loops;
    u64  stack_va;
    u64  child_tp;

    fd = sys_open_now("/README.TXT", O_RDONLY, 0);
    if (fd < 0)
        return 1;

    stack_va = (u64)sys_mmap_now((void *)0, CLONEDEMO_STACK_SIZE,
                                 PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_va == MAP_FAILED_VA)
        return 2;

    child_tp = stack_va + 0x80UL;
    g_stage = 0U;
    g_parent_tid_slot = 0U;
    g_child_tid_slot = 0U;
    g_child_pid_seen = 0U;
    g_child_tid_seen = 0U;
    g_shared_value = 0U;
    g_child_tp_seen = 0ULL;
    g_child_read[0] = '\0';
    g_parent_read[0] = '\0';

    tid = sys_clone_now(clone_flags,
                        (void *)(stack_va + CLONEDEMO_STACK_SIZE - 16UL),
                        &g_parent_tid_slot,
                        child_tp,
                        &g_child_tid_slot);
    if (tid < 0)
        return 3;
    if (tid == 0)
        return demo_child(fd, child_tp);

    for (loops = 0; loops < 200000L && g_stage == 0U; loops++)
        (void)sys_yield_now();
    if (g_stage != 1U)
        return 4;
    if (g_parent_tid_slot != (u32)tid)
        return 5;
    if (g_child_tid_slot != (u32)tid)
        return 6;
    if (g_child_pid_seen != (u32)sys_getpid_now())
        return 7;
    if (g_child_tid_seen != (u32)tid || g_child_tid_seen == (u32)sys_gettid_now())
        return 8;
    if (g_child_tp_seen != child_tp)
        return 9;
    if (g_shared_value != CLONEDEMO_MAGIC)
        return 10;
    if (!demo_streq(g_child_read, CLONEDEMO_CHILD_READ))
        return 11;

    if (sys_getcwd_now(cwd, (u32)sizeof(cwd)) < 0 || !demo_streq(cwd, "/dev"))
        return 12;
    if (sys_chdir_now("/") < 0)
        return 13;

    n = sys_read_now(fd, g_parent_read, 4U);
    if (n != 4)
        return 14;
    g_parent_read[4] = '\0';
    if (!demo_streq(g_parent_read, CLONEDEMO_PARENT_READ))
        return 15;

    g_stage = 2U;
    for (loops = 0; loops < 200000L && g_child_tid_slot != 0U; loops++)
        (void)sys_yield_now();
    if (g_child_tid_slot != 0U)
        return 16;

    if (sys_close_now(fd) < 0)
        return 17;
    return 0;
}

void _start(long argc, const char **argv, const char **envp, const void *auxv)
{
    (void)argc;
    (void)argv;
    (void)envp;
    (void)auxv;
    sys_exit_now(demo_run());
}
