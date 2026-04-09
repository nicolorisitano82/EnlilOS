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
#include "cap.h"
#include "kdebug.h"
#include "keyboard.h"
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

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

/* ════════════════════════════════════════════════════════════════════
 * File descriptor table
 * ════════════════════════════════════════════════════════════════════ */

#define MAX_FD          64

#define FD_TYPE_FREE    0
#define FD_TYPE_VFS     1
#define FD_TYPE_VFSD    2
#define FD_TYPE_PIPE    3

#define VFSD_HANDLE_MAX 64U
#define FD_OBJECT_MAX   (SCHED_MAX_TASKS * MAX_FD)
#define PIPE_POOL_MAX   32U
#define PIPE_BUF_SIZE   4096U

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
 * Indicizzata per [pid % SCHED_MAX_TASKS][fd].
 * Tutti i task partono con 0/1/2 preimpostati come CONSOLE.
 */
static fd_entry_t fd_tables[SCHED_MAX_TASKS][MAX_FD];
static vfs_srv_handle_t vfs_srv_tables[SCHED_MAX_TASKS][VFSD_HANDLE_MAX];
static fd_object_t fd_objects[FD_OBJECT_MAX];
static pipe_t      pipe_pool[PIPE_POOL_MAX];

/* Owner check per blkd: solo il server proprietario della porta "block" può usare le blk_boot_* */
static int blk_srv_owner_ok(void)
{
    port_t *port;

    if (!current_task || !sched_task_is_user(current_task))
        return 0;

    port = mk_port_lookup("block");
    return (port && port->owner_tid == current_task->pid) ? 1 : 0;
}

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

static int  vfsd_proxy_close(uint32_t handle);
static void stat_fill(stat_t *st, uint32_t mode, uint64_t size,
                      uint32_t blksize);

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

/* Indice nel fd_table per il task corrente */
static inline int task_idx(void)
{
    if (!current_task) return 0;
    return (int)(current_task->pid % SCHED_MAX_TASKS);
}

static int task_idx_from_pid(uint32_t pid)
{
    return (int)(pid % SCHED_MAX_TASKS);
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
    return fd_attach_object(e, obj);
}

static int fd_bind_remote_shadow(fd_entry_t *e, const char *path, uint16_t flags)
{
    uint16_t shadow_flags;
    fd_object_t *obj;

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
    return vfs_open(path, shadow_flags, &obj->file);
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

    idx = task_idx_from_pid(task->pid);
    fd_reset_slot_defaults(idx);
    task_brk[idx] = 0ULL;
}

static int vfs_srv_task_slot(void)
{
    if (!current_task)
        return -1;
    return (int)(current_task->pid % SCHED_MAX_TASKS);
}

static int vfs_srv_owner_ok(void)
{
    port_t *port;

    if (!current_task || !sched_task_is_user(current_task))
        return 0;

    port = mk_port_lookup("vfs");
    return (port && port->owner_tid == current_task->pid) ? 1 : 0;
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
    if (port->owner_tid == current_task->pid)
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

static int vfsd_proxy_resolve(const char *path, char *out, size_t cap)
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
    return vfsd_resp_get_path(&resp, out, cap);
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

        if (mmu_space_prepare_write(space, cur, chunk) < 0)
            return -EFAULT;
        dst = mmu_space_resolve_ptr(space, cur, chunk);
        if (!dst) return -EFAULT;
        memcpy(dst, in + copied, chunk);
        copied += chunk;
    }

    return 0;
}

static int resolve_user_vfs_path(const char *input, char *out, size_t cap)
{
    size_t i = 0U;

    if (!input || !out || cap < 2U)
        return -EFAULT;

    if (current_task && sched_task_is_user(current_task) && vfsd_proxy_available())
        return vfsd_proxy_resolve(input, out, cap);

    while (input[i] != '\0' && i + 1U < cap) {
        out[i] = input[i];
        i++;
    }
    if (input[i] != '\0')
        return -ENAMETOOLONG;
    out[i] = '\0';
    return 0;
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
    return current_task ? (uint64_t)current_task->pid : 0ULL;
}

static uint64_t sys_getppid(uint64_t args[6])
{
    (void)args;
    return current_task ? (uint64_t)sched_task_parent_pid(current_task) : 0ULL;
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
    uint16_t    oflags = 0U;
    uint8_t     fd_flags = 0U;
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
    if (path_arg[0] != '/' && dirfd != AT_FDCWD) {
        if (!fd_get(dirfd))
            return ERR(EBADF);
        return ERR(ENOSYS);
    }

    fd_split_open_flags((uint16_t)args[2], &oflags, &fd_flags);
    rc = fd_open_path_current(path_arg, oflags, fd_flags);
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
    uint64_t    stat_args[6] = { 0 };
    uint64_t    close_args[6] = { 0 };
    uint64_t    rc;
    int         fd;
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

    if (path_arg[0] == '\0') {
        if ((flags & AT_EMPTY_PATH) == 0U)
            return ERR(ENOENT);
        if (dirfd == AT_FDCWD) {
            path_arg = ".";
        } else {
            stat_args[0] = (uint64_t)dirfd;
            stat_args[1] = stat_uva;
            return sys_fstat(stat_args);
        }
    }

    if (path_arg[0] != '/' && dirfd != AT_FDCWD) {
        if (!fd_get(dirfd))
            return ERR(EBADF);
        return ERR(ENOSYS);
    }

    fd = fd_open_path_current(path_arg, O_RDONLY, 0U);
    if (fd < 0)
        return ERR(-fd);

    stat_args[0] = (uint64_t)fd;
    stat_args[1] = stat_uva;
    rc = sys_fstat(stat_args);

    close_args[0] = (uint64_t)fd;
    (void)sys_close(close_args);
    return rc;
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
    if (pages == 0ULL || pages > 256ULL)
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
        (void)vmm_map_file(current_task->pid, user_va, aligned,
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
    uint64_t    addr   = args[0];
    uint64_t    length = args[1];
    mm_space_t *space;

    if (addr == 0U || length == 0U) return ERR(EINVAL);
    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EINVAL);
    if (addr < MMU_USER_BASE || addr >= MMU_USER_LIMIT)
        return ERR(EINVAL);

    space = sched_task_space(current_task);

    /* Se la regione era file-backed MAP_SHARED: scrive le pagine sporche */
    if (space)
        (void)vmm_unmap_range(current_task->pid, space,
                              (uintptr_t)addr, (size_t)length);

    /* Il rilascio fisico delle pagine avviene alla distruzione di mm_space */
    return 0;
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

    return (uint64_t)(int64_t)vmm_msync(current_task->pid, space, addr, length);
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
    int                rc;

    if (!current_task || !sched_task_is_user(current_task) || !frame)
        return ERR(ENOSYS);

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

    rc = resolve_user_vfs_path(copy->path, resolved_path, sizeof(resolved_path));
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
    task_brk[task_idx()] = 0ULL;
    /* Reset thread pointer: il nuovo processo inizia con TP=0.
     * crt1/musl lo inizializzeranno durante il proprio startup. */
    sched_task_set_tpidr(current_task, 0ULL);
    __asm__ volatile("msr tpidr_el0, xzr" ::: "memory");
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
    child_idx = (int)(child->pid % SCHED_MAX_TASKS);
    fd_clone_task_table(child_idx, parent_idx);
    task_brk[child_idx] = task_brk[parent_idx];
    signal_task_fork(child, current_task);

    return (uint64_t)child->pid;
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

    caller_pid = current_task->pid;
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
            if (sched_task_parent_pid(t) != caller_pid)
                continue;

            if (pid_arg > 0)
                match = ((uint32_t)pid_arg == t->pid);
            else if (pid_arg == 0)
                match = (sched_task_pgid(t) == sched_task_pgid(current_task));
            else if (pid_arg == -1)
                match = 1;
            else
                match = (sched_task_pgid(t) == (uint32_t)(-pid_arg));

            if (!match)
                continue;

            matched_child = 1;

            if (t->state == TCB_STATE_ZOMBIE) {
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

static uint64_t sys_setpgid(uint64_t args[6])
{
    int32_t      pid_arg = (int32_t)args[0];
    uint32_t     pgid = (uint32_t)args[1];
    sched_tcb_t *target;
    int          rc;

    if (!current_task || !sched_task_is_user(current_task))
        return ERR(EPERM);
    if (pid_arg == 0)
        pid_arg = (int32_t)current_task->pid;

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

    rc = ksem_anon_current((uint32_t)args[0], 0U, &handle);
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
    signal_init();
    mreact_init();
    ksem_init();
    kmon_init();
    tty_init();
    vfs_init();

    /* 2. Inizializza fd_table e object pool */
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
    syscall_table[SYS_PORT_LOOKUP] = (syscall_entry_t){
        sys_port_lookup, SYSCALL_FLAG_RT, "port_lookup"
    };
    syscall_table[SYS_IPC_WAIT] = (syscall_entry_t){
        sys_ipc_wait, 0, "ipc_wait"
    };
    syscall_table[SYS_IPC_REPLY] = (syscall_entry_t){
        sys_ipc_reply, 0, "ipc_reply"
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

    uart_puts("[SYSCALL] 91 syscall base/UX/ipc/vfsd/blkd/ns/signal/mreact/ksem/kmon/cap/tty/musl registrate\n");
    uart_puts("[SYSCALL] fd_table: 0/1/2=VFS(/dev/std*) per 64 task slot\n");
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
