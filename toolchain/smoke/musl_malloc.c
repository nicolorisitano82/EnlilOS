#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    static const char out[] = "malloc calloc realloc ok\n";
    char *a = (char *)malloc(64);
    char *b = (char *)calloc(32, 4);
    char *c;
    int   fd;

    if (!a || !b)
        return 1;

    memset(a, 'A', 64);
    if (b[0] != 0 || b[63] != 0)
        return 2;

    c = (char *)realloc(a, 160);
    if (!c)
        return 3;
    if (c[0] != 'A' || c[63] != 'A')
        return 4;
    memset(c + 64, 'B', 96);

    fd = open("/data/MUSLMALLOC.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 5;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 6;
    }
    if (close(fd) < 0)
        return 7;

    free(c);
    free(b);
    return 0;
}
