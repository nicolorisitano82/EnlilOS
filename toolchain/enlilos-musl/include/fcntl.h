#ifndef ENLILOS_MUSL_FCNTL_H
#define ENLILOS_MUSL_FCNTL_H

#include <stdarg.h>
#include <sys/types.h>

#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     (1 << 6)
#define O_EXCL      (1 << 7)
#define O_TRUNC     (1 << 9)
#define O_APPEND    (1 << 10)
#define O_NONBLOCK  (1 << 11)
#define O_CLOEXEC   (1 << 12)

#define AT_FDCWD    (-100)
#define AT_SYMLINK_NOFOLLOW  0x0100
#define AT_EMPTY_PATH        0x1000

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

#define FD_CLOEXEC          1

#define F_DUPFD             0
#define F_GETFD             1
#define F_SETFD             2
#define F_GETFL             3
#define F_SETFL             4
#define F_DUPFD_CLOEXEC     1030

int open(const char *path, int flags, ...);
int openat(int dirfd, const char *path, int flags, ...);
int fcntl(int fd, int cmd, ...);

#endif
