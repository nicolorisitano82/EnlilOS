#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/time.h>

#include "enlil_syscalls.h"
#include "user_svc.h"

#define ENLIL_SEM_FLAG_NAMED  0x0001U
#define ENLIL_SEM_FLAG_VALID  0x0002U
#define ENLIL_KSEM_PRIVATE    (1U << 0)
#define ENLIL_KSEM_SHARED     (1U << 1)

static int sem_store_errno(long rc)
{
    if (rc < 0) {
        errno = (int)-rc;
        return -1;
    }
    return (int)rc;
}

static int sem_validate(sem_t *sem)
{
    if (!sem || (sem->__flags & ENLIL_SEM_FLAG_VALID) == 0U || sem->__handle == 0U) {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int sem_deadline_to_relative_ns(const struct timespec *abstime,
                                       unsigned long long *timeout_ns)
{
    struct timeval tv;
    long long      now_ns;
    long long      abs_ns;
    long long      delta;

    if (!abstime || !timeout_ns || abstime->tv_sec < 0L ||
        abstime->tv_nsec < 0L || abstime->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    if (gettimeofday(&tv, NULL) < 0)
        return -1;

    now_ns = ((long long)tv.tv_sec * 1000000000LL)
           + ((long long)tv.tv_usec * 1000LL);
    abs_ns = ((long long)abstime->tv_sec * 1000000000LL)
           + (long long)abstime->tv_nsec;
    delta = abs_ns - now_ns;
    if (delta <= 0LL) {
        *timeout_ns = 0ULL;
        errno = ETIMEDOUT;
        return -1;
    }

    *timeout_ns = (unsigned long long)delta;
    return 0;
}

int sem_init(sem_t *sem, int pshared, unsigned int value)
{
    long         rc;
    unsigned int flags = (pshared != 0) ? ENLIL_KSEM_SHARED : ENLIL_KSEM_PRIVATE;

    if (!sem) {
        errno = EINVAL;
        return -1;
    }

    rc = user_svc2(SYS_KSEM_ANON, (long)value, (long)flags);
    if (sem_store_errno(rc) < 0)
        return -1;

    sem->__handle = (unsigned int)rc;
    sem->__flags = ENLIL_SEM_FLAG_VALID;
    return 0;
}

int sem_destroy(sem_t *sem)
{
    int rc;

    if (sem_validate(sem) < 0)
        return -1;
    if ((sem->__flags & ENLIL_SEM_FLAG_NAMED) != 0U) {
        errno = EINVAL;
        return -1;
    }

    rc = sem_store_errno(user_svc1(SYS_KSEM_CLOSE, (long)sem->__handle));
    if (rc < 0)
        return -1;
    sem->__handle = 0U;
    sem->__flags = 0U;
    return 0;
}

sem_t *sem_open(const char *name, int oflag, ...)
{
    unsigned int value = 0U;
    sem_t       *sem;
    long         rc;
    va_list      ap;

    if (!name) {
        errno = EINVAL;
        return SEM_FAILED;
    }

    if ((oflag & O_CREAT) != 0) {
        va_start(ap, oflag);
        (void)va_arg(ap, int);
        value = (unsigned int)va_arg(ap, unsigned int);
        va_end(ap);
    }

    sem = (sem_t *)calloc(1U, sizeof(*sem));
    if (!sem) {
        errno = ENOMEM;
        return SEM_FAILED;
    }

    if ((oflag & O_CREAT) != 0) {
        rc = user_svc3(SYS_KSEM_CREATE, (long)name, (long)value, (long)ENLIL_KSEM_SHARED);
        if (rc < 0 && (int)-rc == EEXIST)
            rc = user_svc2(SYS_KSEM_OPEN, (long)name, (long)ENLIL_KSEM_SHARED);
    } else {
        rc = user_svc2(SYS_KSEM_OPEN, (long)name, (long)ENLIL_KSEM_SHARED);
    }
    if (sem_store_errno(rc) < 0) {
        free(sem);
        return SEM_FAILED;
    }

    sem->__handle = (unsigned int)rc;
    sem->__flags = ENLIL_SEM_FLAG_VALID | ENLIL_SEM_FLAG_NAMED;
    return sem;
}

int sem_close(sem_t *sem)
{
    int rc;

    if (sem_validate(sem) < 0)
        return -1;
    if ((sem->__flags & ENLIL_SEM_FLAG_NAMED) == 0U) {
        errno = EINVAL;
        return -1;
    }

    rc = sem_store_errno(user_svc1(SYS_KSEM_CLOSE, (long)sem->__handle));
    if (rc < 0)
        return -1;
    sem->__handle = 0U;
    sem->__flags = 0U;
    free(sem);
    return 0;
}

int sem_unlink(const char *name)
{
    if (!name) {
        errno = EINVAL;
        return -1;
    }
    return sem_store_errno(user_svc1(SYS_KSEM_UNLINK, (long)name));
}

int sem_post(sem_t *sem)
{
    if (sem_validate(sem) < 0)
        return -1;
    return sem_store_errno(user_svc1(SYS_KSEM_POST, (long)sem->__handle));
}

int sem_wait(sem_t *sem)
{
    long rc;

    if (sem_validate(sem) < 0)
        return -1;

    do {
        rc = user_svc1(SYS_KSEM_WAIT, (long)sem->__handle);
    } while (rc < 0 && (int)-rc == EINTR);

    return sem_store_errno(rc);
}

int sem_trywait(sem_t *sem)
{
    if (sem_validate(sem) < 0)
        return -1;
    return sem_store_errno(user_svc1(SYS_KSEM_TRYWAIT, (long)sem->__handle));
}

int sem_timedwait(sem_t *sem, const struct timespec *abstime)
{
    unsigned long long timeout_ns = 0ULL;

    if (sem_validate(sem) < 0)
        return -1;
    if (sem_deadline_to_relative_ns(abstime, &timeout_ns) < 0) {
        if (errno == ETIMEDOUT) {
            if (user_svc1(SYS_KSEM_TRYWAIT, (long)sem->__handle) >= 0)
                return 0;
            errno = ETIMEDOUT;
        }
        return -1;
    }
    return sem_store_errno(user_svc2(SYS_KSEM_TIMEDWAIT, (long)sem->__handle,
                                     (long)timeout_ns));
}

int sem_getvalue(sem_t *sem, int *value)
{
    if (sem_validate(sem) < 0)
        return -1;
    if (!value) {
        errno = EINVAL;
        return -1;
    }
    return sem_store_errno(user_svc2(SYS_KSEM_GETVALUE, (long)sem->__handle, (long)value));
}
