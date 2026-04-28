/*
 * EnlilOS Microkernel — Kernel Socket Layer v1 (M10-03)
 *
 * Implementa:
 *   - Pool globale di 32 socket (sock_t g_socks[SOCK_MAX_GLOBAL])
 *   - SOCK_STREAM (TCP loopback) + SOCK_DGRAM (UDP loopback)
 *   - Solo loopback 127.0.0.1; connessioni esterne → ENETUNREACH
 *   - Blocking recv/accept con sched_yield() cooperativo (come le pipe)
 *   - AF_UNIX stream socket (M12-01): bind/connect/accept by path
 *
 * Tutti gli indirizzi e le porte sono in host byte order.
 * La conversione NBO←→HBO avviene nei handler syscall in kernel/syscall.c.
 */

#include "sock.h"
#include "sched.h"
#include "syscall.h"
#include "uart.h"

extern void *memset(void *dst, int value, size_t n);
extern void *memcpy(void *dst, const void *src, size_t n);

static size_t sock_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static int sock_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static char *sock_strncpy(char *dst, const char *src, size_t n)
{
    size_t i;
    for (i = 0U; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

/* ════════════════════════════════════════════════════════════════
 * Pool globale
 * ════════════════════════════════════════════════════════════════ */

static sock_t g_socks[SOCK_MAX_GLOBAL];

/* Contatore porta efimera (host byte order). RFC 6335: 49152–65535. */
static uint16_t g_ephemeral = 49152U;

/* ── AF_UNIX path table ───────────────────────────────────────── */
typedef struct {
    char     path[SOCK_UNIX_PATH_MAX];
    uint16_t sock_idx;
    uint8_t  in_use;
} unix_path_entry_t;

static unix_path_entry_t g_unix_paths[SOCK_MAX_GLOBAL];

/* ── Helper interni ───────────────────────────────────────────── */

static uint16_t sock_alloc_ephemeral(void)
{
    uint16_t start = g_ephemeral;

    do {
        uint16_t p = g_ephemeral;
        g_ephemeral = (g_ephemeral == 65535U) ? 49152U : (g_ephemeral + 1U);

        /* verifica conflitto */
        int conflict = 0;
        for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
            if (g_socks[i].in_use && g_socks[i].local_port == p) {
                conflict = 1;
                break;
            }
        }
        if (!conflict)
            return p;

    } while (g_ephemeral != start);

    return 0U;   /* nessuna porta disponibile */
}

static int sock_port_in_use(uint16_t port, uint8_t type)
{
    for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
        if (g_socks[i].in_use && g_socks[i].type == type &&
            g_socks[i].local_port == port)
            return 1;
    }
    return 0;
}

static sock_t *sock_find_listener(uint8_t type, uint32_t ip, uint16_t port)
{
    for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
        sock_t *s = &g_socks[i];
        if (!s->in_use || s->type != type || s->state != SOCK_STATE_LISTENING)
            continue;
        if (s->local_port != port)
            continue;
        if (s->local_ip != SOCK_ANY_IP && s->local_ip != ip)
            continue;
        return s;
    }
    return NULL;
}

/* ── API pubblica ─────────────────────────────────────────────── */

void sock_init(void)
{
    memset(g_socks, 0, sizeof(g_socks));
    g_ephemeral = 49152U;
    uart_puts("[SOCK] Pool 32 socket v1 (loopback TCP/UDP) pronto\n");
}

int sock_alloc(void)
{
    for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
        if (!g_socks[i].in_use)
            return (int)i;
    }
    return -1;
}

void sock_free(int idx)
{
    sock_t *s;
    uint16_t pi;

    if (idx < 0 || idx >= (int)SOCK_MAX_GLOBAL)
        return;
    s = &g_socks[idx];
    if (!s->in_use)
        return;

    /* Notifica il peer (se connesso) che la connessione è chiusa */
    pi = s->peer_idx;
    if (pi != SOCK_IDX_NONE && pi < SOCK_MAX_GLOBAL) {
        sock_t *peer = &g_socks[pi];
        if (peer->in_use && peer->peer_idx == (uint16_t)idx) {
            peer->flags |= SOCK_FL_PEER_CLOSE;
        }
    }

    /* Libera le connessioni pending nella accept queue */
    while (s->accept_head != s->accept_tail) {
        uint16_t q_idx = s->accept_q[s->accept_head];
        s->accept_head = (s->accept_head + 1U) & (uint8_t)(SOCK_ACCEPT_MAX - 1U);
        if (q_idx < SOCK_MAX_GLOBAL && g_socks[q_idx].in_use) {
            sock_t *qs = &g_socks[q_idx];
            uint16_t cli = qs->peer_idx;
            if (cli < SOCK_MAX_GLOBAL && g_socks[cli].in_use)
                g_socks[cli].flags |= SOCK_FL_PEER_CLOSE;
            memset(qs, 0, sizeof(*qs));
        }
    }

    /* Rimuovi dal path table AF_UNIX se applicabile */
    if (s->domain == AF_UNIX && s->unix_path[0] != '\0') {
        for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
            if (g_unix_paths[i].in_use &&
                g_unix_paths[i].sock_idx == (uint16_t)idx) {
                g_unix_paths[i].in_use = 0U;
                break;
            }
        }
    }

    memset(s, 0, sizeof(*s));
}

sock_t *sock_get(int idx)
{
    if (idx < 0 || idx >= (int)SOCK_MAX_GLOBAL)
        return NULL;
    return g_socks[idx].in_use ? &g_socks[idx] : NULL;
}

/* ── Operazioni socket ────────────────────────────────────────── */

/* ════════════════════════════════════════════════════════════════
 * AF_UNIX path table helpers
 * ════════════════════════════════════════════════════════════════ */

static unix_path_entry_t *unix_path_find(const char *path)
{
    for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
        if (g_unix_paths[i].in_use && sock_strcmp(g_unix_paths[i].path, path) == 0)
            return &g_unix_paths[i];
    }
    return NULL;
}

static unix_path_entry_t *unix_path_alloc(void)
{
    for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
        if (!g_unix_paths[i].in_use)
            return &g_unix_paths[i];
    }
    return NULL;
}

int sock_do_socket(int domain, int type, int protocol)
{
    int idx;
    sock_t *s;

    if (domain != AF_INET && domain != AF_UNIX)
        return -EAFNOSUPPORT;
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -EOPNOTSUPP;
    /* AF_UNIX solo SOCK_STREAM per ora */
    if (domain == AF_UNIX && type != SOCK_STREAM)
        return -EOPNOTSUPP;
    (void)protocol;

    idx = sock_alloc();
    if (idx < 0)
        return -ENFILE;

    s = &g_socks[idx];
    s->in_use     = 1U;
    s->domain     = (uint8_t)domain;
    s->type       = (uint8_t)type;
    s->state      = SOCK_STATE_CLOSED;
    s->peer_idx   = SOCK_IDX_NONE;
    return idx;
}

/* ── AF_UNIX bind ─────────────────────────────────────────────── */
int sock_unix_bind(int idx, const char *path)
{
    sock_t *s = sock_get(idx);
    unix_path_entry_t *ent;
    size_t plen;

    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (!path || (plen = sock_strlen(path)) == 0U || plen >= SOCK_UNIX_PATH_MAX)
        return -EINVAL;
    if (s->state != SOCK_STATE_CLOSED)
        return -EINVAL;
    if (unix_path_find(path))
        return -EADDRINUSE;

    ent = unix_path_alloc();
    if (!ent)
        return -EADDRINUSE;

    sock_strncpy(ent->path, path, SOCK_UNIX_PATH_MAX - 1U);
    ent->path[SOCK_UNIX_PATH_MAX - 1U] = '\0';
    ent->sock_idx = (uint16_t)idx;
    ent->in_use   = 1U;

    sock_strncpy(s->unix_path, path, SOCK_UNIX_PATH_MAX - 1U);
    s->unix_path[SOCK_UNIX_PATH_MAX - 1U] = '\0';
    s->state = SOCK_STATE_BOUND;
    return 0;
}

/* ── AF_UNIX connect ──────────────────────────────────────────── */
int sock_unix_connect(int idx, const char *path)
{
    sock_t *s = sock_get(idx);
    unix_path_entry_t *ent;
    sock_t *server;
    sock_t *peer;
    int     peer_idx;
    uint8_t next_tail;

    if (!s || s->domain != AF_UNIX)
        return -EBADF;
    if (s->state == SOCK_STATE_CONNECTED)
        return -EISCONN;
    if (!path || sock_strlen(path) == 0U)
        return -EINVAL;

    ent = unix_path_find(path);
    if (!ent)
        return -ENOENT;

    server = sock_get((int)ent->sock_idx);
    if (!server || server->state != SOCK_STATE_LISTENING)
        return -ECONNREFUSED;

    /* Accept queue piena? */
    next_tail = (server->accept_tail + 1U) & (uint8_t)(SOCK_ACCEPT_MAX - 1U);
    if (next_tail == server->accept_head)
        return -EAGAIN;

    /* Alloca socket lato-server per questa connessione */
    peer_idx = sock_alloc();
    if (peer_idx < 0)
        return -ENFILE;

    peer = &g_socks[peer_idx];
    peer->in_use   = 1U;
    peer->domain   = AF_UNIX;
    peer->type     = SOCK_STREAM;
    peer->state    = SOCK_STATE_CONNECTED;
    peer->peer_idx = (uint16_t)idx;
    sock_strncpy(peer->unix_path, path, SOCK_UNIX_PATH_MAX - 1U);

    s->peer_idx = (uint16_t)peer_idx;
    s->state    = SOCK_STATE_CONNECTED;
    sock_strncpy(s->unix_path, path, SOCK_UNIX_PATH_MAX - 1U);

    /* Inserisci nella accept queue del server */
    server->accept_q[server->accept_tail] = (uint16_t)peer_idx;
    server->accept_tail = next_tail;
    return 0;
}

int sock_do_bind(int idx, uint32_t ip, uint16_t port)
{
    sock_t *s = sock_get(idx);

    if (!s)
        return -EBADF;
    if (s->state != SOCK_STATE_CLOSED)
        return -EINVAL;
    if (ip != SOCK_ANY_IP && ip != SOCK_LOOPBACK_IP)
        return -EADDRNOTAVAIL;
    if (port == 0U)
        return -EINVAL;

    if (!(s->flags & SOCK_FL_REUSEADDR)) {
        if (sock_port_in_use(port, s->type))
            return -EADDRINUSE;
    }

    s->local_ip   = ip;
    s->local_port = port;
    s->state      = SOCK_STATE_BOUND;
    return 0;
}

int sock_do_listen(int idx, int backlog)
{
    sock_t *s = sock_get(idx);
    (void)backlog;

    if (!s)
        return -EBADF;
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->state < SOCK_STATE_BOUND)
        return -EINVAL;

    s->state = SOCK_STATE_LISTENING;
    return 0;
}

int sock_do_accept(int idx, uint32_t *out_ip, uint16_t *out_port)
{
    sock_t  *s = sock_get(idx);
    uint16_t new_idx;
    sock_t  *ns;

    if (!s)
        return -EBADF;
    if (s->type != SOCK_STREAM || s->state != SOCK_STATE_LISTENING)
        return -EINVAL;

    /* Attesa cooperativa (come pipe) */
    while (s->accept_head == s->accept_tail) {
        if (s->flags & SOCK_FL_NONBLOCK)
            return -EAGAIN;
        sched_yield();
    }

    new_idx = s->accept_q[s->accept_head];
    s->accept_head = (s->accept_head + 1U) & (uint8_t)(SOCK_ACCEPT_MAX - 1U);

    ns = sock_get((int)new_idx);
    if (!ns)
        return -EBADF;

    if (out_ip)   *out_ip   = ns->peer_ip;
    if (out_port) *out_port = ns->peer_port;

    return (int)new_idx;
}

int sock_do_connect(int idx, uint32_t ip, uint16_t port)
{
    sock_t  *s = sock_get(idx);
    sock_t  *server;
    sock_t  *peer;
    int      peer_idx;
    uint16_t client_port;
    uint8_t  next_tail;

    if (!s)
        return -EBADF;
    if (s->state == SOCK_STATE_CONNECTED)
        return -EISCONN;

    /* v1: solo loopback */
    if (ip != SOCK_LOOPBACK_IP && ip != SOCK_ANY_IP)
        return -ENETUNREACH;

    /* UDP connect: registra solo il peer */
    if (s->type == SOCK_DGRAM) {
        if (s->local_port == 0U) {
            client_port = sock_alloc_ephemeral();
            if (client_port == 0U)
                return -EADDRNOTAVAIL;
            s->local_ip   = SOCK_LOOPBACK_IP;
            s->local_port = client_port;
        }
        s->peer_ip   = ip;
        s->peer_port = port;
        s->state     = SOCK_STATE_CONNECTED;
        return 0;
    }

    /* TCP connect: trova il server in ascolto */
    server = sock_find_listener(SOCK_STREAM, ip, port);
    if (!server)
        return -ECONNREFUSED;

    /* Accept queue piena? */
    next_tail = (server->accept_tail + 1U) & (uint8_t)(SOCK_ACCEPT_MAX - 1U);
    if (next_tail == server->accept_head)
        return -EAGAIN;

    /* Alloca un socket lato-server per questa connessione */
    peer_idx = sock_alloc();
    if (peer_idx < 0)
        return -ENFILE;

    client_port = s->local_port;
    if (client_port == 0U) {
        client_port = sock_alloc_ephemeral();
        if (client_port == 0U) {
            /* non si può allocare una porta efimera, libera slot appena allocato */
            /* (slot era libero → basta lasciarlo a 0) */
            return -EADDRNOTAVAIL;
        }
        s->local_ip   = SOCK_LOOPBACK_IP;
        s->local_port = client_port;
    }

    /* Inizializza il peer socket (lato server) */
    peer = &g_socks[peer_idx];
    peer->in_use     = 1U;
    peer->domain     = (uint8_t)AF_INET;
    peer->type       = SOCK_STREAM;
    peer->state      = SOCK_STATE_CONNECTED;
    peer->local_ip   = ip;
    peer->local_port = port;
    peer->peer_ip    = SOCK_LOOPBACK_IP;
    peer->peer_port  = client_port;
    peer->peer_idx   = (uint16_t)idx;

    /* Collega il client al peer */
    s->peer_ip   = ip;
    s->peer_port = port;
    s->peer_idx  = (uint16_t)peer_idx;
    s->state     = SOCK_STATE_CONNECTED;

    /* Inserisci nella accept queue del server */
    server->accept_q[server->accept_tail] = (uint16_t)peer_idx;
    server->accept_tail = next_tail;

    return 0;
}

ssize_t sock_do_send(int idx, const void *buf, size_t len, int flags)
{
    sock_t        *s = sock_get(idx);
    sock_t        *peer;
    const uint8_t *src = (const uint8_t *)buf;
    size_t         written = 0U;
    int            nonblock;

    if (!s || !s->in_use)
        return -EBADF;
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->state != SOCK_STATE_CONNECTED)
        return -ENOTCONN;
    if (!buf || len == 0U)
        return 0;

    if (s->peer_idx == SOCK_IDX_NONE)
        return -ENOTCONN;
    peer = sock_get((int)s->peer_idx);
    if (!peer)
        return -EPIPE;
    if (peer->flags & SOCK_FL_PEER_CLOSE)
        return -EPIPE;
    nonblock = ((s->flags & SOCK_FL_NONBLOCK) != 0U) ||
               ((flags & MSG_DONTWAIT) != 0);

    /* Scrivi nel rx ring del peer */
    while (written < len) {
        uint32_t avail = SOCK_RX_BUF_SIZE - (peer->rx_tail - peer->rx_head);
        if (avail == 0U) {
            if (nonblock)
                break;
            sched_yield();
            /* Ricontrolla il peer (potrebbe essere stato chiuso) */
            if (peer->flags & SOCK_FL_PEER_CLOSE)
                return written ? (ssize_t)written : -EPIPE;
            continue;
        }

        size_t chunk = (len - written < avail) ? (len - written) : avail;
        for (size_t i = 0U; i < chunk; i++)
            peer->rx_buf[(peer->rx_tail + (uint32_t)i) & SOCK_RX_MASK] = src[written + i];

        peer->rx_tail += (uint32_t)chunk;
        written       += chunk;
    }

    if (written == 0U && nonblock)
        return -EAGAIN;
    return (ssize_t)written;
}

ssize_t sock_do_recv(int idx, void *buf, size_t len, int flags)
{
    sock_t  *s = sock_get(idx);
    uint8_t *dst = (uint8_t *)buf;
    uint32_t avail;
    size_t   to_read;
    int      nonblock;

    if (!s || !s->in_use)
        return -EBADF;
    if (s->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (s->state != SOCK_STATE_CONNECTED &&
        s->state != SOCK_STATE_CLOSE_WAIT)
        return -ENOTCONN;
    if (!buf || len == 0U)
        return 0;
    nonblock = ((s->flags & SOCK_FL_NONBLOCK) != 0U) ||
               ((flags & MSG_DONTWAIT) != 0);

    /* Attesa cooperativa */
    for (;;) {
        avail = s->rx_tail - s->rx_head;
        if (avail > 0U)
            break;
        if (s->flags & SOCK_FL_PEER_CLOSE)
            return 0;   /* EOF */
        if (nonblock)
            return -EAGAIN;
        sched_yield();
    }

    to_read = (len < avail) ? len : (size_t)avail;
    for (size_t i = 0U; i < to_read; i++)
        dst[i] = s->rx_buf[(s->rx_head + (uint32_t)i) & SOCK_RX_MASK];
    s->rx_head += (uint32_t)to_read;

    return (ssize_t)to_read;
}

ssize_t sock_do_sendto(int idx, const void *buf, size_t len,
                       uint32_t dst_ip, uint16_t dst_port, int flags)
{
    sock_t  *s = sock_get(idx);
    sock_t  *peer = NULL;
    uint16_t cport;
    (void)flags;

    if (!s || !s->in_use)
        return -EBADF;
    if (s->type != SOCK_DGRAM)
        return -EOPNOTSUPP;
    if (!buf || len == 0U)
        return 0;
    if (len > SOCK_UDP_DATA_MAX)
        len = SOCK_UDP_DATA_MAX;

    /* Se il socket è "connected" usa il peer registrato */
    if (s->state == SOCK_STATE_CONNECTED) {
        dst_ip   = s->peer_ip;
        dst_port = s->peer_port;
    }

    if (dst_ip != SOCK_LOOPBACK_IP && dst_ip != SOCK_ANY_IP)
        return -ENETUNREACH;

    /* Assegna porta locale se assente */
    if (s->local_port == 0U) {
        cport = sock_alloc_ephemeral();
        if (cport == 0U)
            return -EADDRNOTAVAIL;
        s->local_ip   = SOCK_LOOPBACK_IP;
        s->local_port = cport;
    }

    /* Trova il destinatario */
    peer = sock_find_listener(SOCK_DGRAM, dst_ip, dst_port);
    if (!peer) {
        /* Cerca un socket DGRAM bound o connected su quella porta */
        for (uint32_t i = 0U; i < SOCK_MAX_GLOBAL; i++) {
            sock_t *c = &g_socks[i];
            if (!c->in_use || c->type != SOCK_DGRAM)
                continue;
            if (c->local_port == dst_port &&
                (c->local_ip == SOCK_ANY_IP || c->local_ip == dst_ip)) {
                peer = c;
                break;
            }
        }
    }
    if (!peer)
        return -ECONNREFUSED;

    /* Aggiungi alla coda UDP del peer */
    uint8_t next = (peer->udp_tail + 1U) & (uint8_t)(SOCK_UDP_QUEUE_MAX - 1U);
    if (next == peer->udp_head)
        return -ENOBUFS;

    sock_udp_dgram_t *dg = &peer->udp_q[peer->udp_tail];
    dg->src_ip   = s->local_ip;
    dg->src_port = s->local_port;
    dg->len      = (uint16_t)len;
    memcpy(dg->data, buf, len);
    peer->udp_tail = next;

    return (ssize_t)len;
}

ssize_t sock_do_recvfrom(int idx, void *buf, size_t len,
                         uint32_t *src_ip, uint16_t *src_port, int flags)
{
    sock_t          *s = sock_get(idx);
    sock_udp_dgram_t *dg;
    size_t            to_copy;
    int               nonblock;

    if (!s || !s->in_use)
        return -EBADF;
    if (s->type != SOCK_DGRAM)
        return -EOPNOTSUPP;
    if (!buf || len == 0U)
        return 0;
    nonblock = ((s->flags & SOCK_FL_NONBLOCK) != 0U) ||
               ((flags & MSG_DONTWAIT) != 0);

    /* Attesa cooperativa */
    while (s->udp_head == s->udp_tail) {
        if (nonblock)
            return -EAGAIN;
        sched_yield();
    }

    dg = &s->udp_q[s->udp_head];
    to_copy = (len < dg->len) ? len : dg->len;
    memcpy(buf, dg->data, to_copy);
    if (src_ip)   *src_ip   = dg->src_ip;
    if (src_port) *src_port = dg->src_port;
    s->udp_head = (s->udp_head + 1U) & (uint8_t)(SOCK_UDP_QUEUE_MAX - 1U);

    return (ssize_t)to_copy;
}

int sock_do_shutdown(int idx, int how)
{
    sock_t *s = sock_get(idx);
    sock_t *peer;

    if (!s)
        return -EBADF;
    if (how < SHUT_RD || how > SHUT_RDWR)
        return -EINVAL;

    if (s->peer_idx != SOCK_IDX_NONE && s->peer_idx < SOCK_MAX_GLOBAL) {
        peer = sock_get((int)s->peer_idx);
        if (peer && (how == SHUT_WR || how == SHUT_RDWR))
            peer->flags |= SOCK_FL_PEER_CLOSE;
    }

    if (how == SHUT_RD || how == SHUT_RDWR)
        s->state = SOCK_STATE_CLOSE_WAIT;

    return 0;
}
