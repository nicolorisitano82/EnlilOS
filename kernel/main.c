/*
 * EnlilOS - Realtime Operating System
 * Microkernel per AArch64 (ARM M-series compatible)
 *
 * Punto di ingresso principale del kernel.
 * Inizializza tutti i sottosistemi e mostra una boot console interattiva.
 */

#include "types.h"
#include "uart.h"
#include "framebuffer.h"
#include "microkernel.h"
#include "exception.h"
#include "mmu.h"
#include "pmm.h"
#include "kheap.h"
#include "gic.h"
#include "kdebug.h"
#include "timer.h"
#include "sched.h"
#include "syscall.h"
#include "keyboard.h"
#include "mouse.h"
#include "net.h"
#include "blk.h"
#include "ane.h"
#include "gpu.h"
#include "elf_loader.h"
#include "ext4.h"
#include "selftest.h"
#include "term80.h"
#include "vfs.h"
#include "cap.h"
#include "vmm.h"
#include "shutdown.h"

/* Banner ASCII art per la console seriale */
static void print_banner(void)
{
    uart_puts("\n");
    uart_puts(" EEEEE  N   N  L      III  L       OOO   SSSS \n");
    uart_puts(" E      NN  N  L       I   L      O   O S     \n");
    uart_puts(" EEEE   N N N  L       I   L      O   O  SSS  \n");
    uart_puts(" E      N  NN  L       I   L      O   O     S \n");
    uart_puts(" EEEEE  N   N  LLLLL  III  LLLLL   OOO   SSSS \n");
    uart_puts("\n");
    uart_puts(" Realtime Operating System\n");
    uart_puts(" Microkernel v0.1.0 - AArch64\n");
    uart_puts(" Architettura: Microkernel\n");
    uart_puts("==========================================\n\n");
}

/* Disegna un bordo decorativo attorno al testo centrato */
static void draw_border(uint32_t cx, uint32_t cy, uint32_t text_w,
                        uint32_t text_h, uint32_t padding, uint32_t color)
{
    uint32_t x1 = cx - padding;
    uint32_t y1 = cy - padding;
    uint32_t x2 = cx + text_w + padding;
    uint32_t y2 = cy + text_h + padding;

    /* Bordo orizzontale superiore e inferiore */
    for (uint32_t x = x1; x <= x2; x++) {
        for (uint32_t t = 0; t < 2; t++) {
            fb_put_pixel(x, y1 + t, color);
            fb_put_pixel(x, y2 - t, color);
        }
    }
    /* Bordo verticale sinistro e destro */
    for (uint32_t y = y1; y <= y2; y++) {
        for (uint32_t t = 0; t < 2; t++) {
            fb_put_pixel(x1 + t, y, color);
            fb_put_pixel(x2 - t, y, color);
        }
    }
}

#define BOOTCLI_HISTORY_MAX   18U
#define BOOTCLI_LINE_MAX      78U
#define BOOTCLI_INPUT_MAX     255U
#define BOOTCLI_PATH_MAX      128U
#define BOOTCLI_MOUSE_EVENT_MAX 4U

static int boot_persist_keyboard_layout(const char *name);
#define BOOTCLI_MOUSE_LINE_MAX  52U

static char      bootcli_lines[BOOTCLI_HISTORY_MAX][BOOTCLI_LINE_MAX + 1];
static uint32_t  bootcli_line_count;
static char      bootcli_input[BOOTCLI_INPUT_MAX + 1];
static uint32_t  bootcli_input_len;
static char      bootcli_cwd[BOOTCLI_PATH_MAX + 1];
static char      bootcli_mouse_lines[BOOTCLI_MOUSE_EVENT_MAX][BOOTCLI_MOUSE_LINE_MAX + 1];
static uint32_t  bootcli_mouse_line_count;
static int32_t   bootcli_mouse_x;
static int32_t   bootcli_mouse_y;
static uint32_t  bootcli_mouse_buttons;
static int32_t   bootcli_mouse_wheel_total;
static uint8_t   bootcli_mouse_ready;
static gpu_caps_t bootcli_caps;
static uint8_t   bootcli_graphics_mode;
static uint8_t   bootcli_mode;
static uint32_t  bootcli_shell_pid;
static char      bootcli_shell_name[24];
static volatile uint32_t bootcli_heartbeat;

#define BOOTCLI_MODE_UI    0U
#define BOOTCLI_MODE_TERM  1U

static const char boot_arkshrc_user_default[] =
    "# ~/.config/arksh/arkshrc\n"
    "# Personalizza qui la shell di login EnlilOS.\n";

static void boot_launch_vfsd(void)
{
    uint32_t pid = 0U;
    port_t  *port;
    int      rc;

    if (elf64_spawn_path("/VFSD.ELF", "/VFSD.ELF", PRIO_NORMAL, &pid) < 0) {
        uart_puts("[VFSD] Spawn fallita, resto su VFS bootstrap in-kernel\n");
        return;
    }

    port = mk_port_lookup("vfs");
    if (!port) {
        uart_puts("[VFSD] Porta 'vfs' non trovata\n");
        return;
    }

    rc = mk_port_rebind(port->port_id, pid);
    if (rc < 0) {
        uart_puts("[VFSD] Rebind porta fallita\n");
        return;
    }
    (void)mk_port_set_budget(port->port_id, timer_cntfrq() / 200ULL); /* 5ms */

    uart_puts("[VFSD] User-space server pronto: pid=");
    {
        char buf[12];
        int  len = 0;
        uint32_t v = pid;

        if (v == 0U) uart_putc('0');
        else {
            while (v) { buf[len++] = (char)('0' + (v % 10U)); v /= 10U; }
            while (len > 0) uart_putc(buf[--len]);
        }
    }
    uart_puts(" port=vfs\n");
}

static void boot_launch_blkd(void)
{
    uint32_t pid = 0U;
    port_t  *port;
    int      rc;

    if (elf64_spawn_path("/BLKD.ELF", "/BLKD.ELF", PRIO_LOW, &pid) < 0) {
        uart_puts("[BLKD] Spawn fallita, blocco rimane in-kernel\n");
        return;
    }

    port = mk_port_lookup("block");
    if (!port) {
        uart_puts("[BLKD] Porta 'block' non trovata\n");
        return;
    }

    rc = mk_port_rebind(port->port_id, pid);
    if (rc < 0) {
        uart_puts("[BLKD] Rebind porta fallita\n");
        return;
    }
    (void)mk_port_set_budget(port->port_id, timer_cntfrq() / 100ULL); /* 10ms */

    uart_puts("[BLKD] User-space block server pronto: pid=");
    {
        char buf[12];
        int  len = 0;
        uint32_t v = pid;

        if (v == 0U) uart_putc('0');
        else {
            while (v) { buf[len++] = (char)('0' + (v % 10U)); v /= 10U; }
            while (len > 0) uart_putc(buf[--len]);
        }
    }
    uart_puts(" port=block\n");
}

static void boot_launch_netd(void)
{
    uint32_t pid = 0U;
    port_t  *port;
    int      rc;

    if (!net_is_ready()) {
        uart_puts("[NETD] Driver rete assente, server non avviato\n");
        return;
    }

    if (elf64_spawn_path("/NETD.ELF", "/NETD.ELF", PRIO_HIGH, &pid) < 0) {
        uart_puts("[NETD] Spawn fallita, networking resta kernel-side\n");
        return;
    }

    port = mk_port_lookup("net");
    if (!port) {
        uart_puts("[NETD] Porta 'net' non trovata\n");
        return;
    }

    rc = mk_port_rebind(port->port_id, pid);
    if (rc < 0) {
        uart_puts("[NETD] Rebind porta fallita\n");
        return;
    }
    (void)mk_port_set_budget(port->port_id, timer_cntfrq() / 100ULL); /* 10ms */

    uart_puts("[NETD] User-space net server pronto: pid=");
    {
        char buf[12];
        int  len = 0;
        uint32_t v = pid;

        if (v == 0U) uart_putc('0');
        else {
            while (v) { buf[len++] = (char)('0' + (v % 10U)); v /= 10U; }
            while (len > 0) uart_putc(buf[--len]);
        }
    }
    uart_puts(" port=net\n");
}

static void boot_seed_dir_if_missing(const char *path, uint32_t mode)
{
    int rc;

    rc = vfs_mkdir(path, mode);
    if (rc == 0 || rc == -EEXIST)
        return;

    uart_puts("[BOOT] mkdir fallita: ");
    uart_puts(path);
    uart_puts("\n");
}

static void boot_seed_text_if_missing(const char *path, const char *text)
{
    vfs_file_t file;
    size_t     len = 0U;
    int        rc;

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc == 0) {
        (void)vfs_close(&file);
        return;
    }

    rc = vfs_open(path, O_CREAT | O_WRONLY | O_TRUNC, &file);
    if (rc < 0) {
        uart_puts("[BOOT] seed file fallita: ");
        uart_puts(path);
        uart_puts("\n");
        return;
    }

    while (text[len] != '\0')
        len++;
    if (len != 0U)
        (void)vfs_write(&file, text, len);
    (void)vfs_fsync(&file);
    (void)vfs_close(&file);
}

static int boot_path_is_regular_file(const char *path)
{
    vfs_file_t file;
    stat_t     st;
    int        rc;

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc < 0)
        return 0;

    rc = vfs_stat(&file, &st);
    (void)vfs_close(&file);
    if (rc < 0)
        return 0;

    return ((st.st_mode & S_IFMT) == S_IFREG);
}

static const char *boot_find_real_arksh_path(void)
{
    if (boot_path_is_regular_file("/bin/arksh.real"))
        return "/bin/arksh";
    return NULL;
}

static void boot_prepare_login_layout(void)
{
    if (!ext4_is_mounted())
        return;

    boot_seed_dir_if_missing("/data/etc", 0755U);
    boot_seed_dir_if_missing("/data/home", 0755U);
    boot_seed_dir_if_missing("/data/home/user", 0755U);
    boot_seed_dir_if_missing("/data/home/user/.config", 0755U);
    boot_seed_dir_if_missing("/data/home/user/.config/arksh", 0755U);
    boot_seed_dir_if_missing("/data/home/user/.local", 0755U);
    boot_seed_dir_if_missing("/data/home/user/.local/state", 0755U);
    boot_seed_dir_if_missing("/data/home/user/.local/state/arksh", 0755U);

    boot_seed_text_if_missing("/data/home/user/.config/arksh/arkshrc",
                              boot_arkshrc_user_default);
    boot_seed_text_if_missing("/data/home/user/.local/state/arksh/history", "");
    boot_seed_text_if_missing("/data/etc/vconsole.conf", "KEYMAP=it\n");
}

static int boot_read_text_file(const char *path, char *buf, size_t cap)
{
    vfs_file_t file;
    ssize_t    n;
    int        rc;

    if (!path || !buf || cap < 2U)
        return -1;

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc < 0)
        return -1;

    n = vfs_read(&file, buf, cap - 1U);
    (void)vfs_close(&file);
    if (n < 0)
        return -1;

    buf[(size_t)n] = '\0';
    return 0;
}

static int boot_parse_keymap_value(const char *text, char *out, size_t cap)
{
    const char *p = text;

    if (!text || !out || cap < 2U)
        return -1;

    while (*p != '\0') {
        const char *line = p;
        size_t      len = 0U;

        while (*line == ' ' || *line == '\t')
            line++;
        if (*line == '#') {
            while (*p != '\0' && *p != '\n')
                p++;
            if (*p == '\n')
                p++;
            continue;
        }

        if (line[0] == 'K' && line[1] == 'E' && line[2] == 'Y' &&
            line[3] == 'M' && line[4] == 'A' && line[5] == 'P' &&
            line[6] == '=') {
            line += 7;
            while (*line == ' ' || *line == '\t')
                line++;
            while (line[len] != '\0' && line[len] != '\n' &&
                   line[len] != '\r' && line[len] != ' ' &&
                   line[len] != '\t' && line[len] != '#' &&
                   len + 1U < cap) {
                out[len] = line[len];
                len++;
            }
            out[len] = '\0';
            return (len > 0U) ? 0 : -1;
        }

        while (*p != '\0' && *p != '\n')
            p++;
        if (*p == '\n')
            p++;
    }

    return -1;
}

static int boot_load_keyboard_layout_from_file(const char *path)
{
    char text[96];
    char layout[16];

    if (boot_read_text_file(path, text, sizeof(text)) < 0)
        return -1;
    if (boot_parse_keymap_value(text, layout, sizeof(layout)) < 0)
        return -1;
    if (keyboard_set_layout_name(layout) < 0)
        return -1;
    return 0;
}

static void boot_apply_keyboard_layout_config(void)
{
    char persisted[96];
    char layout[16];

    if (ext4_is_mounted() &&
        boot_read_text_file("/data/etc/vconsole.conf", persisted,
                            sizeof(persisted)) == 0 &&
        boot_parse_keymap_value(persisted, layout, sizeof(layout)) == 0 &&
        layout[0] == 'u' && layout[1] == 's' && layout[2] == '\0') {
        if (boot_persist_keyboard_layout("it") == 0)
            uart_puts("[KBD] Migrazione default layout: us -> it\n");
    }

    if (ext4_is_mounted() &&
        boot_load_keyboard_layout_from_file("/data/etc/vconsole.conf") == 0) {
        uart_puts("[KBD] Layout persistente caricato da /data/etc/vconsole.conf: ");
        uart_puts(keyboard_get_layout_name());
        uart_puts("\n");
        return;
    }

    if (boot_load_keyboard_layout_from_file("/etc/vconsole.conf") == 0) {
        uart_puts("[KBD] Layout bootstrap caricato da /etc/vconsole.conf: ");
        uart_puts(keyboard_get_layout_name());
        uart_puts("\n");
    }
}

static int boot_persist_keyboard_layout(const char *name)
{
    vfs_file_t file;
    char       line[32];
    size_t     len = 0U;
    int        rc;

    if (!ext4_is_mounted() || !name || name[0] == '\0')
        return -1;

    boot_seed_dir_if_missing("/data/etc", 0755U);

    rc = vfs_open("/data/etc/vconsole.conf", O_CREAT | O_WRONLY | O_TRUNC, &file);
    if (rc < 0)
        return -1;

    {
        static const char prefix[] = "KEYMAP=";
        while (prefix[len] != '\0' && len + 1U < sizeof(line)) {
            line[len] = prefix[len];
            len++;
        }
    }
    while (name[0] != '\0' && len + 2U < sizeof(line)) {
        line[len++] = *name++;
    }
    line[len++] = '\n';
    line[len] = '\0';

    (void)vfs_write(&file, line, len);
    (void)vfs_fsync(&file);
    (void)vfs_close(&file);
    return 0;
}

static uint32_t bootcli_strlen(const char *s)
{
    uint32_t len = 0U;
    while (s[len] != '\0')
        len++;
    return len;
}

static int bootcli_streq(const char *a, const char *b)
{
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int bootcli_startswith(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static void bootcli_copy_trunc(char *dst, const char *src, uint32_t max_chars)
{
    uint32_t i = 0U;

    if (max_chars == 0U) return;

    while (i + 1U < max_chars && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void bootcli_buf_append(char *dst, uint32_t cap, const char *src)
{
    uint32_t len = bootcli_strlen(dst);
    uint32_t i = 0U;

    if (cap == 0U || len >= cap - 1U) return;

    while (src[i] != '\0' && len + 1U < cap) {
        dst[len++] = src[i++];
    }
    dst[len] = '\0';
}

static void bootcli_buf_append_u32(char *dst, uint32_t cap, uint32_t v)
{
    char tmp[10];
    uint32_t len = 0U;

    if (v == 0U) {
        bootcli_buf_append(dst, cap, "0");
        return;
    }

    while (v != 0U && len < (uint32_t)sizeof(tmp)) {
        tmp[len++] = (char)('0' + (v % 10U));
        v /= 10U;
    }

    while (len > 0U) {
        char one[2];
        one[0] = tmp[--len];
        one[1] = '\0';
        bootcli_buf_append(dst, cap, one);
    }
}

static void bootcli_buf_append_hex8(char *dst, uint32_t cap, uint8_t v)
{
    static const char hex[] = "0123456789abcdef";
    char tmp[3];

    tmp[0] = hex[(v >> 4) & 0xFU];
    tmp[1] = hex[v & 0xFU];
    tmp[2] = '\0';
    bootcli_buf_append(dst, cap, tmp);
}

static void bootcli_buf_append_i32(char *dst, uint32_t cap, int32_t v)
{
    if (v < 0) {
        bootcli_buf_append(dst, cap, "-");
        bootcli_buf_append_u32(dst, cap, (uint32_t)(-(int64_t)v));
        return;
    }

    bootcli_buf_append_u32(dst, cap, (uint32_t)v);
}

static int bootcli_parse_u64(const char *s, uint64_t *out)
{
    uint64_t v = 0ULL;
    uint32_t i = 0U;

    if (!s || !*s || !out) return 0;
    while (s[i] != '\0') {
        if (s[i] < '0' || s[i] > '9')
            return 0;
        v = v * 10ULL + (uint64_t)(s[i] - '0');
        i++;
    }
    *out = v;
    return 1;
}

static int bootcli_split_two_paths(const char *src, const char *prefix,
                                   char *lhs, uint32_t lhs_cap,
                                   char *rhs, uint32_t rhs_cap)
{
    const char *p = src + bootcli_strlen(prefix);
    uint32_t lhs_len = 0U;
    uint32_t rhs_len = 0U;

    while (*p == ' ') p++;
    if (*p == '\0') return 0;
    while (*p != '\0' && *p != ' ' && lhs_len + 1U < lhs_cap)
        lhs[lhs_len++] = *p++;
    lhs[lhs_len] = '\0';
    if (lhs_len == 0U) return 0;

    while (*p == ' ') p++;
    if (*p == '\0') return 0;
    while (*p != '\0' && *p != ' ' && rhs_len + 1U < rhs_cap)
        rhs[rhs_len++] = *p++;
    rhs[rhs_len] = '\0';

    if (rhs_len == 0U) return 0;
    while (*p == ' ') p++;
    return (*p == '\0');
}

static void bootcli_path_pop(char *path)
{
    uint32_t len = bootcli_strlen(path);

    if (len <= 1U) {
        path[0] = '/';
        path[1] = '\0';
        return;
    }

    while (len > 1U && path[len - 1U] == '/') {
        path[--len] = '\0';
    }
    while (len > 1U && path[len - 1U] != '/') {
        path[--len] = '\0';
    }
    if (len > 1U)
        path[len - 1U] = '\0';
    if (path[0] == '\0') {
        path[0] = '/';
        path[1] = '\0';
    }
}

static int bootcli_resolve_path(const char *input, char *out, uint32_t cap)
{
    char     tmp[BOOTCLI_PATH_MAX + 1];
    uint32_t tmp_len = 0U;
    uint32_t out_len = 0U;
    uint32_t i = 0U;

    if (!input || !out || cap < 2U)
        return 0;

    if (input[0] == '/') {
        while (input[tmp_len] != '\0' && tmp_len + 1U < (uint32_t)sizeof(tmp)) {
            tmp[tmp_len] = input[tmp_len];
            tmp_len++;
        }
        if (input[tmp_len] != '\0')
            return 0;
    } else {
        const char *base = bootcli_cwd[0] ? bootcli_cwd : "/";

        while (base[tmp_len] != '\0' && tmp_len + 1U < (uint32_t)sizeof(tmp)) {
            tmp[tmp_len] = base[tmp_len];
            tmp_len++;
        }
        if (base[tmp_len] != '\0')
            return 0;
        if (tmp_len == 0U) {
            tmp[tmp_len++] = '/';
        } else if (tmp[tmp_len - 1U] != '/') {
            if (tmp_len + 1U >= (uint32_t)sizeof(tmp))
                return 0;
            tmp[tmp_len++] = '/';
        }
        for (uint32_t j = 0U; input[j] != '\0'; j++) {
            if (tmp_len + 1U >= (uint32_t)sizeof(tmp))
                return 0;
            tmp[tmp_len++] = input[j];
        }
    }
    tmp[tmp_len] = '\0';

    out[0] = '/';
    out[1] = '\0';
    out_len = 1U;

    while (tmp[i] != '\0') {
        uint32_t start;
        uint32_t seg_len = 0U;

        while (tmp[i] == '/')
            i++;
        if (tmp[i] == '\0')
            break;

        start = i;
        while (tmp[i] != '\0' && tmp[i] != '/') {
            seg_len++;
            i++;
        }

        if (seg_len == 1U && tmp[start] == '.')
            continue;
        if (seg_len == 2U && tmp[start] == '.' && tmp[start + 1U] == '.') {
            bootcli_path_pop(out);
            out_len = bootcli_strlen(out);
            continue;
        }

        if (out_len > 1U) {
            if (out_len + 1U >= cap)
                return 0;
            out[out_len++] = '/';
            out[out_len] = '\0';
        }
        if (out_len + seg_len >= cap)
            return 0;
        for (uint32_t j = 0U; j < seg_len; j++)
            out[out_len++] = tmp[start + j];
        out[out_len] = '\0';
    }

    if (out[0] == '\0') {
        out[0] = '/';
        out[1] = '\0';
    }
    return 1;
}

static int32_t bootcli_clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void bootcli_append_button_mask(char *dst, uint32_t cap, uint32_t buttons)
{
    bootcli_buf_append(dst, cap, (buttons & MOUSE_BTN_LEFT) ? "L" : "-");
    bootcli_buf_append(dst, cap, (buttons & MOUSE_BTN_RIGHT) ? "R" : "-");
    bootcli_buf_append(dst, cap, (buttons & MOUSE_BTN_MIDDLE) ? "M" : "-");
}

/* Formatta lo stato corrente del mouse in 'dst' (al massimo 'cap' byte).
 * Usato sia dalla status bar grafica che dal comando "mouse". */
static void bootcli_fmt_mouse_status(char *dst, uint32_t cap)
{
    dst[0] = '\0';
    bootcli_buf_append(dst, cap, "Mouse x=");
    bootcli_buf_append_u32(dst, cap, (uint32_t)bootcli_mouse_x);
    bootcli_buf_append(dst, cap, " y=");
    bootcli_buf_append_u32(dst, cap, (uint32_t)bootcli_mouse_y);
    bootcli_buf_append(dst, cap, " btn=");
    bootcli_append_button_mask(dst, cap, bootcli_mouse_buttons);
    bootcli_buf_append(dst, cap, " wheel=");
    bootcli_buf_append_i32(dst, cap, bootcli_mouse_wheel_total);
}

static void bootcli_fmt_net_status(char *dst, uint32_t cap)
{
    net_info_t info;

    dst[0] = '\0';
    if (net_get_info(&info) < 0) {
        bootcli_buf_append(dst, cap, "Rete guest non rilevata.");
        return;
    }

    bootcli_buf_append(dst, cap, "mac=");
    bootcli_buf_append_hex8(dst, cap, info.mac[0]);
    bootcli_buf_append(dst, cap, ":");
    bootcli_buf_append_hex8(dst, cap, info.mac[1]);
    bootcli_buf_append(dst, cap, ":");
    bootcli_buf_append_hex8(dst, cap, info.mac[2]);
    bootcli_buf_append(dst, cap, ":");
    bootcli_buf_append_hex8(dst, cap, info.mac[3]);
    bootcli_buf_append(dst, cap, ":");
    bootcli_buf_append_hex8(dst, cap, info.mac[4]);
    bootcli_buf_append(dst, cap, ":");
    bootcli_buf_append_hex8(dst, cap, info.mac[5]);
    bootcli_buf_append(dst, cap, " link=");
    bootcli_buf_append(dst, cap, info.link_up ? "up" : "down");
    bootcli_buf_append(dst, cap, " rx=");
    bootcli_buf_append_u32(dst, cap, info.rx_packets);
    bootcli_buf_append(dst, cap, " tx=");
    bootcli_buf_append_u32(dst, cap, info.tx_packets);
    if (info.rx_drops != 0U) {
        bootcli_buf_append(dst, cap, " drop=");
        bootcli_buf_append_u32(dst, cap, info.rx_drops);
    }
}

static void bootcli_fill_rect(uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, uint32_t color)
{
    if (bootcli_graphics_mode) {
        (void)gpu_fill_rect(x, y, w, h, color);
        return;
    }

    for (uint32_t yy = y; yy < y + h && yy < FB_HEIGHT; yy++) {
        for (uint32_t xx = x; xx < x + w && xx < FB_WIDTH; xx++)
            fb_put_pixel(xx, yy, color);
    }
}

static void bootcli_blit_rect(uint32_t dst_x, uint32_t dst_y,
                              uint32_t src_x, uint32_t src_y,
                              uint32_t w, uint32_t h)
{
    if (bootcli_graphics_mode) {
        (void)gpu_blit(dst_x, dst_y, src_x, src_y, w, h);
        return;
    }

    uint32_t *fb = fb_get_ptr();
    if (!fb || w == 0U || h == 0U)
        return;

    if (dst_x >= FB_WIDTH || dst_y >= FB_HEIGHT ||
        src_x >= FB_WIDTH || src_y >= FB_HEIGHT)
        return;
    if (src_x + w > FB_WIDTH) w = FB_WIDTH - src_x;
    if (dst_x + w > FB_WIDTH) w = FB_WIDTH - dst_x;
    if (src_y + h > FB_HEIGHT) h = FB_HEIGHT - src_y;
    if (dst_y + h > FB_HEIGHT) h = FB_HEIGHT - dst_y;

    for (uint32_t row = 0U; row < h; row++) {
        uint32_t *dst_row = fb + (dst_y + row) * FB_WIDTH + dst_x;
        uint32_t *src_row = fb + (src_y + row) * FB_WIDTH + src_x;
        for (uint32_t col = 0U; col < w; col++)
            dst_row[col] = src_row[col];
    }
}

static void bootcli_draw_text(uint32_t x, uint32_t y, const char *s,
                              uint32_t fg, uint32_t bg)
{
    if (bootcli_graphics_mode) {
        (void)gpu_draw_string(x, y, s, fg, bg, true);
        return;
    }

    fb_draw_string(x, y, s, fg, bg);
}

static void bootcli_push_line(const char *s)
{
    if (bootcli_line_count == BOOTCLI_HISTORY_MAX) {
        for (uint32_t i = 1U; i < BOOTCLI_HISTORY_MAX; i++)
            bootcli_copy_trunc(bootcli_lines[i - 1U], bootcli_lines[i],
                               BOOTCLI_LINE_MAX + 1U);
        bootcli_line_count--;
    }

    bootcli_copy_trunc(bootcli_lines[bootcli_line_count++], s,
                       BOOTCLI_LINE_MAX + 1U);

    uart_puts("[BOOTCLI] ");
    uart_puts(s);
    uart_puts("\n");
}

static void bootcli_mouse_push_event(const char *s)
{
    if (bootcli_mouse_line_count == BOOTCLI_MOUSE_EVENT_MAX) {
        for (uint32_t i = 1U; i < BOOTCLI_MOUSE_EVENT_MAX; i++) {
            bootcli_copy_trunc(bootcli_mouse_lines[i - 1U],
                               bootcli_mouse_lines[i],
                               BOOTCLI_MOUSE_LINE_MAX + 1U);
        }
        bootcli_mouse_line_count--;
    }

    bootcli_copy_trunc(bootcli_mouse_lines[bootcli_mouse_line_count++], s,
                       BOOTCLI_MOUSE_LINE_MAX + 1U);
}

static void bootcli_draw_mouse_cursor(void)
{
    static const uint16_t cursor_rows[] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xFC00, 0xD800, 0xCC00, 0x8E00,
        0x0600, 0x0300,
    };
    const uint32_t shadow = 0x00111a21;
    const uint32_t fg = (bootcli_mouse_buttons != 0U) ?
                        0x00ffd166 : 0x00f7fbff;
    static uint32_t shadow_sprite[16U * 16U];
    static uint32_t cursor_sprite[16U * 16U];

    if (!bootcli_graphics_mode || !bootcli_mouse_ready)
        return;

    for (uint32_t i = 0U; i < 16U * 16U; i++) {
        shadow_sprite[i] = 0U;
        cursor_sprite[i] = 0U;
    }

    for (uint32_t row = 0U; row < (uint32_t)(sizeof(cursor_rows) / sizeof(cursor_rows[0])); row++) {
        uint16_t bits = cursor_rows[row];
        for (uint32_t col = 0U; col < 16U; col++) {
            if ((bits & (uint16_t)(0x8000U >> col)) == 0U)
                continue;
            shadow_sprite[row * 16U + col] = 0x78000000U | shadow;
            cursor_sprite[row * 16U + col] = 0xF0000000U | fg;
        }
    }

    (void)gpu_alpha_blend((uint32_t)bootcli_mouse_x + 1U,
                          (uint32_t)bootcli_mouse_y + 1U,
                          shadow_sprite, 16U, 16U, 255U);
    (void)gpu_alpha_blend((uint32_t)bootcli_mouse_x,
                          (uint32_t)bootcli_mouse_y,
                          cursor_sprite, 16U, 16U, 255U);
}

static void bootcli_draw_mouse_cursor_fb(void)
{
    static const uint16_t cursor_rows[] = {
        0x8000, 0xC000, 0xE000, 0xF000,
        0xF800, 0xFC00, 0xFE00, 0xFF00,
        0xFC00, 0xD800, 0xCC00, 0x8E00,
        0x0600, 0x0300,
    };
    const uint32_t shadow = 0x00111a21;
    const uint32_t fg = (bootcli_mouse_buttons != 0U) ?
                        0x00ffd166 : 0x00f7fbff;

    if (bootcli_graphics_mode || !bootcli_mouse_ready)
        return;

    for (uint32_t row = 0U; row < (uint32_t)(sizeof(cursor_rows) / sizeof(cursor_rows[0])); row++) {
        uint16_t bits = cursor_rows[row];
        for (uint32_t col = 0U; col < 16U; col++) {
            if ((bits & (uint16_t)(0x8000U >> col)) == 0U)
                continue;
            fb_put_pixel((uint32_t)bootcli_mouse_x + col + 1U,
                         (uint32_t)bootcli_mouse_y + row + 1U,
                         shadow);
        }
    }

    for (uint32_t row = 0U; row < (uint32_t)(sizeof(cursor_rows) / sizeof(cursor_rows[0])); row++) {
        uint16_t bits = cursor_rows[row];
        for (uint32_t col = 0U; col < 16U; col++) {
            if ((bits & (uint16_t)(0x8000U >> col)) == 0U)
                continue;
            fb_put_pixel((uint32_t)bootcli_mouse_x + col,
                         (uint32_t)bootcli_mouse_y + row,
                         fg);
        }
    }
}

static void bootcli_push_current_input(void)
{
    char line[BOOTCLI_LINE_MAX + 1];

    line[0] = '\0';
    bootcli_buf_append(line, sizeof(line), "[");
    bootcli_buf_append(line, sizeof(line), bootcli_cwd);
    bootcli_buf_append(line, sizeof(line), "] > ");
    bootcli_buf_append(line, sizeof(line), bootcli_input);
    bootcli_push_line(line);
}

static void bootcli_fmt_scanout_status(char *dst, uint32_t cap)
{
    gpu_scanout_info_t info;

    dst[0] = '\0';
    gpu_get_scanout_info(&info);
    bootcli_buf_append(dst, cap, "Scanout ");
    bootcli_buf_append_u32(dst, cap, info.width);
    bootcli_buf_append(dst, cap, "x");
    bootcli_buf_append_u32(dst, cap, info.height);
    bootcli_buf_append(dst, cap, " hz=");
    bootcli_buf_append_u32(dst, cap, info.refresh_hz);
    bootcli_buf_append(dst, cap, " front=");
    bootcli_buf_append_u32(dst, cap, info.front_index);
    bootcli_buf_append(dst, cap, " back=");
    bootcli_buf_append_u32(dst, cap, info.back_index);
    bootcli_buf_append(dst, cap, " frames=");
    bootcli_buf_append_u32(dst, cap, (uint32_t)info.frame_counter);
}

static const char *bootcli_errno_name(int rc)
{
    int err = (rc < 0) ? -rc : rc;

    switch (err) {
    case ENOENT: return "ENOENT";
    case EIO:    return "EIO";
    case EINVAL: return "EINVAL";
    case EROFS:  return "EROFS";
    case ENOTDIR:return "ENOTDIR";
    case EISDIR: return "EISDIR";
    case EBADF:  return "EBADF";
    case EEXIST: return "EEXIST";
    case EXDEV:  return "EXDEV";
    case ENOTEMPTY: return "ENOTEMPTY";
    case ENAMETOOLONG: return "ENAMETOOLONG";
    default:     return "ERR";
    }
}

static void bootcli_push_error(const char *prefix, int rc)
{
    char line[BOOTCLI_LINE_MAX + 1];

    line[0] = '\0';
    bootcli_buf_append(line, sizeof(line), prefix);
    bootcli_buf_append(line, sizeof(line), ": ");
    bootcli_buf_append(line, sizeof(line), bootcli_errno_name(rc));
    bootcli_push_line(line);
}

static void bootcli_cmd_fs(void)
{
    char line[BOOTCLI_LINE_MAX + 1];

    line[0] = '\0';
    bootcli_buf_append(line, sizeof(line), "FS: ");
    bootcli_buf_append(line, sizeof(line), ext4_status());
    bootcli_buf_append(line, sizeof(line), ext4_has_dirty() ? " | dirty=yes" : " | dirty=no");
    bootcli_push_line(line);
}

static void bootcli_cmd_pwd(void)
{
    bootcli_push_line(bootcli_cwd);
}

static void bootcli_cmd_cd(const char *path)
{
    vfs_file_t file;
    stat_t     st;
    char       resolved[BOOTCLI_PATH_MAX + 1];
    char       line[BOOTCLI_LINE_MAX + 1];
    int        rc;

    if (!path || path[0] == '\0')
        path = "/";

    if (!bootcli_resolve_path(path, resolved, sizeof(resolved))) {
        bootcli_push_line("cd: path troppo lungo o non valido.");
        return;
    }

    rc = vfs_open(resolved, O_RDONLY, &file);
    if (rc < 0) {
        bootcli_push_error("cd open", rc);
        return;
    }

    rc = vfs_stat(&file, &st);
    (void)vfs_close(&file);
    if (rc < 0) {
        bootcli_push_error("cd stat", rc);
        return;
    }
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        bootcli_push_line("cd: il path non e' una directory.");
        return;
    }

    bootcli_copy_trunc(bootcli_cwd, resolved, sizeof(bootcli_cwd));
    line[0] = '\0';
    bootcli_buf_append(line, sizeof(line), "cwd -> ");
    bootcli_buf_append(line, sizeof(line), bootcli_cwd);
    bootcli_push_line(line);
}

static void bootcli_cmd_ls(const char *path)
{
    vfs_file_t   file;
    vfs_dirent_t ent;
    stat_t       st;
    char         line[BOOTCLI_LINE_MAX + 1];
    uint32_t     shown = 0U;
    int          rc;

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc < 0) {
        bootcli_push_error("ls open", rc);
        return;
    }

    rc = vfs_stat(&file, &st);
    if (rc < 0) {
        (void)vfs_close(&file);
        bootcli_push_error("ls stat", rc);
        return;
    }
    if ((st.st_mode & S_IFMT) != S_IFDIR) {
        (void)vfs_close(&file);
        bootcli_push_line("ls: il path non e' una directory.");
        return;
    }

    line[0] = '\0';
    bootcli_buf_append(line, sizeof(line), "ls ");
    bootcli_buf_append(line, sizeof(line), path);
    bootcli_push_line(line);

    while ((rc = vfs_readdir(&file, &ent)) == 0) {
        line[0] = '\0';
        bootcli_buf_append(line, sizeof(line), "  ");
        bootcli_buf_append(line, sizeof(line), ent.name);
        if ((ent.mode & S_IFMT) == S_IFDIR)
            bootcli_buf_append(line, sizeof(line), "/");
        bootcli_push_line(line);
        shown++;
        if (shown >= 10U) {
            bootcli_push_line("  ... output troncato ...");
            break;
        }
    }

    (void)vfs_close(&file);

    if (rc < 0 && rc != -ENOENT) {
        bootcli_push_error("ls readdir", rc);
        return;
    }
    if (shown == 0U)
        bootcli_push_line("  <directory vuota>");
}

static void bootcli_cmd_cat(const char *path)
{
    vfs_file_t file;
    stat_t     st;
    char       raw[241];
    char       line[BOOTCLI_LINE_MAX + 1];
    uint32_t   line_len = 0U;
    int        rc;

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc < 0) {
        bootcli_push_error("cat open", rc);
        return;
    }

    rc = vfs_stat(&file, &st);
    if (rc < 0) {
        (void)vfs_close(&file);
        bootcli_push_error("cat stat", rc);
        return;
    }
    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        (void)vfs_close(&file);
        bootcli_push_line("cat: il path e' una directory.");
        return;
    }

    rc = (int)vfs_read(&file, raw, 240U);
    (void)vfs_close(&file);
    if (rc < 0) {
        bootcli_push_error("cat read", rc);
        return;
    }

    if (rc == 0) {
        bootcli_push_line("<file vuoto>");
        return;
    }

    raw[rc] = '\0';
    line[0] = '\0';
    for (int i = 0; i < rc; i++) {
        char ch = raw[i];

        if (ch == '\r')
            continue;
        if (ch == '\n' || line_len >= BOOTCLI_LINE_MAX - 1U) {
            line[line_len] = '\0';
            bootcli_push_line(line);
            line_len = 0U;
            line[0] = '\0';
            if (ch == '\n')
                continue;
        }

        if ((uint8_t)ch < 32U || (uint8_t)ch >= 127U)
            ch = '.';
        line[line_len++] = ch;
    }

    if (line_len > 0U) {
        line[line_len] = '\0';
        bootcli_push_line(line);
    }
    if (rc == 240)
        bootcli_push_line("...[cat troncato a 240 byte]...");
}

static int bootcli_split_path_text(const char *src, const char *prefix,
                                   char *path, uint32_t path_cap,
                                   const char **text_out)
{
    const char *p = src + bootcli_strlen(prefix);
    uint32_t path_len = 0U;

    while (*p == ' ') p++;
    if (*p == '\0') return 0;

    while (*p != '\0' && *p != ' ' && path_len + 1U < path_cap)
        path[path_len++] = *p++;
    path[path_len] = '\0';
    if (path_len == 0U) return 0;

    while (*p == ' ') p++;
    if (*p == '\0') return 0;

    *text_out = p;
    return 1;
}

static int bootcli_split_path_value(const char *src, const char *prefix,
                                    char *path, uint32_t path_cap,
                                    uint64_t *value_out)
{
    const char *p = src + bootcli_strlen(prefix);
    uint32_t path_len = 0U;

    while (*p == ' ') p++;
    if (*p == '\0') return 0;

    while (*p != '\0' && *p != ' ' && path_len + 1U < path_cap)
        path[path_len++] = *p++;
    path[path_len] = '\0';
    if (path_len == 0U) return 0;

    while (*p == ' ') p++;
    if (*p == '\0') return 0;

    return bootcli_parse_u64(p, value_out);
}

static void bootcli_cmd_write_common(const char *path, const char *text, uint32_t flags)
{
    vfs_file_t file;
    char       line[BOOTCLI_LINE_MAX + 1];
    ssize_t    rc;
    int        open_rc;

    open_rc = vfs_open(path, flags, &file);
    if (open_rc < 0) {
        bootcli_push_error("write open", open_rc);
        return;
    }

    rc = vfs_write(&file, text, bootcli_strlen(text));
    if (rc < 0) {
        (void)vfs_close(&file);
        bootcli_push_error("write", (int)rc);
        return;
    }

    open_rc = vfs_close(&file);
    if (open_rc < 0) {
        bootcli_push_error("write close", open_rc);
        return;
    }

    line[0] = '\0';
    bootcli_buf_append(line, sizeof(line), "write OK: ");
    bootcli_buf_append(line, sizeof(line), path);
    bootcli_buf_append(line, sizeof(line), " bytes=");
    bootcli_buf_append_u32(line, sizeof(line), (uint32_t)rc);
    bootcli_push_line(line);
}

static void bootcli_cmd_fsync(const char *path)
{
    vfs_file_t file;
    int        rc;

    rc = vfs_open(path, O_RDONLY, &file);
    if (rc < 0) {
        bootcli_push_error("fsync open", rc);
        return;
    }
    rc = vfs_fsync(&file);
    (void)vfs_close(&file);
    if (rc < 0) {
        bootcli_push_error("fsync", rc);
        return;
    }
    bootcli_push_line("fsync OK.");
}

static void bootcli_render_term80(void)
{
    const uint32_t bg_color     = 0x00070b10;
    const uint32_t panel_color  = 0x00101822;
    const uint32_t border_color = 0x0041e2c2;
    const uint32_t title_color  = 0x00eef5ff;
    const uint32_t muted_color  = 0x0095a8bd;
    const uint32_t term_x       = 80U;
    const uint32_t term_y       = 100U;
    const uint32_t term_w       = 640U;
    const uint32_t term_h       = 400U;
    char rows[TERM80_ROWS][TERM80_COLS + 1U];
    char status[96];
    uint32_t cursor_row = 0U;
    uint32_t cursor_col = 0U;
    uint64_t cursor_phase = timer_now_ms() / 400ULL;

    if (bootcli_graphics_mode)
        (void)gpu_begin_2d_frame(bg_color);
    else
        fb_clear(bg_color);

    bootcli_fill_rect(term_x - 16U, term_y - 40U, term_w + 32U, term_h + 80U, panel_color);
    if (bootcli_graphics_mode) {
        bootcli_fill_rect(term_x - 16U, term_y - 40U, term_w + 32U, 2U, border_color);
        bootcli_fill_rect(term_x - 16U, term_y - 40U, 2U, term_h + 80U, border_color);
        bootcli_fill_rect(term_x - 16U, term_y + term_h + 38U, term_w + 32U, 2U, border_color);
        bootcli_fill_rect(term_x + term_w + 14U, term_y - 40U, 2U, term_h + 80U, border_color);
    } else {
        draw_border(term_x - 16U, term_y - 40U, term_w + 31U, term_h + 79U, 0U, border_color);
    }

    {
        char title[48];
        title[0] = '\0';
        bootcli_buf_append(title, sizeof(title), "ENLILOS ");
        bootcli_buf_append(title, sizeof(title), term80_title());
        bootcli_buf_append(title, sizeof(title), " 80x25");
        bootcli_draw_text(term_x, term_y - 28U, title, title_color, panel_color);
    }

    status[0] = '\0';
    bootcli_buf_append(status, sizeof(status), "pid ");
    bootcli_buf_append_u32(status, sizeof(status), bootcli_shell_pid);
    bootcli_buf_append(status, sizeof(status), " | titolo ");
    bootcli_buf_append(status, sizeof(status), term80_title());
    bootcli_buf_append(status, sizeof(status), " | output seriale + framebuffer");
    bootcli_draw_text(term_x, term_y - 10U, status, muted_color, panel_color);

    for (uint32_t row = 0U; row < TERM80_ROWS; row++)
        term80_copy_row(row, rows[row], sizeof(rows[row]));

    for (uint32_t row = 0U; row < TERM80_ROWS; row++)
        bootcli_draw_text(term_x, term_y + row * 16U, rows[row], title_color, bg_color);

    term80_get_cursor(&cursor_row, &cursor_col);
    if ((cursor_phase & 1ULL) == 0ULL &&
        cursor_row < TERM80_ROWS && cursor_col < TERM80_COLS) {
        uint32_t cursor_x = term_x + cursor_col * 8U;
        uint32_t cursor_y = term_y + cursor_row * 16U + 14U;

        bootcli_fill_rect(cursor_x, cursor_y, 8U, 2U, 0x00ffd166);
    }

    bootcli_draw_text(term_x, term_y + term_h + 12U,
                      "Comandi shell: arksh login nsh ls cat echo exec clear top cd pwd help exit",
                      muted_color, panel_color);

    gpu_present_fullscreen();
}

static void bootcli_launch_shell(const char *path, const char *title,
                                 const char *label, int announce)
{
    uint32_t pid = 0U;
    char     line[BOOTCLI_LINE_MAX + 1];

    if (bootcli_mode == BOOTCLI_MODE_TERM) {
        if (announce)
            bootcli_push_line("shell gia' attiva.");
        return;
    }

    if (elf64_spawn_path(path, path, PRIO_HIGH, &pid) < 0) {
        if (announce)
            bootcli_push_line(elf64_last_error());
        return;
    }

    term80_activate(pid, title);
    bootcli_shell_pid = pid;
    bootcli_mode = BOOTCLI_MODE_TERM;
    bootcli_copy_trunc(bootcli_shell_name, label, sizeof(bootcli_shell_name));

    if (announce) {
        line[0] = '\0';
        bootcli_buf_append(line, sizeof(line), label);
        bootcli_buf_append(line, sizeof(line), " avviata a EL0, pid=");
        bootcli_buf_append_u32(line, sizeof(line), pid);
        bootcli_push_line(line);
    }
}

static void bootcli_launch_nsh(int announce)
{
    bootcli_launch_shell("/NSH.ELF", "nsh", "nsh", announce);
}

static void bootcli_launch_default_shell(int announce)
{
    if (bootcli_mode == BOOTCLI_MODE_TERM) {
        if (announce)
            bootcli_push_line("shell gia' attiva.");
        return;
    }

    if (elf64_spawn_path("/bin/arksh", "/bin/arksh", PRIO_HIGH, &bootcli_shell_pid) >= 0) {
        term80_activate(bootcli_shell_pid, "arksh");
        bootcli_mode = BOOTCLI_MODE_TERM;
        bootcli_copy_trunc(bootcli_shell_name, "arksh", sizeof(bootcli_shell_name));
        if (announce) {
            char line[BOOTCLI_LINE_MAX + 1];
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "arksh login avviata a EL0, pid=");
            bootcli_buf_append_u32(line, sizeof(line), bootcli_shell_pid);
            bootcli_push_line(line);
        }
    } else {
        if (announce)
            bootcli_push_line("arksh non disponibile, fallback a nsh.");
        bootcli_launch_nsh(announce);
    }
}

static void bootcli_launch_real_arksh(int announce)
{
    const char *path = boot_find_real_arksh_path();

    if (!path) {
        if (announce)
            bootcli_push_line("arksh reale non presente in /bin; usa 'login' per il bridge o 'nsh' per recovery.");
        return;
    }

    bootcli_launch_shell(path, "arksh", "arksh", announce);
}

static int bootcli_poll_shell(void)
{
    sched_tcb_t *shell;
    char         shell_name[sizeof(bootcli_shell_name)];
    int32_t      exit_code = 0;
    int          have_exit_code = 0;

    if (bootcli_mode != BOOTCLI_MODE_TERM || bootcli_shell_pid == 0U)
        return 0;

    shell = sched_task_find(bootcli_shell_pid);
    if (shell && shell->state != TCB_STATE_ZOMBIE)
        return term80_take_dirty();
    if (shell && sched_task_get_exit_code(shell, &exit_code) == 0)
        have_exit_code = 1;

    bootcli_copy_trunc(shell_name, bootcli_shell_name, sizeof(shell_name));
    term80_deactivate();
    bootcli_mode = BOOTCLI_MODE_UI;
    bootcli_shell_pid = 0U;
    bootcli_shell_name[0] = '\0';
    if (shell_name[0] != '\0') {
        char line[BOOTCLI_LINE_MAX + 1];
        line[0] = '\0';
        bootcli_buf_append(line, sizeof(line), shell_name);
        bootcli_buf_append(line, sizeof(line), " terminata");
        if (have_exit_code) {
            bootcli_buf_append(line, sizeof(line), " (exit=");
            bootcli_buf_append_i32(line, sizeof(line), exit_code);
            bootcli_buf_append(line, sizeof(line), ")");
        }
        bootcli_buf_append(line, sizeof(line), ". Ritorno alla boot console.");
        bootcli_push_line(line);
    } else {
        bootcli_push_line("shell terminata. Ritorno alla boot console.");
    }
    return 1;
}

static void bootcli_render(void)
{
    if (bootcli_mode == BOOTCLI_MODE_TERM) {
        bootcli_render_term80();
        return;
    }

    const uint32_t bg_color     = 0x000c1118;
    const uint32_t panel_color  = 0x00141d29;
    const uint32_t header_color = 0x001d2937;
    const uint32_t border_color = 0x0000d6b8;
    const uint32_t title_color  = 0x00eef5ff;
    const uint32_t muted_color  = 0x0095a8bd;
    const uint32_t accent_color = 0x007ce7c8;
    const uint32_t prompt_color = 0x00ffd166;
    const uint32_t warning_color = 0x00ff8a5b;
    const uint32_t panel_x      = 32U;
    const uint32_t panel_y      = 24U;
    const uint32_t panel_w      = 736U;
    const uint32_t panel_h      = 552U;
    const uint32_t history_x    = 48U;
    const uint32_t history_y    = bootcli_graphics_mode ? 158U : 136U;
    const uint32_t line_step    = 18U;
    const uint32_t prompt_y     = bootcli_graphics_mode ? 506U : 488U;
    const uint32_t footer_y     = 544U;
    const uint32_t visible      = bootcli_graphics_mode ? 12U : 18U;
    uint32_t first;
    uint32_t row = 0U;
    char prompt_line[BOOTCLI_LINE_MAX + 1];
    char footer[BOOTCLI_LINE_MAX + 1];
    char mouse_status[BOOTCLI_LINE_MAX + 1];
    char scanout_status[BOOTCLI_LINE_MAX + 1];
    uint64_t cursor_phase = timer_now_ms() / 400ULL;

    if (bootcli_graphics_mode)
        (void)gpu_begin_2d_frame(bg_color);
    else
        fb_clear(bg_color);

    bootcli_fill_rect(panel_x, panel_y, panel_w, panel_h, panel_color);
    bootcli_fill_rect(panel_x, panel_y, panel_w, 40U, header_color);
    if (bootcli_graphics_mode) {
        bootcli_fill_rect(panel_x, panel_y, panel_w, 2U, border_color);
        bootcli_fill_rect(panel_x, panel_y, 2U, panel_h, border_color);
        bootcli_blit_rect(panel_x, panel_y + panel_h - 2U,
                          panel_x, panel_y, panel_w, 2U);
        bootcli_blit_rect(panel_x + panel_w - 2U, panel_y,
                          panel_x, panel_y, 2U, panel_h);
    } else {
        draw_border(panel_x, panel_y, panel_w - 1U, panel_h - 1U, 0U, border_color);
    }

    bootcli_draw_text(48U, 36U,
                      bootcli_graphics_mode ?
                          "ENLILOS GRAPHICS CONSOLE" :
                          "ENLILOS BOOT CONSOLE",
                      title_color, header_color);

    bootcli_draw_text(48U, 72U,
                      bootcli_graphics_mode ?
                          "Modo: GRAFICA (VirtIO-GPU)" :
                          "Modo: FRAMEBUFFER DI BOOT",
                      accent_color, panel_color);
    bootcli_draw_text(48U, 92U,
                      "Comandi: help clear pwd cd gpu selftest [nome] fs ls cat write",
                      muted_color, panel_color);
    bootcli_draw_text(48U, 112U,
                      "append mkdir truncate rm mv fsync sync kbdlayout loadkeys arksh login nsh",
                      muted_color, panel_color);
    bootcli_draw_text(48U, 132U,
                      "mreactdemo jobdemo nsdemo posixdemo muslabi muslglob musldl arkshsmoke",
                      muted_color, panel_color);
    bootcli_draw_text(48U, 152U,
                      "socketdemo clonedemo threadlife futexdemo pthreaddemo semdemo tlsmtdemo",
                      muted_color, panel_color);
    bootcli_draw_text(48U, 172U,
                      "crtdemo epolldemo | net mostra MAC/link/counter virtio-net e stato del bootstrap",
                      muted_color, panel_color);

    if (bootcli_graphics_mode) {
        bootcli_fmt_scanout_status(scanout_status, sizeof(scanout_status));
        bootcli_draw_text(48U, 132U, scanout_status, accent_color, panel_color);

        if (bootcli_mouse_ready) {
            bootcli_fmt_mouse_status(mouse_status, sizeof(mouse_status));
        } else {
            mouse_status[0] = '\0';
            bootcli_buf_append(mouse_status, sizeof(mouse_status),
                               "Mouse guest non rilevato: run-gpu richiede virtio-mouse-device.");
        }

        bootcli_draw_text(48U, 150U, mouse_status,
                          bootcli_mouse_ready ? muted_color : warning_color,
                          panel_color);
    }

    first = (bootcli_line_count > visible) ?
            (bootcli_line_count - visible) : 0U;
    for (uint32_t i = first; i < bootcli_line_count; i++) {
        bootcli_draw_text(history_x, history_y + row * line_step,
                          bootcli_lines[i], title_color, panel_color);
        row++;
    }

    if (bootcli_graphics_mode) {
        uint32_t event_y = 420U;

        bootcli_fill_rect(40U, 390U, 720U, 2U, border_color);
        bootcli_draw_text(48U, 402U, "Eventi mouse recenti",
                          accent_color, panel_color);

        if (!bootcli_mouse_ready) {
            bootcli_draw_text(48U, event_y,
                              "Nessun puntatore guest attivo in questa sessione.",
                              muted_color, panel_color);
        } else if (bootcli_mouse_line_count == 0U) {
            bootcli_draw_text(48U, event_y,
                              "Muovi il mouse o fai click per generare eventi.",
                              muted_color, panel_color);
        } else {
            for (uint32_t i = 0U; i < bootcli_mouse_line_count; i++) {
                bootcli_draw_text(48U, event_y + i * line_step,
                                  bootcli_mouse_lines[i], title_color, panel_color);
            }
        }
    }

    bootcli_fill_rect(40U, prompt_y - 6U, 720U, 2U, border_color);

    {
        /* Prompt: "[cwd] > " prefix + input tail + cursor.
         * Scorrimento destra: se l'input e' piu' lungo dello spazio
         * disponibile, mostriamo la coda (posizione cursore) invece
         * di troncare a sinistra perdendo visibilita'. */
        char        prefix[64];
        uint32_t    prefix_len;
        uint32_t    cursor_len = ((cursor_phase & 1ULL) == 0ULL) ? 1U : 0U;
        uint32_t    avail;
        const char *input_tail;

        prefix[0] = '\0';
        bootcli_buf_append(prefix, sizeof(prefix), "[");
        bootcli_buf_append(prefix, sizeof(prefix), bootcli_cwd);
        bootcli_buf_append(prefix, sizeof(prefix), "] > ");
        prefix_len = bootcli_strlen(prefix);

        avail = (BOOTCLI_LINE_MAX > prefix_len + cursor_len)
                ? (BOOTCLI_LINE_MAX - prefix_len - cursor_len)
                : 0U;

        input_tail = bootcli_input;
        if (bootcli_input_len > avail && avail > 0U)
            input_tail = bootcli_input + (bootcli_input_len - avail);

        prompt_line[0] = '\0';
        bootcli_buf_append(prompt_line, sizeof(prompt_line), prefix);
        bootcli_buf_append(prompt_line, sizeof(prompt_line), input_tail);
        if (cursor_len)
            bootcli_buf_append(prompt_line, sizeof(prompt_line), "_");
        bootcli_draw_text(history_x, prompt_y, prompt_line, prompt_color, panel_color);
    }

    footer[0] = '\0';
    bootcli_buf_append(footer, sizeof(footer), "Scheduler OK | Heartbeat ");
    bootcli_buf_append_u32(footer, sizeof(footer), bootcli_heartbeat);
    bootcli_buf_append(footer, sizeof(footer), " | cwd ");
    bootcli_buf_append(footer, sizeof(footer), bootcli_cwd);
    if (bootcli_graphics_mode)
        bootcli_buf_append(footer, sizeof(footer), " | Scanout VirtIO attivo");
    else
        bootcli_buf_append(footer, sizeof(footer), " | Scanout framebuffer");
    if (bootcli_graphics_mode && bootcli_mouse_ready)
        bootcli_buf_append(footer, sizeof(footer), " | Mouse guest attivo");
    bootcli_draw_text(48U, footer_y, footer, muted_color, panel_color);

    bootcli_draw_mouse_cursor();
    bootcli_draw_mouse_cursor_fb();
    gpu_present_fullscreen();
}

static void bootcli_execute_command(void)
{
    char line[BOOTCLI_LINE_MAX + 1];

    bootcli_push_current_input();

    if (bootcli_input_len == 0U) {
        bootcli_push_line("Digita 'help' per i comandi disponibili.");
        return;
    }

    if (bootcli_streq(bootcli_input, "help")) {
        bootcli_push_line("help      mostra i comandi disponibili");
        bootcli_push_line("clear     pulisce la console di boot");
        bootcli_push_line("pwd       mostra la directory corrente");
        bootcli_push_line("cd PATH   cambia directory corrente");
        bootcli_push_line("gpu       mostra il backend grafico attivo");
        bootcli_push_line("net       mostra stato driver virtio-net e contatori base");
        bootcli_push_line("selftest [nome] esegue la suite o un singolo self-test");
        bootcli_push_line("fs        mostra lo stato del mount ext4");
        bootcli_push_line("ls [PATH] lista una directory VFS (default: cwd)");
        bootcli_push_line("cat PATH  mostra il contenuto di un file");
        bootcli_push_line("write P T tronca e scrive in un file ext4 esistente");
        bootcli_push_line("append P T aggiunge testo a un file ext4 esistente");
        bootcli_push_line("mkdir P   crea una directory ext4");
        bootcli_push_line("rm P      rimuove file o directory vuota");
        bootcli_push_line("mv A B    rinomina/sposta un path ext4");
        bootcli_push_line("fsync P   flush esplicito del singolo file");
        bootcli_push_line("truncate P N imposta la size di un file ext4 esistente");
        bootcli_push_line("sync      flush esplicito dei mount VFS attivi");
        bootcli_push_line("kbdlayout mostra il layout tastiera attivo");
        bootcli_push_line("loadkeys L attiva il layout tastiera (us/it) e prova a persisterlo");
        bootcli_push_line("arksh     lancia la shell arksh reale da /bin se presente");
        bootcli_push_line("login     rilancia la login shell bridge (/bin/arksh)");
        bootcli_push_line("nsh       lancia la shell ELF statica 80x25 di recovery");
        bootcli_push_line("elfdemo   lancia il demo ELF statico integrato a EL0");
        bootcli_push_line("execdemo  lancia un ELF che chiama execve('/EXEC2.ELF')");
        bootcli_push_line("dyndemo   lancia un PIE con PT_INTERP + DT_NEEDED");
        bootcli_push_line("forkdemo  lancia un ELF che verifica fork() + COW");
        bootcli_push_line("sigdemo   lancia un ELF che verifica signal + SIGCHLD");
        bootcli_push_line("mreactdemo lancia un ELF che attende su una word reattiva");
        bootcli_push_line("jobdemo   lancia un ELF che verifica groups/sessioni/job control");
        bootcli_push_line("nsdemo    lancia un ELF che verifica mount namespace + pivot_root");
        bootcli_push_line("posixdemo lancia un ELF che verifica pipe/dup/cwd/termios");
        bootcli_push_line("muslabi   lancia un ELF che verifica la ABI minima M11-01a/B3");
        bootcli_push_line("muslglob  lancia un ELF che verifica fnmatch()/glob() bootstrap");
        bootcli_push_line("musldl    lancia un ELF che verifica dlopen()/dlsym()/dlclose()");
        bootcli_push_line("arkshsmoke lancia uno smoke ELF buildato via CMake/toolchain EnlilOS");
        bootcli_push_line("socketdemo lancia un ELF che verifica BSD socket API TCP/UDP loopback");
        bootcli_push_line("clonedemo lancia un ELF che verifica clone(), gettid(), VM/FS/FILES/TLS condivisi");
        bootcli_push_line("threadlife lancia un ELF che verifica set_tid_address/tgkill/exit_group");
        bootcli_push_line("futexdemo lancia un ELF che verifica FUTEX_WAIT/WAKE/REQUEUE e join");
        bootcli_push_line("pthreaddemo lancia un ELF che verifica pthread/mutex/cond/signal");
        bootcli_push_line("semdemo   lancia un ELF che verifica sem_t sopra ksem");
        bootcli_push_line("tlsmtdemo lancia un ELF che verifica __thread + errno per-thread");
        bootcli_push_line("epolldemo lancia un ELF che verifica epoll_create1/ctl/pwait");
        bootcli_push_line("crtdemo   lancia un ELF che verifica crt1/init_array/TLS statico");
        bootcli_push_line("runelf P  carica e lancia un ELF64 da VFS");
        bootcli_push_line("mouse     mostra stato del puntatore guest");
        bootcli_push_line("echo TXT  ristampa il testo scritto");
        bootcli_push_line("keyboard  conferma che l'input arriva");
        bootcli_push_line("poweroff  spegne la macchina (PSCI SYSTEM_OFF)");
        bootcli_push_line("reboot    riavvia la macchina (PSCI SYSTEM_RESET)");
        bootcli_push_line("halt      halt CPU senza spegnere (WFE loop)");
    } else if (bootcli_streq(bootcli_input, "clear")) {
        bootcli_line_count = 0U;
        bootcli_push_line("Console pulita.");
    } else if (bootcli_streq(bootcli_input, "pwd")) {
        bootcli_cmd_pwd();
    } else if (bootcli_streq(bootcli_input, "cd")) {
        bootcli_cmd_cd("/");
    } else if (bootcli_startswith(bootcli_input, "cd ")) {
        bootcli_cmd_cd(bootcli_input + 3);
    } else if (bootcli_streq(bootcli_input, "gpu")) {
        line[0] = '\0';
        bootcli_buf_append(line, sizeof(line), "GPU: ");
        if (bootcli_caps.vendor == GPU_VENDOR_VIRTIO) {
            bootcli_buf_append(line, sizeof(line),
                               "VirtIO-GPU, page flip + renderer 2D batch attivi.");
        } else if (bootcli_caps.vendor == GPU_VENDOR_APPLE_AGX) {
            bootcli_buf_append(line, sizeof(line),
                               "Apple AGX backend + renderer 2D batch.");
        } else {
            bootcli_buf_append(line, sizeof(line),
                               "software fallback con renderer 2D batch.");
        }
        bootcli_push_line(line);
    } else if (bootcli_streq(bootcli_input, "net")) {
        bootcli_fmt_net_status(line, sizeof(line));
        bootcli_push_line(line);
    } else if (bootcli_streq(bootcli_input, "selftest")) {
        int rc = selftest_run_all();
        bootcli_push_line((rc == 0) ?
                          "Selftest: PASS (vedi log seriale per il dettaglio)." :
                          "Selftest: FAIL (vedi log seriale per il dettaglio).");
    } else if (bootcli_startswith(bootcli_input, "selftest ")) {
        int rc = selftest_run_named(bootcli_input + 9);
        bootcli_push_line((rc == 0) ?
                          "Selftest mirato: PASS (vedi log seriale per il dettaglio)." :
                          "Selftest mirato: FAIL (vedi log seriale per il dettaglio).");
    } else if (bootcli_streq(bootcli_input, "fs")) {
        bootcli_cmd_fs();
    } else if (bootcli_streq(bootcli_input, "ls")) {
        bootcli_cmd_ls(bootcli_cwd);
    } else if (bootcli_startswith(bootcli_input, "ls ")) {
        char resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_resolve_path(bootcli_input + 3, resolved, sizeof(resolved)))
            bootcli_push_line("ls: path troppo lungo o non valido.");
        else
            bootcli_cmd_ls(resolved);
    } else if (bootcli_startswith(bootcli_input, "cat ")) {
        char resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_resolve_path(bootcli_input + 4, resolved, sizeof(resolved)))
            bootcli_push_line("cat: path troppo lungo o non valido.");
        else
            bootcli_cmd_cat(resolved);
    } else if (bootcli_startswith(bootcli_input, "write ")) {
        const char *text;
        char        path[BOOTCLI_INPUT_MAX + 1];
        char        resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_split_path_text(bootcli_input, "write", path, sizeof(path), &text)) {
            bootcli_push_line("Uso: write PATH TESTO");
        } else if (!bootcli_resolve_path(path, resolved, sizeof(resolved))) {
            bootcli_push_line("write: path troppo lungo o non valido.");
        } else {
            bootcli_cmd_write_common(resolved, text, O_WRONLY | O_CREAT | O_TRUNC);
        }
    } else if (bootcli_startswith(bootcli_input, "append ")) {
        const char *text;
        char        path[BOOTCLI_INPUT_MAX + 1];
        char        resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_split_path_text(bootcli_input, "append", path, sizeof(path), &text)) {
            bootcli_push_line("Uso: append PATH TESTO");
        } else if (!bootcli_resolve_path(path, resolved, sizeof(resolved))) {
            bootcli_push_line("append: path troppo lungo o non valido.");
        } else {
            bootcli_cmd_write_common(resolved, text, O_WRONLY | O_CREAT | O_APPEND);
        }
    } else if (bootcli_startswith(bootcli_input, "mkdir ")) {
        char resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_resolve_path(bootcli_input + 6, resolved, sizeof(resolved))) {
            bootcli_push_line("mkdir: path troppo lungo o non valido.");
        } else {
            int rc = vfs_mkdir(resolved, 0755U);
            if (rc < 0)
                bootcli_push_error("mkdir", rc);
            else
                bootcli_push_line("mkdir OK.");
        }
    } else if (bootcli_startswith(bootcli_input, "rm ")) {
        char resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_resolve_path(bootcli_input + 3, resolved, sizeof(resolved))) {
            bootcli_push_line("rm: path troppo lungo o non valido.");
        } else {
            int rc = vfs_unlink(resolved);
            if (rc < 0)
                bootcli_push_error("rm", rc);
            else
                bootcli_push_line("rm OK.");
        }
    } else if (bootcli_startswith(bootcli_input, "mv ")) {
        char old_path[BOOTCLI_INPUT_MAX + 1];
        char new_path[BOOTCLI_INPUT_MAX + 1];
        char old_resolved[BOOTCLI_PATH_MAX + 1];
        char new_resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_split_two_paths(bootcli_input, "mv", old_path, sizeof(old_path),
                                     new_path, sizeof(new_path))) {
            bootcli_push_line("Uso: mv OLD_PATH NEW_PATH");
        } else if (!bootcli_resolve_path(old_path, old_resolved, sizeof(old_resolved)) ||
                   !bootcli_resolve_path(new_path, new_resolved, sizeof(new_resolved))) {
            bootcli_push_line("mv: path troppo lungo o non valido.");
        } else {
            int rc = vfs_rename(old_resolved, new_resolved);
            if (rc < 0)
                bootcli_push_error("mv", rc);
            else
                bootcli_push_line("mv OK.");
        }
    } else if (bootcli_startswith(bootcli_input, "fsync ")) {
        char resolved[BOOTCLI_PATH_MAX + 1];

        if (!bootcli_resolve_path(bootcli_input + 6, resolved, sizeof(resolved)))
            bootcli_push_line("fsync: path troppo lungo o non valido.");
        else
            bootcli_cmd_fsync(resolved);
    } else if (bootcli_startswith(bootcli_input, "truncate ")) {
        char     path[BOOTCLI_INPUT_MAX + 1];
        char     resolved[BOOTCLI_PATH_MAX + 1];
        uint64_t size;
        int      rc;

        if (!bootcli_split_path_value(bootcli_input, "truncate", path, sizeof(path), &size)) {
            bootcli_push_line("Uso: truncate PATH SIZE");
        } else if (!bootcli_resolve_path(path, resolved, sizeof(resolved))) {
            bootcli_push_line("truncate: path troppo lungo o non valido.");
        } else {
            rc = vfs_truncate(resolved, size);
            if (rc < 0) {
                bootcli_push_error("truncate", rc);
            } else {
                line[0] = '\0';
                bootcli_buf_append(line, sizeof(line), "truncate OK: ");
                bootcli_buf_append(line, sizeof(line), resolved);
                bootcli_buf_append(line, sizeof(line), " size=");
                bootcli_buf_append_u32(line, sizeof(line), (uint32_t)size);
                bootcli_push_line(line);
            }
        }
    } else if (bootcli_streq(bootcli_input, "sync")) {
        int rc = vfs_sync();
        if (rc < 0)
            bootcli_push_error("sync", rc);
        else
            bootcli_push_line("sync OK: cache ext4 e mount VFS flushati.");
    } else if (bootcli_streq(bootcli_input, "kbdlayout")) {
        line[0] = '\0';
        bootcli_buf_append(line, sizeof(line), "layout tastiera attivo: ");
        bootcli_buf_append(line, sizeof(line), keyboard_get_layout_name());
        bootcli_push_line(line);
    } else if (bootcli_startswith(bootcli_input, "loadkeys ")) {
        const char *name = bootcli_input + 9;

        if (!name[0]) {
            bootcli_push_line("Uso: loadkeys us|it");
        } else if (keyboard_set_layout_name(name) < 0) {
            bootcli_push_line("loadkeys: layout non supportato.");
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "layout attivo: ");
            bootcli_buf_append(line, sizeof(line), keyboard_get_layout_name());
            if (boot_persist_keyboard_layout(keyboard_get_layout_name()) < 0)
                bootcli_buf_append(line, sizeof(line), " (persistenza non salvata)");
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "arksh")) {
        bootcli_launch_real_arksh(1);
    } else if (bootcli_streq(bootcli_input, "login")) {
        bootcli_launch_default_shell(1);
    } else if (bootcli_streq(bootcli_input, "nsh")) {
        bootcli_launch_nsh(1);
    } else if (bootcli_streq(bootcli_input, "elfdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_demo(PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "ELF demo lanciato a EL0, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "execdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/EXEC1.ELF", "/EXEC1.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "execve demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "dyndemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/DYNDEMO.ELF", "/DYNDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "dynamic ELF lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "forkdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/FORKDEMO.ELF", "/FORKDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "fork demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "sigdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/SIGDEMO.ELF", "/SIGDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "signal demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "mreactdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/MREACTDEMO.ELF", "/MREACTDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "mreact demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "jobdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/JOBDEMO.ELF", "/JOBDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "job control demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "nsdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/NSDEMO.ELF", "/NSDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "namespace demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "posixdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/POSIXDEMO.ELF", "/POSIXDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "posix demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "muslabi")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/MUSLABI.ELF", "/MUSLABI.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "musl ABI demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "muslglob")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/MUSLGLOB.ELF", "/MUSLGLOB.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "musl glob demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "musldl")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/MUSLDL.ELF", "/MUSLDL.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "musl libdl demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "arkshsmoke")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/ARKSHSMK.ELF", "/ARKSHSMK.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "arksh toolchain smoke lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "socketdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/SOCKDEMO.ELF", "/SOCKDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "socket demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "clonedemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/CLONEDEMO.ELF", "/CLONEDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "clone demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "threadlife")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/THREADLIFE.ELF", "/THREADLIFE.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "thread lifecycle demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "futexdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/FUTEXDEMO.ELF", "/FUTEXDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "futex demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "pthreaddemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/PTHREADDEMO.ELF", "/PTHREADDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "pthread demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "semdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/SEMDEMO.ELF", "/SEMDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "sem demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "tlsmtdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/TLSMTDEMO.ELF", "/TLSMTDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "tls mt demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "epolldemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/EPOLLDEMO.ELF", "/EPOLLDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "epoll demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "crtdemo")) {
        uint32_t pid = 0U;
        if (elf64_spawn_path("/CRTDEMO.ELF", "/CRTDEMO.ELF", PRIO_KERNEL, &pid) < 0) {
            bootcli_push_line(elf64_last_error());
        } else {
            line[0] = '\0';
            bootcli_buf_append(line, sizeof(line), "crt demo lanciato, pid=");
            bootcli_buf_append_u32(line, sizeof(line), pid);
            bootcli_push_line(line);
        }
    } else if (bootcli_startswith(bootcli_input, "runelf ")) {
        char resolved[BOOTCLI_PATH_MAX + 1];
        if (!bootcli_resolve_path(bootcli_input + 7, resolved, sizeof(resolved))) {
            bootcli_push_line("runelf: path troppo lungo o non valido.");
        } else {
            bootcli_launch_shell(resolved, bootcli_input + 7, bootcli_input + 7, 1);
        }
    } else if (bootcli_streq(bootcli_input, "mouse")) {
        if (!bootcli_mouse_ready) {
            bootcli_push_line("Mouse: nessun puntatore guest attivo.");
        } else {
            bootcli_fmt_mouse_status(line, sizeof(line));
            bootcli_push_line(line);
        }
    } else if (bootcli_streq(bootcli_input, "keyboard")) {
        bootcli_push_line("Tastiera: input ricevuto correttamente.");
    } else if (bootcli_startswith(bootcli_input, "echo ")) {
        bootcli_push_line(bootcli_input + 5);
    } else if (bootcli_streq(bootcli_input, "poweroff") ||
               bootcli_streq(bootcli_input, "shutdown")) {
        bootcli_push_line("Shutdown in corso...");
        bootcli_render();
        shutdown_system(SHUTDOWN_POWEROFF);
    } else if (bootcli_streq(bootcli_input, "reboot")) {
        bootcli_push_line("Riavvio in corso...");
        bootcli_render();
        shutdown_system(SHUTDOWN_REBOOT);
    } else if (bootcli_streq(bootcli_input, "halt")) {
        bootcli_push_line("Halt in corso...");
        bootcli_render();
        shutdown_system(SHUTDOWN_HALT);
    } else {
        bootcli_push_line("Comando sconosciuto. Usa 'help'.");
    }

    bootcli_input_len = 0U;
    bootcli_input[0] = '\0';
}

static int bootcli_poll_input(void)
{
    int dirty = 0;
    int c;

    if (bootcli_mode == BOOTCLI_MODE_TERM)
        return 0;

    while ((c = keyboard_getc()) >= 0) {
        uint8_t ch = (uint8_t)c;

        if (ch == '\r')
            ch = '\n';

        if (ch == '\n') {
            bootcli_execute_command();
            dirty = 1;
            if (bootcli_mode == BOOTCLI_MODE_TERM)
                break;
            continue;
        }

        if (ch == '\b' || ch == 0x7FU) {
            if (bootcli_input_len > 0U) {
                bootcli_input[--bootcli_input_len] = '\0';
                dirty = 1;
            }
            continue;
        }

        if (ch == 0x03U) {
            bootcli_input_len = 0U;
            bootcli_input[0] = '\0';
            bootcli_push_line("^C");
            dirty = 1;
            continue;
        }

        if (ch >= 32U && ch < 127U && bootcli_input_len < BOOTCLI_INPUT_MAX) {
            bootcli_input[bootcli_input_len++] = (char)ch;
            bootcli_input[bootcli_input_len] = '\0';
            dirty = 1;
        }
    }

    return dirty;
}

static void bootcli_record_mouse_event(const mouse_event_t *ev, uint32_t old_buttons)
{
    char line[BOOTCLI_MOUSE_LINE_MAX + 1];

    if (!ev) return;

    line[0] = '\0';

    if (ev->flags & MOUSE_EVT_BUTTON) {
        bootcli_buf_append(line, sizeof(line), "click ");
        if ((old_buttons ^ ev->buttons) & MOUSE_BTN_LEFT)
            bootcli_buf_append(line, sizeof(line),
                               (ev->buttons & MOUSE_BTN_LEFT) ? "L-down " : "L-up ");
        if ((old_buttons ^ ev->buttons) & MOUSE_BTN_RIGHT)
            bootcli_buf_append(line, sizeof(line),
                               (ev->buttons & MOUSE_BTN_RIGHT) ? "R-down " : "R-up ");
        if ((old_buttons ^ ev->buttons) & MOUSE_BTN_MIDDLE)
            bootcli_buf_append(line, sizeof(line),
                               (ev->buttons & MOUSE_BTN_MIDDLE) ? "M-down " : "M-up ");
    } else if ((ev->flags & MOUSE_EVT_WHEEL) && ev->wheel != 0) {
        bootcli_buf_append(line, sizeof(line), "wheel ");
        bootcli_buf_append_i32(line, sizeof(line), ev->wheel);
        bootcli_buf_append(line, sizeof(line), " ");
    } else if (ev->flags & MOUSE_EVT_ABS) {
        bootcli_buf_append(line, sizeof(line), "move abs ");
    } else if (ev->flags & MOUSE_EVT_MOVE) {
        bootcli_buf_append(line, sizeof(line), "move ");
    }

    if ((ev->flags & MOUSE_EVT_MOVE) && !(ev->flags & MOUSE_EVT_ABS)) {
        bootcli_buf_append(line, sizeof(line), "dx=");
        bootcli_buf_append_i32(line, sizeof(line), ev->dx);
        bootcli_buf_append(line, sizeof(line), " dy=");
        bootcli_buf_append_i32(line, sizeof(line), ev->dy);
        bootcli_buf_append(line, sizeof(line), " ");
    }

    if (line[0] == '\0')
        return;

    bootcli_buf_append(line, sizeof(line), "@ ");
    bootcli_buf_append_u32(line, sizeof(line), (uint32_t)bootcli_mouse_x);
    bootcli_buf_append(line, sizeof(line), ",");
    bootcli_buf_append_u32(line, sizeof(line), (uint32_t)bootcli_mouse_y);
    bootcli_mouse_push_event(line);
}

static int bootcli_poll_mouse(void)
{
    int dirty = 0;
    mouse_event_t ev;

    if (bootcli_mode == BOOTCLI_MODE_TERM)
        return 0;

    while (mouse_get_event(&ev)) {
        uint32_t old_buttons = bootcli_mouse_buttons;

        if (ev.flags & MOUSE_EVT_ABS) {
            /* mi_scale_abs() garantisce già [0, FB_W/H-1]; clamp difensivo */
            bootcli_mouse_x = bootcli_clamp_i32((int32_t)ev.x, 0, FB_WIDTH - 1);
            bootcli_mouse_y = bootcli_clamp_i32((int32_t)ev.y, 0, FB_HEIGHT - 1);
        } else {
            bootcli_mouse_x = bootcli_clamp_i32(bootcli_mouse_x + ev.dx,
                                                0, FB_WIDTH - 1);
            bootcli_mouse_y = bootcli_clamp_i32(bootcli_mouse_y + ev.dy,
                                                0, FB_HEIGHT - 1);
        }
        bootcli_mouse_buttons = ev.buttons;
        bootcli_mouse_wheel_total += ev.wheel;
        bootcli_record_mouse_event(&ev, old_buttons);
        dirty = 1;
    }

    return dirty;
}

static void bootcli_init(void)
{
    term80_init();
    bootcli_line_count = 0U;
    bootcli_input_len = 0U;
    bootcli_input[0] = '\0';
    bootcli_cwd[0] = '/';
    bootcli_cwd[1] = '\0';
    bootcli_mouse_line_count = 0U;
    bootcli_mouse_x = FB_WIDTH / 2;
    bootcli_mouse_y = FB_HEIGHT / 2;
    bootcli_mouse_buttons = 0U;
    bootcli_mouse_wheel_total = 0;
    bootcli_mouse_ready = (uint8_t)mouse_is_ready();
    bootcli_mode = BOOTCLI_MODE_UI;
    bootcli_shell_pid = 0U;
    bootcli_shell_name[0] = '\0';

    gpu_get_caps(&bootcli_caps);
    bootcli_graphics_mode = (bootcli_caps.vendor == GPU_VENDOR_VIRTIO) ? 1U : 0U;

    bootcli_push_line("EnlilOS boot console pronta.");
    if (bootcli_graphics_mode)
        bootcli_push_line("Modalita grafica attiva: VirtIO-GPU.");
    else
        bootcli_push_line("Modalita framebuffer locale attiva.");
    bootcli_push_line("Login shell di default: /bin/arksh (bridge con fallback automatico a nsh).");
    bootcli_push_line("Digita 'help' e premi Invio per testare tastiera e comandi di recovery.");
    bootcli_push_line("Prova anche: arksh, login, nsh, net, pwd, cd /data, ls, cat /BOOT.TXT.");
    {
        char line[96];
        line[0] = '\0';
        bootcli_buf_append(line, sizeof(line), "Layout tastiera attivo: ");
        bootcli_buf_append(line, sizeof(line), keyboard_get_layout_name());
        bootcli_buf_append(line, sizeof(line), " | usa 'loadkeys us|it' o 'kbdlayout'.");
        bootcli_push_line(line);
    }
    bootcli_push_line("M5-04: write/append/create/mkdir/rm/mv/fsync/truncate/sync su ext4.");
    bootcli_push_line("M6-03: elfdemo, execdemo, dyndemo e runelf PATH per ELF64 a EL0.");
    bootcli_push_line("M8-04: prova 'jobdemo' per process group, sessione e waitpid(WUNTRACED).");
    bootcli_push_line("M8-08a/b/c: prova 'posixdemo' per pipe, dup/dup2, cwd ed echo/raw termios.");
    bootcli_push_line("M11-01b: prova 'crtdemo' per crt1, costruttori, distruttori e TLS statico.");
    bootcli_push_line("M11-01a/B3: prova 'muslabi' per openat/lseek/readv/writev/fcntl/ioctl/uname/auxv.");
    bootcli_push_line("M8-08d: prova 'muslglob' per fnmatch(), glob() e wildcard su VFS.");
    bootcli_push_line("M11-03: prova 'musldl' per dlopen(), dlsym(), dlclose() e libdl bootstrap.");
    bootcli_push_line("M10-03: prova 'socketdemo' per BSD socket API TCP/UDP su loopback 127.0.0.1.");
    bootcli_push_line("M8-08e: prova 'arkshsmoke' per la toolchain CMake/cross-build EnlilOS.");
    bootcli_push_line("M8-08f: 'login' usa il bridge /bin/arksh; 'arksh' prova solo la shell reale in /bin/arksh.real.");
    bootcli_push_line("M11-02a: prova 'clonedemo' per clone(), gettid(), CLONE_VM/FS/FILES e TPIDR_EL0 per-thread.");
    bootcli_push_line("M11-02b: prova 'threadlife' per set_tid_address(), tgkill(), clear_child_tid ed exit_group().");
    bootcli_push_line("M11-02c: prova 'futexdemo' per FUTEX_WAIT/WAKE/REQUEUE e join via clear_child_tid.");
    bootcli_push_line("M11-02d: prova 'pthreaddemo' e 'semdemo' per pthread/mutex/cond/signal e sem_t POSIX.");
    bootcli_push_line("M11-02e: prova 'tlsmtdemo' per TLS multi-thread, __thread ed errno per-thread.");
    bootcli_push_line("M11-05b: prova 'epolldemo' per epoll_create1(), ctl(), edge-trigger e timeout.");
    bootcli_push_line("M9-04: prova 'nsdemo' per bind mount, cwd reale, unshare e pivot_root.");
    bootcli_push_line("M9-02: vfsd user-space bootstrap attivo sopra il backend VFS kernel.");
    bootcli_push_line("M8-05: prova 'mreactdemo' e poi osserva /data/MREACT.TXT.");
    bootcli_push_line("M7-02: usa 'nsh' per recovery shell, 'login' per il bridge, 'arksh' per la shell reale.");
    if (bootcli_graphics_mode) {
        bootcli_push_line("Fai click nella finestra QEMU per il focus.");
        if (bootcli_mouse_ready)
            bootcli_push_line("Mouse guest pronto: puntatore ed eventi attivi.");
        else
            bootcli_push_line("Mouse guest non rilevato: tastiera soltanto.");
    }
}

/*
 * ticker_task — task demo (M2-03)
 *
 * Gira a priorità PRIO_HIGH (32). Ogni ~500ms aggiorna un heartbeat
 * che la boot console usa per mostrare che scheduler e timer sono vivi.
 */
static void ticker_task(void)
{
    uint64_t last  = 0;

    while (1) {
        uint64_t now = timer_now_ms();
        if (now - last >= 500) {
            last = now;
            bootcli_heartbeat++;
        }
        sched_yield();
    }
}

static void ext4_flush_task(void)
{
    uint64_t last_check = 0ULL;

    while (1) {
        uint64_t now = timer_now_ms();

        if (now - last_check >= 200ULL) {
            (void)ext4_service_writeback(250ULL);
            last_check = now;
        }
        sched_yield();
    }
}

void kernel_main(void)
{
    /* === Fase 1: Inizializzazione hardware === */
    uart_init();
    print_banner();

    uart_puts("[EnlilOS] Boot in corso...\n");
    uart_puts("[EnlilOS] CPU: AArch64 (ARMv8-A)\n");
    uart_puts("[EnlilOS] UART: PL011 @ 0x09000000 - OK\n");

    /* === Fase 2: Exception vectors (M1-01) === */
    exception_init();
    /* Da qui in poi fault/abort vengono catturati e diagnosticati */

    /* === Fase 3: MMU + Cache (M1-02) === */
    mmu_init();
    /* Da qui: VA==PA identity map, D-cache e I-cache attivi.
     * Memoria RAM: Normal WB — accessi kernel cacheable e coerenti.
     * Memoria MMIO: Device-nGnRnE — accessi non-cached, ordered.
     * TLB miss WCET: 1 memory access (L1 block 1GB, nessun L2/L3). */

    /* Pre-faulta il kernel stesso nel TLB: zero TLB miss durante l'esecuzione */
    extern uint8_t __bss_end;
    mmu_prefault_range(0x40000000, (uintptr_t)&__bss_end);

    /* === Fase 4: Physical Memory Manager (M1-03) === */
    pmm_init();
    /* Da qui: phys_alloc_page() O(≤11), kmalloc() O(1) hot path */

    /* Test RT: alloca/libera pagine e oggetti slab */
    uint64_t p1 = phys_alloc_page();
    uint64_t p2 = phys_alloc_pages(2);  /* 4 pagine contigue (ordine 2) */
    void    *o1 = kmalloc(48);          /* → classe 64B */
    void    *o2 = kmalloc(300);         /* → classe 512B */
    phys_free_page(p1);
    phys_free_pages(p2, 2);
    kfree(o1);
    kfree(o2);
    uart_puts("[PMM] Test alloc/free OK\n");

    /* === Fase 5: GIC-400 Interrupt Controller (M2-01) === */
    gic_init();
    /* Registra IRQ UART0 (SPI #33) — livello driver, level-triggered.
     * Handler stub: al boot vogliamo solo il GIC funzionante; il vero
     * driver UART interrupt-driven arriva con M4-01. */
    gic_register_irq(IRQ_UART0, NULL,         /* NULL → default handler */
                     NULL, GIC_PRIO_DRIVER, GIC_FLAG_LEVEL);
    gic_enable_irq(IRQ_UART0);

    /* Abilita IRQ globali sul core — da qui il GIC può interrompere il kernel */
    gic_enable_irqs();
    uart_puts("[GIC] IRQ globali abilitati — DAIF.I = 0\n");

    gic_stats();

    /* === Fase 6: ARM Generic Timer (M2-02) === */
    timer_init();
    timer_start();
    kdebug_init();
    /* Da qui: tick ogni 1ms, jiffies incrementato dall'IRQ handler.
     * timer_now_ns() O(1), timer_now_ms() O(1) — usabili ovunque. */

    /* Verifica che il timer stia girando: aspetta 10ms e controlla jiffies */
    {
        uint64_t t0 = timer_now_ns();
        timer_delay_us(10000);   /* busy-wait 10ms — solo al boot */
        uint64_t t1 = timer_now_ns();
        uint64_t elapsed_us = (t1 - t0) / 1000;
        uart_puts("[TIMER] Test: elapsed ~");
        /* stampa semplice senza printf */
        uint64_t ms = elapsed_us / 1000;
        uint64_t us_rem = elapsed_us % 1000;
        if (ms > 0) { /* pr_dec non è visibile qui, usiamo UART diretta */
            char buf[8]; int len = 0;
            uint64_t v = ms;
            while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
            for (int i = len-1; i >= 0; i--) uart_putc(buf[i]);
        } else {
            uart_putc('0');
        }
        uart_puts(".");
        {
            char buf[4]; int len = 0;
            uint64_t v = us_rem;
            while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
            while (len < 3) buf[len++] = '0';
            for (int i = len-1; i >= 0; i--) uart_putc(buf[i]);
        }
        uart_puts(" ms — jiffies=");
        {
            char buf[12]; int len = 0;
            uint64_t v = timer_now_ms();
            if (v == 0) { uart_putc('0'); }
            else {
                while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
                for (int i = len-1; i >= 0; i--) uart_putc(buf[i]);
            }
        }
        uart_puts("\n");
        (void)elapsed_us;
    }
    timer_stats();

    /* === Fase 7: Scheduler FPP (M2-03) === */
    sched_init();
    syscall_init();
    keyboard_init();
    mouse_init();
    blk_init();
    vfs_rescan();
    boot_prepare_login_layout();
    boot_apply_keyboard_layout_config();
    net_init();
    ane_init();
    gpu_init();
    /* Da qui: task switching via timer IRQ ogni 1ms.
     * sched_tick() → need_resched → vectors.S → schedule() */

    /* Task demo: ticker (prio 64) — stampa un contatore ogni ~500ms */
    sched_task_create("ticker", ticker_task, PRIO_HIGH);
    if (ext4_is_mounted())
        sched_task_create("ext4-flush", ext4_flush_task, PRIO_LOW);
    sched_stats();

    /* === Fase 8: Kernel Heap — named typed caches (M1-04) === */
    kheap_init();
    /* Da qui: task_cache, port_cache, ipc_cache disponibili.
     * kmem_cache_alloc/free O(1) garantito (cache pre-caldate). */

    /* Test named cache: alloca/libera strutture tipizzate */
    void *t1 = kmem_cache_alloc(task_cache);
    void *p1_nc = kmem_cache_alloc(port_cache);
    void *m1 = kmem_cache_alloc(ipc_cache);
    kmem_cache_free(task_cache, t1);
    kmem_cache_free(port_cache, p1_nc);
    kmem_cache_free(ipc_cache,  m1);
    uart_puts("[KHEAP] Test named cache alloc/free OK\n");

    /* === Capability System (M9-01) === */
    cap_init();
    uart_puts("[CAP] Capability system inizializzato\n");

    /* === Virtual Memory Area manager (M8-02) === */
    vmm_init();

    /* === Fase 8: Microkernel === */
    mk_init();

    /* === Fase 9: Framebuffer === */
    fb_init();

    /* Crea task per i server di sistema (stile Hurd) */
    mk_task_create("uart-server", TASK_TYPE_SERVER, 0);
    mk_task_create("fb-server", TASK_TYPE_SERVER, 0);
    mk_task_create("mem-server", TASK_TYPE_SERVER, 0);
    mk_task_create("blk-server", TASK_TYPE_SERVER, 0);
    mk_task_create("net-server", TASK_TYPE_SERVER, 0);
    mk_task_create("vfs-server", TASK_TYPE_SERVER, 0);
    boot_launch_vfsd();
    boot_launch_blkd();
    boot_launch_netd();

    uart_puts("[EnlilOS] Server di sistema registrati\n");

#ifdef ENLILOS_SELFTEST
    {
        int rc = selftest_run_all();

        uart_puts("[SELFTEST] Kernel autorun completato: ");
        uart_puts((rc == 0) ? "PASS\n" : "FAIL\n");
        uart_puts("[SELFTEST] Arresto automatico della macchina di test\n");
        shutdown_system(SHUTDOWN_POWEROFF);
    }
#endif

    bootcli_init();
    bootcli_launch_default_shell(0);
    bootcli_render();

    uart_puts("\n[EnlilOS] ===================================\n");
    uart_puts("[EnlilOS] Boot completato con successo!\n");
    uart_puts("[EnlilOS] Console interattiva pronta\n");
    uart_puts("[EnlilOS] Login shell: /bin/arksh (bridge con fallback automatico a /bin/nsh)\n");
    uart_puts("[EnlilOS] Layout tastiera: ");
    uart_puts(keyboard_get_layout_name());
    uart_puts(" (usa 'loadkeys us|it' o 'kbdlayout')\n");
    uart_puts("[EnlilOS] Comandi: 'arksh' prova la shell reale, 'login' rilancia il bridge, 'nsh' apre la recovery shell\n");
    uart_puts("[EnlilOS] Rete: usa 'net' per vedere MAC/link/counter del driver virtio-net\n");
    uart_puts("[EnlilOS] Scheduler FPP attivo — heartbeat ogni 500ms\n");
    uart_puts("[EnlilOS] ===================================\n\n");

    /* Loop di boot console: input tastiera + refresh grafico opportunistico. */
    uint64_t last_cursor_phase = ~0ULL;
    uint32_t last_heartbeat = bootcli_heartbeat;
    while (1) {
        int dirty = bootcli_poll_shell();
        dirty |= bootcli_poll_input();
        dirty |= bootcli_poll_mouse();
        uint64_t cursor_phase = timer_now_ms() / 400ULL;

        if (bootcli_mode == BOOTCLI_MODE_UI && cursor_phase != last_cursor_phase) {
            last_cursor_phase = cursor_phase;
            dirty = 1;
        }

        if (bootcli_heartbeat != last_heartbeat) {
            last_heartbeat = bootcli_heartbeat;
            dirty = 1;
        }

        if (dirty)
            bootcli_render();

        sched_yield();
    }
}
