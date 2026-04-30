#ifndef ENLILOS_MUSL_SYS_SELECT_H
#define ENLILOS_MUSL_SYS_SELECT_H

#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>

#ifndef FD_SETSIZE
#define FD_SETSIZE 256
#endif

#define __NFDBITS   (8U * (unsigned)sizeof(unsigned long))
#define __FD_ELT(d) ((unsigned)(d) / __NFDBITS)
#define __FD_MASK(d) (1UL << ((unsigned)(d) % __NFDBITS))

typedef struct {
    unsigned long fds_bits[(FD_SETSIZE + __NFDBITS - 1U) / __NFDBITS];
} fd_set;

#define FD_ZERO(set)                                                     \
    do {                                                                 \
        unsigned __i;                                                    \
        for (__i = 0; __i < (unsigned)(sizeof((set)->fds_bits) / sizeof((set)->fds_bits[0])); __i++) \
            (set)->fds_bits[__i] = 0UL;                                  \
    } while (0)

#define FD_SET(fd, set)                                                  \
    do {                                                                 \
        if ((unsigned)(fd) < FD_SETSIZE)                                 \
            (set)->fds_bits[__FD_ELT(fd)] |= __FD_MASK(fd);              \
    } while (0)

#define FD_CLR(fd, set)                                                  \
    do {                                                                 \
        if ((unsigned)(fd) < FD_SETSIZE)                                 \
            (set)->fds_bits[__FD_ELT(fd)] &= ~__FD_MASK(fd);             \
    } while (0)

#define FD_ISSET(fd, set)                                                \
    (((unsigned)(fd) < FD_SETSIZE) &&                                    \
     (((set)->fds_bits[__FD_ELT(fd)] & __FD_MASK(fd)) != 0UL))

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout);
int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask);

#endif
