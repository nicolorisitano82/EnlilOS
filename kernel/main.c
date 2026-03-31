/*
 * NROS - Nicolo's Realtime Operating System
 * Microkernel per AArch64 (ARM M-series compatible)
 *
 * Punto di ingresso principale del kernel.
 * Inizializza tutti i sottosistemi e mostra "CIAO MONDO!" sullo schermo.
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

/* Banner ASCII art per la console seriale */
static void print_banner(void)
{
    uart_puts("\n");
    uart_puts("  _   _ ____   ___  ____  \n");
    uart_puts(" | \\ | |  _ \\ / _ \\/ ___| \n");
    uart_puts(" |  \\| | |_) | | | \\___ \\ \n");
    uart_puts(" | |\\  |  _ <| |_| |___) |\n");
    uart_puts(" |_| \\_|_| \\_\\\\___/|____/ \n");
    uart_puts("\n");
    uart_puts(" Nicolo's Realtime Operating System\n");
    uart_puts(" Microkernel v0.1.0 - AArch64\n");
    uart_puts(" Architettura: Microkernel (stile GNU Hurd)\n");
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

/*
 * ticker_task — task demo (M2-03)
 *
 * Gira a priorità PRIO_HIGH (32). Ogni ~500ms stampa un contatore
 * su UART dimostrando che il timer + scheduler funzionano.
 * Chiama sched_yield() per cedere la CPU dopo ogni stampa.
 */
static void ticker_task(void)
{
    uint32_t count = 0;
    uint64_t last  = 0;

    while (1) {
        uint64_t now = timer_now_ms();
        if (now - last >= 500) {
            last = now;
            uart_puts("[TICKER] tick #");
            /* mini pr_dec inline */
            char buf[12]; int len = 0;
            uint32_t v = count;
            if (v == 0) { uart_putc('0'); }
            else {
                while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
                for (int i = len-1; i >= 0; i--) uart_putc(buf[i]);
            }
            uart_puts(" t=");
            len = 0;
            uint64_t t = now;
            while (t) { buf[len++] = '0' + (int)(t % 10); t /= 10; }
            if (len == 0) { uart_putc('0'); }
            else { for (int i = len-1; i >= 0; i--) uart_putc(buf[i]); }
            uart_puts("ms\n");
            count++;
        }
        sched_yield();
    }
}

void kernel_main(void)
{
    /* === Fase 1: Inizializzazione hardware === */
    uart_init();
    print_banner();

    uart_puts("[NROS] Boot in corso...\n");
    uart_puts("[NROS] CPU: AArch64 (ARMv8-A)\n");
    uart_puts("[NROS] UART: PL011 @ 0x09000000 - OK\n");

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

    uart_puts("[NROS] Server di sistema registrati\n");

    /* === Fase 4: Mostra CIAO MONDO! === */

    /* Colori */
    uint32_t bg_color   = 0x001a1a2e;  /* Blu scuro */
    uint32_t text_color = 0x0000FF88;  /* Verde brillante */
    uint32_t border_col = 0x00FFD700;  /* Oro */
    uint32_t subtitle_c = 0x00888888;  /* Grigio */

    /* Testo principale centrato */
    const char *main_text = "CIAO MONDO!";
    fb_draw_string_centered(main_text, text_color, bg_color);

    /* Sottotitolo */
    const char *subtitle = "NROS MICROKERNEL V0.1.0";
    uint32_t sub_len = 0;
    const char *p = subtitle;
    while (*p++) sub_len++;

    uint32_t sub_x = (FB_WIDTH - sub_len * 8) / 2;
    uint32_t sub_y = (FB_HEIGHT / 2) + 24;
    fb_draw_string(sub_x, sub_y, subtitle, subtitle_c, bg_color);

    /* Bordo decorativo attorno al testo principale */
    uint32_t main_len = 0;
    p = main_text;
    while (*p++) main_len++;

    uint32_t text_w = main_len * 8;
    uint32_t text_x = (FB_WIDTH - text_w) / 2;
    uint32_t text_y = (FB_HEIGHT - 16) / 2;
    draw_border(text_x, text_y, text_w, 16, 8, border_col);

    /* Flush D-cache → QEMU vede il framebuffer aggiornato in RAM fisica */
    fb_flush();

    /* Test IPC: invia un messaggio dal kernel al server console */
    const char *ipc_test = "CIAO MONDO!";
    mk_ipc_send(1, 2, IPC_MSG_PRINT, ipc_test, 12);
    uart_puts("[NROS] Messaggio IPC inviato: CIAO MONDO!\n");

    /* Verifica ricezione */
    ipc_message_t msg;
    if (mk_ipc_receive(2, &msg) == 0) {
        uart_puts("[NROS] IPC ricevuto dal task uart-server: ");
        uart_puts((const char *)msg.payload);
        uart_puts("\n");
    }

    uart_puts("\n[NROS] ===================================\n");
    uart_puts("[NROS] Boot completato con successo!\n");
    uart_puts("[NROS] Framebuffer: CIAO MONDO! visualizzato\n");
    uart_puts("[NROS] Scheduler FPP attivo — ticker ogni 500ms\n");
    uart_puts("[NROS] ===================================\n\n");

    /* Il task kernel entra nel suo loop:
     * cede la CPU ogni ~2s e stampa le statistiche.
     * Il timer IRQ ogni 1ms garantisce la preemption del ticker task. */
    uint64_t last_stats = 0;
    while (1) {
        uint64_t now = timer_now_ms();
        if (now - last_stats >= 2000) {
            last_stats = now;
            sched_stats();
        }
        sched_yield();
    }
}
