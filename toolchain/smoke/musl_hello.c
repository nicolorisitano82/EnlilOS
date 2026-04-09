#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    static const char out[] = "musl-hello-ok\n";
    int fd;

    puts("[musl] hello");
    fd = open("/data/MUSLHELLO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 1;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 2;
    }
    if (close(fd) < 0)
        return 3;
    return 0;
}
