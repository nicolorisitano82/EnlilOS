#include "syscall.h"
#include "user_svc.h"

typedef unsigned long u64;

extern const char *dyn_msg(void);
extern u64 dyn_len(void);

__attribute__((noreturn)) static void sys_exit(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

static long sys_open(const char *path, long flags, long mode)
{
    return user_svc4(SYS_OPENAT, AT_FDCWD, (long)path, flags, mode);
}

static long sys_write(long fd, const void *buf, u64 len)
{
    return user_svc3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_close(long fd)
{
    return user_svc1(SYS_CLOSE, fd);
}

static u64 cstrlen(const char *s)
{
    u64 n = 0UL;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static int cstreq_prefix(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static const char *find_bundle_root(u64 *sp)
{
    u64 argc = *sp++;
    u64 i;

    sp += argc;
    if (*sp == 0UL)
        sp++;

    for (i = 0UL; sp[i] != 0UL; i++) {
        const char *env = (const char *)(unsigned long)sp[i];

        if (env && cstreq_prefix(env, "ENLIL_BUNDLE_ROOT="))
            return env + 18;
    }
    return 0;
}

static int write_full(long fd, const char *buf, u64 len)
{
    while (len > 0UL) {
        long rc = sys_write(fd, buf, len);

        if (rc <= 0)
            return -1;
        buf += (u64)rc;
        len -= (u64)rc;
    }
    return 0;
}

void enlil_bundle_main(u64 *sp)
{
    const char *bundle_root = find_bundle_root(sp);
    const char *msg = dyn_msg();
    u64         msg_len = dyn_len();
    static const char path[] = "/data/ENLILBUNDLE.TXT";
    static const char prefix[] = "bundle-root=";
    static const char newline[] = "\n";
    long fd;

    if (!bundle_root || !msg || msg_len == 0UL)
        sys_exit(10);

    fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        sys_exit(11);

    if (write_full(fd, prefix, sizeof(prefix) - 1U) < 0 ||
        write_full(fd, bundle_root, cstrlen(bundle_root)) < 0 ||
        write_full(fd, newline, sizeof(newline) - 1U) < 0 ||
        write_full(fd, msg, msg_len) < 0) {
        (void)sys_close(fd);
        sys_exit(12);
    }

    (void)sys_close(fd);
    sys_exit(0);
}
