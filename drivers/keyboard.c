/*
 * EnlilOS Microkernel - Console Input Backend (M4-02 / M8-08g)
 *
 * Backend supportati, in ordine di preferenza:
 *   1. VirtIO Input keyboard su QEMU virt (virtio-mmio, queue eventq)
 *   2. UART PL011 RX su stdio QEMU come fallback universale
 *
 * M8-08g separa il path:
 *   device event -> keycode -> keysym -> Unicode UTF-8
 *
 * keyboard_getc() resta disponibile come path legacy byte-oriented, mentre
 * keyboard_get_event() espone anche keycode/modifiers/keysym/unicode.
 */

#include "keyboard.h"
#include "virtio_mmio.h"
#include "gic.h"
#include "mmu.h"
#include "uart.h"

/* ── Ring buffer legacy byte stream (IRQ producer, task consumer) ─── */

#define KBD_BYTE_BUF_SIZE    256U
#define KBD_EVENT_BUF_SIZE    64U

static uint8_t kbd_byte_buf[KBD_BYTE_BUF_SIZE];
static uint8_t kbd_byte_head;
static uint8_t kbd_byte_tail;

static keyboard_event_t kbd_event_buf[KBD_EVENT_BUF_SIZE];
static uint8_t          kbd_event_head;
static uint8_t          kbd_event_tail;

static inline int kbd_byte_empty(void)
{
    return kbd_byte_head == kbd_byte_tail;
}

static inline int kbd_byte_full(void)
{
    return (uint8_t)(kbd_byte_head + 1U) == kbd_byte_tail;
}

static void kbd_byte_push(uint8_t c)
{
    if (!kbd_byte_full()) {
        kbd_byte_buf[kbd_byte_head] = c;
        __asm__ volatile("dmb sy" ::: "memory");
        kbd_byte_head++;
    }
}

static int kbd_byte_pop(void)
{
    if (kbd_byte_empty())
        return -1;

    {
        uint8_t c = kbd_byte_buf[kbd_byte_tail];
        __asm__ volatile("dmb sy" ::: "memory");
        kbd_byte_tail++;
        return (int)c;
    }
}

static inline int kbd_event_empty(void)
{
    return kbd_event_head == kbd_event_tail;
}

static inline int kbd_event_full(void)
{
    return (uint8_t)(kbd_event_head + 1U) == kbd_event_tail;
}

static void kbd_event_push(const keyboard_event_t *ev)
{
    if (!ev || kbd_event_full())
        return;

    kbd_event_buf[kbd_event_head] = *ev;
    __asm__ volatile("dmb sy" ::: "memory");
    kbd_event_head++;
}

static int kbd_event_pop(keyboard_event_t *out)
{
    if (!out || kbd_event_empty())
        return 0;

    *out = kbd_event_buf[kbd_event_tail];
    __asm__ volatile("dmb sy" ::: "memory");
    kbd_event_tail++;
    return 1;
}

/* ── Layout tables: keycode Linux input -> keysym/unicode ─────────── */

typedef struct {
    uint32_t normal;
    uint32_t shift;
    uint32_t altgr;
    uint32_t shift_altgr;
} kbd_keymap_entry_t;

typedef enum {
    KSYM_NONE = 0U,
    KSYM_DEAD_ACUTE = 0x110000U,
    KSYM_DEAD_GRAVE = 0x110001U,
    KSYM_DEAD_CIRCUMFLEX = 0x110002U,
    KSYM_DEAD_TILDE = 0x110003U,
} keyboard_keysym_t;

static const char *const kbd_layout_names[] = {
    "us",
    "it",
};

static const kbd_keymap_entry_t keymap_us[128] = {
    [KEY_ESC]        = { 0x1BU, '!', 0U, 0U },
    [KEY_1]          = { '1', '!', 0U, 0U },
    [KEY_2]          = { '2', '@', 0U, 0U },
    [KEY_3]          = { '3', '#', 0U, 0U },
    [KEY_4]          = { '4', '$', 0U, 0U },
    [KEY_5]          = { '5', '%', 0U, 0U },
    [KEY_6]          = { '6', '^', 0U, 0U },
    [KEY_7]          = { '7', '&', 0U, 0U },
    [KEY_8]          = { '8', '*', 0U, 0U },
    [KEY_9]          = { '9', '(', 0U, 0U },
    [KEY_0]          = { '0', ')', 0U, 0U },
    [KEY_MINUS]      = { '-', '_', 0U, 0U },
    [KEY_EQUAL]      = { '=', '+', 0U, 0U },
    [KEY_BACKSPACE]  = { '\b', '\b', 0U, 0U },
    [KEY_TAB]        = { '\t', '\t', 0U, 0U },
    [KEY_Q]          = { 'q', 'Q', 0U, 0U },
    [KEY_W]          = { 'w', 'W', 0U, 0U },
    [KEY_E]          = { 'e', 'E', 0U, 0U },
    [KEY_R]          = { 'r', 'R', 0U, 0U },
    [KEY_T]          = { 't', 'T', 0U, 0U },
    [KEY_Y]          = { 'y', 'Y', 0U, 0U },
    [KEY_U]          = { 'u', 'U', 0U, 0U },
    [KEY_I]          = { 'i', 'I', 0U, 0U },
    [KEY_O]          = { 'o', 'O', 0U, 0U },
    [KEY_P]          = { 'p', 'P', 0U, 0U },
    [KEY_LEFTBRACE]  = { '[', '{', 0U, 0U },
    [KEY_RIGHTBRACE] = { ']', '}', 0U, 0U },
    [KEY_ENTER]      = { '\n', '\n', 0U, 0U },
    [KEY_A]          = { 'a', 'A', 0U, 0U },
    [KEY_S]          = { 's', 'S', 0U, 0U },
    [KEY_D]          = { 'd', 'D', 0U, 0U },
    [KEY_F]          = { 'f', 'F', 0U, 0U },
    [KEY_G]          = { 'g', 'G', 0U, 0U },
    [KEY_H]          = { 'h', 'H', 0U, 0U },
    [KEY_J]          = { 'j', 'J', 0U, 0U },
    [KEY_K]          = { 'k', 'K', 0U, 0U },
    [KEY_L]          = { 'l', 'L', 0U, 0U },
    [KEY_SEMICOLON]  = { ';', ':', 0U, 0U },
    [KEY_APOSTROPHE] = { '\'', '"', 0U, 0U },
    [KEY_GRAVE]      = { '`', '~', 0U, 0U },
    [KEY_BACKSLASH]  = { '\\', '|', 0U, 0U },
    [KEY_Z]          = { 'z', 'Z', 0U, 0U },
    [KEY_X]          = { 'x', 'X', 0U, 0U },
    [KEY_C]          = { 'c', 'C', 0U, 0U },
    [KEY_V]          = { 'v', 'V', 0U, 0U },
    [KEY_B]          = { 'b', 'B', 0U, 0U },
    [KEY_N]          = { 'n', 'N', 0U, 0U },
    [KEY_M]          = { 'm', 'M', 0U, 0U },
    [KEY_COMMA]      = { ',', '<', 0U, 0U },
    [KEY_DOT]        = { '.', '>', 0U, 0U },
    [KEY_SLASH]      = { '/', '?', 0U, 0U },
    [KEY_102ND]      = { '\\', '|', 0U, 0U },
    [KEY_SPACE]      = { ' ', ' ', 0U, 0U },
};

static const kbd_keymap_entry_t keymap_it[128] = {
    [KEY_ESC]        = { 0x1BU, 0x1BU, 0U, 0U },
    [KEY_1]          = { '1', '!', 0U, 0U },
    [KEY_2]          = { '2', '"', 0U, 0U },
    [KEY_3]          = { '3', 0x00A3U, 0U, 0U },
    [KEY_4]          = { '4', '$', 0U, 0U },
    [KEY_5]          = { '5', '%', 0x20ACU, 0U },
    [KEY_6]          = { '6', '&', 0U, 0U },
    [KEY_7]          = { '7', '/', 0U, 0U },
    [KEY_8]          = { '8', '(', 0U, 0U },
    [KEY_9]          = { '9', ')', 0U, 0U },
    [KEY_0]          = { '0', '=', 0U, 0U },
    [KEY_MINUS]      = { '\'', '?', KSYM_DEAD_ACUTE, 0U },
    [KEY_EQUAL]      = { 0x00ECU, '^', KSYM_DEAD_TILDE, 0U },
    [KEY_BACKSPACE]  = { '\b', '\b', 0U, 0U },
    [KEY_TAB]        = { '\t', '\t', 0U, 0U },
    [KEY_Q]          = { 'q', 'Q', 0U, 0U },
    [KEY_W]          = { 'w', 'W', 0U, 0U },
    [KEY_E]          = { 'e', 'E', 0x20ACU, 0U },
    [KEY_R]          = { 'r', 'R', 0U, 0U },
    [KEY_T]          = { 't', 'T', 0U, 0U },
    [KEY_Y]          = { 'y', 'Y', 0U, 0U },
    [KEY_U]          = { 'u', 'U', 0U, 0U },
    [KEY_I]          = { 'i', 'I', 0U, 0U },
    [KEY_O]          = { 'o', 'O', 0U, 0U },
    [KEY_P]          = { 'p', 'P', 0U, 0U },
    [KEY_LEFTBRACE]  = { 0x00E8U, 0x00E9U, '[', '{' },
    [KEY_RIGHTBRACE] = { '+', '*', ']', '}' },
    [KEY_ENTER]      = { '\n', '\n', 0U, 0U },
    [KEY_A]          = { 'a', 'A', 0U, 0U },
    [KEY_S]          = { 's', 'S', 0U, 0U },
    [KEY_D]          = { 'd', 'D', 0U, 0U },
    [KEY_F]          = { 'f', 'F', 0U, 0U },
    [KEY_G]          = { 'g', 'G', 0U, 0U },
    [KEY_H]          = { 'h', 'H', 0U, 0U },
    [KEY_J]          = { 'j', 'J', 0U, 0U },
    [KEY_K]          = { 'k', 'K', 0U, 0U },
    [KEY_L]          = { 'l', 'L', 0U, 0U },
    [KEY_SEMICOLON]  = { 0x00F2U, 0x00E7U, '@', 0U },
    [KEY_APOSTROPHE] = { 0x00E0U, 0x00B0U, '#', 0U },
    [KEY_GRAVE]      = { '\\', '|', KSYM_DEAD_GRAVE, KSYM_DEAD_CIRCUMFLEX },
    [KEY_BACKSLASH]  = { 0x00F9U, 0x00A7U, 0U, 0U },
    [KEY_Z]          = { 'z', 'Z', 0U, 0U },
    [KEY_X]          = { 'x', 'X', 0U, 0U },
    [KEY_C]          = { 'c', 'C', 0U, 0U },
    [KEY_V]          = { 'v', 'V', 0U, 0U },
    [KEY_B]          = { 'b', 'B', 0U, 0U },
    [KEY_N]          = { 'n', 'N', 0U, 0U },
    [KEY_M]          = { 'm', 'M', 0U, 0U },
    [KEY_COMMA]      = { ',', ';', 0U, 0U },
    [KEY_DOT]        = { '.', ':', 0U, 0U },
    [KEY_SLASH]      = { '-', '_', 0U, 0U },
    [KEY_102ND]      = { '<', '>', '|', 0U },
    [KEY_SPACE]      = { ' ', ' ', ' ', ' ' },
};

/* ── VirtIO Input (virtio-mmio) ───────────────────────────────────── */

#define VIQ_SIZE             16U

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIQ_SIZE];
    uint16_t used_event;
} __attribute__((packed)) vring_avail_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vring_used_elem_t ring[VIQ_SIZE];
    uint16_t avail_event;
} __attribute__((packed)) vring_used_t;

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
static keyboard_layout_t  kbd_layout = KBD_LAYOUT_US;
static uintptr_t          vi_base;
static uint32_t           vi_irq;
static uint16_t           vi_queue_size;
static uint16_t           vi_last_used;
static uint16_t           vi_next_avail;
static uint8_t            vi_ctrl;
static uint8_t            vi_shift;
static uint8_t            vi_alt;
static uint8_t            vi_altgr;
static uint32_t           vi_dead_keysym;

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

static uint16_t kbd_current_modifiers(void)
{
    uint16_t mods = 0U;

    if (vi_shift) mods |= KBD_MOD_SHIFT;
    if (vi_ctrl)  mods |= KBD_MOD_CTRL;
    if (vi_alt)   mods |= KBD_MOD_ALT;
    if (vi_altgr) mods |= KBD_MOD_ALTGR;
    return mods;
}

static int kbd_is_alpha(uint32_t cp)
{
    return (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z');
}

static uint32_t kbd_ctrl_transform(uint32_t cp)
{
    if (cp >= 'a' && cp <= 'z')
        return (cp - 'a') + 1U;
    if (cp >= 'A' && cp <= 'Z')
        return (cp - 'A') + 1U;
    return cp;
}

static const kbd_keymap_entry_t *kbd_layout_table(keyboard_layout_t layout)
{
    switch (layout) {
    case KBD_LAYOUT_IT:
        return keymap_it;
    case KBD_LAYOUT_US:
    default:
        return keymap_us;
    }
}

static uint32_t kbd_lookup_keysym(keyboard_layout_t layout, uint16_t keycode,
                                  uint16_t modifiers)
{
    const kbd_keymap_entry_t *table;
    const kbd_keymap_entry_t *entry;

    if (keycode >= 128U)
        return 0U;

    table = kbd_layout_table(layout);
    entry = &table[keycode];

    if (modifiers & KBD_MOD_ALTGR) {
        uint32_t sym = (modifiers & KBD_MOD_SHIFT) ? entry->shift_altgr
                                                   : entry->altgr;
        if (sym != 0U)
            return sym;
    }

    if (modifiers & KBD_MOD_SHIFT) {
        if (entry->shift != 0U)
            return entry->shift;
    }

    return entry->normal;
}

static int kbd_is_dead_keysym(uint32_t keysym)
{
    return keysym >= (uint32_t)KSYM_DEAD_ACUTE &&
           keysym <= (uint32_t)KSYM_DEAD_TILDE;
}

static uint32_t kbd_dead_fallback_char(uint32_t keysym)
{
    switch (keysym) {
    case KSYM_DEAD_ACUTE:      return '\'';
    case KSYM_DEAD_GRAVE:      return '`';
    case KSYM_DEAD_CIRCUMFLEX: return '^';
    case KSYM_DEAD_TILDE:      return '~';
    default:                   return 0U;
    }
}

static uint32_t kbd_compose_dead(uint32_t dead, uint32_t cp)
{
    switch (dead) {
    case KSYM_DEAD_ACUTE:
        switch (cp) {
        case 'a': return 0x00E1U;
        case 'e': return 0x00E9U;
        case 'i': return 0x00EDU;
        case 'o': return 0x00F3U;
        case 'u': return 0x00FAU;
        case 'A': return 0x00C1U;
        case 'E': return 0x00C9U;
        case 'I': return 0x00CDU;
        case 'O': return 0x00D3U;
        case 'U': return 0x00DAU;
        case ' ': return '\'';
        default:  return 0U;
        }
    case KSYM_DEAD_GRAVE:
        switch (cp) {
        case 'a': return 0x00E0U;
        case 'e': return 0x00E8U;
        case 'i': return 0x00ECU;
        case 'o': return 0x00F2U;
        case 'u': return 0x00F9U;
        case 'A': return 0x00C0U;
        case 'E': return 0x00C8U;
        case 'I': return 0x00CCU;
        case 'O': return 0x00D2U;
        case 'U': return 0x00D9U;
        case ' ': return '`';
        default:  return 0U;
        }
    case KSYM_DEAD_CIRCUMFLEX:
        switch (cp) {
        case 'a': return 0x00E2U;
        case 'e': return 0x00EAU;
        case 'i': return 0x00EEU;
        case 'o': return 0x00F4U;
        case 'u': return 0x00FBU;
        case 'A': return 0x00C2U;
        case 'E': return 0x00CAU;
        case 'I': return 0x00CEU;
        case 'O': return 0x00D4U;
        case 'U': return 0x00DBU;
        case ' ': return '^';
        default:  return 0U;
        }
    case KSYM_DEAD_TILDE:
        switch (cp) {
        case 'a': return 0x00E3U;
        case 'n': return 0x00F1U;
        case 'o': return 0x00F5U;
        case 'A': return 0x00C3U;
        case 'N': return 0x00D1U;
        case 'O': return 0x00D5U;
        case ' ': return '~';
        default:  return 0U;
        }
    default:
        return 0U;
    }
}

static uint32_t kbd_encode_utf8(uint32_t cp, uint8_t out[4])
{
    if (!out || cp == 0U)
        return 0U;

    if (cp <= 0x7FU) {
        out[0] = (uint8_t)cp;
        return 1U;
    }
    if (cp <= 0x7FFU) {
        out[0] = (uint8_t)(0xC0U | ((cp >> 6) & 0x1FU));
        out[1] = (uint8_t)(0x80U | (cp & 0x3FU));
        return 2U;
    }
    if (cp <= 0xFFFFU) {
        out[0] = (uint8_t)(0xE0U | ((cp >> 12) & 0x0FU));
        out[1] = (uint8_t)(0x80U | ((cp >> 6) & 0x3FU));
        out[2] = (uint8_t)(0x80U | (cp & 0x3FU));
        return 3U;
    }
    if (cp <= 0x10FFFFU) {
        out[0] = (uint8_t)(0xF0U | ((cp >> 18) & 0x07U));
        out[1] = (uint8_t)(0x80U | ((cp >> 12) & 0x3FU));
        out[2] = (uint8_t)(0x80U | ((cp >> 6) & 0x3FU));
        out[3] = (uint8_t)(0x80U | (cp & 0x3FU));
        return 4U;
    }
    return 0U;
}

static void kbd_push_utf8_codepoint(uint32_t cp)
{
    uint8_t bytes[4];
    uint32_t n = kbd_encode_utf8(cp, bytes);

    for (uint32_t i = 0U; i < n; i++)
        kbd_byte_push(bytes[i]);
}

static int kbd_ascii_tolower(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A' + 'a';
    return ch;
}

static int kbd_name_match(const char *a, const char *b)
{
    uint32_t i = 0U;

    if (!a || !b)
        return 0;

    while (a[i] != '\0' && b[i] != '\0') {
        if (kbd_ascii_tolower((unsigned char)a[i]) !=
            kbd_ascii_tolower((unsigned char)b[i]))
            return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int kbd_parse_layout_name(const char *name, keyboard_layout_t *out)
{
    char  token[16];
    const char *base;
    uint32_t len = 0U;

    if (!name || !out)
        return -1;

    base = name;
    for (const char *p = name; *p != '\0'; p++) {
        if (*p == '/')
            base = p + 1;
    }

    while (base[len] != '\0' && base[len] != '.' &&
           len + 1U < (uint32_t)sizeof(token)) {
        token[len] = (char)kbd_ascii_tolower((unsigned char)base[len]);
        len++;
    }
    token[len] = '\0';

    if (kbd_name_match(token, "us")) {
        *out = KBD_LAYOUT_US;
        return 0;
    }
    if (kbd_name_match(token, "it")) {
        *out = KBD_LAYOUT_IT;
        return 0;
    }

    return -1;
}

static int vi_config_test_bit(uint8_t select, uint8_t subsel, uint16_t bit)
{
    uint16_t byte;
    uint8_t  size;
    uint8_t  v;

    vi_cfg_write8(0, select);
    vi_cfg_write8(1, subsel);

    size = vi_cfg_read8(2);
    byte = (uint16_t)(bit / 8U);
    if (byte >= size)
        return 0;

    v = vi_cfg_read8(8U + byte);
    return (v >> (bit & 7U)) & 1U;
}

static void vi_read_name(char *buf, size_t buf_size)
{
    uint8_t size;

    if (buf_size == 0U)
        return;

    vi_cfg_write8(0, VIRTIO_INPUT_CFG_ID_NAME);
    vi_cfg_write8(1, 0U);

    size = vi_cfg_read8(2);
    if ((size_t)size >= buf_size)
        size = (uint8_t)(buf_size - 1U);

    for (uint8_t i = 0U; i < size; i++)
        buf[i] = (char)vi_cfg_read8(8U + i);

    buf[size] = '\0';
}

static int vi_is_keyboard(uintptr_t base)
{
    uintptr_t saved = vi_base;
    int looks_like_keyboard;

    vi_base = base;

    looks_like_keyboard =
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
        uint32_t version;

        if (p[VMMIO_MAGIC / 4] != VMMIO_MAGIC_VALUE)
            continue;
        if (p[VMMIO_DEVICE_ID / 4] != VIRTIO_DEVICE_INPUT)
            continue;

        version = p[VMMIO_VERSION / 4];
        if (version == 1U) {
            saw_legacy = 1;
            continue;
        }
        if (version != 2U)
            continue;
        if (!vi_is_keyboard(base))
            continue;

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

    cache_flush_range((uintptr_t)VI_AVAIL, sizeof(*VI_AVAIL));
    vi_mmio_write(VMMIO_QUEUE_NOTIFY, 0U);
}

static void vi_process_key(uint16_t code, uint32_t value)
{
    uint16_t         mods;
    uint32_t         keysym;
    uint32_t         unicode;
    keyboard_event_t ev;

    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
        vi_ctrl = (value != 0U) ? 1U : 0U;
        return;
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
        vi_shift = (value != 0U) ? 1U : 0U;
        return;
    case KEY_LEFTALT:
        vi_alt = (value != 0U) ? 1U : 0U;
        return;
    case KEY_RIGHTALT:
        vi_alt = (value != 0U) ? 1U : 0U;
        vi_altgr = (value != 0U) ? 1U : 0U;
        return;
    default:
        break;
    }

    if (value != 1U && value != 2U)
        return;

    mods = kbd_current_modifiers();
    keysym = kbd_lookup_keysym(kbd_layout, code, mods);
    if (keysym == 0U)
        return;

    ev.keycode   = code;
    ev.modifiers = mods;
    ev.keysym    = keysym;
    ev.unicode   = 0U;
    ev.pressed   = 1U;
    ev.repeat    = (value == 2U) ? 1U : 0U;
    ev.reserved0 = 0U;
    ev.reserved1 = 0U;

    if (kbd_is_dead_keysym(keysym)) {
        vi_dead_keysym = keysym;
        kbd_event_push(&ev);
        return;
    }

    unicode = keysym;
    if ((mods & KBD_MOD_CTRL) && kbd_is_alpha(keysym))
        unicode = kbd_ctrl_transform(keysym);
    else if (vi_dead_keysym != 0U) {
        uint32_t composed = kbd_compose_dead(vi_dead_keysym, unicode);

        if (composed != 0U) {
            unicode = composed;
        } else {
            uint32_t dead_ch = kbd_dead_fallback_char(vi_dead_keysym);
            if (dead_ch != 0U)
                kbd_push_utf8_codepoint(dead_ch);
        }
        vi_dead_keysym = 0U;
    }

    ev.unicode = unicode;
    if (unicode != 0U)
        kbd_push_utf8_codepoint(unicode);
    kbd_event_push(&ev);
}

static void vi_drain_events(void)
{
    if (!vi_base || vi_queue_size == 0U)
        return;

    {
        uint32_t isr = vi_mmio_read(VMMIO_IRQ_STATUS);
        if (isr)
            vi_mmio_write(VMMIO_IRQ_ACK, isr);
    }

    cache_invalidate_range((uintptr_t)VI_USED, sizeof(*VI_USED));

    while (vi_last_used != VI_USED->idx) {
        vring_used_elem_t elem = VI_USED->ring[vi_last_used % vi_queue_size];
        uint16_t desc_id = (uint16_t)elem.id;

        if (desc_id < vi_queue_size) {
            virtio_input_event_t ev;

            cache_invalidate_range((uintptr_t)&vi_events[desc_id],
                                   sizeof(vi_events[desc_id]));

            ev = vi_events[desc_id];
            if (ev.type == EV_KEY)
                vi_process_key(ev.code, ev.value);

            vi_requeue_desc(desc_id);
        }

        vi_last_used++;
        cache_invalidate_range((uintptr_t)VI_USED, sizeof(*VI_USED));
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
    {
        uint32_t qmax = vi_mmio_read(VMMIO_QUEUE_NUM_MAX);
        uint64_t desc_pa;
        uint64_t avail_pa;
        uint64_t used_pa;

        if (qmax == 0U) {
            vi_mmio_write(VMMIO_STATUS, VSTAT_FAILED);
            return 0;
        }

        vi_queue_size = (qmax < VIQ_SIZE) ? (uint16_t)qmax : (uint16_t)VIQ_SIZE;
        vi_mmio_write(VMMIO_QUEUE_NUM, vi_queue_size);

        desc_pa  = (uint64_t)(uintptr_t)(vi_vq_mem + 0);
        avail_pa = (uint64_t)(uintptr_t)(vi_vq_mem + 256);
        used_pa  = (uint64_t)(uintptr_t)(vi_vq_mem + 512);

        vi_mmio_write(VMMIO_QUEUE_DESC_LO, (uint32_t)desc_pa);
        vi_mmio_write(VMMIO_QUEUE_DESC_HI, (uint32_t)(desc_pa >> 32));
        vi_mmio_write(VMMIO_QUEUE_DRV_LO,  (uint32_t)avail_pa);
        vi_mmio_write(VMMIO_QUEUE_DRV_HI,  (uint32_t)(avail_pa >> 32));
        vi_mmio_write(VMMIO_QUEUE_DEV_LO,  (uint32_t)used_pa);
        vi_mmio_write(VMMIO_QUEUE_DEV_HI,  (uint32_t)(used_pa >> 32));
    }

    for (uint16_t i = 0U; i < vi_queue_size; i++) {
        VI_DESC[i].addr  = (uint64_t)(uintptr_t)&vi_events[i];
        VI_DESC[i].len   = (uint32_t)sizeof(vi_events[i]);
        VI_DESC[i].flags = VRING_DESC_F_WRITE;
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
    char name[40];

    vi_base = vi_find_keyboard();
    if (!vi_base)
        return 0;

    if (!vi_transport_init()) {
        uart_puts("[KBD] ERR: init virtio-input fallita\n");
        vi_base = 0U;
        return 0;
    }

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
    uart_puts(" — IRQ attivo, layout=");
    uart_puts(keyboard_get_layout_name());
    uart_puts("\n");
    return 1;
}

/* ── API pubblica ─────────────────────────────────────────────────── */

void keyboard_init(void)
{
    kbd_byte_head = 0U;
    kbd_byte_tail = 0U;
    kbd_event_head = 0U;
    kbd_event_tail = 0U;
    vi_ctrl = 0U;
    vi_shift = 0U;
    vi_alt = 0U;
    vi_altgr = 0U;
    vi_dead_keysym = 0U;
    kbd_layout = KBD_LAYOUT_US;

    if (vi_init())
        return;

    kbd_backend = KBD_BACKEND_UART;
    uart_puts("[KBD] Backend: UART fallback su PL011 stdio, layout=");
    uart_puts(keyboard_get_layout_name());
    uart_puts("\n");
}

int keyboard_getc(void)
{
    if (kbd_backend == KBD_BACKEND_VIRTIO) {
        if (kbd_byte_empty())
            vi_drain_events();

        {
            int c = kbd_byte_pop();
            if (c >= 0)
                return c;
        }
    }

    return uart_getc_nonblock();
}

int keyboard_get_event(keyboard_event_t *out)
{
    if (!out)
        return -1;

    if (kbd_backend == KBD_BACKEND_VIRTIO && kbd_event_empty())
        vi_drain_events();

    return kbd_event_pop(out);
}

int keyboard_set_layout(keyboard_layout_t layout)
{
    if (layout != KBD_LAYOUT_US && layout != KBD_LAYOUT_IT)
        return -1;

    kbd_layout = layout;
    vi_dead_keysym = 0U;
    return 0;
}

int keyboard_set_layout_name(const char *name)
{
    keyboard_layout_t layout;

    if (kbd_parse_layout_name(name, &layout) < 0)
        return -1;
    return keyboard_set_layout(layout);
}

keyboard_layout_t keyboard_get_layout(void)
{
    return kbd_layout;
}

const char *keyboard_get_layout_name(void)
{
    if ((uint32_t)kbd_layout >= (uint32_t)(sizeof(kbd_layout_names) / sizeof(kbd_layout_names[0])))
        return "us";
    return kbd_layout_names[(uint32_t)kbd_layout];
}

int keyboard_selftest_run(void)
{
    keyboard_layout_t saved_layout = keyboard_get_layout();
    uint8_t           utf8[4];

    if (keyboard_set_layout_name("/usr/share/kbd/keymaps/it.map") < 0)
        return -1;
    if (keyboard_get_layout() != KBD_LAYOUT_IT)
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_IT, KEY_LEFTBRACE, 0U) != 0x00E8U)
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_IT, KEY_LEFTBRACE, KBD_MOD_SHIFT) != 0x00E9U)
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_IT, KEY_LEFTBRACE, KBD_MOD_ALTGR) != '[')
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_IT, KEY_LEFTBRACE, KBD_MOD_SHIFT | KBD_MOD_ALTGR) != '{')
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_IT, KEY_APOSTROPHE, 0U) != 0x00E0U)
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_IT, KEY_EQUAL, 0U) != 0x00ECU)
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_IT, KEY_E, KBD_MOD_ALTGR) != 0x20ACU)
        return -1;
    if (kbd_compose_dead(KSYM_DEAD_ACUTE, 'e') != 0x00E9U)
        return -1;
    if (kbd_compose_dead(KSYM_DEAD_GRAVE, 'a') != 0x00E0U)
        return -1;
    if (kbd_encode_utf8(0x00E8U, utf8) != 2U || utf8[0] != 0xC3U || utf8[1] != 0xA8U)
        return -1;

    if (keyboard_set_layout(KBD_LAYOUT_US) < 0)
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_US, KEY_Q, 0U) != 'q')
        return -1;
    if (kbd_lookup_keysym(KBD_LAYOUT_US, KEY_Q, KBD_MOD_SHIFT) != 'Q')
        return -1;
    if (keyboard_set_layout_name("de") >= 0)
        return -1;

    if (keyboard_set_layout(saved_layout) < 0)
        return -1;
    return 0;
}
