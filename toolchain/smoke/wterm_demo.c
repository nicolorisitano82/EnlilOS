/*
 * wterm_demo — terminale Wayland minimale con PTY reale e /bin/bash.
 *
 * Questa e' la prima slice utile della sessione desktop: una finestra
 * grafica Wayland agganciata a un PTY che esegue bash interattiva.
 * Il compositor inoltra i byte tastiera via protocollo privato
 * enlil_term_v1, mentre questo client fa da terminal emulator minimale.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pty.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>

#define WL_DISPLAY_GET_REGISTRY      1U
#define WL_REGISTRY_BIND             0U
#define WL_REGISTRY_GLOBAL           0U
#define WL_COMPOSITOR_CREATE_SURFACE 0U
#define WL_SHM_CREATE_POOL_SYSV      0U
#define WL_SHM_POOL_CREATE_BUFFER    0U
#define WL_SURFACE_ATTACH            1U
#define WL_SURFACE_DAMAGE            2U
#define WL_SURFACE_COMMIT            6U
#define XDG_WM_GET_XDG_SURFACE       2U
#define XDG_SURFACE_GET_TOPLEVEL     1U
#define XDG_SURFACE_ACK_CONFIGURE    4U
#define XDG_TOP_SET_TITLE            2U
#define XDG_SURFACE_CONFIGURE        0U
#define XDG_TOPLEVEL_CLOSE           1U

#define ENLIL_TERM_INPUT             0U
#define ENLIL_TERM_ATTACH_SURFACE    0U
#define ENLIL_TERM_RELEASE           1U

#define OBJ_REGISTRY                 2U
#define OBJ_COMPOSITOR               3U
#define OBJ_SHM                      4U
#define OBJ_XDG_WM                   5U
#define OBJ_SHM_POOL                 6U
#define OBJ_BUFFER                   7U
#define OBJ_SURFACE                  8U
#define OBJ_XDG_SURFACE              9U
#define OBJ_XDG_TOPLEVEL             10U
#define OBJ_ENLIL_TERM               11U

#define TERM_COLS                    80
#define TERM_ROWS                    24
#define CELL_W                       8
#define CELL_H                       16
#define SURF_W                       (TERM_COLS * CELL_W)
#define SURF_H                       (TERM_ROWS * CELL_H)
#define SURF_SZ                      (SURF_W * SURF_H * 4)
#define INPUT_QUEUE_MAX              512U

#define WTERM_READY_PATH             "/data/WTERMREADY.TXT"
#define WTERM_FAIL_PATH              "/data/WTERMFAIL.TXT"

#define TERM_STATE_NORMAL            0U
#define TERM_STATE_ESC               1U
#define TERM_STATE_CSI               2U

#define TERM_ATTR_REVERSE            0x01U

typedef struct {
    int      fd;
    uint8_t  obuf[8192];
    uint32_t olen;
    uint8_t  ibuf[8192];
    uint32_t ilen;
} wl_conn_t;

typedef struct {
    uint32_t obj_id;
    uint32_t opcode;
    uint32_t plen;
    uint8_t  payload[512];
} wl_event_t;

typedef struct {
    uint8_t ch;
    uint8_t fg;
    uint8_t bg;
    uint8_t attr;
} term_cell_t;

typedef struct {
    term_cell_t cells[TERM_ROWS][TERM_COLS];
    uint32_t    cursor_x;
    uint32_t    cursor_y;
    uint32_t    saved_x;
    uint32_t    saved_y;
    uint8_t     cur_fg;
    uint8_t     cur_bg;
    uint8_t     cur_attr;
    uint8_t     parse_state;
    uint8_t     csi_priv;
    uint8_t     csi_count;
    uint8_t     csi_have_value;
    uint32_t    csi_values[8];
    uint32_t    csi_value;
    uint8_t     dirty;
} term_state_t;

typedef struct {
    wl_conn_t   conn;
    uint32_t    comp_name;
    uint32_t    shm_name;
    uint32_t    xdg_name;
    uint32_t    term_name;
    uint32_t    serial;
    uint32_t   *buf;
    int         shmid;
    int         configured;
    int         running;
    int         close_requested;
    int         master_fd;
    pid_t       child_pid;
    int         child_status;
    uint8_t     input_q[INPUT_QUEUE_MAX];
    uint32_t    input_len;
    term_state_t term;
} app_t;

static const uint32_t term_palette[16] = {
    0x00000000U, 0x00CC5555U, 0x0055CC55U, 0x00CDCD55U,
    0x005555CCU, 0x00CC55CCU, 0x0055CDCDU, 0x00C0C0C0U,
    0x00606060U, 0x00FF6666U, 0x0066FF66U, 0x00FFFF66U,
    0x006666FFU, 0x00FF66FFU, 0x0066FFFFU, 0x00FFFFFFU
};

#include "wterm_font.inc"

static void write_marker_file(const char *path, const char *text)
{
    int fd;

    if (!path || !text)
        return;
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0)
        return;
    (void)write(fd, text, strlen(text));
    close(fd);
}

static void sleep_10ms(void)
{
    struct timespec sl = { 0, 10000000L };
    nanosleep(&sl, NULL);
}

static uint32_t in_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void out_u32(wl_conn_t *c, uint32_t v)
{
    if (!c || c->olen + 4U > sizeof(c->obuf))
        return;
    c->obuf[c->olen + 0U] = (uint8_t)v;
    c->obuf[c->olen + 1U] = (uint8_t)(v >> 8);
    c->obuf[c->olen + 2U] = (uint8_t)(v >> 16);
    c->obuf[c->olen + 3U] = (uint8_t)(v >> 24);
    c->olen += 4U;
}

static void out_str(wl_conn_t *c, const char *s)
{
    uint32_t slen;
    uint32_t plen;

    if (!c || !s)
        return;
    slen = (uint32_t)(strlen(s) + 1U);
    plen = (slen + 3U) & ~3U;
    out_u32(c, slen);
    if (c->olen + plen > sizeof(c->obuf))
        return;
    memcpy(c->obuf + c->olen, s, slen);
    if (plen > slen)
        memset(c->obuf + c->olen + slen, 0, plen - slen);
    c->olen += plen;
}

static void msg_begin(wl_conn_t *c, uint32_t obj, uint32_t opcode, uint32_t extra_bytes)
{
    uint32_t size = 8U + extra_bytes;

    out_u32(c, obj);
    out_u32(c, (size << 16) | opcode);
}

static void wl_flush(wl_conn_t *c)
{
    size_t sent = 0U;

    if (!c)
        return;
    while (sent < c->olen) {
        ssize_t rc = send(c->fd, c->obuf + sent, c->olen - sent, MSG_DONTWAIT);
        if (rc <= 0)
            break;
        sent += (size_t)rc;
    }
    if (sent < c->olen)
        memmove(c->obuf, c->obuf + sent, c->olen - sent);
    c->olen -= (uint32_t)sent;
}

static int wl_next_event(wl_conn_t *c, wl_event_t *ev)
{
    uint32_t hdr2;
    uint32_t size;

    if (!c || !ev || c->ilen < 8U)
        return 0;
    hdr2 = in_u32(c->ibuf + 4U);
    size = hdr2 >> 16;
    if (size < 8U || c->ilen < size)
        return 0;
    ev->obj_id = in_u32(c->ibuf);
    ev->opcode = hdr2 & 0xFFFFU;
    ev->plen = size - 8U;
    if (ev->plen > sizeof(ev->payload))
        return 0;
    memcpy(ev->payload, c->ibuf + 8U, ev->plen);
    c->ilen -= size;
    if (c->ilen > 0U)
        memmove(c->ibuf, c->ibuf + size, c->ilen);
    return 1;
}

static void bind_global(wl_conn_t *conn, uint32_t name,
                        const char *iface, uint32_t version, uint32_t new_id)
{
    uint32_t extra = 4U + 4U + (((uint32_t)strlen(iface) + 1U + 3U) & ~3U) + 4U + 4U;

    msg_begin(conn, OBJ_REGISTRY, WL_REGISTRY_BIND, extra);
    out_u32(conn, name);
    out_str(conn, iface);
    out_u32(conn, version);
    out_u32(conn, new_id);
    wl_flush(conn);
}

static int wl_connect_runtime(app_t *app)
{
    struct sockaddr_un sun;
    uint32_t           comp_name = 0U;
    uint32_t           shm_name = 0U;
    uint32_t           xdg_name = 0U;
    uint32_t           term_name = 0U;

    memset(&app->conn, 0, sizeof(app->conn));
    app->conn.fd = -1;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "/run/wayland-0");

    app->conn.fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (app->conn.fd < 0)
        return -1;

    for (int t = 0; t < 30; t++) {
        if (connect(app->conn.fd, (struct sockaddr *)&sun, sizeof(sun)) == 0)
            break;
        if (t == 29)
            return -1;
        sleep_10ms();
    }

    msg_begin(&app->conn, 1U, WL_DISPLAY_GET_REGISTRY, 4U);
    out_u32(&app->conn, OBJ_REGISTRY);
    wl_flush(&app->conn);

    for (int t = 0; t < 100; t++) {
        wl_event_t ev;
        ssize_t    n = recv(app->conn.fd,
                            app->conn.ibuf + app->conn.ilen,
                            sizeof(app->conn.ibuf) - app->conn.ilen,
                            MSG_DONTWAIT);
        if (n > 0)
            app->conn.ilen += (uint32_t)n;

        while (wl_next_event(&app->conn, &ev)) {
            if (ev.obj_id == OBJ_REGISTRY && ev.opcode == WL_REGISTRY_GLOBAL && ev.plen >= 12U) {
                uint32_t name = in_u32(ev.payload);
                uint32_t slen = in_u32(ev.payload + 4U);
                uint32_t copy;
                char     iface[64];

                copy = (slen < sizeof(iface)) ? slen : (uint32_t)sizeof(iface) - 1U;
                memcpy(iface, ev.payload + 8U, copy);
                iface[copy] = '\0';

                if (strcmp(iface, "wl_compositor") == 0)
                    comp_name = name;
                else if (strcmp(iface, "wl_shm") == 0)
                    shm_name = name;
                else if (strcmp(iface, "xdg_wm_base") == 0)
                    xdg_name = name;
                else if (strcmp(iface, "enlil_term_v1") == 0)
                    term_name = name;
            }
        }

        if (comp_name != 0U && shm_name != 0U && xdg_name != 0U && term_name != 0U) {
            app->comp_name = comp_name;
            app->shm_name = shm_name;
            app->xdg_name = xdg_name;
            app->term_name = term_name;
            return 0;
        }
        sleep_10ms();
    }
    return -1;
}

static void term_mark_dirty(term_state_t *t)
{
    if (t)
        t->dirty = 1U;
}

static void term_reset_style(term_state_t *t)
{
    if (!t)
        return;
    t->cur_fg = 7U;
    t->cur_bg = 0U;
    t->cur_attr = 0U;
}

static void term_clear_cell_row(term_state_t *t, uint32_t row, uint32_t from, uint32_t to)
{
    term_cell_t blank;

    if (!t || row >= TERM_ROWS)
        return;
    if (to > TERM_COLS)
        to = TERM_COLS;
    if (from >= to)
        return;

    blank.ch = ' ';
    blank.fg = t->cur_fg;
    blank.bg = t->cur_bg;
    blank.attr = 0U;
    for (uint32_t col = from; col < to; col++)
        t->cells[row][col] = blank;
}

static void term_reset(term_state_t *t)
{
    if (!t)
        return;
    memset(t, 0, sizeof(*t));
    term_reset_style(t);
    for (uint32_t row = 0; row < TERM_ROWS; row++)
        term_clear_cell_row(t, row, 0U, TERM_COLS);
    t->dirty = 1U;
}

static void term_scroll_up(term_state_t *t, uint32_t lines)
{
    if (!t || lines == 0U)
        return;
    if (lines >= TERM_ROWS) {
        for (uint32_t row = 0; row < TERM_ROWS; row++)
            term_clear_cell_row(t, row, 0U, TERM_COLS);
        t->cursor_y = TERM_ROWS - 1U;
        t->cursor_x = 0U;
        term_mark_dirty(t);
        return;
    }

    memmove(&t->cells[0][0],
            &t->cells[lines][0],
            (TERM_ROWS - lines) * TERM_COLS * sizeof(term_cell_t));
    for (uint32_t row = TERM_ROWS - lines; row < TERM_ROWS; row++)
        term_clear_cell_row(t, row, 0U, TERM_COLS);
    if (t->cursor_y >= lines)
        t->cursor_y -= lines;
    else
        t->cursor_y = 0U;
    term_mark_dirty(t);
}

static void term_ensure_cursor_visible(term_state_t *t)
{
    if (!t)
        return;
    if (t->cursor_x >= TERM_COLS) {
        t->cursor_x = 0U;
        t->cursor_y++;
    }
    if (t->cursor_y >= TERM_ROWS)
        term_scroll_up(t, t->cursor_y - (TERM_ROWS - 1U));
}

static void term_put_char(term_state_t *t, uint8_t ch)
{
    term_cell_t *cell;

    if (!t)
        return;
    term_ensure_cursor_visible(t);
    if (t->cursor_y >= TERM_ROWS || t->cursor_x >= TERM_COLS)
        return;

    cell = &t->cells[t->cursor_y][t->cursor_x];
    cell->ch = (ch >= 32U && ch <= 126U) ? ch : (uint8_t)'?';
    cell->fg = t->cur_fg;
    cell->bg = t->cur_bg;
    cell->attr = t->cur_attr;
    t->cursor_x++;
    term_ensure_cursor_visible(t);
    term_mark_dirty(t);
}

static void term_newline(term_state_t *t)
{
    if (!t)
        return;
    t->cursor_x = 0U;
    t->cursor_y++;
    term_ensure_cursor_visible(t);
    term_mark_dirty(t);
}

static void term_carriage_return(term_state_t *t)
{
    if (!t)
        return;
    t->cursor_x = 0U;
    term_mark_dirty(t);
}

static void term_backspace(term_state_t *t)
{
    if (!t)
        return;
    if (t->cursor_x > 0U)
        t->cursor_x--;
    term_mark_dirty(t);
}

static void term_tab(term_state_t *t)
{
    uint32_t next;

    if (!t)
        return;
    next = (t->cursor_x + 8U) & ~7U;
    while (t->cursor_x < next && t->cursor_x < TERM_COLS)
        term_put_char(t, (uint8_t)' ');
}

static uint32_t term_csi_value_or(const term_state_t *t, uint32_t idx, uint32_t def)
{
    if (!t)
        return def;
    if (idx >= t->csi_count)
        return def;
    if (t->csi_values[idx] == 0U)
        return def;
    return t->csi_values[idx];
}

static void term_apply_sgr(term_state_t *t, uint32_t p)
{
    if (!t)
        return;
    switch (p) {
    case 0U:
        term_reset_style(t);
        break;
    case 1U:
        if (t->cur_fg < 8U)
            t->cur_fg = (uint8_t)(t->cur_fg + 8U);
        break;
    case 7U:
        t->cur_attr |= TERM_ATTR_REVERSE;
        break;
    case 27U:
        t->cur_attr &= (uint8_t)~TERM_ATTR_REVERSE;
        break;
    case 39U:
        t->cur_fg = 7U;
        break;
    case 49U:
        t->cur_bg = 0U;
        break;
    default:
        if (p >= 30U && p <= 37U)
            t->cur_fg = (uint8_t)(p - 30U);
        else if (p >= 40U && p <= 47U)
            t->cur_bg = (uint8_t)(p - 40U);
        else if (p >= 90U && p <= 97U)
            t->cur_fg = (uint8_t)(8U + (p - 90U));
        else if (p >= 100U && p <= 107U)
            t->cur_bg = (uint8_t)(8U + (p - 100U));
        break;
    }
}

static void term_finish_csi(term_state_t *t, uint8_t final)
{
    uint32_t p0;

    if (!t)
        return;
    if (t->csi_have_value || t->csi_count == 0U) {
        if (t->csi_count < 8U)
            t->csi_values[t->csi_count++] = t->csi_value;
    }

    p0 = term_csi_value_or(t, 0U, 1U);
    switch (final) {
    case 'A':
        if (p0 > t->cursor_y)
            t->cursor_y = 0U;
        else
            t->cursor_y -= p0;
        break;
    case 'B':
        t->cursor_y += p0;
        if (t->cursor_y >= TERM_ROWS)
            t->cursor_y = TERM_ROWS - 1U;
        break;
    case 'C':
        t->cursor_x += p0;
        if (t->cursor_x >= TERM_COLS)
            t->cursor_x = TERM_COLS - 1U;
        break;
    case 'D':
        if (p0 > t->cursor_x)
            t->cursor_x = 0U;
        else
            t->cursor_x -= p0;
        break;
    case 'G':
        t->cursor_x = (p0 > 0U) ? (p0 - 1U) : 0U;
        if (t->cursor_x >= TERM_COLS)
            t->cursor_x = TERM_COLS - 1U;
        break;
    case 'H':
    case 'f': {
        uint32_t row = term_csi_value_or(t, 0U, 1U);
        uint32_t col = term_csi_value_or(t, 1U, 1U);
        t->cursor_y = (row > 0U) ? (row - 1U) : 0U;
        t->cursor_x = (col > 0U) ? (col - 1U) : 0U;
        if (t->cursor_y >= TERM_ROWS)
            t->cursor_y = TERM_ROWS - 1U;
        if (t->cursor_x >= TERM_COLS)
            t->cursor_x = TERM_COLS - 1U;
        break;
    }
    case 'J':
        if (p0 == 2U) {
            for (uint32_t row = 0; row < TERM_ROWS; row++)
                term_clear_cell_row(t, row, 0U, TERM_COLS);
            t->cursor_x = 0U;
            t->cursor_y = 0U;
        } else {
            term_clear_cell_row(t, t->cursor_y, t->cursor_x, TERM_COLS);
            for (uint32_t row = t->cursor_y + 1U; row < TERM_ROWS; row++)
                term_clear_cell_row(t, row, 0U, TERM_COLS);
        }
        break;
    case 'K':
        if (p0 == 1U) {
            term_clear_cell_row(t, t->cursor_y, 0U, t->cursor_x + 1U);
        } else if (p0 == 2U) {
            term_clear_cell_row(t, t->cursor_y, 0U, TERM_COLS);
        } else {
            term_clear_cell_row(t, t->cursor_y, t->cursor_x, TERM_COLS);
        }
        break;
    case 'm':
        for (uint32_t i = 0U; i < t->csi_count; i++)
            term_apply_sgr(t, t->csi_values[i]);
        break;
    case 's':
        t->saved_x = t->cursor_x;
        t->saved_y = t->cursor_y;
        break;
    case 'u':
        t->cursor_x = t->saved_x;
        t->cursor_y = t->saved_y;
        if (t->cursor_x >= TERM_COLS)
            t->cursor_x = TERM_COLS - 1U;
        if (t->cursor_y >= TERM_ROWS)
            t->cursor_y = TERM_ROWS - 1U;
        break;
    default:
        break;
    }

    t->parse_state = TERM_STATE_NORMAL;
    t->csi_priv = 0U;
    t->csi_count = 0U;
    t->csi_value = 0U;
    t->csi_have_value = 0U;
    term_mark_dirty(t);
}

static void term_feed_byte(term_state_t *t, uint8_t b)
{
    if (!t)
        return;

    switch (t->parse_state) {
    case TERM_STATE_NORMAL:
        switch (b) {
        case 0x00U:
        case 0x07U:
            break;
        case '\r':
            term_carriage_return(t);
            break;
        case '\n':
            term_newline(t);
            break;
        case '\b':
            term_backspace(t);
            break;
        case '\t':
            term_tab(t);
            break;
        case 0x1BU:
            t->parse_state = TERM_STATE_ESC;
            break;
        default:
            if (b >= 32U && b <= 126U)
                term_put_char(t, b);
            else if (b >= 0x80U)
                term_put_char(t, (uint8_t)'?');
            break;
        }
        break;

    case TERM_STATE_ESC:
        if (b == '[') {
            t->parse_state = TERM_STATE_CSI;
            t->csi_priv = 0U;
            t->csi_count = 0U;
            t->csi_value = 0U;
            t->csi_have_value = 0U;
            memset(t->csi_values, 0, sizeof(t->csi_values));
        } else {
            t->parse_state = TERM_STATE_NORMAL;
            if (b == 'c')
                term_reset(t);
        }
        break;

    case TERM_STATE_CSI:
        if (b == '?') {
            t->csi_priv = 1U;
            break;
        }
        if (b >= '0' && b <= '9') {
            t->csi_value = t->csi_value * 10U + (uint32_t)(b - '0');
            t->csi_have_value = 1U;
            break;
        }
        if (b == ';') {
            if (t->csi_count < 8U)
                t->csi_values[t->csi_count++] = t->csi_have_value ? t->csi_value : 0U;
            t->csi_value = 0U;
            t->csi_have_value = 0U;
            break;
        }
        term_finish_csi(t, b);
        break;
    }
}

static void term_feed_bytes(term_state_t *t, const uint8_t *buf, size_t len)
{
    if (!t || !buf)
        return;
    for (size_t i = 0; i < len; i++)
        term_feed_byte(t, buf[i]);
}

static const uint8_t *term_glyph_for(uint8_t ch)
{
    if (ch >= 32U && ch <= 126U)
        return font_8x16[ch - 32U];
    return font_8x16['?' - 32U];
}

static void fill_rect(uint32_t *dst, int x, int y, int w, int h, uint32_t color)
{
    for (int yy = 0; yy < h; yy++) {
        int py = y + yy;
        if (py < 0 || py >= SURF_H)
            continue;
        for (int xx = 0; xx < w; xx++) {
            int px = x + xx;
            if (px < 0 || px >= SURF_W)
                continue;
            dst[py * SURF_W + px] = color;
        }
    }
}

static void draw_cell(uint32_t *dst, uint32_t cx, uint32_t cy, const term_cell_t *cell, int cursor_here)
{
    uint32_t fg;
    uint32_t bg;
    const uint8_t *glyph;
    uint8_t ch;

    if (!dst || !cell || cx >= TERM_COLS || cy >= TERM_ROWS)
        return;

    fg = term_palette[cell->fg & 0x0FU];
    bg = term_palette[cell->bg & 0x0FU];
    if ((cell->attr & TERM_ATTR_REVERSE) != 0U || cursor_here) {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }

    fill_rect(dst, (int)(cx * CELL_W), (int)(cy * CELL_H), CELL_W, CELL_H, bg);

    ch = cell->ch;
    if (ch == 0U)
        ch = ' ';
    glyph = term_glyph_for(ch);
    for (uint32_t row = 0U; row < CELL_H; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0U; col < CELL_W; col++) {
            if ((bits & (uint8_t)(1U << (7U - col))) == 0U)
                continue;
            dst[(cy * CELL_H + row) * SURF_W + (cx * CELL_W + col)] = fg;
        }
    }
}

static void term_render(app_t *app)
{
    if (!app || !app->buf)
        return;
    fill_rect(app->buf, 0, 0, SURF_W, SURF_H, term_palette[0]);
    for (uint32_t row = 0U; row < TERM_ROWS; row++) {
        for (uint32_t col = 0U; col < TERM_COLS; col++) {
            int cursor_here = (row == app->term.cursor_y && col == app->term.cursor_x);
            draw_cell(app->buf, col, row, &app->term.cells[row][col], cursor_here);
        }
    }
    app->term.dirty = 0U;
}

static void wl_present_surface(app_t *app)
{
    if (!app)
        return;

    msg_begin(&app->conn, OBJ_SURFACE, WL_SURFACE_ATTACH, 12U);
    out_u32(&app->conn, OBJ_BUFFER);
    out_u32(&app->conn, 0U);
    out_u32(&app->conn, 0U);

    msg_begin(&app->conn, OBJ_SURFACE, WL_SURFACE_DAMAGE, 16U);
    out_u32(&app->conn, 0U);
    out_u32(&app->conn, 0U);
    out_u32(&app->conn, SURF_W);
    out_u32(&app->conn, SURF_H);

    msg_begin(&app->conn, OBJ_SURFACE, WL_SURFACE_COMMIT, 0U);
    wl_flush(&app->conn);
}

static void app_queue_input(app_t *app, const uint8_t *buf, uint32_t len)
{
    uint32_t space;

    if (!app || !buf || len == 0U)
        return;
    if (len > INPUT_QUEUE_MAX)
        len = INPUT_QUEUE_MAX;
    space = INPUT_QUEUE_MAX - app->input_len;
    if (len > space)
        len = space;
    if (len == 0U)
        return;
    memcpy(app->input_q + app->input_len, buf, len);
    app->input_len += len;
}

static void app_drain_wayland(app_t *app)
{
    wl_event_t ev;
    ssize_t    n;

    if (!app)
        return;
    do {
        n = recv(app->conn.fd,
                 app->conn.ibuf + app->conn.ilen,
                 sizeof(app->conn.ibuf) - app->conn.ilen,
                 MSG_DONTWAIT);
        if (n > 0)
            app->conn.ilen += (uint32_t)n;
    } while (n > 0);

    while (wl_next_event(&app->conn, &ev)) {
        if (ev.obj_id == OBJ_XDG_SURFACE && ev.opcode == XDG_SURFACE_CONFIGURE && ev.plen >= 4U) {
            app->serial = in_u32(ev.payload);
            app->configured = 1;
            msg_begin(&app->conn, OBJ_XDG_SURFACE, XDG_SURFACE_ACK_CONFIGURE, 4U);
            out_u32(&app->conn, app->serial);
            wl_flush(&app->conn);
            /*
             * xdg-shell richiede un commit dopo l'ack del configure.
             * Anche se il terminale non e' "dirty", qui dobbiamo presentare
             * comunque almeno un frame, altrimenti la superficie puo' restare
             * invisibile e dare l'impressione di schermo nero / freeze.
             */
            term_render(app);
            wl_present_surface(app);
        } else if (ev.obj_id == OBJ_XDG_TOPLEVEL && ev.opcode == XDG_TOPLEVEL_CLOSE) {
            app->close_requested = 1;
            app->running = 0;
        } else if (ev.obj_id == OBJ_ENLIL_TERM && ev.opcode == ENLIL_TERM_INPUT && ev.plen >= 4U) {
            uint32_t len = in_u32(ev.payload);
            if (len > ev.plen - 4U)
                len = ev.plen - 4U;
            app_queue_input(app, ev.payload + 4U, len);
        }
    }
}

static void app_flush_pty_input(app_t *app)
{
    ssize_t n;

    if (!app || app->master_fd < 0 || app->input_len == 0U)
        return;
    n = write(app->master_fd, app->input_q, app->input_len);
    if (n > 0) {
        if ((uint32_t)n < app->input_len)
            memmove(app->input_q, app->input_q + n, app->input_len - (uint32_t)n);
        app->input_len -= (uint32_t)n;
    }
}

static void app_drain_pty_output(app_t *app)
{
    uint8_t buf[256];

    if (!app || app->master_fd < 0)
        return;
    for (;;) {
        ssize_t n = read(app->master_fd, buf, sizeof(buf));
        if (n <= 0)
            break;
        term_feed_bytes(&app->term, buf, (size_t)n);
    }
}

static int app_spawn_bash(app_t *app)
{
    struct winsize ws;
    int            master_fd;
    int            slave_fd;
    int            one = 1;
    pid_t          pid;

    if (!app)
        return -1;

    memset(&ws, 0, sizeof(ws));
    ws.ws_row = TERM_ROWS;
    ws.ws_col = TERM_COLS;

    if (openpty(&master_fd, &slave_fd, NULL, NULL, &ws) < 0)
        return -1;

    (void)ioctl(master_fd, FIONBIO, &one);

    pid = fork();
    if (pid < 0) {
        close(master_fd);
        close(slave_fd);
        return -1;
    }

    if (pid == 0) {
        close(master_fd);
        (void)setsid();
        (void)ioctl(slave_fd, TIOCSCTTY, 0);
        (void)setpgid(0, 0);
        (void)dup2(slave_fd, STDIN_FILENO);
        (void)dup2(slave_fd, STDOUT_FILENO);
        (void)dup2(slave_fd, STDERR_FILENO);
        if (slave_fd > STDERR_FILENO)
            close(slave_fd);
        (void)tcsetpgrp(STDIN_FILENO, getpid());
        (void)setenv("TERM", "vt100", 1);
        (void)setenv("COLORTERM", "enlilos", 1);
        (void)setenv("COLUMNS", "80", 1);
        (void)setenv("LINES", "24", 1);
        /* Try shells in order: native bash → bash-linux → nsh → arksh */
        {
            static const char * const shells[] = {
                "/bin/bash", "/data/bash-linux", "/bin/nsh", "/bin/arksh", NULL
            };
            static const char * const names[] = {
                "bash", "bash", "nsh", "arksh", NULL
            };
            int i;
            for (i = 0; shells[i] != NULL; i++) {
                char *const argv_sh[] = { (char *)(uintptr_t)names[i], "-i", NULL };
                execve(shells[i], argv_sh, environ);
            }
        }
        write(STDERR_FILENO, "wterm: no shell found\n", 22);
        _exit(127);
    }

    close(slave_fd);
    app->master_fd = master_fd;
    app->child_pid = pid;
    return 0;
}

static void app_kill_child(app_t *app)
{
    int status = 0;

    if (!app || app->child_pid <= 0)
        return;

    (void)killpg(app->child_pid, SIGHUP);
    for (int i = 0; i < 50; i++) {
        pid_t rc = waitpid(app->child_pid, &status, WNOHANG);
        if (rc == app->child_pid) {
            app->child_status = status;
            app->child_pid = 0;
            return;
        }
        sleep_10ms();
    }
    (void)killpg(app->child_pid, SIGKILL);
    (void)waitpid(app->child_pid, &status, 0);
    app->child_status = status;
    app->child_pid = 0;
}

static int app_setup_surface(app_t *app)
{
    if (!app)
        return -1;

    app->shmid = shmget(IPC_PRIVATE, SURF_SZ, IPC_CREAT | 0600);
    if (app->shmid < 0)
        return -1;
    app->buf = (uint32_t *)shmat(app->shmid, NULL, 0);
    if ((long)app->buf == -1L) {
        app->buf = NULL;
        return -1;
    }

    bind_global(&app->conn, app->comp_name, "wl_compositor", 4U, OBJ_COMPOSITOR);
    bind_global(&app->conn, app->shm_name, "wl_shm", 1U, OBJ_SHM);
    bind_global(&app->conn, app->xdg_name, "xdg_wm_base", 1U, OBJ_XDG_WM);
    bind_global(&app->conn, app->term_name, "enlil_term_v1", 1U, OBJ_ENLIL_TERM);

    msg_begin(&app->conn, OBJ_SHM, WL_SHM_CREATE_POOL_SYSV, 12U);
    out_u32(&app->conn, OBJ_SHM_POOL);
    out_u32(&app->conn, (uint32_t)app->shmid);
    out_u32(&app->conn, SURF_SZ);
    wl_flush(&app->conn);

    msg_begin(&app->conn, OBJ_SHM_POOL, WL_SHM_POOL_CREATE_BUFFER, 24U);
    out_u32(&app->conn, OBJ_BUFFER);
    out_u32(&app->conn, 0U);
    out_u32(&app->conn, SURF_W);
    out_u32(&app->conn, SURF_H);
    out_u32(&app->conn, SURF_W * 4U);
    out_u32(&app->conn, 1U);
    wl_flush(&app->conn);

    msg_begin(&app->conn, OBJ_COMPOSITOR, WL_COMPOSITOR_CREATE_SURFACE, 4U);
    out_u32(&app->conn, OBJ_SURFACE);
    wl_flush(&app->conn);

    msg_begin(&app->conn, OBJ_XDG_WM, XDG_WM_GET_XDG_SURFACE, 8U);
    out_u32(&app->conn, OBJ_XDG_SURFACE);
    out_u32(&app->conn, OBJ_SURFACE);
    wl_flush(&app->conn);

    msg_begin(&app->conn, OBJ_XDG_SURFACE, XDG_SURFACE_GET_TOPLEVEL, 4U);
    out_u32(&app->conn, OBJ_XDG_TOPLEVEL);
    wl_flush(&app->conn);

    msg_begin(&app->conn, OBJ_XDG_TOPLEVEL, XDG_TOP_SET_TITLE,
              4U + (((uint32_t)strlen("Enlil Terminal") + 1U + 3U) & ~3U));
    out_str(&app->conn, "Enlil Terminal");
    wl_flush(&app->conn);

    msg_begin(&app->conn, OBJ_ENLIL_TERM, ENLIL_TERM_ATTACH_SURFACE, 4U);
    out_u32(&app->conn, OBJ_SURFACE);
    wl_flush(&app->conn);

    msg_begin(&app->conn, OBJ_SURFACE, WL_SURFACE_COMMIT, 0U);
    wl_flush(&app->conn);
    return 0;
}

static int app_wait_initial_configure(app_t *app)
{
    if (!app)
        return -1;
    for (int polls = 400; polls-- > 0 && !app->configured; ) {
        app_drain_wayland(app);
        if (!app->configured)
            sleep_10ms();
    }
    return app->configured ? 0 : -1;
}

static void app_cleanup(app_t *app)
{
    if (!app)
        return;

    if (app->child_pid > 0)
        app_kill_child(app);
    if (app->master_fd >= 0) {
        close(app->master_fd);
        app->master_fd = -1;
    }
    if (app->conn.fd >= 0) {
        close(app->conn.fd);
        app->conn.fd = -1;
    }
    if (app->buf) {
        shmdt(app->buf);
        app->buf = NULL;
    }
    if (app->shmid >= 0) {
        (void)shmctl(app->shmid, IPC_RMID, NULL);
        app->shmid = -1;
    }
}

int main(int argc, char **argv)
{
    app_t app;
    int   session_mode = 0;

    memset(&app, 0, sizeof(app));
    app.conn.fd = -1;
    app.master_fd = -1;
    app.child_pid = -1;
    app.shmid = -1;
    app.running = 1;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--session") == 0)
            session_mode = 1;
    }
    if (session_mode) {
        (void)unlink(WTERM_READY_PATH);
        (void)unlink(WTERM_FAIL_PATH);
    }

    if (wl_connect_runtime(&app) < 0) {
        if (session_mode)
            write_marker_file(WTERM_FAIL_PATH, "connect\n");
        write(STDERR_FILENO, "wterm: connect fail\n", 20);
        return 1;
    }
    if (app_setup_surface(&app) < 0) {
        if (session_mode)
            write_marker_file(WTERM_FAIL_PATH, "surface\n");
        write(STDERR_FILENO, "wterm: surface setup fail\n", 26);
        app_cleanup(&app);
        return 2;
    }

    term_reset(&app.term);
    term_feed_bytes(&app.term, (const uint8_t *)"Starting shell...\r\n", 19U);
    term_render(&app);
    wl_present_surface(&app);

    if (app_wait_initial_configure(&app) < 0) {
        if (session_mode)
            write_marker_file(WTERM_FAIL_PATH, "configure\n");
        write(STDERR_FILENO, "wterm: configure timeout\n", 25);
        app_cleanup(&app);
        return 3;
    }

    if (app_spawn_bash(&app) < 0) {
        term_feed_bytes(&app.term, (const uint8_t *)"Failed to start shell\r\n", 23U);
        term_render(&app);
        wl_present_surface(&app);
        if (session_mode)
            write_marker_file(WTERM_FAIL_PATH, "spawn\n");
        sleep_10ms();
        app_cleanup(&app);
        return 4;
    }

    if (session_mode)
        write_marker_file(WTERM_READY_PATH, "ready\n");

    while (app.running) {
        int status = 0;
        pid_t rc;

        app_drain_wayland(&app);
        app_flush_pty_input(&app);
        app_drain_pty_output(&app);

        rc = waitpid(app.child_pid, &status, WNOHANG);
        if (rc == app.child_pid) {
            app.child_status = status;
            app.child_pid = 0;
            app.running = 0;
        }

        if (app.term.dirty != 0U) {
            term_render(&app);
            wl_present_surface(&app);
        }
        if (app.close_requested)
            break;
        sleep_10ms();
    }

    app_cleanup(&app);
    return 0;
}
