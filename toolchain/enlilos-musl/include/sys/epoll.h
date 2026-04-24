#ifndef ENLILOS_MUSL_SYS_EPOLL_H
#define ENLILOS_MUSL_SYS_EPOLL_H

#include <stdint.h>
#include <signal.h>

#define EPOLL_CLOEXEC 0x00001000U

#define EPOLLIN       0x0001U
#define EPOLLPRI      0x0002U
#define EPOLLOUT      0x0004U
#define EPOLLERR      0x0008U
#define EPOLLHUP      0x0010U
#define EPOLLRDHUP    0x2000U
#define EPOLLET       0x80000000U

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

struct epoll_event {
    uint32_t events;
    uint32_t _pad;
    uint64_t data;
};

int epoll_create(int size);
int epoll_create1(int flags);
int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
int epoll_pwait(int epfd, struct epoll_event *events, int maxevents, int timeout,
                const sigset_t *sigmask);

#endif
