/*
 * EnlilOS Microkernel — procfs (M14-01)
 *
 * Filesystem virtuale read-only montato su /proc.
 * Implementato come driver VFS kernel-side (stile devfs), senza server
 * user-space, per semplicità del v1.
 *
 * Gerarchia supportata:
 *   /proc/                    — directory root
 *   /proc/sched               — snapshot scheduler (jiffies, task list)
 *   /proc/<pid>/              — directory per PID esistente
 *   /proc/<pid>/status        — stato task: name, prio, state, runtime
 *   /proc/self                — alias PID task corrente (kernel = 0)
 *   /proc/self/status         — alias
 *
 * Codifica node_id:
 *   bits [7:0]  = tipo (PROC_NODE_*)
 *   bits [31:8] = pid  (0..SCHED_MAX_TASKS)
 */

#include "procfs.h"
#include "syscall.h"
#include "vfs.h"
#include "sched.h"
#include "timer.h"
#include "types.h"

/* ── Tipi di nodo ─────────────────────────────────────────────────── */

#define PROC_NODE_ROOT          0x00U   /* /proc/ directory             */
#define PROC_NODE_SCHED         0x01U   /* /proc/sched                  */
#define PROC_NODE_PID_DIR       0x02U   /* /proc/<pid>/                 */
#define PROC_NODE_PID_STATUS    0x03U   /* /proc/<pid>/status           */
#define PROC_NODE_VERSION       0x04U   /* /proc/version                */
#define PROC_NODE_MEMINFO       0x05U   /* /proc/meminfo                */
#define PROC_NODE_CPUINFO       0x06U   /* /proc/cpuinfo                */
#define PROC_NODE_PID_CMDLINE   0x07U   /* /proc/<pid>/cmdline          */
#define PROC_NODE_PID_ENVIRON   0x08U   /* /proc/<pid>/environ          */
#define PROC_NODE_PID_EXE       0x09U   /* /proc/<pid>/exe              */
#define PROC_NODE_PID_FD_DIR    0x0AU   /* /proc/<pid>/fd               */
#define PROC_NODE_PID_FD_LINK   0x0BU   /* /proc/<pid>/fd/<n>           */

#define PROC_NODE_TYPE(id)      ((id) & 0xFFU)
#define PROC_NODE_PID(id)       ((id) >> 8)
#define PROC_NODE_MAKE(t, pid)  ((uint32_t)(t) | ((uint32_t)(pid) << 8))

/* Dimensione massima del buffer di testo generato per un singolo file.
 * /proc/sched elenca fino a SCHED_MAX_TASKS (64) task × ~80 byte/riga ≈ 5120.
 * Usiamo 4 KB per sicurezza. */
#define PROC_BUFSIZE    4096U
#define PROC_FD_SCAN_MAX 64U

/* Buffer statico condiviso per la generazione del contenuto.
 * Sicuro: tutto il VFS è chiamato dal task kernel (prio=0, non preemptibile). */
static char proc_buf[PROC_BUFSIZE];

/* ── Utilità di formattazione senza libc ──────────────────────────── */

static char *proc_puts(char *p, char *end, const char *s)
{
    while (*s && p < end)
        *p++ = *s++;
    return p;
}

static char *proc_putu64(char *p, char *end, uint64_t v)
{
    char tmp[20];
    int  len = 0;

    if (v == 0U) {
        if (p < end) *p++ = '0';
        return p;
    }
    while (v > 0U) {
        tmp[len++] = (char)('0' + (int)(v % 10U));
        v /= 10U;
    }
    while (len-- > 0 && p < end)
        *p++ = tmp[len];
    return p;
}

static char *proc_putc(char *p, char *end, char c)
{
    if (p < end) *p++ = c;
    return p;
}

static char *proc_puts_pad(char *p, char *end, const char *s, int width)
{
    int len = 0;
    const char *t = s;
    while (*t++) len++;
    p = proc_puts(p, end, s);
    while (len < width && p < end) { *p++ = ' '; len++; }
    return p;
}

static char *proc_putu64_pad(char *p, char *end, uint64_t v, int width)
{
    char tmp[20];
    int  len = 0;
    uint64_t vv = v;

    if (vv == 0U) { tmp[len++] = '0'; }
    else { while (vv > 0U) { tmp[len++] = (char)('0' + (int)(vv % 10U)); vv /= 10U; } }

    /* Padding a sinistra */
    while (len < width && p < end) { *p++ = ' '; width--; }
    while (len-- > 0 && p < end) *p++ = tmp[len];
    return p;
}

/* ── Generatori di contenuto ──────────────────────────────────────── */

static const char *state_name(uint8_t state)
{
    switch (state) {
    case TCB_STATE_RUNNING: return "running";
    case TCB_STATE_READY:   return "ready  ";
    case TCB_STATE_BLOCKED: return "blocked";
    case TCB_STATE_ZOMBIE:  return "zombie ";
    default:                return "unknown";
    }
}

/*
 * Genera il contenuto di /proc/sched.
 * Formato:
 *   jiffies: <ms dal boot>
 *   tasks:   <N>
 *   <header>
 *   <riga per task>...
 */
static size_t gen_sched(char *buf, size_t bufsz)
{
    sched_task_info_t snap[SCHED_MAX_TASKS];
    uint32_t          n;
    char             *p   = buf;
    char             *end = buf + bufsz - 1U; /* lascia spazio per '\0' */

    n = sched_task_snapshot(snap, SCHED_MAX_TASKS);

    p = proc_puts(p, end, "jiffies: ");
    p = proc_putu64(p, end, timer_now_ms());
    p = proc_putc(p, end, '\n');

    p = proc_puts(p, end, "tasks:   ");
    p = proc_putu64(p, end, (uint64_t)n);
    p = proc_putc(p, end, '\n');
    p = proc_putc(p, end, '\n');

    p = proc_puts(p, end, "  PID  PRI  STATE    RUNTIME_MS  NAME\n");
    p = proc_puts(p, end, "  ---  ---  -------  ----------  ----\n");

    for (uint32_t i = 0U; i < n; i++) {
        sched_task_info_t *t = &snap[i];

        p = proc_putc(p, end, ' ');
        p = proc_putc(p, end, ' ');
        p = proc_putu64_pad(p, end, (uint64_t)t->pid,      3);
        p = proc_putc(p, end, ' ');
        p = proc_putc(p, end, ' ');
        p = proc_putu64_pad(p, end, (uint64_t)t->priority,  3);
        p = proc_putc(p, end, ' ');
        p = proc_putc(p, end, ' ');
        p = proc_puts_pad(p, end, state_name(t->state),    7);
        p = proc_putc(p, end, ' ');
        p = proc_putc(p, end, ' ');
        p = proc_putu64_pad(p, end, t->runtime_ns / 1000000ULL, 10);
        p = proc_putc(p, end, ' ');
        p = proc_putc(p, end, ' ');
        p = proc_puts(p, end, t->name);
        p = proc_putc(p, end, '\n');
    }

    *p = '\0';
    return (size_t)(p - buf);
}

/*
 * Genera il contenuto di /proc/<pid>/status.
 * Formato stile /proc/pid/status Linux (sottoinsieme minimo).
 */
static size_t gen_pid_status(char *buf, size_t bufsz, uint32_t pid)
{
    sched_tcb_t *t   = sched_task_find(pid);
    char        *p   = buf;
    char        *end = buf + bufsz - 1U;
    uint64_t     rt_ms;

    if (!t) {
        p = proc_puts(p, end, "Name:\t(not found)\n");
        *p = '\0';
        return (size_t)(p - buf);
    }

    rt_ms = t->runtime_ns / 1000000ULL;

    p = proc_puts(p, end, "Name:\t");
    p = proc_puts(p, end, t->name ? t->name : "(unnamed)");
    p = proc_putc(p, end, '\n');

    p = proc_puts(p, end, "Pid:\t");
    p = proc_putu64(p, end, (uint64_t)t->pid);
    p = proc_putc(p, end, '\n');

    p = proc_puts(p, end, "State:\t");
    p = proc_puts(p, end, state_name(t->state));
    p = proc_putc(p, end, '\n');

    p = proc_puts(p, end, "Priority:\t");
    p = proc_putu64(p, end, (uint64_t)t->priority);
    p = proc_putc(p, end, '\n');

    p = proc_puts(p, end, "Flags:\t");
    if (t->flags & TCB_FLAG_KERNEL) p = proc_puts(p, end, "kernel ");
    if (t->flags & TCB_FLAG_USER)   p = proc_puts(p, end, "user ");
    if (t->flags & TCB_FLAG_RT)     p = proc_puts(p, end, "rt ");
    if (t->flags & TCB_FLAG_IDLE)   p = proc_puts(p, end, "idle ");
    p = proc_putc(p, end, '\n');

    p = proc_puts(p, end, "VmRSS:\t");
    p = proc_putu64(p, end, rt_ms);
    p = proc_puts(p, end, " ms\n"); /* campo runtime_ms rietichettato */

    p = proc_puts(p, end, "RuntimeMs:\t");
    p = proc_putu64(p, end, rt_ms);
    p = proc_putc(p, end, '\n');

    if (t->period_ms > 0U) {
        p = proc_puts(p, end, "PeriodMs:\t");
        p = proc_putu64(p, end, t->period_ms);
        p = proc_putc(p, end, '\n');

        p = proc_puts(p, end, "DeadlineMs:\t");
        p = proc_putu64(p, end, t->deadline_ms);
        p = proc_putc(p, end, '\n');
    }

    *p = '\0';
    return (size_t)(p - buf);
}

static size_t gen_version(char *buf, size_t bufsz)
{
    char *p = buf;
    char *end = buf + bufsz - 1U;

    p = proc_puts(p, end, "Linux version 6.6.0-enlilos (codex@enlilos) #1 SMP PREEMPT\n");
    *p = '\0';
    return (size_t)(p - buf);
}

static size_t gen_meminfo(char *buf, size_t bufsz)
{
    char *p = buf;
    char *end = buf + bufsz - 1U;

    p = proc_puts(p, end, "MemTotal:       524288 kB\n");
    p = proc_puts(p, end, "MemFree:        507904 kB\n");
    p = proc_puts(p, end, "MemAvailable:   507904 kB\n");
    p = proc_puts(p, end, "Buffers:             0 kB\n");
    p = proc_puts(p, end, "Cached:              0 kB\n");
    p = proc_puts(p, end, "SwapTotal:           0 kB\n");
    p = proc_puts(p, end, "SwapFree:            0 kB\n");
    *p = '\0';
    return (size_t)(p - buf);
}

static size_t gen_cpuinfo(char *buf, size_t bufsz)
{
    char *p = buf;
    char *end = buf + bufsz - 1U;

    p = proc_puts(p, end, "processor\t: 0\n");
    p = proc_puts(p, end, "model name\t: ARMv8 AArch64 compatible (EnlilOS/QEMU virt)\n");
    p = proc_puts(p, end, "Features\t: fp asimd evtstrm crc32 atomics\n");
    p = proc_puts(p, end, "CPU architecture: 8\n");
    p = proc_puts(p, end, "Hardware\t: EnlilOS virt\n");
    *p = '\0';
    return (size_t)(p - buf);
}

static size_t gen_pid_cmdline(char *buf, size_t bufsz, uint32_t pid)
{
    sched_tcb_t  *t = sched_task_find(pid);
    const char   *exe;
    size_t        len = 0U;

    if (!t || bufsz == 0U)
        return 0U;

    exe = sched_task_exec_path(t);
    if (!exe || exe[0] == '\0')
        exe = "/INIT.ELF";

    while (exe[len] != '\0' && len + 1U < bufsz) {
        buf[len] = exe[len];
        len++;
    }
    if (len + 1U <= bufsz)
        buf[len++] = '\0';
    return len;
}

static size_t gen_pid_environ(char *buf, size_t bufsz)
{
    static const char env_blob[] =
        "HOME=/home/user\0"
        "PATH=/usr/bin:/bin\0"
        "TERM=vt100\0"
        "USER=user\0"
        "PWD=/\0";
    size_t len = sizeof(env_blob) - 1U;

    if (bufsz == 0U)
        return 0U;
    if (len > bufsz)
        len = bufsz;
    for (size_t i = 0U; i < len; i++)
        buf[i] = env_blob[i];
    return len;
}

static size_t gen_pid_exe(char *buf, size_t bufsz, uint32_t pid)
{
    sched_tcb_t *t = sched_task_find(pid);
    const char  *exe;
    size_t       len = 0U;

    if (!t || bufsz == 0U)
        return 0U;
    exe = sched_task_exec_path(t);
    if (!exe || exe[0] == '\0')
        exe = "/INIT.ELF";

    while (exe[len] != '\0' && len + 1U < bufsz) {
        buf[len] = exe[len];
        len++;
    }
    buf[len] = '\0';
    return len;
}

static size_t gen_pid_fd_path(char *buf, size_t bufsz, int fd)
{
    if (bufsz == 0U)
        return 0U;
    if (syscall_describe_fd_current(fd, buf, bufsz) < 0)
        return 0U;
    for (size_t i = 0U; i < bufsz; i++) {
        if (buf[i] == '\0')
            return i;
    }
    return bufsz;
}

/* ── Utilità path ─────────────────────────────────────────────────── */

static int proc_isdigit(char c)
{
    return (c >= '0' && c <= '9');
}

/* Parsa stringa decimale → pid. Ritorna -1 se non numerica. */
static int proc_parse_pid(const char *s)
{
    uint32_t v = 0U;
    if (!s || !proc_isdigit(*s)) return -1;
    while (proc_isdigit(*s)) {
        v = v * 10U + (uint32_t)(*s - '0');
        s++;
    }
    return (*s == '\0') ? (int)v : -1;
}

static int proc_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0');
}

/* ── Operazioni VFS ───────────────────────────────────────────────── */

static int procfs_open(const vfs_mount_t *mount, const char *relpath,
                       uint32_t flags, vfs_file_t *out)
{
    const char *p = relpath;
    uint32_t    self_pid;
    int         pid;
    size_t      content_len;
    uint8_t     type;

    if (!out) return -EFAULT;

    /* procfs è read-only */
    if (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))
        return -EROFS;

    /* Normalizza "/" iniziale */
    if (p[0] == '/') p++;

    self_pid = current_task ? current_task->pid : 0U;

    /* ── /proc/ ────────────────────────────────────────────────── */
    if (p[0] == '\0') {
        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(PROC_NODE_ROOT, 0U);
        out->flags     = flags;
        out->pos       = 0;
        out->size_hint = 0;
        out->dir_index = 0;
        out->cookie    = 0;
        return 0;
    }

    /* ── /proc/sched ───────────────────────────────────────────── */
    if (proc_streq(p, "sched")) {
        content_len = gen_sched(proc_buf, PROC_BUFSIZE);
        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(PROC_NODE_SCHED, 0U);
        out->flags     = flags;
        out->pos       = 0;
        out->size_hint = (uint64_t)content_len;
        out->dir_index = 0;
        out->cookie    = (uintptr_t)proc_buf;
        return 0;
    }
    if (proc_streq(p, "version")) {
        content_len = gen_version(proc_buf, PROC_BUFSIZE);
        type = PROC_NODE_VERSION;
        goto text_root_node;
    }
    if (proc_streq(p, "meminfo")) {
        content_len = gen_meminfo(proc_buf, PROC_BUFSIZE);
        type = PROC_NODE_MEMINFO;
        goto text_root_node;
    }
    if (proc_streq(p, "cpuinfo")) {
        content_len = gen_cpuinfo(proc_buf, PROC_BUFSIZE);
        type = PROC_NODE_CPUINFO;
        goto text_root_node;
    }

    /* ── /proc/self o /proc/self/status ────────────────────────── */
    if (proc_streq(p, "self")) {
        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_DIR, self_pid);
        out->flags     = flags;
        out->pos       = 0;
        out->size_hint = 0;
        out->dir_index = 0;
        out->cookie    = 0;
        return 0;
    }
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' &&
        p[4] == '/' && proc_streq(p + 5, "cmdline")) {
        content_len = gen_pid_cmdline(proc_buf, PROC_BUFSIZE, self_pid);
        type = PROC_NODE_PID_CMDLINE;
        goto text_self_node;
    }
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' &&
        p[4] == '/' && proc_streq(p + 5, "environ")) {
        content_len = gen_pid_environ(proc_buf, PROC_BUFSIZE);
        type = PROC_NODE_PID_ENVIRON;
        goto text_self_node;
    }
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' &&
        p[4] == '/' && proc_streq(p + 5, "exe")) {
        content_len = gen_pid_exe(proc_buf, PROC_BUFSIZE, self_pid);
        type = PROC_NODE_PID_EXE;
        goto text_self_node;
    }
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' &&
        p[4] == '/' && proc_streq(p + 5, "fd")) {
        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_FD_DIR, self_pid);
        out->flags     = flags;
        out->pos       = 0;
        out->size_hint = 0;
        out->dir_index = 0;
        out->cookie    = 0;
        return 0;
    }
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' &&
        p[4] == '/' && p[5] == 'f' && p[6] == 'd' && p[7] == '/') {
        pid = proc_parse_pid(p + 8);
        if (pid < 0)
            return -ENOENT;
        content_len = gen_pid_fd_path(proc_buf, PROC_BUFSIZE, pid);
        if (content_len == 0U)
            return -ENOENT;
        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_FD_LINK, self_pid);
        out->flags     = flags;
        out->pos       = 0;
        out->size_hint = (uint64_t)content_len;
        out->dir_index = 0;
        out->cookie    = (uintptr_t)proc_buf;
        return 0;
    }
    if (p[0] == 's' && p[1] == 'e' && p[2] == 'l' && p[3] == 'f' &&
        p[4] == '/' && proc_streq(p + 5, "status")) {
        content_len = gen_pid_status(proc_buf, PROC_BUFSIZE, self_pid);
        type = PROC_NODE_PID_STATUS;
text_self_node:
        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(type, self_pid);
        out->flags     = flags;
        out->pos       = 0;
        out->size_hint = (uint64_t)content_len;
        out->dir_index = 0;
        out->cookie    = (uintptr_t)proc_buf;
        return 0;
    }

    /* ── /proc/<pid> o /proc/<pid>/status ──────────────────────── */
    {
        /* Separa la prima componente del path */
        const char *slash = p;
        while (*slash && *slash != '/') slash++;

        /* Copia componente numerica */
        char pidbuf[16];
        size_t seg_len = (size_t)(slash - p);
        if (seg_len >= sizeof(pidbuf)) return -ENOENT;

        for (size_t i = 0U; i < seg_len; i++) pidbuf[i] = p[i];
        pidbuf[seg_len] = '\0';

        pid = proc_parse_pid(pidbuf);
        if (pid < 0) return -ENOENT;

        if (!sched_task_find((uint32_t)pid)) return -ENOENT;

        /* /proc/<pid>/ directory */
        if (*slash == '\0') {
            out->mount     = mount;
            out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_DIR, (uint32_t)pid);
            out->flags     = flags;
            out->pos       = 0;
            out->size_hint = 0;
            out->dir_index = 0;
            out->cookie    = 0;
            return 0;
        }

        /* /proc/<pid>/status */
        const char *after_slash = slash + 1;
        if (proc_streq(after_slash, "status")) {
            content_len = gen_pid_status(proc_buf, PROC_BUFSIZE, (uint32_t)pid);
            type = PROC_NODE_PID_STATUS;
        } else if (proc_streq(after_slash, "cmdline")) {
            content_len = gen_pid_cmdline(proc_buf, PROC_BUFSIZE, (uint32_t)pid);
            type = PROC_NODE_PID_CMDLINE;
        } else if (proc_streq(after_slash, "environ")) {
            content_len = gen_pid_environ(proc_buf, PROC_BUFSIZE);
            type = PROC_NODE_PID_ENVIRON;
        } else if (proc_streq(after_slash, "exe")) {
            content_len = gen_pid_exe(proc_buf, PROC_BUFSIZE, (uint32_t)pid);
            type = PROC_NODE_PID_EXE;
        } else if (proc_streq(after_slash, "fd")) {
            out->mount     = mount;
            out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_FD_DIR, (uint32_t)pid);
            out->flags     = flags;
            out->pos       = 0;
            out->size_hint = 0;
            out->dir_index = 0;
            out->cookie    = 0;
            return 0;
        } else if (after_slash[0] == 'f' && after_slash[1] == 'd' && after_slash[2] == '/') {
            if ((uint32_t)pid != self_pid)
                return -ENOENT;
            pid = proc_parse_pid(after_slash + 3);
            if (pid < 0)
                return -ENOENT;
            content_len = gen_pid_fd_path(proc_buf, PROC_BUFSIZE, pid);
            if (content_len == 0U)
                return -ENOENT;
            out->mount     = mount;
            out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_FD_LINK, self_pid);
            out->flags     = flags;
            out->pos       = 0;
            out->size_hint = (uint64_t)content_len;
            out->dir_index = 0;
            out->cookie    = (uintptr_t)proc_buf;
            return 0;
        }
        else {
            return -ENOENT;
        }

        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(type, (uint32_t)pid);
        out->flags     = flags;
        out->pos       = 0;
        out->size_hint = (uint64_t)content_len;
        out->dir_index = 0;
        out->cookie    = (uintptr_t)proc_buf;
        return 0;
    }

    return -ENOENT;

text_root_node:
    out->mount     = mount;
    out->node_id   = PROC_NODE_MAKE(type, 0U);
    out->flags     = flags;
    out->pos       = 0;
    out->size_hint = (uint64_t)content_len;
    out->dir_index = 0;
    out->cookie    = (uintptr_t)proc_buf;
    return 0;
}

static ssize_t procfs_read(vfs_file_t *file, void *buf, size_t count)
{
    const char *src;
    size_t      remain;
    uint8_t     type = (uint8_t)PROC_NODE_TYPE(file->node_id);

    if (!buf) return -EFAULT;

    if (type == PROC_NODE_ROOT || type == PROC_NODE_PID_DIR)
        return -EISDIR;

    /* I file di testo usano cookie come puntatore al buffer generato */
    if (file->pos >= file->size_hint) return 0;

    remain = (size_t)(file->size_hint - file->pos);
    if (count > remain) count = remain;

    src = (const char *)(uintptr_t)file->cookie + file->pos;
    {
        uint8_t       *dst = (uint8_t *)buf;
        const uint8_t *s   = (const uint8_t *)src;
        for (size_t i = 0; i < count; i++) dst[i] = s[i];
    }
    file->pos += count;
    return (ssize_t)count;
}

static ssize_t procfs_write(vfs_file_t *file, const void *buf, size_t count)
{
    (void)file; (void)buf; (void)count;
    return -EROFS;
}

/*
 * readdir per /proc/: emette "sched", "self", poi i PID esistenti.
 * readdir per /proc/<pid>/: emette "status".
 */
static int procfs_readdir(vfs_file_t *file, vfs_dirent_t *out)
{
    uint8_t  type = (uint8_t)PROC_NODE_TYPE(file->node_id);
    uint32_t idx  = file->dir_index;

    if (!out) return -EFAULT;

    if (type == PROC_NODE_PID_FD_DIR) {
        char target[128];

        while (idx < PROC_FD_SCAN_MAX) {
            if (PROC_NODE_PID(file->node_id) == (current_task ? current_task->pid : 0U) &&
                syscall_describe_fd_current((int)idx, target, sizeof(target)) == 0) {
                char numbuf[12];
                int  len = 0;
                uint32_t fd = idx;

                if (fd == 0U) numbuf[len++] = '0';
                else { while (fd) { numbuf[len++] = (char)('0' + (int)(fd % 10U)); fd /= 10U; } }
                for (int a = 0, b = len - 1; a < b; a++, b--) {
                    char tmp = numbuf[a]; numbuf[a] = numbuf[b]; numbuf[b] = tmp;
                }
                numbuf[len] = '\0';
                for (int i = 0; i <= len; i++) out->name[i] = numbuf[i];
                out->mode = S_IFLNK | S_IRUSR | S_IRGRP | S_IROTH;
                file->dir_index = idx + 1U;
                return 0;
            }
            idx++;
        }
        return -ENOENT;
    }

    if (type == PROC_NODE_PID_DIR) {
        static const struct {
            const char *name;
            uint32_t    mode;
        } entries[] = {
            { "status",  S_IFREG | S_IRUSR | S_IRGRP | S_IROTH },
            { "cmdline", S_IFREG | S_IRUSR | S_IRGRP | S_IROTH },
            { "environ", S_IFREG | S_IRUSR | S_IRGRP | S_IROTH },
            { "exe",     S_IFLNK | S_IRUSR | S_IRGRP | S_IROTH },
            { "fd",      S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH },
        };

        if (idx >= (uint32_t)(sizeof(entries) / sizeof(entries[0])))
            return -ENOENT;
        {
            const char *n = entries[idx].name;
            size_t      i;
            for (i = 0; n[i]; i++) out->name[i] = n[i];
            out->name[i] = '\0';
        }
        out->mode = entries[idx].mode;
        file->dir_index++;
        return 0;
    }

    if (type != PROC_NODE_ROOT) return -ENOTDIR;

    /* Indice 0: "sched" */
    if (idx == 0U) {
        const char *n = "sched";
        size_t i;
        for (i = 0; n[i]; i++) out->name[i] = n[i];
        out->name[i] = '\0';
        out->mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        file->dir_index++;
        return 0;
    }

    /* Indice 1: "self" */
    if (idx == 1U) {
        const char *n = "self";
        size_t i;
        for (i = 0; n[i]; i++) out->name[i] = n[i];
        out->name[i] = '\0';
        out->mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        file->dir_index++;
        return 0;
    }

    if (idx == 2U) {
        const char *n = "version";
        size_t i;
        for (i = 0; n[i]; i++) out->name[i] = n[i];
        out->name[i] = '\0';
        out->mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        file->dir_index++;
        return 0;
    }

    if (idx == 3U) {
        const char *n = "meminfo";
        size_t i;
        for (i = 0; n[i]; i++) out->name[i] = n[i];
        out->name[i] = '\0';
        out->mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        file->dir_index++;
        return 0;
    }

    if (idx == 4U) {
        const char *n = "cpuinfo";
        size_t i;
        for (i = 0; n[i]; i++) out->name[i] = n[i];
        out->name[i] = '\0';
        out->mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
        file->dir_index++;
        return 0;
    }

    /* Indici 5+: PID task (salta quelli inesistenti/zombie) */
    {
        sched_task_info_t snap[SCHED_MAX_TASKS];
        uint32_t          n_tasks = sched_task_snapshot(snap, SCHED_MAX_TASKS);
        uint32_t          task_idx = idx - 5U;

        /* Salta i task zombie */
        uint32_t live = 0U;
        for (uint32_t i = 0U; i < n_tasks; i++) {
            if (snap[i].state == TCB_STATE_ZOMBIE) continue;
            if (live == task_idx) {
                /* Emette il PID come stringa */
                char     pidbuf[12];
                uint32_t pid = snap[i].pid;
                int      len = 0;
                if (pid == 0U) { pidbuf[len++] = '0'; }
                else { while (pid) { pidbuf[len++] = (char)('0' + (int)(pid % 10U)); pid /= 10U; } }
                /* inverti */
                for (int a = 0, b = len - 1; a < b; a++, b--) {
                    char tmp = pidbuf[a]; pidbuf[a] = pidbuf[b]; pidbuf[b] = tmp;
                }
                pidbuf[len] = '\0';
                for (int j = 0; j <= len; j++) out->name[j] = pidbuf[j];
                out->mode = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
                file->dir_index++;
                return 0;
            }
            live++;
        }
    }

    return -ENOENT;
}

static int procfs_stat(vfs_file_t *file, stat_t *out)
{
    uint8_t type = (uint8_t)PROC_NODE_TYPE(file->node_id);

    if (!out) return -EFAULT;

    if (type == PROC_NODE_ROOT || type == PROC_NODE_PID_DIR ||
        type == PROC_NODE_PID_FD_DIR) {
        out->st_mode    = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        out->st_size    = 0;
        out->st_blksize = 512U;
        out->st_blocks  = 0;
        return 0;
    }

    if (type == PROC_NODE_PID_EXE || type == PROC_NODE_PID_FD_LINK)
        out->st_mode = S_IFLNK | S_IRUSR | S_IRGRP | S_IROTH;
    else
        out->st_mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
    out->st_size    = file->size_hint;
    out->st_blksize = 512U;
    out->st_blocks  = (file->size_hint + 511ULL) / 512ULL;
    return 0;
}

static int procfs_close(vfs_file_t *file)
{
    (void)file;
    return 0;
}

static int procfs_lstat(const vfs_mount_t *mount, const char *relpath, stat_t *out)
{
    vfs_file_t file;
    int        rc;

    rc = procfs_open(mount, relpath, O_RDONLY, &file);
    if (rc < 0)
        return rc;
    rc = procfs_stat(&file, out);
    (void)procfs_close(&file);
    return rc;
}

static int procfs_readlink(const vfs_mount_t *mount, const char *relpath,
                           char *out, size_t cap)
{
    vfs_file_t file;
    uint8_t    type;
    int        rc;

    if (!out || cap < 2U)
        return -EFAULT;

    rc = procfs_open(mount, relpath, O_RDONLY, &file);
    if (rc < 0)
        return rc;

    type = (uint8_t)PROC_NODE_TYPE(file.node_id);
    if (!(type == PROC_NODE_PID_EXE || type == PROC_NODE_PID_FD_LINK))
        return -EINVAL;

    if (file.size_hint == 0U || file.size_hint + 1U > cap)
        return -ENOENT;
    {
        const char *src = (const char *)(uintptr_t)file.cookie;
        for (size_t i = 0U; i < (size_t)file.size_hint; i++)
            out[i] = src[i];
        out[file.size_hint] = '\0';
    }
    return 0;
}

/* ── Operazioni VFS (tabella) ─────────────────────────────────────── */

static const vfs_ops_t procfs_ops_table = {
    .open    = procfs_open,
    .read    = procfs_read,
    .write   = procfs_write,
    .readdir = procfs_readdir,
    .stat    = procfs_stat,
    .lstat   = procfs_lstat,
    .close   = procfs_close,
    .readlink = procfs_readlink,
};

const vfs_ops_t *procfs_vfs_ops(void)
{
    return &procfs_ops_table;
}
