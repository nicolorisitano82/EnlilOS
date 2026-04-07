/*
 * EnlilOS blkd - bootstrap Block server (M9-03 v1)
 *
 * Riceve richieste IPC sulla porta "block" e le traduce in chiamate
 * al driver virtio-blk kernel-side tramite syscall blk_boot_*.
 *
 * Pattern identico a vfsd.c (M9-02):
 *   - Lookup porta "block" al boot
 *   - Loop ipc_wait / ipc_reply
 *   - Syscall blk_boot_read/write/flush/sectors per accesso al driver
 *
 * In M9-04+ questo server potrà essere sostituito da un driver puro
 * user-space con accesso diretto MMIO tramite CAP_MMIO.
 */

#include "blk_ipc.h"
#include "microkernel.h"
#include "syscall.h"
#include "user_svc.h"

typedef unsigned long  u64;
typedef unsigned int   u32;
typedef signed int     s32;
typedef unsigned char  u8;

/* ── Helpers syscall ─────────────────────────────────────────────── */

static long sys_write(long fd, const void *buf, long len)
{
    return user_svc3(SYS_WRITE, fd, (long)buf, len);
}

static long sys_port_lookup(const char *name)
{
    return user_svc1(SYS_PORT_LOOKUP, (long)name);
}

static long sys_ipc_wait(u32 port_id, ipc_message_t *msg)
{
    return user_svc2(SYS_IPC_WAIT, (long)port_id, (long)msg);
}

static long sys_ipc_reply(u32 port_id, u32 type, const void *buf, u32 len)
{
    return user_svc4(SYS_IPC_REPLY, (long)port_id, (long)type, (long)buf, (long)len);
}

static long sys_blk_boot_read(u64 sector, u32 count, void *buf)
{
    return user_svc3(SYS_BLK_BOOT_READ, (long)sector, (long)count, (long)buf);
}

static long sys_blk_boot_write(u64 sector, u32 count, const void *buf)
{
    return user_svc3(SYS_BLK_BOOT_WRITE, (long)sector, (long)count, (long)buf);
}

static long sys_blk_boot_flush(void)
{
    return user_svc0(SYS_BLK_BOOT_FLUSH);
}

static long sys_blk_boot_sectors(void)
{
    return user_svc0(SYS_BLK_BOOT_SECTORS);
}

static __attribute__((noreturn)) void sys_exit_now(long code)
{
    user_svc_exit(code, SYS_EXIT);
}

/* ── UART helper minimale ─────────────────────────────────────────── */

static u32 blkd_strlen(const char *s)
{
    u32 n = 0U;
    while (s && s[n] != '\0')
        n++;
    return n;
}

static void blkd_puts(const char *s)
{
    (void)sys_write(1, s, (long)blkd_strlen(s));
}

/* ── Reply helpers ────────────────────────────────────────────────── */

static void blkd_reply_error(u32 port_id, s32 status)
{
    blkd_response_t resp;
    u32 i;

    for (i = 0U; i < (u32)sizeof(resp); i++)
        ((u8 *)&resp)[i] = 0U;
    resp.status = status;
    (void)sys_ipc_reply(port_id, IPC_MSG_BLK_RESP, &resp, (u32)sizeof(resp));
}

/* ── Buffer I/O statico (non transita nell'IPC) ───────────────────── */

static u8 blkd_io_buf[BLKD_MAX_SECTORS * BLKD_SECTOR_SIZE];

/* ── Entry point ──────────────────────────────────────────────────── */

void _start(void)
{
    ipc_message_t msg;
    u32           port_id;
    long          rc;

    rc = sys_port_lookup("block");
    if (rc < 0) {
        blkd_puts("[BLKD] port lookup fallita\n");
        sys_exit_now(1);
    }

    port_id = (u32)rc;
    blkd_puts("[BLKD] User-space block server pronto: port=block\n");

    for (;;) {
        const blkd_request_t *req;
        blkd_response_t       resp;
        u32 i;

        rc = sys_ipc_wait(port_id, &msg);
        if (rc < 0)
            continue;
        if (msg.msg_type != IPC_MSG_BLK_REQ)
            continue;
        if (msg.msg_len < (u32)sizeof(blkd_request_t)) {
            blkd_reply_error(port_id, -(s32)EINVAL);
            continue;
        }

        req = (const blkd_request_t *)msg.payload;

        for (i = 0U; i < (u32)sizeof(resp); i++)
            ((u8 *)&resp)[i] = 0U;

        switch (req->op) {

        case BLKD_REQ_READ:
            if (req->count == 0U || req->count > BLKD_MAX_SECTORS) {
                resp.status = -(s32)EINVAL;
                break;
            }
            rc = sys_blk_boot_read(req->sector, req->count, blkd_io_buf);
            resp.status = (rc == 0) ? 0 : -(s32)EIO;
            if (rc == 0)
                resp.value = (u64)req->count * BLKD_SECTOR_SIZE;
            break;

        case BLKD_REQ_WRITE:
            if (req->count == 0U || req->count > BLKD_MAX_SECTORS) {
                resp.status = -(s32)EINVAL;
                break;
            }
            rc = sys_blk_boot_write(req->sector, req->count, blkd_io_buf);
            resp.status = (rc == 0) ? 0 : -(s32)EIO;
            break;

        case BLKD_REQ_FLUSH:
            rc = sys_blk_boot_flush();
            resp.status = (rc == 0) ? 0 : -(s32)EIO;
            break;

        case BLKD_REQ_SECTORS:
            rc = sys_blk_boot_sectors();
            if (rc < 0) {
                resp.status = -(s32)EIO;
            } else {
                resp.status = 0;
                resp.value  = (u64)rc;
            }
            break;

        default:
            resp.status = -(s32)ENOSYS;
            break;
        }

        (void)sys_ipc_reply(port_id, IPC_MSG_BLK_RESP, &resp, (u32)sizeof(resp));
    }
}
