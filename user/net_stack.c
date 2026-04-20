/*
 * EnlilOS – TCP/IP stack v1 (M10-02)
 *
 * Freestanding, single-threaded, runs inside netd (EL0).
 * No libc. All helpers are local.
 *
 * Protocol support:
 *   Ethernet    frame dispatch
 *   ARP         request + reply, 8-entry cache, gratuitous
 *   IPv4        rx/tx, no fragmentation, checksum
 *   ICMP        echo reply (ping)
 *   UDP         4 sockets, bind + send + recv callback
 *   TCP         4 connections, passive open + active connect, PSH+ACK, FIN+ACK
 */

#include "net_stack.h"

/* ── Utility: byte order ────────────────────────────────── */

static ns_u16 ns_htons(ns_u16 h)
{
    return (ns_u16)((h >> 8) | (h << 8));
}

static ns_u16 __attribute__((unused)) ns_ntohs(ns_u16 n) { return ns_htons(n); }

static ns_u32 ns_htonl(ns_u32 h)
{
    return ((h & 0xFF000000U) >> 24) | ((h & 0x00FF0000U) >>  8)
         | ((h & 0x0000FF00U) <<  8) | ((h & 0x000000FFU) << 24);
}

static ns_u32 __attribute__((unused)) ns_ntohl(ns_u32 n) { return ns_htonl(n); }

/* Read/write big-endian fields from raw byte arrays */
static ns_u16 rd16(const ns_u8 *p)
{
    return ((ns_u16)p[0] << 8) | p[1];
}

static ns_u32 rd32(const ns_u8 *p)
{
    return ((ns_u32)p[0] << 24) | ((ns_u32)p[1] << 16)
         | ((ns_u32)p[2] <<  8) |  (ns_u32)p[3];
}

static void wr16(ns_u8 *p, ns_u16 v)
{
    p[0] = (ns_u8)(v >> 8);
    p[1] = (ns_u8)(v & 0xFFU);
}

static void wr32(ns_u8 *p, ns_u32 v)
{
    p[0] = (ns_u8)(v >> 24);
    p[1] = (ns_u8)((v >> 16) & 0xFFU);
    p[2] = (ns_u8)((v >>  8) & 0xFFU);
    p[3] = (ns_u8)(v & 0xFFU);
}

/* ── Utility: memory ────────────────────────────────────── */

static void ns_memcpy(void *dst, const void *src, ns_u32 n)
{
    ns_u8 *d = dst;
    const ns_u8 *s = src;
    while (n--) *d++ = *s++;
}

static void ns_memset(void *dst, ns_u8 val, ns_u32 n)
{
    ns_u8 *d = dst;
    while (n--) *d++ = val;
}

static int __attribute__((unused)) ns_memcmp(const void *a, const void *b, ns_u32 n)
{
    const ns_u8 *p = a, *q = b;
    while (n--) {
        if (*p != *q) return (int)(*p) - (int)(*q);
        p++; q++;
    }
    return 0;
}

/* ── Utility: checksum ──────────────────────────────────── */

/* Accumulate one's-complement sum over bytes, treating pairs as BE u16 */
static ns_u32 cksum_acc(ns_u32 acc, const ns_u8 *p, ns_u16 len)
{
    while (len >= 2U) {
        acc += ((ns_u32)p[0] << 8) | p[1];
        p += 2; len -= 2U;
    }
    if (len) acc += (ns_u32)p[0] << 8;
    return acc;
}

static ns_u16 cksum_fin(ns_u32 acc)
{
    while (acc >> 16) acc = (acc & 0xFFFFU) + (acc >> 16);
    return (ns_u16)(~acc);
}

static ns_u16 ip_cksum(const ns_u8 *hdr, ns_u16 len)
{
    return cksum_fin(cksum_acc(0U, hdr, len));
}

/* TCP/UDP pseudo-header checksum */
static ns_u16 transport_cksum(ns_u32 src_ip, ns_u32 dst_ip, ns_u8 proto,
                                const ns_u8 *seg, ns_u16 seg_len)
{
    ns_u8 ph[12];
    wr32(ph + 0, src_ip);
    wr32(ph + 4, dst_ip);
    ph[8] = 0U;
    ph[9] = proto;
    wr16(ph + 10, seg_len);

    ns_u32 acc = 0U;
    acc = cksum_acc(acc, ph, 12U);
    acc = cksum_acc(acc, seg, seg_len);
    return cksum_fin(acc);
}

/* ── Protocol constants ─────────────────────────────────── */

#define ETH_HLEN        14U
#define ETH_PROTO_ARP   0x0806U
#define ETH_PROTO_IPv4  0x0800U

#define IP_PROTO_ICMP   1U
#define IP_PROTO_TCP    6U
#define IP_PROTO_UDP    17U

#define ICMP_ECHO_REQ   8U
#define ICMP_ECHO_REPLY 0U

#define TCP_FLAG_FIN    0x01U
#define TCP_FLAG_SYN    0x02U
#define TCP_FLAG_RST    0x04U
#define TCP_FLAG_PSH    0x08U
#define TCP_FLAG_ACK    0x10U
#define TCP_FLAG_URG    0x20U

#define TCP_HDR_LEN     20U   /* no options in outgoing segments except SYN */
#define TCP_SYN_HDR_LEN 24U   /* SYN: 4 bytes MSS option */
#define IP_HDR_LEN      20U
#define UDP_HDR_LEN     8U

#define TCP_MSS         1460U
#define NS_ERR_CONNREFUSED 111

/* ── Internal state ─────────────────────────────────────── */

/* Own MAC and IP */
static ns_u8  g_mac[6];
static ns_u32 g_ip   = NET_STACK_IP;
static ns_u32 g_mask = NET_STACK_MASK;
static ns_u32 g_gw   = NET_STACK_GW;

/* Output hook */
static net_output_fn g_output;

/* Scratch TX frame buffer (1536 bytes = max Ethernet frame) */
static ns_u8 g_tx[1536];

/* Stats */
static net_stack_stats_t g_stats;

/* ── ARP cache ──────────────────────────────────────────── */

typedef struct {
    ns_u32 ip;
    ns_u8  mac[6];
    ns_u8  valid;
} arp_entry_t;

static arp_entry_t g_arp_cache[NET_STACK_ARP_ENTRIES];

/* Pending ARP: one slot – if we're waiting for a MAC, we queue one packet */
static struct {
    ns_u32  dst_ip;          /* IP we're waiting for */
    ns_u8   frame[1500];     /* pending frame payload from IP layer down */
    ns_u16  frame_len;
    ns_u8   active;
} g_arp_pending;

static const ns_u8 MAC_BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static const ns_u8 MAC_ZERO[6]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static const ns_u8 *arp_lookup(ns_u32 ip)
{
    for (int i = 0; i < NET_STACK_ARP_ENTRIES; i++)
        if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip)
            return g_arp_cache[i].mac;
    return (const ns_u8 *)0;
}

static void arp_update(ns_u32 ip, const ns_u8 *mac)
{
    int oldest = 0;
    for (int i = 0; i < NET_STACK_ARP_ENTRIES; i++) {
        if (!g_arp_cache[i].valid || g_arp_cache[i].ip == ip) {
            oldest = i;
            break;
        }
        oldest = i;
    }
    g_arp_cache[oldest].ip    = ip;
    g_arp_cache[oldest].valid = 1U;
    ns_memcpy(g_arp_cache[oldest].mac, mac, 6U);
}

/* ── Ethernet TX ────────────────────────────────────────── */

/* Build Ethernet header at buf[0..13] */
static void eth_hdr_fill(ns_u8 *buf, const ns_u8 *dst_mac, ns_u16 proto)
{
    ns_memcpy(buf,     dst_mac, 6U);
    ns_memcpy(buf + 6, g_mac,   6U);
    wr16(buf + 12, proto);
}

/* Send a fully constructed frame from g_tx of given total length */
static void eth_send(ns_u32 total_len)
{
    g_stats.tx_frames++;
    g_output(g_tx, total_len);
}

/* ── ARP layer ──────────────────────────────────────────── */

/* Offsets inside ARP payload (after Ethernet header) */
#define ARP_OFF_HTYPE   0U
#define ARP_OFF_PTYPE   2U
#define ARP_OFF_HLEN    4U
#define ARP_OFF_PLEN    5U
#define ARP_OFF_OPER    6U
#define ARP_OFF_SHA     8U
#define ARP_OFF_SPA    14U
#define ARP_OFF_THA    18U
#define ARP_OFF_TPA    24U
#define ARP_LEN        28U

static void arp_send_reply(ns_u32 tpa, const ns_u8 *tha, ns_u32 spa)
{
    ns_u8 *arp = g_tx + ETH_HLEN;
    eth_hdr_fill(g_tx, tha, ETH_PROTO_ARP);

    wr16(arp + ARP_OFF_HTYPE, 0x0001U);  /* Ethernet */
    wr16(arp + ARP_OFF_PTYPE, 0x0800U);  /* IPv4     */
    arp[ARP_OFF_HLEN] = 6U;
    arp[ARP_OFF_PLEN] = 4U;
    wr16(arp + ARP_OFF_OPER, 2U);        /* Reply    */
    ns_memcpy(arp + ARP_OFF_SHA, g_mac, 6U);
    wr32(arp + ARP_OFF_SPA, spa);
    ns_memcpy(arp + ARP_OFF_THA, tha, 6U);
    wr32(arp + ARP_OFF_TPA, tpa);

    g_stats.tx_arp++;
    eth_send(ETH_HLEN + ARP_LEN);
}

static void arp_send_request(ns_u32 target_ip)
{
    ns_u8 *arp = g_tx + ETH_HLEN;
    eth_hdr_fill(g_tx, MAC_BCAST, ETH_PROTO_ARP);

    wr16(arp + ARP_OFF_HTYPE, 0x0001U);
    wr16(arp + ARP_OFF_PTYPE, 0x0800U);
    arp[ARP_OFF_HLEN] = 6U;
    arp[ARP_OFF_PLEN] = 4U;
    wr16(arp + ARP_OFF_OPER, 1U);  /* Request */
    ns_memcpy(arp + ARP_OFF_SHA, g_mac, 6U);
    wr32(arp + ARP_OFF_SPA, g_ip);
    ns_memcpy(arp + ARP_OFF_THA, MAC_ZERO, 6U);
    wr32(arp + ARP_OFF_TPA, target_ip);

    g_stats.tx_arp++;
    eth_send(ETH_HLEN + ARP_LEN);
}

static void arp_rx(const ns_u8 *frame, ns_u32 len)
{
    if (len < ETH_HLEN + ARP_LEN) return;

    const ns_u8 *arp = frame + ETH_HLEN;
    ns_u16 oper  = rd16(arp + ARP_OFF_OPER);
    ns_u32 spa   = rd32(arp + ARP_OFF_SPA);
    ns_u32 tpa   = rd32(arp + ARP_OFF_TPA);
    const ns_u8 *sha = arp + ARP_OFF_SHA;

    g_stats.rx_arp++;

    /* Always update cache for sender */
    if (spa != 0U) arp_update(spa, sha);

    if (oper == 1U && tpa == g_ip) {
        /* ARP request for our IP: reply */
        arp_send_reply(spa, sha, g_ip);
    } else if (oper == 2U) {
        /* ARP reply: check if we have a pending frame for this IP */
        if (g_arp_pending.active && g_arp_pending.dst_ip == spa) {
            g_arp_pending.active = 0U;
            /* Patch Ethernet dst in the pending frame and send */
            ns_memcpy(g_arp_pending.frame, sha, 6U);
            g_stats.tx_frames++;
            g_output(g_arp_pending.frame, g_arp_pending.frame_len);
        }
    }
}

/* ── IPv4 TX helpers ────────────────────────────────────── */

#define IP_OFF_VER_IHL  0U
#define IP_OFF_DSCP     1U
#define IP_OFF_TOT_LEN  2U
#define IP_OFF_ID       4U
#define IP_OFF_FRAG     6U
#define IP_OFF_TTL      8U
#define IP_OFF_PROTO    9U
#define IP_OFF_CKSUM   10U
#define IP_OFF_SRC     12U
#define IP_OFF_DST     16U

static ns_u16 g_ip_id;

/* Fill IP header starting at buf (20 bytes). len = total IP length. */
static void ip_hdr_fill(ns_u8 *buf, ns_u8 proto, ns_u32 dst, ns_u16 tot_len)
{
    ns_memset(buf, 0U, IP_HDR_LEN);
    buf[IP_OFF_VER_IHL] = 0x45U;           /* v4, IHL=5 */
    wr16(buf + IP_OFF_TOT_LEN, tot_len);
    wr16(buf + IP_OFF_ID, g_ip_id++);
    buf[IP_OFF_TTL]   = 64U;
    buf[IP_OFF_PROTO] = proto;
    wr32(buf + IP_OFF_SRC, g_ip);
    wr32(buf + IP_OFF_DST, dst);
    wr16(buf + IP_OFF_CKSUM, ip_cksum(buf, IP_HDR_LEN));
}

/*
 * Build and transmit an IPv4 frame. The caller provides the transport
 * payload already placed at g_tx[ETH_HLEN + IP_HDR_LEN].
 * transport_len: bytes of transport payload.
 */
static void ipv4_send_assembled(ns_u8 proto, ns_u32 dst_ip,
                                 ns_u16 transport_len)
{
    ns_u16 tot_len = IP_HDR_LEN + transport_len;
    ns_u8 *ip_hdr  = g_tx + ETH_HLEN;

    ip_hdr_fill(ip_hdr, proto, dst_ip, tot_len);

    /* Resolve next-hop MAC */
    ns_u32 next_hop = ((dst_ip & g_mask) == (g_ip & g_mask)) ? dst_ip : g_gw;
    const ns_u8 *dst_mac = arp_lookup(next_hop);

    if (dst_mac) {
        eth_hdr_fill(g_tx, dst_mac, ETH_PROTO_IPv4);
        eth_send(ETH_HLEN + tot_len);
    } else {
        /* Cache miss: queue and send ARP request */
        if (!g_arp_pending.active) {
            g_arp_pending.active    = 1U;
            g_arp_pending.dst_ip    = next_hop;
            g_arp_pending.frame_len = (ns_u16)(ETH_HLEN + tot_len);
            /* Fill broadcast dst for now; will be patched on ARP reply */
            eth_hdr_fill(g_tx, MAC_BCAST, ETH_PROTO_IPv4);
            ns_memcpy(g_arp_pending.frame, g_tx, g_arp_pending.frame_len);
            arp_send_request(next_hop);
        }
        /* else: drop – only one pending slot in v1 */
    }
}

/* ── ICMP ───────────────────────────────────────────────── */

#define ICMP_OFF_TYPE   0U
#define ICMP_OFF_CODE   1U
#define ICMP_OFF_CKSUM  2U
#define ICMP_OFF_ID     4U
#define ICMP_OFF_SEQ    6U
#define ICMP_HDR_LEN    8U

static void icmp_rx(const ns_u8 *ip_hdr, ns_u32 ip_len)
{
    if (ip_len < IP_HDR_LEN + ICMP_HDR_LEN) return;

    const ns_u8 *icmp = ip_hdr + IP_HDR_LEN;
    ns_u16 icmp_len   = (ns_u16)(ip_len - IP_HDR_LEN);
    ns_u32 src_ip     = rd32(ip_hdr + IP_OFF_SRC);

    g_stats.rx_icmp++;

    if (icmp[ICMP_OFF_TYPE] != ICMP_ECHO_REQ) return;

    /* Build echo reply directly into g_tx transport area */
    ns_u8  *out_icmp = g_tx + ETH_HLEN + IP_HDR_LEN;
    ns_memcpy(out_icmp, icmp, icmp_len);
    out_icmp[ICMP_OFF_TYPE] = ICMP_ECHO_REPLY;
    out_icmp[ICMP_OFF_CODE] = 0U;
    wr16(out_icmp + ICMP_OFF_CKSUM, 0U);
    wr16(out_icmp + ICMP_OFF_CKSUM, ip_cksum(out_icmp, icmp_len));

    g_stats.tx_icmp++;
    ipv4_send_assembled(IP_PROTO_ICMP, src_ip, icmp_len);
}

/* ── UDP ────────────────────────────────────────────────── */

#define UDP_OFF_SPORT  0U
#define UDP_OFF_DPORT  2U
#define UDP_OFF_LEN    4U
#define UDP_OFF_CKSUM  6U

typedef struct {
    ns_u16      port;
    udp_recv_cb cb;
    ns_u8       active;
} udp_sock_t;

static udp_sock_t g_udp_socks[NET_STACK_UDP_SOCKETS];

static void udp_rx(const ns_u8 *ip_hdr, ns_u32 ip_len)
{
    if (ip_len < IP_HDR_LEN + UDP_HDR_LEN) return;

    const ns_u8 *udp = ip_hdr + IP_HDR_LEN;
    ns_u16 sport     = rd16(udp + UDP_OFF_SPORT);
    ns_u16 dport     = rd16(udp + UDP_OFF_DPORT);
    ns_u16 udp_len   = rd16(udp + UDP_OFF_LEN);
    ns_u32 src_ip    = rd32(ip_hdr + IP_OFF_SRC);

    g_stats.rx_udp++;

    if (udp_len < UDP_HDR_LEN) return;
    ns_u16 data_len = udp_len - UDP_HDR_LEN;
    const ns_u8 *data = udp + UDP_HDR_LEN;

    for (int i = 0; i < NET_STACK_UDP_SOCKETS; i++) {
        if (g_udp_socks[i].active && g_udp_socks[i].port == dport) {
            g_udp_socks[i].cb(src_ip, sport, dport, data, data_len);
            return;
        }
    }
    /* No socket: silently drop in v1 */
}

int net_stack_udp_bind(ns_u16 port, udp_recv_cb cb)
{
    for (int i = 0; i < NET_STACK_UDP_SOCKETS; i++) {
        if (!g_udp_socks[i].active) {
            g_udp_socks[i].port   = port;
            g_udp_socks[i].cb     = cb;
            g_udp_socks[i].active = 1U;
            return 0;
        }
    }
    return -1;
}

int net_stack_udp_send(ns_u32 dst_ip, ns_u16 dst_port, ns_u16 src_port,
                        const ns_u8 *data, ns_u16 data_len)
{
    ns_u16 udp_len  = UDP_HDR_LEN + data_len;
    ns_u8 *udp_hdr  = g_tx + ETH_HLEN + IP_HDR_LEN;

    if ((ns_u32)(ETH_HLEN + IP_HDR_LEN + udp_len) > sizeof(g_tx)) return -1;

    wr16(udp_hdr + UDP_OFF_SPORT, src_port);
    wr16(udp_hdr + UDP_OFF_DPORT, dst_port);
    wr16(udp_hdr + UDP_OFF_LEN,   udp_len);
    wr16(udp_hdr + UDP_OFF_CKSUM, 0U);   /* checksum optional for IPv4 UDP */

    ns_memcpy(udp_hdr + UDP_HDR_LEN, data, data_len);

    g_stats.tx_udp++;
    ipv4_send_assembled(IP_PROTO_UDP, dst_ip, udp_len);
    return (int)data_len;
}

/* ── TCP ────────────────────────────────────────────────── */

#define TCP_OFF_SPORT    0U
#define TCP_OFF_DPORT    2U
#define TCP_OFF_SEQ      4U
#define TCP_OFF_ACK      8U
#define TCP_OFF_DOFF    12U   /* data offset (high nibble) in 32-bit words */
#define TCP_OFF_FLAGS   13U
#define TCP_OFF_WINDOW  14U
#define TCP_OFF_CKSUM   16U
#define TCP_OFF_URG     18U

typedef struct {
    ns_u8  allocated;
    ns_u8  state;
    ns_u8  preserve_on_close;
    ns_u8  peer_closed;
    ns_u16 local_port;
    ns_u16 remote_port;
    ns_u32 local_ip;    /* always g_ip in v1 */
    ns_u32 remote_ip;
    ns_u32 snd_nxt;     /* next seq to send */
    ns_u32 rcv_nxt;     /* next seq expected from peer */
    ns_u16 snd_win;     /* peer's receive window */
    int    so_error;

    tcp_data_cb   data_cb;
    tcp_closed_cb closed_cb;

    ns_u8  rx_buf[NET_STACK_TCP_RXBUF];
    ns_u16 rx_len;
} tcp_conn_t;

/* Listen slots: one listen entry per port */
typedef struct {
    ns_u16        port;
    tcp_data_cb   data_cb;
    tcp_closed_cb closed_cb;
    ns_u8         active;
} tcp_listen_t;

static tcp_conn_t   g_tcp_conns[NET_STACK_TCP_CONNS];
static tcp_listen_t g_tcp_listen[NET_STACK_TCP_CONNS];

/* Pseudo-random ISN seed, incremented each time we open a connection */
static ns_u32 g_isn_seed = 0xA5A5A500U;
static ns_u16 g_tcp_ephemeral = 49152U;

static int tcp_alloc_conn(void)
{
    for (int i = 0; i < NET_STACK_TCP_CONNS; i++)
        if (!g_tcp_conns[i].allocated) return i;
    return -1;
}

static ns_u16 tcp_alloc_ephemeral_port(void)
{
    ns_u16 start = g_tcp_ephemeral;

    do {
        ns_u16 port = g_tcp_ephemeral;
        int    conflict = 0;

        g_tcp_ephemeral = (g_tcp_ephemeral == 65535U)
                        ? 49152U : (ns_u16)(g_tcp_ephemeral + 1U);

        for (int i = 0; i < NET_STACK_TCP_CONNS; i++) {
            if (g_tcp_conns[i].allocated &&
                g_tcp_conns[i].local_port == port) {
                conflict = 1;
                break;
            }
        }
        if (!conflict)
            return port;
    } while (g_tcp_ephemeral != start);

    return 0U;
}

/* Build and send a TCP segment. payload already in g_tx transport area
   after TCP header. tcp_flags, seq, ack, win: host byte order. */
static void tcp_send_seg(int ci, ns_u8 flags, ns_u32 seq, ns_u32 ack,
                          const ns_u8 *payload, ns_u16 payload_len,
                          int with_mss)
{
    tcp_conn_t *c = &g_tcp_conns[ci];
    ns_u16 hlen   = with_mss ? TCP_SYN_HDR_LEN : TCP_HDR_LEN;
    ns_u16 seg_len = hlen + payload_len;
    ns_u8 *tcp_hdr = g_tx + ETH_HLEN + IP_HDR_LEN;

    ns_memset(tcp_hdr, 0U, hlen);
    wr16(tcp_hdr + TCP_OFF_SPORT, c->local_port);
    wr16(tcp_hdr + TCP_OFF_DPORT, c->remote_port);
    wr32(tcp_hdr + TCP_OFF_SEQ,   seq);
    wr32(tcp_hdr + TCP_OFF_ACK,   ack);
    tcp_hdr[TCP_OFF_DOFF]  = (ns_u8)((hlen / 4U) << 4);
    tcp_hdr[TCP_OFF_FLAGS] = flags;
    wr16(tcp_hdr + TCP_OFF_WINDOW, (ns_u16)NET_STACK_TCP_RXBUF);

    if (with_mss) {
        /* MSS option: kind=2, len=4, mss=1460 */
        tcp_hdr[TCP_HDR_LEN + 0] = 2U;
        tcp_hdr[TCP_HDR_LEN + 1] = 4U;
        wr16(tcp_hdr + TCP_HDR_LEN + 2, (ns_u16)TCP_MSS);
    }

    if (payload && payload_len) {
        ns_memcpy(tcp_hdr + hlen, payload, payload_len);
    }

    /* Checksum */
    wr16(tcp_hdr + TCP_OFF_CKSUM, 0U);
    ns_u16 ck = transport_cksum(g_ip, c->remote_ip, IP_PROTO_TCP,
                                  tcp_hdr, seg_len);
    wr16(tcp_hdr + TCP_OFF_CKSUM, ck);

    g_stats.tx_tcp++;
    ipv4_send_assembled(IP_PROTO_TCP, c->remote_ip, seg_len);
}

/* Send RST for an unexpected incoming segment */
static void tcp_send_rst(ns_u32 dst_ip, ns_u16 dst_port, ns_u16 src_port,
                          ns_u32 seq, ns_u32 ack, ns_u8 flags)
{
    /* We need a temporary "connection" context for tcp_send_seg */
    /* Simpler: build RST directly */
    ns_u16 hlen    = TCP_HDR_LEN;
    ns_u8 *tcp_hdr = g_tx + ETH_HLEN + IP_HDR_LEN;

    ns_memset(tcp_hdr, 0U, hlen);
    wr16(tcp_hdr + TCP_OFF_SPORT, src_port);
    wr16(tcp_hdr + TCP_OFF_DPORT, dst_port);

    if (flags & TCP_FLAG_ACK) {
        wr32(tcp_hdr + TCP_OFF_SEQ, ack);
        tcp_hdr[TCP_OFF_FLAGS] = TCP_FLAG_RST;
    } else {
        wr32(tcp_hdr + TCP_OFF_SEQ, 0U);
        wr32(tcp_hdr + TCP_OFF_ACK, seq + 1U);
        tcp_hdr[TCP_OFF_FLAGS] = TCP_FLAG_RST | TCP_FLAG_ACK;
    }
    tcp_hdr[TCP_OFF_DOFF] = (ns_u8)((hlen / 4U) << 4);

    wr16(tcp_hdr + TCP_OFF_CKSUM, 0U);

    /* Temporarily place dst_ip to compute checksum via a scratch conn */
    {
        /* build a fake conn just for checksum */
        tcp_conn_t fake;
        fake.local_port  = src_port;
        fake.remote_port = dst_port;
        fake.remote_ip   = dst_ip;
        (void)fake;
        ns_u16 ck = transport_cksum(g_ip, dst_ip, IP_PROTO_TCP,
                                     tcp_hdr, hlen);
        wr16(tcp_hdr + TCP_OFF_CKSUM, ck);
    }

    /* Must build IP + Ethernet without going through a conn */
    ns_u16 tot_len = IP_HDR_LEN + hlen;
    ip_hdr_fill(g_tx + ETH_HLEN, IP_PROTO_TCP, dst_ip, tot_len);
    ns_u32 next_hop = ((dst_ip & g_mask) == (g_ip & g_mask)) ? dst_ip : g_gw;
    const ns_u8 *dmac = arp_lookup(next_hop);
    if (dmac) {
        eth_hdr_fill(g_tx, dmac, ETH_PROTO_IPv4);
        g_stats.tx_frames++;
        g_output(g_tx, ETH_HLEN + tot_len);
    }
}

static void tcp_close_conn(int ci)
{
    tcp_conn_t *c = &g_tcp_conns[ci];
    c->peer_closed = 1U;
    if (c->closed_cb) c->closed_cb(ci);
    if (c->preserve_on_close) {
        c->state = NS_TCP_CLOSED;
        return;
    }
    ns_memset(c, 0U, sizeof(*c));
    c->state = NS_TCP_CLOSED;
}

static void tcp_rx(const ns_u8 *ip_hdr, ns_u32 ip_len)
{
    if (ip_len < IP_HDR_LEN + TCP_HDR_LEN) return;

    const ns_u8 *tcp = ip_hdr + IP_HDR_LEN;
    ns_u16  tcp_hdr_len = (ns_u16)(((tcp[TCP_OFF_DOFF] >> 4) & 0xFU) * 4U);
    if (tcp_hdr_len < TCP_HDR_LEN || ip_len < IP_HDR_LEN + tcp_hdr_len) return;

    ns_u16 sport     = rd16(tcp + TCP_OFF_SPORT);
    ns_u16 dport     = rd16(tcp + TCP_OFF_DPORT);
    ns_u32 seq       = rd32(tcp + TCP_OFF_SEQ);
    ns_u32 ack_val   = rd32(tcp + TCP_OFF_ACK);
    ns_u8  flags     = tcp[TCP_OFF_FLAGS];
    ns_u32 src_ip    = rd32(ip_hdr + IP_OFF_SRC);
    ns_u16 data_len  = (ns_u16)(ip_len - IP_HDR_LEN - tcp_hdr_len);
    const ns_u8 *data = tcp + tcp_hdr_len;

    g_stats.rx_tcp++;

    /* Look for an existing connection */
    int ci = -1;
    for (int i = 0; i < NET_STACK_TCP_CONNS; i++) {
        tcp_conn_t *c = &g_tcp_conns[i];
        if (c->state != NS_TCP_CLOSED
            && c->local_port  == dport
            && c->remote_port == sport
            && c->remote_ip   == src_ip) {
            ci = i;
            break;
        }
    }

    if (ci < 0) {
        /* No existing connection: look for a listener */
        if (!(flags & TCP_FLAG_SYN) || (flags & TCP_FLAG_ACK)) {
            /* Not a clean SYN: send RST */
            if (!(flags & TCP_FLAG_RST))
                tcp_send_rst(src_ip, sport, dport, seq, ack_val, flags);
            return;
        }

        /* Find listener for this port */
        tcp_listen_t *lst = (tcp_listen_t *)0;
        for (int i = 0; i < NET_STACK_TCP_CONNS; i++) {
            if (g_tcp_listen[i].active && g_tcp_listen[i].port == dport) {
                lst = &g_tcp_listen[i];
                break;
            }
        }
        if (!lst) {
            tcp_send_rst(src_ip, sport, dport, seq, ack_val, flags);
            return;
        }

        ci = tcp_alloc_conn();
        if (ci < 0) {
            tcp_send_rst(src_ip, sport, dport, seq, ack_val, flags);
            return;
        }

        tcp_conn_t *c   = &g_tcp_conns[ci];
        ns_memset(c, 0U, sizeof(*c));
        c->allocated    = 1U;
        c->state        = NS_TCP_SYN_RCVD;
        c->local_port   = dport;
        c->remote_port  = sport;
        c->remote_ip    = src_ip;
        c->local_ip     = g_ip;
        c->rcv_nxt      = seq + 1U;
        c->snd_nxt      = g_isn_seed;
        g_isn_seed     += 0x10000U;
        c->snd_win      = rd16(tcp + TCP_OFF_WINDOW);
        c->data_cb      = lst->data_cb;
        c->closed_cb    = lst->closed_cb;
        c->rx_len       = 0U;

        /* Send SYN+ACK with MSS option */
        tcp_send_seg(ci, TCP_FLAG_SYN | TCP_FLAG_ACK,
                     c->snd_nxt, c->rcv_nxt, (const ns_u8 *)0, 0U, 1);
        c->snd_nxt++;
        return;
    }

    tcp_conn_t *c = &g_tcp_conns[ci];

    /* RST: close immediately */
    if (flags & TCP_FLAG_RST) {
        if (c->state == NS_TCP_SYN_SENT)
            c->so_error = NS_ERR_CONNREFUSED;
        tcp_close_conn(ci);
        return;
    }

    switch (c->state) {
    case NS_TCP_SYN_SENT:
        if ((flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) &&
            ack_val == c->snd_nxt) {
            c->snd_win = rd16(tcp + TCP_OFF_WINDOW);
            c->rcv_nxt = seq + 1U;
            c->state   = NS_TCP_ESTABLISHED;
            tcp_send_seg(ci, TCP_FLAG_ACK,
                         c->snd_nxt, c->rcv_nxt,
                         (const ns_u8 *)0, 0U, 0);
        }
        break;

    case NS_TCP_SYN_RCVD:
        /* Expecting ACK to our SYN+ACK */
        if ((flags & TCP_FLAG_ACK) && ack_val == c->snd_nxt) {
            c->state   = NS_TCP_ESTABLISHED;
            c->rcv_nxt = seq;  /* no data in pure ACK */
        }
        break;

    case NS_TCP_ESTABLISHED:
        if (flags & TCP_FLAG_ACK)
            c->snd_win = rd16(tcp + TCP_OFF_WINDOW);

        if (data_len > 0U) {
            ns_u16 room = (ns_u16)(NET_STACK_TCP_RXBUF - c->rx_len);
            ns_u16 copy_len = (data_len < room) ? data_len : room;

            c->rcv_nxt += data_len;

            if (copy_len > 0U) {
                ns_memcpy(c->rx_buf + c->rx_len, data, copy_len);
                c->rx_len = (ns_u16)(c->rx_len + copy_len);
            }

            /* Deliver to application */
            if (c->data_cb) {
                c->data_cb(ci, data, copy_len);
            }

            /* ACK the data */
            tcp_send_seg(ci, TCP_FLAG_ACK,
                         c->snd_nxt, c->rcv_nxt,
                         (const ns_u8 *)0, 0U, 0);
        }

        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            c->state = NS_TCP_CLOSE_WAIT;
            /* ACK the FIN */
            tcp_send_seg(ci, TCP_FLAG_ACK,
                         c->snd_nxt, c->rcv_nxt,
                         (const ns_u8 *)0, 0U, 0);
            /* In v1: immediately send our FIN too (no half-close) */
            tcp_send_seg(ci, TCP_FLAG_FIN | TCP_FLAG_ACK,
                         c->snd_nxt, c->rcv_nxt,
                         (const ns_u8 *)0, 0U, 0);
            c->snd_nxt++;
            c->state = NS_TCP_LAST_ACK;
        }
        break;

    case NS_TCP_LAST_ACK:
        if ((flags & TCP_FLAG_ACK) && ack_val == c->snd_nxt) {
            tcp_close_conn(ci);
        }
        break;

    case NS_TCP_FIN_WAIT_1:
        if (flags & TCP_FLAG_ACK) c->state = NS_TCP_FIN_WAIT_2;
        /* fall through to FIN_WAIT_2 handling if FIN also set */
        if (!(flags & TCP_FLAG_FIN)) break;
        /* FALLTHROUGH */
    case NS_TCP_FIN_WAIT_2:
        if (flags & TCP_FLAG_FIN) {
            c->rcv_nxt++;
            tcp_send_seg(ci, TCP_FLAG_ACK,
                         c->snd_nxt, c->rcv_nxt,
                         (const ns_u8 *)0, 0U, 0);
            tcp_close_conn(ci);
        }
        break;

    default:
        break;
    }
}

int net_stack_tcp_listen(ns_u16 port, tcp_data_cb data_cb,
                          tcp_closed_cb closed_cb)
{
    for (int i = 0; i < NET_STACK_TCP_CONNS; i++) {
        if (!g_tcp_listen[i].active) {
            g_tcp_listen[i].port      = port;
            g_tcp_listen[i].data_cb   = data_cb;
            g_tcp_listen[i].closed_cb = closed_cb;
            g_tcp_listen[i].active    = 1U;
            return i;
        }
    }
    return -1;
}

int net_stack_tcp_connect(ns_u32 dst_ip, ns_u16 dst_port, ns_u16 local_port,
                           tcp_data_cb data_cb, tcp_closed_cb closed_cb)
{
    int        ci;
    tcp_conn_t *c;

    ci = tcp_alloc_conn();
    if (ci < 0)
        return -1;

    if (local_port == 0U) {
        local_port = tcp_alloc_ephemeral_port();
        if (local_port == 0U)
            return -1;
    }

    c = &g_tcp_conns[ci];
    ns_memset(c, 0U, sizeof(*c));
    c->allocated         = 1U;
    c->preserve_on_close = 1U;
    c->state             = NS_TCP_SYN_SENT;
    c->local_port        = local_port;
    c->remote_port       = dst_port;
    c->local_ip          = g_ip;
    c->remote_ip         = dst_ip;
    c->snd_nxt           = g_isn_seed;
    c->rcv_nxt           = 0U;
    c->snd_win           = TCP_MSS;
    c->data_cb           = data_cb;
    c->closed_cb         = closed_cb;
    g_isn_seed          += 0x10000U;

    tcp_send_seg(ci, TCP_FLAG_SYN, c->snd_nxt, 0U,
                 (const ns_u8 *)0, 0U, 1);
    c->snd_nxt++;
    return ci;
}

int net_stack_tcp_send(int conn, const ns_u8 *data, ns_u16 len)
{
    if (conn < 0 || conn >= NET_STACK_TCP_CONNS) return -1;
    tcp_conn_t *c = &g_tcp_conns[conn];
    if (c->state != NS_TCP_ESTABLISHED) return -1;
    if (len == 0U) return 0;

    /* Clamp to MSS and peer window */
    ns_u16 max_send = (c->snd_win < TCP_MSS) ? c->snd_win : TCP_MSS;
    if (max_send == 0U)
        return -1;
    if (len > max_send) len = max_send;

    tcp_send_seg(conn, TCP_FLAG_PSH | TCP_FLAG_ACK,
                 c->snd_nxt, c->rcv_nxt, data, len, 0);
    c->snd_nxt += len;
    return (int)len;
}

int net_stack_tcp_recv(int conn, ns_u8 *buf, ns_u16 maxlen)
{
    tcp_conn_t *c;
    ns_u16      copy_len;
    ns_u16      i;

    if (conn < 0 || conn >= NET_STACK_TCP_CONNS || !buf || maxlen == 0U)
        return -1;

    c = &g_tcp_conns[conn];
    if (!c->allocated)
        return -1;

    if (c->rx_len == 0U) {
        if (c->state == NS_TCP_CLOSED || c->peer_closed)
            return 0;
        return -1;
    }

    copy_len = (maxlen < c->rx_len) ? maxlen : c->rx_len;
    ns_memcpy(buf, c->rx_buf, copy_len);
    for (i = copy_len; i < c->rx_len; i++)
        c->rx_buf[i - copy_len] = c->rx_buf[i];
    c->rx_len = (ns_u16)(c->rx_len - copy_len);
    return (int)copy_len;
}

int net_stack_tcp_info(int conn, net_stack_tcp_info_t *out)
{
    tcp_conn_t *c;

    if (conn < 0 || conn >= NET_STACK_TCP_CONNS || !out)
        return -1;
    c = &g_tcp_conns[conn];
    if (!c->allocated)
        return -1;

    out->state       = c->state;
    out->peer_closed = c->peer_closed;
    out->local_port  = c->local_port;
    out->remote_port = c->remote_port;
    out->rx_len      = c->rx_len;
    out->local_ip    = c->local_ip;
    out->remote_ip   = c->remote_ip;
    out->so_error    = c->so_error;
    return 0;
}

void net_stack_tcp_close(int conn)
{
    if (conn < 0 || conn >= NET_STACK_TCP_CONNS) return;
    tcp_conn_t *c = &g_tcp_conns[conn];
    if (!c->allocated)
        return;
    if (c->state == NS_TCP_SYN_SENT) {
        c->state = NS_TCP_CLOSED;
        c->peer_closed = 1U;
        return;
    }
    if (c->state != NS_TCP_ESTABLISHED
        && c->state != NS_TCP_CLOSE_WAIT) return;

    tcp_send_seg(conn, TCP_FLAG_FIN | TCP_FLAG_ACK,
                 c->snd_nxt, c->rcv_nxt, (const ns_u8 *)0, 0U, 0);
    c->snd_nxt++;
    c->state = NS_TCP_FIN_WAIT_1;
}

void net_stack_tcp_release(int conn)
{
    if (conn < 0 || conn >= NET_STACK_TCP_CONNS)
        return;
    ns_memset(&g_tcp_conns[conn], 0U, sizeof(g_tcp_conns[conn]));
    g_tcp_conns[conn].state = NS_TCP_CLOSED;
}

/* ── IPv4 RX ────────────────────────────────────────────── */

static void ipv4_rx(const ns_u8 *frame, ns_u32 frame_len)
{
    if (frame_len < ETH_HLEN + IP_HDR_LEN) return;

    const ns_u8 *ip = frame + ETH_HLEN;
    ns_u16 ihl      = (ns_u16)((ip[0] & 0x0FU) * 4U);
    if (ihl < IP_HDR_LEN) return;

    ns_u16 tot_len  = rd16(ip + IP_OFF_TOT_LEN);
    if (frame_len < ETH_HLEN + tot_len) return;

    ns_u32 dst_ip   = rd32(ip + IP_OFF_DST);
    ns_u32 bcast    = (g_ip & g_mask) | (~g_mask);

    if (dst_ip != g_ip && dst_ip != 0xFFFFFFFFU && dst_ip != bcast) return;

    g_stats.rx_ipv4++;

    ns_u8 proto = ip[IP_OFF_PROTO];
    switch (proto) {
    case IP_PROTO_ICMP: icmp_rx(ip, tot_len); break;
    case IP_PROTO_UDP:  udp_rx(ip, tot_len);  break;
    case IP_PROTO_TCP:  tcp_rx(ip, tot_len);  break;
    default: g_stats.rx_drops++; break;
    }
}

/* ── Public API ─────────────────────────────────────────── */

void net_stack_init(const ns_u8 *mac, net_output_fn out_fn)
{
    ns_memset(&g_stats,       0U, sizeof(g_stats));
    ns_memset(g_arp_cache,    0U, sizeof(g_arp_cache));
    ns_memset(g_udp_socks,    0U, sizeof(g_udp_socks));
    ns_memset(g_tcp_conns,    0U, sizeof(g_tcp_conns));
    ns_memset(g_tcp_listen,   0U, sizeof(g_tcp_listen));
    ns_memset(&g_arp_pending, 0U, sizeof(g_arp_pending));
    ns_memset(g_tx,           0U, sizeof(g_tx));

    ns_memcpy(g_mac, mac, 6U);
    g_output   = out_fn;
    g_ip_id    = 1U;
    g_isn_seed = 0xA5A5A500U;
    g_tcp_ephemeral = 49152U;
}

void net_stack_input(const ns_u8 *frame, ns_u32 len)
{
    if (len < ETH_HLEN) return;
    g_stats.rx_frames++;

    ns_u16 proto = rd16(frame + 12);
    switch (proto) {
    case ETH_PROTO_ARP:  arp_rx(frame, len);  break;
    case ETH_PROTO_IPv4: ipv4_rx(frame, len); break;
    default: g_stats.rx_drops++; break;
    }
}

void net_stack_send_garp(void)
{
    /*
     * Gratuitous ARP Request: SHA=SPA=our IP, THA=00:00:00:00:00:00,
     * TPA=our IP. Broadcast. Announces our MAC to the local segment.
     */
    ns_u8 *arp = g_tx + ETH_HLEN;
    eth_hdr_fill(g_tx, MAC_BCAST, ETH_PROTO_ARP);

    wr16(arp + ARP_OFF_HTYPE, 0x0001U);
    wr16(arp + ARP_OFF_PTYPE, 0x0800U);
    arp[ARP_OFF_HLEN] = 6U;
    arp[ARP_OFF_PLEN] = 4U;
    wr16(arp + ARP_OFF_OPER, 1U);       /* Request */
    ns_memcpy(arp + ARP_OFF_SHA, g_mac, 6U);
    wr32(arp + ARP_OFF_SPA, g_ip);
    ns_memcpy(arp + ARP_OFF_THA, MAC_ZERO, 6U);
    wr32(arp + ARP_OFF_TPA, g_ip);

    g_stats.tx_arp++;
    eth_send(ETH_HLEN + ARP_LEN);
}

void net_stack_get_stats(net_stack_stats_t *out)
{
    ns_memcpy(out, &g_stats, sizeof(*out));
}
