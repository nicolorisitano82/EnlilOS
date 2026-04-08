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

#define VFSD_HANDLE_MAX 64U

typedef struct {
    uint8_t  in_use;
    uint8_t  _pad0[3];
    vfs_file_t file;
} vfs_srv_handle_t;

typedef struct {
    uint8_t  type;
    uint8_t  _pad0;
    uint16_t flags;
    uint32_t remote_handle;
    vfs_file_t file;
} fd_entry_t;

/*
 * Indicizzata per [pid % SCHED_MAX_TASKS][fd].
 * Tutti i task partono con 0/1/2 preimpostati come CONSOLE.
 */
static fd_entry_t fd_tables[SCHED_MAX_TASKS][MAX_FD];
static vfs_srv_handle_t vfs_srv_tables[SCHED_MAX_TASKS][VFSD_HANDLE_MAX];

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
    e->remote_handle = 0U;
    e->file.mount = NULL;
    e->file.node_id = 0;
    e->file.flags = 0;
    e->file.pos = 0;
    e->file.size_hint = 0;
    e->file.dir_index = 0;
    e->file.cookie = 0;
}

static void fd_clone_task_table(int dst_idx, int src_idx)
{
    memcpy(&fd_tables[dst_idx][0], &fd_tables[src_idx][0], sizeof(fd_tables[0]));
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

static int fd_bind_remote(fd_entry_t *e, uint32_t handle, uint16_t flags)
{
    if (!e || handle == 0U)
        return -EINVAL;

    fd_clear(e);
    e->type = FD_TYPE_VFSD;
    e->flags = flags;
    e->remote_handle = handle;
    return 0;
}

static int fd_bind_remote_shadow(fd_entry_t *e, const char *path, uint16_t flags)
{
    uint16_t shadow_flags;

    if (!e || !path)
        return -EINVAL;

    /*
     * Il file remoto e' gia' stato aperto/creato/troncato tramite vfsd.
     * Per il solo shadow locale usato da mmap/msync evitiamo side effect
     * duplicati come un secondo O_TRUNC/O_CREAT/O_APPEND.
     */
    shadow_flags = (uint16_t)(flags & (uint16_t)~(O_TRUNC | O_CREAT | O_APPEND));
    return vfs_open(path, shadow_flags, &e->file);
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
    for (size_t i = 0U; i + 1U < sizeof(req.u.path) && path[i] != '\0'; i++)
        req.u.path[i] = path[i];

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

    fd_entry_t *e = fd_get(fd);
    if (!e) return ERR(EBADF);
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

    if (e->type == FD_TYPE_VFSD) {
        rc = vfsd_proxy_write(e->remote_handle, src, (size_t)count);
    } else if (e->type == FD_TYPE_VFS) {
        rc = vfs_write(&e->file, src, count);
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

    fd_entry_t *e = fd_get(fd);
    if (!e) return ERR(EBADF);
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

    if (e->type == FD_TYPE_VFSD) {
        rc = vfsd_proxy_read(e->remote_handle, dst, (size_t)cnt);
    } else if (e->type == FD_TYPE_VFS) {
        rc = vfs_read(&e->file, dst, cnt);
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
    uint16_t    oflags   = (uint16_t)args[1];
    int         idx      = task_idx();
    int         rc;

    if (!path) return ERR(EFAULT);
    if (current_task && sched_task_is_user(current_task)) {
        rc = user_copy_cstr(path_uva, path_buf, sizeof(path_buf));
        if (rc < 0)
            return ERR(-rc);
        path_arg = path_buf;
    }

    int fd = fd_alloc();
    if (fd < 0) return ERR(ENFILE);

    if (vfsd_proxy_available()) {
        uint32_t remote_handle = 0U;

        rc = vfsd_proxy_open(path_arg, oflags, &remote_handle);
        if (rc < 0)
            return ERR(-rc);
        rc = fd_bind_remote(&fd_tables[idx][fd], remote_handle, oflags);
        if (rc >= 0) {
            rc = fd_bind_remote_shadow(&fd_tables[idx][fd], path_arg, oflags);
            if (rc < 0) {
                (void)vfsd_proxy_close(remote_handle);
                fd_clear(&fd_tables[idx][fd]);
            }
        }
    } else {
        rc = fd_bind_path(&fd_tables[idx][fd], path_arg, oflags);
    }
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
    } else if (e->type == FD_TYPE_VFSD) {
        int rc = vfsd_proxy_close(e->remote_handle);
        if (e->file.mount)
            (void)vfs_close(&e->file);
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
    int       fd      = (int)args[0];
    uintptr_t buf_uva = (uintptr_t)args[1];
    stat_t   *buf     = (stat_t *)(uintptr_t)args[1];
    stat_t    st;
    int       rc;

    fd_entry_t *e = fd_get(fd);
    if (!e) return ERR(EBADF);
    if (!buf) return ERR(EFAULT);

    if (current_task && sched_task_is_user(current_task))
        buf = &st;

    if (e->type == FD_TYPE_VFSD)
        rc = vfsd_proxy_stat(e->remote_handle, buf);
    else if (e->type == FD_TYPE_VFS)
        rc = vfs_stat(&e->file, buf);
    else
        return ERR(EBADF);

    if (rc < 0) return ERR(-rc);
    if (buf == &st) {
        rc = user_store_bytes(buf_uva, &st, sizeof(st));
        if (rc < 0)
            return ERR(-rc);
    }
    return 0;
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
        fd_entry_t *e;
        vfs_file_t  f;
        uint32_t    vma_flags = 0U;
        uint32_t    mmu_prot;
        size_t      i;

        e = fd_get(fd);
        if (!e) return MAP_FAILED_VA;

        /* Copia il file handle e riposiziona all'offset richiesto */
        f     = e->file;
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
                           vma_flags, file_offset, &e->file);

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

    rc = elf64_load_from_path_exec(copy->path,
                                   copy->argv, copy->argc,
                                   copy->envp, copy->envc,
                                   &image);
    if (rc < 0) {
        kdebug_watchdog_resume();
        kfree(copy);
        return ERR(EIO);
    }

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
        if (signal_has_unblocked_pending(current_task))
            return ERR(EINTR);
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
    fd_entry_t   *e = fd_get(fd);
    uint32_t      copied = 0U;

    if (!e) return ERR(EBADF);
    if (!buf_uva) return ERR(EFAULT);
    if (max_entries == 0U) return 0;
    if (max_entries > 64U) max_entries = 64U;

    while (copied < max_entries) {
        vfs_dirent_t  ent;
        sys_dirent_t  out;
        int           rc;

        if (e->type == FD_TYPE_VFSD)
            rc = vfsd_proxy_readdir(e->remote_handle, &ent);
        else if (e->type == FD_TYPE_VFS)
            rc = vfs_readdir(&e->file, &ent);
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
    uint32_t pid = (uint32_t)args[0];
    int      sig = (int)args[1];
    int      rc;

    uart_puts("[SIGNAL] kill pid=");
    syscall_uart_put_u64((uint64_t)pid);
    uart_puts(" sig=");
    syscall_uart_put_i64((int64_t)sig);
    uart_puts("\n");

    rc = signal_send_pid(pid, sig);
    if (rc < 0) {
        uart_puts("[SIGNAL] kill fail pid=");
        syscall_uart_put_u64((uint64_t)pid);
        uart_puts(" sig=");
        syscall_uart_put_i64((int64_t)sig);
        uart_puts(" rc=");
        syscall_uart_put_i64((int64_t)rc);
        uart_puts("\n");
        return ERR(-rc);
    }
    return 0;
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

    uart_puts("[SYSCALL] 61 syscall base/UX/ipc/vfsd/blkd/signal/mreact/ksem/kmon/cap registrate\n");
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
