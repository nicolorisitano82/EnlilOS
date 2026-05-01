/*
 * wm_demo — client multi-window per validare M12-02
 *
 * Crea due toplevel Wayland distinti, commit di buffer colorati
 * e attende un evento xdg_toplevel.close dal WM oppure un breve timeout
 * dimostrativo.
 * Al close/timeout scrive /data/WMDEMO.TXT con "wm-demo-ok".
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

#define SURF_W  220
#define SURF_H  160
#define SURF_SZ (SURF_W * SURF_H * 4)

#define WL_DISPLAY_GET_REGISTRY  1U
#define WL_REGISTRY_BIND         0U
#define WL_REGISTRY_GLOBAL       0U
#define WL_COMPOSITOR_CREATE_SURFACE 0U
#define WL_SHM_CREATE_POOL_SYSV  0U
#define WL_SHM_POOL_CREATE_BUFFER 0U
#define WL_SURFACE_ATTACH        1U
#define WL_SURFACE_DAMAGE        2U
#define WL_SURFACE_COMMIT        6U
#define XDG_WM_GET_XDG_SURFACE   2U
#define XDG_SURFACE_GET_TOPLEVEL 1U
#define XDG_SURFACE_ACK_CONFIGURE 4U
#define XDG_TOP_SET_TITLE        2U
#define XDG_SURFACE_CONFIGURE    0U
#define XDG_TOPLEVEL_CLOSE       1U

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

static void fill_pattern(uint32_t *dst, uint32_t base, uint32_t accent)
{
    for (int y = 0; y < SURF_H; y++) {
        for (int x = 0; x < SURF_W; x++) {
            uint32_t color = base;
            if (x < 8 || y < 8 || x >= (SURF_W - 8) || y >= (SURF_H - 8))
                color = 0x00FFFFFFU;
            else if (((x / 16) + (y / 16)) & 1)
                color = accent;
            dst[y * SURF_W + x] = color;
        }
    }
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
        {
            struct timespec sl = { 0, 100000000L };
            nanosleep(&sl, NULL);
        }
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
        {
            struct timespec sl = { 0, 10000000L };
            nanosleep(&sl, NULL);
        }
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

static void create_window(wl_conn_t *conn,
                          uint32_t shm_id, uint32_t comp_id, uint32_t xdg_wm_id,
                          uint32_t pool_id, uint32_t buf_id,
                          uint32_t surf_id, uint32_t xsurf_id, uint32_t xtop_id,
                          int shmid, const char *title)
{
    msg_begin(conn, shm_id, WL_SHM_CREATE_POOL_SYSV, 12U);
    out_u32(conn, pool_id);
    out_u32(conn, (uint32_t)shmid);
    out_u32(conn, SURF_SZ);
    wl_flush(conn);

    msg_begin(conn, pool_id, WL_SHM_POOL_CREATE_BUFFER, 24U);
    out_u32(conn, buf_id);
    out_u32(conn, 0U);
    out_u32(conn, SURF_W);
    out_u32(conn, SURF_H);
    out_u32(conn, SURF_W * 4U);
    out_u32(conn, 1U);
    wl_flush(conn);

    msg_begin(conn, comp_id, WL_COMPOSITOR_CREATE_SURFACE, 4U);
    out_u32(conn, surf_id);
    wl_flush(conn);

    msg_begin(conn, xdg_wm_id, XDG_WM_GET_XDG_SURFACE, 8U);
    out_u32(conn, xsurf_id);
    out_u32(conn, surf_id);
    wl_flush(conn);

    msg_begin(conn, xsurf_id, XDG_SURFACE_GET_TOPLEVEL, 4U);
    out_u32(conn, xtop_id);
    wl_flush(conn);

    msg_begin(conn, xtop_id, XDG_TOP_SET_TITLE,
              4U + (((uint32_t)strlen(title) + 1U + 3U) & ~3U));
    out_str(conn, title);
    wl_flush(conn);

    msg_begin(conn, surf_id, WL_SURFACE_COMMIT, 0U);
    wl_flush(conn);
}

static void ack_configure(wl_conn_t *conn, uint32_t xsurf_id, uint32_t serial)
{
    msg_begin(conn, xsurf_id, XDG_SURFACE_ACK_CONFIGURE, 4U);
    out_u32(conn, serial);
    wl_flush(conn);
}

static void commit_buffer(wl_conn_t *conn, uint32_t surf_id, uint32_t buf_id)
{
    msg_begin(conn, surf_id, WL_SURFACE_ATTACH, 12U);
    out_u32(conn, buf_id);
    out_u32(conn, 0U);
    out_u32(conn, 0U);

    msg_begin(conn, surf_id, WL_SURFACE_DAMAGE, 16U);
    out_u32(conn, 0U); out_u32(conn, 0U);
    out_u32(conn, SURF_W); out_u32(conn, SURF_H);

    msg_begin(conn, surf_id, WL_SURFACE_COMMIT, 0U);
    wl_flush(conn);
}

static void write_demo_ok(void)
{
    int fd = open("/data/WMDEMO.TXT", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "wm-demo-ok\n", 11);
        close(fd);
    }
}

static void sleep_10ms(void)
{
    struct timespec sl = { 0, 10000000L };
    nanosleep(&sl, NULL);
}

int main(void)
{
    wl_conn_t conn;
    uint32_t comp_name, shm_name, xdg_name;
    uint32_t *buf1 = NULL, *buf2 = NULL;
    int shmid1 = -1, shmid2 = -1;
    uint32_t serial1 = 0U, serial2 = 0U;
    int cfg1 = 0, cfg2 = 0;

    if (wl_connect_runtime(&conn, &comp_name, &shm_name, &xdg_name) < 0) {
        write(2, "wmdemo: connect fail\n", 21);
        return 1;
    }
    write(2, "[WMDEMO] connected\n", 19);

    bind_global(&conn, comp_name, "wl_compositor", 4U, 3U);
    bind_global(&conn, shm_name, "wl_shm", 1U, 4U);
    bind_global(&conn, xdg_name, "xdg_wm_base", 1U, 5U);

    shmid1 = shmget(IPC_PRIVATE, SURF_SZ, IPC_CREAT | 0600);
    shmid2 = shmget(IPC_PRIVATE, SURF_SZ, IPC_CREAT | 0600);
    if (shmid1 < 0 || shmid2 < 0)
        return 2;
    buf1 = (uint32_t *)shmat(shmid1, NULL, 0);
    buf2 = (uint32_t *)shmat(shmid2, NULL, 0);
    if ((long)buf1 == -1L || (long)buf2 == -1L)
        return 3;
    fill_pattern(buf1, 0x00FF5A36U, 0x00FFD6CCU);
    fill_pattern(buf2, 0x0036B8FFU, 0x00D9F1FFU);

    create_window(&conn, 4U, 3U, 5U, 6U, 7U, 8U, 9U, 10U, shmid1, "WM One");
    create_window(&conn, 4U, 3U, 5U, 11U, 12U, 13U, 14U, 15U, shmid2, "WM Two");

    {
        int polls = 400; /* ~4s a passi da 10ms */
        while (polls-- > 0 && (!cfg1 || !cfg2)) {
            wl_event_t ev;
            ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                             sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
            if (n > 0)
                conn.ilen += (uint32_t)n;

            while (wl_next_event(&conn, &ev)) {
                if (ev.opcode == XDG_SURFACE_CONFIGURE && ev.plen >= 4U) {
                    if (ev.obj_id == 9U) {
                        serial1 = in_u32(ev.payload);
                        cfg1 = 1;
                    } else if (ev.obj_id == 14U) {
                        serial2 = in_u32(ev.payload);
                        cfg2 = 1;
                    }
                }
            }

            if (!cfg1 || !cfg2) {
                sleep_10ms();
            }
        }
    }

    if (!cfg1 || !cfg2) {
        write(2, "wmdemo: configure timeout\n", 26);
        return 4;
    }
    write(2, "[WMDEMO] configured\n", 20);

    ack_configure(&conn, 9U, serial1);
    ack_configure(&conn, 14U, serial2);
    commit_buffer(&conn, 8U, 7U);
    commit_buffer(&conn, 13U, 12U);

    {
        int polls = 400; /* ~4s a passi da 10ms */
        while (polls-- > 0) {
            wl_event_t ev;
            ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                             sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
            if (n > 0)
                conn.ilen += (uint32_t)n;

            while (wl_next_event(&conn, &ev)) {
                if ((ev.obj_id == 10U || ev.obj_id == 15U) &&
                    ev.opcode == XDG_TOPLEVEL_CLOSE) {
                    write(2, "[WMDEMO] close received\n", 24);
                    write_demo_ok();
                    shmdt(buf1);
                    shmdt(buf2);
                    close(conn.fd);
                    return 0;
                }
            }

            sleep_10ms();
        }
    }

    write(2, "[WMDEMO] timeout complete\n", 26);
    write_demo_ok();
    shmdt(buf1);
    shmdt(buf2);
    close(conn.fd);
    return 0;
}
