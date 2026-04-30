#include <errno.h>
#include <stddef.h>
#include <sys/epoll.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

static int fd_requested(const fd_set *set, int fd)
{
    return (set && FD_ISSET(fd, set)) ? 1 : 0;
}

static int timeval_to_ms(const struct timeval *timeout)
{
    long long ms;

    if (!timeout)
        return -1;
    if (timeout->tv_sec < 0L || timeout->tv_usec < 0L || timeout->tv_usec >= 1000000L) {
        errno = EINVAL;
        return -2;
    }

    ms = (long long)timeout->tv_sec * 1000LL;
    ms += ((long long)timeout->tv_usec + 999LL) / 1000LL;
    if (ms > 2147483647LL)
        ms = 2147483647LL;
    return (int)ms;
}

static int timespec_to_ms(const struct timespec *timeout)
{
    long long ms;

    if (!timeout)
        return -1;
    if (timeout->tv_sec < 0L || timeout->tv_nsec < 0L || timeout->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -2;
    }

    ms = (long long)timeout->tv_sec * 1000LL;
    ms += ((long long)timeout->tv_nsec + 999999LL) / 1000000LL;
    if (ms > 2147483647LL)
        ms = 2147483647LL;
    return (int)ms;
}

static void clear_sets(fd_set *readfds, fd_set *writefds, fd_set *exceptfds)
{
    if (readfds)
        FD_ZERO(readfds);
    if (writefds)
        FD_ZERO(writefds);
    if (exceptfds)
        FD_ZERO(exceptfds);
}

static int sleep_timeout_ms(int timeout_ms)
{
    struct timespec req;

    if (timeout_ms < 0) {
        req.tv_sec = 1L;
        req.tv_nsec = 0L;
        for (;;)
            (void)nanosleep(&req, NULL);
    }

    req.tv_sec = timeout_ms / 1000;
    req.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    if (timeout_ms > 0)
        (void)nanosleep(&req, NULL);
    return 0;
}

int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
           struct timeval *timeout)
{
    fd_set              in_read;
    fd_set              in_write;
    fd_set              in_except;
    struct epoll_event  ev;
    struct epoll_event  out[FD_SETSIZE];
    unsigned char       touched[FD_SETSIZE];
    int                 epfd;
    int                 timeout_ms;
    int                 watched = 0;
    int                 ready_count = 0;

    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    timeout_ms = timeval_to_ms(timeout);
    if (timeout_ms == -2)
        return -1;

    if (readfds)
        in_read = *readfds;
    if (writefds)
        in_write = *writefds;
    if (exceptfds)
        in_except = *exceptfds;

    clear_sets(readfds, writefds, exceptfds);

    if (nfds == 0 || (!readfds && !writefds && !exceptfds))
        return sleep_timeout_ms(timeout_ms);

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return -1;

    for (int fd = 0; fd < nfds; fd++) {
        uint32_t events = 0U;

        if (fd_requested(readfds ? &in_read : NULL, fd))
            events |= EPOLLIN;
        if (fd_requested(writefds ? &in_write : NULL, fd))
            events |= EPOLLOUT;
        if (fd_requested(exceptfds ? &in_except : NULL, fd))
            events |= EPOLLPRI;
        if (events == 0U)
            continue;

        ev.events = events | EPOLLERR | EPOLLHUP;
        ev.data = (uint64_t)(unsigned)fd;
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            (void)close(epfd);
            return -1;
        }
        watched++;
    }

    if (watched == 0) {
        (void)close(epfd);
        return sleep_timeout_ms(timeout_ms);
    }

    for (int i = 0; i < FD_SETSIZE; i++)
        touched[i] = 0U;

    watched = epoll_wait(epfd, out, watched, timeout_ms);
    if (watched < 0) {
        (void)close(epfd);
        return -1;
    }

    for (int i = 0; i < watched; i++) {
        int      fd = (int)out[i].data;
        uint32_t events = out[i].events;
        int      marked = 0;

        if (fd < 0 || fd >= nfds || fd >= FD_SETSIZE)
            continue;

        if (readfds && fd_requested(&in_read, fd) &&
            (events & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) {
            FD_SET(fd, readfds);
            marked = 1;
        }
        if (writefds && fd_requested(&in_write, fd) &&
            (events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) != 0U) {
            FD_SET(fd, writefds);
            marked = 1;
        }
        if (exceptfds && fd_requested(&in_except, fd) &&
            (events & (EPOLLPRI | EPOLLERR | EPOLLHUP)) != 0U) {
            FD_SET(fd, exceptfds);
            marked = 1;
        }

        if (marked && touched[fd] == 0U) {
            touched[fd] = 1U;
            ready_count++;
        }
    }

    (void)close(epfd);
    return ready_count;
}

int pselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
            const struct timespec *timeout, const sigset_t *sigmask)
{
    struct timeval tv;
    int            timeout_ms;

    (void)sigmask;

    timeout_ms = timespec_to_ms(timeout);
    if (timeout_ms == -2)
        return -1;
    if (timeout_ms < 0)
        return select(nfds, readfds, writefds, exceptfds, (struct timeval *)NULL);

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (long)(timeout_ms % 1000) * 1000L;
    return select(nfds, readfds, writefds, exceptfds, &tv);
}
