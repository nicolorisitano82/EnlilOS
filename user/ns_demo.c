#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef signed long    s64;
typedef unsigned int   u32;

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

static long sys_open_path(const char *path, u32 flags)
{
    return sys_call3(SYS_OPEN, (long)path, (long)flags, 0);
}

static long sys_close_fd(long fd)
{
    return sys_call3(SYS_CLOSE, fd, 0, 0);
}

static long sys_read_fd(long fd, void *buf, u64 len)
{
    return sys_call3(SYS_READ, fd, (long)buf, (long)len);
}

static long sys_write_fd(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_mount_now(const char *src, const char *dst, const char *fstype, u32 flags)
{
    return sys_call4(SYS_MOUNT, (long)src, (long)dst, (long)fstype, (long)flags);
}

static long sys_umount_now(const char *path)
{
    return sys_call1(SYS_UMOUNT, (long)path);
}

static long sys_chdir_now(const char *path)
{
    return sys_call1(SYS_CHDIR, (long)path);
}

static long sys_getcwd_now(char *buf, u32 len)
{
    return sys_call2(SYS_GETCWD, (long)buf, (long)len);
}

static long sys_unshare_now(u32 flags)
{
    return sys_call1(SYS_UNSHARE, (long)flags);
}

static long sys_pivot_root_now(const char *new_root, const char *old_root)
{
    return sys_call2(SYS_PIVOT_ROOT, (long)new_root, (long)old_root);
}

static long sys_fork_now(void)
{
    return user_svc0(SYS_FORK);
}

static long sys_waitpid_raw(long pid, int *status, u32 options, u64 timeout_ms)
{
    return sys_call4(SYS_WAITPID, pid, (long)status, options, (long)timeout_ms);
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

static int demo_contains(const char *haystack, const char *needle)
{
    u32 i;
    u32 j;

    if (!haystack || !needle || needle[0] == '\0')
        return 0;

    for (i = 0U; haystack[i] != '\0'; i++) {
        for (j = 0U; needle[j] != '\0' && haystack[i + j] == needle[j]; j++)
            ;
        if (needle[j] == '\0')
            return 1;
    }
    return 0;
}

static void demo_puts(const char *text)
{
    (void)sys_write_fd(1, text, demo_strlen(text));
}

static long demo_append_line(const char *path, const char *text)
{
    long fd = sys_open_path(path, O_CREAT | O_WRONLY | O_APPEND);

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

static int demo_expect_file_prefix(const char *path, const char *expected)
{
    char buf[96];
    long fd;
    long n;
    u32  i;

    fd = sys_open_path(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = sys_read_fd(fd, buf, sizeof(buf) - 1U);
    (void)sys_close_fd(fd);
    if (n <= 0)
        return -1;

    buf[(n < (long)(sizeof(buf) - 1U)) ? (u32)n : (sizeof(buf) - 1U)] = '\0';
    for (i = 0U; expected[i] != '\0'; i++) {
        if (buf[i] != expected[i])
            return -1;
    }
    return 0;
}

static int demo_expect_file_contains(const char *path, const char *needle)
{
    char buf[192];
    long fd;
    long n;

    fd = sys_open_path(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = sys_read_fd(fd, buf, sizeof(buf) - 1U);
    (void)sys_close_fd(fd);
    if (n <= 0)
        return -1;

    buf[(n < (long)(sizeof(buf) - 1U)) ? (u32)n : (sizeof(buf) - 1U)] = '\0';
    return demo_contains(buf, needle) ? 0 : -1;
}

static int demo_expect_open_fail(const char *path)
{
    long fd = sys_open_path(path, O_RDONLY);

    if (fd >= 0) {
        (void)sys_close_fd(fd);
        return -1;
    }
    return 0;
}

int main(void)
{
    static const char bind_result[]  = "bind-ok\n";
    static const char umount_result[] = "umount-ok\n";
    static const char fork_result[]  = "fork-ok\n";
    static const char pivot_result[] = "pivot-ok\n";
    static const char nsroot_magic[] = "ns-root\n";
    static const char boot_magic[]   = "Mount profile bootstrap:";
    static const char log_rel[]      = "NSDEMO.TXT";
    char              cwd[96];
    long              pid;
    int               status = 0;

    if (sys_unshare_now(CLONE_NEWNS) < 0)
        sys_exit_now(1);
    if (sys_mount_now("/data", "/mnt", "bind", MS_BIND) < 0)
        sys_exit_now(2);
    if (sys_chdir_now("/mnt") < 0)
        sys_exit_now(3);
    if (sys_getcwd_now(cwd, sizeof(cwd)) < 0 || !demo_streq(cwd, "/mnt"))
        sys_exit_now(4);
    if (demo_expect_file_prefix("NSROOT.TXT", nsroot_magic) < 0)
        sys_exit_now(5);
    if (demo_append_line(log_rel, bind_result) < 0)
        sys_exit_now(6);

    if (sys_mount_now((const char *)0, "/mnt/proc", "procfs", 0U) < 0)
        sys_exit_now(7);
    if (demo_expect_file_contains("/mnt/proc/self/status", "Pid:") < 0)
        sys_exit_now(8);
    if (sys_umount_now("/mnt/proc") < 0)
        sys_exit_now(9);
    if (demo_expect_open_fail("/mnt/proc/self/status") < 0)
        sys_exit_now(10);
    if (demo_append_line(log_rel, umount_result) < 0)
        sys_exit_now(11);

    pid = sys_fork_now();
    if (pid < 0)
        sys_exit_now(12);
    if (pid == 0) {
        if (sys_getcwd_now(cwd, sizeof(cwd)) < 0 || !demo_streq(cwd, "/mnt"))
            sys_exit_now(20);
        if (demo_expect_file_prefix("NSROOT.TXT", nsroot_magic) < 0)
            sys_exit_now(21);
        if (demo_append_line(log_rel, fork_result) < 0)
            sys_exit_now(22);
        sys_exit_now(0);
    }

    if (sys_waitpid_raw(pid, &status, 0U, 2000ULL) != pid)
        sys_exit_now(13);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        sys_exit_now(14);

    if (sys_mount_now((const char *)0, "/mnt/proc", "procfs", 0U) < 0)
        sys_exit_now(15);
    if (sys_pivot_root_now("/mnt", "/mnt/oldroot") < 0)
        sys_exit_now(16);
    if (sys_chdir_now("/") < 0)
        sys_exit_now(17);
    if (sys_getcwd_now(cwd, sizeof(cwd)) < 0 || !demo_streq(cwd, "/"))
        sys_exit_now(18);
    if (demo_expect_file_prefix("/NSROOT.TXT", nsroot_magic) < 0)
        sys_exit_now(19);
    if (demo_expect_file_contains("/oldroot/BOOT.TXT", boot_magic) < 0)
        sys_exit_now(23);
    if (demo_append_line("/NSDEMO.TXT", pivot_result) < 0)
        sys_exit_now(24);

    demo_puts("[NSDEMO] namespace mount/pivot OK\n");
    sys_exit_now(0);
}

void _start(void)
{
    sys_exit_now(main());
}
