/*
 * wterm_demo — placeholder terminal window for M12-02 global shortcut
 *
 * Crea una singola finestra Wayland con look "terminal-style". In modalita'
 * normale resta attiva fino a close dal WM oppure timeout dimostrativo; con
 * --session resta aperta finche' il WM non la chiude esplicitamente.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#define SURF_W  520
#define SURF_H  320
#define SURF_SZ (SURF_W * SURF_H * 4)

#define WL_DISPLAY_GET_REGISTRY   1U
#define WL_REGISTRY_BIND          0U
#define WL_REGISTRY_GLOBAL        0U
#define WL_COMPOSITOR_CREATE_SURFACE 0U
#define WL_SHM_CREATE_POOL_SYSV   0U
#define WL_SHM_POOL_CREATE_BUFFER 0U
#define WL_SURFACE_ATTACH         1U
#define WL_SURFACE_DAMAGE         2U
#define WL_SURFACE_COMMIT         6U
#define XDG_WM_GET_XDG_SURFACE    2U
#define XDG_SURFACE_GET_TOPLEVEL  1U
#define XDG_SURFACE_ACK_CONFIGURE 4U
#define XDG_TOP_SET_TITLE         2U
#define XDG_SURFACE_CONFIGURE     0U
#define XDG_TOPLEVEL_CLOSE        1U

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

static void wl_flush(wl_conn_t *c)
{
    ssize_t sent = 0;
    while ((uint32_t)sent < c->olen) {
        ssize_t r = send(c->fd, c->obuf + sent, c->olen - (uint32_t)sent, 0);
        if (r <= 0)
            break;
        sent += r;
    }
    c->olen = 0U;
}

static void out_u32(wl_conn_t *c, uint32_t v)
{
    if (c->olen + 4U > sizeof(c->obuf))
        return;
    c->obuf[c->olen + 0U] = (uint8_t)v;
    c->obuf[c->olen + 1U] = (uint8_t)(v >> 8);
    c->obuf[c->olen + 2U] = (uint8_t)(v >> 16);
    c->obuf[c->olen + 3U] = (uint8_t)(v >> 24);
    c->olen += 4U;
}

static uint32_t in_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static void out_str(wl_conn_t *c, const char *s)
{
    uint32_t slen = (uint32_t)(strlen(s) + 1U);
    uint32_t plen = (slen + 3U) & ~3U;

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

static int wl_next_event(wl_conn_t *c, wl_event_t *ev)
{
    uint32_t hdr2;
    uint32_t size;

    if (c->ilen < 8U)
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

static void sleep_10ms(void)
{
    struct timespec sl = { 0, 10000000L };
    nanosleep(&sl, NULL);
}

static int wl_connect_runtime(wl_conn_t *conn, uint32_t *comp_name,
                              uint32_t *shm_name, uint32_t *xdg_name)
{
    struct sockaddr_un sun;

    memset(conn, 0, sizeof(*conn));
    conn->fd = -1;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "/run/wayland-0");

    conn->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (conn->fd < 0)
        return -1;
    for (int t = 0; t < 30; t++) {
        if (connect(conn->fd, (struct sockaddr *)&sun, sizeof(sun)) == 0)
            break;
        if (t == 29)
            return -1;
        sleep_10ms();
    }

    msg_begin(conn, 1U, WL_DISPLAY_GET_REGISTRY, 4U);
    out_u32(conn, 2U);
    wl_flush(conn);

    *comp_name = *shm_name = *xdg_name = 0U;
    for (int t = 0; t < 100; t++) {
        wl_event_t ev;
        ssize_t n = recv(conn->fd, conn->ibuf + conn->ilen,
                         sizeof(conn->ibuf) - conn->ilen, MSG_DONTWAIT);
        if (n > 0)
            conn->ilen += (uint32_t)n;

        while (wl_next_event(conn, &ev)) {
            if (ev.obj_id == 2U && ev.opcode == WL_REGISTRY_GLOBAL && ev.plen >= 12U) {
                uint32_t name = in_u32(ev.payload);
                uint32_t slen = in_u32(ev.payload + 4U);
                char iface[64];
                uint32_t copy = (slen < sizeof(iface)) ? slen : (uint32_t)sizeof(iface) - 1U;

                memcpy(iface, ev.payload + 8U, copy);
                iface[copy] = '\0';
                if (strcmp(iface, "wl_compositor") == 0)
                    *comp_name = name;
                else if (strcmp(iface, "wl_shm") == 0)
                    *shm_name = name;
                else if (strcmp(iface, "xdg_wm_base") == 0)
                    *xdg_name = name;
            }
        }

        if (*comp_name && *shm_name && *xdg_name)
            return 0;
        sleep_10ms();
    }
    return -1;
}

static void bind_global(wl_conn_t *conn, uint32_t name,
                        const char *iface, uint32_t version, uint32_t new_id)
{
    msg_begin(conn, 2U, WL_REGISTRY_BIND,
              4U + 4U + (((uint32_t)strlen(iface) + 1U + 3U) & ~3U) + 4U + 4U);
    out_u32(conn, name);
    out_str(conn, iface);
    out_u32(conn, version);
    out_u32(conn, new_id);
    wl_flush(conn);
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

static uint8_t glyph_row(char c, int row)
{
    switch (c) {
    case 'A': { static const uint8_t g[7] = { 0x0EU,0x11U,0x11U,0x1FU,0x11U,0x11U,0x11U }; return g[row]; }
    case 'B': { static const uint8_t g[7] = { 0x1EU,0x11U,0x11U,0x1EU,0x11U,0x11U,0x1EU }; return g[row]; }
    case 'C': { static const uint8_t g[7] = { 0x0EU,0x11U,0x10U,0x10U,0x10U,0x11U,0x0EU }; return g[row]; }
    case 'D': { static const uint8_t g[7] = { 0x1CU,0x12U,0x11U,0x11U,0x11U,0x12U,0x1CU }; return g[row]; }
    case 'E': { static const uint8_t g[7] = { 0x1FU,0x10U,0x10U,0x1EU,0x10U,0x10U,0x1FU }; return g[row]; }
    case 'F': { static const uint8_t g[7] = { 0x1FU,0x10U,0x10U,0x1EU,0x10U,0x10U,0x10U }; return g[row]; }
    case 'G': { static const uint8_t g[7] = { 0x0EU,0x11U,0x10U,0x17U,0x11U,0x11U,0x0FU }; return g[row]; }
    case 'H': { static const uint8_t g[7] = { 0x11U,0x11U,0x11U,0x1FU,0x11U,0x11U,0x11U }; return g[row]; }
    case 'I': { static const uint8_t g[7] = { 0x1FU,0x04U,0x04U,0x04U,0x04U,0x04U,0x1FU }; return g[row]; }
    case 'L': { static const uint8_t g[7] = { 0x10U,0x10U,0x10U,0x10U,0x10U,0x10U,0x1FU }; return g[row]; }
    case 'M': { static const uint8_t g[7] = { 0x11U,0x1BU,0x15U,0x15U,0x11U,0x11U,0x11U }; return g[row]; }
    case 'N': { static const uint8_t g[7] = { 0x11U,0x19U,0x15U,0x13U,0x11U,0x11U,0x11U }; return g[row]; }
    case 'O': { static const uint8_t g[7] = { 0x0EU,0x11U,0x11U,0x11U,0x11U,0x11U,0x0EU }; return g[row]; }
    case 'P': { static const uint8_t g[7] = { 0x1EU,0x11U,0x11U,0x1EU,0x10U,0x10U,0x10U }; return g[row]; }
    case 'Q': { static const uint8_t g[7] = { 0x0EU,0x11U,0x11U,0x11U,0x15U,0x12U,0x0DU }; return g[row]; }
    case 'R': { static const uint8_t g[7] = { 0x1EU,0x11U,0x11U,0x1EU,0x14U,0x12U,0x11U }; return g[row]; }
    case 'S': { static const uint8_t g[7] = { 0x0FU,0x10U,0x10U,0x0EU,0x01U,0x01U,0x1EU }; return g[row]; }
    case 'T': { static const uint8_t g[7] = { 0x1FU,0x04U,0x04U,0x04U,0x04U,0x04U,0x04U }; return g[row]; }
    case 'U': { static const uint8_t g[7] = { 0x11U,0x11U,0x11U,0x11U,0x11U,0x11U,0x0EU }; return g[row]; }
    case 'W': { static const uint8_t g[7] = { 0x11U,0x11U,0x11U,0x15U,0x15U,0x15U,0x0AU }; return g[row]; }
    case 'Y': { static const uint8_t g[7] = { 0x11U,0x11U,0x0AU,0x04U,0x04U,0x04U,0x04U }; return g[row]; }
    case '+': { static const uint8_t g[7] = { 0x00U,0x04U,0x04U,0x1FU,0x04U,0x04U,0x00U }; return g[row]; }
    case '-': { static const uint8_t g[7] = { 0x00U,0x00U,0x00U,0x1FU,0x00U,0x00U,0x00U }; return g[row]; }
    case '/': { static const uint8_t g[7] = { 0x01U,0x02U,0x04U,0x08U,0x10U,0x00U,0x00U }; return g[row]; }
    case ':': { static const uint8_t g[7] = { 0x00U,0x04U,0x00U,0x00U,0x04U,0x00U,0x00U }; return g[row]; }
    case '0': { static const uint8_t g[7] = { 0x0EU,0x11U,0x13U,0x15U,0x19U,0x11U,0x0EU }; return g[row]; }
    case '1': { static const uint8_t g[7] = { 0x04U,0x0CU,0x04U,0x04U,0x04U,0x04U,0x0EU }; return g[row]; }
    case '2': { static const uint8_t g[7] = { 0x0EU,0x11U,0x01U,0x02U,0x04U,0x08U,0x1FU }; return g[row]; }
    case '3': { static const uint8_t g[7] = { 0x1EU,0x01U,0x01U,0x0EU,0x01U,0x01U,0x1EU }; return g[row]; }
    case ' ': default: return 0x00U;
    }
}

static void draw_text(uint32_t *dst, int x, int y, const char *text,
                      uint32_t fg, uint32_t bg)
{
    for (size_t i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 'A');
        fill_rect(dst, x + (int)(i * 14), y, 12, 18, bg);
        for (int row = 0; row < 7; row++) {
            uint8_t bits = glyph_row(c, row);
            for (int col = 0; col < 5; col++) {
                if (!(bits & (uint8_t)(1U << (4 - col))))
                    continue;
                fill_rect(dst,
                          x + (int)(i * 14) + col * 2,
                          y + row * 2,
                          2, 2, fg);
            }
        }
    }
}

static void fill_terminal_pattern(uint32_t *dst)
{
    fill_rect(dst, 0, 0, SURF_W, SURF_H, 0x000B120DU);
    fill_rect(dst, 0, 0, SURF_W, 28, 0x00111D2BU);
    fill_rect(dst, 0, 28, SURF_W, 2, 0x00243B54U);

    draw_text(dst, 18, 8,  "ENLILOS WAYLAND SESSION", 0x00D6F3FFU, 0x00111D2BU);
    draw_text(dst, 24, 52, "PLACEHOLDER WINDOW",      0x00FFFFFFU, 0x000B120DU);
    draw_text(dst, 24, 78, "NOT AN INTERACTIVE SHELL",0x00FCD34DU, 0x000B120DU);
    draw_text(dst, 24, 110,"CTRL+N OR SUPER+ENTER",   0x00A5F28BU, 0x000B120DU);
    draw_text(dst, 24, 136,"CTRL+W OR ESC CLOSE",     0x00A5F28BU, 0x000B120DU);
    draw_text(dst, 24, 162,"TAB CHANGES FOCUS",       0x00A5F28BU, 0x000B120DU);
    draw_text(dst, 24, 188,"CTRL+C RETURNS TO BOOT",  0x00F87171U, 0x000B120DU);

    fill_rect(dst, 20, 228, SURF_W - 40, 58, 0x00070D09U);
    fill_rect(dst, 20, 228, 4, 58, 0x001CD870U);
    draw_text(dst, 32, 240, "WTERM PLACEHOLDER",     0x00FFFFFFU, 0x00070D09U);
    draw_text(dst, 32, 264, "PTY GUI TERMINAL NEXT", 0x0097E3FFU, 0x00070D09U);

    fill_rect(dst, 30, SURF_H - 28, 10, 16, 0x00FFFFFFU);
}

int main(int argc, char **argv)
{
    wl_conn_t conn;
    uint32_t comp_name, shm_name, xdg_name;
    uint32_t *buf = NULL;
    int shmid = -1;
    uint32_t serial = 0U;
    int configured = 0;
    int session_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--session") == 0)
            session_mode = 1;
    }

    if (wl_connect_runtime(&conn, &comp_name, &shm_name, &xdg_name) < 0) {
        write(2, "wterm: connect fail\n", 20);
        return 1;
    }

    bind_global(&conn, comp_name, "wl_compositor", 4U, 3U);
    bind_global(&conn, shm_name, "wl_shm", 1U, 4U);
    bind_global(&conn, xdg_name, "xdg_wm_base", 1U, 5U);

    shmid = shmget(IPC_PRIVATE, SURF_SZ, IPC_CREAT | 0600);
    if (shmid < 0)
        return 2;
    buf = (uint32_t *)shmat(shmid, NULL, 0);
    if ((long)buf == -1L)
        return 3;
    fill_terminal_pattern(buf);

    msg_begin(&conn, 4U, WL_SHM_CREATE_POOL_SYSV, 12U);
    out_u32(&conn, 6U);
    out_u32(&conn, (uint32_t)shmid);
    out_u32(&conn, SURF_SZ);
    wl_flush(&conn);

    msg_begin(&conn, 6U, WL_SHM_POOL_CREATE_BUFFER, 24U);
    out_u32(&conn, 7U);
    out_u32(&conn, 0U);
    out_u32(&conn, SURF_W);
    out_u32(&conn, SURF_H);
    out_u32(&conn, SURF_W * 4U);
    out_u32(&conn, 1U);
    wl_flush(&conn);

    msg_begin(&conn, 3U, WL_COMPOSITOR_CREATE_SURFACE, 4U);
    out_u32(&conn, 8U);
    wl_flush(&conn);

    msg_begin(&conn, 5U, XDG_WM_GET_XDG_SURFACE, 8U);
    out_u32(&conn, 9U);
    out_u32(&conn, 8U);
    wl_flush(&conn);

    msg_begin(&conn, 9U, XDG_SURFACE_GET_TOPLEVEL, 4U);
    out_u32(&conn, 10U);
    wl_flush(&conn);

    msg_begin(&conn, 10U, XDG_TOP_SET_TITLE,
              4U + (((uint32_t)strlen("Terminal") + 1U + 3U) & ~3U));
    out_str(&conn, "Terminal");
    wl_flush(&conn);

    msg_begin(&conn, 8U, WL_SURFACE_COMMIT, 0U);
    wl_flush(&conn);

    for (int polls = 400; polls-- > 0 && !configured; ) {
        wl_event_t ev;
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0)
            conn.ilen += (uint32_t)n;

        while (wl_next_event(&conn, &ev)) {
            if (ev.obj_id == 9U && ev.opcode == XDG_SURFACE_CONFIGURE && ev.plen >= 4U) {
                serial = in_u32(ev.payload);
                configured = 1;
            }
        }
        if (!configured)
            sleep_10ms();
    }

    if (!configured)
        return 4;

    msg_begin(&conn, 9U, XDG_SURFACE_ACK_CONFIGURE, 4U);
    out_u32(&conn, serial);
    wl_flush(&conn);

    msg_begin(&conn, 8U, WL_SURFACE_ATTACH, 12U);
    out_u32(&conn, 7U);
    out_u32(&conn, 0U);
    out_u32(&conn, 0U);

    msg_begin(&conn, 8U, WL_SURFACE_DAMAGE, 16U);
    out_u32(&conn, 0U);
    out_u32(&conn, 0U);
    out_u32(&conn, SURF_W);
    out_u32(&conn, SURF_H);

    msg_begin(&conn, 8U, WL_SURFACE_COMMIT, 0U);
    wl_flush(&conn);

    for (int polls = session_mode ? -1 : 3000; polls != 0; ) {
        wl_event_t ev;
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0)
            conn.ilen += (uint32_t)n;

        while (wl_next_event(&conn, &ev)) {
            if (ev.obj_id == 10U && ev.opcode == XDG_TOPLEVEL_CLOSE) {
                shmdt(buf);
                close(conn.fd);
                return 0;
            }
        }
        if (polls > 0)
            polls--;
        sleep_10ms();
    }

    shmdt(buf);
    close(conn.fd);
    return 0;
}
