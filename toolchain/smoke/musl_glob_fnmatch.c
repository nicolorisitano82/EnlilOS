#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <string.h>
#include <unistd.h>

static int has_path(const glob_t *g, const char *path)
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
    static const char out[] = "glob fnmatch ok\n";
    glob_t g;
    int    fd;

    if (fnmatch("*.ELF", "INIT.ELF", 0) != 0)
        return 1;
    if (fnmatch("*.ELF", "dir/INIT.ELF", FNM_PATHNAME) != FNM_NOMATCH)
        return 2;
    if (fnmatch("\\*.TXT", "*.TXT", 0) != 0)
        return 3;
    if (fnmatch("*", ".hidden", FNM_PERIOD) != FNM_NOMATCH)
        return 4;
    if (fnmatch("[A-Z]OOT.TXT", "BOOT.TXT", 0) != 0)
        return 5;

    (void)memset(&g, 0, sizeof(g));
    if (glob("/*.TXT", 0, NULL, &g) != 0)
        return 6;
    if (!has_path(&g, "/README.TXT") || !has_path(&g, "/BOOT.TXT")) {
        globfree(&g);
        return 7;
    }
    globfree(&g);

    (void)memset(&g, 0, sizeof(g));
    if (glob("/d*", GLOB_MARK, NULL, &g) != 0)
        return 8;
    if (!has_path(&g, "/data/") || !has_path(&g, "/dev/")) {
        globfree(&g);
        return 9;
    }
    globfree(&g);

    (void)memset(&g, 0, sizeof(g));
    if (glob("/MUSL*.ELF", 0, NULL, &g) != 0)
        return 10;
    if (!has_path(&g, "/MUSLHELLO.ELF") ||
        !has_path(&g, "/MUSLSTDIO.ELF") ||
        !has_path(&g, "/MUSLMALLOC.ELF") ||
        !has_path(&g, "/MUSLFORK.ELF") ||
        !has_path(&g, "/MUSLPIPE.ELF")) {
        globfree(&g);
        return 11;
    }
    if (glob("/MUSL*.ELF", GLOB_APPEND, NULL, &g) != 0) {
        globfree(&g);
        return 12;
    }
    if (g.gl_pathc < 10U) {
        globfree(&g);
        return 13;
    }
    globfree(&g);

    (void)memset(&g, 0, sizeof(g));
    fd = open("/data/MUSLGLOBCHK.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 14;
    if (write(fd, "chk\n", 4U) != 4) {
        close(fd);
        return 15;
    }
    if (close(fd) < 0)
        return 16;
    if (glob("/data/MUSLGLOB*.TXT", 0, NULL, &g) != 0)
        return 17;
    if (!has_path(&g, "/data/MUSLGLOBCHK.TXT")) {
        globfree(&g);
        return 18;
    }
    globfree(&g);

    fd = open("/data/MUSLGLOB.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return 19;
    if (write(fd, out, sizeof(out) - 1U) != (ssize_t)(sizeof(out) - 1U)) {
        close(fd);
        return 20;
    }
    if (close(fd) < 0)
        return 21;

    return 0;
}
