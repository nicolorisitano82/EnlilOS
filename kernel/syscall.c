/*
 * EnlilOS Microkernel - Syscall Base (M3-01 / M3-02 / M5-02)
 *
 * Implementa:
 *   - syscall_table[256]: dispatch O(1)
 *   - fd_table[SCHED_MAX_TASKS][MAX_FD]: file descriptor per processo condiviso
 *   - task_brk[SCHED_MAX_TASKS]: program break per processo condiviso
 *   - syscall base + estensioni user-space per M7-02
 *
 * RT: nessuna allocazione nel dispatch path. WCET = O(1).
 */

#include "syscall.h"
#include "sock.h"
#include "cap.h"
#include "futex.h"
#include "kdebug.h"
#include "keyboard.h"
#include "linux_compat.h"
#include "net.h"
#include "net_ipc.h"
#include "elf_loader.h"
#include "kheap.h"
#include "kmon.h"
#include "ksem.h"
#include "mreact.h"
#include "microkernel.h"
#include "mmu.h"
#include "sched.h"
#include "timer.h"
#include "tty.h"
#include "pmm.h"
#include "uart.h"
#include "types.h"
#include "blk.h"
#include "blk_ipc.h"
#include "vfs.h"
#include "vfs_ipc.h"
#include "vmm.h"
#include "shutdown.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

/* ════════════════════════════════════════════════════════════════════
 * File descriptor table
 * ════════════════════════════════════════════════════════════════════ */

#define FD_TYPE_FREE    0
#define FD_TYPE_VFS     1
#define FD_TYPE_VFSD    2
#define FD_TYPE_PIPE    3
#define FD_TYPE_SOCK    4

#define VFSD_HANDLE_MAX 64U
#define FD_OBJECT_MAX   (SCHED_MAX_TASKS * MAX_FD)
#define PIPE_POOL_MAX   32U
#define PIPE_BUF_SIZE   4096U
#define SOCK_REMOTE_NETD_TAG 0x80000000U
#define FLOCK_MAX_LOCKS 32U
#define FLOCK_MAX_HOLDERS 16U
#define FLOCK_MAX_WAITERS SCHED_MAX_TASKS

#define PIPE_END_READ   0U
#define PIPE_END_WRITE  1U

#define DEV_NODE_DIR        1U
#define DEV_NODE_CONSOLE    2U
#define DEV_NODE_TTY        3U
#define DEV_NODE_STDIN      4U
#define DEV_NODE_STDOUT     5U
#define DEV_NODE_STDERR     6U
#define DEV_NODE_NULL       7U

typedef struct {
    uint8_t  in_use;
    uint8_t  _pad0[3];
    vfs_file_t file;
} vfs_srv_handle_t;

#define EXEC_MAX_PATH       SCHED_EXEC_PATH_MAX

typedef struct {
    uint8_t    in_use;
    uint8_t    type;
    uint8_t    pipe_end;
    uint8_t    _pad0;
    uint16_t   flags;
    uint16_t   _pad1;
    uint32_t   refcount;
    uint32_t   remote_handle;
    void      *pipe;
    vfs_file_t file;
    char       path[EXEC_MAX_PATH];
} fd_object_t;

typedef struct {
    fd_object_t *obj;
    uint8_t      fd_flags;
    uint8_t      _pad0[7];
} fd_entry_t;

#define FD_ENTRY_CLOEXEC   0x01U

typedef struct {
    uint8_t  in_use;
    uint8_t  _pad0[3];
    uint32_t readers;
    uint32_t writers;
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t size;
    uint8_t  buf[PIPE_BUF_SIZE];
} pipe_t;

/*
 * Indicizzata per [proc_slot][fd].
 * Tutti i processi partono con 0/1/2 preimpostati come CONSOLE.
 */
static fd_entry_t fd_tables[SCHED_MAX_TASKS][MAX_FD];
static vfs_srv_handle_t vfs_srv_tables[SCHED_MAX_TASKS][VFSD_HANDLE_MAX];
static fd_object_t fd_objects[FD_OBJECT_MAX];
static pipe_t      pipe_pool[PIPE_POOL_MAX];

typedef struct {
    uint8_t            in_use;
    uint8_t            exclusive;
    uint8_t            holder_count;
    uint8_t            _pad0;
    const vfs_mount_t *mount;
    uint32_t           node_id;
    uint32_t           _pad1;
    char               path[EXEC_MAX_PATH];
    fd_object_t       *holders[FLOCK_MAX_HOLDERS];
    struct flock_waiter *wait_head;
    struct flock_waiter *wait_tail;
} flock_entry_t;

typedef struct flock_waiter {
    uint8_t             in_use;
    uint8_t             active;
    uint8_t             exclusive;
    uint8_t             wake_reason;
    sched_tcb_t        *task;
    fd_object_t        *obj;
    flock_entry_t      *entry;
    struct flock_waiter *next;
} flock_waiter_t;

#define FLOCK_WAKE_NONE   0U
#define FLOCK_WAKE_LOCK   1U
#define FLOCK_WAKE_INTR   2U

static flock_entry_t flock_table[FLOCK_MAX_LOCKS];
static flock_waiter_t flock_waiters[FLOCK_MAX_WAITERS];

static void sock_sync_fd_flags(fd_object_t *obj);
static int  netd_proxy_close(uint32_t handle);
static void syscall_copy_fixed_string(char *dst, size_t cap, const char *src);

static int sock_handle_is_remote_netd(uint32_t handle)
{
    return (handle & SOCK_REMOTE_NETD_TAG) != 0U;
}

static uint32_t sock_handle_to_remote_netd(uint32_t handle)
{
    return handle | SOCK_REMOTE_NETD_TAG;
}

static uint32_t sock_handle_from_remote_netd(uint32_t handle)
{
    return handle & ~SOCK_REMOTE_NETD_TAG;
}

/* Owner check per blkd: solo il server proprietario della porta "block" può usare le blk_boot_* */
static int blk_srv_owner_ok(void)
{
    port_t *port;

    if (!current_task || !sched_task_is_user(current_task))
        return 0;

    port = mk_port_lookup("block");
    return (port && port->owner_tid == sched_task_tgid(current_task)) ? 1 : 0;
}

/* Owner check per netd: solo il server proprietario della porta "net" può usare le net_boot_* */
static int net_srv_owner_ok(void)
{
    port_t *port;

    if (!current_task || !sched_task_is_user(current_task))
        return 0;

    port = mk_port_lookup("net");
    return (port && port->owner_tid == sched_task_tgid(current_task)) ? 1 : 0;
}

/*
 * Program break per processo (indirizzato come sopra).
 * brk=0 → non ancora inizializzato.
 */
static uint64_t task_brk[SCHED_MAX_TASKS];
static exception_frame_t *active_syscall_frame;
static uint8_t            active_syscall_replaced;

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

static int  vfsd_proxy_close(uint32_t handle);
static void stat_fill(stat_t *st, uint32_t mode, uint64_t size,
                      uint32_t blksize);
static void syscall_copy_fixed_string(char *dst, size_t cap, const char *src);

/* ── Helpers ──────────────────────────────────────────────────────── */

static void syscall_uart_put_u64(uint64_t v)
{
    char buf[32];
    int  len = 0;

    if (v == 0ULL) {
        uart_putc('0');
        return;
    }

    while (v != 0ULL && len < (int)sizeof(buf)) {
        buf[len++] = (char)('0' + (v % 10ULL));
        v /= 10ULL;
    }
    while (len > 0)
        uart_putc(buf[--len]);
}

static void syscall_uart_put_i64(int64_t v)
{
    uint64_t mag;

    if (v < 0) {
        uart_putc('-');
        mag = (uint64_t)(-(v + 1LL)) + 1ULL;
    } else {
        mag = (uint64_t)v;
    }
    syscall_uart_put_u64(mag);
}

static void stat_fill(stat_t *st, uint32_t mode, uint64_t size,
                      uint32_t blksize)
{
    if (!st) return;
    st->st_mode    = mode;
    st->st_blksize = blksize;
    st->st_size    = size;
    st->st_blocks  = (size + (uint64_t)blksize - 1ULL) / (uint64_t)blksize;
}

/* Indice nel fd_table/spazi condivisi per il processo corrente */
static inline int task_idx(void)
{
    if (!current_task) return 0;
    return (int)(sched_task_proc_slot(current_task) % SCHED_MAX_TASKS);
}

static int syscall_streq(const char *a, const char *b)
{
    if (!a || !b)
        return 0;

    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int flock_object_supported(const fd_object_t *obj)
{
    if (!obj)
        return 0;
    if (obj->type != FD_TYPE_VFS && obj->type != FD_TYPE_VFSD)
        return 0;
    if (obj->file.mount)
        return 1;
    return obj->path[0] != '\0';
}

static flock_waiter_t *flock_waiter_find_task(const sched_tcb_t *task)
{
    if (!task)
        return NULL;

    for (uint32_t i = 0U; i < FLOCK_MAX_WAITERS; i++) {
        if (flock_waiters[i].in_use && flock_waiters[i].task == task)
            return &flock_waiters[i];
    }
    return NULL;
}

static flock_waiter_t *flock_waiter_alloc(sched_tcb_t *task, fd_object_t *obj,
                                          flock_entry_t *entry, int exclusive)
{
    flock_waiter_t *waiter = flock_waiter_find_task(task);

    if (waiter)
        return waiter->active ? NULL : waiter;

    for (uint32_t i = 0U; i < FLOCK_MAX_WAITERS; i++) {
        if (!flock_waiters[i].in_use) {
            waiter = &flock_waiters[i];
            memset(waiter, 0, sizeof(*waiter));
            waiter->in_use = 1U;
            break;
        }
    }

    if (!waiter)
        return NULL;

    waiter->active = 1U;
    waiter->exclusive = exclusive ? 1U : 0U;
    waiter->wake_reason = FLOCK_WAKE_NONE;
    waiter->task = task;
    waiter->obj = obj;
    waiter->entry = entry;
    waiter->next = NULL;
    return waiter;
}

static void flock_waiter_reset(flock_waiter_t *waiter)
{
    if (!waiter)
        return;
    memset(waiter, 0, sizeof(*waiter));
}

static void flock_waitq_push_tail(flock_entry_t *entry, flock_waiter_t *waiter)
{
    if (!entry || !waiter)
        return;

    waiter->next = NULL;
    if (entry->wait_tail)
        entry->wait_tail->next = waiter;
    else
        entry->wait_head = waiter;
    entry->wait_tail = waiter;
}

static void flock_waitq_remove(flock_entry_t *entry, flock_waiter_t *waiter)
{
    flock_waiter_t *prev = NULL;
    flock_waiter_t *cur;

    if (!entry || !waiter)
        return;

    cur = entry->wait_head;
    while (cur) {
        if (cur == waiter) {
            if (prev)
                prev->next = cur->next;
            else
                entry->wait_head = cur->next;
            if (entry->wait_tail == cur)
                entry->wait_tail = prev;
            cur->next = NULL;
            cur->active = 0U;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static flock_waiter_t *flock_waitq_pop_head(flock_entry_t *entry)
{
    flock_waiter_t *waiter;

    if (!entry || !entry->wait_head)
        return NULL;

    waiter = entry->wait_head;
    entry->wait_head = waiter->next;
    if (!entry->wait_head)
        entry->wait_tail = NULL;
    waiter->next = NULL;
    waiter->active = 0U;
    return waiter;
}

static int flock_entry_matches(const flock_entry_t *entry, const fd_object_t *obj)
{
    if (!entry || !entry->in_use || !obj)
        return 0;

    if (entry->mount && obj->file.mount)
        return entry->mount == obj->file.mount &&
               entry->node_id == obj->file.node_id;

    if (entry->path[0] != '\0' && obj->path[0] != '\0')
        return syscall_streq(entry->path, obj->path);

    return 0;
}

static int flock_holder_index(const flock_entry_t *entry, const fd_object_t *obj)
{
    if (!entry || !obj)
        return -1;

    for (uint32_t i = 0U; i < entry->holder_count; i++) {
        if (entry->holders[i] == obj)
            return (int)i;
    }
    return -1;
}

static void flock_entry_clear(flock_entry_t *entry)
{
    if (!entry)
        return;
    while (entry->wait_head) {
        flock_waiter_t *waiter = flock_waitq_pop_head(entry);
        if (!waiter)
            break;
        waiter->wake_reason = FLOCK_WAKE_INTR;
        if (waiter->task && waiter->task->state == TCB_STATE_BLOCKED)
            sched_unblock(waiter->task);
        flock_waiter_reset(waiter);
    }
    memset(entry, 0, sizeof(*entry));
}

static flock_entry_t *flock_find_entry(const fd_object_t *obj)
{
    for (uint32_t i = 0U; i < FLOCK_MAX_LOCKS; i++) {
        if (flock_entry_matches(&flock_table[i], obj))
            return &flock_table[i];
    }
    return NULL;
}

static flock_entry_t *flock_alloc_entry(const fd_object_t *obj)
{
    flock_entry_t *entry;

    if (!obj || !flock_object_supported(obj))
        return NULL;

    for (uint32_t i = 0U; i < FLOCK_MAX_LOCKS; i++) {
        if (flock_table[i].in_use)
            continue;

        entry = &flock_table[i];
        memset(entry, 0, sizeof(*entry));
        entry->in_use  = 1U;
        entry->mount   = obj->file.mount;
        entry->node_id = obj->file.node_id;
        if (!entry->mount)
            syscall_copy_fixed_string(entry->path, sizeof(entry->path), obj->path);
        return entry;
    }

    return NULL;
}

static void flock_drop_holder(flock_entry_t *entry, uint32_t idx)
{
    if (!entry || idx >= entry->holder_count)
        return;

    while (idx + 1U < entry->holder_count) {
        entry->holders[idx] = entry->holders[idx + 1U];
        idx++;
    }
    if (entry->holder_count > 0U)
        entry->holder_count--;
    if (entry->holder_count != 0U && entry->exclusive && entry->holder_count > 1U)
        entry->exclusive = 0U;
}

static int flock_task_refs_object(const sched_tcb_t *task, const fd_object_t *obj)
{
    int slot;

    if (!task || !obj)
        return 0;

    slot = (int)sched_task_proc_slot(task);
    if (slot < 0 || slot >= SCHED_MAX_TASKS)
        return 0;

    for (int fd = 0; fd < MAX_FD; fd++) {
        if (fd_tables[slot][fd].obj == obj)
            return 1;
    }
    return 0;
}

static int flock_deadlock_reaches_task(const sched_tcb_t *target,
                                       const sched_tcb_t *task,
                                       uint32_t depth)
{
    flock_waiter_t *waiter;

    if (!target || !task)
        return 0;
    if (target == task)
        return 1;
    if (depth >= SCHED_MAX_TASKS)
        return 0;

    waiter = flock_waiter_find_task(task);
    if (!waiter || !waiter->active || !waiter->entry)
        return 0;

    for (uint32_t i = 0U; i < waiter->entry->holder_count; i++) {
        fd_object_t *holder_obj = waiter->entry->holders[i];

        if (!holder_obj)
            continue;

        for (uint32_t j = 0U; j < sched_task_count_total(); j++) {
            sched_tcb_t *owner = sched_task_at(j);

            if (!owner || owner->state == TCB_STATE_ZOMBIE)
                continue;
            if (!flock_task_refs_object(owner, holder_obj))
                continue;
            if (flock_deadlock_reaches_task(target, owner, depth + 1U))
                return 1;
        }
    }

    return 0;
}

static int flock_detect_deadlock(fd_object_t *obj, flock_entry_t *entry)
{
    if (!obj || !entry || !current_task)
        return 0;

    for (uint32_t i = 0U; i < entry->holder_count; i++) {
        fd_object_t *holder_obj = entry->holders[i];

        if (!holder_obj)
            continue;
        for (uint32_t j = 0U; j < sched_task_count_total(); j++) {
            sched_tcb_t *owner = sched_task_at(j);

            if (!owner || owner->state == TCB_STATE_ZOMBIE)
                continue;
            if (!flock_task_refs_object(owner, holder_obj))
                continue;
            if (flock_deadlock_reaches_task(current_task, owner, 0U))
                return 1;
        }
    }

    return 0;
}

static void flock_wake_waiters(flock_entry_t *entry)
{
    sched_tcb_t *wake_list[SCHED_MAX_TASKS];
    uint32_t woke = 0U;

    if (!entry || !entry->in_use || entry->holder_count != 0U)
        return;

    while (entry->wait_head && woke < SCHED_MAX_TASKS) {
        flock_waiter_t *waiter = entry->wait_head;

        if (!waiter)
            break;

        if (waiter->exclusive) {
            waiter = flock_waitq_pop_head(entry);
            entry->exclusive = 1U;
            entry->holders[0] = waiter->obj;
            entry->holder_count = 1U;
            waiter->wake_reason = FLOCK_WAKE_LOCK;
            wake_list[woke++] = waiter->task;
            break;
        }

        if (entry->holder_count >= FLOCK_MAX_HOLDERS)
            break;
        waiter = flock_waitq_pop_head(entry);
        entry->holders[entry->holder_count++] = waiter->obj;
        entry->exclusive = 0U;
        waiter->wake_reason = FLOCK_WAKE_LOCK;
        wake_list[woke++] = waiter->task;

        if (!entry->wait_head || entry->wait_head->exclusive)
            break;
    }

    for (uint32_t i = 0U; i < woke; i++) {
        if (wake_list[i] && wake_list[i]->state == TCB_STATE_BLOCKED)
            sched_unblock(wake_list[i]);
    }
}

static void flock_release_object(fd_object_t *obj)
{
    if (!obj)
        return;

    for (uint32_t i = 0U; i < FLOCK_MAX_LOCKS; i++) {
        flock_entry_t *entry = &flock_table[i];
        int            idx;

        if (!entry->in_use)
            continue;

        idx = flock_holder_index(entry, obj);
        if (idx < 0)
            continue;

        flock_drop_holder(entry, (uint32_t)idx);
        flock_wake_waiters(entry);
        if (entry->holder_count == 0U && entry->wait_head == NULL)
            flock_entry_clear(entry);
    }
}

static int flock_unlock_object(fd_object_t *obj)
{
    flock_entry_t *entry;
    int            idx;

    if (!obj)
        return -EBADF;

    entry = flock_find_entry(obj);
    if (!entry)
        return 0;

    idx = flock_holder_index(entry, obj);
    if (idx >= 0) {
        flock_drop_holder(entry, (uint32_t)idx);
        flock_wake_waiters(entry);
        if (entry->holder_count == 0U && entry->wait_head == NULL)
            flock_entry_clear(entry);
    }
    return 0;
}

static int flock_try_lock_object(fd_object_t *obj, uint32_t operation)
{
    flock_entry_t *entry;
    int            idx;
    int            exclusive;

    if (!flock_object_supported(obj))
        return -EINVAL;

    exclusive = (operation == LINUX_LOCK_EX) ? 1 : 0;
    entry = flock_find_entry(obj);
    if (!entry) {
        entry = flock_alloc_entry(obj);
        if (!entry)
            return -ENOMEM;
        entry->exclusive = (uint8_t)exclusive;
        entry->holders[0] = obj;
        entry->holder_count = 1U;
        return 0;
    }

    idx = flock_holder_index(entry, obj);
    if (entry->wait_head && idx < 0)
        return -EAGAIN;
    if (exclusive) {
        if (idx >= 0) {
            if (entry->holder_count == 1U) {
                entry->exclusive = 1U;
                return 0;
            }
            return -EAGAIN;
        }
        if (entry->holder_count != 0U)
            return -EAGAIN;
        entry->exclusive = 1U;
        entry->holders[0] = obj;
        entry->holder_count = 1U;
        return 0;
    }

    if (entry->exclusive) {
        if (idx >= 0 && entry->holder_count == 1U) {
            entry->exclusive = 0U;
            return 0;
        }
        return -EAGAIN;
    }

    if (idx >= 0)
        return 0;
    if (entry->holder_count >= FLOCK_MAX_HOLDERS)
        return -ENOMEM;

    entry->holders[entry->holder_count++] = obj;
    return 0;
}

static fd_entry_t *fd_entry_get(int fd)
{
    if (fd < 0 || fd >= MAX_FD) return NULL;
    fd_entry_t *e = &fd_tables[task_idx()][fd];
    return e->obj ? e : NULL;
}

/* Restituisce l'oggetto fd corrente, NULL se fuori range o free */
static void fd_entry_set_cloexec(fd_entry_t *e, int enabled)
{
    if (!e)
        return;
    if (enabled)
        e->fd_flags |= FD_ENTRY_CLOEXEC;
    else
        e->fd_flags &= (uint8_t)~FD_ENTRY_CLOEXEC;
}

static int fd_entry_cloexec(const fd_entry_t *e)
{
    return (e && (e->fd_flags & FD_ENTRY_CLOEXEC) != 0U) ? 1 : 0;
}

static fd_object_t *fd_get(int fd)
{
    fd_entry_t *e = fd_entry_get(fd);
    return e ? e->obj : NULL;
}

int syscall_describe_fd_current(int fd, char *out, size_t cap)
{
    fd_object_t *obj = fd_get(fd);

    if (!out || cap < 2U)
        return -EINVAL;
    out[0] = '\0';

    if (!obj)
        return -EBADF;

    if (obj->path[0] != '\0') {
        syscall_copy_fixed_string(out, cap, obj->path);
        return 0;
    }

    switch (obj->type) {
    case FD_TYPE_PIPE:
        syscall_copy_fixed_string(out, cap,
                                  obj->pipe_end == PIPE_END_READ ? "pipe:[read]" : "pipe:[write]");
        return 0;
    case FD_TYPE_SOCK:
        syscall_copy_fixed_string(out, cap, "socket:[inet]");
        return 0;
    case FD_TYPE_VFS:
    case FD_TYPE_VFSD:
        syscall_copy_fixed_string(out, cap, "anon:[file]");
        return 0;
    default:
        syscall_copy_fixed_string(out, cap, "anon:[fd]");
        return 0;
    }
}

static fd_object_t *fd_object_alloc(void)
{
    for (uint32_t i = 0U; i < FD_OBJECT_MAX; i++) {
        if (fd_objects[i].in_use)
            continue;

        memset(&fd_objects[i], 0, sizeof(fd_objects[i]));
        fd_objects[i].in_use = 1U;
        return &fd_objects[i];
    }
    return NULL;
}

static pipe_t *fd_pipe_alloc(void)
{
    for (uint32_t i = 0U; i < PIPE_POOL_MAX; i++) {
        if (pipe_pool[i].in_use)
            continue;

        memset(&pipe_pool[i], 0, sizeof(pipe_pool[i]));
        pipe_pool[i].in_use = 1U;
        return &pipe_pool[i];
    }
    return NULL;
}

/* Alloca il primo fd libero >= 3 (non tocca stdin/stdout/stderr) */
static int fd_alloc_in_slot(int idx)
{
    for (int i = 3; i < MAX_FD; i++) {
        if (fd_tables[idx][i].obj == NULL)
            return i;
    }
    return -1;
}

static int fd_alloc_from_in_slot(int idx, int start)
{
    if (idx < 0 || idx >= SCHED_MAX_TASKS)
        return -EINVAL;
    if (start < 0)
        start = 0;

    for (int i = start; i < MAX_FD; i++) {
        if (fd_tables[idx][i].obj == NULL)
            return i;
    }
    return -1;
}

static int fd_alloc(void)
{
    return fd_alloc_in_slot(task_idx());
}

static void fd_clear(fd_entry_t *e)
{
    if (e) {
        e->obj = NULL;
        e->fd_flags = 0U;
    }
}

static void fd_object_put(fd_object_t *obj)
{
    pipe_t *pipe;

    if (!obj || !obj->in_use || obj->refcount == 0U)
        return;

    obj->refcount--;
    if (obj->refcount != 0U)
        return;

    flock_release_object(obj);

    if (obj->type == FD_TYPE_VFS) {
        if (obj->file.mount)
            (void)vfs_close(&obj->file);
    } else if (obj->type == FD_TYPE_VFSD) {
        if (obj->remote_handle != 0U)
            (void)vfsd_proxy_close(obj->remote_handle);
        if (obj->file.mount)
            (void)vfs_close(&obj->file);
    } else if (obj->type == FD_TYPE_PIPE) {
        pipe = (pipe_t *)obj->pipe;
        if (pipe && pipe->in_use) {
            if (obj->pipe_end == PIPE_END_READ) {
                if (pipe->readers > 0U)
                    pipe->readers--;
            } else {
                if (pipe->writers > 0U)
                    pipe->writers--;
            }
            if (pipe->readers == 0U && pipe->writers == 0U)
                memset(pipe, 0, sizeof(*pipe));
        }
    } else if (obj->type == FD_TYPE_SOCK) {
        if (sock_handle_is_remote_netd(obj->remote_handle))
            (void)netd_proxy_close(sock_handle_from_remote_netd(obj->remote_handle));
        else
            sock_free((int)obj->remote_handle);
    }

    memset(obj, 0, sizeof(*obj));
}

static void fd_release_slot(fd_entry_t *e)
{
    if (!e || !e->obj)
        return;

    fd_object_put(e->obj);
    e->obj = NULL;
    e->fd_flags = 0U;
}

static int fd_attach_object(fd_entry_t *e, fd_object_t *obj)
{
    if (!e || !obj || !obj->in_use)
        return -EINVAL;

    obj->refcount++;
    e->obj = obj;
    return 0;
}

static void fd_release_slot_index(int idx)
{
    for (int fd = 0; fd < MAX_FD; fd++)
        fd_release_slot(&fd_tables[idx][fd]);
}

static int fd_bind_path_idx(int idx, int fd, const char *path, uint16_t flags)
{
    fd_object_t *obj;
    int          rc;

    if (idx < 0 || idx >= SCHED_MAX_TASKS || fd < 0 || fd >= MAX_FD)
        return -EINVAL;

    obj = fd_object_alloc();
    if (!obj)
        return -ENFILE;

    rc = vfs_open(path, flags, &obj->file);
    if (rc < 0) {
        memset(obj, 0, sizeof(*obj));
        return rc;
    }

    obj->type  = FD_TYPE_VFS;
    obj->flags = flags;
    syscall_copy_fixed_string(obj->path, sizeof(obj->path), path);
    return fd_attach_object(&fd_tables[idx][fd], obj);
}

static void fd_init_slot_defaults(int idx)
{
    if (idx < 0 || idx >= SCHED_MAX_TASKS)
        return;

    for (int j = 0; j < MAX_FD; j++)
        fd_clear(&fd_tables[idx][j]);

    (void)fd_bind_path_idx(idx, 0, "/dev/stdin",  O_RDONLY);
    (void)fd_bind_path_idx(idx, 1, "/dev/stdout", O_WRONLY);
    (void)fd_bind_path_idx(idx, 2, "/dev/stderr", O_WRONLY);
}

static void fd_reset_slot_defaults(int idx)
{
    fd_release_slot_index(idx);
    fd_init_slot_defaults(idx);
}

static void fd_clone_task_table(int dst_idx, int src_idx)
{
    if (dst_idx < 0 || dst_idx >= SCHED_MAX_TASKS ||
        src_idx < 0 || src_idx >= SCHED_MAX_TASKS)
        return;

    fd_release_slot_index(dst_idx);
    for (int i = 0; i < MAX_FD; i++) {
        fd_object_t *obj = fd_tables[src_idx][i].obj;

        fd_tables[dst_idx][i].obj = NULL;
        fd_tables[dst_idx][i].fd_flags = 0U;
        if (obj)
            (void)fd_attach_object(&fd_tables[dst_idx][i], obj);
        fd_tables[dst_idx][i].fd_flags = fd_tables[src_idx][i].fd_flags;
    }
}

static int fd_bind_path(fd_entry_t *e, const char *path, uint16_t flags)
{
    fd_object_t *obj;
    int          rc;

    if (!e)
        return -EINVAL;

    obj = fd_object_alloc();
    if (!obj)
        return -ENFILE;

    rc = vfs_open(path, flags, &obj->file);
    if (rc < 0) {
        memset(obj, 0, sizeof(*obj));
        return rc;
    }

    obj->type  = FD_TYPE_VFS;
    obj->flags = flags;
    syscall_copy_fixed_string(obj->path, sizeof(obj->path), path);
    return fd_attach_object(e, obj);
}

static int fd_bind_remote(fd_entry_t *e, uint32_t handle, uint16_t flags)
{
    fd_object_t *obj;

    if (!e || handle == 0U)
        return -EINVAL;

    obj = fd_object_alloc();
    if (!obj)
        return -ENFILE;

    obj->type = FD_TYPE_VFSD;
    obj->flags = flags;
    obj->remote_handle = handle;
    obj->path[0] = '\0';
    return fd_attach_object(e, obj);
}

static int fd_bind_remote_shadow(fd_entry_t *e, const char *path, uint16_t flags)
{
    uint16_t shadow_flags;
    fd_object_t *obj;
    int rc;

    if (!e || !path)
        return -EINVAL;
    if (!e->obj)
        return -EBADF;

    /*
     * Il file remoto e' gia' stato aperto/creato/troncato tramite vfsd.
     * Per il solo shadow locale usato da mmap/msync evitiamo side effect
     * duplicati come un secondo O_TRUNC/O_CREAT/O_APPEND.
     */
    obj = e->obj;
    shadow_flags = (uint16_t)(flags & (uint16_t)~(O_TRUNC | O_CREAT | O_APPEND));
    rc = vfs_open(path, shadow_flags, &obj->file);
    if (rc < 0)
        return rc;
    syscall_copy_fixed_string(obj->path, sizeof(obj->path), path);
    return 0;
}

static int fd_is_tty_object(const fd_object_t *obj)
{
    uint32_t node_id;

    if (!obj)
        return 0;
    if ((obj->type != FD_TYPE_VFS && obj->type != FD_TYPE_VFSD) || !obj->file.mount)
        return 0;
    if (!obj->file.mount->path || !syscall_streq(obj->file.mount->path, "/dev"))
        return 0;

    node_id = obj->file.node_id;
    return node_id == DEV_NODE_CONSOLE || node_id == DEV_NODE_TTY ||
           node_id == DEV_NODE_STDIN || node_id == DEV_NODE_STDOUT ||
           node_id == DEV_NODE_STDERR;
}

static ssize_t fd_pipe_read(fd_object_t *obj, void *buf, size_t count)
{
    pipe_t   *pipe;
    uint8_t  *dst = (uint8_t *)buf;
    size_t    got = 0U;

    if (!obj || obj->type != FD_TYPE_PIPE || obj->pipe_end != PIPE_END_READ || !buf)
        return -EBADF;

    pipe = (pipe_t *)obj->pipe;
    if (!pipe || !pipe->in_use)
        return -EBADF;

    while (got < count) {
        while (pipe->size == 0U) {
            if (pipe->writers == 0U)
                return (ssize_t)got;
            if (obj->flags & O_NONBLOCK)
                return got ? (ssize_t)got : -EAGAIN;
            if (signal_has_unblocked_pending(current_task))
                return got ? (ssize_t)got : -EINTR;
            sched_yield();
        }

        dst[got++] = pipe->buf[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1U) % PIPE_BUF_SIZE;
        pipe->size--;

        /*
         * Per una pipe/FIFO restituiamo subito i byte disponibili invece
         * di attendere di riempire tutto il buffer richiesto.
         */
        if (pipe->size == 0U)
            break;

        if (obj->flags & O_NONBLOCK)
            break;
    }

    return (ssize_t)got;
}

static ssize_t fd_pipe_write(fd_object_t *obj, const void *buf, size_t count)
{
    pipe_t        *pipe;
    const uint8_t *src = (const uint8_t *)buf;
    size_t         put = 0U;

    if (!obj || obj->type != FD_TYPE_PIPE || obj->pipe_end != PIPE_END_WRITE || !buf)
        return -EBADF;

    pipe = (pipe_t *)obj->pipe;
    if (!pipe || !pipe->in_use)
        return -EBADF;

    while (put < count) {
        if (pipe->readers == 0U)
            return put ? (ssize_t)put : -EPIPE;

        while (pipe->size >= PIPE_BUF_SIZE) {
            if (pipe->readers == 0U)
                return put ? (ssize_t)put : -EPIPE;
            if (obj->flags & O_NONBLOCK)
                return put ? (ssize_t)put : -EAGAIN;
            if (signal_has_unblocked_pending(current_task))
                return put ? (ssize_t)put : -EINTR;
            sched_yield();
        }

        pipe->buf[pipe->write_pos] = src[put++];
        pipe->write_pos = (pipe->write_pos + 1U) % PIPE_BUF_SIZE;
        pipe->size++;

        if (obj->flags & O_NONBLOCK)
            break;
    }

    return (ssize_t)put;
}

void syscall_task_cleanup(sched_tcb_t *task)
{
    int idx;

    if (!task)
        return;

    idx = (int)(sched_task_proc_slot(task) % SCHED_MAX_TASKS);
    fd_reset_slot_defaults(idx);
    task_brk[idx] = 0ULL;
}

static int vfs_srv_task_slot(void)
{
    if (!current_task)
        return -1;
    return (int)(sched_task_proc_slot(current_task) % SCHED_MAX_TASKS);
}

static int vfs_srv_owner_ok(void)
{
    port_t *port;

    if (!current_task || !sched_task_is_user(current_task))
        return 0;

    port = mk_port_lookup("vfs");
    return (port && port->owner_tid == sched_task_tgid(current_task)) ? 1 : 0;
}

static vfs_srv_handle_t *vfs_srv_handle_get(uint32_t handle)
{
    int slot;

    if (handle == 0U || handle > VFSD_HANDLE_MAX)
        return NULL;

    slot = vfs_srv_task_slot();
    if (slot < 0)
        return NULL;

    return vfs_srv_tables[slot][handle - 1U].in_use ?
           &vfs_srv_tables[slot][handle - 1U] : NULL;
}

static uint32_t vfs_srv_handle_alloc(const vfs_file_t *file)
{
    int slot;

    if (!file)
        return 0U;

    slot = vfs_srv_task_slot();
    if (slot < 0)
        return 0U;

    for (uint32_t i = 0U; i < VFSD_HANDLE_MAX; i++) {
        vfs_srv_handle_t *h = &vfs_srv_tables[slot][i];

        if (h->in_use)
            continue;

        memset(h, 0, sizeof(*h));
        h->in_use = 1U;
        h->file = *file;
        return i + 1U;
    }

    return 0U;
}

static void vfs_srv_handle_free(uint32_t handle)
{
    vfs_srv_handle_t *h = vfs_srv_handle_get(handle);

    if (!h)
        return;

    memset(h, 0, sizeof(*h));
}

static int vfsd_proxy_available(void)
{
    port_t *port;

    if (!current_task || !sched_task_is_user(current_task))
        return 0;

    port = mk_port_lookup("vfs");
    if (!port || port->owner_tid == 0U)
        return 0;
    if (port->owner_tid == sched_task_tgid(current_task))
        return 0;
    return 1;
}

static int vfsd_proxy_call(const vfsd_request_t *req, vfsd_response_t *resp)
{
    port_t        *port;
    ipc_message_t  reply;
    uint64_t       deadline_ms;
    int            rc;

    if (!req || !resp)
        return -EFAULT;

    port = mk_port_lookup("vfs");
    if (!port || port->owner_tid == 0U)
        return -ENOENT;

    deadline_ms = timer_now_ms() + 1000ULL;
    do {
        rc = mk_ipc_call(port->port_id, IPC_MSG_VFS_REQ, req, sizeof(*req), &reply);
        if (rc != -EBUSY && rc != -EAGAIN)
            break;
        sched_yield();
    } while (timer_now_ms() < deadline_ms);

    if (rc < 0)
        return rc;
    if (reply.msg_type != IPC_MSG_VFS_RESP || reply.msg_len < sizeof(*resp))
        return -EIO;

    memcpy(resp, reply.payload, sizeof(*resp));
    return 0;
}

static void vfsd_req_set_path(char dst[VFSD_PATH_BYTES], const char *src)
{
    size_t i = 0U;

    if (!dst)
        return;
    memset(dst, 0, VFSD_PATH_BYTES);
    if (!src)
        return;

    while (src[i] != '\0' && i + 1U < VFSD_PATH_BYTES) {
        dst[i] = src[i];
        i++;
    }
}

static int vfsd_resp_get_path(const vfsd_response_t *resp, char *out, size_t cap)
{
    size_t i;

    if (!resp || !out || cap == 0U)
        return -EFAULT;
    if (resp->data_len == 0U || resp->data_len > VFSD_IO_BYTES)
        return -EIO;

    for (i = 0U; i + 1U < cap && i < (size_t)resp->data_len; i++) {
        out[i] = (char)resp->u.data[i];
        if (out[i] == '\0')
            return 0;
    }

    if (i == cap)
        i = cap - 1U;
    out[i] = '\0';
    return -ENAMETOOLONG;
}

static int vfsd_proxy_open(const char *path, uint32_t flags, uint32_t *handle_out)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!path || !handle_out)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_OPEN;
    req.flags = flags;
    vfsd_req_set_path(req.u.paths.path, path);

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    if (resp.handle <= 0)
        return -EIO;

    *handle_out = (uint32_t)resp.handle;
    return 0;
}

static ssize_t vfsd_proxy_read(uint32_t handle, void *dst, size_t count)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!dst)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_READ;
    req.handle = (int32_t)handle;
    req.count = (count > VFSD_IO_BYTES) ? VFSD_IO_BYTES : (uint32_t)count;

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    if (resp.data_len > VFSD_IO_BYTES)
        return -EIO;

    memcpy(dst, resp.u.data, resp.data_len);
    return (ssize_t)resp.data_len;
}

static ssize_t vfsd_proxy_write(uint32_t handle, const void *src, size_t count)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!src)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_WRITE;
    req.handle = (int32_t)handle;
    req.count = (count > VFSD_IO_BYTES) ? VFSD_IO_BYTES : (uint32_t)count;
    memcpy(req.u.data, src, req.count);

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    return (ssize_t)resp.data_len;
}

static int vfsd_proxy_resolve_meta(const char *path, char *out, size_t cap,
                                   uint32_t *flags_out)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!path || !out || cap == 0U)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_RESOLVE;
    vfsd_req_set_path(req.u.paths.path, path);

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    if (flags_out)
        *flags_out = (uint32_t)((resp.handle >= 0) ? resp.handle : 0);
    return vfsd_resp_get_path(&resp, out, cap);
}

static int vfsd_proxy_resolve(const char *path, char *out, size_t cap)
{
    return vfsd_proxy_resolve_meta(path, out, cap, NULL);
}

static int vfsd_proxy_chdir(const char *path)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!path)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_CHDIR;
    vfsd_req_set_path(req.u.paths.path, path);

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int vfsd_proxy_getcwd(char *out, size_t cap)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!out || cap == 0U)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_GETCWD;
    req.count = (cap > VFSD_IO_BYTES) ? VFSD_IO_BYTES : (uint32_t)cap;

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    return vfsd_resp_get_path(&resp, out, cap);
}

static int vfsd_proxy_mount(const char *src, const char *dst,
                            uint32_t fs_type, uint32_t flags)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!dst)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_MOUNT;
    req.flags = flags;
    req.arg0 = fs_type;
    vfsd_req_set_path(req.u.paths.path, src);
    vfsd_req_set_path(req.u.paths.aux, dst);

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int vfsd_proxy_umount(const char *path)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!path)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_UMOUNT;
    vfsd_req_set_path(req.u.paths.path, path);

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int vfsd_proxy_unshare(uint32_t flags)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_UNSHARE;
    req.flags = flags;

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int vfsd_proxy_pivot_root(const char *new_root, const char *old_root)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!new_root || !old_root)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_PIVOT_ROOT;
    vfsd_req_set_path(req.u.paths.path, new_root);
    vfsd_req_set_path(req.u.paths.aux, old_root);

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int vfsd_proxy_readdir(uint32_t handle, vfs_dirent_t *out)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!out)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_READDIR;
    req.handle = (int32_t)handle;

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;

    *out = resp.u.dirent;
    return 0;
}

static int vfsd_proxy_stat(uint32_t handle, stat_t *out)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    if (!out)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_STAT;
    req.handle = (int32_t)handle;

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;

    *out = resp.u.st;
    return 0;
}

static int vfsd_proxy_close(uint32_t handle)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_CLOSE;
    req.handle = (int32_t)handle;

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int64_t vfsd_proxy_lseek(uint32_t handle, int64_t offset, int whence)
{
    vfsd_request_t  req;
    vfsd_response_t resp;
    int             rc;
    int64_t         result = 0LL;

    memset(&req, 0, sizeof(req));
    req.op = VFSD_REQ_LSEEK;
    req.handle = (int32_t)handle;
    req.arg0 = (uint32_t)whence;
    memcpy(req.u.data, &offset, sizeof(offset));

    rc = vfsd_proxy_call(&req, &resp);
    if (rc < 0)
        return (int64_t)rc;
    if (resp.status < 0)
        return (int64_t)resp.status;
    if (resp.data_len != sizeof(result))
        return -EIO;

    memcpy(&result, resp.u.data, sizeof(result));
    return result;
}

static int netd_proxy_available(void)
{
    port_t *port;

    if (!current_task || !sched_task_is_user(current_task))
        return 0;

    port = mk_port_lookup("net");
    if (!port || port->owner_tid == 0U)
        return 0;
    if (port->owner_tid == sched_task_tgid(current_task))
        return 0;
    return 1;
}

static int netd_proxy_call(const netd_request_t *req, netd_response_t *resp)
{
    port_t        *port;
    ipc_message_t  reply;
    uint64_t       deadline_ms;
    int            rc;

    if (!req || !resp)
        return -EFAULT;

    port = mk_port_lookup("net");
    if (!port || port->owner_tid == 0U)
        return -ENOENT;

    deadline_ms = timer_now_ms() + 1000ULL;
    do {
        rc = mk_ipc_call(port->port_id, IPC_MSG_NET_REQ, req, sizeof(*req), &reply);
        if (rc != -EBUSY && rc != -EAGAIN)
            break;
        sched_yield();
    } while (timer_now_ms() < deadline_ms);

    if (rc < 0)
        return rc;
    if (reply.msg_type != IPC_MSG_NET_RESP || reply.msg_len < sizeof(*resp))
        return -EIO;

    memcpy(resp, reply.payload, sizeof(*resp));
    return 0;
}

static int netd_proxy_socket(int domain, int type, int protocol, uint32_t *handle_out)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    if (!handle_out)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op   = NETD_REQ_SOCKET;
    req.arg0 = (uint32_t)domain;
    req.arg1 = (uint32_t)type;
    req.arg2 = (uint32_t)protocol;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    if (resp.handle <= 0)
        return -EIO;

    *handle_out = (uint32_t)resp.handle;
    return 0;
}

static int netd_proxy_bind(uint32_t handle, uint32_t ip, uint16_t port)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_BIND;
    req.handle = (int32_t)handle;
    req.arg0   = ip;
    req.arg1   = (uint32_t)port;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int netd_proxy_connect(uint32_t handle, uint32_t ip, uint16_t port)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_CONNECT;
    req.handle = (int32_t)handle;
    req.arg0   = ip;
    req.arg1   = (uint32_t)port;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static ssize_t netd_proxy_send(uint32_t handle, const void *src, size_t count)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    if (!src)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_SEND;
    req.handle = (int32_t)handle;
    req.count  = (count > NETD_IO_BYTES) ? NETD_IO_BYTES : (uint32_t)count;
    memcpy(req.data, src, req.count);

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    return (ssize_t)resp.data_len;
}

static ssize_t netd_proxy_recv(uint32_t handle, void *dst, size_t count)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    if (!dst)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_RECV;
    req.handle = (int32_t)handle;
    req.count  = (count > NETD_IO_BYTES) ? NETD_IO_BYTES : (uint32_t)count;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    if (resp.data_len > NETD_IO_BYTES)
        return -EIO;

    memcpy(dst, resp.data, resp.data_len);
    return (ssize_t)resp.data_len;
}

static int netd_proxy_poll(uint32_t handle, uint32_t *mask_out)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    if (!mask_out)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_POLL;
    req.handle = (int32_t)handle;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;
    *mask_out = resp.flags;
    return 0;
}

static int netd_proxy_getsockopt(uint32_t handle, int level, int optname,
                                 void *dst, size_t *len_io)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;
    size_t          len;

    if (!dst || !len_io)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_GETSOCKOPT;
    req.handle = (int32_t)handle;
    req.arg0   = (uint32_t)level;
    req.arg1   = (uint32_t)optname;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;

    len = *len_io;
    if (len > resp.data_len)
        len = resp.data_len;
    memcpy(dst, resp.data, len);
    *len_io = len;
    return 0;
}

static int netd_proxy_setsockopt(uint32_t handle, int level, int optname,
                                 const void *src, size_t len)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_SETSOCKOPT;
    req.handle = (int32_t)handle;
    req.arg0   = (uint32_t)level;
    req.arg1   = (uint32_t)optname;
    req.count  = (len > NETD_IO_BYTES) ? NETD_IO_BYTES : (uint32_t)len;
    if (src && req.count > 0U)
        memcpy(req.data, src, req.count);

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int netd_proxy_addr(uint32_t handle, int peer, uint32_t *ip_out, uint16_t *port_out)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    if (!ip_out || !port_out)
        return -EFAULT;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_ADDR;
    req.handle = (int32_t)handle;
    req.arg0   = (uint32_t)(peer ? 1 : 0);

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    if (resp.status < 0)
        return resp.status;

    *ip_out = resp.addr_ip;
    *port_out = resp.addr_port;
    return 0;
}

static int netd_proxy_shutdown(uint32_t handle, int how)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_SHUTDOWN;
    req.handle = (int32_t)handle;
    req.arg0   = (uint32_t)how;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static int netd_proxy_close(uint32_t handle)
{
    netd_request_t  req;
    netd_response_t resp;
    int             rc;

    memset(&req, 0, sizeof(req));
    req.op     = NETD_REQ_CLOSE;
    req.handle = (int32_t)handle;

    rc = netd_proxy_call(&req, &resp);
    if (rc < 0)
        return rc;
    return resp.status;
}

static void fd_split_open_flags(uint16_t in_flags, uint16_t *file_flags,
                                uint8_t *fd_flags)
{
    if (file_flags)
        *file_flags = (uint16_t)(in_flags & (uint16_t)~O_CLOEXEC);
    if (fd_flags)
        *fd_flags = ((in_flags & O_CLOEXEC) != 0U) ? FD_ENTRY_CLOEXEC : 0U;
}

static int fd_open_path_current(const char *path, uint16_t file_flags,
                                uint8_t fd_flags)
{
    char     resolved_buf[VFSD_IO_BYTES];
    uint32_t remote_handle = 0U;
    int      idx = task_idx();
    int      fd;
    int      rc;

    if (!path || path[0] == '\0')
        return -ENOENT;

    fd = fd_alloc();
    if (fd < 0)
        return -ENFILE;

    if (vfsd_proxy_available()) {
        rc = vfsd_proxy_open(path, file_flags, &remote_handle);
        if (rc < 0)
            return rc;

        rc = fd_bind_remote(&fd_tables[idx][fd], remote_handle, file_flags);
        if (rc >= 0) {
            rc = vfsd_proxy_resolve(path, resolved_buf, sizeof(resolved_buf));
            if (rc >= 0)
                rc = fd_bind_remote_shadow(&fd_tables[idx][fd], resolved_buf, file_flags);
            if (rc < 0) {
                (void)vfsd_proxy_close(remote_handle);
                fd_release_slot(&fd_tables[idx][fd]);
            }
        }
    } else {
        rc = fd_bind_path(&fd_tables[idx][fd], path, file_flags);
    }

    if (rc < 0)
        return rc;

    fd_tables[idx][fd].fd_flags = fd_flags;
    return fd;
}

static void fd_close_cloexec_in_slot(int idx)
{
    if (idx < 0 || idx >= SCHED_MAX_TASKS)
        return;

    for (int fd = 0; fd < MAX_FD; fd++) {
        if (!fd_entry_cloexec(&fd_tables[idx][fd]))
            continue;
        fd_release_slot(&fd_tables[idx][fd]);
    }
}

static int vfs_file_do_seek(vfs_file_t *file, int64_t offset,
                            int whence, uint64_t *new_pos_out)
{
    uint64_t base = 0ULL;
    uint64_t new_pos;
    stat_t   st;
    int      rc;

    if (!file || !new_pos_out)
        return -EINVAL;

    switch (whence) {
    case SEEK_SET:
        base = 0ULL;
        break;
    case SEEK_CUR:
        base = file->pos;
        break;
    case SEEK_END:
        rc = vfs_stat(file, &st);
        if (rc < 0)
            return rc;
        base = st.st_size;
        break;
    default:
        return -EINVAL;
    }

    if (offset < 0 && (uint64_t)(-offset) > base)
        return -EINVAL;

    new_pos = (offset < 0) ? (base - (uint64_t)(-offset))
                           : (base + (uint64_t)offset);
    file->pos = new_pos;
    *new_pos_out = new_pos;
    return 0;
}

static int fd_object_seek(fd_object_t *obj, int64_t offset,
                          int whence, uint64_t *new_pos_out)
{
    stat_t   st;
    uint64_t base = 0ULL;
    uint64_t new_pos;
    int64_t  remote_pos;
    int      rc;

    if (!obj || !new_pos_out)
        return -EINVAL;
    if (obj->type == FD_TYPE_PIPE)
        return -ESPIPE;
    if (obj->type != FD_TYPE_VFS && obj->type != FD_TYPE_VFSD)
        return -EBADF;

    switch (whence) {
    case SEEK_SET:
        base = 0ULL;
        break;
    case SEEK_CUR:
        base = obj->file.pos;
        break;
    case SEEK_END:
        if (obj->file.mount) {
            rc = vfs_stat(&obj->file, &st);
            if (rc < 0)
                return rc;
            base = st.st_size;
        } else if (obj->type == FD_TYPE_VFSD) {
            rc = vfsd_proxy_stat(obj->remote_handle, &st);
            if (rc < 0)
                return rc;
            base = st.st_size;
        } else {
            return -EINVAL;
        }
        break;
    default:
        return -EINVAL;
    }

    if (offset < 0 && (uint64_t)(-offset) > base)
        return -EINVAL;
    new_pos = (offset < 0) ? (base - (uint64_t)(-offset))
                           : (base + (uint64_t)offset);

    if (obj->type == FD_TYPE_VFSD) {
        remote_pos = vfsd_proxy_lseek(obj->remote_handle, (int64_t)new_pos, SEEK_SET);
        if (remote_pos < 0)
            return (int)remote_pos;
        obj->file.pos = (uint64_t)remote_pos;
        *new_pos_out = (uint64_t)remote_pos;
        return 0;
    }

    obj->file.pos = new_pos;
    *new_pos_out = new_pos;
    return 0;
}

static void syscall_copy_fixed_string(char *dst, size_t cap, const char *src)
{
    size_t i = 0U;

    if (!dst || cap == 0U)
        return;

    while (src && src[i] != '\0' && i + 1U < cap) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static int user_copy_bytes(uintptr_t uva, void *dst, size_t size)
{
    mm_space_t *space;

    if (size == 0U) return 0;
    if (!current_task || !sched_task_is_user(current_task))
        return -EFAULT;

    space = sched_task_space(current_task);
    return mmu_read_user(space, uva, dst, size);
}

static int user_copy_cstr(uintptr_t uva, char *dst, size_t cap)
{
    mm_space_t *space;
    size_t      done = 0U;

    if (!uva || !dst || cap == 0U)
        return -EFAULT;
    if (!current_task || !sched_task_is_user(current_task))
        return -EFAULT;

    space = sched_task_space(current_task);

    /* Copia un chunk per pagina, fermandosi al primo NUL. */
    while (done < cap) {
        uintptr_t cur      = uva + done;
        size_t    page_off = (size_t)(cur & (PAGE_SIZE - 1ULL));
        size_t    chunk    = PAGE_SIZE - page_off;
        size_t    remain   = cap - done;
        char     *p;
        size_t    i;

        if (chunk > remain)
            chunk = remain;

        if (mmu_read_user(space, cur, dst + done, chunk) < 0)
            return -EFAULT;

        /* Scansiona il chunk appena copiato per NUL. */
        p = dst + done;
        for (i = 0U; i < chunk; i++) {
            if (p[i] == '\0')
                return 0;
        }
        done += chunk;
    }

    dst[cap - 1U] = '\0';
    return -ENAMETOOLONG;
}

static int user_store_bytes(uintptr_t uva, const void *src, size_t size)
{
    mm_space_t *space;

    if (size == 0U) return 0;
    if (!current_task || !sched_task_is_user(current_task))
        return -EFAULT;

    space = sched_task_space(current_task);
    return mmu_write_user(space, uva, src, size);
}

static int user_probe_write(uintptr_t uva, size_t size)
{
    mm_space_t *space;
    size_t      checked = 0U;

    if (size == 0U)
        return 0;
    if (!current_task || !sched_task_is_user(current_task))
        return -EFAULT;

    space = sched_task_space(current_task);
    while (checked < size) {
        uintptr_t cur = uva + checked;
        size_t    page_off = (size_t)(cur & (PAGE_SIZE - 1ULL));
        size_t    chunk = PAGE_SIZE - page_off;

        if (chunk > size - checked)
            chunk = size - checked;
        if (mmu_space_prepare_write(space, cur, chunk) < 0)
            return -EFAULT;
        if (!mmu_space_resolve_ptr(space, cur, chunk))
            return -EFAULT;
        checked += chunk;
    }

    return 0;
}

static int resolve_user_vfs_path_meta(const char *input, char *out, size_t cap,
                                      uint32_t *flags_out)
{
    size_t i = 0U;

    if (!input || !out || cap < 2U)
        return -EFAULT;

    if (current_task && sched_task_is_user(current_task) && vfsd_proxy_available())
        return vfsd_proxy_resolve_meta(input, out, cap, flags_out);

    while (input[i] != '\0' && i + 1U < cap) {
        out[i] = input[i];
        i++;
    }
    if (input[i] != '\0')
        return -ENAMETOOLONG;
    out[i] = '\0';
    if (flags_out)
        *flags_out = vfs_path_is_linux_compat(out) ?
                     VFSD_RESP_FLAG_LINUX_COMPAT : 0U;
    return 0;
}

static int resolve_user_vfs_path(const char *input, char *out, size_t cap)
{
    return resolve_user_vfs_path_meta(input, out, cap, NULL);
}

static int sys_mount_parse_fs(const char *fstype, uint32_t flags, uint32_t *fs_out)
{
    if (!fstype || !fs_out)
        return -EINVAL;

    if (flags & MS_BIND) {
        *fs_out = VFSD_FS_BIND;
        return 0;
    }
    if (fstype[0] == 'b' && fstype[1] == 'i' && fstype[2] == 'n' &&
        fstype[3] == 'd' && fstype[4] == '\0') {
        *fs_out = VFSD_FS_BIND;
        return 0;
    }
    if (fstype[0] == 'i' && fstype[1] == 'n' && fstype[2] == 'i' &&
        fstype[3] == 't' && fstype[4] == 'r' && fstype[5] == 'd' &&
        fstype[6] == '\0') {
        *fs_out = VFSD_FS_INITRD;
        return 0;
    }
    if (fstype[0] == 'd' && fstype[1] == 'e' && fstype[2] == 'v' &&
        fstype[3] == 'f' && fstype[4] == 's' && fstype[5] == '\0') {
        *fs_out = VFSD_FS_DEVFS;
        return 0;
    }
    if (fstype[0] == 'p' && fstype[1] == 'r' && fstype[2] == 'o' &&
        fstype[3] == 'c' && fstype[4] == 'f' && fstype[5] == 's' &&
        fstype[6] == '\0') {
        *fs_out = VFSD_FS_PROCFS;
        return 0;
    }
    if (fstype[0] == 'e' && fstype[1] == 'x' && fstype[2] == 't' &&
        fstype[3] == '4' && fstype[4] == '\0') {
        *fs_out = VFSD_FS_EXT4_DATA;
        return 0;
    }
    if (fstype[0] == 'e' && fstype[1] == 'x' && fstype[2] == 't' &&
        fstype[3] == '4' && fstype[4] == '-' && fstype[5] == 's' &&
        fstype[6] == 'y' && fstype[7] == 's' && fstype[8] == 'r' &&
        fstype[9] == 'o' && fstype[10] == 'o' && fstype[11] == 't' &&
        fstype[12] == '\0') {
        *fs_out = VFSD_FS_EXT4_SYSROOT;
        return 0;
    }

    return -EINVAL;
}

static uint32_t sys_mmap_prot_to_mmu(uint32_t prot)
{
    uint32_t mmu_prot = MMU_PROT_USER_R;

    if (prot & PROT_WRITE)
        mmu_prot |= MMU_PROT_USER_W;
    if (prot & PROT_EXEC)
        mmu_prot |= MMU_PROT_USER_X;
    return mmu_prot;
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
static syscall_entry_t linux_syscall_table[LINUX_SYSCALL_MAX];
static uint64_t        linux_rand_state = 0x9E3779B97F4A7C15ULL;

#define LINUX_O_ACCMODE      00000003U
#define LINUX_O_CREAT        00000100U
#define LINUX_O_TRUNC        00001000U
#define LINUX_O_APPEND       00002000U
#define LINUX_O_NONBLOCK     00004000U
#define LINUX_O_CLOEXEC      02000000U

#define LINUX_F_OK           0U
#define LINUX_X_OK           1U
#define LINUX_W_OK           2U
#define LINUX_R_OK           4U

#define LINUX_RUSAGE_SELF    0
#define LINUX_RUSAGE_CHILDREN (-1)

#define LINUX_DT_UNKNOWN     0U
#define LINUX_DT_FIFO        1U
#define LINUX_DT_CHR         2U
#define LINUX_DT_DIR         4U
#define LINUX_DT_REG         8U
#define LINUX_DT_LNK         10U

static int syscall_path_join_abs(const char *base, const char *rel,
                                 char *out, size_t cap)
{
    size_t base_len = 0U;
    size_t rel_off = 0U;
    size_t pos = 0U;

    if (!base || !rel || !out || cap < 2U)
        return -EINVAL;

    while (base[base_len] != '\0')
        base_len++;
    while (rel[rel_off] == '/')
        rel_off++;

    if (base_len == 0U || base[0] != '/')
        return -EINVAL;
    if (base_len + 2U > cap)
        return -ENAMETOOLONG;

    syscall_copy_fixed_string(out, cap, base);
    pos = 0U;
    while (out[pos] != '\0')
        pos++;

    if (rel[rel_off] == '\0')
        return 0;

    if (!(pos == 1U && out[0] == '/')) {
        if (out[pos - 1U] != '/') {
            if (pos + 1U >= cap)
                return -ENAMETOOLONG;
            out[pos++] = '/';
        }
    }

    while (rel[rel_off] != '\0' && pos + 1U < cap)
        out[pos++] = rel[rel_off++];
    if (rel[rel_off] != '\0')
        return -ENAMETOOLONG;
    out[pos] = '\0';
    return 0;
}

static int resolve_dirfd_path_meta(int dirfd, const char *path,
                                   int allow_empty,
                                   char *resolved, size_t cap,
                                   uint32_t *flags_out)
{
    fd_object_t *dir_obj;
    char         joined[VFSD_IO_BYTES];
    stat_t       st;
    int          rc;

    if (!path || !resolved || cap < 2U)
        return -EFAULT;

    if (path[0] == '/' || dirfd == AT_FDCWD)
        return resolve_user_vfs_path_meta(path, resolved, cap, flags_out);

    dir_obj = fd_get(dirfd);
    if (!dir_obj)
        return -EBADF;
    if (dir_obj->type != FD_TYPE_VFS && dir_obj->type != FD_TYPE_VFSD)
        return -EBADF;
    if (dir_obj->path[0] == '\0')
        return -ENOSYS;

    if (allow_empty && path[0] == '\0')
        return resolve_user_vfs_path_meta(dir_obj->path, resolved, cap, flags_out);

    rc = (dir_obj->type == FD_TYPE_VFSD)
             ? vfsd_proxy_stat(dir_obj->remote_handle, &st)
             : vfs_stat(&dir_obj->file, &st);
    if (rc < 0)
        return rc;
    if ((st.st_mode & S_IFMT) != S_IFDIR)
        return -ENOTDIR;

    rc = syscall_path_join_abs(dir_obj->path, path, joined, sizeof(joined));
    if (rc < 0)
        return rc;
    return resolve_user_vfs_path_meta(joined, resolved, cap, flags_out);
}

static uint32_t linux_open_flags_to_native(uint32_t flags)
{
    uint32_t native = 0U;

    switch (flags & LINUX_O_ACCMODE) {
    case 0U: native |= O_RDONLY; break;
    case 1U: native |= O_WRONLY; break;
    case 2U: native |= O_RDWR;   break;
    default: native |= O_RDONLY; break;
    }
    if (flags & LINUX_O_CREAT)    native |= O_CREAT;
    if (flags & LINUX_O_TRUNC)    native |= O_TRUNC;
    if (flags & LINUX_O_APPEND)   native |= O_APPEND;
    if (flags & LINUX_O_NONBLOCK) native |= O_NONBLOCK;
    if (flags & LINUX_O_CLOEXEC)  native |= O_CLOEXEC;
    return native;
}

static void linux_stat_from_native(const stat_t *src, linux_stat_t *dst)
{
    if (!src || !dst)
        return;
    memset(dst, 0, sizeof(*dst));
    dst->st_mode = src->st_mode;
    dst->st_nlink = ((src->st_mode & S_IFMT) == S_IFDIR) ? 2U : 1U;
    dst->st_uid = 0U;
    dst->st_gid = 0U;
    dst->st_size = (int64_t)src->st_size;
    dst->st_blksize = (int32_t)src->st_blksize;
    dst->st_blocks = (int64_t)src->st_blocks;
}

static uint8_t linux_dtype_from_mode(uint32_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFIFO: return LINUX_DT_FIFO;
    case S_IFCHR: return LINUX_DT_CHR;
    case S_IFDIR: return LINUX_DT_DIR;
    case S_IFREG: return LINUX_DT_REG;
    case S_IFLNK: return LINUX_DT_LNK;
    default:      return LINUX_DT_UNKNOWN;
    }
}

static uint64_t linux_random_next(void)
{
    uint64_t x = linux_rand_state;

    x ^= timer_now_ns();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    linux_rand_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static void linux_random_fill(void *dst, size_t len)
{
    uint8_t *out = (uint8_t *)dst;
    size_t   off = 0U;

    while (off < len) {
        uint64_t word = linux_random_next();
        size_t   chunk = len - off;

        if (chunk > sizeof(word))
            chunk = sizeof(word);
        memcpy(out + off, &word, chunk);
        off += chunk;
    }
}

static void linux_fill_rusage(linux_rusage_t *ru)
{
    if (!ru)
        return;
    memset(ru, 0, sizeof(*ru));
}

static void linux_fill_sysinfo(linux_sysinfo_t *info)
{
    if (!info)
        return;
    memset(info, 0, sizeof(*info));
    info->uptime   = (int64_t)(timer_now_ms() / 1000ULL);
    info->totalram = 512ULL * 1024ULL * 1024ULL;
    info->freeram  = 384ULL * 1024ULL * 1024ULL;
    info->mem_unit = 1U;
    info->procs    = (uint16_t)sched_task_count_total();
}

static int linux_path_readlink(const char *path, char *out, size_t cap)
{
    uint32_t fd = 0U;
    fd_object_t *obj;

    if (!path || !out || cap < 2U)
        return -EINVAL;
    if (syscall_streq(path, "/bin/sh")) {
        syscall_copy_fixed_string(out, cap, "/usr/bin/bash");
        return 0;
    }
    if (syscall_streq(path, "/dev/random")) {
        syscall_copy_fixed_string(out, cap, "/dev/urandom");
        return 0;
    }
    if (syscall_streq(path, "/proc/self/exe")) {
        const char *exe = sched_task_exec_path(current_task);
        if (!exe)
            return -ENOENT;
        syscall_copy_fixed_string(out, cap, exe);
        return 0;
    }
    if (path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' &&
        path[4] == 'c' && path[5] == '/' && path[6] == 's' && path[7] == 'e' &&
        path[8] == 'l' && path[9] == 'f' && path[10] == '/' && path[11] == 'f' &&
        path[12] == 'd' && path[13] == '/') {
        const char *p = &path[14];

        if (*p == '\0')
            return -ENOENT;
        while (*p >= '0' && *p <= '9') {
            fd = fd * 10U + (uint32_t)(*p - '0');
            p++;
        }
        if (*p != '\0')
            return -ENOENT;
        obj = fd_get((int)fd);
        if (!obj || obj->path[0] == '\0')
            return -ENOENT;
        syscall_copy_fixed_string(out, cap, obj->path);
        return 0;
    }
    return -EINVAL;
}

static int linux_parent_dir(const char *path, char *out, size_t cap)
{
    size_t len = 0U;
    size_t last_slash = 0U;

    if (!path || !out || cap < 2U || path[0] != '/')
        return -EINVAL;

    while (path[len] != '\0') {
        if (path[len] == '/')
            last_slash = len;
        len++;
    }

    if (last_slash == 0U) {
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }
    if (last_slash >= cap)
        return -ENAMETOOLONG;

    memcpy(out, path, last_slash);
    out[last_slash] = '\0';
    return 0;
}

static int linux_resolve_link_target(const char *path, const char *target,
                                     char *out, size_t cap)
{
    char parent[VFSD_IO_BYTES];
    int  rc;

    if (!path || !target || !out || cap < 2U)
        return -EFAULT;
    if (target[0] == '/') {
        syscall_copy_fixed_string(out, cap, target);
        return 0;
    }

    rc = linux_parent_dir(path, parent, sizeof(parent));
    if (rc < 0)
        return rc;
    return syscall_path_join_abs(parent, target, out, cap);
}

static int linux_path_lstat_fill(const char *path, stat_t *out)
{
    char   target[VFSD_IO_BYTES];
    size_t len = 0U;
    int    rc;

    if (!path || !out)
        return -EFAULT;

    rc = linux_path_readlink(path, target, sizeof(target));
    if (rc == 0) {
        while (target[len] != '\0')
            len++;
        stat_fill(out, S_IFLNK | S_IRUSR | S_IRGRP | S_IROTH, (uint64_t)len, 512U);
        return 0;
    }

    return vfs_lstat(path, out);
}

static int linux_path_follow_once(const char *path, char *out, size_t cap)
{
    char target[VFSD_IO_BYTES];
    int  rc;

    if (!path || !out || cap < 2U)
        return -EFAULT;

    rc = linux_path_readlink(path, target, sizeof(target));
    if (rc < 0)
        rc = vfs_readlink(path, target, sizeof(target));
    if (rc < 0)
        return rc;

    return linux_resolve_link_target(path, target, out, cap);
}

static int path_stat_fill_native(const char *path, int nofollow, stat_t *out)
{
    char     current[VFSD_IO_BYTES];
    char     next[VFSD_IO_BYTES];
    fd_object_t *obj;
    fd_entry_t  *e;
    int      fd;
    int      rc;

    if (!path || !out)
        return -EFAULT;
    if (nofollow)
        return linux_path_lstat_fill(path, out);

    syscall_copy_fixed_string(current, sizeof(current), path);
    for (uint32_t depth = 0U; depth < 4U; depth++) {
        rc = linux_path_follow_once(current, next, sizeof(next));
        if (rc < 0)
            break;
        syscall_copy_fixed_string(current, sizeof(current), next);
    }

    fd = fd_open_path_current(current, O_RDONLY, 0U);
    if (fd < 0)
        return fd;

    obj = fd_get(fd);
    if (!obj) {
        rc = -EBADF;
    } else if (obj->type == FD_TYPE_VFSD) {
        rc = vfsd_proxy_stat(obj->remote_handle, out);
    } else if (obj->type == FD_TYPE_VFS) {
        rc = vfs_stat(&obj->file, out);
    } else {
        rc = -EBADF;
    }

    e = fd_entry_get(fd);
    if (e)
        fd_release_slot(e);
    return rc;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 1 — write
 *
 * args: fd, buf, count
 * RT-safe: dipende dal backend VFS; console/devfs rimane bounded.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_write(uint64_t args[6])
{
    int          fd       = (int)args[0];
    uintptr_t    buf_uva  = (uintptr_t)args[1];
    const void  *src      = (const void *)(uintptr_t)args[1];
    void        *bounce   = NULL;
    uint64_t     count    = args[2];
    int          copy_rc;

    fd_object_t *obj = fd_get(fd);
    if (!obj) return ERR(EBADF);
    if (!src) return ERR(EFAULT);
    if (!count) return 0;
    if (count > 4096) count = 4096;

    if (current_task && sched_task_is_user(current_task)) {
        bounce = kmalloc((uint32_t)count);
        if (!bounce)
            return ERR(ENOMEM);

        copy_rc = user_copy_bytes(buf_uva, bounce, (size_t)count);
        if (copy_rc < 0) {
            kfree(bounce);
            return ERR(-copy_rc);
        }
        src = bounce;
    }

    ssize_t rc;

    if (obj->type == FD_TYPE_VFSD) {
        rc = vfsd_proxy_write(obj->remote_handle, src, (size_t)count);
        if (rc > 0)
            obj->file.pos += (uint64_t)rc;
    } else if (obj->type == FD_TYPE_VFS) {
        rc = vfs_write(&obj->file, src, count);
    } else if (obj->type == FD_TYPE_PIPE) {
        rc = fd_pipe_write(obj, src, (size_t)count);
    } else {
        if (bounce)
            kfree(bounce);
        return ERR(EBADF);
    }

    if (bounce)
        kfree(bounce);
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
    int       fd      = (int)args[0];
    uintptr_t buf_uva = (uintptr_t)args[1];
    void     *dst     = (void *)(uintptr_t)args[1];
    void     *bounce  = NULL;
    uint64_t  cnt     = args[2];
    int       copy_rc;

    fd_object_t *obj = fd_get(fd);
    if (!obj) return ERR(EBADF);
    if (!dst) return ERR(EFAULT);
    if (cnt == 0) return 0;

    if (cnt > 4096) cnt = 4096;
    if (current_task && sched_task_is_user(current_task)) {
        bounce = kmalloc((uint32_t)cnt);
        if (!bounce)
            return ERR(ENOMEM);
        dst = bounce;
    }

    ssize_t rc;

    if (obj->type == FD_TYPE_VFSD) {
        rc = vfsd_proxy_read(obj->remote_handle, dst, (size_t)cnt);
        if (rc > 0)
            obj->file.pos += (uint64_t)rc;
    } else if (obj->type == FD_TYPE_VFS) {
        rc = vfs_read(&obj->file, dst, cnt);
    } else if (obj->type == FD_TYPE_PIPE) {
        rc = fd_pipe_read(obj, dst, (size_t)cnt);
    } else {
        if (bounce)
            kfree(bounce);
        return ERR(EBADF);
    }

    if (rc > 0 && bounce) {
        copy_rc = user_store_bytes(buf_uva, bounce, (size_t)rc);
        if (copy_rc < 0) {
            kfree(bounce);
            return ERR(-copy_rc);
        }
    }
    if (bounce)
        kfree(bounce);
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
    sched_task_exit_with_code((int32_t)args[0]);
    return 0;   /* unreachable */
}

static uint64_t sys_yield(uint64_t args[6])
{
    (void)args;
    sched_yield();
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 4 — open
 *
 * args: path, flags, mode (ignorato)
 * Instrada la risoluzione path sul layer VFS.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_open(uint64_t args[6])
{
    uintptr_t   path_uva = (uintptr_t)args[0];
    const char *path     = (const char *)(uintptr_t)args[0];
    const char *path_arg = path;
    char        path_buf[EXEC_MAX_PATH];
    uint16_t    oflags   = 0U;
    uint8_t     fd_flags = 0U;
    int         rc;

    if (!path) return ERR(EFAULT);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path_arg = path_buf;
    }

    fd_split_open_flags((uint16_t)args[1], &oflags, &fd_flags);
    rc = fd_open_path_current(path_arg, oflags, fd_flags);
    return (rc < 0) ? ERR(-rc) : (uint64_t)rc;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 5 — close
 *
 * args: fd
 * Chiude un fd e rilascia la descrizione condivisa se l'ultimo riferimento.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_close(uint64_t args[6])
{
    int fd = (int)args[0];

    if (fd < 0 || fd >= MAX_FD) return ERR(EBADF);

    int idx = task_idx();
    fd_entry_t *e = &fd_tables[idx][fd];
    if (!e->obj) return ERR(EBADF);

    fd_release_slot(e);
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
    int       fd      = (int)args[0];
    uintptr_t buf_uva = (uintptr_t)args[1];
    stat_t   *buf     = (stat_t *)(uintptr_t)args[1];
    stat_t    st;
    int       rc;

    fd_object_t *obj = fd_get(fd);
    if (!obj) return ERR(EBADF);
    if (!buf) return ERR(EFAULT);

    if (current_task && sched_task_is_user(current_task))
        buf = &st;

    if (obj->type == FD_TYPE_VFSD)
        rc = vfsd_proxy_stat(obj->remote_handle, buf);
    else if (obj->type == FD_TYPE_VFS)
        rc = vfs_stat(&obj->file, buf);
    else if (obj->type == FD_TYPE_PIPE) {
        stat_fill(buf, S_IFIFO | S_IRUSR | S_IWUSR |
                       S_IRGRP | S_IWGRP |
                       S_IROTH | S_IWOTH,
                  0ULL, PIPE_BUF_SIZE);
        rc = 0;
    } else
        return ERR(EBADF);

    if (rc < 0) return ERR(-rc);
    if (buf == &st) {
        rc = user_store_bytes(buf_uva, &st, sizeof(st));
        if (rc < 0)
            return ERR(-rc);
    }
    return 0;
}

static uint64_t sys_getpid(uint64_t args[6])
{
    (void)args;
    return current_task ? (uint64_t)sched_task_tgid(current_task) : 0ULL;
}

static uint64_t sys_getppid(uint64_t args[6])
{
    (void)args;
    return current_task ? (uint64_t)sched_task_parent_pid(current_task) : 0ULL;
}

static uint64_t sys_gettid(uint64_t args[6])
{
    (void)args;
    return current_task ? (uint64_t)current_task->pid : 0ULL;
}

static uint64_t sys_set_tid_address(uint64_t args[6])
{
    uintptr_t clear_tid_uva = (uintptr_t)args[0];
    int       rc;

    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);

    if (clear_tid_uva != 0U) {
        rc = user_probe_write(clear_tid_uva, sizeof(uint32_t));
        if (rc < 0)
            return ERR(-rc);
    }

    rc = sched_task_set_clear_tid(current_task, clear_tid_uva);
    if (rc < 0)
        return ERR(EINVAL);
    return (uint64_t)current_task->pid;
}

static uint64_t sys_exit_group(uint64_t args[6])
{
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);

    if (sched_task_begin_exit_group((int32_t)args[0]) < 0)
        return ERR(EINVAL);
    sched_task_exit_with_code((int32_t)args[0]);
    return 0;
}

static uint64_t sys_futex(uint64_t args[6])
{
    uintptr_t uaddr = (uintptr_t)args[0];
    uint32_t  op = (uint32_t)args[1];
    uint32_t  val = (uint32_t)args[2];
    uintptr_t arg3 = (uintptr_t)args[3];
    uintptr_t uaddr2 = (uintptr_t)args[4];
    uint32_t  val3 = (uint32_t)args[5];
    uint32_t  cmd = op & FUTEX_CMD_MASK;
    int       rc;

    switch (cmd) {
    case FUTEX_WAIT:
        rc = futex_wait_current(uaddr, val, arg3);
        break;
    case FUTEX_WAKE:
        rc = futex_wake_current(uaddr, val);
        break;
    case FUTEX_REQUEUE:
        rc = futex_requeue_current(uaddr, val, (uint32_t)arg3, uaddr2, 0, 0U);
        break;
    case FUTEX_CMP_REQUEUE:
        rc = futex_requeue_current(uaddr, val, (uint32_t)arg3, uaddr2, 1, val3);
        break;
    default:
        return ERR(ENOSYS);
    }

    return (rc < 0) ? ERR(-rc) : (uint64_t)(uint32_t)rc;
}

static uint64_t sys_gettimeofday(uint64_t args[6])
{
    uintptr_t  tv_uva = (uintptr_t)args[0];
    timeval_t *tv     = (timeval_t *)(uintptr_t)args[0];
    timeval_t  local_tv;
    uint64_t   ns     = timer_now_ns();
    int        rc;

    (void)args[1];

    if (!tv)
        return 0;

    if (current_task && sched_task_is_user(current_task))
        tv = &local_tv;

    tv->tv_sec  = (int64_t)(ns / 1000000000ULL);
    tv->tv_usec = (int64_t)((ns % 1000000000ULL) / 1000ULL);

    if (tv == &local_tv) {
        rc = user_store_bytes(tv_uva, &local_tv, sizeof(local_tv));
        if (rc < 0)
            return ERR(-rc);
    }

    return 0;
}

static uint64_t sys_nanosleep(uint64_t args[6])
{
    uintptr_t   req_uva = (uintptr_t)args[0];
    uintptr_t   rem_uva = (uintptr_t)args[1];
    timespec_t  req;
    timespec_t  rem;
    uint64_t    start_ns;
    uint64_t    duration_ns;
    int         rc;

    if (!req_uva)
        return ERR(EFAULT);

    rc = user_copy_bytes(req_uva, &req, sizeof(req));
    if (rc < 0)
        return ERR(-rc);
    if (req.tv_sec < 0 || req.tv_nsec < 0 || req.tv_nsec >= 1000000000LL)
        return ERR(EINVAL);

    duration_ns = (uint64_t)req.tv_sec * 1000000000ULL + (uint64_t)req.tv_nsec;
    start_ns = timer_now_ns();

    for (;;) {
        uint64_t now_ns = timer_now_ns();

        if (now_ns - start_ns >= duration_ns)
            break;
        if (signal_has_unblocked_pending(current_task)) {
            if (rem_uva != 0U) {
                uint64_t left_ns = duration_ns - (now_ns - start_ns);

                rem.tv_sec = (int64_t)(left_ns / 1000000000ULL);
                rem.tv_nsec = (int64_t)(left_ns % 1000000000ULL);
                rc = user_store_bytes(rem_uva, &rem, sizeof(rem));
                if (rc < 0)
                    return ERR(-rc);
            }
            return ERR(EINTR);
        }
        sched_yield();
    }

    if (rem_uva != 0U) {
        rem.tv_sec = 0;
        rem.tv_nsec = 0;
        rc = user_store_bytes(rem_uva, &rem, sizeof(rem));
        if (rc < 0)
            return ERR(-rc);
    }

    return 0;
}

static uint64_t sys_getuid(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

static uint64_t sys_getgid(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

static uint64_t sys_geteuid(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

static uint64_t sys_getegid(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

static uint64_t sys_lseek(uint64_t args[6])
{
    int          fd     = (int)args[0];
    int64_t      off    = (int64_t)args[1];
    int          whence = (int)args[2];
    fd_object_t *obj    = fd_get(fd);
    uint64_t     new_pos = 0ULL;
    int          rc;

    if (!obj)
        return ERR(EBADF);

    rc = fd_object_seek(obj, off, whence, &new_pos);
    return (rc < 0) ? ERR(-rc) : new_pos;
}

static uint64_t sys_rw_vector(uint64_t args[6], int is_write)
{
    int      fd      = (int)args[0];
    uintptr_t iov_uva = (uintptr_t)args[1];
    int      iovcnt  = (int)args[2];
    iovec_t  iov[IOV_MAX];
    uint64_t total = 0ULL;
    int      rc;

    if (!fd_get(fd))
        return ERR(EBADF);
    if (!iov_uva)
        return ERR(EFAULT);
    if (iovcnt < 0 || iovcnt > IOV_MAX)
        return ERR(EINVAL);
    if (iovcnt == 0)
        return 0ULL;

    rc = user_copy_bytes(iov_uva, iov, (size_t)iovcnt * sizeof(iov[0]));
    if (rc < 0)
        return ERR(-rc);

    for (int i = 0; i < iovcnt; i++) {
        uintptr_t base = (uintptr_t)iov[i].iov_base;
        size_t    remain = iov[i].iov_len;

        while (remain > 0U) {
            uint64_t call_args[6] = { 0 };
            size_t   chunk = (remain > 4096U) ? 4096U : remain;
            uint64_t n;

            call_args[0] = (uint64_t)fd;
            call_args[1] = (uint64_t)base;
            call_args[2] = (uint64_t)chunk;

            n = is_write ? sys_write(call_args) : sys_read(call_args);
            if ((int64_t)n < 0) {
                if (total != 0ULL)
                    return total;
                return n;
            }
            if (n == 0ULL)
                return total;

            total += n;
            base += (uintptr_t)n;
            remain -= (size_t)n;
            if (n < (uint64_t)chunk)
                break;
        }
    }

    return total;
}

static uint64_t sys_readv(uint64_t args[6])
{
    return sys_rw_vector(args, 0);
}

static uint64_t sys_writev(uint64_t args[6])
{
    return sys_rw_vector(args, 1);
}

static uint64_t sys_fcntl(uint64_t args[6])
{
    int         fd  = (int)args[0];
    int         cmd = (int)args[1];
    int64_t     arg = (int64_t)args[2];
    int         idx = task_idx();
    fd_entry_t *e   = NULL;
    fd_object_t *obj = NULL;
    int         newfd;
    uint32_t    keep_flags;

    if (fd < 0 || fd >= MAX_FD)
        return ERR(EBADF);
    e = &fd_tables[idx][fd];
    obj = e->obj;
    if (!obj)
        return ERR(EBADF);

    switch (cmd) {
    case F_GETFD:
        return fd_entry_cloexec(e) ? FD_CLOEXEC : 0ULL;
    case F_SETFD:
        fd_entry_set_cloexec(e, ((uint32_t)arg & FD_CLOEXEC) != 0U);
        return 0ULL;
    case F_GETFL:
        return (uint64_t)obj->flags;
    case F_SETFL:
        keep_flags = obj->flags & (O_RDONLY | O_WRONLY | O_RDWR);
        obj->flags = (uint16_t)(keep_flags |
                                ((uint32_t)arg & (O_APPEND | O_NONBLOCK)));
        sock_sync_fd_flags(obj);
        if (obj->type == FD_TYPE_VFS || obj->type == FD_TYPE_VFSD)
            obj->file.flags = obj->flags;
        return 0ULL;
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        if (arg < 0 || arg >= MAX_FD)
            return ERR(EINVAL);
        newfd = fd_alloc_from_in_slot(idx, (int)arg);
        if (newfd < 0)
            return ERR(ENFILE);
        if (fd_attach_object(&fd_tables[idx][newfd], obj) < 0)
            return ERR(ENFILE);
        fd_tables[idx][newfd].fd_flags =
            (cmd == F_DUPFD_CLOEXEC) ? FD_ENTRY_CLOEXEC : 0U;
        return (uint64_t)newfd;
    default:
        return ERR(ENOSYS);
    }
}

static uint64_t sys_openat(uint64_t args[6])
{
    int         dirfd = (int)args[0];
    uintptr_t   path_uva = (uintptr_t)args[1];
    const char *path = (const char *)(uintptr_t)args[1];
    const char *path_arg = path;
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    uint16_t    oflags = 0U;
    uint8_t     fd_flags = 0U;
    uint32_t    resolve_flags = 0U;
    int         rc;

    if (!path)
        return ERR(EFAULT);

    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path_arg = path_buf;
    }

    if (path_arg[0] == '\0')
        return ERR(ENOENT);

    fd_split_open_flags((uint16_t)args[2], &oflags, &fd_flags);
    rc = resolve_dirfd_path_meta(dirfd, path_arg, 0, resolved, sizeof(resolved),
                                 &resolve_flags);
    (void)resolve_flags;
    if (rc < 0)
        return ERR(-rc);

    rc = fd_open_path_current(resolved, oflags, fd_flags);
    return (rc < 0) ? ERR(-rc) : (uint64_t)rc;
}

static uint64_t sys_fstatat(uint64_t args[6])
{
    int         dirfd = (int)args[0];
    uintptr_t   path_uva = (uintptr_t)args[1];
    const char *path = (const char *)(uintptr_t)args[1];
    const char *path_arg = path;
    uintptr_t   stat_uva = (uintptr_t)args[2];
    uint32_t    flags = (uint32_t)args[3];
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    stat_t      st;
    int         rc;
    int         copy_rc;

    if (!stat_uva)
        return ERR(EFAULT);
    if (flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))
        return ERR(EINVAL);

    if (path) {
        if (current_task && sched_task_is_user(current_task)) {
            copy_rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
            if (copy_rc < 0)
                return ERR(-copy_rc);
            path_arg = path_buf;
        }
    } else {
        path_arg = "";
    }

    rc = resolve_dirfd_path_meta(dirfd, path_arg, (flags & AT_EMPTY_PATH) != 0U,
                                 resolved, sizeof(resolved), NULL);
    if (rc < 0)
        return ERR(-rc);

    rc = path_stat_fill_native(resolved, (flags & AT_SYMLINK_NOFOLLOW) != 0U, &st);
    if (rc < 0)
        return ERR(-rc);
    rc = user_store_bytes(stat_uva, &st, sizeof(st));
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_mkdir(uint64_t args[6])
{
    uintptr_t   path_uva = (uintptr_t)args[0];
    const char *path = (const char *)(uintptr_t)args[0];
    const char *path_arg = path;
    uint32_t    mode = (uint32_t)args[1];
    char        path_buf[EXEC_MAX_PATH];
    int         rc;

    if (!path)
        return ERR(EFAULT);

    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path_arg = path_buf;
    }

    if (path_arg[0] == '\0')
        return ERR(ENOENT);
    if (path_arg[0] != '/')
        return ERR(ENOSYS);

    rc = vfs_mkdir(path_arg, mode & 07777U);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_unlink(uint64_t args[6])
{
    uintptr_t   path_uva = (uintptr_t)args[0];
    const char *path = (const char *)(uintptr_t)args[0];
    const char *path_arg = path;
    char        path_buf[EXEC_MAX_PATH];
    int         rc;

    if (!path)
        return ERR(EFAULT);

    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path_arg = path_buf;
    }

    if (path_arg[0] == '\0')
        return ERR(ENOENT);
    if (path_arg[0] != '/')
        return ERR(ENOSYS);

    rc = vfs_unlink(path_arg);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_rename(uint64_t args[6])
{
    uintptr_t   old_uva = (uintptr_t)args[0];
    uintptr_t   new_uva = (uintptr_t)args[1];
    const char *old_path = (const char *)(uintptr_t)args[0];
    const char *new_path = (const char *)(uintptr_t)args[1];
    const char *old_arg = old_path;
    const char *new_arg = new_path;
    char        old_buf[EXEC_MAX_PATH];
    char        new_buf[EXEC_MAX_PATH];
    int         rc;

    if (!old_path || !new_path)
        return ERR(EFAULT);

    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(old_uva, old_buf, sizeof(old_buf));
        if (rc < 0)
            return ERR(-rc);
        rc = user_copy_cstr(new_uva, new_buf, sizeof(new_buf));
        if (rc < 0)
            return ERR(-rc);
        old_arg = old_buf;
        new_arg = new_buf;
    }

    if (old_arg[0] == '\0' || new_arg[0] == '\0')
        return ERR(ENOENT);
    if (old_arg[0] != '/' || new_arg[0] != '/')
        return ERR(ENOSYS);

    rc = vfs_rename(old_arg, new_arg);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_ioctl(uint64_t args[6])
{
    int         fd      = (int)args[0];
    uint64_t    req     = args[1];
    uintptr_t   arg_uva = (uintptr_t)args[2];
    fd_object_t *obj    = fd_get(fd);
    int         rc;

    if (!obj)
        return ERR(EBADF);

    switch (req) {
    case TCGETS: {
        termios_t term;

        if (!arg_uva)
            return ERR(EFAULT);
        if (!fd_is_tty_object(obj))
            return ERR(ENOTTY);
        rc = tty_tcgetattr(&term);
        if (rc < 0)
            return ERR(-rc);
        rc = user_store_bytes(arg_uva, &term, sizeof(term));
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }
    case TCSETS: {
        termios_t term;

        if (!arg_uva)
            return ERR(EFAULT);
        if (!fd_is_tty_object(obj))
            return ERR(ENOTTY);
        rc = user_copy_bytes(arg_uva, &term, sizeof(term));
        if (rc < 0)
            return ERR(-rc);
        rc = tty_tcsetattr(TCSANOW, &term);
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }
    case TIOCGPGRP: {
        uint32_t pgid;

        if (!arg_uva)
            return ERR(EFAULT);
        if (!fd_is_tty_object(obj))
            return ERR(ENOTTY);
        pgid = tty_tcgetpgrp();
        rc = user_store_bytes(arg_uva, &pgid, sizeof(pgid));
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }
    case TIOCSPGRP: {
        uint32_t pgid;

        if (!arg_uva)
            return ERR(EFAULT);
        if (!fd_is_tty_object(obj))
            return ERR(ENOTTY);
        rc = user_copy_bytes(arg_uva, &pgid, sizeof(pgid));
        if (rc < 0)
            return ERR(-rc);
        rc = tty_tcsetpgrp_current(pgid);
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }
    case TIOCGWINSZ: {
        winsize_t ws;

        if (!arg_uva)
            return ERR(EFAULT);
        if (!fd_is_tty_object(obj))
            return ERR(ENOTTY);
        ws.ws_row = 25U;
        ws.ws_col = 80U;
        ws.ws_xpixel = 0U;
        ws.ws_ypixel = 0U;
        rc = user_store_bytes(arg_uva, &ws, sizeof(ws));
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }
    case FIONBIO: {
        uint32_t enable = 0U;

        if (!arg_uva)
            return ERR(EFAULT);
        rc = user_copy_bytes(arg_uva, &enable, sizeof(enable));
        if (rc < 0)
            return ERR(-rc);
        if (enable)
            obj->flags |= O_NONBLOCK;
        else
            obj->flags &= (uint16_t)~O_NONBLOCK;
        sock_sync_fd_flags(obj);
        if (obj->type == FD_TYPE_VFS || obj->type == FD_TYPE_VFSD)
            obj->file.flags = obj->flags;
        return 0ULL;
    }
    default:
        return ERR(ENOTTY);
    }
}

static uint64_t sys_uname(uint64_t args[6])
{
    uintptr_t  uts_uva = (uintptr_t)args[0];
    utsname_t  uts;
    int        rc;

    if (!uts_uva)
        return ERR(EFAULT);

    memset(&uts, 0, sizeof(uts));
    syscall_copy_fixed_string(uts.sysname, sizeof(uts.sysname), "EnlilOS");
    syscall_copy_fixed_string(uts.nodename, sizeof(uts.nodename), "enlil");
    syscall_copy_fixed_string(uts.release, sizeof(uts.release), "0.1.0");
    syscall_copy_fixed_string(uts.version, sizeof(uts.version), "Microkernel");
    syscall_copy_fixed_string(uts.machine, sizeof(uts.machine), "aarch64");
    syscall_copy_fixed_string(uts.domainname, sizeof(uts.domainname), "localdomain");

    rc = user_store_bytes(uts_uva, &uts, sizeof(uts));
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 7 — mmap
 *
 * args: addr(hint), length, prot, flags, fd, offset
 * Supporta MAP_ANONYMOUS (anonima) e file-backed (MAP_SHARED/MAP_PRIVATE).
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_mmap(uint64_t args[6])
{
    uint64_t    length      = args[1];
    uint32_t    prot        = (uint32_t)args[2];
    uint32_t    flags       = (uint32_t)args[3];
    int         fd          = (int)(int64_t)args[4];
    uint64_t    file_offset = args[5];
    uint64_t    pages;
    size_t      aligned;
    uintptr_t   user_va;
    mm_space_t *space;

    (void)args[0]; /* hint VA — ignorato, usiamo mmap_base */

    if (!current_task || !sched_task_is_user(current_task))
        return MAP_FAILED_VA;
    if (length == 0ULL)
        return MAP_FAILED_VA;

    pages = (length + PAGE_SIZE - 1ULL) / PAGE_SIZE;
    /* 1024 pages (4 MB) — sufficiente per ArkshAst (~460 pp) e allocazioni grandi */
    if (pages == 0ULL || pages > 1024ULL)
        return MAP_FAILED_VA;

    aligned = (size_t)pages * PAGE_SIZE;
    space   = sched_task_space(current_task);
    if (!space)
        return MAP_FAILED_VA;

    /* ── Caso 1: mappatura anonima ──────────────────────────────── */
    if (flags & MAP_ANONYMOUS) {
        if (mmu_map_user_anywhere(space, aligned,
                                  sys_mmap_prot_to_mmu(prot), &user_va) < 0)
            return MAP_FAILED_VA;
        return (uint64_t)user_va;
    }

    /* ── Caso 2: file-backed (M8-02) ───────────────────────────── */
    {
        fd_object_t *obj;
        vfs_file_t   f;
        uint32_t    vma_flags = 0U;
        uint32_t    mmu_prot;
        size_t      i;

        obj = fd_get(fd);
        if (!obj) return MAP_FAILED_VA;
        if (obj->type != FD_TYPE_VFS && obj->type != FD_TYPE_VFSD)
            return MAP_FAILED_VA;

        /* Copia il file handle e riposiziona all'offset richiesto */
        f     = obj->file;
        f.pos = file_offset;

        /* Alloca le pagine fisiche e mappa in user space */
        mmu_prot = sys_mmap_prot_to_mmu(prot);
        /* MAP_PRIVATE: anche PROT_READ-only ottiene copia scrivibile temporaneamente
         * per il caricamento; dopo settiamo in read-only se non PROT_WRITE. */
        if (mmu_map_user_anywhere(space, aligned,
                                  mmu_prot | MMU_PROT_USER_R, &user_va) < 0)
            return MAP_FAILED_VA;

        /* Carica il contenuto del file pagina per pagina (eager loading) */
        for (i = 0U; i < (size_t)pages; i++) {
            uintptr_t  page_va = user_va + (uintptr_t)(i * PAGE_SIZE);
            uint64_t   off     = file_offset + (uint64_t)(i * PAGE_SIZE);
            void      *kva;
            ssize_t    nr;

            kva = mmu_space_resolve_ptr(space, page_va, PAGE_SIZE);
            if (!kva) continue;

            f.pos = off;
            nr = vfs_read(&f, kva, PAGE_SIZE);
            (void)nr; /* il resto è già azzerato da mmu_map_user_anywhere */
        }

        /* Costruisce i flag VMM */
        if (flags & MAP_SHARED) vma_flags |= VMA_FLAG_SHARED;
        if (prot & PROT_WRITE)  vma_flags |= VMA_FLAG_WRITE;
        if (prot & PROT_EXEC)   vma_flags |= VMA_FLAG_EXEC;

        /* Registra la VMA per il write-back (msync/munmap) */
        (void)vmm_map_file(sched_task_proc_slot(current_task), user_va, aligned,
                           vma_flags, file_offset, &obj->file);

        return (uint64_t)user_va;
    }
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 8 — munmap
 *
 * args: addr, length
 * Per le VMA file-backed MAP_SHARED: write-back prima di rimuovere.
 * Il free fisico delle pagine è rinviato alla distruzione dello mm_space.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_munmap(uint64_t args[6])
{
    uintptr_t   addr   = (uintptr_t)args[0];
    uint64_t    length = args[1];
    uintptr_t   aligned_end;
    size_t      aligned_len;
    mm_space_t *space;

    if (addr == 0U || length == 0U) return ERR(EINVAL);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EINVAL);
    if (addr < MMU_USER_BASE || addr >= MMU_USER_LIMIT)
        return ERR(EINVAL);
    if ((addr & (PAGE_SIZE - 1ULL)) != 0ULL)
        return ERR(EINVAL);

    aligned_end = (uintptr_t)((addr + length + PAGE_SIZE - 1ULL) & PAGE_MASK);
    if (aligned_end < addr || aligned_end > MMU_USER_LIMIT)
        return ERR(EINVAL);
    aligned_len = (size_t)(aligned_end - addr);
    if (aligned_len == 0U)
        return ERR(EINVAL);

    space = sched_task_space(current_task);
    if (!space)
        return ERR(EINVAL);

    /* Se la regione era file-backed MAP_SHARED: scrive le pagine sporche */
    (void)vmm_unmap_range(sched_task_proc_slot(current_task), space,
                          addr, aligned_len);

    if (mmu_unmap_user_region(space, addr, aligned_len) < 0)
        return ERR(EINVAL);
    return 0ULL;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall 24 — msync (M8-02)
 *
 * args: addr, length, flags
 * Scrive le pagine modificate di una VMA MAP_SHARED nel file di backing.
 * flags: MS_SYNC (4) = sincrono, MS_ASYNC (1) = no-op in questa v1.
 * ════════════════════════════════════════════════════════════════════ */
static uint64_t sys_msync(uint64_t args[6])
{
    uintptr_t   addr   = (uintptr_t)args[0];
    size_t      length = (size_t)args[1];
    uint32_t    flags  = (uint32_t)args[2];
    mm_space_t *space;

    (void)flags; /* MS_ASYNC trattato come MS_SYNC per semplicità */

    if (addr == 0U || length == 0U) return ERR(EINVAL);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EINVAL);
    if (addr < MMU_USER_BASE || addr >= MMU_USER_LIMIT)
        return ERR(EINVAL);

    space = sched_task_space(current_task);
    if (!space) return ERR(EINVAL);

    return (uint64_t)(int64_t)vmm_msync(sched_task_proc_slot(current_task), space, addr, length);
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
    char               resolved_path[VFSD_IO_BYTES];
    uint32_t           resolve_flags = 0U;
    int                rc;

    if (!current_task || !sched_task_is_user(current_task) || !frame)
        return ERR(ENOSYS);
    if (sched_task_proc_refcount(current_task) > 1U)
        return ERR(EBUSY);

    copy = (exec_copy_t *)kmalloc((uint32_t)sizeof(exec_copy_t));
    if (!copy)
        return ERR(ENOMEM);

    kdebug_watchdog_pause();
    rc = exec_copy_from_user(copy,
                             (uintptr_t)args[0],
                             (uintptr_t)args[1],
                             (uintptr_t)args[2]);
    if (rc < 0) {
        kdebug_watchdog_resume();
        kfree(copy);
        return ERR(-rc);
    }

    rc = resolve_user_vfs_path_meta(copy->path, resolved_path, sizeof(resolved_path),
                                    &resolve_flags);
    if (rc < 0) {
        kdebug_watchdog_resume();
        kfree(copy);
        return ERR(-rc);
    }

    rc = elf64_load_from_path_exec(resolved_path,
                                   copy->argv, copy->argc,
                                   copy->envp, copy->envc,
                                   &image);
    if (rc < 0) {
        kdebug_watchdog_resume();
        kfree(copy);
        return ERR(EIO);
    }

    fd_close_cloexec_in_slot(task_idx());

    old_mm = sched_task_space(current_task);
    if (sched_task_rebind_user(current_task, image.space,
                               image.entry, image.user_sp,
                               image.argc, image.argv,
                               image.envp, image.auxv) < 0) {
        elf64_unload_image(&image);
        kdebug_watchdog_resume();
        kfree(copy);
        return ERR(EIO);
    }

    mreact_task_cleanup(current_task);
    kmon_task_cleanup(current_task);
    ksem_task_cleanup(current_task);
    mmu_activate_space(image.space);
    if (old_mm && old_mm != image.space && old_mm != mmu_kernel_space())
        mmu_space_destroy(old_mm);

    signal_task_reset_for_exec(current_task);
    (void)sched_task_set_abi_mode(current_task,
                                  (resolve_flags & VFSD_RESP_FLAG_LINUX_COMPAT) != 0U
                                      ? SCHED_ABI_LINUX
                                      : SCHED_ABI_ENLILOS);
    (void)sched_task_set_exec_path(current_task, resolved_path);
    task_brk[task_idx()] = 0ULL;
    elf64_dlreset_proc((uint32_t)task_idx());
    /* Rebase del thread pointer sul TLS iniziale preparato dal loader.
     * Binarî statici musl possono toccare errno/TLS gia' nello startup
     * o nelle prime syscall libc, quindi azzerare TPIDR_EL0 qui rompe
     * execve() per programmi perfettamente validi come arksh. */
    sched_task_set_tpidr(current_task, image.tpidr_el0);
    __asm__ volatile("msr tpidr_el0, %0" :: "r"(image.tpidr_el0) : "memory");
    memset(frame->x, 0, sizeof(frame->x));
    frame->x[0] = image.argc;
    frame->x[1] = image.argv;
    frame->x[2] = image.envp;
    frame->x[3] = image.auxv;
    frame->sp   = image.user_sp;
    frame->pc   = image.entry;
    frame->spsr = 0ULL;
    active_syscall_replaced = 1U;
    kdebug_watchdog_resume();
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
    mm_space_t         *parent_mm;
    mm_space_t         *child_mm;
    sched_tcb_t        *child;
    exception_frame_t  *frame = active_syscall_frame;
    int                 parent_idx;
    int                 child_idx;

    (void)args;

    if (!current_task || !sched_task_is_user(current_task) || !frame)
        return ERR(ENOSYS);
    if (current_task->flags & TCB_FLAG_RT)
        kdebug_panic("fork() vietato su task hard-RT");
    if (sched_task_proc_refcount(current_task) > 1U)
        return ERR(EBUSY);

    parent_mm = sched_task_space(current_task);
    child_mm = mmu_space_clone_cow(parent_mm, frame->sp);
    if (!child_mm)
        return ERR(ENOMEM);

    child = sched_task_fork_user(current_task->name ? current_task->name : "user-fork",
                                 child_mm, frame, current_task->priority);
    if (!child) {
        mmu_space_destroy(child_mm);
        return ERR(ENOMEM);
    }

    parent_idx = task_idx();
    child_idx = (int)(sched_task_proc_slot(child) % SCHED_MAX_TASKS);
    fd_clone_task_table(child_idx, parent_idx);
    task_brk[child_idx] = task_brk[parent_idx];
    signal_task_fork(child, current_task);
    (void)sched_task_set_abi_mode(child, sched_task_abi_mode(current_task));

    return (uint64_t)child->pid;
}

static uint64_t sys_clone(uint64_t args[6])
{
    static const uint32_t required =
        CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD;
    static const uint32_t supported =
        required | CLONE_SYSVSEM | CLONE_SETTLS |
        CLONE_PARENT_SETTID | CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID;

    uint32_t          flags = (uint32_t)args[0];
    uintptr_t         child_stack = (uintptr_t)args[1];
    uintptr_t         parent_tid_uva = (uintptr_t)args[2];
    uint64_t          child_tpidr = args[3];
    uintptr_t         child_tid_uva = (uintptr_t)args[4];
    exception_frame_t *frame = active_syscall_frame;
    sched_tcb_t       *child;
    uint32_t           child_tid;
    int                rc;

    if (!current_task || !sched_task_is_user(current_task) || !frame)
        return ERR(ENOSYS);
    if ((flags & required) != required)
        return ERR(EINVAL);
    if ((flags & ~supported) != 0U)
        return ERR(EINVAL);
    if (sched_task_proc_refcount(current_task) >= SCHED_MAX_TASKS)
        return ERR(EAGAIN);
    if (child_stack != 0U &&
        (child_stack < MMU_USER_BASE || child_stack >= MMU_USER_LIMIT))
        return ERR(EINVAL);
    if ((flags & CLONE_PARENT_SETTID) != 0U) {
        rc = user_probe_write(parent_tid_uva, sizeof(uint32_t));
        if (rc < 0)
            return ERR(-rc);
    }
    if ((flags & (CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID)) != 0U) {
        rc = user_probe_write(child_tid_uva, sizeof(uint32_t));
        if (rc < 0)
            return ERR(-rc);
    }

    if ((flags & CLONE_SETTLS) == 0U)
        child_tpidr = sched_task_get_tpidr(current_task);

    child = sched_task_clone_user_thread(current_task->name ? current_task->name : "user-thread",
                                         frame,
                                         child_stack,
                                         child_tpidr,
                                         ((flags & CLONE_CHILD_SETTID) != 0U)
                                             ? child_tid_uva : 0U,
                                         ((flags & CLONE_CHILD_CLEARTID) != 0U)
                                             ? child_tid_uva : 0U,
                                         current_task->priority);
    if (!child)
        return ERR(ENOMEM);

    signal_task_clone_thread(child, current_task);
    child_tid = child->pid;

    if ((flags & CLONE_PARENT_SETTID) != 0U) {
        rc = user_store_bytes(parent_tid_uva, &child_tid, sizeof(child_tid));
        if (rc < 0)
            return ERR(-rc);
    }

    return (uint64_t)child_tid;
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
    int32_t   pid_arg      = (int32_t)args[0];
    uintptr_t status_uva   = (uintptr_t)args[1];
    uint32_t  options      = (uint32_t)args[2];
    uint64_t  timeout_ms   = args[3];
    uint64_t  deadline;
    uint32_t  caller_pid;

    if (options & ~(WNOHANG | WUNTRACED))
        return ERR(EINVAL);
    if (!current_task)
        return ERR(ECHILD);

    caller_pid = sched_task_tgid(current_task);
    deadline = (timeout_ms > 0U) ? (timer_now_ms() + timeout_ms) : (uint64_t)-1ULL;

    for (;;) {
        int matched_child = 0;

        for (uint32_t i = 0U; i < sched_task_count_total(); i++) {
            sched_tcb_t *t = sched_task_at(i);
            int          status = 0;
            int32_t      code = 0;
            int          stop_sig = 0;
            int          match = 0;

            if (!t)
                continue;
            if (sched_task_is_thread(t))
                continue;
            if (sched_task_parent_pid(t) != caller_pid)
                continue;

            if (pid_arg > 0)
                match = ((uint32_t)pid_arg == sched_task_tgid(t));
            else if (pid_arg == 0)
                match = (sched_task_pgid(t) == sched_task_pgid(current_task));
            else if (pid_arg == -1)
                match = 1;
            else
                match = (sched_task_pgid(t) == (uint32_t)(-pid_arg));

            if (!match)
                continue;

            matched_child = 1;

            if (t->state == TCB_STATE_ZOMBIE && sched_task_is_process_waitable(t)) {
                if (sched_task_get_exit_code(t, &code) < 0)
                    code = 0;
                status = ((int)code & 0xFF) << 8;
                if (status_uva != 0U &&
                    user_store_bytes(status_uva, &status, sizeof(status)) < 0)
                    return ERR(EFAULT);
                return (uint64_t)t->pid;
            }

            if ((options & WUNTRACED) &&
                signal_task_consume_stop_report(t, &stop_sig)) {
                status = (((int)stop_sig & 0xFF) << 8) | 0x7F;
                if (status_uva != 0U &&
                    user_store_bytes(status_uva, &status, sizeof(status)) < 0)
                    return ERR(EFAULT);
                return (uint64_t)t->pid;
            }
        }

        if (!matched_child)
            return ERR(ECHILD);
        if (options & WNOHANG)
            return 0;
        if (signal_has_unblocked_pending(current_task))
            return ERR(EINTR);
        if (timer_now_ms() >= deadline)
            return ERR(EAGAIN);
        sched_yield();
    }
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
    uintptr_t   ts_uva = (uintptr_t)args[1];
    timespec_t *ts     = (timespec_t *)(uintptr_t)args[1];
    timespec_t  local_ts;
    int         rc;

    uint64_t ns = timer_now_ns();

    if (ts) {
        if (current_task && sched_task_is_user(current_task))
            ts = &local_ts;

        ts->tv_sec  = (int64_t)(ns / 1000000000ULL);
        ts->tv_nsec = (int64_t)(ns % 1000000000ULL);

        if (ts == &local_ts) {
            rc = user_store_bytes(ts_uva, &local_ts, sizeof(local_ts));
            if (rc < 0)
                return ERR(-rc);
        }
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
    fd_object_t  *obj = fd_get(fd);
    uint32_t      copied = 0U;

    if (!obj) return ERR(EBADF);
    if (!buf_uva) return ERR(EFAULT);
    if (max_entries == 0U) return 0;
    if (max_entries > 64U) max_entries = 64U;

    while (copied < max_entries) {
        vfs_dirent_t  ent;
        sys_dirent_t  out;
        int           rc;

        if (obj->type == FD_TYPE_VFSD)
            rc = vfsd_proxy_readdir(obj->remote_handle, &ent);
        else if (obj->type == FD_TYPE_VFS)
            rc = vfs_readdir(&obj->file, &ent);
        else
            return ERR(EBADF);

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
    char      resolved_path[VFSD_IO_BYTES];
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

    rc = resolve_user_vfs_path(path, resolved_path, sizeof(resolved_path));
    if (rc < 0)
        return ERR(-rc);

    rc = elf64_spawn_path(resolved_path, path, prio, &pid);
    if (rc < 0)
        return ERR(EIO);

    rc = user_store_bytes(pid_uva, &pid, sizeof(pid));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_sigaction(uint64_t args[6])
{
    int               sig = (int)args[0];
    uintptr_t         act_uva = (uintptr_t)args[1];
    uintptr_t         old_uva = (uintptr_t)args[2];
    sigaction_t       act;
    sigaction_t       old;
    sigaction_t      *act_ptr = NULL;
    sigaction_t      *old_ptr = NULL;
    int               rc;

    if (act_uva != 0U) {
        rc = user_copy_bytes(act_uva, &act, sizeof(act));
        if (rc < 0)
            return ERR(-rc);
        act_ptr = &act;
    }
    if (old_uva != 0U)
        old_ptr = &old;

    rc = signal_sigaction_current(sig, act_ptr, old_ptr);
    if (rc < 0)
        return ERR(-rc);
    if (old_ptr) {
        rc = user_store_bytes(old_uva, &old, sizeof(old));
        if (rc < 0)
            return ERR(-rc);
    }
    return 0;
}

static uint64_t sys_sigprocmask(uint64_t args[6])
{
    int        how = (int)args[0];
    uintptr_t  set_uva = (uintptr_t)args[1];
    uintptr_t  old_uva = (uintptr_t)args[2];
    uint64_t   set_mask = 0ULL;
    uint64_t   old_mask = 0ULL;
    uint64_t  *set_ptr = NULL;
    uint64_t  *old_ptr = NULL;
    int        rc;

    if (set_uva != 0U) {
        rc = user_copy_bytes(set_uva, &set_mask, sizeof(set_mask));
        if (rc < 0)
            return ERR(-rc);
        set_ptr = &set_mask;
    }
    if (old_uva != 0U)
        old_ptr = &old_mask;

    rc = signal_sigprocmask_current(how, set_ptr, old_ptr);
    if (rc < 0)
        return ERR(-rc);
    if (old_ptr) {
        rc = user_store_bytes(old_uva, &old_mask, sizeof(old_mask));
        if (rc < 0)
            return ERR(-rc);
    }
    return 0;
}

static uint64_t sys_sigreturn(uint64_t args[6])
{
    int rc;

    (void)args;
    if (!active_syscall_frame)
        return ERR(EFAULT);

    rc = signal_sigreturn_current(active_syscall_frame);
    if (rc < 0)
        return ERR(-rc);

    active_syscall_replaced = 1U;
    return 0;
}

static uint64_t sys_kill(uint64_t args[6])
{
    int32_t  pid_arg = (int32_t)args[0];
    int      sig = (int)args[1];
    int      rc;
    uint32_t pgid;

    if (pid_arg > 0) {
        rc = signal_send_pid((uint32_t)pid_arg, sig);
    } else if (pid_arg == 0) {
        pgid = sched_task_pgid(current_task);
        rc = (pgid == 0U) ? -EINVAL : signal_send_pgrp(pgid, sig);
    } else if (pid_arg < -1) {
        rc = signal_send_pgrp((uint32_t)(-pid_arg), sig);
    } else {
        rc = -EINVAL;
    }

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_tgkill(uint64_t args[6])
{
    uint32_t tgid = (uint32_t)args[0];
    uint32_t tid  = (uint32_t)args[1];
    int      sig  = (int)args[2];
    int      rc;

    rc = signal_send_tgkill(tgid, tid, sig);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_dlopen(uint64_t args[6])
{
    uintptr_t path_uva = (uintptr_t)args[0];
    uint32_t  flags = (uint32_t)args[1];
    uintptr_t handle = 0U;
    char      path[EXEC_MAX_PATH];
    int       rc;

    if (!path_uva)
        return ERR(EFAULT);

    rc = user_copy_cstr(path_uva, path, sizeof(path));
    if (rc < 0)
        return ERR(-rc);

    rc = elf64_dlopen_current(path, flags, &handle);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)handle;
}

static uint64_t sys_dlsym(uint64_t args[6])
{
    uintptr_t handle = (uintptr_t)args[0];
    uintptr_t name_uva = (uintptr_t)args[1];
    uintptr_t value = 0U;
    char      name[EXEC_MAX_PATH];
    int       rc;

    if (!name_uva)
        return ERR(EFAULT);

    rc = user_copy_cstr(name_uva, name, sizeof(name));
    if (rc < 0)
        return ERR(-rc);

    rc = elf64_dlsym_current(handle, name, &value);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)value;
}

static uint64_t sys_dlclose(uint64_t args[6])
{
    int rc = elf64_dlclose_current((uintptr_t)args[0]);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_dlerror(uint64_t args[6])
{
    uintptr_t buf_uva = (uintptr_t)args[0];
    size_t    cap = (size_t)args[1];
    char      msg[96];
    int       rc;
    size_t    copy_len;
    uint64_t  ret_len;

    if (buf_uva == 0U || cap == 0U)
        return ERR(EFAULT);

    rc = elf64_dlerror_drain_current(msg, sizeof(msg));
    if (rc < 0)
        return ERR(-rc);

    copy_len = (size_t)rc + 1U;
    if (copy_len > cap)
        copy_len = cap;
    if (copy_len == 0U)
        copy_len = 1U;
    ret_len = (uint64_t)((rc > 0) ? (uint32_t)rc : 0U);

    rc = user_store_bytes(buf_uva, msg, copy_len);
    if (rc < 0)
        return ERR(-rc);
    return ret_len;
}

static uint64_t sys_setpgid(uint64_t args[6])
{
    int32_t      pid_arg = (int32_t)args[0];
    uint32_t     pgid = (uint32_t)args[1];
    sched_tcb_t *target;
    int          rc;

    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (pid_arg == 0)
        pid_arg = (int32_t)sched_task_tgid(current_task);

    target = sched_task_find((uint32_t)pid_arg);
    if (!target)
        return ERR(ESRCH);

    rc = sched_task_setpgid(current_task, target, pgid);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_getpgid(uint64_t args[6])
{
    int32_t      pid_arg = (int32_t)args[0];
    sched_tcb_t *target;

    if (!current_task)
        return ERR(ESRCH);
    if (pid_arg == 0)
        return (uint64_t)sched_task_pgid(current_task);

    target = sched_task_find((uint32_t)pid_arg);
    if (!target)
        return ERR(ESRCH);
    return (uint64_t)sched_task_pgid(target);
}

static uint64_t sys_setsid(uint64_t args[6])
{
    uint32_t sid = 0U;
    int      rc;

    (void)args;
    rc = sched_task_setsid(current_task, &sid);
    return (rc < 0) ? ERR(-rc) : (uint64_t)sid;
}

static uint64_t sys_getsid(uint64_t args[6])
{
    int32_t      pid_arg = (int32_t)args[0];
    sched_tcb_t *target;

    if (!current_task)
        return ERR(ESRCH);
    if (pid_arg == 0)
        return (uint64_t)sched_task_sid(current_task);

    target = sched_task_find((uint32_t)pid_arg);
    if (!target)
        return ERR(ESRCH);
    return (uint64_t)sched_task_sid(target);
}

static uint64_t sys_tcsetpgrp(uint64_t args[6])
{
    int rc = tty_tcsetpgrp_current((uint32_t)args[0]);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_tcgetpgrp(uint64_t args[6])
{
    (void)args;
    return (uint64_t)tty_tcgetpgrp();
}

static uint64_t sys_chdir(uint64_t args[6])
{
    uintptr_t path_uva = (uintptr_t)args[0];
    char      path[VFSD_PATH_BYTES];
    int       rc;

    if (!path_uva)
        return ERR(EFAULT);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (!vfsd_proxy_available())
        return ERR(ENOSYS);

    rc = user_copy_cstr(path_uva, path, sizeof(path));
    if (rc < 0)
        return ERR(-rc);

    rc = vfsd_proxy_chdir(path);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_getcwd(uint64_t args[6])
{
    uintptr_t buf_uva = (uintptr_t)args[0];
    uint32_t  size = (uint32_t)args[1];
    char      path[VFSD_IO_BYTES];
    size_t    len;
    int       rc;

    if (!buf_uva || size == 0U)
        return ERR(EFAULT);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (!vfsd_proxy_available())
        return ERR(ENOSYS);

    rc = vfsd_proxy_getcwd(path, sizeof(path));
    if (rc < 0)
        return ERR(-rc);

    for (len = 0U; len < sizeof(path) && path[len] != '\0'; len++)
        ;
    len++;
    if (len > size)
        return ERR(ERANGE);

    rc = user_store_bytes(buf_uva, path, len);
    if (rc < 0)
        return ERR(-rc);
    return buf_uva;
}

static uint64_t sys_pipe(uint64_t args[6])
{
    uintptr_t    fds_uva = (uintptr_t)args[0];
    int32_t      fds[2];
    int          idx = task_idx();
    int          fd_r;
    int          fd_w;
    pipe_t      *pipe;
    fd_object_t *obj_r;
    fd_object_t *obj_w;
    int          rc;

    if (!fds_uva)
        return ERR(EFAULT);

    fd_r = fd_alloc_in_slot(idx);
    if (fd_r < 0)
        return ERR(ENFILE);

    fd_w = -1;
    for (int i = 3; i < MAX_FD; i++) {
        if (i == fd_r)
            continue;
        if (fd_tables[idx][i].obj == NULL) {
            fd_w = i;
            break;
        }
    }
    if (fd_w < 0)
        return ERR(ENFILE);

    pipe = fd_pipe_alloc();
    if (!pipe)
        return ERR(ENFILE);

    obj_r = fd_object_alloc();
    obj_w = fd_object_alloc();
    if (!obj_r || !obj_w) {
        if (obj_r)
            memset(obj_r, 0, sizeof(*obj_r));
        if (obj_w)
            memset(obj_w, 0, sizeof(*obj_w));
        memset(pipe, 0, sizeof(*pipe));
        return ERR(ENFILE);
    }

    pipe->readers = 1U;
    pipe->writers = 1U;

    obj_r->type = FD_TYPE_PIPE;
    obj_r->flags = O_RDONLY;
    obj_r->pipe = pipe;
    obj_r->pipe_end = PIPE_END_READ;

    obj_w->type = FD_TYPE_PIPE;
    obj_w->flags = O_WRONLY;
    obj_w->pipe = pipe;
    obj_w->pipe_end = PIPE_END_WRITE;

    rc = fd_attach_object(&fd_tables[idx][fd_r], obj_r);
    if (rc < 0) {
        memset(obj_r, 0, sizeof(*obj_r));
        memset(obj_w, 0, sizeof(*obj_w));
        memset(pipe, 0, sizeof(*pipe));
        return ERR(-rc);
    }

    rc = fd_attach_object(&fd_tables[idx][fd_w], obj_w);
    if (rc < 0) {
        fd_release_slot(&fd_tables[idx][fd_r]);
        memset(obj_w, 0, sizeof(*obj_w));
        memset(pipe, 0, sizeof(*pipe));
        return ERR(-rc);
    }

    fds[0] = fd_r;
    fds[1] = fd_w;
    rc = user_store_bytes(fds_uva, fds, sizeof(fds));
    if (rc < 0) {
        fd_release_slot(&fd_tables[idx][fd_r]);
        fd_release_slot(&fd_tables[idx][fd_w]);
        return ERR(-rc);
    }

    return 0;
}

static uint64_t sys_dup(uint64_t args[6])
{
    int          oldfd = (int)args[0];
    int          newfd;
    fd_object_t *obj = fd_get(oldfd);

    if (!obj)
        return ERR(EBADF);

    newfd = fd_alloc();
    if (newfd < 0)
        return ERR(ENFILE);

    if (fd_attach_object(&fd_tables[task_idx()][newfd], obj) < 0)
        return ERR(ENFILE);
    fd_tables[task_idx()][newfd].fd_flags = 0U;
    return (uint64_t)newfd;
}

static uint64_t sys_dup2(uint64_t args[6])
{
    int          oldfd = (int)args[0];
    int          newfd = (int)args[1];
    int          idx = task_idx();
    fd_object_t *obj = fd_get(oldfd);

    if (!obj)
        return ERR(EBADF);
    if (newfd < 0 || newfd >= MAX_FD)
        return ERR(EBADF);
    if (oldfd == newfd)
        return (uint64_t)newfd;

    fd_release_slot(&fd_tables[idx][newfd]);
    if (fd_attach_object(&fd_tables[idx][newfd], obj) < 0)
        return ERR(ENFILE);
    fd_tables[idx][newfd].fd_flags = 0U;
    return (uint64_t)newfd;
}

static uint64_t sys_tcgetattr(uint64_t args[6])
{
    int        fd = (int)args[0];
    uintptr_t  term_uva = (uintptr_t)args[1];
    termios_t  term;
    int        rc;

    if (!term_uva)
        return ERR(EFAULT);
    if (!fd_is_tty_object(fd_get(fd)))
        return ERR(ENOTTY);

    rc = tty_tcgetattr(&term);
    if (rc < 0)
        return ERR(-rc);
    rc = user_store_bytes(term_uva, &term, sizeof(term));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_tcsetattr(uint64_t args[6])
{
    int        fd = (int)args[0];
    int        action = (int)args[1];
    uintptr_t  term_uva = (uintptr_t)args[2];
    termios_t  term;
    int        rc;

    if (!term_uva)
        return ERR(EFAULT);
    if (!fd_is_tty_object(fd_get(fd)))
        return ERR(ENOTTY);

    rc = user_copy_bytes(term_uva, &term, sizeof(term));
    if (rc < 0)
        return ERR(-rc);
    rc = tty_tcsetattr(action, &term);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_kbd_set_layout(uint64_t args[6])
{
    uintptr_t name_uva = (uintptr_t)args[0];
    char      name[32];
    int       rc;

    if (!name_uva)
        return ERR(EFAULT);

    rc = user_copy_cstr(name_uva, name, sizeof(name));
    if (rc < 0)
        return ERR(-rc);
    if (keyboard_set_layout_name(name) < 0)
        return ERR(EINVAL);
    return 0;
}

static uint64_t sys_kbd_get_layout(uint64_t args[6])
{
    uintptr_t buf_uva = (uintptr_t)args[0];
    uint64_t  len = args[1];
    const char *name = keyboard_get_layout_name();
    size_t     copy_len = 0U;
    int        rc;

    if (!buf_uva)
        return ERR(EFAULT);
    if (len == 0ULL)
        return ERR(EINVAL);

    while (name[copy_len] != '\0')
        copy_len++;
    copy_len++;
    if (copy_len > (size_t)len)
        return ERR(ERANGE);

    rc = user_store_bytes(buf_uva, name, copy_len);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)(copy_len - 1U);
}

static uint64_t sys_isatty(uint64_t args[6])
{
    return fd_is_tty_object(fd_get((int)args[0])) ? 1ULL : 0ULL;
}

static uint64_t sys_mount(uint64_t args[6])
{
    uintptr_t src_uva = (uintptr_t)args[0];
    uintptr_t dst_uva = (uintptr_t)args[1];
    uintptr_t fs_uva  = (uintptr_t)args[2];
    uint32_t  flags   = (uint32_t)args[3];
    char      src[VFSD_PATH_BYTES];
    char      dst[VFSD_PATH_BYTES];
    char      fs[32];
    uint32_t  fs_type = VFSD_FS_NONE;
    int       rc;

    if (!dst_uva || !fs_uva)
        return ERR(EFAULT);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (!vfsd_proxy_available())
        return ERR(ENOSYS);

    memset(src, 0, sizeof(src));
    rc = user_copy_cstr(dst_uva, dst, sizeof(dst));
    if (rc < 0)
        return ERR(-rc);
    rc = user_copy_cstr(fs_uva, fs, sizeof(fs));
    if (rc < 0)
        return ERR(-rc);
    if (src_uva != 0U) {
        rc = user_copy_cstr(src_uva, src, sizeof(src));
        if (rc < 0)
            return ERR(-rc);
    }

    rc = sys_mount_parse_fs(fs, flags, &fs_type);
    if (rc < 0)
        return ERR(-rc);

    rc = vfsd_proxy_mount((src_uva != 0U) ? src : NULL, dst, fs_type, flags);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_umount(uint64_t args[6])
{
    uintptr_t path_uva = (uintptr_t)args[0];
    char      path[VFSD_PATH_BYTES];
    int       rc;

    if (!path_uva)
        return ERR(EFAULT);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (!vfsd_proxy_available())
        return ERR(ENOSYS);

    rc = user_copy_cstr(path_uva, path, sizeof(path));
    if (rc < 0)
        return ERR(-rc);

    rc = vfsd_proxy_umount(path);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_pivot_root(uint64_t args[6])
{
    uintptr_t new_uva = (uintptr_t)args[0];
    uintptr_t old_uva = (uintptr_t)args[1];
    char      new_root[VFSD_PATH_BYTES];
    char      old_root[VFSD_PATH_BYTES];
    int       rc;

    if (!new_uva || !old_uva)
        return ERR(EFAULT);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (!vfsd_proxy_available())
        return ERR(ENOSYS);

    rc = user_copy_cstr(new_uva, new_root, sizeof(new_root));
    if (rc < 0)
        return ERR(-rc);
    rc = user_copy_cstr(old_uva, old_root, sizeof(old_root));
    if (rc < 0)
        return ERR(-rc);

    rc = vfsd_proxy_pivot_root(new_root, old_root);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_unshare(uint64_t args[6])
{
    uint32_t flags = (uint32_t)args[0];
    int      rc;

    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (!vfsd_proxy_available())
        return ERR(ENOSYS);

    rc = vfsd_proxy_unshare(flags);
    return (rc < 0) ? ERR(-rc) : 0;
}

static uint64_t sys_mreact_subscribe(uint64_t args[6])
{
    mreact_sub_t    sub;
    mreact_handle_t handle = 0U;
    int             rc;

    sub.addr = (void *)(uintptr_t)args[0];
    sub.size = (size_t)args[1];
    sub.pred = (mreact_pred_t)(uint32_t)args[2];
    sub.value = args[3];
    sub.flags = (uint32_t)args[4];
    sub._pad = 0U;

    rc = mreact_subscribe_current(&sub, 1U, 1, sub.flags, &handle);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)handle;
}

static uint64_t sys_mreact_wait(uint64_t args[6])
{
    int rc = mreact_wait_current((mreact_handle_t)(uint32_t)args[0], args[1]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_mreact_cancel(uint64_t args[6])
{
    int rc = mreact_cancel_current((mreact_handle_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_mreact_subscribe_group(uint64_t args[6], int require_all)
{
    uintptr_t       subs_uva = (uintptr_t)args[0];
    uint32_t        count = (uint32_t)args[1];
    uint32_t        flags = (uint32_t)args[2];
    mreact_sub_t    subs[MREACT_MAX_SUBS];
    mreact_handle_t handle = 0U;
    int             rc;

    if (!subs_uva || count == 0U || count > MREACT_MAX_SUBS)
        return ERR(EINVAL);

    rc = user_copy_bytes(subs_uva, subs, (size_t)count * sizeof(subs[0]));
    if (rc < 0)
        return ERR(-rc);

    rc = mreact_subscribe_current(subs, count, require_all, flags, &handle);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)handle;
}

static uint64_t sys_mreact_subscribe_all(uint64_t args[6])
{
    return sys_mreact_subscribe_group(args, 1);
}

static uint64_t sys_mreact_subscribe_any(uint64_t args[6])
{
    return sys_mreact_subscribe_group(args, 0);
}

static uint64_t sys_ksem_create(uint64_t args[6])
{
    char   name[KSEM_NAME_MAX];
    ksem_t handle = KSEM_INVALID;
    int    rc;

    if ((uintptr_t)args[0] == 0U)
        return ERR(EFAULT);

    rc = user_copy_cstr((uintptr_t)args[0], name, sizeof(name));
    if (rc < 0)
        return ERR(-rc);

    rc = ksem_create_current(name, (uint32_t)args[1], (uint32_t)args[2], &handle);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)handle;
}

static uint64_t sys_ksem_open(uint64_t args[6])
{
    char   name[KSEM_NAME_MAX];
    ksem_t handle = KSEM_INVALID;
    int    rc;

    if ((uintptr_t)args[0] == 0U)
        return ERR(EFAULT);

    rc = user_copy_cstr((uintptr_t)args[0], name, sizeof(name));
    if (rc < 0)
        return ERR(-rc);

    rc = ksem_open_current(name, (uint32_t)args[1], &handle);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)handle;
}

static uint64_t sys_ksem_close(uint64_t args[6])
{
    int rc = ksem_close_current((ksem_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ksem_unlink(uint64_t args[6])
{
    char name[KSEM_NAME_MAX];
    int  rc;

    if ((uintptr_t)args[0] == 0U)
        return ERR(EFAULT);

    rc = user_copy_cstr((uintptr_t)args[0], name, sizeof(name));
    if (rc < 0)
        return ERR(-rc);

    rc = ksem_unlink_current(name);
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ksem_post(uint64_t args[6])
{
    int rc = ksem_post_current((ksem_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ksem_wait(uint64_t args[6])
{
    int rc = ksem_wait_current((ksem_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ksem_timedwait(uint64_t args[6])
{
    int rc = ksem_timedwait_current((ksem_t)(uint32_t)args[0], args[1]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ksem_trywait(uint64_t args[6])
{
    int rc = ksem_trywait_current((ksem_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ksem_getvalue(uint64_t args[6])
{
    int32_t value = 0;
    int     rc;

    if ((uintptr_t)args[1] == 0U)
        return ERR(EFAULT);

    rc = ksem_getvalue_current((ksem_t)(uint32_t)args[0], &value);
    if (rc < 0)
        return ERR(-rc);

    rc = user_store_bytes((uintptr_t)args[1], &value, sizeof(value));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ksem_anon(uint64_t args[6])
{
    ksem_t handle = KSEM_INVALID;
    int    rc;

    rc = ksem_anon_current((uint32_t)args[0], (uint32_t)args[1], &handle);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)handle;
}

static uint64_t sys_kmon_create(uint64_t args[6])
{
    kmon_t handle = KMON_INVALID;
    int    rc;

    rc = kmon_create_current((uint32_t)args[0], (uint32_t)args[1], &handle);
    if (rc < 0)
        return ERR(-rc);
    return (uint64_t)handle;
}

static uint64_t sys_kmon_destroy(uint64_t args[6])
{
    int rc = kmon_destroy_current((kmon_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_kmon_enter(uint64_t args[6])
{
    int rc = kmon_enter_current((kmon_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_kmon_exit(uint64_t args[6])
{
    int rc = kmon_exit_current((kmon_t)(uint32_t)args[0]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_kmon_wait(uint64_t args[6])
{
    int rc = kmon_wait_current((kmon_t)(uint32_t)args[0],
                               (uint8_t)args[1], args[2]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_kmon_signal(uint64_t args[6])
{
    int rc = kmon_signal_current((kmon_t)(uint32_t)args[0],
                                 (uint8_t)args[1]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_kmon_broadcast(uint64_t args[6])
{
    int rc = kmon_broadcast_current((kmon_t)(uint32_t)args[0],
                                    (uint8_t)args[1]);

    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_port_lookup(uint64_t args[6])
{
    char   name[TASK_NAME_LEN];
    port_t *port;
    int     rc;

    if ((uintptr_t)args[0] == 0U)
        return ERR(EFAULT);

    rc = user_copy_cstr((uintptr_t)args[0], name, sizeof(name));
    if (rc < 0)
        return ERR(-rc);

    port = mk_port_lookup(name);
    if (!port)
        return ERR(ENOENT);
    return (uint64_t)port->port_id;
}

static uint64_t sys_ipc_wait(uint64_t args[6])
{
    uintptr_t     msg_uva = (uintptr_t)args[1];
    ipc_message_t msg;
    int           rc;

    if (!msg_uva)
        return ERR(EFAULT);

    rc = mk_ipc_wait((uint32_t)args[0], &msg);
    if (rc < 0)
        return ERR(-rc);

    rc = user_store_bytes(msg_uva, &msg, sizeof(msg));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ipc_poll(uint64_t args[6])
{
    uintptr_t     msg_uva = (uintptr_t)args[1];
    ipc_message_t msg;
    int           rc;

    if (!msg_uva)
        return ERR(EFAULT);

    rc = mk_ipc_poll((uint32_t)args[0], &msg);
    if (rc < 0)
        return ERR(-rc);

    rc = user_store_bytes(msg_uva, &msg, sizeof(msg));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_ipc_reply(uint64_t args[6])
{
    uint32_t port_id = (uint32_t)args[0];
    uint32_t type = (uint32_t)args[1];
    uintptr_t data_uva = (uintptr_t)args[2];
    uint32_t len = (uint32_t)args[3];
    uint8_t  payload[IPC_MSG_MAX_SIZE];
    int      rc;

    if (len > IPC_MSG_MAX_SIZE)
        len = IPC_MSG_MAX_SIZE;
    if (len > 0U) {
        if (!data_uva)
            return ERR(EFAULT);
        rc = user_copy_bytes(data_uva, payload, len);
        if (rc < 0)
            return ERR(-rc);
    }

    rc = mk_ipc_reply(port_id, type, (len > 0U) ? payload : NULL, len);
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_vfs_boot_open(uint64_t args[6])
{
    char       path[VFSD_IO_BYTES];
    vfs_file_t file;
    uint32_t   handle;
    int        rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);
    if ((uintptr_t)args[0] == 0U)
        return ERR(EFAULT);

    rc = user_copy_cstr((uintptr_t)args[0], path, sizeof(path));
    if (rc < 0)
        return ERR(-rc);

    rc = vfs_open(path, (uint32_t)args[1], &file);
    if (rc < 0) {
        uart_puts("[VFSD] boot_open fail: ");
        uart_puts(path);
        uart_puts(" rc=");
        syscall_uart_put_i64((int64_t)rc);
        uart_puts(" flags=");
        syscall_uart_put_u64((uint64_t)(uint32_t)args[1]);
        uart_puts("\n");
        return ERR(-rc);
    }

    handle = vfs_srv_handle_alloc(&file);
    if (handle == 0U) {
        (void)vfs_close(&file);
        return ERR(ENFILE);
    }

    return (uint64_t)handle;
}

static uint64_t sys_vfs_boot_read(uint64_t args[6])
{
    vfs_srv_handle_t *h;
    uintptr_t         buf_uva = (uintptr_t)args[1];
    uint32_t          count = (uint32_t)args[2];
    uint8_t           bounce[VFSD_IO_BYTES];
    ssize_t           rc;
    int               copy_rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);
    if (!buf_uva)
        return ERR(EFAULT);

    h = vfs_srv_handle_get((uint32_t)args[0]);
    if (!h)
        return ERR(EBADF);

    if (count > VFSD_IO_BYTES)
        count = VFSD_IO_BYTES;

    rc = vfs_read(&h->file, bounce, count);
    if (rc < 0)
        return ERR((int)-rc);
    if (rc == 0)
        return 0;

    copy_rc = user_store_bytes(buf_uva, bounce, (size_t)rc);
    if (copy_rc < 0)
        return ERR(-copy_rc);
    return (uint64_t)rc;
}

static uint64_t sys_vfs_boot_write(uint64_t args[6])
{
    vfs_srv_handle_t *h;
    uintptr_t         buf_uva = (uintptr_t)args[1];
    uint32_t          count = (uint32_t)args[2];
    uint8_t           bounce[VFSD_IO_BYTES];
    int               copy_rc;
    ssize_t           rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);
    if (!buf_uva)
        return ERR(EFAULT);

    h = vfs_srv_handle_get((uint32_t)args[0]);
    if (!h)
        return ERR(EBADF);

    if (count > VFSD_IO_BYTES)
        count = VFSD_IO_BYTES;

    copy_rc = user_copy_bytes(buf_uva, bounce, count);
    if (copy_rc < 0)
        return ERR(-copy_rc);

    rc = vfs_write(&h->file, bounce, count);
    if (rc < 0)
        return ERR((int)-rc);
    return (uint64_t)rc;
}

static uint64_t sys_vfs_boot_readdir(uint64_t args[6])
{
    vfs_srv_handle_t *h;
    uintptr_t         buf_uva = (uintptr_t)args[1];
    vfs_dirent_t      ent;
    int               rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);
    if (!buf_uva)
        return ERR(EFAULT);

    h = vfs_srv_handle_get((uint32_t)args[0]);
    if (!h)
        return ERR(EBADF);

    rc = vfs_readdir(&h->file, &ent);
    if (rc < 0)
        return ERR(-rc);

    rc = user_store_bytes(buf_uva, &ent, sizeof(ent));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_vfs_boot_stat(uint64_t args[6])
{
    vfs_srv_handle_t *h;
    uintptr_t         buf_uva = (uintptr_t)args[1];
    stat_t            st;
    int               rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);
    if (!buf_uva)
        return ERR(EFAULT);

    h = vfs_srv_handle_get((uint32_t)args[0]);
    if (!h)
        return ERR(EBADF);

    rc = vfs_stat(&h->file, &st);
    if (rc < 0)
        return ERR(-rc);

    rc = user_store_bytes(buf_uva, &st, sizeof(st));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

static uint64_t sys_vfs_boot_close(uint64_t args[6])
{
    vfs_srv_handle_t *h;
    int               rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);

    h = vfs_srv_handle_get((uint32_t)args[0]);
    if (!h)
        return ERR(EBADF);

    rc = vfs_close(&h->file);
    if (rc < 0)
        return ERR(-rc);

    vfs_srv_handle_free((uint32_t)args[0]);
    return 0;
}

static uint64_t sys_vfs_boot_lseek(uint64_t args[6])
{
    vfs_srv_handle_t *h;
    uint64_t          new_pos = 0ULL;
    int               rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);

    h = vfs_srv_handle_get((uint32_t)args[0]);
    if (!h)
        return ERR(EBADF);

    rc = vfs_file_do_seek(&h->file, (int64_t)args[1], (int)args[2], &new_pos);
    return (rc < 0) ? ERR(-rc) : new_pos;
}

static uint64_t sys_vfs_boot_taskinfo(uint64_t args[6])
{
    uint32_t         pid = (uint32_t)args[0];
    uintptr_t        buf_uva = (uintptr_t)args[1];
    sched_tcb_t     *task;
    vfsd_taskinfo_t  info;
    int              rc;

    if (!vfs_srv_owner_ok())
        return ERR(EPERM);
    if (!pid || !buf_uva)
        return ERR(EFAULT);

    task = sched_task_find(pid);
    if (!task)
        return ERR(ESRCH);

    memset(&info, 0, sizeof(info));
    info.pid = pid;
    info.tgid = sched_task_tgid(task);
    info.parent_pid = sched_task_parent_pid(task);
    info.pgid = sched_task_pgid(task);
    info.sid = sched_task_sid(task);

    rc = user_store_bytes(buf_uva, &info, sizeof(info));
    if (rc < 0)
        return ERR(-rc);
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Block server bootstrap syscall (M9-03 v1)
 *
 * Accesso diretto al driver virtio-blk da parte del server user-space blkd.
 * Solo il processo proprietario della porta IPC "block" può chiamarle.
 * ════════════════════════════════════════════════════════════════════ */

/* SYS_BLK_BOOT_READ (156): args[0]=sector, args[1]=count, args[2]=buf_uva */
static uint64_t sys_blk_boot_read(uint64_t args[6])
{
    uint64_t  sector = args[0];
    uint32_t  count  = (uint32_t)args[1];
    uintptr_t buf_va = (uintptr_t)args[2];
    int       rc;

    if (!blk_srv_owner_ok())
        return ERR(EPERM);
    if (count == 0U || count > BLKD_MAX_SECTORS)
        return ERR(EINVAL);
    if (buf_va == 0U)
        return ERR(EFAULT);

    rc = blk_read_sync(sector, (void *)buf_va, count);
    if (rc != BLK_OK)
        return ERR(EIO);
    return 0;
}

/* SYS_BLK_BOOT_WRITE (157): args[0]=sector, args[1]=count, args[2]=buf_uva */
static uint64_t sys_blk_boot_write(uint64_t args[6])
{
    uint64_t  sector = args[0];
    uint32_t  count  = (uint32_t)args[1];
    uintptr_t buf_va = (uintptr_t)args[2];
    int       rc;

    if (!blk_srv_owner_ok())
        return ERR(EPERM);
    if (count == 0U || count > BLKD_MAX_SECTORS)
        return ERR(EINVAL);
    if (buf_va == 0U)
        return ERR(EFAULT);

    rc = blk_write_sync(sector, (const void *)buf_va, count);
    if (rc != BLK_OK)
        return ERR(EIO);
    return 0;
}

/* SYS_BLK_BOOT_FLUSH (158): nessun argomento */
static uint64_t sys_blk_boot_flush(uint64_t args[6])
{
    int rc;

    (void)args;
    if (!blk_srv_owner_ok())
        return ERR(EPERM);

    rc = blk_flush_sync();
    if (rc != BLK_OK)
        return ERR(EIO);
    return 0;
}

/* SYS_BLK_BOOT_SECTORS (159): ritorna capacity in settori */
static uint64_t sys_blk_boot_sectors(uint64_t args[6])
{
    (void)args;
    if (!blk_srv_owner_ok())
        return ERR(EPERM);
    return blk_sector_count();
}

/* SYS_NET_BOOT_SEND (162): args[0]=buf_uva, args[1]=len */
static uint64_t sys_net_boot_send(uint64_t args[6])
{
    uintptr_t buf_uva = (uintptr_t)args[0];
    uint32_t  len     = (uint32_t)args[1];
    uint8_t   bounce[NET_FRAME_MAX];
    int       rc;

    if (!net_srv_owner_ok())
        return ERR(EPERM);
    if (!buf_uva)
        return ERR(EFAULT);
    if (len == 0U || len > NET_FRAME_MAX)
        return ERR(EINVAL);

    rc = user_copy_bytes(buf_uva, bounce, len);
    if (rc < 0)
        return ERR(-rc);

    rc = net_send(bounce, len);
    if (rc == NET_OK)
        return len;
    if (rc == NET_ERR_BUSY)
        return ERR(EAGAIN);
    if (rc == NET_ERR_TIMEOUT)
        return ERR(ETIMEDOUT);
    if (rc == NET_ERR_NOT_READY)
        return ERR(ENODEV);
    return ERR(EIO);
}

/* SYS_NET_BOOT_RECV (163): args[0]=buf_uva, args[1]=maxlen */
static uint64_t sys_net_boot_recv(uint64_t args[6])
{
    uintptr_t buf_uva = (uintptr_t)args[0];
    uint32_t  maxlen  = (uint32_t)args[1];
    uint8_t   bounce[NET_FRAME_MAX];
    int       rc;

    if (!net_srv_owner_ok())
        return ERR(EPERM);
    if (!buf_uva)
        return ERR(EFAULT);
    if (maxlen == 0U)
        return ERR(EINVAL);
    if (maxlen > NET_FRAME_MAX)
        maxlen = NET_FRAME_MAX;

    rc = net_recv(bounce, maxlen);
    if (rc < 0) {
        if (rc == NET_ERR_NOT_READY)
            return ERR(ENODEV);
        return ERR(EIO);
    }
    if (rc == 0)
        return 0;

    {
        int copied = rc;
        rc = user_store_bytes(buf_uva, bounce, (size_t)copied);
        if (rc < 0)
            return ERR(-rc);
        return (uint64_t)copied;
    }
}

/* SYS_NET_BOOT_INFO (164): args[0]=info_uva */
static uint64_t sys_net_boot_info(uint64_t args[6])
{
    uintptr_t  info_uva = (uintptr_t)args[0];
    net_info_t info;
    int        rc;

    if (!net_srv_owner_ok())
        return ERR(EPERM);
    if (!info_uva)
        return ERR(EFAULT);

    rc = net_get_info(&info);
    if (rc != NET_OK)
        return ERR(ENODEV);

    rc = user_store_bytes(info_uva, &info, sizeof(info));
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
 * Syscall 200–211 — BSD Socket API (M10-03)
 *
 * Conversione NBO←→HBO: sin_port/sin_addr passati dal user sono in
 * network byte order (big-endian). Convertiamo in host order (LE) prima
 * di chiamare il layer sock.c, che opera in HBO internamente.
 * ════════════════════════════════════════════════════════════════════ */

/* Struttura sockaddr_in minima letta dalla user-space */
typedef struct {
    uint16_t sin_family;
    uint16_t sin_port;   /* network byte order */
    uint32_t sin_addr;   /* network byte order */
    uint8_t  sin_zero[8];
} kern_sockaddr_in_t;

static inline uint16_t sock_ntohs(uint16_t v)
{
    return (uint16_t)((v >> 8U) | (v << 8U));
}

static inline uint32_t sock_ntohl(uint32_t v)
{
    return ((v >> 24U) & 0xFFU)
         | ((v >>  8U) & 0xFF00U)
         | ((v <<  8U) & 0xFF0000U)
         | ((v << 24U) & 0xFF000000U);
}

static void sock_sync_fd_flags(fd_object_t *obj)
{
    sock_t *sk;

    if (!obj || obj->type != FD_TYPE_SOCK)
        return;
    if (sock_handle_is_remote_netd(obj->remote_handle))
        return;
    sk = sock_get((int)obj->remote_handle);
    if (!sk)
        return;
    if (obj->flags & O_NONBLOCK)
        sk->flags |= SOCK_FL_NONBLOCK;
    else
        sk->flags &= (uint16_t)~SOCK_FL_NONBLOCK;
}

static fd_object_t *sock_fd_object(int fd, int *out_err)
{
    fd_object_t *obj = fd_get(fd);

    if (!obj) { *out_err = -EBADF; return NULL; }
    if (obj->type != FD_TYPE_SOCK) { *out_err = -ENOTSOCK; return NULL; }
    *out_err = 0;
    return obj;
}

static int sock_fd_is_remote_netd(const fd_object_t *obj)
{
    return obj && obj->type == FD_TYPE_SOCK &&
           sock_handle_is_remote_netd(obj->remote_handle);
}

/* Alloca un fd di tipo SOCK e lo collega a un sock_t appena creato. */
static int sock_open_fd(int sock_idx, uint16_t open_flags, uint8_t fd_flags)
{
    fd_object_t *obj;
    int          fd;
    int          idx = task_idx();

    obj = fd_object_alloc();
    if (!obj) {
        sock_free(sock_idx);
        return -ENFILE;
    }
    obj->type          = FD_TYPE_SOCK;
    obj->remote_handle = (uint32_t)sock_idx;
    obj->flags         = open_flags;

    fd = fd_alloc();
    if (fd < 0) {
        sock_free(sock_idx);
        memset(obj, 0, sizeof(*obj));
        return -ENFILE;
    }
    (void)fd_attach_object(&fd_tables[idx][fd], obj);
    fd_tables[idx][fd].fd_flags = fd_flags;
    sock_sync_fd_flags(obj);
    return fd;
}

static uint64_t sys_socket(uint64_t args[6])
{
    int domain   = (int)args[0];
    int type     = (int)args[1];
    int protocol = (int)args[2];
    int base_type = type & 0xF;
    uint16_t open_flags = 0U;
    uint8_t  fd_flags = 0U;
    int sock_idx, fd;

    if (type & SOCK_NONBLOCK)
        open_flags |= O_NONBLOCK;
    if (type & SOCK_CLOEXEC)
        fd_flags |= FD_ENTRY_CLOEXEC;

    sock_idx = sock_do_socket(domain, base_type, protocol);
    if (sock_idx < 0)
        return ERR(-sock_idx);

    fd = sock_open_fd(sock_idx, open_flags, fd_flags);
    return (fd < 0) ? ERR(-fd) : (uint64_t)fd;
}

static uint64_t sys_bind(uint64_t args[6])
{
    int                fd      = (int)args[0];
    uintptr_t          sa_uva  = (uintptr_t)args[1];
    kern_sockaddr_in_t sa;
    fd_object_t       *obj;
    int                err;
    int                rc;

    obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);

    if (!sa_uva)
        return ERR(EFAULT);
    if (user_copy_bytes(sa_uva, &sa, sizeof(sa)) < 0)
        return ERR(EFAULT);
    if (sa.sin_family != AF_INET)
        return ERR(EAFNOSUPPORT);

    if (sock_fd_is_remote_netd(obj)) {
        rc = netd_proxy_bind(sock_handle_from_remote_netd(obj->remote_handle),
                             sock_ntohl(sa.sin_addr),
                             sock_ntohs(sa.sin_port));
    } else {
        rc = sock_do_bind((int)obj->remote_handle,
                          sock_ntohl(sa.sin_addr),
                          sock_ntohs(sa.sin_port));
    }
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_listen(uint64_t args[6])
{
    int fd      = (int)args[0];
    int backlog = (int)args[1];
    fd_object_t *obj;
    int err, rc;

    obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);
    if (sock_fd_is_remote_netd(obj))
        return ERR(EOPNOTSUPP);

    rc = sock_do_listen((int)obj->remote_handle, backlog);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_accept(uint64_t args[6])
{
    int                fd      = (int)args[0];
    uintptr_t          sa_uva  = (uintptr_t)args[1];
    uintptr_t          len_uva = (uintptr_t)args[2];
    int                err;
    uint32_t           peer_ip   = 0U;
    uint16_t           peer_port = 0U;
    int                new_sock_idx, new_fd;

    fd_object_t *obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);
    if (sock_fd_is_remote_netd(obj))
        return ERR(EOPNOTSUPP);

    new_sock_idx = sock_do_accept((int)obj->remote_handle, &peer_ip, &peer_port);
    if (new_sock_idx < 0)
        return ERR(-new_sock_idx);

    /* Scrivi sockaddr peer se richiesto */
    if (sa_uva) {
        kern_sockaddr_in_t sa;
        sa.sin_family   = AF_INET;
        sa.sin_port     = sock_ntohs(peer_port);
        sa.sin_addr     = sock_ntohl(peer_ip);
        sa.sin_zero[0]  = 0U;
        (void)user_store_bytes(sa_uva, &sa, sizeof(sa));
        if (len_uva) {
            uint32_t slen = (uint32_t)sizeof(sa);
            (void)user_store_bytes(len_uva, &slen, sizeof(slen));
        }
    }

    new_fd = sock_open_fd(new_sock_idx, 0U, 0U);
    return (new_fd < 0) ? ERR(-new_fd) : (uint64_t)new_fd;
}

static uint64_t sys_connect(uint64_t args[6])
{
    int                fd     = (int)args[0];
    uintptr_t          sa_uva = (uintptr_t)args[1];
    kern_sockaddr_in_t sa;
    fd_object_t       *obj;
    sock_t            *sk = NULL;
    uint32_t           dst_ip;
    uint16_t           dst_port;
    int                err, rc;
    int                nonblock;
    uint64_t           deadline_ms = 0ULL;

    obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);

    if (!sa_uva)
        return ERR(EFAULT);
    if (user_copy_bytes(sa_uva, &sa, sizeof(sa)) < 0)
        return ERR(EFAULT);
    if (sa.sin_family != AF_INET)
        return ERR(EAFNOSUPPORT);

    dst_ip = sock_ntohl(sa.sin_addr);
    dst_port = sock_ntohs(sa.sin_port);
    nonblock = ((obj->flags & O_NONBLOCK) != 0U);
    if (!nonblock)
        deadline_ms = timer_now_ms() + 5000ULL;

    if (sock_fd_is_remote_netd(obj)) {
        do {
            rc = netd_proxy_connect(sock_handle_from_remote_netd(obj->remote_handle),
                                    dst_ip, dst_port);
            if (rc != -EINPROGRESS || nonblock)
                break;
            if (timer_now_ms() >= deadline_ms) {
                rc = -ETIMEDOUT;
                break;
            }
            sched_yield();
        } while (1);
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }

    sk = sock_get((int)obj->remote_handle);
    if (!sk)
        return ERR(EBADF);

    if (dst_ip == SOCK_LOOPBACK_IP || dst_ip == SOCK_ANY_IP) {
        rc = sock_do_connect((int)obj->remote_handle, dst_ip, dst_port);
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }

    if (sk->type != SOCK_STREAM)
        return ERR(ENETUNREACH);
    if (!netd_proxy_available())
        return ERR(ENETUNREACH);

    {
        uint32_t remote_handle = 0U;
        uint32_t bind_ip = sk->local_ip;
        uint16_t bind_port = sk->local_port;

        rc = netd_proxy_socket(AF_INET, sk->type, 0, &remote_handle);
        if (rc < 0)
            return ERR(-rc);

        if (bind_ip != 0U || bind_port != 0U) {
            rc = netd_proxy_bind(remote_handle, bind_ip, bind_port);
            if (rc < 0) {
                (void)netd_proxy_close(remote_handle);
                return ERR(-rc);
            }
        }

        sock_free((int)obj->remote_handle);
        obj->remote_handle = sock_handle_to_remote_netd(remote_handle);
    }

    do {
        rc = netd_proxy_connect(sock_handle_from_remote_netd(obj->remote_handle),
                                dst_ip, dst_port);
        if (rc != -EINPROGRESS || nonblock)
            break;
        if (timer_now_ms() >= deadline_ms) {
            rc = -ETIMEDOUT;
            break;
        }
        sched_yield();
    } while (1);

    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_send(uint64_t args[6])
{
    int       fd    = (int)args[0];
    uintptr_t buva  = (uintptr_t)args[1];
    size_t    len   = (size_t)args[2];
    int       flags = (int)args[3];
    void     *bounce;
    fd_object_t *obj;
    int       err;
    ssize_t   rc;
    int       nonblock;

    obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);

    if (!buva || len == 0U)
        return 0ULL;
    if (len > 4096U)
        len = 4096U;

    bounce = kmalloc((uint32_t)len);
    if (!bounce)
        return ERR(ENOMEM);
    if (user_copy_bytes(buva, bounce, len) < 0) {
        kfree(bounce);
        return ERR(EFAULT);
    }

    nonblock = ((obj->flags & O_NONBLOCK) != 0U) || ((flags & MSG_DONTWAIT) != 0);
    if (sock_fd_is_remote_netd(obj)) {
        do {
            rc = netd_proxy_send(sock_handle_from_remote_netd(obj->remote_handle),
                                 bounce, len);
            if (rc != -EAGAIN || nonblock)
                break;
            sched_yield();
        } while (1);
    } else {
        sock_sync_fd_flags(obj);
        rc = sock_do_send((int)obj->remote_handle, bounce, len, flags);
    }
    kfree(bounce);
    return (rc < 0) ? ERR((int)-rc) : (uint64_t)rc;
}

static uint64_t sys_recv(uint64_t args[6])
{
    int       fd    = (int)args[0];
    uintptr_t buva  = (uintptr_t)args[1];
    size_t    len   = (size_t)args[2];
    int       flags = (int)args[3];
    void     *bounce;
    fd_object_t *obj;
    int       err;
    ssize_t   rc;
    int       nonblock;

    obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);

    if (!buva || len == 0U)
        return 0ULL;
    if (len > 4096U)
        len = 4096U;

    bounce = kmalloc((uint32_t)len);
    if (!bounce)
        return ERR(ENOMEM);

    nonblock = ((obj->flags & O_NONBLOCK) != 0U) || ((flags & MSG_DONTWAIT) != 0);
    if (sock_fd_is_remote_netd(obj)) {
        do {
            rc = netd_proxy_recv(sock_handle_from_remote_netd(obj->remote_handle),
                                 bounce, len);
            if (rc != -EAGAIN || nonblock)
                break;
            sched_yield();
        } while (1);
    } else {
        sock_sync_fd_flags(obj);
        rc = sock_do_recv((int)obj->remote_handle, bounce, len, flags);
    }
    if (rc > 0)
        (void)user_store_bytes(buva, bounce, (size_t)rc);
    kfree(bounce);
    return (rc < 0) ? ERR((int)-rc) : (uint64_t)rc;
}

static uint64_t sys_sendto(uint64_t args[6])
{
    int                fd     = (int)args[0];
    uintptr_t          buva   = (uintptr_t)args[1];
    size_t             len    = (size_t)args[2];
    int                flags  = (int)args[3];
    uintptr_t          sa_uva = (uintptr_t)args[4];
    uint32_t           dst_ip   = 0U;
    uint16_t           dst_port = 0U;
    kern_sockaddr_in_t sa;
    void              *bounce;
    int                err;
    ssize_t            rc;

    fd_object_t *obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);
    if (sock_fd_is_remote_netd(obj))
        return ERR(EOPNOTSUPP);

    if (!buva || len == 0U)
        return 0ULL;
    if (len > SOCK_UDP_DATA_MAX)
        len = SOCK_UDP_DATA_MAX;

    if (sa_uva) {
        if (user_copy_bytes(sa_uva, &sa, sizeof(sa)) < 0)
            return ERR(EFAULT);
        dst_ip   = sock_ntohl(sa.sin_addr);
        dst_port = sock_ntohs(sa.sin_port);
    }

    bounce = kmalloc((uint32_t)len);
    if (!bounce)
        return ERR(ENOMEM);
    if (user_copy_bytes(buva, bounce, len) < 0) {
        kfree(bounce);
        return ERR(EFAULT);
    }

    sock_sync_fd_flags(obj);
    rc = sock_do_sendto((int)obj->remote_handle, bounce, len, dst_ip, dst_port, flags);
    kfree(bounce);
    return (rc < 0) ? ERR((int)-rc) : (uint64_t)rc;
}

static uint64_t sys_recvfrom(uint64_t args[6])
{
    int                fd      = (int)args[0];
    uintptr_t          buva    = (uintptr_t)args[1];
    size_t             len     = (size_t)args[2];
    int                flags   = (int)args[3];
    uintptr_t          sa_uva  = (uintptr_t)args[4];
    uintptr_t          len_uva = (uintptr_t)args[5];
    uint32_t           src_ip   = 0U;
    uint16_t           src_port = 0U;
    void              *bounce;
    int                err;
    ssize_t            rc;

    fd_object_t *obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);
    if (sock_fd_is_remote_netd(obj))
        return ERR(EOPNOTSUPP);

    if (!buva || len == 0U)
        return 0ULL;
    if (len > SOCK_UDP_DATA_MAX)
        len = SOCK_UDP_DATA_MAX;

    bounce = kmalloc((uint32_t)len);
    if (!bounce)
        return ERR(ENOMEM);

    sock_sync_fd_flags(obj);
    rc = sock_do_recvfrom((int)obj->remote_handle, bounce, len,
                          &src_ip, &src_port, flags);
    if (rc > 0) {
        (void)user_store_bytes(buva, bounce, (size_t)rc);
        if (sa_uva) {
            kern_sockaddr_in_t sa;
            sa.sin_family  = AF_INET;
            sa.sin_port    = sock_ntohs(src_port);
            sa.sin_addr    = sock_ntohl(src_ip);
            sa.sin_zero[0] = 0U;
            (void)user_store_bytes(sa_uva, &sa, sizeof(sa));
            if (len_uva) {
                uint32_t slen = (uint32_t)sizeof(sa);
                (void)user_store_bytes(len_uva, &slen, sizeof(slen));
            }
        }
    }
    kfree(bounce);
    return (rc < 0) ? ERR((int)-rc) : (uint64_t)rc;
}

static uint64_t sys_setsockopt(uint64_t args[6])
{
    int       fd      = (int)args[0];
    int       level   = (int)args[1];
    int       optname = (int)args[2];
    uintptr_t val_uva = (uintptr_t)args[3];
    socklen_t optlen  = (socklen_t)args[4];
    fd_object_t *obj;
    int       err;
    int       rc;
    uint32_t  val = 0U;

    obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);

    /* Leggi il valore se presente */
    if (val_uva && optlen >= sizeof(uint32_t))
        (void)user_copy_bytes(val_uva, &val, sizeof(val));

    if (sock_fd_is_remote_netd(obj)) {
        rc = netd_proxy_setsockopt(sock_handle_from_remote_netd(obj->remote_handle),
                                   level, optname, &val, sizeof(val));
        return (rc < 0) ? ERR(-rc) : 0ULL;
    }

    sock_t *sk = sock_get((int)obj->remote_handle);
    if (!sk)
        return ERR(EBADF);

    (void)level;
    switch (optname) {
    case SO_REUSEADDR:
        if (val)
            sk->flags |= SOCK_FL_REUSEADDR;
        else
            sk->flags &= (uint16_t)~SOCK_FL_REUSEADDR;
        return 0ULL;
    case SO_KEEPALIVE:
    case SO_SNDBUF:
    case SO_RCVBUF:
    case TCP_NODELAY:
        return 0ULL;   /* accettato, ignorato in v1 */
    default:
        return ERR(ENOPROTOOPT);
    }
}

static uint64_t sys_getsockopt(uint64_t args[6])
{
    int       fd      = (int)args[0];
    int       level   = (int)args[1];
    int       optname = (int)args[2];
    uintptr_t val_uva = (uintptr_t)args[3];
    uintptr_t len_uva = (uintptr_t)args[4];
    fd_object_t *obj;
    int       err;
    int       rc;
    uint32_t  val = 0U;

    obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);

    (void)level;
    if (sock_fd_is_remote_netd(obj)) {
        size_t slen = sizeof(val);
        rc = netd_proxy_getsockopt(sock_handle_from_remote_netd(obj->remote_handle),
                                   level, optname, &val, &slen);
        if (rc < 0)
            return ERR(-rc);
        if (val_uva)
            (void)user_store_bytes(val_uva, &val, slen);
        if (len_uva) {
            uint32_t out_len = (uint32_t)slen;
            (void)user_store_bytes(len_uva, &out_len, sizeof(out_len));
        }
        return 0ULL;
    }

    sock_t *sk = sock_get((int)obj->remote_handle);
    if (!sk)
        return ERR(EBADF);

    switch (optname) {
    case SO_ERROR:
        val = 0U;
        break;
    case SO_REUSEADDR:
        val = (sk->flags & SOCK_FL_REUSEADDR) ? 1U : 0U;
        break;
    default:
        return ERR(ENOPROTOOPT);
    }

    if (val_uva)
        (void)user_store_bytes(val_uva, &val, sizeof(val));
    if (len_uva) {
        uint32_t slen = sizeof(val);
        (void)user_store_bytes(len_uva, &slen, sizeof(slen));
    }
    return 0ULL;
}

static uint64_t sys_shutdown(uint64_t args[6])
{
    int fd  = (int)args[0];
    int how = (int)args[1];
    int err, rc;

    fd_object_t *obj = sock_fd_object(fd, &err);
    if (!obj)
        return ERR(-err);

    if (sock_fd_is_remote_netd(obj))
        rc = netd_proxy_shutdown(sock_handle_from_remote_netd(obj->remote_handle), how);
    else
        rc = sock_do_shutdown((int)obj->remote_handle, how);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

/* ════════════════════════════════════════════════════════════════════
 * SYS_PRLIMIT64 (212) — resource limits nativi
 * args: pid, resource, new_limit_uva, old_limit_uva
 * ════════════════════════════════════════════════════════════════════ */

static uint64_t sys_prlimit64(uint64_t args[6])
{
    uint32_t     pid          = (uint32_t)args[0];
    uint32_t     resource     = (uint32_t)args[1];
    uintptr_t    new_uva      = (uintptr_t)args[2];
    uintptr_t    old_uva      = (uintptr_t)args[3];
    sched_tcb_t *target;
    rlimit64_t   lim;
    int          rc;

    if (resource >= RLIMIT_NLIMITS)
        return ERR(EINVAL);

    /* pid==0 → processo corrente; altrimenti cerca per tgid */
    if (pid == 0U || (current_task && pid == sched_task_tgid(current_task))) {
        target = current_task;
    } else {
        target = sched_task_find(pid);
        if (!target)
            return ERR(ESRCH);
    }

    /* Ritorna vecchio limite se richiesto */
    if (old_uva != 0U) {
        if (sched_proc_get_rlimit(target, resource, &lim) < 0)
            return ERR(EINVAL);
        rc = user_store_bytes(old_uva, &lim, sizeof(lim));
        if (rc < 0)
            return ERR(EFAULT);
    }

    /* Imposta nuovo limite se fornito */
    if (new_uva != 0U) {
        if (user_copy_bytes(new_uva, &lim, sizeof(lim)) < 0)
            return ERR(EFAULT);
        /* soft non può superare hard */
        if (lim.rlim_cur > lim.rlim_max && lim.rlim_max != RLIM64_INFINITY)
            return ERR(EINVAL);
        if (sched_proc_set_rlimit(target, resource, &lim) < 0)
            return ERR(EINVAL);
    }

    return 0ULL;
}

/* SYS_REBOOT (213): cmd = REBOOT_CMD_POWER_OFF / RESTART / HALT */
static uint64_t sys_reboot(uint64_t args[6])
{
    uint32_t cmd = (uint32_t)args[0];

    switch (cmd) {
    case REBOOT_CMD_POWER_OFF:
        shutdown_system(SHUTDOWN_POWEROFF);
        break;
    case REBOOT_CMD_RESTART:
        shutdown_system(SHUTDOWN_REBOOT);
        break;
    case REBOOT_CMD_HALT:
        shutdown_system(SHUTDOWN_HALT);
        break;
    default:
        return ERR(EINVAL);
    }
    /* shutdown_system() è noreturn, ma il compilatore non lo sa qui */
    __builtin_unreachable();
}

/* ════════════════════════════════════════════════════════════════════
 * Linux AArch64 compatibility syscall table (M11-05)
 * ════════════════════════════════════════════════════════════════════ */

static uint64_t sys_linux_passthrough(uint64_t args[6], syscall_handler_fn fn)
{
    return fn ? fn(args) : ERR(ENOSYS);
}

static uint64_t sys_linux_exit(uint64_t args[6])            { return sys_linux_passthrough(args, sys_exit); }
static uint64_t sys_linux_exit_group(uint64_t args[6])      { return sys_linux_passthrough(args, sys_exit_group); }
static uint64_t sys_linux_read(uint64_t args[6])            { return sys_linux_passthrough(args, sys_read); }
static uint64_t sys_linux_write(uint64_t args[6])           { return sys_linux_passthrough(args, sys_write); }
static uint64_t sys_linux_close(uint64_t args[6])           { return sys_linux_passthrough(args, sys_close); }
static uint64_t sys_linux_dup(uint64_t args[6])             { return sys_linux_passthrough(args, sys_dup); }
static uint64_t sys_linux_fcntl(uint64_t args[6])           { return sys_linux_passthrough(args, sys_fcntl); }
static uint64_t sys_linux_lseek(uint64_t args[6])           { return sys_linux_passthrough(args, sys_lseek); }
static uint64_t sys_linux_readv(uint64_t args[6])           { return sys_linux_passthrough(args, sys_readv); }
static uint64_t sys_linux_writev(uint64_t args[6])          { return sys_linux_passthrough(args, sys_writev); }
static uint64_t sys_linux_ioctl(uint64_t args[6])           { return sys_linux_passthrough(args, sys_ioctl); }
static uint64_t sys_linux_getcwd(uint64_t args[6])          { return sys_linux_passthrough(args, sys_getcwd); }
static uint64_t sys_linux_set_tid_address(uint64_t args[6]) { return sys_linux_passthrough(args, sys_set_tid_address); }
static uint64_t sys_linux_futex(uint64_t args[6])           { return sys_linux_passthrough(args, sys_futex); }
static uint64_t sys_linux_nanosleep(uint64_t args[6])       { return sys_linux_passthrough(args, sys_nanosleep); }
static uint64_t sys_linux_clock_gettime(uint64_t args[6])   { return sys_linux_passthrough(args, sys_clock_gettime); }
static uint64_t sys_linux_uname(uint64_t args[6])           { return sys_linux_passthrough(args, sys_uname); }
static uint64_t sys_linux_gettimeofday(uint64_t args[6])    { return sys_linux_passthrough(args, sys_gettimeofday); }
static uint64_t sys_linux_getpid(uint64_t args[6])          { return sys_linux_passthrough(args, sys_getpid); }
static uint64_t sys_linux_getppid(uint64_t args[6])         { return sys_linux_passthrough(args, sys_getppid); }
static uint64_t sys_linux_getuid(uint64_t args[6])          { return sys_linux_passthrough(args, sys_getuid); }
static uint64_t sys_linux_geteuid(uint64_t args[6])         { return sys_linux_passthrough(args, sys_geteuid); }
static uint64_t sys_linux_getgid(uint64_t args[6])          { return sys_linux_passthrough(args, sys_getgid); }
static uint64_t sys_linux_getegid(uint64_t args[6])         { return sys_linux_passthrough(args, sys_getegid); }
static uint64_t sys_linux_gettid(uint64_t args[6])          { return sys_linux_passthrough(args, sys_gettid); }
static uint64_t sys_linux_tgkill(uint64_t args[6])          { return sys_linux_passthrough(args, sys_tgkill); }
static uint64_t sys_linux_chdir(uint64_t args[6])           { return sys_linux_passthrough(args, sys_chdir); }
static uint64_t sys_linux_kill(uint64_t args[6])            { return sys_linux_passthrough(args, sys_kill); }
static uint64_t sys_linux_setpgid(uint64_t args[6])        { return sys_linux_passthrough(args, sys_setpgid); }
static uint64_t sys_linux_setsid(uint64_t args[6])         { return sys_linux_passthrough(args, sys_setsid); }

/* tkill(tid, sig): Linux NR=130 — resolve tgid from tid, then tgkill */
static uint64_t sys_linux_tkill(uint64_t args[6])
{
    uint32_t     tid  = (uint32_t)args[0];
    int          sig  = (int)args[1];
    sched_tcb_t *target;
    uint64_t     fwd[6];

    target = sched_task_find(tid);
    if (!target || !sched_task_is_user(target) || target->state == TCB_STATE_ZOMBIE)
        return ERR(ESRCH);

    fwd[0] = (uint64_t)sched_task_tgid(target);
    fwd[1] = (uint64_t)tid;
    fwd[2] = (uint64_t)sig;
    fwd[3] = 0U; fwd[4] = 0U; fwd[5] = 0U;
    return sys_tgkill(fwd);
}

/* getpgrp(): Linux NR=155 — returns current process group */
static uint64_t sys_linux_getpgrp(uint64_t args[6])
{
    uint64_t zero_args[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
    (void)args;
    return sys_getpgid(zero_args);
}

/* getrlimit(resource, *rlim): Linux NR=163 → prlimit64(0, res, NULL, old) */
static uint64_t sys_linux_getrlimit(uint64_t args[6])
{
    uint64_t fwd[6] = { 0U, args[0], 0U, args[1], 0U, 0U };
    return sys_prlimit64(fwd);
}

/* setrlimit(resource, *rlim): Linux NR=164 → prlimit64(0, res, new, NULL) */
static uint64_t sys_linux_setrlimit(uint64_t args[6])
{
    uint64_t fwd[6] = { 0U, args[0], args[1], 0U, 0U, 0U };
    return sys_prlimit64(fwd);
}
static uint64_t sys_linux_clone(uint64_t args[6])           { return sys_linux_passthrough(args, sys_clone); }
static uint64_t sys_linux_execve(uint64_t args[6])          { return sys_linux_passthrough(args, sys_execve); }
static uint64_t sys_linux_mmap(uint64_t args[6])            { return sys_linux_passthrough(args, sys_mmap); }
static uint64_t sys_linux_munmap(uint64_t args[6])          { return sys_linux_passthrough(args, sys_munmap); }

/* ── mremap(old_addr, old_size, new_size, flags, [new_addr]) ─────── */
static uint64_t sys_linux_mremap(uint64_t args[6])
{
    uintptr_t   old_addr  = (uintptr_t)args[0];
    size_t      old_size  = (size_t)args[1];
    size_t      new_size  = (size_t)args[2];
    uint32_t    flags     = (uint32_t)args[3];
    uintptr_t   fixed_addr = (uintptr_t)args[4];
    mm_space_t *space;
    uintptr_t   new_va = 0U;
    int         rc;

    if (!current_task || !sched_task_is_user(current_task))
        return MAP_FAILED_VA;
    if (new_size == 0U)
        return MAP_FAILED_VA;

    space = sched_task_space(current_task);
    if (!space)
        return MAP_FAILED_VA;

    rc = mmu_remap_user_region(space, old_addr, old_size, new_size,
                               flags, fixed_addr, &new_va);
    return (rc < 0) ? MAP_FAILED_VA : (uint64_t)new_va;
}
static uint64_t sys_linux_brk(uint64_t args[6])             { return sys_linux_passthrough(args, sys_brk); }
static uint64_t sys_linux_socket(uint64_t args[6])          { return sys_linux_passthrough(args, sys_socket); }
static uint64_t sys_linux_bind(uint64_t args[6])            { return sys_linux_passthrough(args, sys_bind); }
static uint64_t sys_linux_listen(uint64_t args[6])          { return sys_linux_passthrough(args, sys_listen); }
static uint64_t sys_linux_accept(uint64_t args[6])          { return sys_linux_passthrough(args, sys_accept); }
static uint64_t sys_linux_connect(uint64_t args[6])         { return sys_linux_passthrough(args, sys_connect); }
static uint64_t sys_linux_sendto(uint64_t args[6])          { return sys_linux_passthrough(args, sys_sendto); }
static uint64_t sys_linux_recvfrom(uint64_t args[6])        { return sys_linux_passthrough(args, sys_recvfrom); }
static uint64_t sys_linux_setsockopt(uint64_t args[6])      { return sys_linux_passthrough(args, sys_setsockopt); }
static uint64_t sys_linux_getsockopt(uint64_t args[6])      { return sys_linux_passthrough(args, sys_getsockopt); }
static uint64_t sys_linux_shutdown(uint64_t args[6])        { return sys_linux_passthrough(args, sys_shutdown); }

static uint64_t sys_linux_sched_yield(uint64_t args[6])
{
    return sys_yield(args);
}

static uint64_t sys_linux_dup3(uint64_t args[6])
{
    int oldfd = (int)args[0];
    int newfd = (int)args[1];
    int flags = (int)args[2];
    uint64_t native_args[6] = { 0 };
    uint64_t rc;

    if ((flags & ~(int)LINUX_O_CLOEXEC) != 0)
        return ERR(EINVAL);
    if (oldfd == newfd)
        return ERR(EINVAL);

    native_args[0] = (uint64_t)oldfd;
    native_args[1] = (uint64_t)newfd;
    rc = sys_dup2(native_args);
    if ((int64_t)rc < 0)
        return rc;
    if (flags & (int)LINUX_O_CLOEXEC)
        fd_entry_set_cloexec(fd_entry_get(newfd), 1);
    return rc;
}

static uint64_t sys_linux_pipe2(uint64_t args[6])
{
    uintptr_t fds_uva = (uintptr_t)args[0];
    uint32_t  flags   = (uint32_t)args[1];
    uint64_t  native_args[6] = { 0 };
    int32_t   fds[2];
    int       rc;

    if ((flags & ~(LINUX_O_CLOEXEC | LINUX_O_NONBLOCK)) != 0U)
        return ERR(EINVAL);

    native_args[0] = (uint64_t)fds_uva;
    if ((int64_t)sys_pipe(native_args) < 0)
        return sys_pipe(native_args);
    rc = user_copy_bytes(fds_uva, fds, sizeof(fds));
    if (rc < 0)
        return ERR(-rc);

    for (uint32_t i = 0U; i < 2U; i++) {
        fd_entry_t  *e = fd_entry_get(fds[i]);
        fd_object_t *obj = e ? e->obj : NULL;

        if (!obj)
            continue;
        if (flags & LINUX_O_NONBLOCK)
            obj->flags |= O_NONBLOCK;
        if (flags & LINUX_O_CLOEXEC)
            fd_entry_set_cloexec(e, 1);
    }
    return 0;
}

static int linux_fd_stat_fill(int fd, linux_stat_t *out)
{
    fd_object_t *obj = fd_get(fd);
    stat_t       st;
    int          rc;

    if (!obj || !out)
        return -EBADF;

    if (obj->type == FD_TYPE_VFSD)
        rc = vfsd_proxy_stat(obj->remote_handle, &st);
    else if (obj->type == FD_TYPE_VFS)
        rc = vfs_stat(&obj->file, &st);
    else if (obj->type == FD_TYPE_PIPE) {
        stat_fill(&st, S_IFIFO | S_IRUSR | S_IWUSR |
                       S_IRGRP | S_IWGRP |
                       S_IROTH | S_IWOTH,
                  0ULL, PIPE_BUF_SIZE);
        rc = 0;
    } else if (obj->type == FD_TYPE_SOCK) {
        stat_fill(&st, S_IFSOCK | S_IRUSR | S_IWUSR, 0ULL, 512U);
        rc = 0;
    } else {
        return -EBADF;
    }

    if (rc < 0)
        return rc;
    linux_stat_from_native(&st, out);
    return 0;
}

static uint64_t sys_linux_fstat(uint64_t args[6])
{
    linux_stat_t st;
    int          rc;

    if ((uintptr_t)args[1] == 0U)
        return ERR(EFAULT);
    rc = linux_fd_stat_fill((int)args[0], &st);
    if (rc < 0)
        return ERR(-rc);
    rc = user_store_bytes((uintptr_t)args[1], &st, sizeof(st));
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_openat(uint64_t args[6])
{
    uint64_t native_args[6] = {
        args[0], args[1], (uint64_t)linux_open_flags_to_native((uint32_t)args[2]),
        args[3], 0U, 0U
    };
    return sys_openat(native_args);
}

static uint64_t sys_linux_mkdirat(uint64_t args[6])
{
    int         dirfd = (int)args[0];
    uintptr_t   path_uva = (uintptr_t)args[1];
    const char *path = (const char *)(uintptr_t)args[1];
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    int         rc;

    if (!path)
        return ERR(EFAULT);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path = path_buf;
    }

    rc = resolve_dirfd_path_meta(dirfd, path, 0, resolved, sizeof(resolved), NULL);
    if (rc < 0)
        return ERR(-rc);
    rc = vfs_mkdir(resolved, (uint32_t)args[2] & 07777U);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_unlinkat(uint64_t args[6])
{
    int         dirfd = (int)args[0];
    uintptr_t   path_uva = (uintptr_t)args[1];
    const char *path = (const char *)(uintptr_t)args[1];
    uint32_t    flags = (uint32_t)args[2];
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    stat_t      st;
    int         rc;

    if (!path)
        return ERR(EFAULT);
    if ((flags & ~LINUX_AT_REMOVEDIR) != 0U)
        return ERR(EINVAL);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path = path_buf;
    }

    rc = resolve_dirfd_path_meta(dirfd, path, 0, resolved, sizeof(resolved), NULL);
    if (rc < 0)
        return ERR(-rc);
    rc = linux_path_lstat_fill(resolved, &st);
    if (rc < 0)
        return ERR(-rc);
    if ((flags & LINUX_AT_REMOVEDIR) != 0U) {
        if ((st.st_mode & S_IFMT) != S_IFDIR)
            return ERR(ENOTDIR);
    } else if ((st.st_mode & S_IFMT) == S_IFDIR) {
        return ERR(EISDIR);
    }
    rc = vfs_unlink(resolved);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_symlinkat(uint64_t args[6])
{
    uintptr_t   target_uva = (uintptr_t)args[0];
    int         dirfd = (int)args[1];
    uintptr_t   link_uva = (uintptr_t)args[2];
    const char *target = (const char *)(uintptr_t)args[0];
    const char *linkpath = (const char *)(uintptr_t)args[2];
    char        target_buf[VFSD_IO_BYTES];
    char        link_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    int         rc;

    if (!target || !linkpath)
        return ERR(EFAULT);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(target_uva, target_buf, sizeof(target_buf));
        if (rc < 0)
            return ERR(-rc);
        rc = user_copy_cstr(link_uva, link_buf, sizeof(link_buf));
        if (rc < 0)
            return ERR(-rc);
        target = target_buf;
        linkpath = link_buf;
    }

    rc = resolve_dirfd_path_meta(dirfd, linkpath, 0, resolved, sizeof(resolved), NULL);
    if (rc < 0)
        return ERR(-rc);
    rc = vfs_symlink(target, resolved);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_readlinkat(uint64_t args[6])
{
    int         dirfd = (int)args[0];
    uintptr_t   path_uva = (uintptr_t)args[1];
    const char *path = (const char *)(uintptr_t)args[1];
    uintptr_t   buf_uva = (uintptr_t)args[2];
    size_t      buflen = (size_t)args[3];
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    char        target[VFSD_IO_BYTES];
    size_t      len = 0U;
    int         rc;

    if (!path || !buf_uva || buflen == 0U)
        return ERR(EFAULT);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path = path_buf;
    }

    rc = resolve_dirfd_path_meta(dirfd, path, 0, resolved, sizeof(resolved), NULL);
    if (rc < 0)
        return ERR(-rc);
    rc = linux_path_readlink(resolved, target, sizeof(target));
    if (rc < 0)
        rc = vfs_readlink(resolved, target, sizeof(target));
    if (rc < 0)
        return ERR(-rc);

    while (target[len] != '\0')
        len++;
    if (len > buflen)
        len = buflen;
    rc = user_store_bytes(buf_uva, target, len);
    return (rc < 0) ? ERR(-rc) : (uint64_t)len;
}

static uint64_t sys_linux_newfstatat(uint64_t args[6])
{
    int         dirfd = (int)args[0];
    uintptr_t   path_uva = (uintptr_t)args[1];
    const char *path = (const char *)(uintptr_t)args[1];
    uint32_t    flags = (uint32_t)args[3];
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    stat_t      native_st;
    linux_stat_t st;
    int         rc;

    if ((uintptr_t)args[2] == 0U)
        return ERR(EFAULT);
    if (flags & ~(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_NOFOLLOW))
        return ERR(EINVAL);

    if (path) {
        if (current_task && sched_task_is_user(current_task)) {
            rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
            if (rc < 0)
                return ERR(-rc);
            path = path_buf;
        }
    } else {
        path = "";
    }

    rc = resolve_dirfd_path_meta(dirfd, path,
                                 (flags & LINUX_AT_EMPTY_PATH) != 0U,
                                 resolved, sizeof(resolved), NULL);
    if (rc < 0)
        return ERR(-rc);

    rc = path_stat_fill_native(resolved, (flags & LINUX_AT_SYMLINK_NOFOLLOW) != 0U,
                               &native_st);
    if (rc < 0)
        return ERR(-rc);
    linux_stat_from_native(&native_st, &st);
    rc = user_store_bytes((uintptr_t)args[2], &st, sizeof(st));
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_getdents64(uint64_t args[6])
{
    int          fd = (int)args[0];
    uintptr_t    buf_uva = (uintptr_t)args[1];
    uint32_t     count = (uint32_t)args[2];
    fd_object_t *obj = fd_get(fd);
    uint32_t     copied = 0U;

    if (!obj)
        return ERR(EBADF);
    if (!buf_uva)
        return ERR(EFAULT);
    if (count < 24U)
        return 0ULL;

    while (copied + 24U <= count) {
        vfs_dirent_t      ent;
        linux_dirent64_t  out;
        size_t            namelen = 0U;
        size_t            reclen;
        int               rc;

        if (obj->type == FD_TYPE_VFSD)
            rc = vfsd_proxy_readdir(obj->remote_handle, &ent);
        else if (obj->type == FD_TYPE_VFS)
            rc = vfs_readdir(&obj->file, &ent);
        else
            return ERR(EBADF);

        if (rc == -ENOENT)
            break;
        if (rc < 0)
            return ERR(-rc);

        memset(&out, 0, sizeof(out));
        while (namelen + 1U < sizeof(out.d_name) && ent.name[namelen] != '\0') {
            out.d_name[namelen] = ent.name[namelen];
            namelen++;
        }
        out.d_name[namelen] = '\0';
        reclen = (19U + namelen + 1U + 7U) & ~7U;
        if (copied + reclen > count)
            break;

        out.d_ino = (uint64_t)(obj->file.dir_index + 1U);
        out.d_off = (int64_t)obj->file.dir_index;
        out.d_reclen = (uint16_t)reclen;
        out.d_type = linux_dtype_from_mode(ent.mode);

        rc = user_store_bytes(buf_uva + copied, &out, reclen);
        if (rc < 0)
            return ERR(-rc);
        copied += (uint32_t)reclen;
    }

    return copied;
}

static uint64_t sys_linux_wait4(uint64_t args[6])
{
    uint64_t native_args[6] = { args[0], args[1], args[2], 0U, 0U, 0U };
    uint64_t rc = sys_waitpid(native_args);

    if ((int64_t)rc < 0)
        return rc;
    if ((uintptr_t)args[3] != 0U) {
        linux_rusage_t ru;
        int store_rc;

        linux_fill_rusage(&ru);
        store_rc = user_store_bytes((uintptr_t)args[3], &ru, sizeof(ru));
        if (store_rc < 0)
            return ERR(-store_rc);
    }
    return rc;
}

static uint64_t sys_linux_fsync(uint64_t args[6])
{
    fd_object_t *obj = fd_get((int)args[0]);
    int          rc;

    if (!obj)
        return ERR(EBADF);
    if (obj->type != FD_TYPE_VFS && obj->type != FD_TYPE_VFSD)
        return ERR(EINVAL);
    if (obj->file.mount == NULL)
        return 0ULL;
    rc = vfs_fsync(&obj->file);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_truncate(uint64_t args[6])
{
    uintptr_t   path_uva = (uintptr_t)args[0];
    const char *path = (const char *)(uintptr_t)args[0];
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    int         rc;

    if (!path)
        return ERR(EFAULT);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path = path_buf;
    }

    rc = resolve_user_vfs_path(path, resolved, sizeof(resolved));
    if (rc < 0)
        return ERR(-rc);
    rc = vfs_truncate(resolved, args[1]);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_ftruncate(uint64_t args[6])
{
    fd_object_t *obj = fd_get((int)args[0]);
    int          rc;

    if (!obj)
        return ERR(EBADF);
    if ((obj->type != FD_TYPE_VFS && obj->type != FD_TYPE_VFSD) ||
        obj->path[0] == '\0')
        return ERR(EINVAL);
    rc = vfs_truncate(obj->path, args[1]);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_faccessat(uint64_t args[6])
{
    int         dirfd = (int)args[0];
    uintptr_t   path_uva = (uintptr_t)args[1];
    const char *path = (const char *)(uintptr_t)args[1];
    char        path_buf[EXEC_MAX_PATH];
    char        resolved[VFSD_IO_BYTES];
    stat_t      st;
    uint32_t    mode = (uint32_t)args[2];
    int         rc;

    if (!path)
        return ERR(EFAULT);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path = path_buf;
    }

    rc = resolve_dirfd_path_meta(dirfd, path, 0, resolved, sizeof(resolved), NULL);
    if (rc < 0)
        return ERR(-rc);
    rc = path_stat_fill_native(resolved, 0, &st);
    if (rc < 0)
        return ERR(-rc);
    if ((mode & LINUX_W_OK) && (st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0U)
        return ERR(EACCES);
    if ((mode & LINUX_R_OK) && (st.st_mode & (S_IRUSR | S_IRGRP | S_IROTH)) == 0U)
        return ERR(EACCES);
    if ((mode & LINUX_X_OK) && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0U)
        return ERR(EACCES);
    return 0ULL;
}

/* ── fchmod(fd, mode): no permission model, return 0 ──────────────── */
static uint64_t sys_linux_fchmod(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

/* ── fchmodat(dirfd, path, mode, flags): no permission model, return 0 */
static uint64_t sys_linux_fchmodat(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

/* ── fchownat(dirfd, path, uid, gid, flags): no ownership model, return 0 */
static uint64_t sys_linux_fchownat(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

/* ── sched_getaffinity(pid, cpusetsize, mask): stub, 1 CPU ────────── */
static uint64_t sys_linux_sched_getaffinity(uint64_t args[6])
{
    uintptr_t  mask_uva  = (uintptr_t)args[2];
    uint64_t   setsize   = args[1];
    uint8_t    buf[128];
    uint64_t   write_sz;
    int        rc;

    if (setsize == 0U)
        return ERR(EINVAL);
    write_sz = (setsize < sizeof(buf)) ? setsize : sizeof(buf);
    memset(buf, 0, (size_t)write_sz);
    buf[0] = 0x01U;  /* CPU 0 only */
    if (mask_uva != 0U) {
        rc = user_store_bytes(mask_uva, buf, (uint32_t)write_sz);
        if (rc < 0)
            return ERR(-rc);
    }
    return 0ULL;
}

/* ── sched_get_priority_max/min: stub (SCHED_OTHER → 0) ──────────── */
static uint64_t sys_linux_sched_get_priority_max(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

static uint64_t sys_linux_sched_get_priority_min(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

/* ── getrusage(who, *rusage): zeroed struct ───────────────────────── */
static uint64_t sys_linux_getrusage(uint64_t args[6])
{
    uintptr_t      ru_uva = (uintptr_t)args[1];
    linux_rusage_t ru;
    int            rc;

    if (ru_uva == 0U)
        return ERR(EFAULT);
    linux_fill_rusage(&ru);
    rc = user_store_bytes(ru_uva, &ru, sizeof(ru));
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

/* ── umask(mask): stub, return 022, no tracking ───────────────────── */
static uint64_t sys_linux_umask(uint64_t args[6])
{
    (void)args;
    return 022ULL;
}

/* ── madvise(addr, length, advice): hint only, return 0 ──────────── */
static uint64_t sys_linux_madvise(uint64_t args[6])
{
    (void)args;
    return 0ULL;
}

/* ── faccessat2(dirfd, path, mode, flags): delegate to faccessat ─── */
static uint64_t sys_linux_faccessat2(uint64_t args[6])
{
    /* args[3] = flags (AT_EACCESS etc.) — ignore for v1 */
    uint64_t fwd[6] = { args[0], args[1], args[2], 0U, 0U, 0U };
    return sys_linux_faccessat(fwd);
}

static uint64_t sys_linux_flock(uint64_t args[6])
{
    fd_object_t *obj = fd_get((int)args[0]);
    uint32_t     op = (uint32_t)args[1];
    uint32_t     mode = op & (uint32_t)~LINUX_LOCK_NB;
    int          nonblock = (op & LINUX_LOCK_NB) != 0U;
    flock_entry_t  *entry;
    flock_waiter_t *waiter;
    int             held_idx;
    int          rc;

    if (!obj)
        return ERR(EBADF);
    if (obj->type != FD_TYPE_VFS && obj->type != FD_TYPE_VFSD)
        return ERR(EINVAL);

    switch (mode) {
    case LINUX_LOCK_UN:
        rc = flock_unlock_object(obj);
        return (rc < 0) ? ERR(-rc) : 0ULL;
    case LINUX_LOCK_SH:
    case LINUX_LOCK_EX:
        break;
    default:
        return ERR(EINVAL);
    }

    rc = flock_try_lock_object(obj, mode);
    if (rc == 0)
        return 0ULL;
    if (rc != -EAGAIN)
        return ERR(-rc);
    if (nonblock)
        return ERR(EAGAIN);

    entry = flock_find_entry(obj);
    if (!entry)
        return ERR(EAGAIN);

    if (flock_detect_deadlock(obj, entry))
        return ERR(EDEADLK);

    held_idx = flock_holder_index(entry, obj);
    if (mode == LINUX_LOCK_EX && held_idx >= 0 && entry->holder_count > 1U)
        flock_drop_holder(entry, (uint32_t)held_idx);

    waiter = flock_waiter_alloc(current_task, obj, entry,
                                mode == LINUX_LOCK_EX);
    if (!waiter)
        return ERR(EBUSY);

    flock_waitq_push_tail(entry, waiter);

    for (;;) {
        if (waiter->wake_reason == FLOCK_WAKE_LOCK) {
            flock_waiter_reset(waiter);
            return 0ULL;
        }
        if (waiter->wake_reason == FLOCK_WAKE_INTR) {
            flock_waiter_reset(waiter);
            return ERR(EINTR);
        }
        if (signal_has_unblocked_pending(current_task)) {
            if (waiter->active)
                flock_waitq_remove(entry, waiter);
            flock_waiter_reset(waiter);
            return ERR(EINTR);
        }
        sched_yield();
    }
}

static uint64_t sys_linux_mprotect(uint64_t args[6])
{
    uintptr_t   addr = (uintptr_t)args[0];
    size_t      len = (size_t)args[1];
    mm_space_t *space;

    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EINVAL);
    if (len == 0U)
        return 0ULL;

    space = sched_task_space(current_task);
    if (!space)
        return ERR(EINVAL);
    if (!mmu_space_resolve_ptr(space, addr & PAGE_MASK, 1U))
        return ERR(EINVAL);
    (void)args[2];
    return 0ULL;
}

static uint64_t sys_linux_prlimit64(uint64_t args[6])
{
    /* Delega alla syscall nativa (stesso ABI: pid, resource, new, old) */
    return sys_prlimit64(args);
}

static uint64_t sys_linux_sysinfo(uint64_t args[6])
{
    linux_sysinfo_t info;
    int             rc;

    if ((uintptr_t)args[0] == 0U)
        return ERR(EFAULT);
    linux_fill_sysinfo(&info);
    rc = user_store_bytes((uintptr_t)args[0], &info, sizeof(info));
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static uint64_t sys_linux_getrandom(uint64_t args[6])
{
    uintptr_t buf_uva = (uintptr_t)args[0];
    size_t    len = (size_t)args[1];
    void     *bounce;
    int       rc;

    (void)args[2];
    if (!buf_uva)
        return ERR(EFAULT);
    if (len == 0U)
        return 0ULL;
    if (len > 4096U)
        len = 4096U;

    bounce = kmalloc((uint32_t)len);
    if (!bounce)
        return ERR(ENOMEM);
    linux_random_fill(bounce, len);
    rc = user_store_bytes(buf_uva, bounce, len);
    kfree(bounce);
    return (rc < 0) ? ERR(-rc) : (uint64_t)len;
}

static uint64_t sys_linux_rseq(uint64_t args[6])
{
    (void)args;
    return ERR(ENOSYS);
}

static uint16_t fd_poll_ready_mask(fd_object_t *obj)
{
    uint16_t mask = 0U;

    if (!obj)
        return LINUX_POLLNVAL;

    if (fd_is_tty_object(obj)) {
        if (tty_has_input())
            mask |= LINUX_POLLIN;
        mask |= LINUX_POLLOUT;
        return mask;
    }

    if (obj->type == FD_TYPE_VFS || obj->type == FD_TYPE_VFSD) {
        mask |= LINUX_POLLIN | LINUX_POLLOUT;
        return mask;
    }

    if (obj->type == FD_TYPE_PIPE) {
        pipe_t *pipe = (pipe_t *)obj->pipe;

        if (!pipe || !pipe->in_use)
            return LINUX_POLLNVAL;
        if (obj->pipe_end == PIPE_END_READ) {
            if (pipe->size > 0U)
                mask |= LINUX_POLLIN;
            if (pipe->writers == 0U)
                mask |= LINUX_POLLHUP;
        } else {
            if (pipe->readers == 0U)
                mask |= LINUX_POLLERR;
            else if (pipe->size < PIPE_BUF_SIZE)
                mask |= LINUX_POLLOUT;
        }
        return mask;
    }

    if (obj->type == FD_TYPE_SOCK) {
        if (sock_fd_is_remote_netd(obj)) {
            uint32_t remote_mask = 0U;
            int      rc = netd_proxy_poll(sock_handle_from_remote_netd(obj->remote_handle),
                                          &remote_mask);

            if (rc < 0)
                return LINUX_POLLERR;
            mask |= (uint16_t)(remote_mask & 0xFFFFU);
            return mask;
        }
        sock_t *sk = sock_get((int)obj->remote_handle);

        if (!sk)
            return LINUX_POLLNVAL;
        if (sk->type == SOCK_STREAM) {
            if (sk->state == SOCK_STATE_LISTENING) {
                if (sk->accept_head != sk->accept_tail)
                    mask |= LINUX_POLLIN;
            } else if (sk->state == SOCK_STATE_CONNECTED || sk->state == SOCK_STATE_CLOSE_WAIT) {
                if (sk->rx_tail != sk->rx_head)
                    mask |= LINUX_POLLIN;
                if ((sk->flags & SOCK_FL_PEER_CLOSE) != 0U)
                    mask |= LINUX_POLLHUP;
                if ((sk->flags & SOCK_FL_PEER_CLOSE) == 0U)
                    mask |= LINUX_POLLOUT;
            }
        } else if (sk->type == SOCK_DGRAM) {
            if (sk->udp_head != sk->udp_tail)
                mask |= LINUX_POLLIN;
            mask |= LINUX_POLLOUT;
        }
        return mask;
    }

    return LINUX_POLLNVAL;
}

static uint16_t fd_poll_revents(fd_object_t *obj, uint16_t requested)
{
    uint16_t ready = fd_poll_ready_mask(obj);
    uint16_t base = (uint16_t)(requested & (LINUX_POLLIN | LINUX_POLLPRI | LINUX_POLLOUT));

    ready &= (uint16_t)(base | LINUX_POLLERR | LINUX_POLLHUP | LINUX_POLLNVAL);
    if (ready == 0U && (requested == 0U))
        ready = fd_poll_ready_mask(obj);
    return ready;
}

static uint64_t sys_linux_ppoll(uint64_t args[6])
{
    uintptr_t       pfds_uva = (uintptr_t)args[0];
    uint32_t        nfds = (uint32_t)args[1];
    uintptr_t       tsp_uva = (uintptr_t)args[2];
    linux_pollfd_t  pfds[MAX_FD];
    timespec_t      ts;
    uint64_t        deadline = (uint64_t)-1ULL;
    int             ready;
    int             rc;

    (void)args[3];

    if (!pfds_uva && nfds != 0U)
        return ERR(EFAULT);
    if (nfds > MAX_FD)
        return ERR(EINVAL);
    if (nfds > 0U) {
        rc = user_copy_bytes(pfds_uva, pfds, (size_t)nfds * sizeof(pfds[0]));
        if (rc < 0)
            return ERR(-rc);
    }
    if (tsp_uva != 0U) {
        rc = user_copy_bytes(tsp_uva, &ts, sizeof(ts));
        if (rc < 0)
            return ERR(-rc);
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
            return ERR(EINVAL);
        deadline = timer_now_ns() + (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    for (;;) {
        ready = 0;
        for (uint32_t i = 0U; i < nfds; i++) {
            fd_object_t *obj = fd_get(pfds[i].fd);

            pfds[i].revents = (int16_t)fd_poll_revents(obj, (uint16_t)pfds[i].events);
            if (pfds[i].revents != 0)
                ready++;
        }
        if (ready > 0)
            break;
        if (signal_has_unblocked_pending(current_task))
            return ERR(EINTR);
        if (deadline != (uint64_t)-1ULL && timer_now_ns() >= deadline)
            break;
        if (tsp_uva == 0U && nfds == 0U)
            break;
        sched_yield();
    }

    if (nfds > 0U) {
        rc = user_store_bytes(pfds_uva, pfds, (size_t)nfds * sizeof(pfds[0]));
        if (rc < 0)
            return ERR(-rc);
    }
    return (uint64_t)(ready > 0 ? ready : 0);
}

static uint64_t sys_linux_pselect6(uint64_t args[6])
{
    uintptr_t read_uva  = (uintptr_t)args[1];
    uintptr_t write_uva = (uintptr_t)args[2];
    uintptr_t except_uva = (uintptr_t)args[3];
    uintptr_t tsp_uva   = (uintptr_t)args[4];
    uint32_t  nfds      = (uint32_t)args[0];
    uint64_t  read_set[16];
    uint64_t  write_set[16];
    uint64_t  except_set[16];
    uint64_t  out_read[16];
    uint64_t  out_write[16];
    uint64_t  out_except[16];
    uint32_t  words;
    timespec_t ts;
    uint64_t  deadline = (uint64_t)-1ULL;
    int       ready = 0;
    int       rc;

    (void)args[5];

    if (nfds > 1024U)
        return ERR(EINVAL);
    words = (nfds + 63U) / 64U;
    if (words > 16U)
        words = 16U;

    memset(read_set, 0, sizeof(read_set));
    memset(write_set, 0, sizeof(write_set));
    memset(except_set, 0, sizeof(except_set));
    memset(out_read, 0, sizeof(out_read));
    memset(out_write, 0, sizeof(out_write));
    memset(out_except, 0, sizeof(out_except));

    if (read_uva) {
        rc = user_copy_bytes(read_uva, read_set, (size_t)words * sizeof(uint64_t));
        if (rc < 0)
            return ERR(-rc);
    }
    if (write_uva) {
        rc = user_copy_bytes(write_uva, write_set, (size_t)words * sizeof(uint64_t));
        if (rc < 0)
            return ERR(-rc);
    }
    if (except_uva) {
        rc = user_copy_bytes(except_uva, except_set, (size_t)words * sizeof(uint64_t));
        if (rc < 0)
            return ERR(-rc);
    }
    if (tsp_uva != 0U) {
        rc = user_copy_bytes(tsp_uva, &ts, sizeof(ts));
        if (rc < 0)
            return ERR(-rc);
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
            return ERR(EINVAL);
        deadline = timer_now_ns() + (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    }

    for (;;) {
        ready = 0;
        memset(out_read, 0, sizeof(out_read));
        memset(out_write, 0, sizeof(out_write));
        memset(out_except, 0, sizeof(out_except));

        for (uint32_t fd = 0U; fd < nfds && fd < MAX_FD; fd++) {
            fd_object_t *obj;
            uint16_t     mask;
            uint32_t     word = fd / 64U;
            uint64_t     bit = 1ULL << (fd % 64U);

            if (((read_set[word] | write_set[word] | except_set[word]) & bit) == 0ULL)
                continue;

            obj = fd_get((int)fd);
            mask = fd_poll_ready_mask(obj);
            if ((read_set[word] & bit) && (mask & (LINUX_POLLIN | LINUX_POLLHUP))) {
                out_read[word] |= bit;
                ready++;
            }
            if ((write_set[word] & bit) && (mask & LINUX_POLLOUT)) {
                out_write[word] |= bit;
                ready++;
            }
            if ((except_set[word] & bit) && (mask & (LINUX_POLLERR | LINUX_POLLNVAL))) {
                out_except[word] |= bit;
                ready++;
            }
        }

        if (ready > 0)
            break;
        if (signal_has_unblocked_pending(current_task))
            return ERR(EINTR);
        if (deadline != (uint64_t)-1ULL && timer_now_ns() >= deadline)
            break;
        if (tsp_uva == 0U && nfds == 0U)
            break;
        sched_yield();
    }

    if (read_uva) {
        rc = user_store_bytes(read_uva, out_read, (size_t)words * sizeof(uint64_t));
        if (rc < 0)
            return ERR(-rc);
    }
    if (write_uva) {
        rc = user_store_bytes(write_uva, out_write, (size_t)words * sizeof(uint64_t));
        if (rc < 0)
            return ERR(-rc);
    }
    if (except_uva) {
        rc = user_store_bytes(except_uva, out_except, (size_t)words * sizeof(uint64_t));
        if (rc < 0)
            return ERR(-rc);
    }
    return (uint64_t)ready;
}

static uint64_t sys_linux_rt_sigaction(uint64_t args[6])
{
    int                sig = (int)args[0];
    uintptr_t          act_uva = (uintptr_t)args[1];
    uintptr_t          old_uva = (uintptr_t)args[2];
    uint64_t           sigsetsize = args[3];
    linux_sigaction_t  lsa;
    linux_sigaction_t  lold;
    sigaction_t        act;
    sigaction_t        old;
    sigaction_t       *act_ptr = NULL;
    sigaction_t       *old_ptr = NULL;
    int                rc;

    if (sigsetsize != sizeof(uint64_t))
        return ERR(EINVAL);

    if (act_uva != 0U) {
        rc = user_copy_bytes(act_uva, &lsa, sizeof(lsa));
        if (rc < 0)
            return ERR(-rc);
        act.sa_handler = (sighandler_t)(uintptr_t)lsa.sa_handler;
        act.sa_flags   = (uint32_t)lsa.sa_flags;
        act.sa_mask    = lsa.sa_mask;
        act._pad       = 0U;
        act_ptr = &act;
    }
    if (old_uva != 0U)
        old_ptr = &old;

    rc = signal_sigaction_current(sig, act_ptr, old_ptr);
    if (rc < 0)
        return ERR(-rc);
    if (old_ptr) {
        memset(&lold, 0, sizeof(lold));
        lold.sa_handler = (uintptr_t)old.sa_handler;
        lold.sa_flags   = old.sa_flags;
        lold.sa_mask    = old.sa_mask;
        rc = user_store_bytes(old_uva, &lold, sizeof(lold));
        if (rc < 0)
            return ERR(-rc);
    }
    return 0ULL;
}

static uint64_t sys_linux_rt_sigprocmask(uint64_t args[6])
{
    int        how = (int)args[0];
    uintptr_t  set_uva = (uintptr_t)args[1];
    uintptr_t  old_uva = (uintptr_t)args[2];
    uint64_t   sigsetsize = args[3];
    uint64_t   set_mask = 0ULL;
    uint64_t   old_mask = 0ULL;
    uint64_t  *set_ptr = NULL;
    uint64_t  *old_ptr = NULL;
    int        rc;

    if (sigsetsize != sizeof(uint64_t))
        return ERR(EINVAL);
    if (set_uva != 0U) {
        rc = user_copy_bytes(set_uva, &set_mask, sizeof(set_mask));
        if (rc < 0)
            return ERR(-rc);
        set_ptr = &set_mask;
    }
    if (old_uva != 0U)
        old_ptr = &old_mask;

    rc = signal_sigprocmask_current(how, set_ptr, old_ptr);
    if (rc < 0)
        return ERR(-rc);
    if (old_ptr) {
        rc = user_store_bytes(old_uva, &old_mask, sizeof(old_mask));
        if (rc < 0)
            return ERR(-rc);
    }
    return 0ULL;
}

static uint64_t sys_linux_rt_sigreturn(uint64_t args[6])
{
    return sys_sigreturn(args);
}

static uint64_t sys_linux_getsockname_common(uint64_t args[6], int peer)
{
    int                fd = (int)args[0];
    uintptr_t          sa_uva = (uintptr_t)args[1];
    uintptr_t          len_uva = (uintptr_t)args[2];
    fd_object_t       *obj = fd_get(fd);
    sock_t            *sk;
    kern_sockaddr_in_t sa;
    uint32_t           slen = sizeof(sa);
    uint32_t           addr_ip = 0U;
    uint16_t           addr_port = 0U;
    int                rc;

    if (!obj || obj->type != FD_TYPE_SOCK)
        return ERR(ENOTSOCK);
    if (!sa_uva || !len_uva)
        return ERR(EFAULT);

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    if (sock_fd_is_remote_netd(obj)) {
        rc = netd_proxy_addr(sock_handle_from_remote_netd(obj->remote_handle),
                             peer, &addr_ip, &addr_port);
        if (rc < 0)
            return ERR(-rc);
    } else {
        sk = sock_get((int)obj->remote_handle);
        if (!sk)
            return ERR(EBADF);
        addr_ip = peer ? sk->peer_ip : sk->local_ip;
        addr_port = peer ? sk->peer_port : sk->local_port;
    }
    sa.sin_port = sock_ntohs(addr_port);
    sa.sin_addr = sock_ntohl(addr_ip);
    if (user_store_bytes(sa_uva, &sa, sizeof(sa)) < 0)
        return ERR(EFAULT);
    if (user_store_bytes(len_uva, &slen, sizeof(slen)) < 0)
        return ERR(EFAULT);
    return 0ULL;
}

static uint64_t sys_linux_getsockname(uint64_t args[6])
{
    return sys_linux_getsockname_common(args, 0);
}

static uint64_t sys_linux_getpeername(uint64_t args[6])
{
    return sys_linux_getsockname_common(args, 1);
}

static uint64_t sys_linux_renameat(uint64_t args[6])
{
    int         old_dirfd = (int)args[0];
    int         new_dirfd = (int)args[2];
    const char *old_path = (const char *)(uintptr_t)args[1];
    const char *new_path = (const char *)(uintptr_t)args[3];
    char        old_buf[EXEC_MAX_PATH];
    char        new_buf[EXEC_MAX_PATH];
    int         rc;

    if (!old_path || !new_path)
        return ERR(EFAULT);

    rc = resolve_dirfd_path_meta(old_dirfd, old_path, 0,
                                 old_buf, sizeof(old_buf), NULL);
    if (rc < 0)
        return ERR(-rc);
    rc = resolve_dirfd_path_meta(new_dirfd, new_path, 0,
                                 new_buf, sizeof(new_buf), NULL);
    if (rc < 0)
        return ERR(-rc);

    rc = vfs_rename(old_buf, new_buf);
    return (rc < 0) ? ERR(-rc) : 0ULL;
}

static void linux_syscall_bind(uint32_t nr, syscall_handler_fn handler,
                               uint32_t flags, const char *name)
{
    if (nr >= LINUX_SYSCALL_MAX)
        return;
    linux_syscall_table[nr].handler = handler;
    linux_syscall_table[nr].flags   = flags;
    linux_syscall_table[nr].name    = name;
}

/* ════════════════════════════════════════════════════════════════════
 * syscall_init — popola la tabella e inizializza le strutture dati
 * ════════════════════════════════════════════════════════════════════ */
void syscall_init(void)
{
    /* 1. Porta su line discipline + VFS bootstrap */
    signal_init();
    futex_init();
    mreact_init();
    ksem_init();
    kmon_init();
    tty_init();
    vfs_init();

    /* 2. Inizializza fd_table e object pool */
    sock_init();
    memset(fd_objects, 0, sizeof(fd_objects));
    memset(pipe_pool, 0, sizeof(pipe_pool));
    for (int i = 0; i < SCHED_MAX_TASKS; i++) {
        fd_init_slot_defaults(i);
    }

    /* 3. Inizializza task_brk a zero (lazy-init alla prima chiamata brk) */
    for (int i = 0; i < SCHED_MAX_TASKS; i++)
        task_brk[i] = 0;
    memset(vfs_srv_tables, 0, sizeof(vfs_srv_tables));

    /* 4. Riempi la tabella con ENOSYS */
    for (int i = 0; i < SYSCALL_MAX; i++) {
        syscall_table[i].handler = sys_enosys;
        syscall_table[i].flags   = 0;
        syscall_table[i].name    = "enosys";
    }
    for (int i = 0; i < (int)LINUX_SYSCALL_MAX; i++) {
        linux_syscall_table[i].handler = sys_enosys;
        linux_syscall_table[i].flags   = 0;
        linux_syscall_table[i].name    = "enosys";
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
    syscall_table[SYS_MSYNC] = (syscall_entry_t){
        sys_msync, 0, "msync"
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
    syscall_table[SYS_SIGACTION] = (syscall_entry_t){
        sys_sigaction, 0, "sigaction"
    };
    syscall_table[SYS_SIGPROCMASK] = (syscall_entry_t){
        sys_sigprocmask, SYSCALL_FLAG_RT, "sigprocmask"
    };
    syscall_table[SYS_SIGRETURN] = (syscall_entry_t){
        sys_sigreturn, SYSCALL_FLAG_RT, "sigreturn"
    };
    syscall_table[SYS_YIELD] = (syscall_entry_t){
        sys_yield, SYSCALL_FLAG_RT, "yield"
    };
    syscall_table[SYS_SETPGID] = (syscall_entry_t){
        sys_setpgid, SYSCALL_FLAG_RT, "setpgid"
    };
    syscall_table[SYS_GETPGID] = (syscall_entry_t){
        sys_getpgid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "getpgid"
    };
    syscall_table[SYS_SETSID] = (syscall_entry_t){
        sys_setsid, SYSCALL_FLAG_RT, "setsid"
    };
    syscall_table[SYS_GETSID] = (syscall_entry_t){
        sys_getsid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "getsid"
    };
    syscall_table[SYS_TCSETPGRP] = (syscall_entry_t){
        sys_tcsetpgrp, SYSCALL_FLAG_RT, "tcsetpgrp"
    };
    syscall_table[SYS_TCGETPGRP] = (syscall_entry_t){
        sys_tcgetpgrp, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "tcgetpgrp"
    };
    syscall_table[SYS_CHDIR] = (syscall_entry_t){
        sys_chdir, 0, "chdir"
    };
    syscall_table[SYS_GETCWD] = (syscall_entry_t){
        sys_getcwd, 0, "getcwd"
    };
    syscall_table[SYS_PIPE] = (syscall_entry_t){
        sys_pipe, 0, "pipe"
    };
    syscall_table[SYS_DUP] = (syscall_entry_t){
        sys_dup, SYSCALL_FLAG_RT, "dup"
    };
    syscall_table[SYS_DUP2] = (syscall_entry_t){
        sys_dup2, SYSCALL_FLAG_RT, "dup2"
    };
    syscall_table[SYS_TCGETATTR] = (syscall_entry_t){
        sys_tcgetattr, 0, "tcgetattr"
    };
    syscall_table[SYS_TCSETATTR] = (syscall_entry_t){
        sys_tcsetattr, 0, "tcsetattr"
    };
    syscall_table[SYS_KBD_SET_LAYOUT] = (syscall_entry_t){
        sys_kbd_set_layout, 0, "kbd_set_layout"
    };
    syscall_table[SYS_KBD_GET_LAYOUT] = (syscall_entry_t){
        sys_kbd_get_layout, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "kbd_get_layout"
    };
    syscall_table[SYS_GETPID] = (syscall_entry_t){
        sys_getpid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "getpid"
    };
    syscall_table[SYS_GETPPID] = (syscall_entry_t){
        sys_getppid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "getppid"
    };
    syscall_table[SYS_ISATTY] = (syscall_entry_t){
        sys_isatty, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "isatty"
    };
    syscall_table[SYS_GETTIMEOFDAY] = (syscall_entry_t){
        sys_gettimeofday, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "gettimeofday"
    };
    syscall_table[SYS_NANOSLEEP] = (syscall_entry_t){
        sys_nanosleep, 0, "nanosleep"
    };
    syscall_table[SYS_GETUID] = (syscall_entry_t){
        sys_getuid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "getuid"
    };
    syscall_table[SYS_GETGID] = (syscall_entry_t){
        sys_getgid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "getgid"
    };
    syscall_table[SYS_GETEUID] = (syscall_entry_t){
        sys_geteuid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "geteuid"
    };
    syscall_table[SYS_GETEGID] = (syscall_entry_t){
        sys_getegid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "getegid"
    };
    syscall_table[SYS_LSEEK] = (syscall_entry_t){
        sys_lseek, 0, "lseek"
    };
    syscall_table[SYS_READV] = (syscall_entry_t){
        sys_readv, 0, "readv"
    };
    syscall_table[SYS_WRITEV] = (syscall_entry_t){
        sys_writev, 0, "writev"
    };
    syscall_table[SYS_FCNTL] = (syscall_entry_t){
        sys_fcntl, 0, "fcntl"
    };
    syscall_table[SYS_OPENAT] = (syscall_entry_t){
        sys_openat, 0, "openat"
    };
    syscall_table[SYS_FSTATAT] = (syscall_entry_t){
        sys_fstatat, 0, "fstatat"
    };
    syscall_table[SYS_IOCTL] = (syscall_entry_t){
        sys_ioctl, 0, "ioctl"
    };
    syscall_table[SYS_UNAME] = (syscall_entry_t){
        sys_uname, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "uname"
    };
    syscall_table[SYS_CLONE] = (syscall_entry_t){
        sys_clone, 0, "clone"
    };
    syscall_table[SYS_GETTID] = (syscall_entry_t){
        sys_gettid, SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "gettid"
    };
    syscall_table[SYS_SET_TID_ADDRESS] = (syscall_entry_t){
        sys_set_tid_address, 0, "set_tid_address"
    };
    syscall_table[SYS_EXIT_GROUP] = (syscall_entry_t){
        sys_exit_group, 0, "exit_group"
    };
    syscall_table[SYS_DLOPEN] = (syscall_entry_t){
        sys_dlopen, 0, "dlopen"
    };
    syscall_table[SYS_DLSYM] = (syscall_entry_t){
        sys_dlsym, 0, "dlsym"
    };
    syscall_table[SYS_DLCLOSE] = (syscall_entry_t){
        sys_dlclose, 0, "dlclose"
    };
    syscall_table[SYS_DLERROR] = (syscall_entry_t){
        sys_dlerror, 0, "dlerror"
    };
    syscall_table[SYS_MKDIR] = (syscall_entry_t){
        sys_mkdir, 0, "mkdir"
    };
    syscall_table[SYS_UNLINK] = (syscall_entry_t){
        sys_unlink, 0, "unlink"
    };
    syscall_table[SYS_RENAME] = (syscall_entry_t){
        sys_rename, 0, "rename"
    };
    syscall_table[SYS_FUTEX] = (syscall_entry_t){
        sys_futex, 0, "futex"
    };
    syscall_table[SYS_MOUNT] = (syscall_entry_t){
        sys_mount, 0, "mount"
    };
    syscall_table[SYS_UMOUNT] = (syscall_entry_t){
        sys_umount, 0, "umount"
    };
    syscall_table[SYS_PIVOT_ROOT] = (syscall_entry_t){
        sys_pivot_root, 0, "pivot_root"
    };
    syscall_table[SYS_UNSHARE] = (syscall_entry_t){
        sys_unshare, 0, "unshare"
    };
    syscall_table[SYS_KILL] = (syscall_entry_t){
        sys_kill, SYSCALL_FLAG_RT, "kill"
    };
    syscall_table[SYS_TGKILL] = (syscall_entry_t){
        sys_tgkill, SYSCALL_FLAG_RT, "tgkill"
    };
    syscall_table[SYS_PORT_LOOKUP] = (syscall_entry_t){
        sys_port_lookup, SYSCALL_FLAG_RT, "port_lookup"
    };
    syscall_table[SYS_IPC_WAIT] = (syscall_entry_t){
        sys_ipc_wait, 0, "ipc_wait"
    };
    syscall_table[SYS_IPC_REPLY] = (syscall_entry_t){
        sys_ipc_reply, 0, "ipc_reply"
    };
    syscall_table[SYS_IPC_POLL] = (syscall_entry_t){
        sys_ipc_poll, 0, "ipc_poll"
    };
    syscall_table[SYS_VFS_BOOT_OPEN] = (syscall_entry_t){
        sys_vfs_boot_open, 0, "vfs_boot_open"
    };
    syscall_table[SYS_VFS_BOOT_READ] = (syscall_entry_t){
        sys_vfs_boot_read, 0, "vfs_boot_read"
    };
    syscall_table[SYS_VFS_BOOT_WRITE] = (syscall_entry_t){
        sys_vfs_boot_write, 0, "vfs_boot_write"
    };
    syscall_table[SYS_VFS_BOOT_READDIR] = (syscall_entry_t){
        sys_vfs_boot_readdir, 0, "vfs_boot_readdir"
    };
    syscall_table[SYS_VFS_BOOT_STAT] = (syscall_entry_t){
        sys_vfs_boot_stat, 0, "vfs_boot_stat"
    };
    syscall_table[SYS_VFS_BOOT_CLOSE] = (syscall_entry_t){
        sys_vfs_boot_close, 0, "vfs_boot_close"
    };
    syscall_table[SYS_VFS_BOOT_TASKINFO] = (syscall_entry_t){
        sys_vfs_boot_taskinfo, SYSCALL_FLAG_RT, "vfs_boot_taskinfo"
    };
    syscall_table[SYS_VFS_BOOT_LSEEK] = (syscall_entry_t){
        sys_vfs_boot_lseek, 0, "vfs_boot_lseek"
    };
    syscall_table[SYS_BLK_BOOT_READ] = (syscall_entry_t){
        sys_blk_boot_read, 0, "blk_boot_read"
    };
    syscall_table[SYS_BLK_BOOT_WRITE] = (syscall_entry_t){
        sys_blk_boot_write, 0, "blk_boot_write"
    };
    syscall_table[SYS_BLK_BOOT_FLUSH] = (syscall_entry_t){
        sys_blk_boot_flush, 0, "blk_boot_flush"
    };
    syscall_table[SYS_BLK_BOOT_SECTORS] = (syscall_entry_t){
        sys_blk_boot_sectors, SYSCALL_FLAG_RT, "blk_boot_sectors"
    };
    syscall_table[SYS_NET_BOOT_SEND] = (syscall_entry_t){
        sys_net_boot_send, 0, "net_boot_send"
    };
    syscall_table[SYS_NET_BOOT_RECV] = (syscall_entry_t){
        sys_net_boot_recv, 0, "net_boot_recv"
    };
    syscall_table[SYS_NET_BOOT_INFO] = (syscall_entry_t){
        sys_net_boot_info, SYSCALL_FLAG_RT, "net_boot_info"
    };
    syscall_table[SYS_MREACT_SUBSCRIBE] = (syscall_entry_t){
        sys_mreact_subscribe, 0, "mreact_subscribe"
    };
    syscall_table[SYS_MREACT_WAIT] = (syscall_entry_t){
        sys_mreact_wait, SYSCALL_FLAG_RT, "mreact_wait"
    };
    syscall_table[SYS_MREACT_CANCEL] = (syscall_entry_t){
        sys_mreact_cancel, SYSCALL_FLAG_RT, "mreact_cancel"
    };
    syscall_table[SYS_MREACT_SUBSCRIBE_ALL] = (syscall_entry_t){
        sys_mreact_subscribe_all, 0, "mreact_subscribe_all"
    };
    syscall_table[SYS_MREACT_SUBSCRIBE_ANY] = (syscall_entry_t){
        sys_mreact_subscribe_any, 0, "mreact_subscribe_any"
    };
    syscall_table[SYS_KSEM_CREATE] = (syscall_entry_t){
        sys_ksem_create, 0, "ksem_create"
    };
    syscall_table[SYS_KSEM_OPEN] = (syscall_entry_t){
        sys_ksem_open, 0, "ksem_open"
    };
    syscall_table[SYS_KSEM_CLOSE] = (syscall_entry_t){
        sys_ksem_close, SYSCALL_FLAG_RT, "ksem_close"
    };
    syscall_table[SYS_KSEM_UNLINK] = (syscall_entry_t){
        sys_ksem_unlink, 0, "ksem_unlink"
    };
    syscall_table[SYS_KSEM_POST] = (syscall_entry_t){
        sys_ksem_post, SYSCALL_FLAG_RT, "ksem_post"
    };
    syscall_table[SYS_KSEM_WAIT] = (syscall_entry_t){
        sys_ksem_wait, SYSCALL_FLAG_RT, "ksem_wait"
    };
    syscall_table[SYS_KSEM_TIMEDWAIT] = (syscall_entry_t){
        sys_ksem_timedwait, SYSCALL_FLAG_RT, "ksem_timedwait"
    };
    syscall_table[SYS_KSEM_TRYWAIT] = (syscall_entry_t){
        sys_ksem_trywait, SYSCALL_FLAG_RT, "ksem_trywait"
    };
    syscall_table[SYS_KSEM_GETVALUE] = (syscall_entry_t){
        sys_ksem_getvalue, SYSCALL_FLAG_RT, "ksem_getvalue"
    };
    syscall_table[SYS_KSEM_ANON] = (syscall_entry_t){
        sys_ksem_anon, 0, "ksem_anon"
    };
    syscall_table[SYS_KMON_CREATE] = (syscall_entry_t){
        sys_kmon_create, 0, "kmon_create"
    };
    syscall_table[SYS_KMON_DESTROY] = (syscall_entry_t){
        sys_kmon_destroy, 0, "kmon_destroy"
    };
    syscall_table[SYS_KMON_ENTER] = (syscall_entry_t){
        sys_kmon_enter, SYSCALL_FLAG_RT, "kmon_enter"
    };
    syscall_table[SYS_KMON_EXIT] = (syscall_entry_t){
        sys_kmon_exit, SYSCALL_FLAG_RT, "kmon_exit"
    };
    syscall_table[SYS_KMON_WAIT] = (syscall_entry_t){
        sys_kmon_wait, SYSCALL_FLAG_RT, "kmon_wait"
    };
    syscall_table[SYS_KMON_SIGNAL] = (syscall_entry_t){
        sys_kmon_signal, SYSCALL_FLAG_RT, "kmon_signal"
    };
    syscall_table[SYS_KMON_BROADCAST] = (syscall_entry_t){
        sys_kmon_broadcast, SYSCALL_FLAG_RT, "kmon_broadcast"
    };

    /* === Capability System (M9-01) === */
    syscall_table[SYS_CAP_ALLOC]  = (syscall_entry_t){
        sys_cap_alloc,  0,               "cap_alloc"
    };
    syscall_table[SYS_CAP_SEND]   = (syscall_entry_t){
        sys_cap_send,   SYSCALL_FLAG_RT, "cap_send"
    };
    syscall_table[SYS_CAP_REVOKE] = (syscall_entry_t){
        sys_cap_revoke, SYSCALL_FLAG_RT, "cap_revoke"
    };
    syscall_table[SYS_CAP_DERIVE] = (syscall_entry_t){
        sys_cap_derive, 0,               "cap_derive"
    };
    syscall_table[SYS_CAP_QUERY]  = (syscall_entry_t){
        sys_cap_query,  SYSCALL_FLAG_RT, "cap_query"
    };

    /* ── BSD socket API (M10-03) ── */
    syscall_table[SYS_SOCKET]     = (syscall_entry_t){ sys_socket,     0, "socket"     };
    syscall_table[SYS_BIND]       = (syscall_entry_t){ sys_bind,       0, "bind"       };
    syscall_table[SYS_LISTEN]     = (syscall_entry_t){ sys_listen,     0, "listen"     };
    syscall_table[SYS_ACCEPT]     = (syscall_entry_t){ sys_accept,     0, "accept"     };
    syscall_table[SYS_CONNECT]    = (syscall_entry_t){ sys_connect,    0, "connect"    };
    syscall_table[SYS_SEND]       = (syscall_entry_t){ sys_send,       0, "send"       };
    syscall_table[SYS_RECV]       = (syscall_entry_t){ sys_recv,       0, "recv"       };
    syscall_table[SYS_SENDTO]     = (syscall_entry_t){ sys_sendto,     0, "sendto"     };
    syscall_table[SYS_RECVFROM]   = (syscall_entry_t){ sys_recvfrom,   0, "recvfrom"   };
    syscall_table[SYS_SETSOCKOPT] = (syscall_entry_t){ sys_setsockopt, 0, "setsockopt" };
    syscall_table[SYS_GETSOCKOPT] = (syscall_entry_t){ sys_getsockopt, 0, "getsockopt" };
    syscall_table[SYS_SHUTDOWN]   = (syscall_entry_t){ sys_shutdown,   0, "shutdown"   };
    syscall_table[SYS_PRLIMIT64]  = (syscall_entry_t){ sys_prlimit64,  0, "prlimit64"  };
    syscall_table[SYS_REBOOT]     = (syscall_entry_t){ sys_reboot,     0, "reboot"     };

    /* ── Linux AArch64 compat ABI (M11-05 v1) ── */
    linux_syscall_bind(LINUX_NR_read, sys_linux_read,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_read");
    linux_syscall_bind(LINUX_NR_write, sys_linux_write,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_write");
    linux_syscall_bind(LINUX_NR_close, sys_linux_close,
                       SYSCALL_FLAG_RT, "linux_close");
    linux_syscall_bind(LINUX_NR_openat, sys_linux_openat, 0, "linux_openat");
    linux_syscall_bind(LINUX_NR_newfstatat, sys_linux_newfstatat, 0, "linux_newfstatat");
    linux_syscall_bind(LINUX_NR_fstat, sys_linux_fstat, 0, "linux_fstat");
    linux_syscall_bind(LINUX_NR_getdents64, sys_linux_getdents64, 0, "linux_getdents64");
    linux_syscall_bind(LINUX_NR_lseek, sys_linux_lseek, 0, "linux_lseek");
    linux_syscall_bind(LINUX_NR_readv, sys_linux_readv, 0, "linux_readv");
    linux_syscall_bind(LINUX_NR_writev, sys_linux_writev, 0, "linux_writev");
    linux_syscall_bind(LINUX_NR_ioctl, sys_linux_ioctl, 0, "linux_ioctl");
    linux_syscall_bind(LINUX_NR_dup, sys_linux_dup, SYSCALL_FLAG_RT, "linux_dup");
    linux_syscall_bind(LINUX_NR_dup3, sys_linux_dup3, SYSCALL_FLAG_RT, "linux_dup3");
    linux_syscall_bind(LINUX_NR_pipe2, sys_linux_pipe2, 0, "linux_pipe2");
    linux_syscall_bind(LINUX_NR_fcntl, sys_linux_fcntl, 0, "linux_fcntl");
    linux_syscall_bind(LINUX_NR_getcwd, sys_linux_getcwd,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getcwd");
    linux_syscall_bind(LINUX_NR_futex, sys_linux_futex, 0, "linux_futex");
    linux_syscall_bind(LINUX_NR_clock_gettime, sys_linux_clock_gettime,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_clock_gettime");
    linux_syscall_bind(LINUX_NR_nanosleep, sys_linux_nanosleep, 0, "linux_nanosleep");
    linux_syscall_bind(LINUX_NR_sched_yield, sys_linux_sched_yield,
                       SYSCALL_FLAG_RT, "linux_sched_yield");
    linux_syscall_bind(LINUX_NR_getpid, sys_linux_getpid,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getpid");
    linux_syscall_bind(LINUX_NR_getppid, sys_linux_getppid,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getppid");
    linux_syscall_bind(LINUX_NR_getuid, sys_linux_getuid,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getuid");
    linux_syscall_bind(LINUX_NR_geteuid, sys_linux_geteuid,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_geteuid");
    linux_syscall_bind(LINUX_NR_getgid, sys_linux_getgid,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getgid");
    linux_syscall_bind(LINUX_NR_getegid, sys_linux_getegid,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getegid");
    linux_syscall_bind(LINUX_NR_gettid, sys_linux_gettid,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_gettid");
    linux_syscall_bind(LINUX_NR_tgkill, sys_linux_tgkill,
                       SYSCALL_FLAG_RT, "linux_tgkill");
    linux_syscall_bind(LINUX_NR_rt_sigaction, sys_linux_rt_sigaction,
                       SYSCALL_FLAG_RT, "linux_rt_sigaction");
    linux_syscall_bind(LINUX_NR_rt_sigprocmask, sys_linux_rt_sigprocmask,
                       SYSCALL_FLAG_RT, "linux_rt_sigprocmask");
    linux_syscall_bind(LINUX_NR_rt_sigreturn, sys_linux_rt_sigreturn,
                       SYSCALL_FLAG_RT, "linux_rt_sigreturn");
    linux_syscall_bind(LINUX_NR_uname, sys_linux_uname,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_uname");
    linux_syscall_bind(LINUX_NR_gettimeofday, sys_linux_gettimeofday,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_gettimeofday");
    linux_syscall_bind(LINUX_NR_clone, sys_linux_clone, 0, "linux_clone");
    linux_syscall_bind(LINUX_NR_execve, sys_linux_execve, 0, "linux_execve");
    linux_syscall_bind(LINUX_NR_mmap, sys_linux_mmap, 0, "linux_mmap");
    linux_syscall_bind(LINUX_NR_mremap, sys_linux_mremap, 0, "linux_mremap");
    linux_syscall_bind(LINUX_NR_mprotect, sys_linux_mprotect, 0, "linux_mprotect");
    linux_syscall_bind(LINUX_NR_munmap, sys_linux_munmap, 0, "linux_munmap");
    linux_syscall_bind(LINUX_NR_brk, sys_linux_brk,
                       SYSCALL_FLAG_RT, "linux_brk");
    linux_syscall_bind(LINUX_NR_wait4, sys_linux_wait4,
                       SYSCALL_FLAG_RT, "linux_wait4");
    linux_syscall_bind(LINUX_NR_fsync, sys_linux_fsync, 0, "linux_fsync");
    linux_syscall_bind(LINUX_NR_truncate, sys_linux_truncate, 0, "linux_truncate");
    linux_syscall_bind(LINUX_NR_ftruncate, sys_linux_ftruncate, 0, "linux_ftruncate");
    linux_syscall_bind(LINUX_NR_faccessat, sys_linux_faccessat, 0, "linux_faccessat");
    linux_syscall_bind(LINUX_NR_faccessat2, sys_linux_faccessat2, 0, "linux_faccessat2");
    linux_syscall_bind(LINUX_NR_fchmod, sys_linux_fchmod,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_fchmod");
    linux_syscall_bind(LINUX_NR_fchmodat, sys_linux_fchmodat,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_fchmodat");
    linux_syscall_bind(LINUX_NR_fchownat, sys_linux_fchownat,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_fchownat");
    linux_syscall_bind(LINUX_NR_sched_getaffinity, sys_linux_sched_getaffinity,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_sched_getaffinity");
    linux_syscall_bind(LINUX_NR_sched_get_priority_max, sys_linux_sched_get_priority_max,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_sched_get_priority_max");
    linux_syscall_bind(LINUX_NR_sched_get_priority_min, sys_linux_sched_get_priority_min,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_sched_get_priority_min");
    linux_syscall_bind(LINUX_NR_getrusage, sys_linux_getrusage,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getrusage");
    linux_syscall_bind(LINUX_NR_umask, sys_linux_umask,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_umask");
    linux_syscall_bind(LINUX_NR_madvise, sys_linux_madvise,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_madvise");
    linux_syscall_bind(LINUX_NR_unlinkat, sys_linux_unlinkat, 0, "linux_unlinkat");
    linux_syscall_bind(LINUX_NR_symlinkat, sys_linux_symlinkat, 0, "linux_symlinkat");
    linux_syscall_bind(LINUX_NR_readlinkat, sys_linux_readlinkat, 0, "linux_readlinkat");
    linux_syscall_bind(LINUX_NR_mkdirat, sys_linux_mkdirat, 0, "linux_mkdirat");
    linux_syscall_bind(LINUX_NR_renameat, sys_linux_renameat, 0, "linux_renameat");
    linux_syscall_bind(LINUX_NR_chdir, sys_linux_chdir, 0, "linux_chdir");
    linux_syscall_bind(LINUX_NR_kill, sys_linux_kill,
                       SYSCALL_FLAG_RT, "linux_kill");
    linux_syscall_bind(LINUX_NR_tkill, sys_linux_tkill,
                       SYSCALL_FLAG_RT, "linux_tkill");
    linux_syscall_bind(LINUX_NR_setpgid, sys_linux_setpgid,
                       SYSCALL_FLAG_RT, "linux_setpgid");
    linux_syscall_bind(LINUX_NR_getpgrp, sys_linux_getpgrp,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getpgrp");
    linux_syscall_bind(LINUX_NR_setsid, sys_linux_setsid,
                       SYSCALL_FLAG_RT, "linux_setsid");
    linux_syscall_bind(LINUX_NR_getrlimit, sys_linux_getrlimit,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_getrlimit");
    linux_syscall_bind(LINUX_NR_setrlimit, sys_linux_setrlimit,
                       SYSCALL_FLAG_RT, "linux_setrlimit");
    linux_syscall_bind(LINUX_NR_flock, sys_linux_flock, 0, "linux_flock");
    linux_syscall_bind(LINUX_NR_prlimit64, sys_linux_prlimit64, 0, "linux_prlimit64");
    linux_syscall_bind(LINUX_NR_sysinfo, sys_linux_sysinfo,
                       SYSCALL_FLAG_RT | SYSCALL_FLAG_NOBLOCK, "linux_sysinfo");
    linux_syscall_bind(LINUX_NR_getrandom, sys_linux_getrandom, 0, "linux_getrandom");
    linux_syscall_bind(LINUX_NR_rseq, sys_linux_rseq, 0, "linux_rseq");
    linux_syscall_bind(LINUX_NR_pselect6, sys_linux_pselect6, 0, "linux_pselect6");
    linux_syscall_bind(LINUX_NR_ppoll, sys_linux_ppoll, 0, "linux_ppoll");
    linux_syscall_bind(LINUX_NR_socket, sys_linux_socket, 0, "linux_socket");
    linux_syscall_bind(LINUX_NR_bind, sys_linux_bind, 0, "linux_bind");
    linux_syscall_bind(LINUX_NR_listen, sys_linux_listen, 0, "linux_listen");
    linux_syscall_bind(LINUX_NR_accept, sys_linux_accept, 0, "linux_accept");
    linux_syscall_bind(LINUX_NR_connect, sys_linux_connect, 0, "linux_connect");
    linux_syscall_bind(LINUX_NR_getsockname, sys_linux_getsockname, 0, "linux_getsockname");
    linux_syscall_bind(LINUX_NR_getpeername, sys_linux_getpeername, 0, "linux_getpeername");
    linux_syscall_bind(LINUX_NR_sendto, sys_linux_sendto, 0, "linux_sendto");
    linux_syscall_bind(LINUX_NR_recvfrom, sys_linux_recvfrom, 0, "linux_recvfrom");
    linux_syscall_bind(LINUX_NR_setsockopt, sys_linux_setsockopt, 0, "linux_setsockopt");
    linux_syscall_bind(LINUX_NR_getsockopt, sys_linux_getsockopt, 0, "linux_getsockopt");
    linux_syscall_bind(LINUX_NR_shutdown, sys_linux_shutdown, 0, "linux_shutdown");
    linux_syscall_bind(LINUX_NR_set_tid_address, sys_linux_set_tid_address, 0, "linux_set_tid_address");
    linux_syscall_bind(LINUX_NR_exit, sys_linux_exit, SYSCALL_FLAG_RT, "linux_exit");
    linux_syscall_bind(LINUX_NR_exit_group, sys_linux_exit_group, SYSCALL_FLAG_RT, "linux_exit_group");

    uart_puts("[SYSCALL] syscall native + Linux compat ABI registrate\n");
    uart_puts("[SYSCALL] fd_table: 0/1/2=VFS(/dev/std*) per ");
    syscall_uart_put_u64((uint64_t)SCHED_MAX_TASKS);
    uart_puts(" proc slot\n");
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
    const syscall_entry_t *table = syscall_table;
    uint64_t max = SYSCALL_MAX;

    if (current_task && sched_task_abi_mode(current_task) == SCHED_ABI_LINUX) {
        table = linux_syscall_table;
        max = LINUX_SYSCALL_MAX;
    }

    if (nr >= max) {
        frame->x[0] = ERR(ENOSYS);
        return;
    }

    uint64_t args[6] = {
        frame->x[0], frame->x[1], frame->x[2],
        frame->x[3], frame->x[4], frame->x[5],
    };

    active_syscall_frame = frame;
    active_syscall_replaced = 0U;
    ret = table[nr].handler(args);

    if (!active_syscall_replaced)
        frame->x[0] = ret;
    active_syscall_frame = NULL;
    active_syscall_replaced = 0U;
}
