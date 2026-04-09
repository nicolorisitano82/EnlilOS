#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
    static const char out[] = "fork exec wait ok\n";
    static char *const child_argv[] = { "/MUSLHELLO.ELF", NULL };
    char   buf[32];
    int    fd;
    int    status = 0;
    pid_t  pid;

    pid = fork();
    if (pid < 0)
        return 1;

    if (pid == 0) {
        execve("/MUSLHELLO.ELF", child_argv, environ);
        _exit(111);
    }

    if (waitpid(pid, &status, 0) != pid)
        return 2;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return 3;

    fd = open("/data/MUSLHELLO.TXT", O_RDONLY, 0);
    if (fd < 0)
        return 4;
    if (read(fd, buf, sizeof(buf)) <= 0) {
        close(fd);
        return 5;
    }
    close(fd);
    if (strncmp(buf, "musl-hello-ok\n", 15) != 0)
        return 6;

    fd = open("/data/MUSLFORK.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 7;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 8;
    }
    if (close(fd) < 0)
        return 9;
    return 0;
}
