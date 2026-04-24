#include "syscall.h"
#include "sysv_ipc.h"
#include "user_svc.h"

typedef unsigned int  u32;
typedef unsigned long u64;

static long sys_open_path(const char *path, u32 flags)
{
    return user_svc3(SYS_OPEN, (long)path, (long)flags, 0);
}

static long sys_close_fd(long fd)
{
    return user_svc3(SYS_CLOSE, fd, 0, 0);
}

static long sys_write_fd(long fd, const void *buf, u64 len)
{
    return user_svc3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_fork_now(void)
{
    return user_svc0(SYS_FORK);
}

static long sys_waitpid_now(long pid, long *status)
{
    return user_svc3(SYS_WAITPID, pid, (long)status, 0);
}

static long sys_shmget_now(u32 key, u64 size, u32 flags)
{
    return user_svc3(SYS_SHMGET, (long)key, (long)size, (long)flags);
}

static long sys_shmat_now(long shmid, long addr, u32 flags)
{
    return user_svc3(SYS_SHMAT, shmid, addr, (long)flags);
}

static long sys_shmdt_now(void *addr)
{
    return user_svc1(SYS_SHMDT, (long)addr);
}

static long sys_shmctl_now(long shmid, int cmd)
{
    return user_svc3(SYS_SHMCTL, shmid, cmd, 0);
}

static long sys_semget_now(u32 key, u32 nsems, u32 flags)
{
    return user_svc3(SYS_SEMGET, (long)key, (long)nsems, (long)flags);
}

static long sys_semop_now(long semid, const sysv_sembuf_t *ops, u32 nsops)
{
    return user_svc3(SYS_SEMOP, semid, (long)ops, (long)nsops);
}

static long sys_semctl_now(long semid, u32 semnum, int cmd, u64 arg)
{
    return user_svc4(SYS_SEMCTL, semid, (long)semnum, (long)cmd, (long)arg);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

static u64 demo_strlen(const char *s)
{
    u64 n = 0U;

    while (s && s[n] != '\0')
        n++;
    return n;
}

static int demo_streq(const char *a, const char *b)
{
    u64 i = 0U;

    while (a[i] != '\0' && b[i] != '\0' && a[i] == b[i])
        i++;
    return a[i] == b[i];
}

static void demo_copy(char *dst, const char *src)
{
    u64 i = 0U;

    while (src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int demo_write_text(const char *path, const char *text)
{
    long fd = sys_open_path(path, O_WRONLY | O_CREAT | O_TRUNC);

    if (fd < 0)
        return -1;
    if (sys_write_fd(fd, text, demo_strlen(text)) < 0) {
        (void)sys_close_fd(fd);
        return -1;
    }
    if (sys_close_fd(fd) < 0)
        return -1;
    return 0;
}

static const char sysvipc_out_ok[]   = "sysv ipc ok\n";
static const char sysvipc_out_fail[] = "sysv ipc fail\n";

int main(void)
{
    static const char parent_msg[] = "parent->child";
    static const char child_msg[]  = "child->parent";
    sysv_sembuf_t op;
    long          shmid;
    long          semid;
    long          pid;
    long          status = 0;
    char         *shm;

    shmid = sys_shmget_now(SYSV_IPC_PRIVATE, 4096U, SYSV_IPC_CREAT);
    if (shmid < 0)
        sys_exit_now(1);
    semid = sys_semget_now(SYSV_IPC_PRIVATE, 2U, SYSV_IPC_CREAT);
    if (semid < 0)
        sys_exit_now(2);
    if (sys_semctl_now(semid, 0U, SYSV_SEM_SETVAL, 0U) < 0 ||
        sys_semctl_now(semid, 1U, SYSV_SEM_SETVAL, 0U) < 0)
        sys_exit_now(3);

    pid = sys_fork_now();
    if (pid < 0)
        sys_exit_now(4);

    if (pid == 0) {
        shm = (char *)(unsigned long)sys_shmat_now(shmid, 0, 0U);
        if ((long)shm < 0)
            sys_exit_now(10);

        op.sem_num = 0U;
        op.sem_op  = -1;
        op.sem_flg = 0;
        if (sys_semop_now(semid, &op, 1U) < 0)
            sys_exit_now(11);
        if (!demo_streq(shm, parent_msg))
            sys_exit_now(12);
        demo_copy(shm, child_msg);
        op.sem_num = 1U;
        op.sem_op  = 1;
        op.sem_flg = 0;
        if (sys_semop_now(semid, &op, 1U) < 0)
            sys_exit_now(13);
        (void)sys_shmdt_now(shm);
        sys_exit_now(0);
    }

    shm = (char *)(unsigned long)sys_shmat_now(shmid, 0, 0U);
    if ((long)shm < 0)
        sys_exit_now(5);
    demo_copy(shm, parent_msg);

    op.sem_num = 0U;
    op.sem_op  = 1;
    op.sem_flg = 0;
    if (sys_semop_now(semid, &op, 1U) < 0)
        sys_exit_now(6);
    op.sem_num = 1U;
    op.sem_op  = -1;
    if (sys_semop_now(semid, &op, 1U) < 0)
        sys_exit_now(7);
    if (!demo_streq(shm, child_msg))
        sys_exit_now(8);
    if (sys_waitpid_now(pid, &status) < 0 || status != 0)
        sys_exit_now(9);

    (void)sys_shmdt_now(shm);
    (void)sys_shmctl_now(shmid, SYSV_IPC_RMID);
    (void)sys_semctl_now(semid, 0U, SYSV_IPC_RMID, 0U);

    if (demo_write_text("/data/SYSVIPC.TXT", sysvipc_out_ok) < 0)
        sys_exit_now(14);
    sys_exit_now(0);
}

void _start(void)
{
    int rc = main();

    if (rc != 0)
        (void)demo_write_text("/data/SYSVIPC.TXT", sysvipc_out_fail);
    sys_exit_now(rc);
}
