#include <sys/epoll.h>
#include <errno.h>

#include "user_svc.h"
#include "enlil_syscalls.h"

static int epoll_errno(long rc)
{
    if (rc < 0) {
        errno = (int)(-rc);
        return -1;
    }
    return (int)rc;
}

int epoll_create(int size)
{
    (void)size;
    return epoll_create1(0);
}

int epoll_create1(int flags)
{
    return epoll_errno(user_svc1(SYS_EPOLL_CREATE1, flags));
}

int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    return epoll_errno(user_svc4(SYS_EPOLL_CTL, epfd, op, fd, (long)event));
}

int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout,
                const sigset_t *sigmask)
{
    return epoll_errno(user_svc5(SYS_EPOLL_PWAIT, epfd, (long)events,
                                 maxevents, timeout, (long)sigmask));
}

int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    return epoll_pwait(epfd, events, maxevents, timeout, (const sigset_t *)0);
}
