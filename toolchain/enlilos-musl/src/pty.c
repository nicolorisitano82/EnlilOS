/*
 * EnlilOS Bootstrap musl - Pseudo-Terminal POSIX API (M11-05f)
 *
 * posix_openpt / grantpt / unlockpt / ptsname / ptsname_r / openpty
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "enlil_syscalls.h"
#include "user_svc.h"

/* ── helpers interni ─────────────────────────────────────────────── */

static int set_errno_neg(long rc)
{
    if (rc < 0) { errno = (int)(-rc); return -1; }
    return (int)rc;
}

/* ── posix_openpt ────────────────────────────────────────────────── */

int posix_openpt(int flags)
{
    /* apre /dev/ptmx: il kernel alloca un PTY slot e ritorna master fd */
    long rc = user_svc3(SYS_OPENAT, (long)-100 /* AT_FDCWD */,
                        (long)"/dev/ptmx", (long)flags);
    return set_errno_neg(rc);
}

/* ── grantpt ─────────────────────────────────────────────────────── */

int grantpt(int fd)
{
    /* No-op su EnlilOS: il kernel non usa uid/gid su device nodes */
    (void)fd;
    return 0;
}

/* ── unlockpt ────────────────────────────────────────────────────── */

int unlockpt(int fd)
{
    int zero = 0;
    long rc = user_svc3(SYS_IOCTL, (long)fd, (long)TIOCSPTLCK, (long)&zero);
    return set_errno_neg(rc);
}

/* ── ptsname_r ───────────────────────────────────────────────────── */

int ptsname_r(int fd, char *buf, size_t len)
{
    unsigned int n = 0U;
    long rc;
    const char prefix[] = "/dev/pts/";
    size_t plen = sizeof(prefix) - 1U;

    if (!buf || len == 0U) { errno = EINVAL; return EINVAL; }

    rc = user_svc3(SYS_IOCTL, (long)fd, (long)TIOCGPTN, (long)&n);
    if (rc < 0) { errno = (int)(-rc); return (int)(-rc); }

    /* formato "/dev/pts/N" o "/dev/pts/NN" */
    if (n < 10U) {
        if (len < plen + 2U) { errno = ERANGE; return ERANGE; }
        memcpy(buf, prefix, plen);
        buf[plen]     = (char)('0' + n);
        buf[plen + 1] = '\0';
    } else {
        if (len < plen + 3U) { errno = ERANGE; return ERANGE; }
        memcpy(buf, prefix, plen);
        buf[plen]     = (char)('0' + n / 10U);
        buf[plen + 1] = (char)('0' + n % 10U);
        buf[plen + 2] = '\0';
    }
    return 0;
}

/* ── ptsname ─────────────────────────────────────────────────────── */

char *ptsname(int fd)
{
    static char buf[24];
    if (ptsname_r(fd, buf, sizeof(buf)) != 0)
        return (char *)0;
    return buf;
}

/* ── openpty ─────────────────────────────────────────────────────── */

int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp, const struct winsize *winp)
{
    int   master_fd, slave_fd;
    char  slave_path[24];
    int   rc;

    if (!amaster || !aslave) { errno = EINVAL; return -1; }

    master_fd = posix_openpt(O_RDWR);
    if (master_fd < 0) return -1;

    if (grantpt(master_fd) < 0 || unlockpt(master_fd) < 0) {
        user_svc1(SYS_CLOSE, (long)master_fd);
        return -1;
    }

    rc = ptsname_r(master_fd, slave_path, sizeof(slave_path));
    if (rc != 0) {
        user_svc1(SYS_CLOSE, (long)master_fd);
        errno = rc;
        return -1;
    }

    slave_fd = (int)user_svc3(SYS_OPENAT, (long)-100 /* AT_FDCWD */,
                              (long)slave_path, (long)O_RDWR);
    if (slave_fd < 0) {
        errno = -slave_fd;
        user_svc1(SYS_CLOSE, (long)master_fd);
        return -1;
    }

    if (termp) {
        /* tcsetattr(slave_fd, TCSAFLUSH, termp) */
        user_svc3(SYS_TCSETATTR, (long)slave_fd, (long)TCSAFLUSH, (long)termp);
    }
    if (winp) {
        /* ioctl(slave_fd, TIOCSWINSZ, winp) */
        user_svc3(SYS_IOCTL, (long)slave_fd, (long)TIOCSWINSZ, (long)winp);
    }
    if (name) {
        size_t slen = strlen(slave_path);
        memcpy(name, slave_path, slen + 1U);
    }

    *amaster = master_fd;
    *aslave  = slave_fd;
    return 0;
}
