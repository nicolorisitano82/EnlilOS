#include <fcntl.h>
#include <unistd.h>

#include "../../include/user_svc.h"

#define SYS_MOUNT 30
#define MS_BIND   4096U

#define ARKSHBOOT_OUT        "/data/ARKSHBOOT.TXT"
#define ARKSH_USER_RC        "/home/user/.config/arksh/arkshrc"
#define ARKSH_ETC_RC         "/etc/arkshrc"

static unsigned long ab_strlen(const char *s)
{
    unsigned long n = 0UL;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static int ab_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static int ab_contains(const char *haystack, const char *needle)
{
    unsigned long i;
    unsigned long j;

    if (!haystack || !needle || needle[0] == '\0')
        return 0;

    for (i = 0UL; haystack[i] != '\0'; i++) {
        for (j = 0UL; needle[j] != '\0' && haystack[i + j] == needle[j]; j++)
            ;
        if (needle[j] == '\0')
            return 1;
    }
    return 0;
}

static long ab_mount(const char *src, const char *dst, const char *fstype,
                     unsigned int flags)
{
    return user_svc4(SYS_MOUNT, (long)src, (long)dst, (long)fstype, (long)flags);
}

static int ab_write_all_fd(int fd, const char *text)
{
    unsigned long off = 0UL;
    unsigned long len = ab_strlen(text);

    while (off < len) {
        ssize_t rc = write(fd, text + off, len - off);

        if (rc <= 0)
            return -1;
        off += (unsigned long)rc;
    }
    return 0;
}

static int ab_write_file(const char *path, const char *text, int trunc)
{
    int fd = open(path, O_WRONLY | O_CREAT | (trunc ? O_TRUNC : O_APPEND), 0644);

    if (fd < 0)
        return -1;
    if (ab_write_all_fd(fd, text) < 0) {
        close(fd);
        return -1;
    }
    if (close(fd) < 0)
        return -1;
    return 0;
}

static int ab_file_exists(const char *path)
{
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static int ab_read_contains(const char *path, const char *needle)
{
    char buf[256];
    int  fd;
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1U);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return ab_contains(buf, needle);
}

static int ab_env_has(char **envp, const char *entry)
{
    if (!envp)
        return 0;
    while (*envp) {
        if (ab_streq(*envp, entry))
            return 1;
        envp++;
    }
    return 0;
}

static void ab_prepare_login_home(void)
{
    if (ab_file_exists("/data/home"))
        (void)ab_mount("/data/home", "/home", "bind", MS_BIND);

    if (chdir("/home/user") < 0)
        (void)chdir("/");
}

static int ab_selftest(char **envp)
{
    char cwd[128];
    int  out_fd;

    ab_prepare_login_home();

    out_fd = open(ARKSHBOOT_OUT, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0)
        return 10;

    if (!ab_env_has(envp, "PATH=/bin:/usr/bin") ||
        !ab_env_has(envp, "HOME=/home/user") ||
        !ab_env_has(envp, "SHELL=/bin/arksh") ||
        !ab_env_has(envp, "TERM=vt100")) {
        close(out_fd);
        return 11;
    }
    if (ab_write_all_fd(out_fd, "env-ok\n") < 0) {
        close(out_fd);
        return 12;
    }

    if (!getcwd(cwd, sizeof(cwd)) || !ab_streq(cwd, "/home/user")) {
        close(out_fd);
        return 13;
    }
    if (ab_write_all_fd(out_fd, "cwd-ok\n") < 0) {
        close(out_fd);
        return 14;
    }

    if (!ab_read_contains(ARKSH_ETC_RC, "PATH=/bin:/usr/bin")) {
        close(out_fd);
        return 15;
    }
    if (ab_write_all_fd(out_fd, "etc-rc-ok\n") < 0) {
        close(out_fd);
        return 16;
    }

    if (!ab_read_contains(ARKSH_USER_RC, "arkshrc")) {
        close(out_fd);
        return 17;
    }
    if (ab_write_all_fd(out_fd, "user-rc-ok\n") < 0) {
        close(out_fd);
        return 18;
    }

    if (!ab_file_exists("/bin/arksh") || !ab_file_exists("/bin/nsh")) {
        close(out_fd);
        return 21;
    }
    if (ab_write_all_fd(out_fd, "bin-layout-ok\n") < 0) {
        close(out_fd);
        return 22;
    }

    close(out_fd);
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    static const char fallback_msg[] =
        "[arksh] shell reale non presente, fallback su /bin/nsh\n";
    static char *const real_argv[] = { (char *)"/bin/arksh", NULL };
    static char *const nsh_argv[] = { (char *)"/bin/nsh", NULL };

    (void)argc;
#ifdef ARKSH_BOOT_SELFTEST
    (void)argv;
        return ab_selftest(envp);
#endif

    ab_prepare_login_home();

    if (ab_file_exists("/bin/arksh.real")) {
        execve("/bin/arksh.real", real_argv, envp);
    }

    (void)ab_write_all_fd(STDOUT_FILENO, fallback_msg);
    execve("/bin/nsh", nsh_argv, envp);
    (void)ab_write_all_fd(STDERR_FILENO, "[arksh] exec fallita\n");
    return 127;
}
