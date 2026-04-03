/*
 * EnlilOS vfsd - bootstrap VFS server (M9-02 v1)
 *
 * Riceve richieste IPC sulla porta "vfs" e le traduce verso il backend
 * VFS kernel-side tramite syscall bootstrap dedicate. In M9-03/M9-04 questo
 * strato restera' user-space, mentre il backend verra' spostato fuori dal kernel.
 */

#include "microkernel.h"
#include "syscall.h"
#include "vfs_ipc.h"

typedef unsigned long  u64;
typedef signed long    s64;
typedef unsigned int   u32;
typedef unsigned char  u8;

static long sys_call1(long nr, long a0)
{
    register long x0 asm("x0") = a0;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x8)
                 : "memory");
    return x0;
}

static long sys_call2(long nr, long a0, long a1)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x8)
                 : "memory");
    return x0;
}

static long sys_call3(long nr, long a0, long a1, long a2)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x8)
                 : "memory");
    return x0;
}

static long sys_call4(long nr, long a0, long a1, long a2, long a3)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    register long x8 asm("x8") = nr;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                 : "memory");
    return x0;
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    register long x0 asm("x0") = code;
    register long x8 asm("x8") = SYS_EXIT;

    asm volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
    for (;;)
        asm volatile("wfe");
}

static u32 vfsd_strlen(const char *s)
{
    u32 n = 0U;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static void vfsd_puts(const char *s)
{
    (void)sys_call3(SYS_WRITE, 1, (long)s, (long)vfsd_strlen(s));
}

static long sys_port_lookup_name(const char *name)
{
    return sys_call1(SYS_PORT_LOOKUP, (long)name);
}

static long sys_ipc_wait_msg(u32 port_id, ipc_message_t *msg)
{
    return sys_call2(SYS_IPC_WAIT, (long)port_id, (long)msg);
}

static long sys_ipc_reply_msg(u32 port_id, u32 type, const void *buf, u32 len)
{
    return sys_call4(SYS_IPC_REPLY, (long)port_id, (long)type, (long)buf, (long)len);
}

static long sys_vfs_boot_open_now(const char *path, u32 flags)
{
    return sys_call2(SYS_VFS_BOOT_OPEN, (long)path, (long)flags);
}

static long sys_vfs_boot_read_now(u32 handle, void *buf, u32 len)
{
    return sys_call3(SYS_VFS_BOOT_READ, (long)handle, (long)buf, (long)len);
}

static long sys_vfs_boot_write_now(u32 handle, const void *buf, u32 len)
{
    return sys_call3(SYS_VFS_BOOT_WRITE, (long)handle, (long)buf, (long)len);
}

static long sys_vfs_boot_readdir_now(u32 handle, vfs_dirent_t *ent)
{
    return sys_call2(SYS_VFS_BOOT_READDIR, (long)handle, (long)ent);
}

static long sys_vfs_boot_stat_now(u32 handle, stat_t *st)
{
    return sys_call2(SYS_VFS_BOOT_STAT, (long)handle, (long)st);
}

static long sys_vfs_boot_close_now(u32 handle)
{
    return sys_call1(SYS_VFS_BOOT_CLOSE, (long)handle);
}

static void vfsd_bzero(void *ptr, u64 len)
{
    u8 *p = (u8 *)ptr;
    while (len-- > 0U)
        *p++ = 0U;
}

static void vfsd_reply_error(u32 port_id, long rc)
{
    vfsd_response_t resp;

    vfsd_bzero(&resp, sizeof(resp));
    resp.status = (int)rc;
    (void)sys_ipc_reply_msg(port_id, IPC_MSG_VFS_RESP, &resp, (u32)sizeof(resp));
}

void _start(void)
{
    static const char port_name[] = "vfs";
    ipc_message_t     msg;
    u32               port_id;
    long              rc;

    rc = sys_port_lookup_name(port_name);
    if (rc < 0) {
        vfsd_puts("[VFSD] port lookup fallita\n");
        sys_exit_now(1);
    }

    port_id = (u32)rc;
    vfsd_puts("[VFSD] online\n");

    for (;;) {
        const vfsd_request_t *req;
        vfsd_response_t       resp;

        rc = sys_ipc_wait_msg(port_id, &msg);
        if (rc < 0)
            continue;
        if (msg.msg_type != IPC_MSG_VFS_REQ)
            continue;
        if (msg.msg_len < (u32)sizeof(vfsd_request_t)) {
            vfsd_reply_error(port_id, -(long)EINVAL);
            continue;
        }

        req = (const vfsd_request_t *)msg.payload;
        vfsd_bzero(&resp, sizeof(resp));

        switch (req->op) {
        case VFSD_REQ_OPEN:
            rc = sys_vfs_boot_open_now(req->u.path, req->flags);
            if (rc < 0)
                resp.status = (int)rc;
            else {
                resp.status = 0;
                resp.handle = (int)rc;
            }
            break;
        case VFSD_REQ_READ:
            rc = sys_vfs_boot_read_now((u32)req->handle, resp.u.data,
                                       (req->count > VFSD_IO_BYTES) ? VFSD_IO_BYTES : req->count);
            if (rc < 0)
                resp.status = (int)rc;
            else {
                resp.status = 0;
                resp.data_len = (u32)rc;
            }
            break;
        case VFSD_REQ_WRITE:
            rc = sys_vfs_boot_write_now((u32)req->handle, req->u.data,
                                        (req->count > VFSD_IO_BYTES) ? VFSD_IO_BYTES : req->count);
            if (rc < 0)
                resp.status = (int)rc;
            else {
                resp.status = 0;
                resp.data_len = (u32)rc;
            }
            break;
        case VFSD_REQ_READDIR:
            rc = sys_vfs_boot_readdir_now((u32)req->handle, &resp.u.dirent);
            resp.status = (int)rc;
            break;
        case VFSD_REQ_STAT:
            rc = sys_vfs_boot_stat_now((u32)req->handle, &resp.u.st);
            resp.status = (int)rc;
            break;
        case VFSD_REQ_CLOSE:
            rc = sys_vfs_boot_close_now((u32)req->handle);
            resp.status = (int)rc;
            break;
        default:
            resp.status = -(int)ENOSYS;
            break;
        }

        (void)sys_ipc_reply_msg(port_id, IPC_MSG_VFS_RESP, &resp, (u32)sizeof(resp));
    }
}
