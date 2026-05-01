/*
 * EnlilOS Microkernel - Mouse Input Backend (M4-05)
 *
 * Backend supportato:
 *   1. VirtIO Input pointer su QEMU virt (virtio-mmio, queue eventq)
 *
 * Il device produce eventi Linux input (`EV_REL`, `EV_ABS`, `EV_KEY`) che
 * vengono aggregati per SYN_REPORT e convertiti in record `mouse_event_t`
 * inseriti in una ring buffer SPSC.
 */

#include "mouse.h"
#include "virtio_mmio.h"
#include "framebuffer.h"
#include "gic.h"
#include "mmu.h"
#include "uart.h"

/* ── Ring buffer eventi mouse (IRQ producer, task consumer) ───────── */

#define MOUSE_BUF_SIZE    256U

static mouse_event_t mouse_buf[MOUSE_BUF_SIZE];
static uint8_t       mouse_head;
static uint8_t       mouse_tail;

static inline int mouse_buf_empty(void)
{
    return mouse_head == mouse_tail;
}

static inline int mouse_buf_full(void)
{
    return (uint8_t)(mouse_head + 1U) == mouse_tail;
}

static void mouse_buf_push(const mouse_event_t *ev)
{
    if (!ev || mouse_buf_full()) return;

    mouse_buf[mouse_head] = *ev;
    __asm__ volatile("dmb sy" ::: "memory");
    mouse_head++;
}

static int mouse_buf_pop(mouse_event_t *out)
{
    if (!out || mouse_buf_empty()) return 0;

    *out = mouse_buf[mouse_tail];
    __asm__ volatile("dmb sy" ::: "memory");
    mouse_tail++;
    return 1;
}

/* ── VirtIO Input (virtio-mmio) ───────────────────────────────────── */

/* Queue depth per il mouse input eventq */
#define MIQ_SIZE             16U

/* vring_avail e vring_used con ring di lunghezza MIQ_SIZE */
typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[MIQ_SIZE];
    uint16_t used_event;
} __attribute__((packed)) vring_avail_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[MIQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) vring_used_t;

static uint8_t vi_vq_mem[4096] __attribute__((aligned(4096)));
static virtio_input_event_t vi_events[MIQ_SIZE] __attribute__((aligned(64)));

#define MI_DESC  ((vring_desc_t  *)(vi_vq_mem + 0))
#define MI_AVAIL ((vring_avail_t *)(vi_vq_mem + 256))
#define MI_USED  ((vring_used_t  *)(vi_vq_mem + 512))

typedef enum {
    MOUSE_BACKEND_NONE = 0,
    MOUSE_BACKEND_VIRTIO = 1,
} mouse_backend_t;

static mouse_backend_t mi_backend;
static uintptr_t       mi_base;
static uint32_t        mi_irq;
static uint16_t        mi_queue_size;
static uint16_t        mi_last_used;
static uint16_t        mi_next_avail;
static uint8_t         mi_has_rel;
static uint8_t         mi_has_abs;
static uint32_t        mi_buttons;
static int32_t         mi_pending_dx;
static int32_t         mi_pending_dy;
static int32_t         mi_pending_wheel;
static uint8_t         mi_pending_button_change;
static int32_t         mi_abs_raw_x;
static int32_t         mi_abs_raw_y;
static uint8_t         mi_have_abs_x;
static uint8_t         mi_have_abs_y;
static int32_t         mi_abs_min_x;
static int32_t         mi_abs_max_x;
static int32_t         mi_abs_min_y;
static int32_t         mi_abs_max_y;
static uint32_t        mi_abs_px_x;
static uint32_t        mi_abs_px_y;
static uint32_t        mi_debug_log_budget;

static void mi_log_u32(uint32_t value)
{
    char buf[16];
    uint32_t pos = 0U;

    if (value == 0U) {
        uart_putc('0');
        return;
    }
    while (value != 0U && pos < sizeof(buf)) {
        buf[pos++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (pos > 0U)
        uart_putc(buf[--pos]);
}

static void mi_log_i32(int32_t value)
{
    if (value < 0) {
        uart_putc('-');
        mi_log_u32((uint32_t)(-value));
        return;
    }
    mi_log_u32((uint32_t)value);
}

static void mi_log_event(const mouse_event_t *ev)
{
    if (!ev || mi_debug_log_budget == 0U)
        return;

    mi_debug_log_budget--;
    uart_puts("[MOUSE] ev flags=");
    mi_log_u32(ev->flags);
    uart_puts(" btn=");
    mi_log_u32(ev->buttons);
    uart_puts(" x=");
    mi_log_u32(ev->x);
    uart_puts(" y=");
    mi_log_u32(ev->y);
    uart_puts(" dx=");
    mi_log_i32(ev->dx);
    uart_puts(" dy=");
    mi_log_i32(ev->dy);
    uart_puts(" wh=");
    mi_log_i32(ev->wheel);
    uart_puts("\n");
}

static inline uint32_t mi_mmio_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(mi_base + off);
}

static inline void mi_mmio_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(mi_base + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint8_t mi_cfg_read8(uint32_t off)
{
    return *(volatile uint8_t *)(uintptr_t)(mi_base + VMMIO_CONFIG + off);
}

static inline uint32_t mi_cfg_read32(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(mi_base + VMMIO_CONFIG + off);
}

static inline void mi_cfg_write8(uint32_t off, uint8_t val)
{
    *(volatile uint8_t *)(uintptr_t)(mi_base + VMMIO_CONFIG + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}


static int mi_config_test_bit(uint8_t select, uint8_t subsel, uint16_t bit)
{
    mi_cfg_write8(0, select);
    mi_cfg_write8(1, subsel);

    uint8_t size = mi_cfg_read8(2);
    uint16_t byte = (uint16_t)(bit / 8U);
    if (byte >= size) return 0;

    uint8_t v = mi_cfg_read8(8U + byte);
    return (v >> (bit & 7U)) & 1U;
}

/* Variante senza effetti sul globale mi_base — usata durante il probe. */
static int mi_probe_test_bit(uintptr_t base, uint8_t select,
                             uint8_t subsel, uint16_t bit)
{
    volatile uint8_t *cfg = (volatile uint8_t *)(base + VMMIO_CONFIG);

    cfg[0] = select;
    __asm__ volatile("dsb sy" ::: "memory");
    cfg[1] = subsel;
    __asm__ volatile("dsb sy" ::: "memory");

    uint8_t size = cfg[2];
    uint16_t byte = (uint16_t)(bit / 8U);
    if (byte >= size) return 0;

    uint8_t v = cfg[8U + byte];
    return (v >> (bit & 7U)) & 1U;
}

static void mi_read_name(char *buf, size_t buf_size)
{
    if (buf_size == 0U) return;

    mi_cfg_write8(0, VIRTIO_INPUT_CFG_ID_NAME);
    mi_cfg_write8(1, 0U);

    uint8_t size = mi_cfg_read8(2);
    if ((size_t)size >= buf_size)
        size = (uint8_t)(buf_size - 1U);

    for (uint8_t i = 0; i < size; i++)
        buf[i] = (char)mi_cfg_read8(8U + i);

    buf[size] = '\0';
}

static int mi_read_absinfo(uint8_t axis, int32_t *out_min, int32_t *out_max)
{
    mi_cfg_write8(0, VIRTIO_INPUT_CFG_ABS_INFO);
    mi_cfg_write8(1, axis);

    uint8_t size = mi_cfg_read8(2);
    if (size < 8U || !out_min || !out_max)
        return 0;

    *out_min = (int32_t)mi_cfg_read32(8U);
    *out_max = (int32_t)mi_cfg_read32(12U);
    return 1;
}

static int mi_is_pointer(uintptr_t base)
{
    int has_left = mi_probe_test_bit(base, VIRTIO_INPUT_CFG_EV_BITS, EV_KEY, BTN_LEFT);
    int has_rel  = mi_probe_test_bit(base, VIRTIO_INPUT_CFG_EV_BITS, EV_REL, REL_X) &&
                   mi_probe_test_bit(base, VIRTIO_INPUT_CFG_EV_BITS, EV_REL, REL_Y);
    int has_abs  = mi_probe_test_bit(base, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS, ABS_X) &&
                   mi_probe_test_bit(base, VIRTIO_INPUT_CFG_EV_BITS, EV_ABS, ABS_Y);

    return has_left && (has_rel || has_abs);
}

static uintptr_t mi_find_pointer(void)
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
        if (!mi_is_pointer(base)) continue;

        mi_irq = IRQ_VIRTIO(slot);
        return base;
    }

    if (saw_legacy) {
        uart_puts("[MOUSE] WARN: virtio-input MMIO legacy rilevato\n");
        uart_puts("[MOUSE] WARN: usa -global virtio-mmio.force-legacy=false\n");
    }

    return 0U;
}

static void mi_requeue_desc(uint16_t desc_id)
{
    MI_AVAIL->ring[mi_next_avail % mi_queue_size] = desc_id;
    __asm__ volatile("dmb sy" ::: "memory");
    mi_next_avail++;
    MI_AVAIL->idx = mi_next_avail;

    /* Flush solo l'avail ring (una cache line) — non l'intera pagina vq_mem.
     * Il device legge solo MI_AVAIL->ring e MI_AVAIL->idx per capire quali
     * descriptor sono disponibili. */
    cache_flush_range((uintptr_t)MI_AVAIL, sizeof(*MI_AVAIL));
    mi_mmio_write(VMMIO_QUEUE_NOTIFY, 0U);
}

static uint32_t mi_scale_abs(int32_t value, int32_t min, int32_t max,
                             uint32_t limit)
{
    int64_t range = (int64_t)max - (int64_t)min;
    int64_t pos   = (int64_t)value - (int64_t)min;

    if (limit <= 1U || range <= 0)
        return 0U;

    if (pos < 0) pos = 0;
    if (pos > range) pos = range;

    return (uint32_t)(((uint64_t)pos * (uint64_t)(limit - 1U)) /
                      (uint64_t)range);
}

static void mi_commit_report(void)
{
    mouse_event_t ev;
    uint32_t flags = 0U;

    if (mi_pending_dx != 0 || mi_pending_dy != 0)
        flags |= MOUSE_EVT_MOVE;
    if (mi_pending_wheel != 0)
        flags |= MOUSE_EVT_WHEEL;
    if (mi_pending_button_change)
        flags |= MOUSE_EVT_BUTTON;

    ev.dx      = mi_pending_dx;
    ev.dy      = mi_pending_dy;
    ev.wheel   = mi_pending_wheel;
    ev.x       = mi_abs_px_x;
    ev.y       = mi_abs_px_y;
    ev.buttons = mi_buttons;
    ev.flags   = flags;

    if (mi_has_abs && (mi_have_abs_x || mi_have_abs_y)) {
        if (mi_have_abs_x)
            mi_abs_px_x = mi_scale_abs(mi_abs_raw_x,
                                       mi_abs_min_x, mi_abs_max_x, FB_WIDTH);
        if (mi_have_abs_y)
            mi_abs_px_y = mi_scale_abs(mi_abs_raw_y,
                                       mi_abs_min_y, mi_abs_max_y, FB_HEIGHT);
        ev.x = mi_abs_px_x;
        ev.y = mi_abs_px_y;
        ev.flags |= MOUSE_EVT_ABS | MOUSE_EVT_MOVE;
    }

    if (ev.flags != 0U)
    {
        mouse_buf_push(&ev);
        mi_log_event(&ev);
    }

    mi_pending_dx = 0;
    mi_pending_dy = 0;
    mi_pending_wheel = 0;
    mi_pending_button_change = 0U;
    mi_have_abs_x = 0U;
    mi_have_abs_y = 0U;
}

static void mi_process_input_event(virtio_input_event_t ev)
{
    if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
        mi_commit_report();
        return;
    }

    if (ev.type == EV_KEY) {
        uint32_t mask = 0U;

        if (ev.code == BTN_LEFT)      mask = MOUSE_BTN_LEFT;
        if (ev.code == BTN_RIGHT)     mask = MOUSE_BTN_RIGHT;
        if (ev.code == BTN_MIDDLE)    mask = MOUSE_BTN_MIDDLE;
        if (mask == 0U) return;

        if (ev.value != 0U) mi_buttons |= mask;
        else                mi_buttons &= ~mask;
        mi_pending_button_change = 1U;
        return;
    }

    if (ev.type == EV_REL) {
        if (ev.code == REL_X)     mi_pending_dx += (int32_t)ev.value;
        if (ev.code == REL_Y)     mi_pending_dy += (int32_t)ev.value;
        if (ev.code == REL_WHEEL) mi_pending_wheel += (int32_t)ev.value;
        return;
    }

    if (ev.type == EV_ABS) {
        if (ev.code == ABS_X) {
            mi_abs_raw_x = (int32_t)ev.value;
            mi_have_abs_x = 1U;
        }
        if (ev.code == ABS_Y) {
            mi_abs_raw_y = (int32_t)ev.value;
            mi_have_abs_y = 1U;
        }
    }
}

static void mi_drain_events(void)
{
    if (!mi_base || mi_queue_size == 0U) return;

    uint32_t isr = mi_mmio_read(VMMIO_IRQ_STATUS);
    if (isr)
        mi_mmio_write(VMMIO_IRQ_ACK, isr);

    cache_invalidate_range((uintptr_t)MI_USED, sizeof(*MI_USED));

    while (mi_last_used != MI_USED->idx) {
        vring_used_elem_t elem = MI_USED->ring[mi_last_used % mi_queue_size];
        uint16_t desc_id = (uint16_t)elem.id;

        if (desc_id < mi_queue_size) {
            cache_invalidate_range((uintptr_t)&vi_events[desc_id],
                                         sizeof(vi_events[desc_id]));
            mi_process_input_event(vi_events[desc_id]);
            mi_requeue_desc(desc_id);
        }

        mi_last_used++;
        cache_invalidate_range((uintptr_t)MI_USED, sizeof(*MI_USED));
    }
}

static void mi_irq_handler(uint32_t irq, void *data)
{
    (void)irq;
    (void)data;
    mi_drain_events();
}

static int mi_transport_init(void)
{
    mi_mmio_write(VMMIO_STATUS, 0U);
    mi_mmio_write(VMMIO_STATUS, VSTAT_ACKNOWLEDGE);
    mi_mmio_write(VMMIO_STATUS, VSTAT_ACKNOWLEDGE | VSTAT_DRIVER);

    mi_mmio_write(VMMIO_DRV_FEAT_SEL, 0U);
    mi_mmio_write(VMMIO_DRV_FEATURES, 0U);
    mi_mmio_write(VMMIO_DRV_FEAT_SEL, 1U);
    mi_mmio_write(VMMIO_DRV_FEATURES, VIRTIO_F_VERSION_1);

    mi_mmio_write(VMMIO_STATUS,
                  VSTAT_ACKNOWLEDGE | VSTAT_DRIVER | VSTAT_FEATURES_OK);
    if (!(mi_mmio_read(VMMIO_STATUS) & VSTAT_FEATURES_OK)) {
        mi_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
        return 0;
    }

    mi_mmio_write(VMMIO_QUEUE_SEL, 0U);
    {
        uint32_t qmax = mi_mmio_read(VMMIO_QUEUE_NUM_MAX);
        if (qmax == 0U) {
            mi_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
            return 0;
        }
        mi_queue_size = (qmax < MIQ_SIZE) ? (uint16_t)qmax : (uint16_t)MIQ_SIZE;
    }

    mi_mmio_write(VMMIO_QUEUE_NUM, mi_queue_size);

    {
        uint64_t desc_pa  = (uint64_t)(uintptr_t)(vi_vq_mem + 0);
        uint64_t avail_pa = (uint64_t)(uintptr_t)(vi_vq_mem + 256);
        uint64_t used_pa  = (uint64_t)(uintptr_t)(vi_vq_mem + 512);

        mi_mmio_write(VMMIO_QUEUE_DESC_LO, (uint32_t)desc_pa);
        mi_mmio_write(VMMIO_QUEUE_DESC_HI, (uint32_t)(desc_pa >> 32));
        mi_mmio_write(VMMIO_QUEUE_DRV_LO,  (uint32_t)avail_pa);
        mi_mmio_write(VMMIO_QUEUE_DRV_HI,  (uint32_t)(avail_pa >> 32));
        mi_mmio_write(VMMIO_QUEUE_DEV_LO,  (uint32_t)used_pa);
        mi_mmio_write(VMMIO_QUEUE_DEV_HI,  (uint32_t)(used_pa >> 32));
    }

    for (uint16_t i = 0U; i < mi_queue_size; i++) {
        MI_DESC[i].addr  = (uint64_t)(uintptr_t)&vi_events[i];
        MI_DESC[i].len   = (uint32_t)sizeof(vi_events[i]);
        MI_DESC[i].flags = 2U;
        MI_DESC[i].next  = 0U;
        MI_AVAIL->ring[i] = i;
    }

    mi_next_avail = mi_queue_size;
    MI_AVAIL->idx = mi_queue_size;

    cache_flush_range((uintptr_t)vi_events,
                      sizeof(virtio_input_event_t) * mi_queue_size);
    cache_flush_range((uintptr_t)vi_vq_mem, sizeof(vi_vq_mem));

    mi_mmio_write(VMMIO_QUEUE_READY, 1U);
    mi_mmio_write(VMMIO_QUEUE_NOTIFY, 0U);
    mi_mmio_write(VMMIO_STATUS,
                  VSTAT_ACKNOWLEDGE | VSTAT_DRIVER |
                  VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);

    mi_last_used = 0U;
    return 1;
}

static int mi_init(void)
{
    mi_base = mi_find_pointer();
    if (!mi_base) return 0;

    if (!mi_transport_init()) {
        uart_puts("[MOUSE] ERR: init virtio-input fallita\n");
        mi_base = 0U;
        return 0;
    }

    mi_has_rel = mi_config_test_bit(VIRTIO_INPUT_CFG_EV_BITS, EV_REL, REL_X) &&
                 mi_config_test_bit(VIRTIO_INPUT_CFG_EV_BITS, EV_REL, REL_Y);
    mi_has_abs = mi_config_test_bit(VIRTIO_INPUT_CFG_EV_BITS, EV_ABS, ABS_X) &&
                 mi_config_test_bit(VIRTIO_INPUT_CFG_EV_BITS, EV_ABS, ABS_Y);

    if (mi_has_abs) {
        if (!mi_read_absinfo(ABS_X, &mi_abs_min_x, &mi_abs_max_x)) {
            mi_abs_min_x = 0;
            mi_abs_max_x = FB_WIDTH - 1;
        }
        if (!mi_read_absinfo(ABS_Y, &mi_abs_min_y, &mi_abs_max_y)) {
            mi_abs_min_y = 0;
            mi_abs_max_y = FB_HEIGHT - 1;
        }
    }

    {
        char name[40];
        mi_read_name(name, sizeof(name));

        mi_backend = MOUSE_BACKEND_VIRTIO;
        mi_drain_events();

        gic_register_irq(mi_irq, mi_irq_handler, NULL,
                         GIC_PRIO_DRIVER, GIC_FLAG_LEVEL);
        gic_enable_irq(mi_irq);

        uart_puts("[MOUSE] Backend: VirtIO Input");
        if (name[0] != '\0') {
            uart_puts(" (");
            uart_puts(name);
            uart_puts(")");
        }
        uart_puts(mi_has_abs ? " — absolute pointer IRQ attivo\n"
                             : " — relative pointer IRQ attivo\n");
    }
    return 1;
}

/* ── API pubblica ─────────────────────────────────────────────────── */

void mouse_init(void)
{
    mouse_head = 0U;
    mouse_tail = 0U;
    mi_backend = MOUSE_BACKEND_NONE;
    mi_base = 0U;
    mi_queue_size = 0U;
    mi_last_used = 0U;
    mi_next_avail = 0U;
    mi_has_rel = 0U;
    mi_has_abs = 0U;
    mi_buttons = 0U;
    mi_pending_dx = 0;
    mi_pending_dy = 0;
    mi_pending_wheel = 0;
    mi_pending_button_change = 0U;
    mi_abs_raw_x = 0;
    mi_abs_raw_y = 0;
    mi_have_abs_x = 0U;
    mi_have_abs_y = 0U;
    mi_abs_min_x = 0;
    mi_abs_max_x = FB_WIDTH - 1;
    mi_abs_min_y = 0;
    mi_abs_max_y = FB_HEIGHT - 1;
    mi_abs_px_x = FB_WIDTH / 2U;
    mi_abs_px_y = FB_HEIGHT / 2U;
    mi_debug_log_budget = 0U;

    if (mi_init())
        return;

    uart_puts("[MOUSE] Backend: nessun puntatore VirtIO rilevato\n");
}

int mouse_is_ready(void)
{
    return mi_backend == MOUSE_BACKEND_VIRTIO;
}

int mouse_get_event(mouse_event_t *out)
{
    if (!out || mi_backend != MOUSE_BACKEND_VIRTIO)
        return 0;

    if (mouse_buf_empty())
        mi_drain_events();

    return mouse_buf_pop(out);
}
