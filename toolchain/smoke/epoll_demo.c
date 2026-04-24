#include <sys/epoll.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static int write_text(const char *path, const char *text)
{
    int    fd;
    size_t len = strlen(text);

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;
    if (write(fd, text, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    if (close(fd) < 0)
        return -1;
    return 0;
}

int main(void)
{
    static const char out[] = "epoll core ok\n";
    struct epoll_event ev;
    struct epoll_event events[2];
    char               c = 'x';
    int                fds[2];
    int                epfd;
    int                rc;

    if (pipe(fds) < 0)
        return 1;

    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        return 2;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLET;
    ev.data = 0x1111ULL;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fds[0], &ev) < 0)
        return 3;

    rc = epoll_wait(epfd, events, 2, 0);
    if (rc != 0)
        return 4;

    if (write(fds[1], &c, 1) != 1)
        return 5;

    rc = epoll_wait(epfd, events, 2, 50);
    if (rc != 1)
        return 6;
    if ((events[0].events & EPOLLIN) == 0U || events[0].data != 0x1111ULL)
        return 7;

    rc = epoll_wait(epfd, events, 2, 0);
    if (rc != 0)
        return 8;

    if (read(fds[0], &c, 1) != 1)
        return 9;

    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data = 0x2222ULL;
    if (epoll_ctl(epfd, EPOLL_CTL_MOD, fds[0], &ev) < 0)
        return 10;
    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fds[0], 0) < 0)
        return 11;

    if (write(fds[1], &c, 1) != 1)
        return 12;
    rc = epoll_wait(epfd, events, 2, 0);
    if (rc != 0)
        return 13;

    close(fds[0]);
    close(fds[1]);
    close(epfd);

    if (write_text("/data/EPOLLDEMO.TXT", out) < 0)
        return 14;
    return 0;
}
