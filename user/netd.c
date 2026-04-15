/*
 * EnlilOS netd – bootstrap network server (M10-01 / M10-02)
 *
 * M10-01: raw Ethernet drain via SYS_NET_BOOT_RECV
 * M10-02: full TCP/IP stack (ARP/IPv4/ICMP/UDP/TCP) via net_stack.c
 *
 * Architecture:
 *   - netd owns the "net" IPC port (enforced kernel-side)
 *   - Receives raw Ethernet frames via SYS_NET_BOOT_RECV
 *   - Feeds frames into net_stack_input()
 *   - net_stack calls netd_output() for TX frames → SYS_NET_BOOT_SEND
 *   - On startup sends a Gratuitous ARP (proves stack is live to selftest)
 */

#include "net_stack.h"

/* ── Syscall numbers (copied from syscall.h, no kernel include) ──── */
#define SYS_WRITE          1
#define SYS_EXIT           3
#define SYS_YIELD         20
#define SYS_PORT_LOOKUP  140
#define SYS_NET_BOOT_SEND 162
#define SYS_NET_BOOT_RECV 163
#define SYS_NET_BOOT_INFO 164

/* ── SVC wrappers ──────────────────────────────────────────────────── */

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

/* net_info_t layout (must match include/net.h) */
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

#define NET_FRAME_MAX 1536U

static void netd_puts(const char *s)
{
    ns_u32 n = 0;
    while (s[n]) n++;
    (void)svc3(SYS_WRITE, 1, (long)s, (long)n);
}

/* Output hook called by net_stack when a frame is ready to TX */
static void netd_output(const ns_u8 *frame, ns_u32 len)
{
    (void)svc2(SYS_NET_BOOT_SEND, (long)frame, (long)len);
}

void _start(void)
{
    static ns_u8     rx_buf[NET_FRAME_MAX];
    static netd_info_t info;
    long             rc;
    ns_u32           idle = 0U;

    /* Verify we own the "net" port */
    rc = svc1(SYS_PORT_LOOKUP, (long)"net");
    if (rc < 0) {
        netd_puts("[NETD] porta net non trovata\n");
        (void)svc1(SYS_EXIT, 1);
        __builtin_unreachable();
    }

    /* Query driver info (includes our MAC) */
    rc = svc1(SYS_NET_BOOT_INFO, (long)&info);
    if (rc < 0) {
        netd_puts("[NETD] driver bootstrap non disponibile\n");
        (void)svc1(SYS_EXIT, 1);
        __builtin_unreachable();
    }

    /* Initialise TCP/IP stack */
    net_stack_init(info.mac, netd_output);

    /* Announce our presence on the local segment */
    net_stack_send_garp();

    netd_puts("[NETD] stack TCP/IP v1 online (10.0.2.15/24)\n");

    /* Main poll loop */
    for (;;) {
        rc = svc2(SYS_NET_BOOT_RECV, (long)rx_buf, (long)NET_FRAME_MAX);
        if (rc > 0) {
            idle = 0U;
            net_stack_input(rx_buf, (ns_u32)rc);
            continue;
        }

        idle++;
        if ((idle & 0x1FU) == 0U)
            (void)svc1(SYS_YIELD, 0);
    }
}
