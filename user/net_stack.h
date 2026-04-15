/*
 * EnlilOS – TCP/IP stack v1 API (M10-02)
 *
 * Freestanding, single-threaded, runs inside netd (EL0).
 *
 * Supported in v1:
 *   Ethernet / ARP (8-entry cache, gratuitous ARP at init)
 *   IPv4  (static IP 10.0.2.15/24, no fragmentation)
 *   ICMP  Echo Reply
 *   UDP   (4 sockets, bind + send + recv callback)
 *   TCP   (4 connections, passive open, PSH+ACK, FIN+ACK)
 *
 * Static config matches QEMU SLIRP defaults.
 */

#ifndef ENLILOS_NET_STACK_H
#define ENLILOS_NET_STACK_H

typedef unsigned char      ns_u8;
typedef unsigned short     ns_u16;
typedef unsigned int       ns_u32;

/* ── Static IP config (QEMU SLIRP) ──────────────────────── */
#define NET_STACK_IP    0x0A00020FU   /* 10.0.2.15  */
#define NET_STACK_MASK  0xFFFFFF00U   /* /24         */
#define NET_STACK_GW    0x0A000202U   /* 10.0.2.2   */

/* ── Pool sizes ──────────────────────────────────────────── */
#define NET_STACK_ARP_ENTRIES 8
#define NET_STACK_UDP_SOCKETS 4
#define NET_STACK_TCP_CONNS   4
#define NET_STACK_TCP_RXBUF   2048
#define NET_STACK_TCP_TXBUF   1024

/* ── TCP connection states ───────────────────────────────── */
#define NS_TCP_CLOSED       0
#define NS_TCP_LISTEN       1
#define NS_TCP_SYN_RCVD     2
#define NS_TCP_ESTABLISHED  3
#define NS_TCP_FIN_WAIT_1   4
#define NS_TCP_FIN_WAIT_2   5
#define NS_TCP_CLOSE_WAIT   6
#define NS_TCP_LAST_ACK     7

/* ── Stats ───────────────────────────────────────────────── */
typedef struct {
    ns_u32 rx_frames;
    ns_u32 tx_frames;
    ns_u32 rx_arp;
    ns_u32 rx_ipv4;
    ns_u32 rx_icmp;
    ns_u32 rx_udp;
    ns_u32 rx_tcp;
    ns_u32 tx_arp;
    ns_u32 tx_icmp;
    ns_u32 tx_udp;
    ns_u32 tx_tcp;
    ns_u32 rx_drops;
} net_stack_stats_t;

/* ── Callbacks ───────────────────────────────────────────── */

/* UDP: called on every received datagram for a bound port */
typedef void (*udp_recv_cb)(ns_u32 src_ip, ns_u16 src_port, ns_u16 dst_port,
                             const ns_u8 *data, ns_u16 len);

/* TCP: called when data arrives on an established connection */
typedef void (*tcp_data_cb)(int conn, const ns_u8 *data, ns_u16 len);

/* TCP: called when a connection is fully closed */
typedef void (*tcp_closed_cb)(int conn);

/* Raw Ethernet output function provided by netd */
typedef void (*net_output_fn)(const ns_u8 *frame, ns_u32 len);

/* ── Public API ──────────────────────────────────────────── */

/* Initialise stack. Must be called once with our MAC and output hook. */
void net_stack_init(const ns_u8 *mac, net_output_fn out_fn);

/* Feed one raw Ethernet frame into the stack (from netd recv loop). */
void net_stack_input(const ns_u8 *frame, ns_u32 len);

/* Send a gratuitous ARP announcing our IP (call once after init). */
void net_stack_send_garp(void);

/* Copy current stats into *out. */
void net_stack_get_stats(net_stack_stats_t *out);

/* UDP -------------------------------------------------------------- */

/* Bind port to a receive callback; returns 0 on success, -1 if no slot. */
int  net_stack_udp_bind(ns_u16 port, udp_recv_cb cb);

/* Send a UDP datagram. ARP cache is consulted; if MAC unknown, packet
   is queued behind an ARP request (one pending per destination in v1). */
int  net_stack_udp_send(ns_u32 dst_ip, ns_u16 dst_port, ns_u16 src_port,
                         const ns_u8 *data, ns_u16 len);

/* TCP -------------------------------------------------------------- */

/* Register a passive-open listener on port; returns listen slot or -1. */
int  net_stack_tcp_listen(ns_u16 port, tcp_data_cb data_cb,
                           tcp_closed_cb closed_cb);

/* Send data on an ESTABLISHED connection; returns bytes sent or -1. */
int  net_stack_tcp_send(int conn, const ns_u8 *data, ns_u16 len);

/* Initiate graceful close (send FIN). */
void net_stack_tcp_close(int conn);

#endif /* ENLILOS_NET_STACK_H */
