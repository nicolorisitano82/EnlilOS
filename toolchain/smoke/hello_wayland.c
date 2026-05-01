#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
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
#define XDG_TOP_SET_APP_ID           3U
#define XDG_SURFACE_CONFIGURE        0U
#define XDG_TOPLEVEL_CLOSE           1U
#define SYS_CACHE_FLUSH              228U

#define SURF_W  360
#define SURF_H  220
#define SURF_SZ (SURF_W * SURF_H * 4)

#define HELLO_READY_PATH "/data/HELLOREADY.TXT"
#define HELLO_FAIL_PATH  "/data/HELLOFAIL.TXT"

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

static long raw_cache_flush(void *ptr, uint32_t size)
{
    register long x0 __asm__("x0") = (long)ptr;
    register long x1 __asm__("x1") = (long)size;
    register long x8 __asm__("x8") = SYS_CACHE_FLUSH;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x8)
                     : "memory");
    return x0;
}

static void flush_shm_pixels(void *ptr, uint32_t size)
{
    if (!ptr || size == 0U)
        return;
    (void)raw_cache_flush(ptr, size);
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

static void draw_glyph(uint32_t *dst, int x, int y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph;

    if (!dst)
        return;
    if (ch < 32 || ch > 126)
        ch = '?';
    glyph = font_8x16[(uint8_t)ch - 32U];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            int py = y + row;
            if (px < 0 || py < 0 || px >= SURF_W || py >= SURF_H)
                continue;
            dst[py * SURF_W + px] = (bits & (uint8_t)(1U << (7 - col))) ? fg : bg;
        }
    }
}

static void draw_text(uint32_t *dst, int x, int y, const char *s, uint32_t fg, uint32_t bg)
{
    if (!dst || !s)
        return;
    while (*s) {
        draw_glyph(dst, x, y, *s++, fg, bg);
        x += 8;
    }
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

static void render_hello(uint32_t *dst, uint32_t tick)
{
    int title_x;
    int sub_x;
    int hint_x;
    int bar_w;
    uint32_t pulse;
    const char *title = "CIAO";
    const char *subtitle = "Trascinami con il mouse";
    const char *hint = "ESC o Ctrl+] per uscire";

    fill_rect(dst, 0, 0, SURF_W, SURF_H, 0x00F7FAFFU);
    fill_rect(dst, 12, 12, SURF_W - 24, SURF_H - 24, 0x00DFF4FFU);
    fill_rect(dst, 12, 12, SURF_W - 24, 32, 0x0000D6B8U);
    fill_rect(dst, 28, 64, SURF_W - 56, SURF_H - 96, 0x00FFFFFFU);

    title_x = (SURF_W - ((int)strlen(title) * 8 * 3)) / 2;
    for (int i = 0; title[i] != '\0'; i++) {
        for (int oy = 0; oy < 3; oy++) {
            for (int ox = 0; ox < 3; ox++) {
                draw_glyph(dst,
                           title_x + i * 24 + ox,
                           88 + oy,
                           title[i],
                           0x00000000U,
                           0x00FFFFFFU);
            }
        }
    }

    sub_x = (SURF_W - ((int)strlen(subtitle) * 8)) / 2;
    draw_text(dst, sub_x, 160, subtitle, 0x00003D4DU, 0x00FFFFFFU);
    hint_x = (SURF_W - ((int)strlen(hint) * 8)) / 2;
    draw_text(dst, hint_x, 178, hint, 0x00005B6EU, 0x00FFFFFFU);
    draw_text(dst, 28, 22, "EnlilOS Hello", 0x00FFFFFFU, 0x0000D6B8U);

    pulse = 48U + ((tick / 6U) % 160U);
    if (pulse > 200U)
        pulse = 200U;
    bar_w = 40 + (int)((tick * 7U) % (uint32_t)(SURF_W - 120));
    fill_rect(dst, 56, 196, SURF_W - 112, 10, 0x0098DCE8U);
    fill_rect(dst, 56, 196, bar_w, 10,
              ((pulse & 0xFFU) << 16) | 0x0000C080U);
}

static void commit_full_surface(wl_conn_t *conn)
{
    msg_begin(conn, 8U, WL_SURFACE_ATTACH, 12U);
    out_u32(conn, 7U);
    out_u32(conn, 0U);
    out_u32(conn, 0U);
    msg_begin(conn, 8U, WL_SURFACE_DAMAGE, 16U);
    out_u32(conn, 0U);
    out_u32(conn, 0U);
    out_u32(conn, SURF_W);
    out_u32(conn, SURF_H);
    msg_begin(conn, 8U, WL_SURFACE_COMMIT, 0U);
    wl_flush(conn);
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

int main(int argc, char **argv)
{
    wl_conn_t  conn;
    wl_event_t ev;
    int        shmid = -1;
    uint32_t  *shm_ptr = NULL;
    uint32_t   comp_name = 0U, shm_name = 0U, xdg_name = 0U;
    uint32_t   serial = 0U;
    int        configured = 0;
    int        session_mode = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "--session") == 0)
            session_mode = 1;
    }
    if (session_mode) {
        (void)unlink(HELLO_READY_PATH);
        (void)unlink(HELLO_FAIL_PATH);
    }

    if (wl_connect_runtime(&conn, &comp_name, &shm_name, &xdg_name) < 0) {
        if (session_mode)
            write_marker_file(HELLO_FAIL_PATH, "connect\n");
        write(2, "hello: connect fail\n", 20);
        return 1;
    }

    shmid = shmget(IPC_PRIVATE, SURF_SZ, IPC_CREAT | 0600);
    if (shmid < 0) {
        if (session_mode)
            write_marker_file(HELLO_FAIL_PATH, "shmget\n");
        return 2;
    }
    shm_ptr = (uint32_t *)shmat(shmid, NULL, 0);
    if ((long)shm_ptr == -1L) {
        if (session_mode)
            write_marker_file(HELLO_FAIL_PATH, "shmat\n");
        return 3;
    }
    render_hello(shm_ptr, 0U);
    flush_shm_pixels(shm_ptr, SURF_SZ);

    bind_global(&conn, comp_name, "wl_compositor", 4U, 3U);
    bind_global(&conn, shm_name, "wl_shm", 1U, 4U);
    bind_global(&conn, xdg_name, "xdg_wm_base", 1U, 5U);

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
              4U + (((uint32_t)strlen("Hello") + 1U + 3U) & ~3U));
    out_str(&conn, "Hello");
    wl_flush(&conn);

    msg_begin(&conn, 10U, XDG_TOP_SET_APP_ID,
              4U + (((uint32_t)strlen("enlil-hello") + 1U + 3U) & ~3U));
    out_str(&conn, "enlil-hello");
    wl_flush(&conn);

    msg_begin(&conn, 8U, WL_SURFACE_COMMIT, 0U);
    wl_flush(&conn);

    for (int polls = 400; polls-- > 0 && !configured; ) {
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0)
            conn.ilen += (uint32_t)n;

        while (wl_next_event(&conn, &ev)) {
            if (ev.obj_id == 9U && ev.opcode == XDG_SURFACE_CONFIGURE && ev.plen >= 4U) {
                serial = in_u32(ev.payload);
                configured = 1;
            } else if (ev.obj_id == 10U && ev.opcode == XDG_TOPLEVEL_CLOSE) {
                configured = -1;
            }
        }
        if (!configured)
            sleep_10ms();
    }

    if (configured <= 0) {
        if (session_mode)
            write_marker_file(HELLO_FAIL_PATH, "configure\n");
        write(2, "hello: configure timeout\n", 25);
        if (shm_ptr && (long)shm_ptr != -1L)
            shmdt(shm_ptr);
        if (shmid >= 0)
            (void)shmctl(shmid, IPC_RMID, NULL);
        if (conn.fd >= 0)
            close(conn.fd);
        return 4;
    }

    msg_begin(&conn, 9U, XDG_SURFACE_ACK_CONFIGURE, 4U);
    out_u32(&conn, serial);
    wl_flush(&conn);

    commit_full_surface(&conn);

    if (session_mode)
        write_marker_file(HELLO_READY_PATH, "ready\n");

    for (;;) {
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0)
            conn.ilen += (uint32_t)n;

        while (wl_next_event(&conn, &ev)) {
            if (ev.obj_id == 10U && ev.opcode == XDG_TOPLEVEL_CLOSE)
                goto out;
        }
        sleep_10ms();
    }

out:
    if (shm_ptr && (long)shm_ptr != -1L)
        shmdt(shm_ptr);
    if (shmid >= 0)
        (void)shmctl(shmid, IPC_RMID, NULL);
    if (conn.fd >= 0)
        close(conn.fd);
    return 0;
}
