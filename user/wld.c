/*
 * EnlilOS Wayland Compositor — wld (M12-01)
 *
 * Server Wayland minimale che gira in EL0.
 * Protocolli: wl_compositor, wl_shm, wl_surface, wl_buffer,
 *             xdg_wm_base, xdg_surface, xdg_toplevel, wl_seat, wl_output.
 *
 * Trasporto: AF_UNIX SOCK_STREAM su /run/wayland-0.
 * SHM:       SysV IPC (shmget/shmat) — client invia shmid inline.
 * Present:   SYS_WLD_PRESENT → kernel copia pixel nel framebuffer.
 *
 * Freestanding (no libc) — usa user_svc.h per tutte le syscall.
 */

#include "user_svc.h"
#include "syscall.h"
#include "types.h"

/* ── syscall wrappers ───────────────────────────────────────────── */
#define WLD_SYS_SOCKET      200
#define WLD_SYS_BIND        201
#define WLD_SYS_LISTEN      202
#define WLD_SYS_ACCEPT      203
#define WLD_SYS_SEND        205
#define WLD_SYS_RECV        206
#define WLD_SYS_CLOSE       5
#define WLD_SYS_MMAP        7
#define WLD_SYS_SHMGET      217
#define WLD_SYS_SHMAT       218
#define WLD_SYS_SHMDT       219
#define WLD_SYS_SHMCTL      220
#define WLD_SYS_GETTIMEOFDAY 42
#define WLD_SYS_NANOSLEEP  43
#define WLD_SYS_OPEN        4
#define WLD_SYS_WRITE       1
#define WLD_SYS_YIELD       20
#define WLD_SYS_WLD_PRESENT 224

/* mmap/mprotect flags */
#define WLD_PROT_READ   1
#define WLD_PROT_WRITE  2
#define WLD_MAP_PRIVATE     2
#define WLD_MAP_ANONYMOUS   0x20
#define WLD_MAP_FAILED      ((void *)(long)-1)

/* IPC_PRIVATE, IPC_CREAT|IPC_EXCL */
#define WLD_IPC_PRIVATE     0
#define WLD_IPC_CREAT       0x0200
#define WLD_IPC_RMID        0

/* socket */
#define WLD_AF_UNIX         1
#define WLD_SOCK_STREAM     1
#define WLD_SOCK_NONBLOCK   04000

#define WLD_O_WRONLY  1
#define WLD_O_CREAT   0100
#define WLD_O_TRUNC   01000

#define WLD_READY_PATH "/data/WLDREADY.TXT"

/* framebuffer dimensions */
#define WLD_FB_W    800U
#define WLD_FB_H    600U
#define WLD_FB_SZ   (WLD_FB_W * WLD_FB_H * 4U)

/* ── Mini strlib ────────────────────────────────────────────────── */
static void wld_memcpy(void *d, const void *s, long n)
{
    char *dd = (char *)d;
    const char *ss = (const char *)s;
    while (n-- > 0) *dd++ = *ss++;
}
static void wld_memset(void *d, int v, long n)
{
    char *p = (char *)d;
    while (n-- > 0) *p++ = (char)v;
}
static long wld_strlen(const char *s)
{
    long n = 0;
    while (s[n]) n++;
    return n;
}
static void wld_strcpy(char *d, const char *s)
{
    while ((*d++ = *s++));
}

/* ── Pool limits ────────────────────────────────────────────────── */
#define WLD_MAX_CLIENTS     8
#define WLD_MAX_SURFACES    24
#define WLD_MAX_POOLS       16
#define WLD_MAX_BUFFERS     32
#define WLD_MAX_OBJS        80   /* per client */

/* ── Wayland object types ───────────────────────────────────────── */
#define WOBJ_FREE          0
#define WOBJ_DISPLAY       1
#define WOBJ_REGISTRY      2
#define WOBJ_COMPOSITOR    3
#define WOBJ_SHM           4
#define WOBJ_SHM_POOL      5
#define WOBJ_SURFACE       6
#define WOBJ_BUFFER        7
#define WOBJ_XDG_WM        8
#define WOBJ_XDG_SURF      9
#define WOBJ_XDG_TOP       10
#define WOBJ_CALLBACK      11
#define WOBJ_SEAT          12
#define WOBJ_OUTPUT        13
#define WOBJ_REGION        14
#define WOBJ_ENLIL_WM      15

/* ── Global registry names ──────────────────────────────────────── */
#define GLOBAL_COMPOSITOR  1U
#define GLOBAL_SHM         2U
#define GLOBAL_XDG_WM      3U
#define GLOBAL_SEAT        4U
#define GLOBAL_OUTPUT      5U
#define GLOBAL_ENLIL_WM    6U

/* ── Pixel formats ──────────────────────────────────────────────── */
#define WL_SHM_FORMAT_ARGB8888  0U
#define WL_SHM_FORMAT_XRGB8888  1U

/* ── Wayland message opcodes (server-side) ──────────────────────── */
/* wl_display events */
#define WL_DISPLAY_ERROR         0U
#define WL_DISPLAY_DELETE_ID     1U
/* wl_registry events */
#define WL_REGISTRY_GLOBAL       0U
/* wl_shm events */
#define WL_SHM_FORMAT            0U
/* xdg_wm_base events */
#define XDG_WM_BASE_PING         0U
/* xdg_surface events */
#define XDG_SURFACE_CONFIGURE    0U
/* xdg_toplevel events */
#define XDG_TOPLEVEL_CONFIGURE   0U
#define XDG_TOPLEVEL_CLOSE       1U
/* wl_callback events */
#define WL_CALLBACK_DONE         0U
/* wl_seat events */
#define WL_SEAT_CAPABILITIES     0U
/* wl_output events */
#define WL_OUTPUT_GEOMETRY       0U
#define WL_OUTPUT_MODE           1U
#define WL_OUTPUT_DONE           3U
/* enlil_wm_v1 events */
#define ENLIL_WM_STATE           0U

/* ── Wayland message opcodes (client requests) ──────────────────── */
/* wl_display */
#define WL_DISPLAY_SYNC          0U
#define WL_DISPLAY_GET_REGISTRY  1U
/* wl_registry */
#define WL_REGISTRY_BIND         0U
/* wl_compositor */
#define WL_COMPOSITOR_CREATE_SURFACE  0U
#define WL_COMPOSITOR_CREATE_REGION   1U
/* wl_shm */
#define WL_SHM_CREATE_POOL_SYSV  0U   /* EnlilOS: new_id + shmid_as_int32 + size */
/* wl_shm_pool */
#define WL_SHM_POOL_CREATE_BUFFER  0U
#define WL_SHM_POOL_DESTROY        1U
#define WL_SHM_POOL_RESIZE         2U
/* wl_buffer */
#define WL_BUFFER_DESTROY          0U
/* wl_surface */
#define WL_SURFACE_DESTROY         0U
#define WL_SURFACE_ATTACH          1U
#define WL_SURFACE_DAMAGE          2U
#define WL_SURFACE_FRAME           3U
#define WL_SURFACE_COMMIT          6U
/* xdg_wm_base */
#define XDG_WM_DESTROY             0U
#define XDG_WM_CREATE_POSITIONER   1U
#define XDG_WM_GET_XDG_SURFACE     2U
#define XDG_WM_PONG                3U
/* xdg_surface */
#define XDG_SURFACE_DESTROY        0U
#define XDG_SURFACE_GET_TOPLEVEL   1U
#define XDG_SURFACE_ACK_CONFIGURE  4U
/* xdg_toplevel */
#define XDG_TOP_DESTROY            0U
#define XDG_TOP_SET_TITLE          2U
#define XDG_TOP_SET_APP_ID         3U
/* enlil_wm_v1 */
#define ENLIL_WM_SET_LAYOUT        0U
#define ENLIL_WM_FOCUS_NEXT        1U
#define ENLIL_WM_FOCUS_PREV        2U
#define ENLIL_WM_CLOSE_FOCUSED     3U
#define ENLIL_WM_GET_STATE         4U

#define ENLIL_WM_LAYOUT_TILE       1U

/* ════════════════════════════════════════════════════════════════
 * Data structures
 * ════════════════════════════════════════════════════════════════ */

typedef struct {
    uint32_t id;
    uint8_t  type;   /* WOBJ_* */
    uint8_t  alive;
    uint16_t extra;  /* type-specific index or data */
} wobj_t;

typedef struct {
    void    *ptr;   /* shmat result */
    int      shmid;
    uint32_t size;
    uint8_t  alive;
} wpool_t;

typedef struct {
    uint32_t pool_idx;
    uint32_t offset;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint8_t  alive;
} wbuf_t;

typedef struct {
    uint8_t  alive;
    uint32_t client_idx;
    uint32_t surface_obj;  /* wl_surface object id */
    uint32_t xdg_surf_obj;
    uint32_t xdg_top_obj;
    /* pending state */
    uint32_t pending_buf;  /* wbuf_t index, 0xFFFF = none */
    /* current committed state */
    uint32_t current_buf;  /* wbuf_t index, 0xFFFF = none */
    uint32_t frame_cb_id;  /* wl_callback object id, 0 = none */
    int32_t  x, y;
    uint32_t view_w;
    uint32_t view_h;
    char     title[64];
    uint8_t  configured;
    uint8_t  focused;
    uint8_t  fade_ticks;
    uint8_t  _pad0;
} wsurf_t;

typedef struct {
    int      fd;
    uint8_t  alive;
    uint32_t serial;
    wobj_t   objs[WLD_MAX_OBJS];
    uint32_t obj_count;
    /* input buffer */
    uint8_t  ibuf[8192];
    uint32_t ilen;
    /* output buffer */
    uint8_t  obuf[8192];
    uint32_t olen;
} wclient_t;

/* ── Global state ───────────────────────────────────────────────── */
static wclient_t wld_clients[WLD_MAX_CLIENTS];
static wpool_t   wld_pools  [WLD_MAX_POOLS];
static wbuf_t    wld_buffers[WLD_MAX_BUFFERS];
static wsurf_t   wld_surfs  [WLD_MAX_SURFACES];

static int      wld_listen_fd = -1;
static void    *wld_composite = WLD_MAP_FAILED;  /* MAP_ANONYMOUS compositing buf */
static uint32_t wld_frame_serial = 1U;
static uint32_t wld_present_count = 0U;
static uint32_t wld_layout_mode = ENLIL_WM_LAYOUT_TILE;
static uint32_t wld_focus_idx = 0xFFFFU;

/* ── sockaddr_un ────────────────────────────────────────────────── */
typedef struct {
    unsigned short sun_family;
    char           sun_path[108];
} wld_sockaddr_un_t;

typedef struct {
    long tv_sec;
    long tv_nsec;
} wld_timespec_t;

/* ── Syscall wrappers ───────────────────────────────────────────── */
static long wld_socket(int dom, int type, int proto)
{ return user_svc3(WLD_SYS_SOCKET, dom, type, proto); }

static long wld_bind(int fd, const void *addr, int addrlen)
{ return user_svc3(WLD_SYS_BIND, fd, (long)addr, addrlen); }

static long wld_listen(int fd, int bl)
{ return user_svc2(WLD_SYS_LISTEN, fd, bl); }

static long wld_accept(int fd, void *addr, void *alen)
{ return user_svc3(WLD_SYS_ACCEPT, fd, (long)addr, (long)alen); }

static long wld_send(int fd, const void *buf, long len, int fl)
{ return user_svc4(WLD_SYS_SEND, fd, (long)buf, len, fl); }

static long wld_recv(int fd, void *buf, long len, int fl)
{ return user_svc4(WLD_SYS_RECV, fd, (long)buf, len, fl); }

static void wld_close(int fd)
{ (void)user_svc1(WLD_SYS_CLOSE, fd); }

static void *wld_mmap(long size)
{
    return (void *)user_svc6(WLD_SYS_MMAP, 0, size,
                             WLD_PROT_READ | WLD_PROT_WRITE,
                             WLD_MAP_PRIVATE | WLD_MAP_ANONYMOUS,
                             (long)-1, 0);
}

static void *wld_shmat(int shmid)
{ return (void *)user_svc3(WLD_SYS_SHMAT, shmid, 0, 0); }

static void wld_shmdt(void *ptr)
{ (void)user_svc1(WLD_SYS_SHMDT, (long)ptr); }

static long wld_gettimeofday_ms(void)
{
    struct { long tv_sec; long tv_usec; } tv;
    (void)user_svc2(WLD_SYS_GETTIMEOFDAY, (long)&tv, 0);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}

static void wld_sleep_ms(long ms)
{
    wld_timespec_t ts;

    if (ms <= 0L)
        return;
    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    (void)user_svc2(WLD_SYS_NANOSLEEP, (long)&ts, 0);
}

static void wld_write_log(const char *s)
{
    long len = wld_strlen(s);
    (void)user_svc3(WLD_SYS_WRITE, 2, (long)s, len); /* stderr fd=2 */
}

static void wld_write_ready_marker(void)
{
    static const char ready_text[] = "ready\n";
    long fd = user_svc3(WLD_SYS_OPEN, (long)WLD_READY_PATH,
                        WLD_O_WRONLY | WLD_O_CREAT | WLD_O_TRUNC, 0644);
    if (fd < 0)
        return;
    (void)user_svc3(WLD_SYS_WRITE, fd, (long)ready_text, (long)(sizeof(ready_text) - 1U));
    wld_close((int)fd);
}

static long wld_present(long w, long h, long stride)
{ return user_svc4(WLD_SYS_WLD_PRESENT, (long)wld_composite, w, h, stride); }

/* ── Object helpers ─────────────────────────────────────────────── */
static wobj_t *wobj_find(wclient_t *c, uint32_t id)
{
    for (uint32_t i = 0U; i < c->obj_count; i++) {
        if (c->objs[i].alive && c->objs[i].id == id)
            return &c->objs[i];
    }
    return (wobj_t *)0;
}

static wobj_t *wobj_alloc(wclient_t *c, uint32_t id, uint8_t type, uint16_t extra)
{
    if (c->obj_count >= WLD_MAX_OBJS)
        return (wobj_t *)0;
    wobj_t *o = &c->objs[c->obj_count++];
    o->id    = id;
    o->type  = type;
    o->alive = 1U;
    o->extra = extra;
    return o;
}

static uint32_t pool_alloc(void)
{
    for (uint32_t i = 0U; i < WLD_MAX_POOLS; i++)
        if (!wld_pools[i].alive) return i;
    return 0xFFFFU;
}

static uint32_t buf_alloc(void)
{
    for (uint32_t i = 0U; i < WLD_MAX_BUFFERS; i++)
        if (!wld_buffers[i].alive) return i;
    return 0xFFFFU;
}

static uint32_t surf_alloc(void)
{
    for (uint32_t i = 0U; i < WLD_MAX_SURFACES; i++)
        if (!wld_surfs[i].alive) return i;
    return 0xFFFFU;
}

static uint32_t client_idx_of(wclient_t *c)
{
    return (uint32_t)(c - wld_clients);
}

/* ── Wire protocol helpers ──────────────────────────────────────── */
/* Scrivi uint32 LE nel buffer output */
static void out_u32(wclient_t *c, uint32_t v)
{
    if (c->olen + 4U > sizeof(c->obuf)) return;
    c->obuf[c->olen+0] = (uint8_t)(v      );
    c->obuf[c->olen+1] = (uint8_t)(v >>  8);
    c->obuf[c->olen+2] = (uint8_t)(v >> 16);
    c->obuf[c->olen+3] = (uint8_t)(v >> 24);
    c->olen += 4U;
}

/* Leggi uint32 LE dal buffer input */
static uint32_t in_u32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Scrivi stringa Wayland: len (con NUL) + bytes + padding */
static void out_str(wclient_t *c, const char *s)
{
    uint32_t slen = (uint32_t)(wld_strlen(s) + 1U);
    uint32_t plen = (slen + 3U) & ~3U;
    uint8_t  pad[4] = {0, 0, 0, 0};

    out_u32(c, slen);
    if (c->olen + plen > sizeof(c->obuf)) return;
    wld_memcpy(c->obuf + c->olen, s, (long)slen);
    if (plen > slen)
        wld_memset(c->obuf + c->olen + slen, 0, (long)(plen - slen));
    c->olen += plen;
    (void)pad;
}

/* Invia messaggio: comincia con header poi chiama out_u32/out_str */
static void msg_begin(wclient_t *c, uint32_t obj_id, uint32_t opcode,
                      uint32_t payload_bytes)
{
    uint32_t size = 8U + payload_bytes;
    out_u32(c, obj_id);
    out_u32(c, (size << 16) | opcode);
}

/* Flush output buffer */
static void client_flush(wclient_t *c)
{
    if (c->olen == 0U || c->fd < 0) return;
    long sent = 0;
    while ((uint32_t)sent < c->olen) {
        long r = wld_send(c->fd, c->obuf + sent, (long)(c->olen - (uint32_t)sent), 0);
        if (r <= 0) break;
        sent += r;
    }
    c->olen = 0U;
}

static void send_configure(wclient_t *c, wsurf_t *surf);

static int wsurf_is_viewable(const wsurf_t *surf)
{
    return (surf && surf->alive &&
            surf->xdg_top_obj != 0U &&
            surf->current_buf != 0xFFFFU &&
            surf->current_buf < WLD_MAX_BUFFERS &&
            wld_buffers[surf->current_buf].alive);
}

static void wld_fill_rect(uint32_t *dst, int32_t x, int32_t y,
                          uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t row = 0U; row < h; row++) {
        int32_t dy = y + (int32_t)row;
        if (dy < 0 || dy >= (int32_t)WLD_FB_H)
            continue;
        for (uint32_t col = 0U; col < w; col++) {
            int32_t dx = x + (int32_t)col;
            if (dx < 0 || dx >= (int32_t)WLD_FB_W)
                continue;
            dst[(uint32_t)dy * WLD_FB_W + (uint32_t)dx] = color;
        }
    }
}

static uint32_t wld_blend_rgb(uint32_t dst, uint32_t src, uint8_t alpha)
{
    uint32_t inv = (uint32_t)(255U - alpha);
    uint32_t dr  = (dst >> 16) & 0xFFU;
    uint32_t dg  = (dst >> 8)  & 0xFFU;
    uint32_t db  = dst & 0xFFU;
    uint32_t sr  = (src >> 16) & 0xFFU;
    uint32_t sg  = (src >> 8)  & 0xFFU;
    uint32_t sb  = src & 0xFFU;
    uint32_t rr  = (dr * inv + sr * alpha) / 255U;
    uint32_t rg  = (dg * inv + sg * alpha) / 255U;
    uint32_t rb  = (db * inv + sb * alpha) / 255U;

    return (rr << 16) | (rg << 8) | rb;
}

static void wld_blit_scaled(uint32_t *dst, int32_t dx0, int32_t dy0,
                            uint32_t dw, uint32_t dh,
                            const uint32_t *src, uint32_t sw, uint32_t sh,
                            uint32_t stride_px, uint8_t alpha)
{
    if (!src || !dst || sw == 0U || sh == 0U || dw == 0U || dh == 0U)
        return;

    for (uint32_t dy = 0U; dy < dh; dy++) {
        int32_t out_y = dy0 + (int32_t)dy;
        uint32_t sy;

        if (out_y < 0 || out_y >= (int32_t)WLD_FB_H)
            continue;
        sy = (dy * sh) / dh;
        if (sy >= sh)
            sy = sh - 1U;

        for (uint32_t dx = 0U; dx < dw; dx++) {
            int32_t out_x = dx0 + (int32_t)dx;
            uint32_t sx;
            uint32_t pix;
            uint32_t *dst_px;

            if (out_x < 0 || out_x >= (int32_t)WLD_FB_W)
                continue;
            sx = (dx * sw) / dw;
            if (sx >= sw)
                sx = sw - 1U;
            pix = src[sy * stride_px + sx];
            dst_px = &dst[(uint32_t)out_y * WLD_FB_W + (uint32_t)out_x];
            *dst_px = (alpha >= 255U) ? pix : wld_blend_rgb(*dst_px, pix, alpha);
        }
    }
}

static uint32_t wld_count_viewable(void)
{
    uint32_t count = 0U;

    for (uint32_t i = 0U; i < WLD_MAX_SURFACES; i++) {
        if (wsurf_is_viewable(&wld_surfs[i]))
            count++;
    }
    return count;
}

static void wld_focus_sanitize(void)
{
    if (wld_focus_idx < WLD_MAX_SURFACES && wsurf_is_viewable(&wld_surfs[wld_focus_idx]))
        return;

    wld_focus_idx = 0xFFFFU;
    for (uint32_t i = 0U; i < WLD_MAX_SURFACES; i++) {
        if (!wsurf_is_viewable(&wld_surfs[i]))
            continue;
        wld_focus_idx = i;
        return;
    }
}

static void wld_send_wm_state(wclient_t *c, uint32_t obj_id)
{
    msg_begin(c, obj_id, ENLIL_WM_STATE, 16U);
    out_u32(c, wld_layout_mode);
    out_u32(c, wld_count_viewable());
    out_u32(c, wld_focus_idx);
    out_u32(c, wld_present_count);
    client_flush(c);
}

static void wld_request_close_focused(void)
{
    uint32_t ci;
    wclient_t *c;
    wsurf_t   *surf;

    wld_focus_sanitize();
    if (wld_focus_idx >= WLD_MAX_SURFACES)
        return;

    surf = &wld_surfs[wld_focus_idx];
    ci = surf->client_idx;
    if (ci >= WLD_MAX_CLIENTS || !wld_clients[ci].alive || surf->xdg_top_obj == 0U)
        return;
    c = &wld_clients[ci];
    msg_begin(c, surf->xdg_top_obj, XDG_TOPLEVEL_CLOSE, 0U);
    client_flush(c);
}

static void wld_relayout(int send_cfg)
{
    uint32_t count = wld_count_viewable();
    uint32_t visible = 0U;
    const uint32_t gap = 12U;
    uint32_t avail_w;
    uint32_t col_w;

    wld_focus_sanitize();
    if (count == 0U)
        return;

    if (count == 1U) {
        avail_w = WLD_FB_W - (gap * 2U);
        col_w = avail_w;
    } else {
        avail_w = WLD_FB_W - gap * (count + 1U);
        col_w = (count > 0U) ? (avail_w / count) : avail_w;
    }

    for (uint32_t i = 0U; i < WLD_MAX_SURFACES; i++) {
        wsurf_t *surf = &wld_surfs[i];
        wclient_t *c;
        uint32_t x;
        uint32_t y = gap;
        uint32_t h = WLD_FB_H - (gap * 2U);

        surf->focused = 0U;
        if (!wsurf_is_viewable(surf))
            continue;

        x = gap + visible * (col_w + gap);
        surf->x = (int32_t)x;
        surf->y = (int32_t)y;
        surf->view_w = col_w;
        surf->view_h = h;
        surf->focused = (i == wld_focus_idx) ? 1U : 0U;
        visible++;

        if (!send_cfg)
            continue;
        if (surf->client_idx >= WLD_MAX_CLIENTS)
            continue;
        c = &wld_clients[surf->client_idx];
        if (!c->alive)
            continue;
        send_configure(c, surf);
    }
}

static void wld_focus_rotate(int dir)
{
    uint32_t start;
    uint32_t idx;

    if (wld_count_viewable() == 0U)
        return;
    wld_focus_sanitize();
    if (wld_focus_idx >= WLD_MAX_SURFACES) {
        wld_focus_sanitize();
        wld_relayout(0);
        return;
    }

    start = wld_focus_idx;
    idx = start;
    for (uint32_t n = 0U; n < WLD_MAX_SURFACES; n++) {
        if (dir >= 0)
            idx = (idx + 1U) % WLD_MAX_SURFACES;
        else
            idx = (idx == 0U) ? (WLD_MAX_SURFACES - 1U) : (idx - 1U);
        if (!wsurf_is_viewable(&wld_surfs[idx]))
            continue;
        wld_focus_idx = idx;
        break;
    }
    if (wld_focus_idx == start && !wsurf_is_viewable(&wld_surfs[wld_focus_idx]))
        wld_focus_sanitize();
    wld_relayout(0);
}

/* ── Compositor logic ───────────────────────────────────────────── */
static void composite_and_present(void)
{
    const uint32_t border = 2U;
    const uint32_t title_h = 22U;

    if (wld_composite == WLD_MAP_FAILED) return;

    /* Pulisci a nero */
    wld_memset(wld_composite, 0, (long)WLD_FB_SZ);

    uint32_t *dst = (uint32_t *)wld_composite;

    wld_relayout(0);

    for (uint32_t pass = 0U; pass < 2U; pass++) {
    for (uint32_t si = 0U; si < WLD_MAX_SURFACES; si++) {
        wsurf_t *surf = &wld_surfs[si];
        uint8_t focused;
        uint32_t frame_w;
        uint32_t frame_h;
        uint32_t content_x;
        uint32_t content_y;
        uint32_t content_w;
        uint32_t content_h;
        uint32_t alpha = 255U;
        uint32_t deco = 0x002a3648U;
        uint32_t frame = 0x0053647dU;

        if (!wsurf_is_viewable(surf))
            continue;
        focused = (si == wld_focus_idx) ? 1U : 0U;
        if ((pass == 0U && focused) || (pass == 1U && !focused))
            continue;

        wbuf_t *buf = &wld_buffers[surf->current_buf];
        if (!buf->alive) continue;
        if (buf->pool_idx >= WLD_MAX_POOLS) continue;

        wpool_t *pool = &wld_pools[buf->pool_idx];
        if (!pool->alive || !pool->ptr) continue;

        uint8_t *src = (uint8_t *)pool->ptr + buf->offset;
        uint32_t w   = buf->width;
        uint32_t h   = buf->height;
        uint32_t str = buf->stride;
        int32_t  sx  = surf->x;
        int32_t  sy  = surf->y;

        frame_w = (surf->view_w != 0U) ? surf->view_w : w;
        frame_h = (surf->view_h != 0U) ? surf->view_h : (h + title_h + border);
        content_x = (uint32_t)((sx >= 0) ? sx : 0);
        content_y = (uint32_t)((sy >= 0) ? sy : 0);
        if (frame_w <= border * 2U || frame_h <= title_h + border * 2U)
            continue;

        if (focused) {
            deco = 0x00324766U;
            frame = 0x0000d6b8U;
        }

        wld_fill_rect(dst, sx + 4, sy + 4, frame_w, frame_h, 0x00101822U);
        wld_fill_rect(dst, sx, sy, frame_w, frame_h, frame);
        wld_fill_rect(dst, sx + (int32_t)border, sy + (int32_t)border,
                      frame_w - border * 2U, title_h, deco);

        content_x = (uint32_t)(sx + (int32_t)border);
        content_y = (uint32_t)(sy + (int32_t)title_h + (int32_t)border);
        content_w = frame_w - border * 2U;
        content_h = frame_h - title_h - border * 2U;
        wld_fill_rect(dst, (int32_t)content_x, (int32_t)content_y,
                      content_w, content_h, 0x00070a10U);

        if (surf->fade_ticks > 0U) {
            alpha = ((uint32_t)(9U - surf->fade_ticks) * 255U) / 8U;
            if (alpha > 255U)
                alpha = 255U;
        }

        wld_blit_scaled(dst, (int32_t)content_x, (int32_t)content_y,
                        content_w, content_h, (const uint32_t *)src,
                        w, h, str / 4U, (uint8_t)alpha);

        if (surf->fade_ticks > 0U)
            surf->fade_ticks--;
    }
    }

    wld_present((long)WLD_FB_W, (long)WLD_FB_H, (long)(WLD_FB_W * 4U));
    wld_present_count++;
}

/* Send frame done callbacks to clients whose surfaces committed this frame */
static void send_frame_dones(void)
{
    uint32_t time_ms = (uint32_t)wld_gettimeofday_ms();

    for (uint32_t si = 0U; si < WLD_MAX_SURFACES; si++) {
        wsurf_t *surf = &wld_surfs[si];
        if (!surf->alive || surf->frame_cb_id == 0U)
            continue;

        uint32_t ci = surf->client_idx;
        if (ci >= WLD_MAX_CLIENTS || !wld_clients[ci].alive)
            continue;

        wclient_t *c = &wld_clients[ci];
        uint32_t cb_id = surf->frame_cb_id;
        surf->frame_cb_id = 0U;

        /* wl_callback.done(callback_data=timestamp_ms) */
        msg_begin(c, cb_id, WL_CALLBACK_DONE, 4U);
        out_u32(c, time_ms);
        /* delete callback object */
        msg_begin(c, 1U, WL_DISPLAY_DELETE_ID, 4U);
        out_u32(c, cb_id);
        client_flush(c);
    }
}

/* ── Registry: announce globals to new client ───────────────────── */
static void send_globals(wclient_t *c, uint32_t reg_id)
{
    struct { uint32_t name; const char *iface; uint32_t ver; } globals[] = {
        { GLOBAL_COMPOSITOR, "wl_compositor",  4U },
        { GLOBAL_SHM,        "wl_shm",         1U },
        { GLOBAL_XDG_WM,     "xdg_wm_base",    1U },
        { GLOBAL_SEAT,       "wl_seat",         5U },
        { GLOBAL_OUTPUT,     "wl_output",       3U },
        { GLOBAL_ENLIL_WM,   "enlil_wm_v1",    1U },
    };
    for (uint32_t i = 0U; i < (uint32_t)(sizeof(globals) / sizeof(globals[0])); i++) {
        uint32_t slen  = (uint32_t)(wld_strlen(globals[i].iface) + 1U);
        uint32_t plen  = (slen + 3U) & ~3U;
        uint32_t pyld  = 4U + 4U + plen + 4U;  /* name + slen + str + version */
        msg_begin(c, reg_id, WL_REGISTRY_GLOBAL, pyld);
        out_u32(c, globals[i].name);
        out_str(c, globals[i].iface);
        out_u32(c, globals[i].ver);
        client_flush(c);
    }
}

/* Send wl_shm format events */
static void send_shm_formats(wclient_t *c, uint32_t shm_id)
{
    msg_begin(c, shm_id, WL_SHM_FORMAT, 4U);
    out_u32(c, WL_SHM_FORMAT_ARGB8888);
    msg_begin(c, shm_id, WL_SHM_FORMAT, 4U);
    out_u32(c, WL_SHM_FORMAT_XRGB8888);
    client_flush(c);
}

/* Send xdg_surface.configure + xdg_toplevel.configure */
static void send_configure(wclient_t *c, wsurf_t *surf)
{
    uint32_t serial = ++wld_frame_serial;
    uint32_t width = 0U;
    uint32_t height = 0U;

    if (surf->view_w > 4U)
        width = surf->view_w - 4U;
    if (surf->view_h > 26U)
        height = surf->view_h - 26U;

    /* xdg_toplevel.configure(width, height, empty_states) */
    if (surf->xdg_top_obj) {
        /* states array: empty array = [length=0] */
        msg_begin(c, surf->xdg_top_obj, XDG_TOPLEVEL_CONFIGURE, 12U);
        out_u32(c, width);
        out_u32(c, height);
        out_u32(c, 0U); /* states array length = 0 */
    }

    /* xdg_surface.configure(serial) */
    if (surf->xdg_surf_obj) {
        msg_begin(c, surf->xdg_surf_obj, XDG_SURFACE_CONFIGURE, 4U);
        out_u32(c, serial);
    }
    client_flush(c);
}

/* Send wl_seat capabilities */
static void send_seat_caps(wclient_t *c, uint32_t seat_id)
{
    /* WL_SEAT_CAPABILITY_KEYBOARD=2, WL_SEAT_CAPABILITY_POINTER=1 */
    msg_begin(c, seat_id, WL_SEAT_CAPABILITIES, 4U);
    out_u32(c, 3U);
    client_flush(c);
}

/* Send wl_output events */
static void send_output_info(wclient_t *c, uint32_t out_id)
{
    /* geometry: x,y,w_mm,h_mm,subpixel,make,model,transform */
    uint32_t make_len   = (4U + 3U) & ~3U; /* "wld\0" */
    uint32_t model_len  = (5U + 3U) & ~3U; /* "virt\0" */
    uint32_t geom_pyld  = 4U+4U+4U+4U+4U + (4U+make_len) + (4U+model_len) + 4U;
    msg_begin(c, out_id, WL_OUTPUT_GEOMETRY, geom_pyld);
    out_u32(c, 0U); out_u32(c, 0U);   /* x, y */
    out_u32(c, 200U); out_u32(c, 150U); /* phys mm */
    out_u32(c, 0U);                    /* subpixel */
    out_str(c, "wld");
    out_str(c, "virt");
    out_u32(c, 0U);                    /* transform normal */

    /* mode: WL_OUTPUT_MODE_CURRENT=1, w, h, refresh */
    msg_begin(c, out_id, WL_OUTPUT_MODE, 16U);
    out_u32(c, 1U); out_u32(c, WLD_FB_W); out_u32(c, WLD_FB_H);
    out_u32(c, 60000U); /* 60Hz in mHz */

    /* done */
    msg_begin(c, out_id, WL_OUTPUT_DONE, 0U);
    client_flush(c);
}

/* ── Parse helper: read wl_registry.bind interface string ───────── */
static void parse_str(const uint8_t *p, uint32_t avail,
                      char *out, uint32_t cap, uint32_t *consumed)
{
    uint32_t slen, plen;
    *consumed = 0U;
    if (avail < 4U) return;
    slen = in_u32(p);
    plen = (slen + 3U) & ~3U;
    if (avail < 4U + plen) return;
    *consumed = 4U + plen;
    uint32_t copy = slen < cap ? slen : cap - 1U;
    wld_memcpy(out, p + 4U, (long)copy);
    out[copy] = '\0';
}

/* ── Dispatch a single Wayland request ──────────────────────────── */
static void dispatch_msg(wclient_t *c, uint32_t obj_id, uint32_t opcode,
                         const uint8_t *payload, uint32_t payload_len)
{
    wobj_t *obj = wobj_find(c, obj_id);

    /* ── wl_display ── */
    if (obj_id == 1U) {
        if (opcode == WL_DISPLAY_GET_REGISTRY && payload_len >= 4U) {
            uint32_t reg_id = in_u32(payload);
            wobj_alloc(c, reg_id, WOBJ_REGISTRY, 0U);
            send_globals(c, reg_id);
        } else if (opcode == WL_DISPLAY_SYNC && payload_len >= 4U) {
            uint32_t cb_id = in_u32(payload);
            wobj_alloc(c, cb_id, WOBJ_CALLBACK, 0U);
            /* Send done immediately */
            msg_begin(c, cb_id, WL_CALLBACK_DONE, 4U);
            out_u32(c, (uint32_t)wld_gettimeofday_ms());
            msg_begin(c, 1U, WL_DISPLAY_DELETE_ID, 4U);
            out_u32(c, cb_id);
            client_flush(c);
        }
        return;
    }

    if (!obj) return;

    /* ── wl_registry ── */
    if (obj->type == WOBJ_REGISTRY) {
        if (opcode == WL_REGISTRY_BIND && payload_len >= 12U) {
            uint32_t name  = in_u32(payload);
            uint32_t cons  = 0U;
            char     iface[64];
            parse_str(payload + 4U, payload_len - 4U, iface, sizeof(iface), &cons);
            if (cons == 0U || payload_len < 4U + cons + 8U) return;
            /* version + new_id after string */
            uint32_t new_id = in_u32(payload + 4U + cons + 4U);

            if (name == GLOBAL_COMPOSITOR) {
                wobj_alloc(c, new_id, WOBJ_COMPOSITOR, 0U);
            } else if (name == GLOBAL_SHM) {
                wobj_alloc(c, new_id, WOBJ_SHM, 0U);
                send_shm_formats(c, new_id);
            } else if (name == GLOBAL_XDG_WM) {
                wobj_alloc(c, new_id, WOBJ_XDG_WM, 0U);
            } else if (name == GLOBAL_SEAT) {
                wobj_alloc(c, new_id, WOBJ_SEAT, 0U);
                send_seat_caps(c, new_id);
            } else if (name == GLOBAL_OUTPUT) {
                wobj_alloc(c, new_id, WOBJ_OUTPUT, 0U);
                send_output_info(c, new_id);
            } else if (name == GLOBAL_ENLIL_WM) {
                wobj_alloc(c, new_id, WOBJ_ENLIL_WM, 0U);
                wld_send_wm_state(c, new_id);
            }
        }
        return;
    }

    /* ── wl_compositor ── */
    if (obj->type == WOBJ_COMPOSITOR) {
        if (opcode == WL_COMPOSITOR_CREATE_SURFACE && payload_len >= 4U) {
            uint32_t new_id = in_u32(payload);
            uint32_t si     = surf_alloc();
            if (si == 0xFFFFU) return;
            /* Zero whole slot first: resets configured/xdg_*_obj/title/focused */
            wld_memset(&wld_surfs[si], 0, (long)sizeof(wld_surfs[si]));
            wld_surfs[si].alive       = 1U;
            wld_surfs[si].client_idx  = client_idx_of(c);
            wld_surfs[si].surface_obj = new_id;
            wld_surfs[si].pending_buf = 0xFFFFU;
            wld_surfs[si].current_buf = 0xFFFFU;
            wld_surfs[si].view_w     = 320U;
            wld_surfs[si].view_h     = 220U;
            wld_surfs[si].fade_ticks = 8U;
            wobj_alloc(c, new_id, WOBJ_SURFACE, (uint16_t)si);
        }
        return;
    }

    /* ── wl_shm ── */
    if (obj->type == WOBJ_SHM) {
        /* WL_SHM_CREATE_POOL_SYSV: new_id(4) + shmid(4) + size(4) */
        if (opcode == WL_SHM_CREATE_POOL_SYSV && payload_len >= 12U) {
            uint32_t new_id = in_u32(payload);
            int      shmid  = (int)in_u32(payload + 4U);
            uint32_t size   = in_u32(payload + 8U);
            uint32_t pi     = pool_alloc();
            if (pi == 0xFFFFU) return;
            void *ptr = wld_shmat(shmid);
            if ((long)ptr < 0L || ptr == (void *)-1L) return;
            wld_pools[pi].alive = 1U;
            wld_pools[pi].ptr   = ptr;
            wld_pools[pi].shmid = shmid;
            wld_pools[pi].size  = size;
            wobj_alloc(c, new_id, WOBJ_SHM_POOL, (uint16_t)pi);
        }
        return;
    }

    /* ── wl_shm_pool ── */
    if (obj->type == WOBJ_SHM_POOL) {
        uint32_t pi = obj->extra;
        if (opcode == WL_SHM_POOL_CREATE_BUFFER && payload_len >= 24U) {
            uint32_t new_id = in_u32(payload);
            uint32_t offset = in_u32(payload + 4U);
            uint32_t width  = in_u32(payload + 8U);
            uint32_t height = in_u32(payload + 12U);
            uint32_t stride = in_u32(payload + 16U);
            uint32_t format = in_u32(payload + 20U);
            uint32_t bi     = buf_alloc();
            if (bi == 0xFFFFU) return;
            wld_buffers[bi].alive    = 1U;
            wld_buffers[bi].pool_idx = pi;
            wld_buffers[bi].offset   = offset;
            wld_buffers[bi].width    = width;
            wld_buffers[bi].height   = height;
            wld_buffers[bi].stride   = stride;
            wld_buffers[bi].format   = format;
            wobj_alloc(c, new_id, WOBJ_BUFFER, (uint16_t)bi);
        } else if (opcode == WL_SHM_POOL_DESTROY) {
            if (pi < WLD_MAX_POOLS && wld_pools[pi].alive) {
                wld_shmdt(wld_pools[pi].ptr);
                wld_pools[pi].ptr   = (void *)0;
                wld_pools[pi].alive = 0U;
            }
            obj->alive = 0U;
        }
        return;
    }

    /* ── wl_buffer ── */
    if (obj->type == WOBJ_BUFFER) {
        if (opcode == WL_BUFFER_DESTROY) {
            uint32_t bi = obj->extra;
            if (bi < WLD_MAX_BUFFERS) wld_buffers[bi].alive = 0U;
            obj->alive = 0U;
        }
        return;
    }

    /* ── wl_surface ── */
    if (obj->type == WOBJ_SURFACE) {
        uint32_t si = obj->extra;
        wsurf_t *surf = (si < WLD_MAX_SURFACES) ? &wld_surfs[si] : (wsurf_t *)0;

        if (opcode == WL_SURFACE_ATTACH && payload_len >= 12U && surf) {
            uint32_t buf_id = in_u32(payload);
            wobj_t *bo = wobj_find(c, buf_id);
            surf->pending_buf = (bo && bo->type == WOBJ_BUFFER)
                                ? bo->extra : 0xFFFFU;
        } else if (opcode == WL_SURFACE_DAMAGE) {
            /* accepted, ignored in v1 (full redraw each frame) */
        } else if (opcode == WL_SURFACE_FRAME && payload_len >= 4U && surf) {
            uint32_t cb_id = in_u32(payload);
            wobj_alloc(c, cb_id, WOBJ_CALLBACK, 0U);
            surf->frame_cb_id = cb_id;
        } else if (opcode == WL_SURFACE_COMMIT && surf) {
            surf->current_buf = surf->pending_buf;
            surf->fade_ticks = 8U;
            if (wld_focus_idx >= WLD_MAX_SURFACES || !wsurf_is_viewable(&wld_surfs[wld_focus_idx]))
                wld_focus_idx = si;
            wld_relayout(1);
            /* If xdg_surface not yet configured, do it now */
            if (surf->xdg_surf_obj && !surf->configured) {
                surf->configured = 1U;
                send_configure(c, surf);
            }
        } else if (opcode == WL_SURFACE_DESTROY && surf) {
            surf->alive = 0U;
            obj->alive  = 0U;
            wld_relayout(1);
        }
        return;
    }

    /* ── xdg_wm_base ── */
    if (obj->type == WOBJ_XDG_WM) {
        if (opcode == XDG_WM_GET_XDG_SURFACE && payload_len >= 8U) {
            uint32_t new_id  = in_u32(payload);
            uint32_t surf_id = in_u32(payload + 4U);
            wobj_t  *so      = wobj_find(c, surf_id);
            if (!so || so->type != WOBJ_SURFACE) return;
            uint32_t si      = so->extra;
            wobj_t  *xo      = wobj_alloc(c, new_id, WOBJ_XDG_SURF, (uint16_t)si);
            if (!xo) return;
            if (si < WLD_MAX_SURFACES)
                wld_surfs[si].xdg_surf_obj = new_id;
        } else if (opcode == XDG_WM_PONG) {
            /* acknowledged */
        }
        return;
    }

    /* ── xdg_surface ── */
    if (obj->type == WOBJ_XDG_SURF) {
        uint32_t si = obj->extra;
        if (opcode == XDG_SURFACE_GET_TOPLEVEL && payload_len >= 4U) {
            uint32_t new_id = in_u32(payload);
            wobj_alloc(c, new_id, WOBJ_XDG_TOP, (uint16_t)si);
            if (si < WLD_MAX_SURFACES) {
                wld_surfs[si].xdg_top_obj = new_id;
                if (!wld_surfs[si].configured) {
                    wld_surfs[si].configured = 1U;
                    send_configure(c, &wld_surfs[si]);
                }
            }
        } else if (opcode == XDG_SURFACE_ACK_CONFIGURE) {
            /* acknowledged */
        }
        return;
    }

    /* ── xdg_toplevel ── */
    if (obj->type == WOBJ_XDG_TOP) {
        uint32_t si = obj->extra;
        if (opcode == XDG_TOP_SET_TITLE && payload_len >= 4U) {
            char title[64];
            uint32_t cons = 0U;
            parse_str(payload, payload_len, title, sizeof(title), &cons);
            if (cons > 0U && si < WLD_MAX_SURFACES)
                wld_memcpy(wld_surfs[si].title, title, (long)(wld_strlen(title)+1));
        }
        return;
    }

    if (obj->type == WOBJ_ENLIL_WM) {
        if (opcode == ENLIL_WM_SET_LAYOUT && payload_len >= 4U) {
            uint32_t mode = in_u32(payload);
            if (mode == ENLIL_WM_LAYOUT_TILE)
                wld_layout_mode = mode;
            wld_relayout(1);
            wld_send_wm_state(c, obj_id);
        } else if (opcode == ENLIL_WM_FOCUS_NEXT) {
            wld_focus_rotate(+1);
            wld_send_wm_state(c, obj_id);
        } else if (opcode == ENLIL_WM_FOCUS_PREV) {
            wld_focus_rotate(-1);
            wld_send_wm_state(c, obj_id);
        } else if (opcode == ENLIL_WM_CLOSE_FOCUSED) {
            wld_request_close_focused();
            wld_send_wm_state(c, obj_id);
        } else if (opcode == ENLIL_WM_GET_STATE) {
            wld_send_wm_state(c, obj_id);
        }
        return;
    }
}

/* ── Parse and dispatch messages from client input buffer ───────── */
static void client_process_input(wclient_t *c)
{
    while (c->ilen >= 8U) {
        uint32_t obj_id  = in_u32(c->ibuf);
        uint32_t hdr2    = in_u32(c->ibuf + 4U);
        uint32_t size    = hdr2 >> 16U;
        uint32_t opcode  = hdr2 & 0xFFFFU;

        if (size < 8U) { c->ilen = 0U; return; } /* corrupt */
        if (c->ilen < size) break;                /* partial */

        uint32_t plen = size - 8U;
        dispatch_msg(c, obj_id, opcode, c->ibuf + 8U, plen);

        /* Shift buffer */
        c->ilen -= size;
        if (c->ilen > 0U)
            wld_memcpy(c->ibuf, c->ibuf + size, (long)c->ilen);
    }
}

/* ── Accept new client connection ───────────────────────────────── */
static void accept_clients(void)
{
    while (1) {
        long fd = wld_accept(wld_listen_fd, (void *)0, (void *)0);
        if (fd < 0) break; /* EAGAIN */

        /* Find free slot */
        uint32_t slot = 0xFFFFU;
        for (uint32_t i = 0U; i < WLD_MAX_CLIENTS; i++) {
            if (!wld_clients[i].alive) { slot = i; break; }
        }
        if (slot == 0xFFFFU) { wld_close((int)fd); continue; }

        wclient_t *c = &wld_clients[slot];
        wld_memset(c, 0, (long)sizeof(*c));
        c->fd     = (int)fd;
        c->alive  = 1U;
        c->serial = 1U;
        /* Create wl_display object (id=1) for this client */
        wobj_alloc(c, 1U, WOBJ_DISPLAY, 0U);
    }
}

/* ── Close client and cleanup surfaces ─────────────────────────── */
static void client_close(wclient_t *c)
{
    uint32_t obj_count;

    if (c->fd >= 0) wld_close(c->fd);
    /* Free surfaces belonging to this client */
    uint32_t ci = client_idx_of(c);
    for (uint32_t si = 0U; si < WLD_MAX_SURFACES; si++) {
        if (wld_surfs[si].alive && wld_surfs[si].client_idx == ci) {
            /* Detach pool buffers */
            uint32_t bi = wld_surfs[si].current_buf;
            if (bi < WLD_MAX_BUFFERS) wld_buffers[bi].alive = 0U;
            wld_surfs[si].alive = 0U;
        }
    }
    /* Detach pools owned by this client */
    obj_count = c->obj_count;
    if (obj_count > WLD_MAX_OBJS)
        obj_count = WLD_MAX_OBJS;
    for (uint32_t i = 0U; i < obj_count; i++) {
        if (c->objs[i].alive && c->objs[i].type == WOBJ_SHM_POOL) {
            uint32_t pi = c->objs[i].extra;
            if (pi < WLD_MAX_POOLS && wld_pools[pi].alive) {
                wld_shmdt(wld_pools[pi].ptr);
                wld_pools[pi].alive = 0U;
            }
        }
    }
    wld_memset(c, 0, (long)sizeof(*c));
    c->fd = -1;
    wld_relayout(1);
}

/* ── Create /run directory and socket ───────────────────────────── */
static void wld_setup_socket(void)
{
    wld_sockaddr_un_t sun;
    sun.sun_family = WLD_AF_UNIX;
    wld_strcpy(sun.sun_path, "/run/wayland-0");

    long fd = wld_socket(WLD_AF_UNIX, WLD_SOCK_STREAM | WLD_SOCK_NONBLOCK, 0);
    if (fd < 0) {
        wld_write_log("[WLD] socket() fallita\n");
        return;
    }

    long rc = wld_bind((int)fd, &sun, (int)(sizeof(sun.sun_family) +
                                             wld_strlen(sun.sun_path) + 1));
    if (rc < 0) {
        wld_write_log("[WLD] bind() fallita\n");
        wld_close((int)fd);
        return;
    }

    rc = wld_listen((int)fd, 8);
    if (rc < 0) {
        wld_write_log("[WLD] listen() fallita\n");
        wld_close((int)fd);
        return;
    }
    wld_listen_fd = (int)fd;
    wld_write_ready_marker();
    wld_write_log("[WLD] ascoltando su /run/wayland-0\n");
}

/* ── main ───────────────────────────────────────────────────────── */
static int wld_main(void)
{
    wld_memset(wld_clients, 0, (long)sizeof(wld_clients));
    wld_memset(wld_pools,   0, (long)sizeof(wld_pools));
    wld_memset(wld_buffers, 0, (long)sizeof(wld_buffers));
    wld_memset(wld_surfs,   0, (long)sizeof(wld_surfs));

    for (uint32_t i = 0U; i < WLD_MAX_CLIENTS; i++)
        wld_clients[i].fd = -1;

    /* Alloca compositing buffer */
    wld_composite = wld_mmap((long)WLD_FB_SZ);
    if (wld_composite == WLD_MAP_FAILED || (long)wld_composite < 0) {
        wld_write_log("[WLD] mmap compositing buffer fallita\n");
        wld_composite = WLD_MAP_FAILED;
    }

    /* Crea socket */
    wld_setup_socket();
    if (wld_listen_fd < 0) {
        wld_write_log("[WLD] inizializzazione fallita\n");
        user_svc_exit(1, SYS_EXIT);
    }

    /* Primo frame vuoto */
    composite_and_present();

    long last_frame_ms = wld_gettimeofday_ms();

    while (1) {
        /* Accetta nuovi client */
        accept_clients();

        /* Leggi messaggi dai client */
        for (uint32_t i = 0U; i < WLD_MAX_CLIENTS; i++) {
            wclient_t *c = &wld_clients[i];
            if (!c->alive || c->fd < 0) continue;

            uint32_t avail = (uint32_t)(sizeof(c->ibuf) - c->ilen);
            if (avail == 0U) { client_process_input(c); continue; }

            long n = wld_recv(c->fd, c->ibuf + c->ilen, (long)avail,
                              0x40 /* MSG_DONTWAIT */);
            if (n > 0) {
                c->ilen += (uint32_t)n;
                client_process_input(c);
            } else if (n == 0 || (n < 0 && n != -11 /* EAGAIN */)) {
                /* connessione chiusa o errore */
                client_close(c);
            }
        }

        /* Frame: ogni ~16ms componi e presenta */
        long now_ms = wld_gettimeofday_ms();
        if (now_ms - last_frame_ms >= 16L) {
            last_frame_ms = now_ms;
            composite_and_present();
            send_frame_dones();
        }

        /* Dorme davvero un tick per non affamare task meno prioritari. */
        wld_sleep_ms(1L);
    }
}

/* ── Entry point ────────────────────────────────────────────────── */
void _start(void) __attribute__((section(".text._start")));
void _start(void)
{
    (void)wld_main();
    user_svc_exit(0, WLD_SYS_YIELD);
}
