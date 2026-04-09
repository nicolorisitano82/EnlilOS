#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

int main(void)
{
    static const char out[] = "pipe dup termios ok\n";
    int            fds[2];
    int            dupfd;
    char           buf[32];
    struct termios term;
    int            fd;

    if (pipe(fds) < 0)
        return 1;
    dupfd = dup2(fds[1], 9);
    if (dupfd != 9)
        return 2;
    close(fds[1]);
    if (write(dupfd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U))
        return 3;
    if (read(fds[0], buf, sizeof(buf)) != (ssize_t)(sizeof(out) - 1U))
        return 4;
    if (memcmp(buf, out, sizeof(out) - 1U) != 0)
        return 5;
    if (!isatty(STDOUT_FILENO))
        return 6;
    if (tcgetattr(STDOUT_FILENO, &term) < 0)
        return 7;
    if (tcsetattr(STDOUT_FILENO, TCSANOW, &term) < 0)
        return 8;

    fd = open("/data/MUSLPIPE.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 9;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 10;
    }
    if (close(fd) < 0)
        return 11;

    close(dupfd);
    close(fds[0]);
    return 0;
}
