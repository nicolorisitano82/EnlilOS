/*
 * wm_manager — window manager controller v1 per wld (M12-02)
 *
 * Modalità normale:
 *   - si connette a /run/wayland-0
 *   - bind di enlil_wm_v1
 *   - imposta layout TILE e resta residente
 *
 * Modalità selftest (--selftest):
 *   - attende almeno 2 finestre mappate
 *   - esegue focus_next
 *   - invia close_focused
 *   - scrive /data/WMSTATE.TXT con lo stato osservato
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

typedef struct {
    int      fd;
    uint8_t  obuf[4096];
    uint32_t olen;
    uint8_t  ibuf[4096];
    uint32_t ilen;
} wl_conn_t;

typedef struct {
    uint32_t obj_id;
    uint32_t opcode;
    uint32_t plen;
    uint8_t  payload[128];
} wl_event_t;

#define WL_DISPLAY_GET_REGISTRY  1U
#define WL_REGISTRY_BIND         0U
#define WL_REGISTRY_GLOBAL       0U

#define ENLIL_WM_STATE           0U
#define ENLIL_WM_SET_LAYOUT      0U
#define ENLIL_WM_FOCUS_NEXT      1U
#define ENLIL_WM_CLOSE_FOCUSED   3U
#define ENLIL_WM_GET_STATE       4U
#define ENLIL_WM_SET_FOCUS_POLICY 5U
#define ENLIL_WM_LAYOUT_TILE     1U
#define ENLIL_WM_FOCUS_CLICK     0U
#define ENLIL_WM_FOCUS_POINTER   1U

static void wl_flush(wl_conn_t *c)
{
    ssize_t sent = 0;

    while ((uint32_t)sent < c->olen) {
        ssize_t r = send(c->fd, c->obuf + sent, c->olen - (uint32_t)sent,
                         MSG_DONTWAIT);
        if (r <= 0)
            break;
        sent += r;
    }
    if (sent > 0 && (uint32_t)sent < c->olen)
        memmove(c->obuf, c->obuf + sent, c->olen - (uint32_t)sent);
    c->olen -= (uint32_t)sent;
}

static void out_u32(wl_conn_t *c, uint32_t v)
{
    if (c->olen + 4U > sizeof(c->obuf))
        return;
    c->obuf[c->olen + 0U] = (uint8_t)(v);
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

static int wl_wait_state(wl_conn_t *c, uint32_t wm_id,
                         uint32_t *layout, uint32_t *count,
                         uint32_t *focus, uint32_t *present,
                         int timeout_ms)
{
    int tries = timeout_ms / 5;

    if (tries < 1)
        tries = 1;
    for (int i = 0; i < tries; i++) {
        wl_event_t ev;
        while (wl_next_event(c, &ev)) {
            if (ev.obj_id == wm_id && ev.opcode == ENLIL_WM_STATE && ev.plen >= 16U) {
                if (layout)  *layout  = in_u32(ev.payload + 0U);
                if (count)   *count   = in_u32(ev.payload + 4U);
                if (focus)   *focus   = in_u32(ev.payload + 8U);
                if (present) *present = in_u32(ev.payload + 12U);
                return 1;
            }
        }

        {
            ssize_t n = recv(c->fd, c->ibuf + c->ilen,
                             sizeof(c->ibuf) - c->ilen, MSG_DONTWAIT);
            if (n > 0) {
                c->ilen += (uint32_t)n;
                continue;
            }
            if (n == 0)
                return 0;
            if (errno != EAGAIN)
                return 0;
        }

        {
            struct timespec sl = { 0, 5000000L };
            nanosleep(&sl, NULL);
        }
    }

    return 0;
}

static int wm_connect(wl_conn_t *conn, uint32_t *out_wm_name)
{
    struct sockaddr_un sun;
    uint32_t wm_name = 0U;

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
        {
            struct timespec sl = { 0, 100000000L };
            nanosleep(&sl, NULL);
        }
    }

    msg_begin(conn, 1U, WL_DISPLAY_GET_REGISTRY, 4U);
    out_u32(conn, 2U);
    wl_flush(conn);

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
                if (strcmp(iface, "enlil_wm_v1") == 0)
                    wm_name = name;
            }
        }

        if (wm_name != 0U)
            break;
        {
            struct timespec sl = { 0, 10000000L };
            nanosleep(&sl, NULL);
        }
    }

    if (wm_name == 0U)
        return -1;
    *out_wm_name = wm_name;
    return 0;
}

static void wm_bind(wl_conn_t *conn, uint32_t wm_name, uint32_t wm_id)
{
    msg_begin(conn, 2U, WL_REGISTRY_BIND,
              4U + 4U + (((uint32_t)strlen("enlil_wm_v1") + 1U + 3U) & ~3U) + 4U + 4U);
    out_u32(conn, wm_name);
    out_str(conn, "enlil_wm_v1");
    out_u32(conn, 1U);
    out_u32(conn, wm_id);
    wl_flush(conn);
}

static void wm_set_layout(wl_conn_t *conn, uint32_t wm_id, uint32_t mode)
{
    msg_begin(conn, wm_id, ENLIL_WM_SET_LAYOUT, 4U);
    out_u32(conn, mode);
    wl_flush(conn);
}

static void wm_get_state(wl_conn_t *conn, uint32_t wm_id)
{
    msg_begin(conn, wm_id, ENLIL_WM_GET_STATE, 0U);
    wl_flush(conn);
}

static void wm_set_focus_policy(wl_conn_t *conn, uint32_t wm_id, uint32_t mode)
{
    msg_begin(conn, wm_id, ENLIL_WM_SET_FOCUS_POLICY, 4U);
    out_u32(conn, mode);
    wl_flush(conn);
}

static void wm_focus_next(wl_conn_t *conn, uint32_t wm_id)
{
    msg_begin(conn, wm_id, ENLIL_WM_FOCUS_NEXT, 0U);
    wl_flush(conn);
}

static void wm_close_focused(wl_conn_t *conn, uint32_t wm_id)
{
    msg_begin(conn, wm_id, ENLIL_WM_CLOSE_FOCUSED, 0U);
    wl_flush(conn);
}

static void wm_write_state(uint32_t windows, uint32_t focus_before, uint32_t focus_after)
{
    char buf[128];
    int fd;
    int len;

    len = snprintf(buf, sizeof(buf),
                   "wm-ok layout=tile windows=%u focus_before=%u focus_after=%u\n",
                   windows, focus_before, focus_after);
    fd = open("/data/WMSTATE.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, buf, (size_t)len);
        close(fd);
    }
}

int main(int argc, char **argv)
{
    wl_conn_t conn;
    uint32_t wm_name = 0U;
    const uint32_t wm_id = 3U;
    int selftest = (argc > 1 && strcmp(argv[1], "--selftest") == 0);

    if (wm_connect(&conn, &wm_name) < 0) {
        write(2, "wm: connect fail\n", 17);
        return 1;
    }
    write(2, "[WM] connected\n", 15);

    wm_bind(&conn, wm_name, wm_id);
    wm_set_layout(&conn, wm_id, ENLIL_WM_LAYOUT_TILE);
    wm_set_focus_policy(&conn, wm_id, ENLIL_WM_FOCUS_POINTER);
    write(2, "[WM] bound + tile\n", 18);

    if (selftest) {
        uint64_t deadline = (uint64_t)time(NULL) + 6ULL;

        while ((uint64_t)time(NULL) < deadline) {
            uint32_t layout = 0U, count = 0U, focus_before = 0U, present = 0U, focus_after = 0U;

            wm_get_state(&conn, wm_id);
            if (!wl_wait_state(&conn, wm_id, &layout, &count, &focus_before, &present, 500))
                continue;
            {
                char dbg[64];
                int len = snprintf(dbg, sizeof(dbg),
                                   "[WM] state layout=%u count=%u focus=%u\n",
                                   layout, count, focus_before);
                write(2, dbg, (size_t)len);
            }
            if (layout != ENLIL_WM_LAYOUT_TILE || count < 2U) {
                struct timespec sl = { 0, 20000000L };
                nanosleep(&sl, NULL);
                continue;
            }

            wm_focus_next(&conn, wm_id);
            if (!wl_wait_state(&conn, wm_id, &layout, &count, &focus_after, &present, 500))
                continue;
            wm_close_focused(&conn, wm_id);
            write(2, "[WM] focus+close sent\n", 22);
            wm_write_state(count, focus_before, focus_after);
            close(conn.fd);
            return 0;
        }

        write(2, "wm: selftest timeout\n", 21);
        close(conn.fd);
        return 2;
    }

    for (;;) {
        struct timespec sl = { 0, 250000000L };
        nanosleep(&sl, NULL);
    }
}
