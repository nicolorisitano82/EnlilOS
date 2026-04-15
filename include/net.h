/*
 * EnlilOS Microkernel - VirtIO Network API (M10-01)
 *
 * Driver kernel per virtio-net su virtio-mmio (QEMU virt machine).
 *
 * Scope v1:
 *   - Ethernet raw send/recv
 *   - MAC address bootstrap
 *   - RX ring buffer statico + TX sincrono bounded
 *   - Server user-space `netd` sopra syscall bootstrap dedicate
 *
 * Il vero stack IPv4/TCP/UDP arriva in M10-02.
 */

#ifndef ENLILOS_NET_H
#define ENLILOS_NET_H

#include "types.h"

#define NET_FRAME_MAX      1536U
#define NET_RX_RING_SIZE   32U
#define NET_TX_QUEUE_DEPTH 16U
#define NET_RX_QUEUE_DEPTH 16U
#define NET_TX_TIMEOUT     5000000U

#define NET_OK              0
#define NET_ERR_NOT_READY  -1
#define NET_ERR_IO         -2
#define NET_ERR_TIMEOUT    -3
#define NET_ERR_RANGE      -4
#define NET_ERR_BUSY       -5

typedef struct {
    uint8_t  mac[6];
    uint8_t  has_mac;
    uint8_t  has_status;
    uint16_t status;
    uint16_t mtu;
    uint8_t  link_up;
    uint8_t  reserved0;
    uint16_t reserved1;
    uint32_t rx_packets;
    uint32_t tx_packets;
    uint32_t rx_drops;
    uint32_t tx_errors;
} net_info_t;

int  net_init(void);
int  net_is_ready(void);
int  net_get_info(net_info_t *out);
int  net_send(const void *buf, uint32_t len);
int  net_recv(void *buf, uint32_t maxlen);
int  net_selftest_run(void);

#endif /* ENLILOS_NET_H */
