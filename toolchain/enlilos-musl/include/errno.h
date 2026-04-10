#ifndef ENLILOS_MUSL_ERRNO_H
#define ENLILOS_MUSL_ERRNO_H

extern __thread int errno;

#define EPERM               1
#define ENOENT              2
#define ESRCH               3
#define EINTR               4
#define EIO                 5
#define EBADF               9
#define ECHILD              10
#define EAGAIN              11
#define ENOMEM              12
#define EFAULT              14
#define EBUSY               16
#define EEXIST              17
#define EXDEV               18
#define ENOTDIR             20
#define EISDIR              21
#define EINVAL              22
#define ENFILE              23
#define ENOTTY              25
#define ENOSPC              28
#define ESPIPE              29
#define EROFS               30
#define EPIPE               32
#define ERANGE              34
#define EDEADLK             35
#define ENOSYS              38
#define ENOTEMPTY           39
#define ENAMETOOLONG        36
#define ETIMEDOUT           110

#endif
