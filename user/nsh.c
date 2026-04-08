/*
 * EnlilOS user-space shell (M7-02)
 *
 * Shell minimale freestanding per EL0:
 *   - ls, cat, echo, exec, clear, top
 *   - extra utili: cd, pwd, help, exit
 */

#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef signed long    s64;
typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define NSH_LINE_MAX   160U
#define NSH_PATH_MAX   128U
#define NSH_READ_CHUNK 256U
#define NSH_TASK_MAX   32U
#define NSH_DIRENT_MAX 8U

static char nsh_cwd[NSH_PATH_MAX];
static volatile u32 nsh_sigint_seen;

static long sys_call3(long nr, long a0, long a1, long a2)
{
    return user_svc3(nr, a0, a1, a2);
}

static long sys_call4(long nr, long a0, long a1, long a2, long a3)
{
    return user_svc4(nr, a0, a1, a2, a3);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

static long sys_write_fd(long fd, const void *buf, u64 len)
{
    return sys_call3(SYS_WRITE, fd, (long)buf, (long)len);
}

static long sys_read_fd(long fd, void *buf, u64 len)
{
    return sys_call3(SYS_READ, fd, (long)buf, (long)len);
}

static long sys_open_path(const char *path, u32 flags)
{
    return sys_call3(SYS_OPEN, (long)path, (long)flags, 0);
}

static long sys_close_fd(long fd)
{
    return sys_call3(SYS_CLOSE, fd, 0, 0);
}

static long sys_fstat_fd(long fd, stat_t *st)
{
    return sys_call3(SYS_FSTAT, fd, (long)st, 0);
}

static long sys_wait_pid(long pid)
{
    return sys_call4(SYS_WAITPID, pid, 0, 0, 0);
}

static long sys_getdents_fd(long fd, sys_dirent_t *out, u32 max_entries)
{
    return sys_call3(SYS_GETDENTS, fd, (long)out, max_entries);
}

static long sys_task_snapshot(task_snapshot_t *out, u32 max_entries)
{
    return sys_call3(SYS_TASK_SNAPSHOT, (long)out, max_entries, 0);
}

static long sys_spawn_path(const char *path, u32 *pid_out, u32 priority)
{
    return sys_call3(SYS_SPAWN, (long)path, (long)pid_out, priority);
}

static long sys_sigaction_now(int sig, const sigaction_t *act, sigaction_t *old)
{
    return sys_call3(SYS_SIGACTION, sig, (long)act, (long)old);
}

static long sys_chdir_path(const char *path)
{
    return user_svc1(SYS_CHDIR, (long)path);
}

static long sys_getcwd_now(char *buf, u32 len)
{
    return user_svc2(SYS_GETCWD, (long)buf, (long)len);
}

static u32 nsh_strlen(const char *s)
{
    u32 n = 0U;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static int nsh_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void nsh_on_sigint(int sig)
{
    (void)sig;
    nsh_sigint_seen = 1U;
}

static void nsh_strlcpy(char *dst, const char *src, u32 cap)
{
    u32 i = 0U;

    if (cap == 0U)
        return;
    while (src && src[i] != '\0' && i + 1U < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void nsh_bzero(void *ptr, u64 len)
{
    u8 *p = (u8 *)ptr;
    while (len-- > 0U)
        *p++ = 0U;
}

static void nsh_puts(const char *s)
{
    (void)sys_write_fd(1, s, nsh_strlen(s));
}

static void nsh_putln(const char *s)
{
    nsh_puts(s);
    nsh_puts("\n");
}

static void nsh_append(char *dst, u32 cap, const char *src)
{
    u32 len = nsh_strlen(dst);
    u32 i = 0U;

    if (len >= cap - 1U)
        return;

    while (src[i] != '\0' && len + 1U < cap)
        dst[len++] = src[i++];
    dst[len] = '\0';
}

static void nsh_append_u64(char *dst, u32 cap, u64 value)
{
    char tmp[32];
    u32  len = 0U;

    if (value == 0ULL) {
        nsh_append(dst, cap, "0");
        return;
    }

    while (value != 0ULL && len < (u32)sizeof(tmp)) {
        tmp[len++] = (char)('0' + (value % 10ULL));
        value /= 10ULL;
    }
    while (len > 0U) {
        char one[2];
        one[0] = tmp[--len];
        one[1] = '\0';
        nsh_append(dst, cap, one);
    }
}

static void nsh_append_pad(char *dst, u32 cap, const char *src, u32 width)
{
    u32 len = nsh_strlen(src);
    nsh_append(dst, cap, src);
    while (len++ < width)
        nsh_append(dst, cap, " ");
}

static const char *nsh_errno_name(long rc)
{
    switch ((int)(-rc)) {
    case EPERM: return "EPERM";
    case ENOENT: return "ENOENT";
    case ESRCH: return "ESRCH";
    case ECHILD: return "ECHILD";
    case EAGAIN: return "EAGAIN";
    case EINTR: return "EINTR";
    case ENOMEM: return "ENOMEM";
    case EFAULT: return "EFAULT";
    case EBADF: return "EBADF";
    case EBUSY: return "EBUSY";
    case EEXIST: return "EEXIST";
    case EXDEV: return "EXDEV";
    case ENOTDIR: return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case EINVAL: return "EINVAL";
    case ENFILE: return "ENFILE";
    case ENOSPC: return "ENOSPC";
    case EROFS: return "EROFS";
    case ERANGE: return "ERANGE";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    case ENOTEMPTY: return "ENOTEMPTY";
    case EIO: return "EIO";
    case ENOSYS: return "ENOSYS";
    default: return "ERR";
    }
}

static void nsh_print_error(const char *prefix, long rc)
{
    char line[96];
    nsh_bzero(line, sizeof(line));
    nsh_append(line, sizeof(line), prefix);
    nsh_append(line, sizeof(line), ": ");
    nsh_append(line, sizeof(line), nsh_errno_name(rc));
    nsh_putln(line);
}

static void nsh_path_pop(char *path)
{
    u32 len = nsh_strlen(path);

    if (len <= 1U) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    while (len > 1U && path[len - 1U] == '/') {
        path[--len] = '\0';
    }
    while (len > 1U && path[len - 1U] != '/') {
        path[--len] = '\0';
    }
    if (len > 1U)
        path[len - 1U] = '\0';
    if (path[0] == '\0') {
        path[0] = '/';
        path[1] = '\0';
    }
}

static int nsh_resolve_path(const char *input, char *out, u32 cap)
{
    char tmp[NSH_PATH_MAX];
    u32  tmp_len = 0U;
    u32  out_len = 0U;
    u32  i = 0U;

    if (!input || !out || cap < 2U)
        return 0;

    if (input[0] == '/') {
        while (input[tmp_len] != '\0' && tmp_len + 1U < (u32)sizeof(tmp))
            tmp[tmp_len] = input[tmp_len], tmp_len++;
        tmp[tmp_len] = '\0';
    } else {
        const char *base = nsh_cwd[0] ? nsh_cwd : "/";
        while (base[tmp_len] != '\0' && tmp_len + 1U < (u32)sizeof(tmp))
            tmp[tmp_len] = base[tmp_len], tmp_len++;
        if (tmp_len == 0U) tmp[tmp_len++] = '/';
        if (tmp[tmp_len - 1U] != '/' && tmp_len + 1U < (u32)sizeof(tmp))
            tmp[tmp_len++] = '/';
        i = 0U;
        while (input[i] != '\0' && tmp_len + 1U < (u32)sizeof(tmp))
            tmp[tmp_len++] = input[i++];
        tmp[tmp_len] = '\0';
    }

    out[out_len++] = '/';
    out[1] = '\0';
    i = 0U;

    while (tmp[i] != '\0') {
        char component[NSH_PATH_MAX];
        u32  comp_len = 0U;

        while (tmp[i] == '/')
            i++;
        if (tmp[i] == '\0')
            break;

        while (tmp[i] != '\0' && tmp[i] != '/' &&
               comp_len + 1U < (u32)sizeof(component)) {
            component[comp_len++] = tmp[i++];
        }
        component[comp_len] = '\0';

        if (nsh_streq(component, "."))
            continue;
        if (nsh_streq(component, "..")) {
            nsh_path_pop(out);
            out_len = nsh_strlen(out);
            continue;
        }

        if (out_len > 1U) {
            if (out_len + 1U >= cap)
                return 0;
            out[out_len++] = '/';
            out[out_len] = '\0';
        }

        if (out_len + comp_len >= cap)
            return 0;
        for (u32 j = 0U; j < comp_len; j++)
            out[out_len++] = component[j];
        out[out_len] = '\0';
    }

    if (out_len == 0U) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 1;
}

static int nsh_readline(char *buf, u32 cap)
{
    for (;;) {
        long rc;

        rc = sys_read_fd(0, buf, cap - 1U);
        if (rc > 0) {
            u32 len = (u32)rc;
            while (len > 0U && (buf[len - 1U] == '\n' || buf[len - 1U] == '\r'))
                len--;
            buf[len] = '\0';
            return (int)len;
        }
        if (rc == -(long)EAGAIN)
            continue;
        if (rc == -(long)EINTR) {
            buf[0] = '\0';
            return -(int)EINTR;
        }
        return (int)rc;
    }
}

static char *nsh_skip_spaces(char *s)
{
    while (*s == ' ')
        s++;
    return s;
}

static char *nsh_next_token(char *s)
{
    while (*s != '\0' && *s != ' ')
        s++;
    return s;
}

static const char *nsh_state_name(u8 state)
{
    switch (state) {
    case TASK_SNAPSHOT_RUNNING: return "RUN";
    case TASK_SNAPSHOT_READY:   return "RDY";
    case TASK_SNAPSHOT_BLOCKED: return "BLK";
    case TASK_SNAPSHOT_ZOMBIE:  return "ZMB";
    default:                    return "???";
    }
}

static void nsh_cmd_help(void)
{
    nsh_putln("Comandi: ls cat echo exec clear top cd pwd help exit");
    nsh_putln("Esempi : ls /data | cat /BOOT.TXT | exec /DEMO.ELF | top");
}

static void nsh_cmd_pwd(void)
{
    char cwd[NSH_PATH_MAX];

    if (sys_getcwd_now(cwd, sizeof(cwd)) >= 0) {
        nsh_strlcpy(nsh_cwd, cwd, sizeof(nsh_cwd));
        nsh_putln(cwd);
        return;
    }
    nsh_putln(nsh_cwd);
}

static void nsh_cmd_cd(const char *arg)
{
    char   resolved[NSH_PATH_MAX];
    long   rc;
    if (!arg || !*arg) {
        arg = "/";
    }
    if (!nsh_resolve_path(arg, resolved, sizeof(resolved))) {
        nsh_putln("cd: path non valido");
        return;
    }

    rc = sys_chdir_path(resolved);
    if (rc < 0) {
        nsh_print_error("cd", rc);
        return;
    }
    if (sys_getcwd_now(nsh_cwd, sizeof(nsh_cwd)) < 0)
        nsh_strlcpy(nsh_cwd, resolved, sizeof(nsh_cwd));
}

static void nsh_cmd_ls(const char *arg)
{
    char         path[NSH_PATH_MAX];
    stat_t       st;
    sys_dirent_t ents[NSH_DIRENT_MAX];
    long         fd;
    long         rc;

    if (!arg || !*arg)
        arg = nsh_cwd;
    if (!nsh_resolve_path(arg, path, sizeof(path))) {
        nsh_putln("ls: path non valido");
        return;
    }

    fd = sys_open_path(path, O_RDONLY);
    if (fd < 0) {
        nsh_print_error("ls open", fd);
        return;
    }
    rc = sys_fstat_fd(fd, &st);
    if (rc < 0) {
        nsh_print_error("ls stat", rc);
        (void)sys_close_fd(fd);
        return;
    }
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        nsh_putln("ls: non e' una directory");
        (void)sys_close_fd(fd);
        return;
    }

    for (;;) {
        rc = sys_getdents_fd(fd, ents, NSH_DIRENT_MAX);
        if (rc < 0) {
            nsh_print_error("ls getdents", rc);
            break;
        }
        if (rc == 0)
            break;

        for (u32 i = 0U; i < (u32)rc; i++) {
            nsh_puts(ents[i].name);
            if ((ents[i].mode & S_IFMT) == S_IFDIR)
                nsh_puts("/");
            nsh_puts("\n");
        }
    }

    (void)sys_close_fd(fd);
}

static void nsh_cmd_cat(const char *arg)
{
    char   path[NSH_PATH_MAX];
    char   buf[NSH_READ_CHUNK];
    stat_t st;
    long   fd;
    long   rc;

    if (!arg || !*arg) {
        nsh_putln("cat: path mancante");
        return;
    }
    if (!nsh_resolve_path(arg, path, sizeof(path))) {
        nsh_putln("cat: path non valido");
        return;
    }

    fd = sys_open_path(path, O_RDONLY);
    if (fd < 0) {
        nsh_print_error("cat open", fd);
        return;
    }
    rc = sys_fstat_fd(fd, &st);
    if (rc < 0) {
        nsh_print_error("cat stat", rc);
        (void)sys_close_fd(fd);
        return;
    }
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        nsh_putln("cat: il path e' una directory");
        (void)sys_close_fd(fd);
        return;
    }

    for (;;) {
        rc = sys_read_fd(fd, buf, sizeof(buf));
        if (rc < 0) {
            nsh_print_error("cat read", rc);
            break;
        }
        if (rc == 0)
            break;
        (void)sys_write_fd(1, buf, (u64)rc);
    }
    if (st.st_size == 0ULL)
        nsh_putln("<file vuoto>");
    else if (buf[0] != '\n')
        nsh_puts("");
    if (st.st_size != 0ULL)
        nsh_puts("\n");
    (void)sys_close_fd(fd);
}

static void nsh_cmd_echo(const char *arg)
{
    if (!arg) arg = "";
    nsh_putln(arg);
}

static void nsh_cmd_clear(void)
{
    static const char seq[] = "\x1b[2J\x1b[H\f";
    (void)sys_write_fd(1, seq, sizeof(seq) - 1U);
}

static void nsh_cmd_top(void)
{
    task_snapshot_t tasks[NSH_TASK_MAX];
    long            rc = sys_task_snapshot(tasks, NSH_TASK_MAX);

    if (rc < 0) {
        nsh_print_error("top", rc);
        return;
    }

    nsh_putln("PID PRI ST  CPU(ms) DEADLINE NAME");
    for (u32 i = 0U; i < (u32)rc; i++) {
        char line[128];

        nsh_bzero(line, sizeof(line));
        nsh_append_u64(line, sizeof(line), tasks[i].pid);
        nsh_append(line, sizeof(line), " ");
        if (tasks[i].priority < 10U) nsh_append(line, sizeof(line), "  ");
        else if (tasks[i].priority < 100U) nsh_append(line, sizeof(line), " ");
        nsh_append_u64(line, sizeof(line), tasks[i].priority);
        nsh_append(line, sizeof(line), " ");
        nsh_append_pad(line, sizeof(line), nsh_state_name(tasks[i].state), 3U);
        nsh_append(line, sizeof(line), " ");
        nsh_append_u64(line, sizeof(line), tasks[i].runtime_ns / 1000000ULL);
        nsh_append(line, sizeof(line), "      ");
        if (tasks[i].deadline_ms == 0ULL)
            nsh_append(line, sizeof(line), "-");
        else
            nsh_append_u64(line, sizeof(line), tasks[i].deadline_ms);
        nsh_append(line, sizeof(line), "      ");
        nsh_append(line, sizeof(line), tasks[i].name);
        nsh_putln(line);
    }
}

static void nsh_cmd_exec(const char *arg)
{
    char path[NSH_PATH_MAX];
    u32  pid = 0U;
    long rc;

    if (!arg || !*arg) {
        nsh_putln("exec: path mancante");
        return;
    }
    if (!nsh_resolve_path(arg, path, sizeof(path))) {
        nsh_putln("exec: path non valido");
        return;
    }

    /*
     * exec e' sincrono: la shell attende il figlio con waitpid().
     * Se il figlio parte a priorita' troppo bassa puo' restare affamato
     * dal parent in polling. Lo lanciamo almeno alla stessa classe della nsh.
     */
    rc = sys_spawn_path(path, &pid, 32U);
    if (rc < 0) {
        nsh_print_error("exec spawn", rc);
        return;
    }

    nsh_puts("[nsh] spawned pid=");
    {
        char tmp[24];
        nsh_bzero(tmp, sizeof(tmp));
        nsh_append_u64(tmp, sizeof(tmp), pid);
        nsh_putln(tmp);
    }

    rc = sys_wait_pid((long)pid);
    if (rc < 0)
        nsh_print_error("exec waitpid", rc);
}

static void nsh_dispatch(char *line)
{
    char *cmd;
    char *arg;

    line = nsh_skip_spaces(line);
    if (*line == '\0')
        return;

    cmd = line;
    arg = nsh_next_token(line);
    if (*arg != '\0') {
        *arg++ = '\0';
        arg = nsh_skip_spaces(arg);
    }

    if (nsh_streq(cmd, "help"))
        nsh_cmd_help();
    else if (nsh_streq(cmd, "pwd"))
        nsh_cmd_pwd();
    else if (nsh_streq(cmd, "cd"))
        nsh_cmd_cd(arg);
    else if (nsh_streq(cmd, "ls"))
        nsh_cmd_ls(arg);
    else if (nsh_streq(cmd, "cat"))
        nsh_cmd_cat(arg);
    else if (nsh_streq(cmd, "echo"))
        nsh_cmd_echo(arg);
    else if (nsh_streq(cmd, "clear"))
        nsh_cmd_clear();
    else if (nsh_streq(cmd, "top"))
        nsh_cmd_top();
    else if (nsh_streq(cmd, "exec"))
        nsh_cmd_exec(arg);
    else if (nsh_streq(cmd, "exit"))
        sys_exit_now(0);
    else
        nsh_putln("Comando sconosciuto. Usa 'help'.");
}

void _start(void)
{
    sigaction_t sa;
    char line[NSH_LINE_MAX];

    if (sys_getcwd_now(nsh_cwd, sizeof(nsh_cwd)) < 0)
        nsh_strlcpy(nsh_cwd, "/", sizeof(nsh_cwd));
    nsh_sigint_seen = 0U;
    sa.sa_handler = nsh_on_sigint;
    sa.sa_mask = 0ULL;
    sa.sa_flags = 0U;
    sa._pad = 0U;
    (void)sys_sigaction_now(SIGINT, &sa, (sigaction_t *)0);
    nsh_cmd_clear();
    nsh_putln("EnlilOS nsh - shell ELF statico");
    nsh_putln("Digita 'help' per i comandi.");

    for (;;) {
        int rc;

        nsh_puts("nsh:");
        nsh_puts(nsh_cwd);
        nsh_puts("$ ");

        rc = nsh_readline(line, sizeof(line));
        if (rc == -(int)EINTR) {
            nsh_sigint_seen = 0U;
            nsh_putln("^C");
            continue;
        }
        if (rc < 0) {
            nsh_print_error("read", rc);
            continue;
        }

        nsh_dispatch(line);
    }
}
