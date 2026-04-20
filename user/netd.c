/*
 * EnlilOS netd – bootstrap network server (M10-01 / M10-02 / M11-05)
 *
 * v1 responsibilities:
 *   - Raw Ethernet drain via SYS_NET_BOOT_RECV
 *   - TCP/IP stack bring-up via net_stack.c
 *   - IPC server for outbound AF_INET stream sockets (non-loopback)
 */

#include "microkernel.h"
#include "net_ipc.h"
#include "net_stack.h"

/* ── Syscall numbers (kept local to stay freestanding) ───────────── */
#define SYS_WRITE          1
#define SYS_EXIT           3
#define SYS_YIELD         20
#define SYS_PORT_LOOKUP  140
#define SYS_IPC_WAIT     141
#define SYS_IPC_REPLY    142
#define SYS_IPC_POLL     143
#define SYS_NET_BOOT_SEND 162
#define SYS_NET_BOOT_RECV 163
#define SYS_NET_BOOT_INFO 164

/* ── Socket constants (copied from sock.h/sys/socket.h) ──────────── */
#define NETD_AF_INET        2
#define NETD_SOCK_STREAM    1
#define NETD_SOL_SOCKET     1
#define NETD_SO_REUSEADDR   2
#define NETD_SO_KEEPALIVE   9
#define NETD_SO_ERROR       4
#define NETD_TCP_NODELAY    1
#define NETD_IPPROTO_TCP    6
#define NETD_SHUT_RD        0
#define NETD_SHUT_WR        1
#define NETD_SHUT_RDWR      2
#define NETD_MSG_DONTWAIT   0x40

/* poll flags (Linux-compatible subset) */
#define NETD_POLLIN         0x0001U
#define NETD_POLLOUT        0x0004U
#define NETD_POLLERR        0x0008U
#define NETD_POLLHUP        0x0010U

/* errno subset */
#define NETD_EPERM          1
#define NETD_ENOENT         2
#define NETD_EINTR          4
#define NETD_EIO            5
#define NETD_EBADF          9
#define NETD_EAGAIN         11
#define NETD_EFAULT         14
#define NETD_EBUSY          16
#define NETD_EINVAL         22
#define NETD_ENFILE         23
#define NETD_ENOTSOCK       88
#define NETD_ENOPROTOOPT    92
#define NETD_EOPNOTSUPP     95
#define NETD_EAFNOSUPPORT   97
#define NETD_EADDRINUSE     98
#define NETD_EADDRNOTAVAIL  99
#define NETD_ENETUNREACH   101
#define NETD_ECONNRESET    104
#define NETD_ENOBUFS       105
#define NETD_EISCONN       106
#define NETD_ENOTCONN      107
#define NETD_ETIMEDOUT     110
#define NETD_ECONNREFUSED  111
#define NETD_EINPROGRESS   115

#define NET_FRAME_MAX      1536U
#define NETD_HANDLE_MAX      8U
#define NETD_IO_MAX       NETD_IO_BYTES

typedef struct {
    ns_u8  mac[6];
    ns_u8  has_mac;
    ns_u8  has_status;
    ns_u16 status;
    ns_u16 mtu;
    ns_u8  link_up;
    ns_u8  reserved0;
    ns_u16 reserved1;
    ns_u32 rx_packets;
    ns_u32 tx_packets;
    ns_u32 rx_drops;
    ns_u32 tx_errors;
} netd_info_t;

typedef struct {
    ns_u8  in_use;
    ns_u8  type;
    ns_u8  shut_rd;
    ns_u8  shut_wr;
    ns_u16 local_port;
    ns_u16 remote_port;
    ns_u32 local_ip;
    ns_u32 remote_ip;
    int    conn;
    int    so_error;
} netd_sock_t;

static netd_sock_t g_socks[NETD_HANDLE_MAX];

/* ── SVC wrappers ─────────────────────────────────────────────────── */

static long __attribute__((noinline))
svc1(long nr, long a0)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}

static long __attribute__((noinline))
svc2(long nr, long a0, long a1)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

static long __attribute__((noinline))
svc3(long nr, long a0, long a1, long a2)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
}

static long __attribute__((noinline))
svc4(long nr, long a0, long a1, long a2, long a3)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    register long x3 __asm__("x3") = a3;
    __asm__ volatile("svc #0" : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                     : "memory");
    return x0;
}

/* ── Tiny helpers ─────────────────────────────────────────────────── */

static void *netd_memcpy(void *dst, const void *src, ns_u32 n)
{
    ns_u8       *d = (ns_u8 *)dst;
    const ns_u8 *s = (const ns_u8 *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *netd_memset(void *dst, int val, ns_u32 n)
{
    ns_u8 *d = (ns_u8 *)dst;
    while (n--) *d++ = (ns_u8)val;
    return dst;
}

static void netd_puts(const char *s)
{
    ns_u32 n = 0U;
    while (s[n]) n++;
    (void)svc3(SYS_WRITE, 1, (long)s, (long)n);
}

static void netd_output(const ns_u8 *frame, ns_u32 len)
{
    (void)svc2(SYS_NET_BOOT_SEND, (long)frame, (long)len);
}

static netd_sock_t *netd_sock_get(int handle)
{
    if (handle <= 0 || handle > (int)NETD_HANDLE_MAX)
        return (netd_sock_t *)0;
    if (!g_socks[handle - 1].in_use)
        return (netd_sock_t *)0;
    return &g_socks[handle - 1];
}

static int netd_sock_alloc(int type)
{
    for (int i = 0; i < (int)NETD_HANDLE_MAX; i++) {
        if (g_socks[i].in_use)
            continue;
        netd_memset(&g_socks[i], 0, sizeof(g_socks[i]));
        g_socks[i].in_use = 1U;
        g_socks[i].type   = (ns_u8)type;
        g_socks[i].conn   = -1;
        return i + 1;
    }
    return -1;
}

static void netd_sock_release(netd_sock_t *sock)
{
    if (!sock || !sock->in_use)
        return;
    if (sock->conn >= 0)
        net_stack_tcp_release(sock->conn);
    netd_memset(sock, 0, sizeof(*sock));
}

static void netd_sock_refresh(netd_sock_t *sock)
{
    net_stack_tcp_info_t info;

    if (!sock || !sock->in_use || sock->conn < 0)
        return;
    if (net_stack_tcp_info(sock->conn, &info) < 0)
        return;

    sock->local_ip    = info.local_ip;
    sock->local_port  = info.local_port;
    sock->remote_ip   = info.remote_ip;
    sock->remote_port = info.remote_port;
    if (info.so_error != 0 && sock->so_error == 0)
        sock->so_error = info.so_error;
}

static ns_u32 netd_sock_pollmask(netd_sock_t *sock)
{
    net_stack_tcp_info_t info;
    ns_u32               mask = 0U;

    if (!sock || !sock->in_use)
        return NETD_POLLERR;

    if (sock->conn < 0) {
        if (sock->so_error != 0)
            return NETD_POLLERR | NETD_POLLHUP;
        return 0U;
    }

    if (net_stack_tcp_info(sock->conn, &info) < 0)
        return NETD_POLLERR;

    if (info.so_error != 0 || sock->so_error != 0)
        mask |= NETD_POLLERR;
    if (info.state == NS_TCP_ESTABLISHED) {
        mask |= NETD_POLLOUT;
        if (info.rx_len > 0U)
            mask |= NETD_POLLIN;
    }
    if (info.state == NS_TCP_CLOSED || info.peer_closed) {
        if (info.rx_len > 0U)
            mask |= NETD_POLLIN;
        mask |= NETD_POLLHUP;
    }
    return mask;
}

static int netd_sock_connect(netd_sock_t *sock, ns_u32 dst_ip, ns_u16 dst_port)
{
    net_stack_tcp_info_t info;

    if (!sock || !sock->in_use)
        return -NETD_EBADF;
    if (sock->type != NETD_SOCK_STREAM)
        return -NETD_EOPNOTSUPP;
    if (sock->conn >= 0) {
        if (net_stack_tcp_info(sock->conn, &info) < 0)
            return -NETD_EIO;
        if (info.state == NS_TCP_ESTABLISHED)
            return 0;
        if (sock->so_error != 0)
            return -sock->so_error;
        return -NETD_EINPROGRESS;
    }

    if (dst_ip == 0U || dst_ip == 0x7F000001U)
        return -NETD_ENETUNREACH;

    sock->conn = net_stack_tcp_connect(dst_ip, dst_port, sock->local_port,
                                       (tcp_data_cb)0, (tcp_closed_cb)0);
    if (sock->conn < 0)
        return -NETD_EAGAIN;

    sock->remote_ip   = dst_ip;
    sock->remote_port = dst_port;
    netd_sock_refresh(sock);

    if (net_stack_tcp_info(sock->conn, &info) == 0 &&
        info.state == NS_TCP_ESTABLISHED)
        return 0;
    return -NETD_EINPROGRESS;
}

static int netd_sock_send(netd_sock_t *sock, const ns_u8 *buf, ns_u32 len)
{
    int sent;

    if (!sock || !sock->in_use || !buf)
        return -NETD_EBADF;
    if (sock->conn < 0)
        return (sock->so_error != 0) ? -sock->so_error : -NETD_ENOTCONN;

    netd_sock_refresh(sock);
    if ((netd_sock_pollmask(sock) & NETD_POLLERR) != 0U && sock->so_error != 0)
        return -sock->so_error;

    sent = net_stack_tcp_send(sock->conn, buf, (ns_u16)len);
    if (sent < 0)
        return -NETD_EAGAIN;
    return sent;
}

static int netd_sock_recv(netd_sock_t *sock, ns_u8 *buf, ns_u32 maxlen)
{
    int recvd;

    if (!sock || !sock->in_use || !buf)
        return -NETD_EBADF;
    if (sock->conn < 0)
        return (sock->so_error != 0) ? -sock->so_error : -NETD_ENOTCONN;

    netd_sock_refresh(sock);
    recvd = net_stack_tcp_recv(sock->conn, buf, (ns_u16)maxlen);
    if (recvd < 0)
        return -NETD_EAGAIN;
    return recvd;
}

static int netd_reply(uint32_t port_id, const netd_response_t *resp)
{
    return (int)svc4(SYS_IPC_REPLY, (long)port_id, IPC_MSG_NET_RESP,
                     (long)resp, sizeof(*resp));
}

static void netd_handle_request(uint32_t port_id, const ipc_message_t *msg)
{
    const netd_request_t *req = (const netd_request_t *)msg->payload;
    netd_response_t       resp;
    netd_sock_t          *sock;
    ns_u32                copy_len;
    int                   rc = -NETD_EINVAL;

    netd_memset(&resp, 0, sizeof(resp));

    if (!msg || msg->msg_type != IPC_MSG_NET_REQ || msg->msg_len < sizeof(*req)) {
        resp.status = -NETD_EINVAL;
        (void)netd_reply(port_id, &resp);
        return;
    }

    switch (req->op) {
    case NETD_REQ_SOCKET:
        if ((int)req->arg0 != NETD_AF_INET || (int)req->arg1 != NETD_SOCK_STREAM) {
            resp.status = -NETD_EAFNOSUPPORT;
            break;
        }
        rc = netd_sock_alloc((int)req->arg1);
        if (rc < 0) {
            resp.status = -NETD_ENFILE;
            break;
        }
        resp.status = 0;
        resp.handle = rc;
        break;

    case NETD_REQ_BIND:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        if (req->arg0 != 0U && req->arg0 != NET_STACK_IP) {
            resp.status = -NETD_EADDRNOTAVAIL;
            break;
        }
        sock->local_ip = (req->arg0 != 0U) ? req->arg0 : NET_STACK_IP;
        sock->local_port = (ns_u16)req->arg1;
        resp.status = 0;
        break;

    case NETD_REQ_CONNECT:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        resp.status = netd_sock_connect(sock, req->arg0, (ns_u16)req->arg1);
        if (resp.status == 0) {
            netd_sock_refresh(sock);
            resp.addr_ip   = sock->local_ip;
            resp.addr_port = sock->local_port;
            resp.aux_ip    = sock->remote_ip;
            resp.aux_port  = sock->remote_port;
        }
        break;

    case NETD_REQ_SEND:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        copy_len = (req->count > NETD_IO_MAX) ? NETD_IO_MAX : req->count;
        resp.status = netd_sock_send(sock, req->data, copy_len);
        if (resp.status >= 0)
            resp.data_len = (ns_u32)resp.status;
        break;

    case NETD_REQ_RECV:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        copy_len = (req->count > NETD_IO_MAX) ? NETD_IO_MAX : req->count;
        rc = netd_sock_recv(sock, resp.data, copy_len);
        resp.status = rc;
        if (rc >= 0)
            resp.data_len = (ns_u32)rc;
        break;

    case NETD_REQ_CLOSE:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        if (sock->conn >= 0)
            net_stack_tcp_close(sock->conn);
        netd_sock_release(sock);
        resp.status = 0;
        break;

    case NETD_REQ_POLL:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        netd_sock_refresh(sock);
        resp.status = 0;
        resp.flags  = netd_sock_pollmask(sock);
        break;

    case NETD_REQ_GETSOCKOPT:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        if ((int)req->arg0 == NETD_SOL_SOCKET && (int)req->arg1 == NETD_SO_ERROR) {
            int soerr = sock->so_error;
            netd_memcpy(resp.data, &soerr, sizeof(soerr));
            sock->so_error = 0;
            resp.status = 0;
            resp.data_len = sizeof(soerr);
            break;
        }
        resp.status = -NETD_ENOPROTOOPT;
        break;

    case NETD_REQ_SETSOCKOPT:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        if (((int)req->arg0 == NETD_SOL_SOCKET &&
             ((int)req->arg1 == NETD_SO_REUSEADDR || (int)req->arg1 == NETD_SO_KEEPALIVE)) ||
            ((int)req->arg0 == NETD_IPPROTO_TCP && (int)req->arg1 == NETD_TCP_NODELAY)) {
            resp.status = 0;
            break;
        }
        resp.status = -NETD_ENOPROTOOPT;
        break;

    case NETD_REQ_ADDR:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        netd_sock_refresh(sock);
        resp.status    = 0;
        resp.addr_ip   = (req->arg0 != 0U) ? sock->remote_ip : sock->local_ip;
        resp.addr_port = (req->arg0 != 0U) ? sock->remote_port : sock->local_port;
        break;

    case NETD_REQ_SHUTDOWN:
        sock = netd_sock_get(req->handle);
        if (!sock) {
            resp.status = -NETD_EBADF;
            break;
        }
        if ((int)req->arg0 == NETD_SHUT_RD || (int)req->arg0 == NETD_SHUT_RDWR)
            sock->shut_rd = 1U;
        if ((int)req->arg0 == NETD_SHUT_WR || (int)req->arg0 == NETD_SHUT_RDWR)
            sock->shut_wr = 1U;
        if (sock->conn >= 0 && sock->shut_wr)
            net_stack_tcp_close(sock->conn);
        resp.status = 0;
        break;

    default:
        resp.status = -NETD_EINVAL;
        break;
    }

    (void)netd_reply(port_id, &resp);
}

void _start(void)
{
    static ns_u8       rx_buf[NET_FRAME_MAX];
    static netd_info_t info;
    static ipc_message_t msg;
    long               rc;
    long               port_id;
    ns_u32             idle = 0U;
    ns_u8              did_work;

    port_id = svc1(SYS_PORT_LOOKUP, (long)"net");
    if (port_id < 0) {
        netd_puts("[NETD] porta net non trovata\n");
        (void)svc1(SYS_EXIT, 1);
        __builtin_unreachable();
    }

    rc = svc1(SYS_NET_BOOT_INFO, (long)&info);
    if (rc < 0) {
        netd_puts("[NETD] driver bootstrap non disponibile\n");
        (void)svc1(SYS_EXIT, 1);
        __builtin_unreachable();
    }

    net_stack_init(info.mac, netd_output);
    net_stack_send_garp();
    netd_puts("[NETD] stack TCP/IP v1 online (10.0.2.15/24)\n");

    for (;;) {
        did_work = 0U;

        rc = svc2(SYS_NET_BOOT_RECV, (long)rx_buf, (long)NET_FRAME_MAX);
        if (rc > 0) {
            did_work = 1U;
            net_stack_input(rx_buf, (ns_u32)rc);
        }

        rc = svc2(SYS_IPC_POLL, port_id, (long)&msg);
        if (rc == 0) {
            netd_handle_request((ns_u32)port_id, &msg);
            did_work = 1U;
        }

        if (did_work) {
            idle = 0U;
            continue;
        }

        idle++;
        if ((idle & 0x0FU) == 0U)
            (void)svc1(SYS_YIELD, 0);
    }
}
