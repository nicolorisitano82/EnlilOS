/*
 * EnlilOS Microkernel — Kernel Socket Layer (M10-03)
 *
 * BSD socket v1: SOCK_STREAM + SOCK_DGRAM su loopback 127.0.0.1.
 * Connessioni esterne → ENETUNREACH.
 *
 * Indirizzi e porte memorizzati in host byte order internamente.
 * Conversione NBO←→HBO al confine syscall (in kernel/syscall.c).
 */

#ifndef ENLILOS_SOCK_H
#define ENLILOS_SOCK_H

#include "types.h"
#include "sched.h"

/* ── Dimensioni pool ───────────────────────────────────────────── */

#define SOCK_MAX_GLOBAL     32U
#define SOCK_RX_BUF_SIZE    4096U
#define SOCK_RX_MASK        (SOCK_RX_BUF_SIZE - 1U)
#define SOCK_ACCEPT_MAX     8U   /* deve essere potenza di 2 */
#define SOCK_UDP_QUEUE_MAX  4U
#define SOCK_UDP_DATA_MAX   512U

/* Indice "assente" per peer_idx */
#define SOCK_IDX_NONE       0xFFFFU

/* Indirizzi in host byte order (AArch64 LE) */
#define SOCK_LOOPBACK_IP    0x7F000001U   /* 127.0.0.1 */
#define SOCK_ANY_IP         0x00000000U   /* 0.0.0.0   */

/* ── Socket type / domain / protocol ──────────────────────────── */

#define SOCK_STREAM         1
#define SOCK_DGRAM          2
#define SOCK_NONBLOCK       04000
#define SOCK_CLOEXEC        02000000

#define AF_UNIX             1
#define AF_INET             2

#define IPPROTO_TCP         6
#define IPPROTO_UDP         17

/* ── Stati ─────────────────────────────────────────────────────── */

#define SOCK_STATE_FREE         0U
#define SOCK_STATE_CLOSED       1U
#define SOCK_STATE_BOUND        2U
#define SOCK_STATE_LISTENING    3U
#define SOCK_STATE_CONNECTED    4U
#define SOCK_STATE_CLOSE_WAIT   5U

/* ── Flag interni ──────────────────────────────────────────────── */

#define SOCK_FL_NONBLOCK    0x0001U
#define SOCK_FL_REUSEADDR   0x0002U
#define SOCK_FL_PEER_CLOSE  0x0004U   /* peer ha chiuso la connessione */

/* ── shutdown(how) ─────────────────────────────────────────────── */

#define SHUT_RD     0
#define SHUT_WR     1
#define SHUT_RDWR   2

#define MSG_DONTWAIT        0x40
#define MSG_NOSIGNAL        0x4000

/* ── setsockopt/getsockopt ─────────────────────────────────────── */

#define SOL_SOCKET      1
#define SO_REUSEADDR    2
#define SO_KEEPALIVE    9
#define SO_SNDBUF       7
#define SO_RCVBUF       8
#define SO_ERROR        4

#define IPPROTO_TCP_OPT IPPROTO_TCP
#define TCP_NODELAY     1

/* ── Datagramma UDP in coda ────────────────────────────────────── */

typedef struct {
    uint32_t src_ip;    /* host byte order */
    uint16_t src_port;  /* host byte order */
    uint16_t len;
    uint8_t  data[SOCK_UDP_DATA_MAX];
} sock_udp_dgram_t;

/* ════════════════════════════════════════════════════════════════
 * sock_t — descrittore socket globale
 * ════════════════════════════════════════════════════════════════ */
typedef struct {
    /* ── identificazione ── */
    uint8_t    in_use;
    uint8_t    domain;
    uint8_t    type;     /* SOCK_STREAM / SOCK_DGRAM */
    uint8_t    state;
    uint16_t   flags;    /* SOCK_FL_* */
    uint16_t   _pad0;

    /* ── indirizzo locale ── */
    uint32_t   local_ip;   /* host byte order */
    uint16_t   local_port; /* host byte order */
    uint16_t   _pad1;

    /* ── indirizzo remoto / peer ── */
    uint32_t   peer_ip;    /* host byte order */
    uint16_t   peer_port;  /* host byte order */
    uint16_t   peer_idx;   /* indice in g_socks, SOCK_IDX_NONE se assente */

    /* ── accept queue (server LISTEN) ── */
    uint16_t   accept_q[SOCK_ACCEPT_MAX];
    uint8_t    accept_head;
    uint8_t    accept_tail;
    uint16_t   _pad2;

    /* ── rx ring TCP (stream) ── */
    uint32_t   rx_head;    /* monotono crescente, indice mod SOCK_RX_MASK */
    uint32_t   rx_tail;
    uint8_t    rx_buf[SOCK_RX_BUF_SIZE];

    /* ── coda UDP (datagrammi) ── */
    uint8_t    udp_head;
    uint8_t    udp_tail;
    uint8_t    _pad3[2];
    sock_udp_dgram_t udp_q[SOCK_UDP_QUEUE_MAX];

    /* ── waiter per spin-yield blocking ── */
    /* (non usati in v1 — il blocking usa sched_yield loop come le pipe) */
    uint8_t    _reserved[8];
} sock_t;

/* ── API kernel ────────────────────────────────────────────────── */

void    sock_init(void);
int     sock_alloc(void);
void    sock_free(int idx);
sock_t *sock_get(int idx);

int     sock_do_socket(int domain, int type, int protocol);
int     sock_do_bind(int idx, uint32_t ip, uint16_t port);
int     sock_do_listen(int idx, int backlog);
int     sock_do_accept(int idx, uint32_t *out_ip, uint16_t *out_port);
int     sock_do_connect(int idx, uint32_t ip, uint16_t port);
ssize_t sock_do_send(int idx, const void *buf, size_t len, int flags);
ssize_t sock_do_recv(int idx, void *buf, size_t len, int flags);
ssize_t sock_do_sendto(int idx, const void *buf, size_t len,
                       uint32_t dst_ip, uint16_t dst_port, int flags);
ssize_t sock_do_recvfrom(int idx, void *buf, size_t len,
                         uint32_t *src_ip, uint16_t *src_port, int flags);
int     sock_do_shutdown(int idx, int how);

#endif /* ENLILOS_SOCK_H */
