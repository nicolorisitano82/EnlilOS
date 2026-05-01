/*
 * EnlilOS Desktop Shell — M12-02
 *
 * Due superfici su una connessione Wayland:
 *   dock   — 800x56,  app_id="enlil-dock",   nessuna decorazione wld, fissa in basso
 *   finder — 640x420, app_id="enlil-finder",  finestra floating stile Finder macOS
 *
 * Registra enlil_pointer_v1 per ricevere eventi mouse assoluti.
 * Scritto /data/HELLOREADY.TXT quando entrambe le superfici sono pronte.
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <dirent.h>

/* ── Wayland opcodes ─────────────────────────────────────────────── */
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
#define ENLIL_POINTER_EVENT          0U

#define SYS_CACHE_FLUSH   228U

/* ── Layout dock ─────────────────────────────────────────────────── */
#define DOCK_W     800
#define DOCK_H      56
#define DOCK_SZ    (DOCK_W * DOCK_H * 4)
#define DOCK_NBTN    3
#define DOCK_BTN_W  120
#define DOCK_BTN_H   40
#define DOCK_BTN_GAP  16
/* x del primo bottone: centrato */
#define DOCK_BTN_X0  ((DOCK_W - (DOCK_NBTN * DOCK_BTN_W + (DOCK_NBTN-1) * DOCK_BTN_GAP)) / 2)
#define DOCK_BTN_Y0  ((DOCK_H - DOCK_BTN_H) / 2)

/* ── Layout finder ───────────────────────────────────────────────── */
#define FIND_W       640
#define FIND_H       420
#define FIND_SZ     (FIND_W * FIND_H * 4)
#define FIND_TBAR_H   32    /* toolbar altezza */
#define FIND_ROW_H    26    /* riga file list */
#define FIND_STAT_H   28    /* status bar bottom */
#define FIND_LIST_Y   33    /* y inizio lista file nel buffer */
#define FIND_MAX_ROWS 13
#define FIND_MAX_ENT  64

/* ── Posizione schermo iniziale (wld centra finder) ─────────────── */
/* frame = FIND_W + 2*border(2) = 644, FIND_H + title_h(22) + 2*border(2) = 446 */
#define FIND_BORDER     2
#define FIND_TITLE_H   22
#define FIND_FRAME_W   (FIND_W + FIND_BORDER * 2)
#define FIND_FRAME_H   (FIND_H + FIND_TITLE_H + FIND_BORDER * 2)
/* coordinate schermo contenuto finder (centro FB 800x600) */
#define FIND_SCRN_X    ((800 - FIND_FRAME_W) / 2 + FIND_BORDER)
#define FIND_SCRN_Y    ((600 - FIND_FRAME_H) / 2 + FIND_TITLE_H + FIND_BORDER)
/* Y dock su schermo */
#define DOCK_SCRN_Y    (600 - DOCK_H)

/* ── Colori ──────────────────────────────────────────────────────── */
#define COL_DOCK_BG       0x00111827U
#define COL_BTN_TER       0x0022AA66U   /* verde  — terminale */
#define COL_BTN_FIL       0x00CC9922U   /* giallo — file */
#define COL_BTN_SET       0x00445566U   /* grigio — impostazioni */
#define COL_BTN_HOV       0x00FFFFFFU   /* highlight hover */
#define COL_FIND_BG       0x00F2F3F5U
#define COL_FIND_TBAR     0x00DDE0E6U
#define COL_FIND_TBAR_TXT 0x00222233U
#define COL_FIND_SEP      0x00BCC0CAU
#define COL_SBAR_BG       0x00C8CDD6U
#define COL_ICON_DIR      0x00DDAA33U
#define COL_ICON_FILE     0x00A0A8C0U
#define COL_ROW_SEL       0x003478F0U
#define COL_ROW_ALT       0x00E8EAEFU
#define COL_ROW_TXT       0x00111111U
#define COL_ROW_SEL_TXT   0x00FFFFFFU
#define COL_STAT_TXT      0x00444444U

/* ── Ready paths ─────────────────────────────────────────────────── */
#define READY_PATH  "/data/HELLOREADY.TXT"
#define FAIL_PATH   "/data/HELLOFAIL.TXT"

/* ── Wayland conn ────────────────────────────────────────────────── */
typedef struct {
    int      fd;
    uint8_t  obuf[16384];
    uint32_t olen;
    uint8_t  ibuf[16384];
    uint32_t ilen;
} wl_conn_t;

typedef struct {
    uint32_t obj_id;
    uint32_t opcode;
    uint32_t plen;
    uint8_t  payload[512];
} wl_event_t;

#include "wterm_font.inc"

/* ── Helper ──────────────────────────────────────────────────────── */
static void write_marker(const char *path, const char *text)
{
    int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) return;
    write(fd, text, strlen(text));
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
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static void flush_pixels(void *ptr, uint32_t size)
{
    if (ptr && size > 0U)
        (void)raw_cache_flush(ptr, size);
}

/* ── Wire protocol ───────────────────────────────────────────────── */
static void out_u32(wl_conn_t *c, uint32_t v)
{
    if (c->olen + 4U > sizeof(c->obuf)) return;
    c->obuf[c->olen+0] = (uint8_t)(v);
    c->obuf[c->olen+1] = (uint8_t)(v >> 8);
    c->obuf[c->olen+2] = (uint8_t)(v >> 16);
    c->obuf[c->olen+3] = (uint8_t)(v >> 24);
    c->olen += 4U;
}

static uint32_t in_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

static void out_str(wl_conn_t *c, const char *s)
{
    uint32_t slen = (uint32_t)(strlen(s) + 1U);
    uint32_t plen = (slen + 3U) & ~3U;
    out_u32(c, slen);
    if (c->olen + plen > sizeof(c->obuf)) return;
    memcpy(c->obuf + c->olen, s, slen);
    if (plen > slen) memset(c->obuf + c->olen + slen, 0, plen - slen);
    c->olen += plen;
}

static void msg_begin(wl_conn_t *c, uint32_t obj, uint32_t opcode, uint32_t extra)
{
    out_u32(c, obj);
    out_u32(c, ((8U + extra) << 16) | opcode);
}

static void wl_flush(wl_conn_t *c)
{
    ssize_t sent = 0;
    while ((uint32_t)sent < c->olen) {
        ssize_t r = send(c->fd, c->obuf + sent, c->olen - (uint32_t)sent, 0);
        if (r <= 0) break;
        sent += r;
    }
    c->olen = 0U;
}

static int wl_next_event(wl_conn_t *c, wl_event_t *ev)
{
    if (c->ilen < 8U) return 0;
    uint32_t hdr2 = in_u32(c->ibuf + 4U);
    uint32_t size = hdr2 >> 16;
    if (size < 8U || c->ilen < size) return 0;
    ev->obj_id = in_u32(c->ibuf);
    ev->opcode = hdr2 & 0xFFFFU;
    ev->plen = size - 8U;
    if (ev->plen > sizeof(ev->payload)) return 0;
    memcpy(ev->payload, c->ibuf + 8U, ev->plen);
    c->ilen -= size;
    if (c->ilen > 0U) memmove(c->ibuf, c->ibuf + size, c->ilen);
    return 1;
}

static void bind_global(wl_conn_t *c, uint32_t name,
                        const char *iface, uint32_t ver, uint32_t new_id)
{
    uint32_t slen = (uint32_t)(strlen(iface) + 1U);
    uint32_t plen = (slen + 3U) & ~3U;
    msg_begin(c, 2U, WL_REGISTRY_BIND, 4U + 4U + plen + 4U + 4U);
    out_u32(c, name);
    out_str(c, iface);
    out_u32(c, ver);
    out_u32(c, new_id);
    wl_flush(c);
}

/* ── Pixel draw helpers ──────────────────────────────────────────── */
static void fill_rect(uint32_t *dst, int x, int y, int w, int h,
                      uint32_t col, int surf_w, int surf_h)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= surf_h) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= surf_w) continue;
            dst[yy * surf_w + xx] = col;
        }
    }
}

static void draw_glyph(uint32_t *dst, int x, int y, char ch,
                       uint32_t fg, uint32_t bg, int surf_w, int surf_h)
{
    if (ch < 32 || ch > 126) ch = '?';
    const uint8_t *g = font_8x16[(uint8_t)ch - 32U];
    for (int row = 0; row < 16; row++) {
        int py = y + row;
        if (py < 0 || py >= surf_h) continue;
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            int px = x + col;
            if (px < 0 || px >= surf_w) continue;
            dst[py * surf_w + px] = (bits & (uint8_t)(1U << (7-col))) ? fg : bg;
        }
    }
}

static void draw_text(uint32_t *dst, int x, int y, const char *s,
                      uint32_t fg, uint32_t bg, int surf_w, int surf_h)
{
    while (*s) {
        draw_glyph(dst, x, y, *s++, fg, bg, surf_w, surf_h);
        x += 8;
    }
}

static void draw_text_clip(uint32_t *dst, int x, int y, const char *s,
                           uint32_t fg, uint32_t bg,
                           int clip_x1, int surf_w, int surf_h)
{
    /* Draw text clipped to x < clip_x1 */
    while (*s) {
        if (x + 8 > clip_x1) break;
        draw_glyph(dst, x, y, *s++, fg, bg, surf_w, surf_h);
        x += 8;
    }
}

/* ── Dock drawing ────────────────────────────────────────────────── */
typedef struct {
    const char *label;
    uint32_t    color;
} dock_btn_t;

static const dock_btn_t dock_btns[DOCK_NBTN] = {
    { "Terminale", COL_BTN_TER },
    { "File",      COL_BTN_FIL },
    { "Opzioni",   COL_BTN_SET },
};

static void draw_dock(uint32_t *dst, int hover_btn)
{
    fill_rect(dst, 0, 0, DOCK_W, DOCK_H, COL_DOCK_BG, DOCK_W, DOCK_H);

    for (int i = 0; i < DOCK_NBTN; i++) {
        int bx = DOCK_BTN_X0 + i * (DOCK_BTN_W + DOCK_BTN_GAP);
        int by = DOCK_BTN_Y0;
        uint32_t bg = dock_btns[i].color;
        uint32_t fg = 0x00FFFFFFU;

        if (i == hover_btn) {
            /* Lighten by blending toward white */
            uint32_t r = ((bg >> 16) & 0xFF);
            uint32_t g = ((bg >>  8) & 0xFF);
            uint32_t b = (bg & 0xFF);
            r = (r + 255U) / 2U;
            g = (g + 255U) / 2U;
            b = (b + 255U) / 2U;
            bg = (r << 16) | (g << 8) | b;
        }

        fill_rect(dst, bx, by, DOCK_BTN_W, DOCK_BTN_H, bg, DOCK_W, DOCK_H);

        /* Label centered */
        int lx = bx + (DOCK_BTN_W - (int)(strlen(dock_btns[i].label) * 8U)) / 2;
        int ly = by + (DOCK_BTN_H - 16) / 2;
        draw_text(dst, lx, ly, dock_btns[i].label, fg, bg, DOCK_W, DOCK_H);
    }
}

/* ── File entry ──────────────────────────────────────────────────── */
typedef struct {
    char name[256];
    int  is_dir;
} ds_entry_t;

static int entry_cmp(const void *a, const void *b)
{
    const ds_entry_t *ea = (const ds_entry_t *)a;
    const ds_entry_t *eb = (const ds_entry_t *)b;
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    return strcmp(ea->name, eb->name);
}

/* ── Desktop state ───────────────────────────────────────────────── */
typedef struct {
    char       cwd[256];
    ds_entry_t entries[FIND_MAX_ENT];
    int        entry_count;
    int        scroll;
    int        selected;
    int        dirty_dock;
    int        dirty_find;
    int        dock_hover;
    /* screen pos of finder content (updated from configure) */
    int        find_scrn_x;  /* abs x of pixel (0,0) in find buf */
    int        find_scrn_y;
} ds_state_t;

static void ds_scan_dir(ds_state_t *st)
{
    DIR *d = opendir(st->cwd);
    st->entry_count = 0;
    st->scroll = 0;
    st->selected = -1;

    if (!d) return;

    struct dirent *de;
    while ((de = readdir(d)) != NULL && st->entry_count < FIND_MAX_ENT) {
        if (strcmp(de->d_name, ".") == 0) continue;
        strncpy(st->entries[st->entry_count].name, de->d_name, 255);
        st->entries[st->entry_count].name[255] = '\0';
        /* detect directory via d_type if available, else stat */
        st->entries[st->entry_count].is_dir = (de->d_type == DT_DIR) ? 1 : 0;
        st->entry_count++;
    }
    closedir(d);
    qsort(st->entries, (size_t)st->entry_count, sizeof(ds_entry_t), entry_cmp);
}

/* ── Finder drawing ──────────────────────────────────────────────── */
static void draw_finder(uint32_t *dst, const ds_state_t *st)
{
    int w = FIND_W, h = FIND_H;

    /* Background */
    fill_rect(dst, 0, 0, w, h, COL_FIND_BG, w, h);

    /* Toolbar */
    fill_rect(dst, 0, 0, w, FIND_TBAR_H, COL_FIND_TBAR, w, h);

    /* Back button stub */
    fill_rect(dst, 8, 8, 16, 16, 0x00AAAAAA, w, h);
    draw_text(dst, 10, 10, "<", 0x00FFFFFF, 0x00AAAAAA, w, h);

    /* Path in toolbar */
    char path_buf[64];
    snprintf(path_buf, sizeof(path_buf), "%s", st->cwd);
    draw_text_clip(dst, 34, 8, path_buf, COL_FIND_TBAR_TXT, COL_FIND_TBAR,
                   w - 8, w, h);

    /* Separator */
    fill_rect(dst, 0, FIND_TBAR_H, w, 1, COL_FIND_SEP, w, h);

    /* File list area */
    int list_y = FIND_LIST_Y;
    int list_h = h - FIND_TBAR_H - 1 - FIND_STAT_H;
    int max_rows = list_h / FIND_ROW_H;
    if (max_rows > FIND_MAX_ROWS) max_rows = FIND_MAX_ROWS;

    for (int r = 0; r < max_rows; r++) {
        int idx = st->scroll + r;
        int ry = list_y + r * FIND_ROW_H;
        uint32_t bg;
        uint32_t fg;
        int is_sel = (idx == st->selected);

        if (idx >= st->entry_count) {
            fill_rect(dst, 0, ry, w, FIND_ROW_H, COL_FIND_BG, w, h);
            continue;
        }

        bg = is_sel ? COL_ROW_SEL : ((r & 1) ? COL_ROW_ALT : COL_FIND_BG);
        fg = is_sel ? COL_ROW_SEL_TXT : COL_ROW_TXT;

        fill_rect(dst, 0, ry, w, FIND_ROW_H, bg, w, h);

        /* Icon */
        int icon_x = 8, icon_y = ry + (FIND_ROW_H - 16) / 2;
        uint32_t icon_col = st->entries[idx].is_dir ? COL_ICON_DIR : COL_ICON_FILE;
        fill_rect(dst, icon_x, icon_y, 14, 14, icon_col, w, h);
        if (st->entries[idx].is_dir) {
            /* Folder tab */
            fill_rect(dst, icon_x, icon_y - 3, 8, 3, icon_col, w, h);
        }

        /* Name */
        int txt_y = ry + (FIND_ROW_H - 16) / 2;
        draw_text_clip(dst, 28, txt_y, st->entries[idx].name, fg, bg,
                       w - 8, w, h);
    }

    /* Status bar */
    int stat_y = h - FIND_STAT_H;
    fill_rect(dst, 0, stat_y, w, FIND_STAT_H, COL_SBAR_BG, w, h);
    fill_rect(dst, 0, stat_y, w, 1, COL_FIND_SEP, w, h);

    char stat_buf[64];
    snprintf(stat_buf, sizeof(stat_buf), "%d elementi", st->entry_count);
    draw_text(dst, 8, stat_y + (FIND_STAT_H - 16) / 2, stat_buf,
              COL_STAT_TXT, COL_SBAR_BG, w, h);
}

/* ── Commit surface ──────────────────────────────────────────────── */
static void commit_surface(wl_conn_t *c, uint32_t surf_obj, uint32_t buf_obj,
                           int sw, int sh)
{
    msg_begin(c, surf_obj, WL_SURFACE_ATTACH, 12U);
    out_u32(c, buf_obj); out_u32(c, 0U); out_u32(c, 0U);
    msg_begin(c, surf_obj, WL_SURFACE_DAMAGE, 16U);
    out_u32(c, 0U); out_u32(c, 0U);
    out_u32(c, (uint32_t)sw); out_u32(c, (uint32_t)sh);
    msg_begin(c, surf_obj, WL_SURFACE_COMMIT, 0U);
    wl_flush(c);
}

/* ── Spawn app ───────────────────────────────────────────────────── */
static void ds_spawn(const char *path)
{
    pid_t pid = fork();
    if (pid == 0) {
        char *const argv[] = { (char *)path, NULL };
        char *const envp[] = {
            "WAYLAND_DISPLAY=/run/wayland-0",
            "HOME=/home/user",
            "PATH=/bin:/usr/bin",
            NULL
        };
        execve(path, argv, envp);
        _exit(1);
    }
    /* parent: reap later in main loop */
}

/* ── Hit test helpers ────────────────────────────────────────────── */
static int dock_btn_hit(int screen_x, int screen_y, int *btn_out)
{
    if (screen_y < DOCK_SCRN_Y || screen_y >= DOCK_SCRN_Y + DOCK_H)
        return 0;
    int local_y = screen_y - DOCK_SCRN_Y;
    if (local_y < DOCK_BTN_Y0 || local_y >= DOCK_BTN_Y0 + DOCK_BTN_H)
        return 0;
    int local_x = screen_x;
    for (int i = 0; i < DOCK_NBTN; i++) {
        int bx = DOCK_BTN_X0 + i * (DOCK_BTN_W + DOCK_BTN_GAP);
        if (local_x >= bx && local_x < bx + DOCK_BTN_W) {
            if (btn_out) *btn_out = i;
            return 1;
        }
    }
    return 0;
}

static int finder_row_hit(const ds_state_t *st,
                          int screen_x, int screen_y, int *row_out)
{
    /* finder content on screen: (find_scrn_x, find_scrn_y) for pixel (0,0) */
    int lx = screen_x - st->find_scrn_x;
    int ly = screen_y - st->find_scrn_y;

    if (lx < 0 || lx >= FIND_W) return 0;
    /* list starts at FIND_LIST_Y inside buffer */
    int list_end = FIND_H - FIND_STAT_H;
    if (ly < FIND_LIST_Y || ly >= list_end) return 0;

    int row = (ly - FIND_LIST_Y) / FIND_ROW_H;
    if (row < 0 || row >= FIND_MAX_ROWS) return 0;
    if (row_out) *row_out = row + st->scroll;
    return 1;
}

/* ── Connect + discover globals ──────────────────────────────────── */
static int wl_connect(wl_conn_t *c,
                      uint32_t *comp_name, uint32_t *shm_name,
                      uint32_t *xdg_name,  uint32_t *ptr_name)
{
    struct sockaddr_un sun;
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    strcpy(sun.sun_path, "/run/wayland-0");

    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) return -1;

    for (int t = 0; t < 30; t++) {
        if (connect(c->fd, (struct sockaddr *)&sun, sizeof(sun)) == 0) break;
        if (t == 29) return -1;
        sleep_10ms();
    }

    msg_begin(c, 1U, WL_DISPLAY_GET_REGISTRY, 4U);
    out_u32(c, 2U);
    wl_flush(c);

    *comp_name = *shm_name = *xdg_name = *ptr_name = 0U;
    for (int t = 0; t < 100; t++) {
        ssize_t n = recv(c->fd, c->ibuf + c->ilen,
                         sizeof(c->ibuf) - c->ilen, MSG_DONTWAIT);
        if (n > 0) c->ilen += (uint32_t)n;

        wl_event_t ev;
        while (wl_next_event(c, &ev)) {
            if (ev.obj_id == 2U && ev.opcode == WL_REGISTRY_GLOBAL && ev.plen >= 12U) {
                uint32_t name = in_u32(ev.payload);
                uint32_t slen = in_u32(ev.payload + 4U);
                char iface[64];
                uint32_t copy = (slen < sizeof(iface)) ? slen : (uint32_t)sizeof(iface)-1U;
                memcpy(iface, ev.payload + 8U, copy);
                iface[copy] = '\0';
                if (strcmp(iface, "wl_compositor")    == 0) *comp_name = name;
                else if (strcmp(iface, "wl_shm")      == 0) *shm_name  = name;
                else if (strcmp(iface, "xdg_wm_base") == 0) *xdg_name  = name;
                else if (strcmp(iface, "enlil_pointer_v1") == 0) *ptr_name = name;
            }
        }
        if (*comp_name && *shm_name && *xdg_name && *ptr_name) return 0;
        sleep_10ms();
    }
    return -1;
}

/* ════════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════════ */
int main(int argc, char **argv)
{
    wl_conn_t conn;
    wl_event_t ev;
    uint32_t comp_name, shm_name, xdg_name, ptr_name;
    int session_mode = 0;

    for (int i = 1; i < argc; i++)
        if (argv[i] && strcmp(argv[i], "--session") == 0)
            session_mode = 1;

    if (session_mode) {
        unlink(READY_PATH);
        unlink(FAIL_PATH);
    }

    if (wl_connect(&conn, &comp_name, &shm_name, &xdg_name, &ptr_name) < 0) {
        if (session_mode) write_marker(FAIL_PATH, "connect\n");
        write(2, "desktop: connect fail\n", 22);
        return 1;
    }

    /* ── Bind globals ─────────────────────────────────────────────── */
    /* IDs: 3=compositor 4=shm 5=xdg_wm 6=enlil_pointer */
    bind_global(&conn, comp_name,  "wl_compositor",      4U, 3U);
    bind_global(&conn, shm_name,   "wl_shm",             1U, 4U);
    bind_global(&conn, xdg_name,   "xdg_wm_base",        1U, 5U);
    if (ptr_name)
        bind_global(&conn, ptr_name, "enlil_pointer_v1", 1U, 6U);

    /* ── SHM pools ────────────────────────────────────────────────── */
    /* Dock: pool=7 buf=8 */
    int dock_shmid = shmget(IPC_PRIVATE, DOCK_SZ, IPC_CREAT | 0600);
    if (dock_shmid < 0) { if (session_mode) write_marker(FAIL_PATH, "shmget-dock\n"); return 2; }
    uint32_t *dock_pixels = (uint32_t *)shmat(dock_shmid, NULL, 0);
    if ((long)dock_pixels == -1L) { if (session_mode) write_marker(FAIL_PATH, "shmat-dock\n"); return 3; }

    /* Finder: pool=12 buf=13 */
    int find_shmid = shmget(IPC_PRIVATE, FIND_SZ, IPC_CREAT | 0600);
    if (find_shmid < 0) { if (session_mode) write_marker(FAIL_PATH, "shmget-find\n"); return 4; }
    uint32_t *find_pixels = (uint32_t *)shmat(find_shmid, NULL, 0);
    if ((long)find_pixels == -1L) { if (session_mode) write_marker(FAIL_PATH, "shmat-find\n"); return 5; }

    /* ── Register pools & buffers ─────────────────────────────────── */
    msg_begin(&conn, 4U, WL_SHM_CREATE_POOL_SYSV, 12U);
    out_u32(&conn, 7U); out_u32(&conn, (uint32_t)dock_shmid); out_u32(&conn, DOCK_SZ);
    wl_flush(&conn);
    msg_begin(&conn, 7U, WL_SHM_POOL_CREATE_BUFFER, 24U);
    out_u32(&conn, 8U);  /* buf id */
    out_u32(&conn, 0U);  /* offset */
    out_u32(&conn, DOCK_W); out_u32(&conn, DOCK_H);
    out_u32(&conn, DOCK_W * 4U); out_u32(&conn, 1U); /* XRGB8888 */
    wl_flush(&conn);

    msg_begin(&conn, 4U, WL_SHM_CREATE_POOL_SYSV, 12U);
    out_u32(&conn, 12U); out_u32(&conn, (uint32_t)find_shmid); out_u32(&conn, FIND_SZ);
    wl_flush(&conn);
    msg_begin(&conn, 12U, WL_SHM_POOL_CREATE_BUFFER, 24U);
    out_u32(&conn, 13U); /* buf id */
    out_u32(&conn, 0U);
    out_u32(&conn, FIND_W); out_u32(&conn, FIND_H);
    out_u32(&conn, FIND_W * 4U); out_u32(&conn, 1U);
    wl_flush(&conn);

    /* ── Dock surface ─────────────────────────────────────────────── */
    /* surface=9 xdg_surf=10 xdg_top=11 */
    msg_begin(&conn, 3U, WL_COMPOSITOR_CREATE_SURFACE, 4U); out_u32(&conn, 9U);  wl_flush(&conn);
    msg_begin(&conn, 5U, XDG_WM_GET_XDG_SURFACE, 8U); out_u32(&conn, 10U); out_u32(&conn, 9U);  wl_flush(&conn);
    msg_begin(&conn, 10U, XDG_SURFACE_GET_TOPLEVEL, 4U); out_u32(&conn, 11U); wl_flush(&conn);
    msg_begin(&conn, 11U, XDG_TOP_SET_TITLE,
              4U + (((uint32_t)strlen("Dock")+1U+3U)&~3U));
    out_str(&conn, "Dock"); wl_flush(&conn);
    msg_begin(&conn, 11U, XDG_TOP_SET_APP_ID,
              4U + (((uint32_t)strlen("enlil-dock")+1U+3U)&~3U));
    out_str(&conn, "enlil-dock"); wl_flush(&conn);
    msg_begin(&conn, 9U, WL_SURFACE_COMMIT, 0U); wl_flush(&conn);

    /* ── Finder surface ───────────────────────────────────────────── */
    /* surface=14 xdg_surf=15 xdg_top=16 */
    msg_begin(&conn, 3U, WL_COMPOSITOR_CREATE_SURFACE, 4U); out_u32(&conn, 14U); wl_flush(&conn);
    msg_begin(&conn, 5U, XDG_WM_GET_XDG_SURFACE, 8U); out_u32(&conn, 15U); out_u32(&conn, 14U); wl_flush(&conn);
    msg_begin(&conn, 15U, XDG_SURFACE_GET_TOPLEVEL, 4U); out_u32(&conn, 16U); wl_flush(&conn);
    msg_begin(&conn, 16U, XDG_TOP_SET_TITLE,
              4U + (((uint32_t)strlen("Finder")+1U+3U)&~3U));
    out_str(&conn, "Finder"); wl_flush(&conn);
    msg_begin(&conn, 16U, XDG_TOP_SET_APP_ID,
              4U + (((uint32_t)strlen("enlil-finder")+1U+3U)&~3U));
    out_str(&conn, "enlil-finder"); wl_flush(&conn);
    msg_begin(&conn, 14U, WL_SURFACE_COMMIT, 0U); wl_flush(&conn);

    /* ── Wait for configure on BOTH surfaces ──────────────────────── */
    int dock_cfg = 0, find_cfg = 0;
    uint32_t dock_serial = 0U, find_serial = 0U;

    for (int polls = 500; polls-- > 0 && !(dock_cfg && find_cfg); ) {
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0) conn.ilen += (uint32_t)n;

        while (wl_next_event(&conn, &ev)) {
            if (ev.obj_id == 10U && ev.opcode == XDG_SURFACE_CONFIGURE && ev.plen >= 4U) {
                dock_serial = in_u32(ev.payload); dock_cfg = 1;
            } else if (ev.obj_id == 15U && ev.opcode == XDG_SURFACE_CONFIGURE && ev.plen >= 4U) {
                find_serial = in_u32(ev.payload); find_cfg = 1;
            }
        }
        if (!(dock_cfg && find_cfg)) sleep_10ms();
    }

    if (!dock_cfg || !find_cfg) {
        if (session_mode) write_marker(FAIL_PATH, "configure\n");
        write(2, "desktop: configure timeout\n", 27);
        return 6;
    }

    /* ── ACK + first draw ─────────────────────────────────────────── */
    ds_state_t st;
    memset(&st, 0, sizeof(st));
    strcpy(st.cwd, "/");
    st.selected = -1;
    st.dock_hover = -1;
    st.find_scrn_x = FIND_SCRN_X;
    st.find_scrn_y = FIND_SCRN_Y;
    ds_scan_dir(&st);

    draw_dock(dock_pixels, st.dock_hover);
    flush_pixels(dock_pixels, DOCK_SZ);

    draw_finder(find_pixels, &st);
    flush_pixels(find_pixels, FIND_SZ);

    msg_begin(&conn, 10U, XDG_SURFACE_ACK_CONFIGURE, 4U); out_u32(&conn, dock_serial); wl_flush(&conn);
    commit_surface(&conn, 9U,  8U,  DOCK_W, DOCK_H);

    msg_begin(&conn, 15U, XDG_SURFACE_ACK_CONFIGURE, 4U); out_u32(&conn, find_serial); wl_flush(&conn);
    commit_surface(&conn, 14U, 13U, FIND_W, FIND_H);

    if (session_mode) write_marker(READY_PATH, "ready\n");

    /* ── Main loop ────────────────────────────────────────────────── */
    int done = 0;
    while (!done) {
        ssize_t n = recv(conn.fd, conn.ibuf + conn.ilen,
                         sizeof(conn.ibuf) - conn.ilen, MSG_DONTWAIT);
        if (n > 0) conn.ilen += (uint32_t)n;

        while (wl_next_event(&conn, &ev)) {
            /* Finder close (wld close button or Super+Q) */
            if (ev.obj_id == 16U && ev.opcode == XDG_TOPLEVEL_CLOSE) {
                done = 1; break;
            }
            /* Dock close — not expected but handle gracefully */
            if (ev.obj_id == 11U && ev.opcode == XDG_TOPLEVEL_CLOSE) {
                done = 1; break;
            }

            /* enlil_pointer_v1 event: x(4) y(4) buttons(4) */
            if (ev.obj_id == 6U && ev.opcode == ENLIL_POINTER_EVENT && ev.plen >= 12U) {
                int px = (int)(int32_t)in_u32(ev.payload);
                int py = (int)(int32_t)in_u32(ev.payload + 4U);
                uint32_t btns = in_u32(ev.payload + 8U);

                int btn;
                int row;
                int new_hover = -1;

                /* Dock hover */
                if (py >= DOCK_SCRN_Y) {
                    if (dock_btn_hit(px, py, &btn))
                        new_hover = btn;
                }
                if (new_hover != st.dock_hover) {
                    st.dock_hover = new_hover;
                    st.dirty_dock = 1;
                }

                /* Left click */
                if (btns & 1U) {
                    /* Dock click */
                    if (dock_btn_hit(px, py, &btn)) {
                        if (btn == 0)       /* Terminale */
                            ds_spawn("/WTERMDEMO.ELF");
                        else if (btn == 1)  /* File — navigate to root */
                            strcpy(st.cwd, "/");
                        /* btn==2 Opzioni: no-op v1 */
                        if (btn == 1) {
                            ds_scan_dir(&st);
                            st.dirty_find = 1;
                        }
                    }
                    /* Finder row click */
                    if (finder_row_hit(&st, px, py, &row)) {
                        if (row < st.entry_count) {
                            if (st.selected == row && st.entries[row].is_dir) {
                                /* Double-click: navigate in */
                                char newpath[256];
                                if (strcmp(st.cwd, "/") == 0)
                                    snprintf(newpath, sizeof(newpath), "/%s", st.entries[row].name);
                                else
                                    snprintf(newpath, sizeof(newpath), "%s/%s", st.cwd, st.entries[row].name);
                                strcpy(st.cwd, newpath);
                                ds_scan_dir(&st);
                            } else {
                                st.selected = row;
                            }
                            st.dirty_find = 1;
                        }
                    }
                }

                /* Reap zombie children */
                while (waitpid(-1, NULL, WNOHANG) > 0)
                    ;
            }
        }

        /* Redraw dirty surfaces */
        if (st.dirty_dock) {
            draw_dock(dock_pixels, st.dock_hover);
            flush_pixels(dock_pixels, DOCK_SZ);
            commit_surface(&conn, 9U, 8U, DOCK_W, DOCK_H);
            st.dirty_dock = 0;
        }
        if (st.dirty_find) {
            draw_finder(find_pixels, &st);
            flush_pixels(find_pixels, FIND_SZ);
            commit_surface(&conn, 14U, 13U, FIND_W, FIND_H);
            st.dirty_find = 0;
        }

        sleep_10ms();
    }

    /* ── Cleanup ──────────────────────────────────────────────────── */
    shmdt(dock_pixels);
    shmctl(dock_shmid, IPC_RMID, NULL);
    shmdt(find_pixels);
    shmctl(find_shmid, IPC_RMID, NULL);
    close(conn.fd);
    return 0;
}
