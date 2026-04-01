/*
 * EnlilOS Microkernel - Syscall Base (M3-01 / M3-02 / M5-02)
 *
 * Implementa:
 *   - syscall_table[256]: dispatch O(1)
 *   - fd_table[SCHED_MAX_TASKS][MAX_FD]: file descriptor per task
 *   - task_brk[SCHED_MAX_TASKS]: program break per task
 *   - syscall base + estensioni user-space per M7-02
 *
 * RT: nessuna allocazione nel dispatch path. WCET = O(1).
 */

#include "syscall.h"
#include "keyboard.h"
#include "elf_loader.h"
#include "kheap.h"
#include "mmu.h"
#include "sched.h"
#include "timer.h"
#include "tty.h"
#include "pmm.h"
#include "uart.h"
#include "types.h"
#include "vfs.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

/* ════════════════════════════════════════════════════════════════════
 * File descriptor table
 * ════════════════════════════════════════════════════════════════════ */

#define MAX_FD          64

#define FD_TYPE_FREE    0
#define FD_TYPE_VFS     1

typedef struct {
    uint8_t  type;
    uint8_t  _pad0;
    uint16_t flags;
    uint32_t _pad1;
    vfs_file_t file;
} fd_entry_t;

/*
 * Indicizzata per [pid % SCHED_MAX_TASKS][fd].
 * Tutti i task partono con 0/1/2 preimpostati come CONSOLE.
 */
static fd_entry_t fd_tables[SCHED_MAX_TASKS][MAX_FD];

/*
 * Program break per task (indirizzato come sopra).
 * brk=0 → non ancora inizializzato.
 */
static uint64_t task_brk[SCHED_MAX_TASKS];
static exception_frame_t *active_syscall_frame;
static uint8_t            active_syscall_replaced;

#define EXEC_MAX_PATH       256U
#define EXEC_MAX_ARGS       ELF_LOADER_MAX_ARGS
#define EXEC_MAX_ENVP       ELF_LOADER_MAX_ENVP
#define EXEC_STRPOOL_SIZE   2048U

typedef struct {
    char        path[EXEC_MAX_PATH];
    char        strpool[EXEC_STRPOOL_SIZE];
    const char *argv[EXEC_MAX_ARGS];
    const char *envp[EXEC_MAX_ENVP];
    uint64_t    argc;
    uint64_t    envc;
    size_t      used;
} exec_copy_t;

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Indice nel fd_table per il task corrente */
static inline int task_idx(void)
{
    if (!current_task) return 0;
    return (int)(current_task->pid % SCHED_MAX_TASKS);
}

/* Restituisce l'entry fd, NULL se fuori range o free */
static fd_entry_t *fd_get(int fd)
{
    if (fd < 0 || fd >= MAX_FD) return NULL;
    fd_entry_t *e = &fd_tables[task_idx()][fd];
    return (e->type != FD_TYPE_FREE) ? e : NULL;
}

/* Alloca il primo fd libero >= 3 (non tocca stdin/stdout/stderr) */
static int fd_alloc(void)
{
    int idx = task_idx();
    for (int i = 3; i < MAX_FD; i++) {
        if (fd_tables[idx][i].type == FD_TYPE_FREE)
            return i;
    }
    return -1;
}

static void fd_clear(fd_entry_t *e)
{
    e->type = FD_TYPE_FREE;
    e->flags = 0;
    e->file.mount = NULL;
    e->file.node_id = 0;
    e->file.flags = 0;
    e->file.pos = 0;
    e->file.size_hint = 0;
    e->file.dir_index = 0;
    e->file.cookie = 0;
}

static int fd_bind_path(fd_entry_t *e, const char *path, uint16_t flags)
{
    int rc = vfs_open(path, flags, &e->file);
    if (rc < 0) {
        fd_clear(e);
        return rc;
    }

    e->type  = FD_TYPE_VFS;
    e->flags = flags;
    return 0;
}

static int user_copy_bytes(uintptr_t uva, void *dst, size_t size)
{
    mm_space_t *space;
    uint8_t    *out = (uint8_t *)dst;
    size_t      copied = 0U;

    if (size == 0U) return 0;
    if (!current_task || !sched_task_is_user(current_task))
        return -EFAULT;

    space = sched_task_space(current_task);
    while (copied < size) {
        uintptr_t cur = uva + copied;
        size_t    page_off = (size_t)(cur & (PAGE_SIZE - 1ULL));
        size_t    chunk = PAGE_SIZE - page_off;
        void     *src;

        if (chunk > size - copied)
            chunk = size - copied;

        src = mmu_space_resolve_ptr(space, cur, chunk);
        if (!src) return -EFAULT;
        memcpy(out + copied, src, chunk);
        copied += chunk;
    }

    return 0;
}

static int user_copy_cstr(uintptr_t uva, char *dst, size_t cap)
{
    if (!uva || !dst || cap == 0U)
        return -EFAULT;

    for (size_t i = 0U; i < cap; i++) {
        int rc = user_copy_bytes(uva + i, &dst[i], 1U);
        if (rc < 0) return rc;
        if (dst[i] == '\0')
            return 0;
    }

    dst[cap - 1U] = '\0';
    return -ENAMETOOLONG;
}

static int user_store_bytes(uintptr_t uva, const void *src, size_t size)
{
    mm_space_t    *space;
    const uint8_t *in = (const uint8_t *)src;
    size_t         copied = 0U;

    if (size == 0U) return 0;
    if (!current_task || !sched_task_is_user(current_task))
        return -EFAULT;

    space = sched_task_space(current_task);
    while (copied < size) {
        uintptr_t cur = uva + copied;
        size_t    page_off = (size_t)(cur & (PAGE_SIZE - 1ULL));
        size_t    chunk = PAGE_SIZE - page_off;
        void     *dst;

        if (chunk > size - copied)
            chunk = size - copied;

        dst = mmu_space_resolve_ptr(space, cur, chunk);
        if (!dst) return -EFAULT;
        memcpy(dst, in + copied, chunk);
        copied += chunk;
    }

    return 0;
}

static int exec_copy_push_string(exec_copy_t *copy, uintptr_t user_ptr,
                                 const char **out)
{
    size_t start;
    int    rc;

    if (!copy || !out || copy->used >= sizeof(copy->strpool))
        return -EFAULT;

    start = copy->used;
    rc = user_copy_cstr(user_ptr, &copy->strpool[start],
                        sizeof(copy->strpool) - start);
    if (rc < 0)
        return rc;

    while (copy->used < sizeof(copy->strpool) &&
           copy->strpool[copy->used] != '\0')
        copy->used++;
    if (copy->used >= sizeof(copy->strpool))
        return -ENAMETOOLONG;

    copy->used++;
    *out = &copy->strpool[start];
    return 0;
}

static int exec_copy_from_user(exec_copy_t *copy,
                               uintptr_t path_uva,
                               uintptr_t argv_uva,
                               uintptr_t envp_uva)
{
    int rc;

    if (!copy) return -EFAULT;
    memset(copy, 0, sizeof(*copy));

    rc = user_copy_cstr(path_uva, copy->path, sizeof(copy->path));
    if (rc < 0) return rc;

    if (argv_uva != 0ULL) {
        for (uint64_t i = 0ULL; i < EXEC_MAX_ARGS; i++) {
            uint64_t ptr = 0ULL;

            rc = user_copy_bytes(argv_uva + i * sizeof(uint64_t), &ptr, sizeof(ptr));
            if (rc < 0) return rc;
            if (ptr == 0ULL) break;

            rc = exec_copy_push_string(copy, (uintptr_t)ptr, &copy->argv[copy->argc]);
            if (rc < 0) return rc;
            copy->argc++;
        }
    }

    if (copy->argc == 0ULL) {
        copy->argv[0] = copy->path;
        copy->argc = 1ULL;
    }

    if (envp_uva != 0ULL) {
        for (uint64_t i = 0ULL; i < EXEC_MAX_ENVP; i++) {
            uint64_t ptr = 0ULL;

            rc = user_copy_bytes(envp_uva + i * sizeof(uint64_t), &ptr, sizeof(ptr));
            if (rc < 0) return rc;
            if (ptr == 0ULL) break;

            rc = exec_copy_push_string(copy, (uintptr_t)ptr, &copy->envp[copy->envc]);
            if (rc < 0) return rc;
            copy->envc++;
        }
    }

    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Tabella globale
 * ════════════════════════════════════════════════════════════════════ */

syscall_entry_t syscall_table[SYSCALL_MAX];

/* ════════════════════════════════════════════════════════════════════
 * Syscall 1 — write
 *
 * args: fd, buf, count
 * RT-safe: dipende dal backend VFS; console/devfs rimane bounded.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_write(uint64_t args[6])
{
    int         fd    = (int)args[0];
    const char *buf   = (const char *)(uintptr_t)args[1];
    uint64_t    count = args[2];

    fd_entry_t *e = fd_get(fd);
    if (!e) return ERR(EBADF);
    if (!buf) return ERR(EFAULT);
    if (!count) return 0;
    if (count > 4096) count = 4096;
    if (e->type != FD_TYPE_VFS) return ERR(EBADF);

    ssize_t rc = vfs_write(&e->file, buf, count);
    if (rc < 0) return ERR((int)-rc);
    return (uint64_t)rc;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 2 — read
 *
 * args: fd, buf, count
 * RT-safe per devfs/console; gli altri backend dipendono dal mount.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_read(uint64_t args[6])
{
    int      fd  = (int)args[0];
    char    *buf = (char *)(uintptr_t)args[1];
    uint64_t cnt = args[2];

    fd_entry_t *e = fd_get(fd);
    if (!e) return ERR(EBADF);
    if (!buf) return ERR(EFAULT);
    if (cnt == 0) return 0;
    if (e->type != FD_TYPE_VFS) return ERR(EBADF);

    if (cnt > 4096) cnt = 4096;
    ssize_t rc = vfs_read(&e->file, buf, cnt);
    if (rc < 0) return ERR((int)-rc);
    return (uint64_t)rc;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 3 — exit
 *
 * args: exit_code
 * Non ritorna mai. Segna il task ZOMBIE e blocca.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_exit(uint64_t args[6])
{
    (void)args;
    if (current_task) {
        current_task->state = TCB_STATE_ZOMBIE;
        schedule();
    }
    return 0;   /* unreachable */
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 4 — open
 *
 * args: path, flags, mode (ignorato)
 * Instrada la risoluzione path sul layer VFS.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_open(uint64_t args[6])
{
    const char *path  = (const char *)(uintptr_t)args[0];
    uint16_t    oflags = (uint16_t)args[1];
    int         idx = task_idx();
    int         rc;

    if (!path) return ERR(EFAULT);

    int fd = fd_alloc();
    if (fd < 0) return ERR(ENFILE);

    rc = fd_bind_path(&fd_tables[idx][fd], path, oflags);
    if (rc < 0) return ERR(-rc);

    return (uint64_t)fd;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 5 — close
 *
 * args: fd
 * Non si chiudono fd 0/1/2 (protezione stdin/stdout/stderr).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_close(uint64_t args[6])
{
    int fd = (int)args[0];

    if (fd < 0 || fd >= MAX_FD) return ERR(EBADF);

    /* Protezione su fd standard */
    if (fd <= 2) return ERR(EBADF);

    int idx = task_idx();
    fd_entry_t *e = &fd_tables[idx][fd];
    if (e->type == FD_TYPE_FREE) return ERR(EBADF);

    if (e->type == FD_TYPE_VFS) {
        int rc = vfs_close(&e->file);
        if (rc < 0) return ERR(-rc);
    }

    fd_clear(e);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 6 — fstat
 *
 * args: fd, struct stat *buf
 * Popola la struct stat minimale di EnlilOS.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_fstat(uint64_t args[6])
{
    int     fd  = (int)args[0];
    stat_t *buf = (stat_t *)(uintptr_t)args[1];

    fd_entry_t *e = fd_get(fd);
    if (!e) return ERR(EBADF);
    if (!buf) return ERR(EFAULT);
    if (e->type != FD_TYPE_VFS) return ERR(EBADF);

    int rc = vfs_stat(&e->file, buf);
    if (rc < 0) return ERR(-rc);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 7 — mmap
 *
 * args: addr(hint), length, prot, flags, fd, offset
 * Solo MAP_ANONYMOUS supportato. MMU identity-mapped → PA == VA.
 * Usa il buddy allocator per allocare pagine fisiche contigue.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_mmap(uint64_t args[6])
{
    /* uint64_t addr_hint = args[0]; (ignorato) */
    uint64_t length = args[1];
    /* uint32_t prot  = (uint32_t)args[2]; */
    uint32_t flags  = (uint32_t)args[3];
    /* int      mapfd = (int)args[4]; */
    /* uint64_t off   = args[5]; */

    if (length == 0)                return MAP_FAILED_VA;
    if (!(flags & MAP_ANONYMOUS))   return MAP_FAILED_VA;

    /* Calcola ordine buddy (pagine = ceiling(length / 4096)) */
    uint64_t pages = (length + 4095ULL) / 4096ULL;
    if (pages > 256) return MAP_FAILED_VA;  /* max 1MB per chiamata */

    uint32_t order = 0;
    uint64_t p = pages;
    while (p > 1) { p >>= 1; order++; }
    if ((1ULL << order) < pages) order++;
    if (order > 10) order = 10;

    uint64_t pa = phys_alloc_pages(order);
    if (pa == 0) return MAP_FAILED_VA;

    /* Zero-fill: evita leakage di dati kernel in user-space */
    uint8_t *ptr = (uint8_t *)(uintptr_t)pa;
    uint64_t alloc_bytes = (1ULL << order) * 4096ULL;
    for (uint64_t i = 0; i < alloc_bytes; i++) ptr[i] = 0;

    return pa;  /* identity map: PA == VA */
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 8 — munmap
 *
 * args: addr, length
 * Rilascia le pagine al buddy allocator.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_munmap(uint64_t args[6])
{
    uint64_t addr   = args[0];
    uint64_t length = args[1];

    if (addr == 0 || length == 0) return ERR(EINVAL);

    uint64_t pages = (length + 4095ULL) / 4096ULL;
    uint32_t order = 0;
    uint64_t p = pages;
    while (p > 1) { p >>= 1; order++; }
    if ((1ULL << order) < pages) order++;
    if (order > 10) order = 10;

    phys_free_pages(addr, order);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 9 — brk
 *
 * args: new_brk (0 = query)
 * Gestisce il program break per il task corrente.
 * Il break cresce verso l'alto a partire da HEAP_BASE.
 * Non alloca pagine (usa mmap per quello): traccia solo il puntatore.
 *
 * RT: O(1), nessuna allocazione.
 * ════════════════════════════════════════════════════════════════════ */

/*
 * Ogni task ha uno spazio heap virtuale distinto:
 *   task[i] → base = HEAP_BASE + i * HEAP_TASK_SIZE
 *
 * Con MMU identity-mapped, questo richiede che le aree non si
 * sovrappongano. 4MB per task è più che sufficiente per M3-M6.
 */
#define HEAP_BASE       0x60000000ULL   /* 1.5 GB fisico */
#define HEAP_TASK_SIZE  0x00400000ULL   /* 4 MB per task */

static uint64_t sys_brk(uint64_t args[6])
{
    if (!current_task) return ERR(ENOMEM);

    int idx = task_idx();

    /* Prima chiamata: inizializza il break alla base dell'area del task */
    if (task_brk[idx] == 0)
        task_brk[idx] = HEAP_BASE + (uint64_t)idx * HEAP_TASK_SIZE;

    uint64_t new_brk = args[0];

    /* args[0] == 0 → query del break corrente */
    if (new_brk == 0)
        return task_brk[idx];

    uint64_t base  = HEAP_BASE + (uint64_t)idx * HEAP_TASK_SIZE;
    uint64_t limit = base + HEAP_TASK_SIZE;

    if (new_brk < base || new_brk >= limit)
        return task_brk[idx];   /* ritorna vecchio break = ENOMEM implicito */

    task_brk[idx] = new_brk;
    return new_brk;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 10 — execve
 *
 * Stub: richiede ELF loader (M6-02).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_execve(uint64_t args[6])
{
    exec_copy_t       *copy;
    elf_image_t        image;
    exception_frame_t *frame = active_syscall_frame;
    mm_space_t        *old_mm;
    int                rc;

    if (!current_task || !sched_task_is_user(current_task) || !frame)
        return ERR(ENOSYS);

    copy = (exec_copy_t *)kmalloc((uint32_t)sizeof(exec_copy_t));
    if (!copy)
        return ERR(ENOMEM);

    rc = exec_copy_from_user(copy,
                             (uintptr_t)args[0],
                             (uintptr_t)args[1],
                             (uintptr_t)args[2]);
    if (rc < 0) {
        kfree(copy);
        return ERR(-rc);
    }

    rc = elf64_load_from_path_exec(copy->path,
                                   copy->argv, copy->argc,
                                   copy->envp, copy->envc,
                                   &image);
    if (rc < 0) {
        kfree(copy);
        return ERR(EIO);
    }

    old_mm = sched_task_space(current_task);
    if (sched_task_rebind_user(current_task, image.space,
                               image.entry, image.user_sp,
                               image.argc, image.argv,
                               image.envp, image.auxv) < 0) {
        elf64_unload_image(&image);
        kfree(copy);
        return ERR(EIO);
    }

    mmu_activate_space(image.space);
    if (old_mm && old_mm != image.space && old_mm != mmu_kernel_space())
        mmu_space_destroy(old_mm);

    task_brk[task_idx()] = 0ULL;
    memset(frame->x, 0, sizeof(frame->x));
    frame->x[0] = image.argc;
    frame->x[1] = image.argv;
    frame->x[2] = image.envp;
    frame->x[3] = image.auxv;
    frame->sp   = image.user_sp;
    frame->pc   = image.entry;
    frame->spsr = 0ULL;
    active_syscall_replaced = 1U;
    kfree(copy);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 11 — fork
 *
 * Stub: richiede copy-on-write MMU (M6+).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_fork(uint64_t args[6])
{
    (void)args;
    return ERR(ENOSYS);
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 12 — waitpid
 *
 * args: pid, status_ptr (ignorato), options, timeout_ms
 * RT-safe con timeout_ms > 0: attesa bounded garantita.
 *
 * pid > 0: attende quel task specifico
 * pid == -1: nessun parent tracking ancora → ECHILD
 * WNOHANG: polling non-blocking (RT-safe incondizionatamente)
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_waitpid(uint64_t args[6])
{
    int32_t  pid_arg    = (int32_t)args[0];
    /* args[1] = status ptr (ignorato) */
    uint32_t options    = (uint32_t)args[2];
    uint64_t timeout_ms = args[3];

    /* pid = -1: attesa di "qualsiasi figlio" — non supportata senza fork */
    if (pid_arg < 0) return ERR(ECHILD);

    sched_tcb_t *t = sched_task_find((uint32_t)pid_arg);
    if (!t) return ERR(ECHILD);

    /* WNOHANG: non blocca, ritorna 0 se non ancora zombie */
    if (options & WNOHANG)
        return (t->state == TCB_STATE_ZOMBIE) ? (uint64_t)pid_arg : 0;

    /*
     * Attesa con timeout.
     * timeout_ms = 0 → attesa illimitata (sconsigliato per task RT).
     * Il loop cede la CPU ad ogni iterazione → non è busy-wait puro.
     */
    uint64_t deadline = (timeout_ms > 0)
        ? timer_now_ms() + timeout_ms
        : (uint64_t)-1ULL;

    while (t->state != TCB_STATE_ZOMBIE) {
        if (timer_now_ms() >= deadline)
            return ERR(EAGAIN);
        sched_yield();
    }

    return (uint64_t)pid_arg;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 13 — clock_gettime
 *
 * args: clock_id, struct timespec *ts
 * Scrive { tv_sec, tv_nsec } nel puntatore ts.
 * RT-safe O(1): legge CNTPCT_EL0 direttamente.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_clock_gettime(uint64_t args[6])
{
    /* clock_id: CLOCK_REALTIME=0, CLOCK_MONOTONIC=1 — entrambi usano CNTPCT */
    timespec_t *ts = (timespec_t *)(uintptr_t)args[1];

    uint64_t ns = timer_now_ns();

    if (ts) {
        ts->tv_sec  = (int64_t)(ns / 1000000000ULL);
        ts->tv_nsec = (int64_t)(ns % 1000000000ULL);
    }

    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 14 — getdents
 *
 * args: fd, sys_dirent_t *buf, max_entries
 * Copia fino a max_entries entry della directory aperta.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_getdents(uint64_t args[6])
{
    int           fd = (int)args[0];
    uintptr_t     buf_uva = (uintptr_t)args[1];
    uint32_t      max_entries = (uint32_t)args[2];
    fd_entry_t   *e = fd_get(fd);
    uint32_t      copied = 0U;

    if (!e) return ERR(EBADF);
    if (!buf_uva) return ERR(EFAULT);
    if (e->type != FD_TYPE_VFS) return ERR(EBADF);
    if (max_entries == 0U) return 0;
    if (max_entries > 64U) max_entries = 64U;

    while (copied < max_entries) {
        vfs_dirent_t  ent;
        sys_dirent_t  out;
        int           rc = vfs_readdir(&e->file, &ent);

        if (rc == -ENOENT)
            break;
        if (rc < 0)
            return ERR(-rc);

        memset(&out, 0, sizeof(out));
        memcpy(out.name, ent.name, sizeof(out.name));
        out.mode = ent.mode;
        rc = user_store_bytes(buf_uva + copied * sizeof(out), &out, sizeof(out));
        if (rc < 0)
            return ERR(-rc);
        copied++;
    }

    return copied;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 15 — task_snapshot
 *
 * args: task_snapshot_t *buf, max_entries
 * Espone lo snapshot dei task per 'top' / monitor user-space.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_task_snapshot(uint64_t args[6])
{
    uintptr_t          buf_uva = (uintptr_t)args[0];
    uint32_t           max_entries = (uint32_t)args[1];
    sched_task_info_t *info;
    uint32_t           count;
    int                rc;

    if (!buf_uva) return ERR(EFAULT);
    if (max_entries == 0U) return 0;
    if (max_entries > SCHED_MAX_TASKS) max_entries = SCHED_MAX_TASKS;

    info = (sched_task_info_t *)kmalloc(max_entries * sizeof(*info));
    if (!info) return ERR(ENOMEM);

    count = sched_task_snapshot(info, max_entries);
    for (uint32_t i = 0U; i < count; i++) {
        task_snapshot_t snap;

        memset(&snap, 0, sizeof(snap));
        snap.pid         = info[i].pid;
        snap.priority    = info[i].priority;
        snap.state       = info[i].state;
        snap.flags       = info[i].flags;
        snap.runtime_ns  = info[i].runtime_ns;
        snap.budget_ns   = info[i].budget_ns;
        snap.period_ms   = info[i].period_ms;
        snap.deadline_ms = info[i].deadline_ms;
        memcpy(snap.name, info[i].name, sizeof(snap.name));

        rc = user_store_bytes(buf_uva + i * sizeof(snap), &snap, sizeof(snap));
        if (rc < 0) {
            kfree(info);
            return ERR(-rc);
        }
    }

    kfree(info);
    return count;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 16 — spawn
 *
 * args: path, pid_out, priority
 * Lancia un ELF statico/dinamico senza sostituire il chiamante.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_spawn(uint64_t args[6])
{
    char      path[EXEC_MAX_PATH];
    uintptr_t path_uva = (uintptr_t)args[0];
    uintptr_t pid_uva  = (uintptr_t)args[1];
    uint8_t   prio     = (args[2] <= 255ULL) ? (uint8_t)args[2] : PRIO_NORMAL;
    uint32_t  pid = 0U;
    int       rc;

    if (!path_uva || !pid_uva)
        return ERR(EFAULT);

    rc = user_copy_cstr(path_uva, path, sizeof(path));
    if (rc < 0)
        return ERR(-rc);

    rc = elf64_spawn_path(path, path, prio, &pid);
    if (rc < 0)
        return ERR(EIO);

    rc = user_store_bytes(pid_uva, &pid, sizeof(pid));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * ENOSYS fallback
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_enosys(uint64_t args[6])
{
    (void)args;
    return ERR(ENOSYS);
}

/* ════════════════════════════════════════════════════════════════════
 * syscall_init — popola la tabella e inizializza le strutture dati
 * ════════════════════════════════════════════════════════════════════ */
void syscall_init(void)
{
    /* 1. Porta su line discipline + VFS bootstrap */
    tty_init();
    vfs_init();

    /* 2. Inizializza fd_table: fd 0/1/2 = VFS devfs per tutti i task */
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        for (int j = 0; j < MAX_FD; j++)
            fd_clear(&fd_tables[i][j]);

        (void)fd_bind_path(&fd_tables[i][0], "/dev/stdin",  O_RDONLY);
        (void)fd_bind_path(&fd_tables[i][1], "/dev/stdout", O_WRONLY);
        (void)fd_bind_path(&fd_tables[i][2], "/dev/stderr", O_WRONLY);
    }

    /* 3. Inizializza task_brk a zero (lazy-init alla prima chiamata brk) */
    for (int i = 0; i < SCHED_MAX_TASKS; i++)
        task_brk[i] = 0;

    /* 4. Riempi la tabella con ENOSYS */
    for (int i = 0; i < SYSCALL_MAX; i++) {
        syscall_table[i].handler = sys_enosys;
        syscall_table[i].flags   = 0;
        syscall_table[i].name    = "enosys";
    }

    /* 5. Registra le syscall base + estensioni per la shell user-space */
    syscall_table[SYS_WRITE] = (syscall_entry_t){
        sys_write, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "write"
    };
    syscall_table[SYS_READ] = (syscall_entry_t){
        sys_read, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "read"
    };
    syscall_table[SYS_EXIT] = (syscall_entry_t){
        sys_exit, SYSCALL_FLAG_RT, "exit"
    };
    syscall_table[SYS_OPEN] = (syscall_entry_t){
        sys_open, 0, "open"
    };
    syscall_table[SYS_CLOSE] = (syscall_entry_t){
        sys_close, SYSCALL_FLAG_RT, "close"
    };
    syscall_table[SYS_FSTAT] = (syscall_entry_t){
        sys_fstat, 0, "fstat"
    };
    syscall_table[SYS_MMAP] = (syscall_entry_t){
        sys_mmap, 0, "mmap"
    };
    syscall_table[SYS_MUNMAP] = (syscall_entry_t){
        sys_munmap, 0, "munmap"
    };
    syscall_table[SYS_BRK] = (syscall_entry_t){
        sys_brk, SYSCALL_FLAG_RT, "brk"
    };
    syscall_table[SYS_EXECVE] = (syscall_entry_t){
        sys_execve, 0, "execve"
    };
    syscall_table[SYS_FORK] = (syscall_entry_t){
        sys_fork, 0, "fork"
    };
    syscall_table[SYS_WAITPID] = (syscall_entry_t){
        sys_waitpid, SYSCALL_FLAG_RT, "waitpid"
    };
    syscall_table[SYS_CLOCK_GETTIME] = (syscall_entry_t){
        sys_clock_gettime, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "clock_gettime"
    };
    syscall_table[SYS_GETDENTS] = (syscall_entry_t){
        sys_getdents, 0, "getdents"
    };
    syscall_table[SYS_TASK_SNAPSHOT] = (syscall_entry_t){
        sys_task_snapshot, 0, "task_snapshot"
    };
    syscall_table[SYS_SPAWN] = (syscall_entry_t){
        sys_spawn, 0, "spawn"
    };

    uart_puts("[SYSCALL] 16 syscall base/UX registrate\n");
    uart_puts("[SYSCALL] fd_table: 0/1/2=VFS(/dev/std*) per 32 task slot\n");
}

/* ════════════════════════════════════════════════════════════════════
 * syscall_dispatch — entry point da exception_handler (EC=0x15)
 *
 * Legge nr da frame->x[8], args da x0–x5, scrive risultato in x0.
 * WCET: O(1).
 * ════════════════════════════════════════════════════════════════════ */
void syscall_dispatch(exception_frame_t *frame)
{
    uint64_t nr = frame->x[8];
    uint64_t ret;

    if (nr >= SYSCALL_MAX) {
        frame->x[0] = ERR(ENOSYS);
        return;
    }

    uint64_t args[6] = {
        frame->x[0], frame->x[1], frame->x[2],
        frame->x[3], frame->x[4], frame->x[5],
    };

    active_syscall_frame = frame;
    active_syscall_replaced = 0U;
    ret = syscall_table[nr].handler(args);
    if (!active_syscall_replaced)
        frame->x[0] = ret;
    active_syscall_frame = NULL;
    active_syscall_replaced = 0U;
}
