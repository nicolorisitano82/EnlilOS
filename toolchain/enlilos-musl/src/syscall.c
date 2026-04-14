#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "enlil_syscalls.h"
#include "user_svc.h"

extern char **environ;
static void (*g_atexit_handlers[16])(void);
static unsigned g_atexit_count;

static long libc_set_errno(long rc)
{
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return rc;
}

ssize_t read(int fd, void *buf, size_t count)
{
    return (ssize_t)libc_set_errno(user_svc3(SYS_READ, fd, (long)buf, (long)count));
}

ssize_t write(int fd, const void *buf, size_t count)
{
    return (ssize_t)libc_set_errno(user_svc3(SYS_WRITE, fd, (long)buf, (long)count));
}

int close(int fd)
{
    return (int)libc_set_errno(user_svc1(SYS_CLOSE, fd));
}

off_t lseek(int fd, off_t offset, int whence)
{
    return (off_t)libc_set_errno(user_svc3(SYS_LSEEK, fd, (long)offset, whence));
}

int openat(int dirfd, const char *path, int flags, ...)
{
    mode_t  mode = 0U;
    va_list ap;

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return (int)libc_set_errno(user_svc4(SYS_OPENAT, dirfd, (long)path, flags, mode));
}

int open(const char *path, int flags, ...)
{
    mode_t  mode = 0U;
    va_list ap;

    if (flags & O_CREAT) {
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    return openat(AT_FDCWD, path, flags, mode);
}

int fcntl(int fd, int cmd, ...)
{
    long    arg = 0L;
    va_list ap;

    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    return (int)libc_set_errno(user_svc3(SYS_FCNTL, fd, cmd, arg));
}

ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
{
    return (ssize_t)libc_set_errno(user_svc3(SYS_READV, fd, (long)iov, iovcnt));
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    return (ssize_t)libc_set_errno(user_svc3(SYS_WRITEV, fd, (long)iov, iovcnt));
}

int ioctl(int fd, unsigned long req, ...)
{
    long    arg = 0L;
    va_list ap;

    va_start(ap, req);
    arg = va_arg(ap, long);
    va_end(ap);
    return (int)libc_set_errno(user_svc3(SYS_IOCTL, fd, (long)req, arg));
}

int uname(struct utsname *buf)
{
    return (int)libc_set_errno(user_svc1(SYS_UNAME, (long)buf));
}

pid_t getpid(void)
{
    return (pid_t)user_svc0(SYS_GETPID);
}

pid_t gettid(void)
{
    return (pid_t)user_svc0(SYS_GETTID);
}

pid_t getppid(void)
{
    return (pid_t)user_svc0(SYS_GETPPID);
}

uid_t getuid(void)
{
    return (uid_t)user_svc0(SYS_GETUID);
}

gid_t getgid(void)
{
    return (gid_t)user_svc0(SYS_GETGID);
}

uid_t geteuid(void)
{
    return (uid_t)user_svc0(SYS_GETEUID);
}

gid_t getegid(void)
{
    return (gid_t)user_svc0(SYS_GETEGID);
}

int gettimeofday(struct timeval *tv, void *tz)
{
    return (int)libc_set_errno(user_svc2(SYS_GETTIMEOFDAY, (long)tv, (long)tz));
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    return (int)libc_set_errno(user_svc3(SYS_SIGACTION, sig, (long)act, (long)old));
}

int sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    return (int)libc_set_errno(user_svc3(SYS_SIGPROCMASK, how, (long)set, (long)old));
}

int kill(pid_t pid, int sig)
{
    return (int)libc_set_errno(user_svc2(SYS_KILL, pid, sig));
}

sighandler_t signal(int sig, sighandler_t handler)
{
    struct sigaction act;
    struct sigaction old;

    act.sa_handler = handler;
    act.sa_mask = 0ULL;
    act.sa_flags = 0U;
    act.__pad = 0U;

    if (sigaction(sig, &act, &old) < 0)
        return SIG_ERR;
    return old.sa_handler;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    return (int)libc_set_errno(user_svc2(SYS_NANOSLEEP, (long)req, (long)rem));
}

int isatty(int fd)
{
    long rc = user_svc1(SYS_ISATTY, fd);

    if (rc < 0)
        return (int)libc_set_errno(rc);
    return rc ? 1 : 0;
}

int tcgetattr(int fd, struct termios *term)
{
    return (int)libc_set_errno(user_svc2(SYS_TCGETATTR, fd, (long)term));
}

int tcsetattr(int fd, int action, const struct termios *term)
{
    return (int)libc_set_errno(user_svc3(SYS_TCSETATTR, fd, action, (long)term));
}

int chdir(const char *path)
{
    return (int)libc_set_errno(user_svc1(SYS_CHDIR, (long)path));
}

char *getcwd(char *buf, size_t size)
{
    long rc = user_svc2(SYS_GETCWD, (long)buf, (long)size);

    if (rc < 0) {
        (void)libc_set_errno(rc);
        return NULL;
    }
    return buf;
}

int pipe(int fds[2])
{
    return (int)libc_set_errno(user_svc1(SYS_PIPE, (long)fds));
}

int dup(int oldfd)
{
    return (int)libc_set_errno(user_svc1(SYS_DUP, oldfd));
}

int dup2(int oldfd, int newfd)
{
    return (int)libc_set_errno(user_svc2(SYS_DUP2, oldfd, newfd));
}

int setpgid(pid_t pid, pid_t pgid)
{
    return (int)libc_set_errno(user_svc2(SYS_SETPGID, pid, pgid));
}

pid_t getpgid(pid_t pid)
{
    return (pid_t)libc_set_errno(user_svc1(SYS_GETPGID, pid));
}

pid_t getpgrp(void)
{
    return getpgid(0);
}

pid_t setsid(void)
{
    return (pid_t)libc_set_errno(user_svc0(SYS_SETSID));
}

pid_t getsid(pid_t pid)
{
    return (pid_t)libc_set_errno(user_svc1(SYS_GETSID, pid));
}

int tcsetpgrp(int fd, pid_t pgrp)
{
    return (int)libc_set_errno(user_svc2(SYS_TCSETPGRP, fd, pgrp));
}

pid_t tcgetpgrp(int fd)
{
    return (pid_t)libc_set_errno(user_svc1(SYS_TCGETPGRP, fd));
}

pid_t fork(void)
{
    return (pid_t)libc_set_errno(user_svc0(SYS_FORK));
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    char *const *use_envp = envp ? envp : environ;
    return (int)libc_set_errno(user_svc3(SYS_EXECVE, (long)path,
                                         (long)argv, (long)use_envp));
}

pid_t waitpid(pid_t pid, int *status, int options)
{
    options &= ~WCONTINUED;
    return (pid_t)libc_set_errno(user_svc4(SYS_WAITPID, pid,
                                           (long)status, options, 0));
}

void _Exit(int status)
{
    user_svc_exit(status, SYS_EXIT_GROUP);
}

void _exit(int status)
{
    user_svc_exit(status, SYS_EXIT_GROUP);
}

int atexit(void (*function)(void))
{
    if (!function || g_atexit_count >= (sizeof(g_atexit_handlers) / sizeof(g_atexit_handlers[0]))) {
        errno = ENOMEM;
        return -1;
    }

    g_atexit_handlers[g_atexit_count++] = function;
    return 0;
}

void exit(int status)
{
    while (g_atexit_count > 0U) {
        void (*fn)(void) = g_atexit_handlers[--g_atexit_count];

        if (fn)
            fn();
    }
    user_svc_exit(status, SYS_EXIT_GROUP);
}

void abort(void)
{
    static const char msg[] = "abort\n";
    (void)write(STDERR_FILENO, msg, sizeof(msg) - 1U);
    user_svc_exit(127, SYS_EXIT_GROUP);
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    long rc = user_svc6(SYS_MMAP, (long)addr, (long)length, prot,
                        flags, fd, (long)offset);
    if ((uint64_t)rc == MAP_FAILED_VA) {
        errno = ENOMEM;
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)rc;
}

int munmap(void *addr, size_t length)
{
    return (int)libc_set_errno(user_svc2(SYS_MUNMAP, (long)addr, (long)length));
}
