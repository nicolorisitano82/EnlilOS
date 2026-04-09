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
#include "vfs.h"
#include "sched.h"
#include "timer.h"
#include "types.h"

/* ── Tipi di nodo ─────────────────────────────────────────────────── */

#define PROC_NODE_ROOT          0x00U   /* /proc/ directory             */
#define PROC_NODE_SCHED         0x01U   /* /proc/sched                  */
#define PROC_NODE_PID_DIR       0x02U   /* /proc/<pid>/                 */
#define PROC_NODE_PID_STATUS    0x03U   /* /proc/<pid>/status           */

#define PROC_NODE_TYPE(id)      ((id) & 0xFFU)
#define PROC_NODE_PID(id)       ((id) >> 8)
#define PROC_NODE_MAKE(t, pid)  ((uint32_t)(t) | ((uint32_t)(pid) << 8))

/* Dimensione massima del buffer di testo generato per un singolo file.
 * /proc/sched elenca fino a SCHED_MAX_TASKS (64) task × ~80 byte/riga ≈ 5120.
 * Usiamo 4 KB per sicurezza. */
#define PROC_BUFSIZE    4096U

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
        p[4] == '/' && proc_streq(p + 5, "status")) {
        content_len = gen_pid_status(proc_buf, PROC_BUFSIZE, self_pid);
        out->mount     = mount;
        out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_STATUS, self_pid);
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
            out->mount     = mount;
            out->node_id   = PROC_NODE_MAKE(PROC_NODE_PID_STATUS, (uint32_t)pid);
            out->flags     = flags;
            out->pos       = 0;
            out->size_hint = (uint64_t)content_len;
            out->dir_index = 0;
            out->cookie    = (uintptr_t)proc_buf;
            return 0;
        }
    }

    return -ENOENT;
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

    if (type == PROC_NODE_PID_DIR) {
        /* Solo "status" */
        if (idx > 0U) return -ENOENT;
        file->dir_index++;
        {
            const char *n = "status";
            size_t      i;
            for (i = 0; n[i]; i++) out->name[i] = n[i];
            out->name[i] = '\0';
        }
        out->mode = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
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

    /* Indici 2+: PID task (salta quelli inesistenti/zombie) */
    {
        sched_task_info_t snap[SCHED_MAX_TASKS];
        uint32_t          n_tasks = sched_task_snapshot(snap, SCHED_MAX_TASKS);
        uint32_t          task_idx = idx - 2U;

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

    if (type == PROC_NODE_ROOT || type == PROC_NODE_PID_DIR) {
        out->st_mode    = S_IFDIR | S_IRUSR | S_IRGRP | S_IROTH;
        out->st_size    = 0;
        out->st_blksize = 512U;
        out->st_blocks  = 0;
        return 0;
    }

    out->st_mode    = S_IFREG | S_IRUSR | S_IRGRP | S_IROTH;
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

/* ── Operazioni VFS (tabella) ─────────────────────────────────────── */

static const vfs_ops_t procfs_ops_table = {
    .open    = procfs_open,
    .read    = procfs_read,
    .write   = procfs_write,
    .readdir = procfs_readdir,
    .stat    = procfs_stat,
    .close   = procfs_close,
};

const vfs_ops_t *procfs_vfs_ops(void)
{
    return &procfs_ops_table;
}
