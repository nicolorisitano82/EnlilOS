#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    char buf[96];
    int  fd;
    int  len;

    printf("[musl] stdio smoke\n");
    len = snprintf(buf, sizeof(buf), "stdio value=%d hex=%x ok\n", 42, 42);
    if (len <= 0 || (size_t)len >= sizeof(buf))
        return 1;

    fd = open("/data/MUSLSTDIO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 2;
    if (write(fd, buf, (size_t)len) != (ssize_t)len) {
        close(fd);
        return 3;
    }
    if (close(fd) < 0)
        return 4;
    return 0;
}
