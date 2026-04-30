#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "enlil_syscalls.h"
#include "user_svc.h"

typedef struct {
    uint32_t st_mode;
    uint32_t st_blksize;
    uint64_t st_size;
    uint64_t st_blocks;
} enlil_kstat_t;

static mode_t g_umask = 0022U;
extern char **environ;
static char g_login_name[] = "user";
static char g_tty_name[] = "/dev/tty";
static struct passwd g_passwd = {
    .pw_name = g_login_name,
    .pw_passwd = (char *)"x",
    .pw_uid = 0U,
    .pw_gid = 0U,
    .pw_gecos = (char *)"EnlilOS User",
    .pw_dir = (char *)"/home/user",
    .pw_shell = (char *)"/bin/bash",
};
static struct rlimit g_rlimits[10] = {
    [RLIMIT_CPU]     = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_FSIZE]   = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_DATA]    = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_STACK]   = { 8UL * 1024UL * 1024UL, 8UL * 1024UL * 1024UL },
    [RLIMIT_CORE]    = { 0UL, 0UL },
    [RLIMIT_RSS]     = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_NPROC]   = { 96UL, 96UL },
    [RLIMIT_NOFILE]  = { 64UL, 64UL },
    [RLIMIT_MEMLOCK] = { RLIM_INFINITY, RLIM_INFINITY },
    [RLIMIT_AS]      = { RLIM_INFINITY, RLIM_INFINITY },
};

static int posix_set_errno(long rc)
{
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return 0;
}

static unsigned long stat_hash_path(const char *path)
{
    unsigned long hash = 1469598103934665603UL;

    if (!path)
        return 0UL;

    while (*path != '\0') {
        hash ^= (unsigned long)(unsigned char)(*path++);
        hash *= 1099511628211UL;
    }
    return hash ? hash : 1UL;
}

static unsigned long stat_mount_dev(const char *path)
{
    if (!path || path[0] != '/')
        return 0UL;
    if (strncmp(path, "/dev", 4) == 0 && (path[4] == '\0' || path[4] == '/'))
        return 2UL;
    if (strncmp(path, "/data", 5) == 0 && (path[5] == '\0' || path[5] == '/'))
        return 3UL;
    if (strncmp(path, "/sysroot", 8) == 0 && (path[8] == '\0' || path[8] == '/'))
        return 4UL;
    if (strncmp(path, "/proc", 5) == 0 && (path[5] == '\0' || path[5] == '/'))
        return 5UL;
    return 1UL;
}

static void stat_translate(struct stat *out, const enlil_kstat_t *kst,
                           const char *path_hint, int fd_hint)
{
    if (!out || !kst)
        return;

    memset(out, 0, sizeof(*out));
    out->st_mode = (mode_t)kst->st_mode;
    out->st_blksize = (blksize_t)kst->st_blksize;
    out->st_blocks = (blkcnt_t)kst->st_blocks;
    out->st_size = (off_t)kst->st_size;
    out->st_uid = geteuid();
    out->st_gid = getegid();
    out->st_nlink = S_ISDIR(out->st_mode) ? 2UL : 1UL;
    if (path_hint) {
        out->st_dev = stat_mount_dev(path_hint);
        out->st_ino = stat_hash_path(path_hint);
    } else {
        out->st_dev = 0UL;
        out->st_ino = (ino_t)((unsigned long)(fd_hint + 1));
    }
}

int fstat(int fd, struct stat *st)
{
    enlil_kstat_t kst;
    long          rc;

    if (!st) {
        errno = EFAULT;
        return -1;
    }

    rc = user_svc2(SYS_FSTAT, fd, (long)&kst);
    if (posix_set_errno(rc) < 0)
        return -1;

    stat_translate(st, &kst, NULL, fd);
    return 0;
}

int fstatat(int dirfd, const char *path, struct stat *st, int flags)
{
    enlil_kstat_t kst;
    long          rc;

    if (!st) {
        errno = EFAULT;
        return -1;
    }

    rc = user_svc4(SYS_FSTATAT, dirfd, (long)path, (long)&kst, flags);
    if (posix_set_errno(rc) < 0)
        return -1;

    stat_translate(st, &kst, path, -1);
    return 0;
}

int stat(const char *path, struct stat *st)
{
    return fstatat(AT_FDCWD, path, st, 0);
}

int lstat(const char *path, struct stat *st)
{
    return fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
}

int mkdir(const char *path, mode_t mode)
{
    long rc;

    rc = user_svc2(SYS_MKDIR, (long)path, (long)mode);
    return posix_set_errno(rc);
}

int mkfifo(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    errno = ENOSYS;
    return -1;
}

int tcflow(int fd, int action)
{
    if (!isatty(fd)) {
        if (errno == 0)
            errno = ENOTTY;
        return -1;
    }
    if (action != TCOOFF && action != TCOON) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

int fchmod(int fd, mode_t mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

int access(const char *path, int mode)
{
    struct stat st;
    mode_t      bits;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (stat(path, &st) < 0)
        return -1;
    if (mode == F_OK)
        return 0;

    bits = st.st_mode;
    if ((mode & R_OK) != 0 && (bits & (S_IRUSR | S_IRGRP | S_IROTH)) == 0U) {
        errno = EACCES;
        return -1;
    }
    if ((mode & W_OK) != 0 && (bits & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0U) {
        errno = EACCES;
        return -1;
    }
    if ((mode & X_OK) != 0 && (bits & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0U) {
        errno = EACCES;
        return -1;
    }
    return 0;
}

int gethostname(char *name, int len)
{
    static const char host[] = "enlilos";

    if (!name || len <= 0) {
        errno = EINVAL;
        return -1;
    }
    if ((size_t)len <= sizeof(host) - 1U) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(name, host, sizeof(host));
    return 0;
}

long sysconf(int name)
{
    switch (name) {
    case _SC_NPROCESSORS_ONLN:
        return 1L;
    default:
        errno = EINVAL;
        return -1L;
    }
}

int unlink(const char *path)
{
    long rc;

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    rc = user_svc1(SYS_UNLINK, (long)path);
    return posix_set_errno(rc);
}

int remove(const char *path)
{
    return unlink(path);
}

int rename(const char *oldpath, const char *newpath)
{
    long rc;

    if (!oldpath || !newpath) {
        errno = EFAULT;
        return -1;
    }

    rc = user_svc2(SYS_RENAME, (long)oldpath, (long)newpath);
    return posix_set_errno(rc);
}

int usleep(unsigned int usec)
{
    struct timespec ts;

    ts.tv_sec = (long)(usec / 1000000U);
    ts.tv_nsec = (long)((usec % 1000000U) * 1000U);
    return nanosleep(&ts, NULL);
}

unsigned int sleep(unsigned int sec)
{
    struct timespec ts;

    ts.tv_sec = (long)sec;
    ts.tv_nsec = 0L;
    (void)nanosleep(&ts, NULL);
    return 0U;
}

unsigned int alarm(unsigned int sec)
{
    (void)sec;
    return 0U;
}

int execvp(const char *file, char *const argv[])
{
    static const char default_path[] = "/bin:/usr/bin:/usr/local/bin";
    char              candidate[256];
    char              path_copy[256];
    char             *cursor;
    char             *save = NULL;
    const char       *path_env;

    if (!file || file[0] == '\0') {
        errno = ENOENT;
        return -1;
    }

    if (strchr(file, '/') != NULL)
        return execve(file, argv, environ);

    path_env = getenv("PATH");
    if (!path_env || path_env[0] == '\0')
        path_env = default_path;

    snprintf(path_copy, sizeof(path_copy), "%s", path_env);
    cursor = strtok_r(path_copy, ":", &save);
    while (cursor) {
        if (cursor[0] != '\0')
            snprintf(candidate, sizeof(candidate), "%s/%s", cursor, file);
        else
            snprintf(candidate, sizeof(candidate), "%s", file);

        execve(candidate, argv, environ);
        if (errno != ENOENT && errno != ENOTDIR)
            return -1;
        cursor = strtok_r(NULL, ":", &save);
    }

    errno = ENOENT;
    return -1;
}

char *ttyname(int fd)
{
    if (fd < 0 || !isatty(fd)) {
        errno = ENOTTY;
        return NULL;
    }
    return g_tty_name;
}

char *getlogin(void)
{
    return g_login_name;
}

int setuid(uid_t uid)
{
    if (uid != getuid() && uid != geteuid()) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

int setgid(gid_t gid)
{
    if (gid != getgid() && gid != getegid()) {
        errno = EPERM;
        return -1;
    }
    return 0;
}

struct passwd *getpwuid(uid_t uid)
{
    if (uid != g_passwd.pw_uid) {
        errno = ENOENT;
        return NULL;
    }
    return &g_passwd;
}

struct passwd *getpwnam(const char *name)
{
    if (!name || strcmp(name, g_passwd.pw_name) != 0) {
        errno = ENOENT;
        return NULL;
    }
    return &g_passwd;
}

mode_t umask(mode_t mask)
{
    mode_t old = g_umask;

    g_umask = (mode_t)(mask & 0777U);
    return old;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    if (!rlim || resource < 0 || resource >= (int)(sizeof(g_rlimits) / sizeof(g_rlimits[0]))) {
        errno = EINVAL;
        return -1;
    }

    *rlim = g_rlimits[resource];
    return 0;
}

int setrlimit(int resource, const struct rlimit *rlim)
{
    if (!rlim || resource < 0 || resource >= (int)(sizeof(g_rlimits) / sizeof(g_rlimits[0]))) {
        errno = EINVAL;
        return -1;
    }

    g_rlimits[resource] = *rlim;
    return 0;
}
