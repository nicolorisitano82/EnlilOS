/*
 * EnlilOS Microkernel - Console Input Backend (M4-02)
 *
 * Backend supportati, in ordine di preferenza:
 *   1. VirtIO Input keyboard su QEMU virt (virtio-mmio, queue eventq)
 *   2. UART PL011 RX su stdio QEMU come fallback universale
 *
 * Il backend VirtIO e' interrupt-driven: il device scrive eventi key nel
 * ring eventq, l'IRQ handler li converte in ASCII e li inserisce in un
 * ring buffer SPSC da cui sys_read() consuma in modo non bloccante.
 */

#include "keyboard.h"
#include "gic.h"
#include "mmu.h"
#include "uart.h"

/* ── Ring buffer ASCII (IRQ producer, task consumer) ─────────────── */

#define KBD_BUF_SIZE    256U

static uint8_t kbd_buf[KBD_BUF_SIZE];
static uint8_t kbd_head;
static uint8_t kbd_tail;

static inline int kbd_buf_empty(void)
{
    return kbd_head == kbd_tail;
}

static inline int kbd_buf_full(void)
{
    return (uint8_t)(kbd_head + 1U) == kbd_tail;
}

static void kbd_buf_push(uint8_t c)
{
    if (!kbd_buf_full()) {
        kbd_buf[kbd_head] = c;
        __asm__ volatile("dmb sy" ::: "memory");
        kbd_head++;
    }
}

static int kbd_buf_pop(void)
{
    if (kbd_buf_empty()) return -1;

    uint8_t c = kbd_buf[kbd_tail];
    __asm__ volatile("dmb sy" ::: "memory");
    kbd_tail++;
    return (int)c;
}

/* ── Tabella keycode Linux input → ASCII ─────────────────────────── */

static const uint8_t key_normal[128] = {
    [1]  = 0x1B,
    [2]  = '1', [3]  = '2', [4]  = '3', [5]  = '4',
    [6]  = '5', [7]  = '6', [8]  = '7', [9]  = '8',
    [10] = '9', [11] = '0', [12] = '-', [13] = '=',
    [14] = '\b', [15] = '\t',
    [16] = 'q', [17] = 'w', [18] = 'e', [19] = 'r',
    [20] = 't', [21] = 'y', [22] = 'u', [23] = 'i',
    [24] = 'o', [25] = 'p', [26] = '[', [27] = ']',
    [28] = '\n',
    [30] = 'a', [31] = 's', [32] = 'd', [33] = 'f',
    [34] = 'g', [35] = 'h', [36] = 'j', [37] = 'k',
    [38] = 'l', [39] = ';', [40] = '\'', [41] = '`',
    [43] = '\\',
    [44] = 'z', [45] = 'x', [46] = 'c', [47] = 'v',
    [48] = 'b', [49] = 'n', [50] = 'm', [51] = ',',
    [52] = '.', [53] = '/', [57] = ' ',
};

static const uint8_t key_shifted[128] = {
    [1]  = 0x1B,
    [2]  = '!', [3]  = '@', [4]  = '#', [5]  = '$',
    [6]  = '%', [7]  = '^', [8]  = '&', [9]  = '*',
    [10] = '(', [11] = ')', [12] = '_', [13] = '+',
    [14] = '\b', [15] = '\t',
    [16] = 'Q', [17] = 'W', [18] = 'E', [19] = 'R',
    [20] = 'T', [21] = 'Y', [22] = 'U', [23] = 'I',
    [24] = 'O', [25] = 'P', [26] = '{', [27] = '}',
    [28] = '\n',
    [30] = 'A', [31] = 'S', [32] = 'D', [33] = 'F',
    [34] = 'G', [35] = 'H', [36] = 'J', [37] = 'K',
    [38] = 'L', [39] = ':', [40] = '"', [41] = '~',
    [43] = '|',
    [44] = 'Z', [45] = 'X', [46] = 'C', [47] = 'V',
    [48] = 'B', [49] = 'N', [50] = 'M', [51] = '<',
    [52] = '>', [53] = '?', [57] = ' ',
};

/* ── VirtIO Input (virtio-mmio) ──────────────────────────────────── */

#define VMMIO_BASE           0x0a000000UL
#define VMMIO_SLOT_SIZE      0x200UL
#define VMMIO_MAX_SLOTS      32U

#define VMMIO_MAGIC          0x000U
#define VMMIO_VERSION        0x004U
#define VMMIO_DEVICE_ID      0x008U
#define VMMIO_DEV_FEATURES   0x010U
#define VMMIO_DEV_FEAT_SEL   0x014U
#define VMMIO_DRV_FEATURES   0x020U
#define VMMIO_DRV_FEAT_SEL   0x024U
#define VMMIO_QUEUE_SEL      0x030U
#define VMMIO_QUEUE_NUM_MAX  0x034U
#define VMMIO_QUEUE_NUM      0x038U
#define VMMIO_QUEUE_READY    0x044U
#define VMMIO_QUEUE_NOTIFY   0x050U
#define VMMIO_IRQ_STATUS     0x060U
#define VMMIO_IRQ_ACK        0x064U
#define VMMIO_STATUS         0x070U
#define VMMIO_QUEUE_DESC_LO  0x080U
#define VMMIO_QUEUE_DESC_HI  0x084U
#define VMMIO_QUEUE_DRV_LO   0x090U
#define VMMIO_QUEUE_DRV_HI   0x094U
#define VMMIO_QUEUE_DEV_LO   0x0A0U
#define VMMIO_QUEUE_DEV_HI   0x0A4U
#define VMMIO_CONFIG         0x100U

#define VMMIO_MAGIC_VALUE    0x74726976UL
#define VIRTIO_DEVICE_INPUT  18U

#define VSTAT_ACKNOWLEDGE    1U
#define VSTAT_DRIVER         2U
#define VSTAT_DRIVER_OK      4U
#define VSTAT_FEATURES_OK    8U
#define VSTAT_FAILED         128U

#define VIRTIO_F_VERSION_1   (1U << 0)

#define VIRTIO_INPUT_CFG_ID_NAME  0x01U
#define VIRTIO_INPUT_CFG_EV_BITS  0x11U

#define EV_SYN               0U
#define EV_KEY               1U

#define SYN_REPORT           0U

#define KEY_ENTER            28U
#define KEY_A                30U
#define KEY_LEFTCTRL         29U
#define KEY_LEFTSHIFT        42U
#define KEY_RIGHTSHIFT       54U
#define KEY_SPACE            57U
#define KEY_RIGHTCTRL        97U

#define VIQ_SIZE             16U

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vring_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIQ_SIZE];
    uint16_t used_event;
} __attribute__((packed)) vring_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vring_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[VIQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) vring_used_t;

typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed)) virtio_input_event_t;

static uint8_t vi_vq_mem[4096] __attribute__((aligned(4096)));
static virtio_input_event_t vi_events[VIQ_SIZE] __attribute__((aligned(64)));

#define VI_DESC  ((vring_desc_t  *)(vi_vq_mem + 0))
#define VI_AVAIL ((vring_avail_t *)(vi_vq_mem + 256))
#define VI_USED  ((vring_used_t  *)(vi_vq_mem + 512))

typedef enum {
    KBD_BACKEND_UART = 0,
    KBD_BACKEND_VIRTIO = 1,
} keyboard_backend_t;

static keyboard_backend_t kbd_backend;
static uintptr_t vi_base;
static uint32_t  vi_irq;
static uint16_t  vi_queue_size;
static uint16_t  vi_last_used;
static uint16_t  vi_next_avail;
static uint8_t   vi_ctrl;
static uint8_t   vi_shift;

static inline uint32_t vi_mmio_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(vi_base + off);
}

static inline void vi_mmio_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(vi_base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint8_t vi_cfg_read8(uint32_t off)
{
    return *(volatile uint8_t *)(uintptr_t)(vi_base + VMMIO_CONFIG + off);
}

static inline void vi_cfg_write8(uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(uintptr_t)(vi_base + VMMIO_CONFIG + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static void cache_invalidate_range_local(uintptr_t start, size_t size)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));

    uintptr_t line = (uintptr_t)(4UL << ((ctr >> 16) & 0xFU));
    uintptr_t mask = line - 1U;
    uintptr_t addr = start & ~mask;
    uintptr_t end  = start + size;

    while (addr < end) {
        __asm__ volatile("dc ivac, %0" :: "r"(addr) : "memory");
        addr += line;
    }

    __asm__ volatile("dsb sy" ::: "memory");
}

static int vi_config_test_bit(uint8_t select, uint8_t subsel, uint16_t bit)
{
    vi_cfg_write8(0, select);
    vi_cfg_write8(1, subsel);

    uint8_t size = vi_cfg_read8(2);
    uint16_t byte = (uint16_t)(bit / 8U);
    if (byte >= size) return 0;

    uint8_t v = vi_cfg_read8(8U + byte);
    return (v >> (bit & 7U)) & 1U;
}

static void vi_read_name(char *buf, size_t buf_size)
{
    if (buf_size == 0U) return;

    vi_cfg_write8(0, VIRTIO_INPUT_CFG_ID_NAME);
    vi_cfg_write8(1, 0U);

    uint8_t size = vi_cfg_read8(2);
    if ((size_t)size >= buf_size)
        size = (uint8_t)(buf_size - 1U);

    for (uint8_t i = 0; i < size; i++)
        buf[i] = (char)vi_cfg_read8(8U + i);

    buf[size] = '\0';
}

static int vi_is_keyboard(uintptr_t base)
{
    uintptr_t saved = vi_base;
    vi_base = base;

    int looks_like_keyboard =
        vi_config_test_bit(VIRTIO_INPUT_CFG_EV_BITS, EV_KEY, KEY_A) &&
        vi_config_test_bit(VIRTIO_INPUT_CFG_EV_BITS, EV_KEY, KEY_ENTER) &&
        vi_config_test_bit(VIRTIO_INPUT_CFG_EV_BITS, EV_KEY, KEY_SPACE);

    vi_base = saved;
    return looks_like_keyboard;
}

static uintptr_t vi_find_keyboard(void)
{
    int saw_legacy = 0;

    for (uint32_t slot = 0U; slot < VMMIO_MAX_SLOTS; slot++) {
        uintptr_t base = VMMIO_BASE + (uintptr_t)(slot * VMMIO_SLOT_SIZE);
        volatile uint32_t *p = (volatile uint32_t *)base;

        if (p[VMMIO_MAGIC / 4] != VMMIO_MAGIC_VALUE) continue;
        if (p[VMMIO_DEVICE_ID / 4] != VIRTIO_DEVICE_INPUT) continue;

        uint32_t version = p[VMMIO_VERSION / 4];
        if (version == 1U) {
            saw_legacy = 1;
            continue;
        }
        if (version != 2U) continue;
        if (!vi_is_keyboard(base)) continue;

        vi_irq = IRQ_VIRTIO(slot);
        return base;
    }

    if (saw_legacy) {
        uart_puts("[KBD] WARN: virtio-input MMIO legacy rilevato\n");
        uart_puts("[KBD] WARN: usa -global virtio-mmio.force-legacy=false\n");
    }

    return 0U;
}

static void vi_requeue_desc(uint16_t desc_id)
{
    VI_AVAIL->ring[vi_next_avail % vi_queue_size] = desc_id;
    __asm__ volatile("dmb sy" ::: "memory");
    vi_next_avail++;
    VI_AVAIL->idx = vi_next_avail;

    cache_flush_range((uintptr_t)vi_vq_mem, sizeof(vi_vq_mem));
    vi_mmio_write(VMMIO_QUEUE_NOTIFY, 0U);
}

static void vi_process_key(uint16_t code, uint32_t value)
{
    if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) {
        vi_ctrl = (value != 0U) ? 1U : 0U;
        return;
    }

    if (code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT) {
        vi_shift = (value != 0U) ? 1U : 0U;
        return;
    }

    if (value != 1U && value != 2U) return;
    if (code >= 128U) return;

    uint8_t c = vi_shift ? key_shifted[code] : key_normal[code];
    if (vi_ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c = (uint8_t)((c & 0x1FU));
    if (c) kbd_buf_push(c);
}

static void vi_drain_events(void)
{
    if (!vi_base || vi_queue_size == 0U) return;

    uint32_t isr = vi_mmio_read(VMMIO_IRQ_STATUS);
    if (isr)
        vi_mmio_write(VMMIO_IRQ_ACK, isr);

    cache_invalidate_range_local((uintptr_t)VI_USED, sizeof(*VI_USED));

    while (vi_last_used != VI_USED->idx) {
        vring_used_elem_t elem = VI_USED->ring[vi_last_used % vi_queue_size];
        uint16_t desc_id = (uint16_t)elem.id;

        if (desc_id < vi_queue_size) {
            cache_invalidate_range_local((uintptr_t)&vi_events[desc_id],
                                         sizeof(vi_events[desc_id]));

            virtio_input_event_t ev = vi_events[desc_id];
            if (ev.type == EV_KEY)
                vi_process_key(ev.code, ev.value);

            vi_requeue_desc(desc_id);
        }

        vi_last_used++;
        cache_invalidate_range_local((uintptr_t)VI_USED, sizeof(*VI_USED));
    }
}

static void vi_irq_handler(uint32_t irq, void *data)
{
    (void)irq;
    (void)data;
    vi_drain_events();
}

static int vi_transport_init(void)
{
    vi_mmio_write(VMMIO_STATUS, 0U);
    vi_mmio_write(VMMIO_STATUS, VSTAT_ACKNOWLEDGE);
    vi_mmio_write(VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    vi_mmio_write(VMMIO_DRV_FEAT_SEL, 0U);
    vi_mmio_write(VMMIO_DRV_FEATURES, 0U);
    vi_mmio_write(VMMIO_DRV_FEAT_SEL, 1U);
    vi_mmio_write(VMMIO_DRV_FEATURES, VIRTIO_F_VERSION_1);

    vi_mmio_write(VMMIO_STATUS,
                  VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    if (!(vi_mmio_read(VMMIO_STATUS) & VSTAT_FEATURES_OK)) {
        vi_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
        return 0;
    }

    vi_mmio_write(VMMIO_QUEUE_SEL, 0U);
    uint32_t qmax = vi_mmio_read(VMMIO_QUEUE_NUM_MAX);
    if (qmax == 0U) {
        vi_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
        return 0;
    }

    vi_queue_size = (qmax < VIQ_SIZE) ? (uint16_t)qmax : (uint16_t)VIQ_SIZE;
    vi_mmio_write(VMMIO_QUEUE_NUM, vi_queue_size);

    uint64_t desc_pa  = (uint64_t)(uintptr_t)(vi_vq_mem + 0);
    uint64_t avail_pa = (uint64_t)(uintptr_t)(vi_vq_mem + 256);
    uint64_t used_pa  = (uint64_t)(uintptr_t)(vi_vq_mem + 512);

    vi_mmio_write(VMMIO_QUEUE_DESC_LO, (uint32_t)desc_pa);
    vi_mmio_write(VMMIO_QUEUE_DESC_HI, (uint32_t)(desc_pa >> 32));
    vi_mmio_write(VMMIO_QUEUE_DRV_LO,  (uint32_t)avail_pa);
    vi_mmio_write(VMMIO_QUEUE_DRV_HI,  (uint32_t)(avail_pa >> 32));
    vi_mmio_write(VMMIO_QUEUE_DEV_LO,  (uint32_t)used_pa);
    vi_mmio_write(VMMIO_QUEUE_DEV_HI,  (uint32_t)(used_pa >> 32));

    for (uint16_t i = 0U; i < vi_queue_size; i++) {
        VI_DESC[i].addr  = (uint64_t)(uintptr_t)&vi_events[i];
        VI_DESC[i].len   = (uint32_t)sizeof(vi_events[i]);
        VI_DESC[i].flags = 2U; /* device-writable */
        VI_DESC[i].next  = 0U;
        VI_AVAIL->ring[i] = i;
    }

    vi_next_avail = vi_queue_size;
    VI_AVAIL->idx = vi_queue_size;

    cache_flush_range((uintptr_t)vi_events,
                      sizeof(virtio_input_event_t) * vi_queue_size);
    cache_flush_range((uintptr_t)vi_vq_mem, sizeof(vi_vq_mem));

    vi_mmio_write(VMMIO_QUEUE_READY, 1U);
    vi_mmio_write(VMMIO_QUEUE_NOTIFY, 0U);

    vi_mmio_write(VMMIO_STATUS,
                  VSTAT_ACKNOWLEDGE | VSTAT_DRIVER |
                  VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);

    vi_last_used = 0U;
    return 1;
}

static int vi_init(void)
{
    vi_base = vi_find_keyboard();
    if (!vi_base) return 0;

    if (!vi_transport_init()) {
        uart_puts("[KBD] ERR: init virtio-input fallita\n");
        vi_base = 0U;
        return 0;
    }

    char name[40];
    vi_read_name(name, sizeof(name));

    /*
     * Il device puo' alzare la linea IRQ non appena la queue e' pronta.
     * Se l'handler scatta prima che il backend sia attivo, dobbiamo
     * comunque ackare e drenare l'eventq per evitare un level IRQ storm.
     */
    kbd_backend = KBD_BACKEND_VIRTIO;
    vi_drain_events();

    gic_register_irq(vi_irq, vi_irq_handler, NULL,
                     GIC_PRIO_DRIVER, GIC_FLAG_LEVEL);
    gic_enable_irq(vi_irq);

    uart_puts("[KBD] Backend: VirtIO Input");
    if (name[0] != '\0') {
        uart_puts(" (");
        uart_puts(name);
        uart_puts(")");
    }
    uart_puts(" — IRQ attivo\n");
    return 1;
}

/* ── API pubblica ────────────────────────────────────────────────── */

void keyboard_init(void)
{
    if (vi_init()) {
        return;
    }

    kbd_backend = KBD_BACKEND_UART;
    uart_puts("[KBD] Backend: UART fallback su PL011 stdio\n");
}

int keyboard_getc(void)
{
    if (kbd_backend == KBD_BACKEND_VIRTIO) {
        if (kbd_buf_empty())
            vi_drain_events();

        int c = kbd_buf_pop();
        if (c >= 0) return c;
    }

    return uart_getc_nonblock();
}
