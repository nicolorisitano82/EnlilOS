/*
 * EnlilOS user-space demo - M11-01a
 *
 * Verifica il pacchetto ABI minimo per il bootstrap musl:
 *   - pid/ppid, uid/gid stub, uname
 *   - gettimeofday + nanosleep
 *   - openat + fstatat + lseek
 *   - readv/writev
 *   - fcntl minimo
 *   - ioctl minimo TTY
 */

#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef signed long    s64;
typedef unsigned int   u32;

typedef struct {
    u64 a_type;
    u64 a_val;
} auxv_t;

#define MUSL_ABI_PATH     "/data/MUSLABI.TXT"
#define MUSL_ABI_MSG_A    "hello "
#define MUSL_ABI_MSG_B    "m11a\n"
#define MUSL_ABI_MSG      "hello m11a\n"

#define AT_NULL           0UL
#define AT_UID            11UL
#define AT_EUID           12UL
#define AT_GID            13UL
#define AT_EGID           14UL
#define AT_RANDOM         25UL

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

static long sys_write_fd(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_close_fd(long fd)
{
    return sys_call1(SYS_CLOSE, fd);
}

static long sys_getpid_now(void)
{
    return sys_call0(SYS_GETPID);
}

static long sys_getppid_now(void)
{
    return sys_call0(SYS_GETPPID);
}

static long sys_gettimeofday_now(timeval_t *tv)
{
    return sys_call2(SYS_GETTIMEOFDAY, (long)tv, 0);
}

static long sys_nanosleep_now(const timespec_t *req, timespec_t *rem)
{
    return sys_call2(SYS_NANOSLEEP, (long)req, (long)rem);
}

static long sys_getuid_now(void)
{
    return sys_call0(SYS_GETUID);
}

static long sys_getgid_now(void)
{
    return sys_call0(SYS_GETGID);
}

static long sys_geteuid_now(void)
{
    return sys_call0(SYS_GETEUID);
}

static long sys_getegid_now(void)
{
    return sys_call0(SYS_GETEGID);
}

static long sys_lseek_now(long fd, long off, long whence)
{
    return sys_call3(SYS_LSEEK, fd, off, whence);
}

static long sys_readv_now(long fd, const iovec_t *iov, long iovcnt)
{
    return sys_call3(SYS_READV, fd, (long)iov, iovcnt);
}

static long sys_writev_now(long fd, const iovec_t *iov, long iovcnt)
{
    return sys_call3(SYS_WRITEV, fd, (long)iov, iovcnt);
}

static long sys_fcntl_now(long fd, long cmd, long arg)
{
    return sys_call3(SYS_FCNTL, fd, cmd, arg);
}

static long sys_openat_now(long dirfd, const char *path, long flags, long mode)
{
    return sys_call4(SYS_OPENAT, dirfd, (long)path, flags, mode);
}

static long sys_fstatat_now(long dirfd, const char *path, stat_t *st, long flags)
{
    return sys_call4(SYS_FSTATAT, dirfd, (long)path, (long)st, flags);
}

static long sys_ioctl_now(long fd, unsigned long req, void *arg)
{
    return sys_call3(SYS_IOCTL, fd, (long)req, (long)arg);
}

static long sys_uname_now(utsname_t *uts)
{
    return sys_call1(SYS_UNAME, (long)uts);
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

static void demo_memcpy(void *dst, const void *src, u32 len)
{
    u32 i;
    char *d = (char *)dst;
    const char *s = (const char *)src;

    for (i = 0U; i < len; i++)
        d[i] = s[i];
}

static int demo_memcmp(const void *a, const void *b, u32 len)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;

    for (u32 i = 0U; i < len; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}

static const auxv_t *demo_auxv_find(const auxv_t *auxv, u64 tag)
{
    if (!auxv)
        return (const auxv_t *)0;

    for (u32 i = 0U; i < 64U; i++) {
        if (auxv[i].a_type == AT_NULL)
            break;
        if (auxv[i].a_type == tag)
            return &auxv[i];
    }
    return (const auxv_t *)0;
}

static u64 demo_timeval_to_us(const timeval_t *tv)
{
    return (u64)tv->tv_sec * 1000000ULL + (u64)tv->tv_usec;
}

static void demo_log(const char *msg)
{
    (void)sys_write_fd(1, msg, demo_strlen(msg));
}

static int demo_run(const auxv_t *auxv)
{
    timeval_t tv0;
    timeval_t tv1;
    timespec_t req;
    timespec_t rem;
    utsname_t uts;
    termios_t tio;
    winsize_t ws;
    stat_t st;
    stat_t st2;
    iovec_t wiov[2];
    iovec_t riov[2];
    char buf_a[8];
    char buf_b[8];
    char joined[sizeof(MUSL_ABI_MSG)];
    unsigned char zero_random[16];
    const auxv_t *at_random;
    const auxv_t *at_uid;
    const auxv_t *at_euid;
    const auxv_t *at_gid;
    const auxv_t *at_egid;
    const unsigned char *rand_bytes;
    long pid;
    long ppid;
    long fd;
    long dupfd;
    long rc;
    long flags;

    at_random = demo_auxv_find(auxv, AT_RANDOM);
    at_uid = demo_auxv_find(auxv, AT_UID);
    at_euid = demo_auxv_find(auxv, AT_EUID);
    at_gid = demo_auxv_find(auxv, AT_GID);
    at_egid = demo_auxv_find(auxv, AT_EGID);

    if (!at_random || at_random->a_val == 0UL)
        return 29;
    if (!at_uid || !at_euid || !at_gid || !at_egid)
        return 30;
    if (at_uid->a_val != 0UL || at_euid->a_val != 0UL ||
        at_gid->a_val != 0UL || at_egid->a_val != 0UL)
        return 31;

    for (u32 i = 0U; i < sizeof(zero_random); i++)
        zero_random[i] = 0U;
    rand_bytes = (const unsigned char *)(unsigned long)at_random->a_val;
    if (demo_memcmp(rand_bytes, zero_random, sizeof(zero_random)) == 0)
        return 32;

    pid = sys_getpid_now();
    ppid = sys_getppid_now();
    if (pid <= 1 || ppid < 0 || pid == ppid)
        return 1;

    if (sys_getuid_now() != 0 || sys_getgid_now() != 0 ||
        sys_geteuid_now() != 0 || sys_getegid_now() != 0)
        return 2;

    if (sys_uname_now(&uts) < 0)
        return 3;
    if (!demo_streq(uts.sysname, "EnlilOS") ||
        !demo_streq(uts.machine, "aarch64"))
        return 4;

    if (sys_gettimeofday_now(&tv0) < 0)
        return 5;
    req.tv_sec = 0;
    req.tv_nsec = 5000000LL;
    rem.tv_sec = 0;
    rem.tv_nsec = 0;
    if (sys_nanosleep_now(&req, &rem) < 0)
        return 6;
    if (sys_gettimeofday_now(&tv1) < 0)
        return 7;
    if (demo_timeval_to_us(&tv1) < demo_timeval_to_us(&tv0))
        return 8;

    fd = sys_openat_now(AT_FDCWD, MUSL_ABI_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 9;

    flags = sys_fcntl_now(fd, F_GETFL, 0);
    if (flags < 0 || ((u32)flags & 0x3U) != O_RDWR)
        return 10;

    if (sys_fcntl_now(fd, F_SETFD, FD_CLOEXEC) < 0)
        return 11;
    if (sys_fcntl_now(fd, F_GETFD, 0) != FD_CLOEXEC)
        return 12;

    dupfd = sys_fcntl_now(fd, F_DUPFD, 5);
    if (dupfd < 5)
        return 13;
    if (sys_close_fd(dupfd) < 0)
        return 14;

    wiov[0].iov_base = (void *)MUSL_ABI_MSG_A;
    wiov[0].iov_len = demo_strlen(MUSL_ABI_MSG_A);
    wiov[1].iov_base = (void *)MUSL_ABI_MSG_B;
    wiov[1].iov_len = demo_strlen(MUSL_ABI_MSG_B);
    rc = sys_writev_now(fd, wiov, 2);
    if (rc != (long)demo_strlen(MUSL_ABI_MSG))
        return 15;

    rc = sys_lseek_now(fd, 0, SEEK_END);
    if (rc != (long)demo_strlen(MUSL_ABI_MSG))
        return 16;
    rc = sys_lseek_now(fd, 0, SEEK_SET);
    if (rc != 0)
        return 17;

    buf_a[0] = '\0';
    buf_b[0] = '\0';
    riov[0].iov_base = buf_a;
    riov[0].iov_len = demo_strlen(MUSL_ABI_MSG_A);
    riov[1].iov_base = buf_b;
    riov[1].iov_len = demo_strlen(MUSL_ABI_MSG_B);
    rc = sys_readv_now(fd, riov, 2);
    if (rc != (long)demo_strlen(MUSL_ABI_MSG))
        return 18;
    buf_a[demo_strlen(MUSL_ABI_MSG_A)] = '\0';
    buf_b[demo_strlen(MUSL_ABI_MSG_B)] = '\0';
    demo_memcpy(joined, buf_a, demo_strlen(MUSL_ABI_MSG_A));
    demo_memcpy(joined + demo_strlen(MUSL_ABI_MSG_A),
                buf_b, demo_strlen(MUSL_ABI_MSG_B));
    joined[demo_strlen(MUSL_ABI_MSG)] = '\0';
    if (!demo_streq(joined, MUSL_ABI_MSG))
        return 19;

    if (sys_fstatat_now(AT_FDCWD, MUSL_ABI_PATH, &st, 0) < 0)
        return 20;
    if (st.st_size != (u64)demo_strlen(MUSL_ABI_MSG))
        return 21;
    if (sys_fstatat_now(fd, "", &st2, AT_EMPTY_PATH) < 0)
        return 22;
    if (st2.st_size != st.st_size)
        return 23;

    if (sys_ioctl_now(0, TCGETS, &tio) < 0)
        return 24;
    if (sys_ioctl_now(0, TIOCGWINSZ, &ws) < 0)
        return 25;
    if (ws.ws_row == 0U || ws.ws_col == 0U)
        return 26;

    if (sys_ioctl_now(fd, 0xDEADBEEFUL, 0) >= 0)
        return 27;

    if (sys_close_fd(fd) < 0)
        return 28;

    demo_log("[M11A/B3] ABI minima musl + auxv pronti\n");
    return 0;
}

void _start(long argc, char **argv, char **envp, void *auxv)
{
    (void)argc;
    (void)argv;
    (void)envp;
    sys_exit_now(demo_run((const auxv_t *)auxv));
}
