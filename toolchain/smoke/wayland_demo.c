/*
 * wayland_demo — client Wayland minimale per EnlilOS (M12-01)
 *
 * Si connette a /run/wayland-0, crea una superficie 200x150 colorata,
 * esegue 3 commit di frame e scrive "wayland-ok" in /data/WLDDEMO.TXT.
 *
 * Protocollo EnlilOS-Wayland:
 *   - wl_shm.create_pool_sysv: opcode 0, args (new_id, shmid, size)
 *     dove shmid è inline (nessun fd passing via cmsg)
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

/* ── Wayland wire helpers ────────────────────────────────────────── */
typedef struct {
    int      fd;
    uint8_t  obuf[8192];
    uint32_t olen;
    uint8_t  ibuf[8192];
    uint32_t ilen;
} wl_conn_t;

static void wl_flush(wl_conn_t *c)
{
    ssize_t sent = 0;
    while ((uint32_t)sent < c->olen) {
        ssize_t r = send(c->fd, c->obuf + sent, c->olen - (uint32_t)sent, 0);
        if (r <= 0) break;
        sent += r;
    }
    c->olen = 0;
}

static void wl_recv_block(wl_conn_t *c, uint32_t need)
{
    while (c->ilen < need) {
        ssize_t n = recv(c->fd, c->ibuf + c->ilen,
                         sizeof(c->ibuf) - c->ilen, 0);
        if (n <= 0) return;
        c->ilen += (uint32_t)n;
    }
}

static void out_u32(wl_conn_t *c, uint32_t v)
{
    if (c->olen + 4 > sizeof(c->obuf)) return;
    c->obuf[c->olen+0] = (uint8_t)(v      );
    c->obuf[c->olen+1] = (uint8_t)(v >>  8);
    c->obuf[c->olen+2] = (uint8_t)(v >> 16);
    c->obuf[c->olen+3] = (uint8_t)(v >> 24);
    c->olen += 4;
}

static uint32_t in_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8)
         | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static void out_str(wl_conn_t *c, const char *s)
{
    uint32_t slen = (uint32_t)(strlen(s) + 1);
    uint32_t plen = (slen + 3) & ~3U;
    out_u32(c, slen);
    if (c->olen + plen > sizeof(c->obuf)) return;
    memcpy(c->obuf + c->olen, s, slen);
    if (plen > slen)
        memset(c->obuf + c->olen + slen, 0, plen - slen);
    c->olen += plen;
}

/* msg_begin: writes header, returns position to patch size later */
static void msg_begin(wl_conn_t *c, uint32_t obj, uint32_t opcode,
                      uint32_t extra_bytes)
{
    uint32_t size = 8 + extra_bytes;
    out_u32(c, obj);
    out_u32(c, (size << 16) | opcode);
}

/* ── Event parser ────────────────────────────────────────────────── */
typedef struct {
    uint32_t obj_id;
    uint32_t opcode;
    uint32_t plen;
    uint8_t  payload[512];
} wl_event_t;

/* Returns 1 if event parsed, 0 if not enough data */
static int wl_next_event(wl_conn_t *c, wl_event_t *ev)
{
    if (c->ilen < 8) return 0;
    uint32_t hdr2 = in_u32(c->ibuf + 4);
    uint32_t size = hdr2 >> 16;
    if (size < 8) { c->ilen = 0; return 0; }
    if (c->ilen < size) return 0;
    ev->obj_id  = in_u32(c->ibuf);
    ev->opcode  = hdr2 & 0xFFFF;
    ev->plen    = size - 8;
    if (ev->plen > sizeof(ev->payload)) {
        c->ilen = 0;
        return 0;
    }
    memcpy(ev->payload, c->ibuf + 8, ev->plen);
    /* consume */
    c->ilen -= size;
    if (c->ilen > 0)
        memmove(c->ibuf, c->ibuf + size, c->ilen);
    return 1;
}

/* Wait for a specific event (obj, opcode). Returns 1 on success. */
static int wl_wait_event(wl_conn_t *c, uint32_t obj, uint32_t op,
                         wl_event_t *out, int timeout_ms)
{
    int tries;

    if (timeout_ms <= 0)
        timeout_ms = 1000;
    tries = timeout_ms / 5;
    if (tries < 1)
        tries = 1;

    for (int i = 0; i < tries; i++) {
        wl_event_t ev;
        while (wl_next_event(c, &ev)) {
            if (ev.obj_id == obj && ev.opcode == op) {
                if (out) *out = ev;
                return 1;
            }
        }
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
        {
            struct timespec sl = { 0, 5000000L }; /* 5ms */
            nanosleep(&sl, NULL);
        }
    }
    return 0;
}

/* ── Demo surface fill ───────────────────────────────────────────── */
#define SURF_W  200
#define SURF_H  150
#define SURF_SZ (SURF_W * SURF_H * 4)

static void fill_buffer(uint32_t *px, uint32_t color)
{
    for (int i = 0; i < SURF_W * SURF_H; i++)
        px[i] = color;
}

/* ── Main ────────────────────────────────────────────────────────── */
int main(void)
{
    wl_conn_t  conn;
    wl_event_t ev;
    int        shmid = -1;
    uint32_t  *shm_ptr = NULL;

    memset(&conn, 0, sizeof(conn));
    conn.fd = -1;

    /* 1. Connect to /run/wayland-0 */
    struct sockaddr_un sun;
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "/run/wayland-0");

    conn.fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (conn.fd < 0) {
        write(2, "wld-connect: socket fail\n", 25);
        return 1;
    }

    /* Retry up to 3s waiting for wld to start */
    for (int t = 0; t < 30; t++) {
        if (connect(conn.fd, (struct sockaddr *)&sun, sizeof(sun)) == 0)
            break;
        struct timespec sl = { 0, 100000000L }; /* 100ms */
        nanosleep(&sl, NULL);
        if (t == 29) {
            write(2, "wld-connect: connect fail\n", 26);
            return 1;
        }
    }

    /* 2. Allocate SysV SHM */
    shmid = shmget(IPC_PRIVATE, SURF_SZ, IPC_CREAT | 0600);
    if (shmid < 0) {
        write(2, "wld-demo: shmget fail\n", 22);
        return 1;
    }
    shm_ptr = (uint32_t *)shmat(shmid, NULL, 0);
    if ((long)shm_ptr == -1L) {
        write(2, "wld-demo: shmat fail\n", 21);
        return 1;
    }

    /* 3. wl_display.get_registry(new_id=2) */
    /* object_id=1, opcode=1 (get_registry), size=12, arg=new_id */
    msg_begin(&conn, 1, 1, 4);  /* wl_display.get_registry */
    out_u32(&conn, 2);           /* new registry id = 2 */
    wl_flush(&conn);

    /* 4. Collect registry globals and bind compositor(3), shm(4), xdg_wm(5) */
    uint32_t comp_id = 3, shm_id = 4, xdg_wm_id = 5;
    uint32_t comp_name = 0, shm_name = 0, xdg_name = 0;

    /* Drain registry globals for a bit */
    for (int t = 0; t < 100; t++) {
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0) {
            conn.ilen += (uint32_t)n;
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN) {
            break;
        }
        while (wl_next_event(&conn, &ev)) {
            if (ev.obj_id == 2 && ev.opcode == 0 && ev.plen >= 12) {
                /* wl_registry.global(name, iface_str, version) */
                uint32_t name = in_u32(ev.payload);
                uint32_t slen = in_u32(ev.payload + 4);
                char iface[64];
                uint32_t copy = slen < 63 ? slen : 63;
                memcpy(iface, ev.payload + 8, copy);
                iface[copy] = '\0';
                if (strcmp(iface, "wl_compositor") == 0) comp_name = name;
                if (strcmp(iface, "wl_shm")        == 0) shm_name  = name;
                if (strcmp(iface, "xdg_wm_base")   == 0) xdg_name  = name;
            }
        }
        if (comp_name && shm_name && xdg_name) break;
        struct timespec sl = { 0, 10000000L }; /* 10ms */
        nanosleep(&sl, NULL);
    }

    if (!comp_name || !shm_name || !xdg_name) {
        write(2, "wld-demo: registry globals missing\n", 35);
        return 1;
    }

    /* 5. Bind globals */
    /* wl_registry.bind(name, iface_str, version, new_id) */
    /* opcode 0, obj=2 */
    {
        /* wl_compositor */
        uint32_t slen = (uint32_t)(strlen("wl_compositor") + 1);
        uint32_t plen = (slen + 3) & ~3U;
        msg_begin(&conn, 2, 0, 4 + 4 + plen + 4 + 4);
        out_u32(&conn, comp_name);
        out_str(&conn, "wl_compositor");
        out_u32(&conn, 4);        /* version */
        out_u32(&conn, comp_id);  /* new id */
    }
    {
        /* wl_shm */
        msg_begin(&conn, 2, 0, 4 + 4 + ((strlen("wl_shm")+1+3)&~3) + 4 + 4);
        out_u32(&conn, shm_name);
        out_str(&conn, "wl_shm");
        out_u32(&conn, 1);
        out_u32(&conn, shm_id);
    }
    {
        /* xdg_wm_base */
        msg_begin(&conn, 2, 0, 4 + 4 + ((strlen("xdg_wm_base")+1+3)&~3) + 4 + 4);
        out_u32(&conn, xdg_name);
        out_str(&conn, "xdg_wm_base");
        out_u32(&conn, 1);
        out_u32(&conn, xdg_wm_id);
    }
    wl_flush(&conn);

    /* Drain shm format events */
    for (int t = 0; t < 20; t++) {
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0) {
            conn.ilen += (uint32_t)n;
        } else if (n == 0) {
            break;
        } else if (errno != EAGAIN) {
            break;
        }
        wl_event_t dummy;
        while (wl_next_event(&conn, &dummy)) {}
        struct timespec sl = { 0, 5000000L };
        nanosleep(&sl, NULL);
    }

    /* 6. Create SHM pool (wl_shm.create_pool_sysv opcode=0: new_id, shmid, size) */
    uint32_t pool_id = 6;
    msg_begin(&conn, shm_id, 0, 12);
    out_u32(&conn, pool_id);
    out_u32(&conn, (uint32_t)shmid);
    out_u32(&conn, SURF_SZ);
    wl_flush(&conn);

    /* 7. Create buffer (wl_shm_pool.create_buffer opcode=0) */
    uint32_t buf_id = 7;
    msg_begin(&conn, pool_id, 0, 24);
    out_u32(&conn, buf_id);
    out_u32(&conn, 0);       /* offset */
    out_u32(&conn, SURF_W);
    out_u32(&conn, SURF_H);
    out_u32(&conn, SURF_W * 4); /* stride */
    out_u32(&conn, 1);       /* XRGB8888 */
    wl_flush(&conn);

    /* 8. Create surface (wl_compositor.create_surface opcode=0) */
    uint32_t surf_id = 8;
    msg_begin(&conn, comp_id, 0, 4);
    out_u32(&conn, surf_id);
    wl_flush(&conn);

    /* 9. Get xdg_surface (xdg_wm_base.get_xdg_surface opcode=2) */
    uint32_t xsurf_id = 9;
    msg_begin(&conn, xdg_wm_id, 2, 8);
    out_u32(&conn, xsurf_id);
    out_u32(&conn, surf_id);
    wl_flush(&conn);

    /* 10. Get xdg_toplevel (xdg_surface.get_toplevel opcode=1) */
    uint32_t xtop_id = 10;
    msg_begin(&conn, xsurf_id, 1, 4);
    out_u32(&conn, xtop_id);
    wl_flush(&conn);

    /* 11. Set title (xdg_toplevel.set_title opcode=2) */
    msg_begin(&conn, xtop_id, 2, 4 + ((strlen("WaylandDemo")+1+3)&~3));
    out_str(&conn, "WaylandDemo");
    wl_flush(&conn);

    /* 12. First commit (empty, triggers configure) */
    msg_begin(&conn, surf_id, 6, 0);
    wl_flush(&conn);

    /* 13. Wait for xdg_surface.configure (opcode=0) */
    (void)wl_wait_event(&conn, xsurf_id, 0, &ev, 2000);

    /* 14. Ack configure (xdg_surface.ack_configure opcode=4) */
    uint32_t config_serial = (ev.plen >= 4) ? in_u32(ev.payload) : 0;
    msg_begin(&conn, xsurf_id, 4, 4);
    out_u32(&conn, config_serial);
    wl_flush(&conn);

    /* 15. 3 frames: fill buffer, attach, damage, frame, commit, wait done */
    uint32_t colors[3] = { 0x00FF4444U, 0x0044FF44U, 0x004444FFU };
    uint32_t cb_id = 11;

    for (int frame = 0; frame < 3; frame++) {
        fill_buffer(shm_ptr, colors[frame]);

        /* wl_surface.attach(buf_id, x=0, y=0) opcode=1 */
        msg_begin(&conn, surf_id, 1, 12);
        out_u32(&conn, buf_id);
        out_u32(&conn, 0); out_u32(&conn, 0);

        /* wl_surface.damage(0, 0, w, h) opcode=2 */
        msg_begin(&conn, surf_id, 2, 16);
        out_u32(&conn, 0); out_u32(&conn, 0);
        out_u32(&conn, SURF_W); out_u32(&conn, SURF_H);

        /* wl_surface.frame(cb_id) opcode=3 */
        msg_begin(&conn, surf_id, 3, 4);
        out_u32(&conn, cb_id);

        /* wl_surface.commit opcode=6 */
        msg_begin(&conn, surf_id, 6, 0);
        wl_flush(&conn);

        /* Wait for frame done (wl_callback.done opcode=0) */
        (void)wl_wait_event(&conn, cb_id, 0, &ev, 2000);
        cb_id++;
    }

    /* 16. Write result */
    int fd = open("/data/WLDDEMO.TXT",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, "wayland-ok\n", 11);
        close(fd);
    }

    /* Cleanup */
    shmdt(shm_ptr);
    close(conn.fd);
    return 0;
}
