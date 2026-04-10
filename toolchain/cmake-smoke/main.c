#include <fcntl.h>
#include <glob.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#if !defined(ENLILOS) || (ENLILOS != 1)
#error "ENLILOS=1 must be provided by tools/enlilos-aarch64.cmake"
#endif

static int write_full(int fd, const char *buf, size_t len)
{
    size_t off = 0U;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

static int glob_has(const glob_t *g, const char *path)
{
    size_t i;

    if (!g || !path)
        return 0;
    for (i = 0U; i < g->gl_pathc; i++) {
        if (strcmp(g->gl_pathv[g->gl_offs + i], path) == 0)
            return 1;
    }
    return 0;
}

int main(void)
{
    static const char out[] =
        "cmake-ok\n"
        "enlilos-define-ok\n"
        "cwd-ok\n"
        "pipe-dup2-ok\n"
        "glob-ok\n"
        "termios-header-ok\n";
    glob_t         g;
    struct termios term;
    char           cwd[64];
    char           ch = '\0';
    int            fds[2];
    int            saved_stdout = -1;
    int            fd = -1;

    (void)term;

    if (!getcwd(cwd, sizeof(cwd)))
        return 1;
    if (cwd[0] != '/')
        return 2;

    if (pipe(fds) < 0)
        return 3;
    saved_stdout = dup(STDOUT_FILENO);
    if (saved_stdout < 0)
        return 4;
    if (dup2(fds[1], STDOUT_FILENO) < 0)
        return 5;
    if (write(STDOUT_FILENO, "P", 1U) != 1) {
        (void)dup2(saved_stdout, STDOUT_FILENO);
        return 6;
    }
    if (dup2(saved_stdout, STDOUT_FILENO) < 0)
        return 7;
    if (close(saved_stdout) < 0)
        return 8;
    saved_stdout = -1;
    if (close(fds[1]) < 0)
        return 9;
    if (read(fds[0], &ch, 1U) != 1 || ch != 'P')
        return 10;
    if (close(fds[0]) < 0)
        return 11;

    (void)memset(&g, 0, sizeof(g));
    if (glob("/MUSL*.ELF", 0, NULL, &g) != 0)
        return 12;
    if (!glob_has(&g, "/MUSLHELLO.ELF") || !glob_has(&g, "/MUSLGLOB.ELF")) {
        globfree(&g);
        return 13;
    }
    globfree(&g);

    fd = open("/data/ARKSHSMK.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 14;
    if (write_full(fd, out, sizeof(out) - 1U) < 0) {
        (void)close(fd);
        return 15;
    }
    if (close(fd) < 0)
        return 16;

    return 0;
}
