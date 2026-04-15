/*
 * EnlilOS Microkernel - VirtIO Network Driver (M10-01)
 *
 * Backend supportato:
 *   - virtio-net su virtio-mmio (QEMU virt)
 *
 * Scope v1:
 *   - due queue split-ring: RX (0) + TX (1)
 *   - MAC address bootstrap
 *   - RX IRQ -> copia frame in ring buffer statico
 *   - TX sincrono bounded con polling del completamento
 *
 * Non ancora in scope:
 *   - control queue
 *   - offload checksum/GSO
 *   - multiqueue
 *   - stack IP/TCP/UDP (M10-02)
 */

#include "net.h"
#include "virtio_mmio.h"
#include "gic.h"
#include "mmu.h"
#include "uart.h"

#define VNET_F_MAC_NUM       5U
#define VNET_F_STATUS_NUM    16U

#define VNET_S_LINK_UP       1U

#define VNET_RX_QUEUE_ID     0U
#define VNET_TX_QUEUE_ID     1U

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[NET_RX_QUEUE_DEPTH];
    uint16_t used_event;
} __attribute__((packed)) vnet_avail_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[NET_RX_QUEUE_DEPTH];
    uint16_t avail_event;
} __attribute__((packed)) vnet_used_t;

typedef struct __attribute__((packed)) {
    uint8_t  flags;
    uint8_t  gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} vnet_hdr_t;

typedef struct {
    uint16_t len;
    uint8_t  data[NET_FRAME_MAX];
} net_frame_t;

static uint8_t rx_vq_mem[4096] __attribute__((aligned(4096)));
static uint8_t tx_vq_mem[4096] __attribute__((aligned(4096)));

static uint8_t rx_dma[NET_RX_QUEUE_DEPTH][sizeof(vnet_hdr_t) + NET_FRAME_MAX]
    __attribute__((aligned(64)));
static uint8_t tx_dma[NET_TX_QUEUE_DEPTH][sizeof(vnet_hdr_t) + NET_FRAME_MAX]
    __attribute__((aligned(64)));

static net_frame_t net_rx_ring[NET_RX_RING_SIZE];
static uint8_t     net_rx_head;
static uint8_t     net_rx_tail;

#define RX_DESC  ((vring_desc_t *)(rx_vq_mem + 0))
#define RX_AVAIL ((vnet_avail_t *)(rx_vq_mem + 256))
#define RX_USED  ((vnet_used_t *)(rx_vq_mem + 512))

#define TX_DESC  ((vring_desc_t *)(tx_vq_mem + 0))
#define TX_AVAIL ((vnet_avail_t *)(tx_vq_mem + 256))
#define TX_USED  ((vnet_used_t *)(tx_vq_mem + 512))

static uintptr_t net_base;
static uint32_t  net_irq;
static uint16_t  rx_queue_size;
static uint16_t  tx_queue_size;
static uint16_t  rx_next_avail;
static uint16_t  rx_last_used;
static uint16_t  tx_next_avail;
static uint16_t  tx_last_used;
static uint8_t   tx_inflight[NET_TX_QUEUE_DEPTH];
static net_info_t net_info;

static inline int net_rx_ring_empty(void)
{
    return net_rx_head == net_rx_tail;
}

static inline int net_rx_ring_full(void)
{
    return (uint8_t)(net_rx_head + 1U) == net_rx_tail;
}

static void net_rx_ring_push(const uint8_t *buf, uint16_t len)
{
    if (!buf || len == 0U)
        return;

    if (net_rx_ring_full()) {
        net_info.rx_drops++;
        return;
    }

    net_rx_ring[net_rx_head].len = len;
    for (uint16_t i = 0U; i < len; i++)
        net_rx_ring[net_rx_head].data[i] = buf[i];

    __asm__ volatile("dmb sy" ::: "memory");
    net_rx_head++;
    net_info.rx_packets++;
}

static int net_rx_ring_pop(void *buf, uint32_t maxlen)
{
    uint16_t len;
    uint8_t *dst = (uint8_t *)buf;

    if (!buf || maxlen == 0U || net_rx_ring_empty())
        return 0;

    len = net_rx_ring[net_rx_tail].len;
    if (len > maxlen)
        len = (uint16_t)maxlen;

    for (uint16_t i = 0U; i < len; i++)
        dst[i] = net_rx_ring[net_rx_tail].data[i];

    __asm__ volatile("dmb sy" ::: "memory");
    net_rx_tail++;
    return (int)len;
}

static inline uint32_t net_mmio_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(net_base + off);
}

static inline void net_mmio_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(net_base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint8_t net_cfg_read8(uint32_t off)
{
    return *(volatile uint8_t *)(uintptr_t)(net_base + VMMIO_CONFIG + off);
}

static inline uint16_t net_cfg_read16(uint32_t off)
{
    return *(volatile uint16_t *)(uintptr_t)(net_base + VMMIO_CONFIG + off);
}

static uint32_t net_dev_features(uint32_t page)
{
    net_mmio_write(VMMIO_DEV_FEAT_SEL, page);
    return net_mmio_read(VMMIO_DEV_FEATURES);
}

static int net_link_up(void)
{
    if (!net_info.has_status)
        return 1;
    net_info.status = net_cfg_read16(6U);
    return (net_info.status & VNET_S_LINK_UP) ? 1 : 0;
}

static void net_print_mac(const uint8_t mac[6])
{
    static const char hex[] = "0123456789abcdef";

    for (uint32_t i = 0U; i < 6U; i++) {
        uart_putc(hex[(mac[i] >> 4) & 0xFU]);
        uart_putc(hex[mac[i] & 0xFU]);
        if (i != 5U)
            uart_putc(':');
    }
}

static uintptr_t net_find_device(void)
{
    int saw_legacy = 0;

    for (uint32_t slot = 0U; slot < VMMIO_MAX_SLOTS; slot++) {
        uintptr_t base = VMMIO_BASE + (uintptr_t)(slot * VMMIO_SLOT_SIZE);
        volatile uint32_t *p = (volatile uint32_t *)base;
        uint32_t version;

        if (p[VMMIO_MAGIC / 4] != VMMIO_MAGIC_VALUE)
            continue;
        if (p[VMMIO_DEVICE_ID / 4] != VIRTIO_DEVICE_NET)
            continue;

        version = p[VMMIO_VERSION / 4];
        if (version == 1U) {
            saw_legacy = 1;
            continue;
        }
        if (version != 2U)
            continue;

        net_irq = IRQ_VIRTIO(slot);
        return base;
    }

    if (saw_legacy) {
        uart_puts("[NET] WARN: virtio-net MMIO legacy rilevato\n");
        uart_puts("[NET] WARN: usa -global virtio-mmio.force-legacy=false\n");
    }

    return 0U;
}

static int net_configure_queue(uint32_t queue_id, uint8_t *vq_mem,
                               uint16_t want_size, uint16_t *out_size)
{
    uint32_t qmax;
    uint16_t qsize;
    uint64_t desc_pa;
    uint64_t avail_pa;
    uint64_t used_pa;

    net_mmio_write(VMMIO_QUEUE_SEL, queue_id);
    qmax = net_mmio_read(VMMIO_QUEUE_NUM_MAX);
    if (qmax == 0U)
        return 0;

    qsize = (qmax < want_size) ? (uint16_t)qmax : want_size;
    net_mmio_write(VMMIO_QUEUE_NUM, qsize);

    desc_pa = (uint64_t)(uintptr_t)(vq_mem + 0);
    avail_pa = (uint64_t)(uintptr_t)(vq_mem + 256);
    used_pa = (uint64_t)(uintptr_t)(vq_mem + 512);

    net_mmio_write(VMMIO_QUEUE_DESC_LO, (uint32_t)desc_pa);
    net_mmio_write(VMMIO_QUEUE_DESC_HI, (uint32_t)(desc_pa >> 32));
    net_mmio_write(VMMIO_QUEUE_DRV_LO, (uint32_t)avail_pa);
    net_mmio_write(VMMIO_QUEUE_DRV_HI, (uint32_t)(avail_pa >> 32));
    net_mmio_write(VMMIO_QUEUE_DEV_LO, (uint32_t)used_pa);
    net_mmio_write(VMMIO_QUEUE_DEV_HI, (uint32_t)(used_pa >> 32));
    net_mmio_write(VMMIO_QUEUE_READY, 1U);

    *out_size = qsize;
    return 1;
}

static void net_rx_requeue_desc(uint16_t desc_id)
{
    RX_AVAIL->ring[rx_next_avail % rx_queue_size] = desc_id;
    __asm__ volatile("dmb sy" ::: "memory");
    rx_next_avail++;
    RX_AVAIL->idx = rx_next_avail;
    cache_flush_range((uintptr_t)RX_AVAIL, sizeof(*RX_AVAIL));
    net_mmio_write(VMMIO_QUEUE_NOTIFY, VNET_RX_QUEUE_ID);
}

static void net_drain_tx_used(void)
{
    cache_invalidate_range((uintptr_t)TX_USED, sizeof(*TX_USED));

    while (tx_last_used != TX_USED->idx) {
        vring_used_elem_t elem = TX_USED->ring[tx_last_used % tx_queue_size];
        uint16_t desc_id = (uint16_t)elem.id;

        if (desc_id < tx_queue_size && tx_inflight[desc_id]) {
            tx_inflight[desc_id] = 0U;
            net_info.tx_packets++;
        }

        tx_last_used++;
        cache_invalidate_range((uintptr_t)TX_USED, sizeof(*TX_USED));
    }
}

static void net_drain_rx_used(void)
{
    cache_invalidate_range((uintptr_t)RX_USED, sizeof(*RX_USED));

    while (rx_last_used != RX_USED->idx) {
        vring_used_elem_t elem = RX_USED->ring[rx_last_used % rx_queue_size];
        uint16_t desc_id = (uint16_t)elem.id;
        uint32_t total_len = elem.len;

        if (desc_id < rx_queue_size) {
            if (total_len > (uint32_t)(sizeof(vnet_hdr_t) + NET_FRAME_MAX))
                total_len = (uint32_t)(sizeof(vnet_hdr_t) + NET_FRAME_MAX);
            if (total_len > sizeof(vnet_hdr_t)) {
                uint16_t payload_len = (uint16_t)(total_len - sizeof(vnet_hdr_t));

                if (payload_len > NET_FRAME_MAX)
                    payload_len = NET_FRAME_MAX;

                cache_invalidate_range((uintptr_t)rx_dma[desc_id], total_len);
                net_rx_ring_push(&rx_dma[desc_id][sizeof(vnet_hdr_t)], payload_len);
            } else {
                net_info.rx_drops++;
            }

            net_rx_requeue_desc(desc_id);
        }

        rx_last_used++;
        cache_invalidate_range((uintptr_t)RX_USED, sizeof(*RX_USED));
    }
}

static void net_drain_queues(void)
{
    uint32_t isr;

    if (!net_base)
        return;

    isr = net_mmio_read(VMMIO_IRQ_STATUS);
    if (isr)
        net_mmio_write(VMMIO_IRQ_ACK, isr);

    net_drain_rx_used();
    net_drain_tx_used();
}

static void net_irq_handler(uint32_t irq, void *data)
{
    (void)irq;
    (void)data;
    net_drain_queues();
}

static int net_transport_init(void)
{
    uint32_t dev0;
    uint32_t dev1;
    uint32_t drv0 = 0U;

    net_mmio_write(VMMIO_STATUS, 0U);
    net_mmio_write(VMMIO_STATUS, VSTAT_ACKNOWLEDGE);
    net_mmio_write(VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    dev0 = net_dev_features(0U);
    dev1 = net_dev_features(1U);

    if (dev0 & (1U << VNET_F_MAC_NUM)) {
        drv0 |= (1U << VNET_F_MAC_NUM);
        net_info.has_mac = 1U;
    }
    if (dev0 & (1U << VNET_F_STATUS_NUM)) {
        drv0 |= (1U << VNET_F_STATUS_NUM);
        net_info.has_status = 1U;
    }
    if ((dev1 & VIRTIO_F_VERSION_1) == 0U) {
        uart_puts("[NET] ERR: VIRTIO_F_VERSION_1 mancante\n");
        net_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
        return 0;
    }

    net_mmio_write(VMMIO_DRV_FEAT_SEL, 0U);
    net_mmio_write(VMMIO_DRV_FEATURES, drv0);
    net_mmio_write(VMMIO_DRV_FEAT_SEL, 1U);
    net_mmio_write(VMMIO_DRV_FEATURES, VIRTIO_F_VERSION_1);

    net_mmio_write(VMMIO_STATUS,
                   VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    if ((net_mmio_read(VMMIO_STATUS) & VSTAT_FEATURES_OK) == 0U) {
        uart_puts("[NET] ERR: FEATURES_OK non accettato\n");
        net_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
        return 0;
    }

    if (!net_configure_queue(VNET_RX_QUEUE_ID, rx_vq_mem,
                             NET_RX_QUEUE_DEPTH, &rx_queue_size)) {
        uart_puts("[NET] ERR: RX queue assente\n");
        net_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
        return 0;
    }
    if (!net_configure_queue(VNET_TX_QUEUE_ID, tx_vq_mem,
                             NET_TX_QUEUE_DEPTH, &tx_queue_size)) {
        uart_puts("[NET] ERR: TX queue assente\n");
        net_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
        return 0;
    }

    for (uint16_t i = 0U; i < rx_queue_size; i++) {
        RX_DESC[i].addr  = (uint64_t)(uintptr_t)rx_dma[i];
        RX_DESC[i].len   = (uint32_t)(sizeof(vnet_hdr_t) + NET_FRAME_MAX);
        RX_DESC[i].flags = VRING_DESC_F_WRITE;
        RX_DESC[i].next  = 0U;
        RX_AVAIL->ring[i] = i;
    }
    RX_AVAIL->flags = 0U;
    RX_AVAIL->idx   = rx_queue_size;
    RX_USED->flags  = 0U;
    RX_USED->idx    = 0U;
    rx_next_avail   = rx_queue_size;
    rx_last_used    = 0U;

    for (uint16_t i = 0U; i < tx_queue_size; i++) {
        TX_DESC[i].addr  = (uint64_t)(uintptr_t)tx_dma[i];
        TX_DESC[i].len   = 0U;
        TX_DESC[i].flags = 0U;
        TX_DESC[i].next  = 0U;
        tx_inflight[i]   = 0U;
    }
    TX_AVAIL->flags = 1U; /* NO_INTERRUPT lato TX */
    TX_AVAIL->idx   = 0U;
    TX_USED->flags  = 0U;
    TX_USED->idx    = 0U;
    tx_next_avail   = 0U;
    tx_last_used    = 0U;

    cache_flush_range((uintptr_t)rx_vq_mem, sizeof(rx_vq_mem));
    cache_flush_range((uintptr_t)tx_vq_mem, sizeof(tx_vq_mem));
    cache_flush_range((uintptr_t)rx_dma, sizeof(rx_dma));
    cache_flush_range((uintptr_t)tx_dma, sizeof(tx_dma));

    net_mmio_write(VMMIO_STATUS,
                   VSTAT_ACKNOWLEDGE | VSTAT_DRIVER |
                   VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);
    net_mmio_write(VMMIO_QUEUE_NOTIFY, VNET_RX_QUEUE_ID);

    return 1;
}

int net_init(void)
{
    net_base = net_find_device();
    if (!net_base) {
        uart_puts("[NET] Nessun virtio-net trovato\n");
        return 0;
    }

    net_rx_head = 0U;
    net_rx_tail = 0U;
    for (uint32_t i = 0U; i < sizeof(net_info); i++)
        ((uint8_t *)&net_info)[i] = 0U;
    net_info.mtu = 1500U;

    if (!net_transport_init()) {
        net_base = 0U;
        return 0;
    }

    if (net_info.has_mac) {
        for (uint32_t i = 0U; i < 6U; i++)
            net_info.mac[i] = net_cfg_read8(i);
    }
    if (net_info.has_status)
        net_info.status = net_cfg_read16(6U);
    net_info.link_up = (uint8_t)net_link_up();

    net_drain_queues();
    gic_register_irq(net_irq, net_irq_handler, NULL,
                     GIC_PRIO_DRIVER, GIC_FLAG_LEVEL);
    gic_enable_irq(net_irq);

    uart_puts("[NET] VirtIO-net pronto: mac=");
    if (net_info.has_mac)
        net_print_mac(net_info.mac);
    else
        uart_puts("none");
    uart_puts(" link=");
    uart_puts(net_info.link_up ? "up" : "down");
    uart_puts(" rxq=");
    uart_putc('0' + (char)(rx_queue_size / 10U % 10U));
    uart_putc('0' + (char)(rx_queue_size % 10U));
    uart_puts(" txq=");
    uart_putc('0' + (char)(tx_queue_size / 10U % 10U));
    uart_putc('0' + (char)(tx_queue_size % 10U));
    uart_puts("\n");
    return 1;
}

int net_is_ready(void)
{
    return net_base != 0U;
}

int net_get_info(net_info_t *out)
{
    if (!out)
        return NET_ERR_RANGE;
    if (!net_base)
        return NET_ERR_NOT_READY;

    net_info.link_up = (uint8_t)net_link_up();
    *out = net_info;
    return NET_OK;
}

int net_recv(void *buf, uint32_t maxlen)
{
    if (!net_base)
        return NET_ERR_NOT_READY;
    if (!buf || maxlen == 0U)
        return NET_ERR_RANGE;

    net_drain_queues();
    return net_rx_ring_pop(buf, maxlen);
}

int net_send(const void *buf, uint32_t len)
{
    uint16_t slot = 0xFFFFU;
    uint32_t timeout = NET_TX_TIMEOUT;
    uint8_t *dst;
    uint32_t frame_len;

    if (!net_base)
        return NET_ERR_NOT_READY;
    if (!buf || len == 0U || len > NET_FRAME_MAX)
        return NET_ERR_RANGE;

    net_drain_queues();

    for (uint16_t i = 0U; i < tx_queue_size; i++) {
        if (!tx_inflight[i]) {
            slot = i;
            break;
        }
    }
    if (slot == 0xFFFFU)
        return NET_ERR_BUSY;

    dst = tx_dma[slot];
    frame_len = (len < 60U) ? 60U : len;

    for (uint32_t i = 0U; i < sizeof(vnet_hdr_t); i++)
        dst[i] = 0U;
    for (uint32_t i = 0U; i < len; i++)
        dst[sizeof(vnet_hdr_t) + i] = ((const uint8_t *)buf)[i];
    for (uint32_t i = len; i < frame_len; i++)
        dst[sizeof(vnet_hdr_t) + i] = 0U;

    TX_DESC[slot].addr  = (uint64_t)(uintptr_t)tx_dma[slot];
    TX_DESC[slot].len   = (uint32_t)(sizeof(vnet_hdr_t) + frame_len);
    TX_DESC[slot].flags = 0U;
    TX_DESC[slot].next  = 0U;
    tx_inflight[slot]   = 1U;

    cache_flush_range((uintptr_t)tx_dma[slot], sizeof(vnet_hdr_t) + frame_len);
    cache_flush_range((uintptr_t)&TX_DESC[slot], sizeof(TX_DESC[slot]));

    TX_AVAIL->ring[tx_next_avail % tx_queue_size] = slot;
    __asm__ volatile("dmb sy" ::: "memory");
    tx_next_avail++;
    TX_AVAIL->idx = tx_next_avail;
    cache_flush_range((uintptr_t)TX_AVAIL, sizeof(*TX_AVAIL));
    net_mmio_write(VMMIO_QUEUE_NOTIFY, VNET_TX_QUEUE_ID);

    while (timeout-- > 0U) {
        net_drain_queues();
        if (!tx_inflight[slot])
            return NET_OK;
    }

    net_info.tx_errors++;
    return NET_ERR_TIMEOUT;
}

int net_selftest_run(void)
{
    net_info_t info;
    uint8_t    frame[60];
    int        rc;
    int        all_zero = 1;

    rc = net_get_info(&info);
    if (rc != NET_OK)
        return -1;

    for (uint32_t i = 0U; i < 6U; i++) {
        if (info.mac[i] != 0U) {
            all_zero = 0;
            break;
        }
    }
    if (info.has_mac && all_zero)
        return -1;

    for (uint32_t i = 0U; i < 6U; i++)
        frame[i] = 0xFFU;
    for (uint32_t i = 0U; i < 6U; i++)
        frame[6U + i] = info.mac[i];
    frame[12] = 0x88U;
    frame[13] = 0xB5U;
    for (uint32_t i = 14U; i < sizeof(frame); i++)
        frame[i] = (uint8_t)i;

    rc = net_send(frame, sizeof(frame));
    if (rc != NET_OK)
        return -1;

    rc = net_get_info(&info);
    if (rc != NET_OK || info.tx_packets == 0U)
        return -1;

    return 0;
}
