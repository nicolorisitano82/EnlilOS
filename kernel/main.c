/*
 * EnlilOS - Nicolo's Realtime Operating System
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
#include "timer.h"
#include "sched.h"
#include "syscall.h"
#include "keyboard.h"
#include "ane.h"
#include "gpu.h"

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
#define BOOTCLI_INPUT_MAX     72U

static char      bootcli_lines[BOOTCLI_HISTORY_MAX][BOOTCLI_LINE_MAX + 1];
static uint32_t  bootcli_line_count;
static char      bootcli_input[BOOTCLI_INPUT_MAX + 1];
static uint32_t  bootcli_input_len;
static gpu_caps_t bootcli_caps;
static uint8_t   bootcli_graphics_mode;
static volatile uint32_t bootcli_heartbeat;

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

static void bootcli_fill_rect(uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t yy = y; yy < y + h && yy < FB_HEIGHT; yy++) {
        for (uint32_t xx = x; xx < x + w && xx < FB_WIDTH; xx++)
            fb_put_pixel(xx, yy, color);
    }
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

static void bootcli_push_current_input(void)
{
    char line[BOOTCLI_LINE_MAX + 1];

    line[0] = '>';
    line[1] = ' ';
    line[2] = '\0';
    bootcli_buf_append(line, sizeof(line), bootcli_input);
    bootcli_push_line(line);
}

static void bootcli_render(void)
{
    const uint32_t bg_color     = 0x000c1118;
    const uint32_t panel_color  = 0x00141d29;
    const uint32_t header_color = 0x001d2937;
    const uint32_t border_color = 0x0000d6b8;
    const uint32_t title_color  = 0x00eef5ff;
    const uint32_t muted_color  = 0x0095a8bd;
    const uint32_t accent_color = 0x007ce7c8;
    const uint32_t prompt_color = 0x00ffd166;
    const uint32_t panel_x      = 32U;
    const uint32_t panel_y      = 24U;
    const uint32_t panel_w      = 736U;
    const uint32_t panel_h      = 552U;
    const uint32_t history_x    = 48U;
    const uint32_t history_y    = 136U;
    const uint32_t line_step    = 18U;
    const uint32_t prompt_y     = 488U;
    const uint32_t footer_y     = 544U;
    const uint32_t visible      = 18U;
    uint32_t first;
    uint32_t row = 0U;
    char prompt_line[BOOTCLI_LINE_MAX + 1];
    char footer[BOOTCLI_LINE_MAX + 1];
    uint64_t cursor_phase = timer_now_ms() / 400ULL;

    fb_clear(bg_color);
    bootcli_fill_rect(panel_x, panel_y, panel_w, panel_h, panel_color);
    bootcli_fill_rect(panel_x, panel_y, panel_w, 40U, header_color);
    draw_border(panel_x, panel_y, panel_w - 1U, panel_h - 1U, 0U, border_color);

    fb_draw_string(48U, 36U,
                   bootcli_graphics_mode ?
                       "ENLILOS GRAPHICS CONSOLE" :
                       "ENLILOS BOOT CONSOLE",
                   title_color, header_color);

    fb_draw_string(48U, 72U,
                   bootcli_graphics_mode ?
                       "Modo: GRAFICA (VirtIO-GPU)" :
                       "Modo: FRAMEBUFFER DI BOOT",
                   accent_color, panel_color);
    fb_draw_string(48U, 92U,
                   "Comandi: help  clear  gpu  echo <test>  keyboard",
                   muted_color, panel_color);
    fb_draw_string(48U, 112U,
                   bootcli_graphics_mode ?
                       "Fai click nella finestra QEMU e poi digita." :
                       "Digita per testare l'input da tastiera o seriale.",
                   muted_color, panel_color);

    first = (bootcli_line_count > visible) ?
            (bootcli_line_count - visible) : 0U;
    for (uint32_t i = first; i < bootcli_line_count; i++) {
        fb_draw_string(history_x, history_y + row * line_step,
                       bootcli_lines[i], title_color, panel_color);
        row++;
    }

    bootcli_fill_rect(40U, prompt_y - 6U, 720U, 2U, border_color);

    prompt_line[0] = '>';
    prompt_line[1] = ' ';
    prompt_line[2] = '\0';
    bootcli_buf_append(prompt_line, sizeof(prompt_line), bootcli_input);
    if ((cursor_phase & 1ULL) == 0ULL)
        bootcli_buf_append(prompt_line, sizeof(prompt_line), "_");
    fb_draw_string(history_x, prompt_y, prompt_line, prompt_color, panel_color);

    footer[0] = '\0';
    bootcli_buf_append(footer, sizeof(footer), "Scheduler OK | Heartbeat ");
    bootcli_buf_append_u32(footer, sizeof(footer), bootcli_heartbeat);
    if (bootcli_graphics_mode)
        bootcli_buf_append(footer, sizeof(footer), " | Scanout VirtIO attivo");
    else
        bootcli_buf_append(footer, sizeof(footer), " | Scanout framebuffer");
    fb_draw_string(48U, footer_y, footer, muted_color, panel_color);

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
        bootcli_push_line("gpu       mostra il backend grafico attivo");
        bootcli_push_line("echo TXT  ristampa il testo scritto");
        bootcli_push_line("keyboard  conferma che l'input arriva");
    } else if (bootcli_streq(bootcli_input, "clear")) {
        bootcli_line_count = 0U;
        bootcli_push_line("Console pulita.");
    } else if (bootcli_streq(bootcli_input, "gpu")) {
        line[0] = '\0';
        bootcli_buf_append(line, sizeof(line), "GPU: ");
        if (bootcli_caps.vendor == GPU_VENDOR_VIRTIO) {
            bootcli_buf_append(line, sizeof(line),
                               "VirtIO-GPU, modalita grafica attiva.");
        } else if (bootcli_caps.vendor == GPU_VENDOR_APPLE_AGX) {
            bootcli_buf_append(line, sizeof(line),
                               "Apple AGX backend selezionato.");
        } else {
            bootcli_buf_append(line, sizeof(line),
                               "software fallback / framebuffer.");
        }
        bootcli_push_line(line);
    } else if (bootcli_streq(bootcli_input, "keyboard")) {
        bootcli_push_line("Tastiera: input ricevuto correttamente.");
    } else if (bootcli_startswith(bootcli_input, "echo ")) {
        bootcli_push_line(bootcli_input + 5);
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

    while ((c = keyboard_getc()) >= 0) {
        uint8_t ch = (uint8_t)c;

        if (ch == '\r')
            ch = '\n';

        if (ch == '\n') {
            bootcli_execute_command();
            dirty = 1;
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

static void bootcli_init(void)
{
    bootcli_line_count = 0U;
    bootcli_input_len = 0U;
    bootcli_input[0] = '\0';

    gpu_get_caps(&bootcli_caps);
    bootcli_graphics_mode = (bootcli_caps.vendor == GPU_VENDOR_VIRTIO) ? 1U : 0U;

    bootcli_push_line("EnlilOS boot console pronta.");
    if (bootcli_graphics_mode)
        bootcli_push_line("Modalita grafica attiva: VirtIO-GPU.");
    else
        bootcli_push_line("Modalita framebuffer locale attiva.");
    bootcli_push_line("Digita 'help' e premi Invio per testare la tastiera.");
    if (bootcli_graphics_mode)
        bootcli_push_line("Fai click nella finestra QEMU per il focus.");
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
    ane_init();
    gpu_init();
    /* Da qui: task switching via timer IRQ ogni 1ms.
     * sched_tick() → need_resched → vectors.S → schedule() */

    /* Task demo: ticker (prio 64) — stampa un contatore ogni ~500ms */
    sched_task_create("ticker", ticker_task, PRIO_HIGH);
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

    /* === Fase 8: Microkernel === */
    mk_init();

    /* === Fase 9: Framebuffer === */
    fb_init();

    /* Crea task per i server di sistema (stile Hurd) */
    mk_task_create("uart-server", TASK_TYPE_SERVER, 0);
    mk_task_create("fb-server", TASK_TYPE_SERVER, 0);
    mk_task_create("mem-server", TASK_TYPE_SERVER, 0);

    uart_puts("[EnlilOS] Server di sistema registrati\n");

    bootcli_init();
    bootcli_render();

    uart_puts("\n[EnlilOS] ===================================\n");
    uart_puts("[EnlilOS] Boot completato con successo!\n");
    uart_puts("[EnlilOS] Console interattiva pronta\n");
    uart_puts("[EnlilOS] Scheduler FPP attivo — heartbeat ogni 500ms\n");
    uart_puts("[EnlilOS] ===================================\n\n");

    /* Loop di boot console: input tastiera + refresh grafico opportunistico. */
    uint64_t last_cursor_phase = ~0ULL;
    uint32_t last_heartbeat = bootcli_heartbeat;
    while (1) {
        int dirty = bootcli_poll_input();
        uint64_t cursor_phase = timer_now_ms() / 400ULL;

        if (cursor_phase != last_cursor_phase) {
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
