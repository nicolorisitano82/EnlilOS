/*
 * NROS Microkernel - GIC-400 Interrupt Controller (M2-01)
 *
 * ARM Generic Interrupt Controller v2 (GIC-400).
 *
 * Il GIC è composto da due blocchi:
 *
 *  GICD (Distributor)    — gestisce la distribuzione degli interrupt a
 *                          tutti i core. Registri accessibili globalmente.
 *
 *  GICC (CPU Interface)  — per-core. Riceve gli interrupt dal GICD,
 *                          li presenta alla CPU con la priorità più alta,
 *                          e attende l'acknowledge (IAR) e il segnale di
 *                          fine gestione (EOIR).
 *
 * Tabella di dispatch:
 *   irq_table[GIC_MAX_IRQS] — {handler, data} statici.
 *   Indicizzata direttamente sull'IRQ ID: O(1) garantito.
 *   Nessuna lista, nessuna ricerca, nessuna allocazione nel hot path.
 *
 * RT compliance:
 *   gic_handle_irq() esegue: IAR read, bounds check, call, EOIR write.
 *   WCET = 3 MMIO accesses + 1 array lookup + 1 indirect call = costante.
 *   Tutto dentro il frame salvato da __exc_common in vectors.S.
 */

#include "gic.h"
#include "uart.h"

/* ── Accesso MMIO ─────────────────────────────────────────────────── */

static inline uint32_t gicd_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline void gicd_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(GICD_BASE + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline uint32_t gicc_read(uint32_t off)
{
    return *(volatile uint32_t *)(uintptr_t)(GICC_BASE + off);
}

static inline void gicc_write(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)(uintptr_t)(GICC_BASE + off) = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Accesso byte: priorità e target CPU (1 byte per IRQ nel registro a 32 bit) */
static inline uint8_t gicd_read8(uint32_t byte_off)
{
    return *(volatile uint8_t *)(uintptr_t)(GICD_BASE + byte_off);
}

static inline void gicd_write8(uint32_t byte_off, uint8_t val)
{
    *(volatile uint8_t *)(uintptr_t)(GICD_BASE + byte_off) = val;
}

/* ── Tabella di dispatch IRQ ──────────────────────────────────────── */

typedef struct {
    irq_handler_fn  handler;    /* callback registrato, mai NULL        */
    void           *data;       /* argomento opaco per il callback      */
} irq_desc_t;

/* Tutti in BSS → zero-inizializzati al boot */
static irq_desc_t irq_table[GIC_MAX_IRQS];
static uint32_t   irq_count[GIC_MAX_IRQS];     /* contatore per irq    */
static uint32_t   irq_spurious_count;

/* Numero di linee IRQ rilevato dal GICD_TYPER */
static uint32_t gic_num_irqs;

/* ── Helper UART ─────────────────────────────────────────────────── */

static void pr_dec(uint32_t v)
{
    if (v == 0) { uart_putc('0'); return; }
    char buf[12]; int len = 0;
    while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = len - 1; i >= 0; i--) uart_putc(buf[i]);
}

static void pr_hex8(uint8_t v)
{
    static const char h[] = "0123456789ABCDEF";
    uart_puts("0x");
    uart_putc(h[v >> 4]);
    uart_putc(h[v & 0xF]);
}

/* ── Handler di default ───────────────────────────────────────────── */

/*
 * Viene installato per ogni IRQ non ancora registrato.
 * Stampa un avviso ma non halts: l'interrupt viene comunque ACK-ato
 * e l'EOIR è scritto da gic_handle_irq() dopo il ritorno.
 */
static void gic_default_handler(uint32_t irq, void *data)
{
    (void)data;
    uart_puts("[GIC] WARN: IRQ #");
    pr_dec(irq);
    uart_puts(" senza handler registrato\n");
}

/* ══════════════════════════════════════════════════════════════════
 * gic_init — inizializzazione GICD + GICC
 * ══════════════════════════════════════════════════════════════════ */
void gic_init(void)
{
    uart_puts("[GIC] Inizializzazione GIC-400...\n");

    /* ── 1. Disabilita il Distributor durante la configurazione ──── */
    gicd_write(GICD_CTLR, 0);

    /* ── 2. Determina il numero di IRQ supportati ─────────────────
     *
     * TYPER[4:0] = ITLinesNumber N → numero linee = (N+1)*32
     * Le prime 32 sono sempre SGI+PPI.
     */
    uint32_t typer    = gicd_read(GICD_TYPER);
    uint32_t it_lines = (typer & 0x1F) + 1;
    gic_num_irqs      = it_lines * 32;
    if (gic_num_irqs > GIC_MAX_IRQS)
        gic_num_irqs = GIC_MAX_IRQS;

    uart_puts("[GIC]   Interrupt lines: ");
    pr_dec(gic_num_irqs);
    uart_puts(" (TYPER.ITLines=");
    pr_dec(it_lines);
    uart_puts(")\n");

    /* ── 3. Disabilita, cancella pending/active per tutti i gruppi ─ */
    for (uint32_t i = 0; i < it_lines; i++) {
        gicd_write(GICD_ICENABLER(i), 0xFFFFFFFF); /* Clear-Enable   */
        gicd_write(GICD_ICPENDR(i),   0xFFFFFFFF); /* Clear-Pending  */
        gicd_write(GICD_ICACTIVER(i), 0xFFFFFFFF); /* Clear-Active   */
    }

    /* ── 4. Priorità default = MIN (0xFF) per tutti gli IRQ ─────── */
    for (uint32_t irq = 0; irq < gic_num_irqs; irq++) {
        gicd_write8(GICD_IPRIORITYR(0) + irq, GIC_PRIO_MIN);
    }

    /* ── 5. Target CPU0 per tutte le SPI (≥ 32) ─────────────────── */
    for (uint32_t irq = GIC_SPI_BASE; irq < gic_num_irqs; irq++) {
        gicd_write8(GICD_ITARGETSR(0) + irq, 0x01); /* bit0 = CPU0  */
    }

    /* ── 6. Level-triggered per tutte le linee (ICFGR = 0) ──────── */
    for (uint32_t i = 0; i < it_lines * 2; i++)
        gicd_write(GICD_ICFGR(i), 0x00000000);

    /* ── 7. Abilita il Distributor ───────────────────────────────── */
    gicd_write(GICD_CTLR, 1);

    /* ── 8. Configura e abilita la CPU Interface ─────────────────── *
     *
     * PMR (Priority Mask Register): il GIC presenta alla CPU solo gli
     * interrupt con priorità < PMR. GIC_PMR_THRESHOLD = 0xF0:
     * accettiamo tutto tranne la priorità minima 0xFF.
     *
     * BPR = 0: tutti i bit di priorità sono usati come group priority
     * (nessuna suddivisione in subpriority).
     *
     * CTLR = 1: abilita la CPU Interface.
     */
    gicc_write(GICC_PMR,  GIC_PMR_THRESHOLD);
    gicc_write(GICC_BPR,  0x00);
    gicc_write(GICC_CTLR, 1);

    /* ── 9. Popola tabella di dispatch con handler di default ─────── */
    for (uint32_t i = 0; i < GIC_MAX_IRQS; i++) {
        irq_table[i].handler = gic_default_handler;
        irq_table[i].data    = NULL;
    }

    uart_puts("[GIC] GICD @ 0x08000000, GICC @ 0x08010000 — OK\n");
    uart_puts("[GIC] IRQ globali: DISABILITATI (chiamare gic_enable_irqs())\n");
}

/* ══════════════════════════════════════════════════════════════════
 * gic_register_irq — registra un handler per l'IRQ 'irq'
 * ══════════════════════════════════════════════════════════════════ */
void gic_register_irq(uint32_t irq, irq_handler_fn handler,
                      void *data, uint8_t priority, uint32_t flags)
{
    if (irq >= GIC_MAX_IRQS) {
        uart_puts("[GIC] WARN: gic_register_irq — IRQ #");
        pr_dec(irq);
        uart_puts(" fuori range (max ");
        pr_dec(GIC_MAX_IRQS - 1);
        uart_puts(")\n");
        return;
    }

    /* Registra nella tabella di dispatch */
    irq_table[irq].handler = handler ? handler : gic_default_handler;
    irq_table[irq].data    = data;

    /* Priorità nel GICD (1 byte per IRQ) */
    gicd_write8(GICD_IPRIORITYR(0) + irq, priority);

    /* Target CPU0 per SPI */
    if (irq >= GIC_SPI_BASE)
        gicd_write8(GICD_ITARGETSR(0) + irq, 0x01);

    /* Edge / Level trigger (solo SPI e PPI: i SGI sono sempre edge) */
    if (irq >= GIC_PPI_BASE) {
        uint32_t reg_idx = irq / 16;        /* ogni reg copre 16 IRQ  */
        uint32_t bit_pos = (irq % 16) * 2; /* 2 bit per IRQ          */
        uint32_t val     = gicd_read(GICD_ICFGR(reg_idx));

        if (flags & GIC_FLAG_EDGE)
            val |=  (2U << bit_pos);  /* bit[1]=1 → edge-triggered  */
        else
            val &= ~(2U << bit_pos);  /* bit[1]=0 → level-triggered  */

        gicd_write(GICD_ICFGR(reg_idx), val);
    }

    uart_puts("[GIC] IRQ #");
    pr_dec(irq);
    uart_puts(" — prio=");
    pr_hex8(priority);
    uart_puts(flags & GIC_FLAG_EDGE ? " edge" : " level");
    uart_puts(" registrato\n");
}

/* ══════════════════════════════════════════════════════════════════
 * gic_enable_irq / gic_disable_irq
 * ══════════════════════════════════════════════════════════════════ */
void gic_enable_irq(uint32_t irq)
{
    if (irq >= GIC_MAX_IRQS) return;
    /* ISENABLER: write 1 al bit corrispondente → set enable */
    gicd_write(GICD_ISENABLER(irq / 32), 1U << (irq % 32));
}

void gic_disable_irq(uint32_t irq)
{
    if (irq >= GIC_MAX_IRQS) return;
    /* ICENABLER: write 1 al bit corrispondente → clear enable */
    gicd_write(GICD_ICENABLER(irq / 32), 1U << (irq % 32));
}

int gic_irq_is_pending(uint32_t irq)
{
    if (irq >= GIC_MAX_IRQS) return 0;
    uint32_t val = gicd_read(GICD_ISPENDR(irq / 32));
    return (val >> (irq % 32)) & 1;
}

/* ══════════════════════════════════════════════════════════════════
 * gic_handle_irq — percorso hot, O(1) WCET
 *
 * Chiamato da irq_handler() in exception.c ogni volta che il
 * processore riceve un'eccezione di tipo IRQ (SPSR.M → ELt IRQ).
 *
 * Protocollo GIC-400:
 *   IAR  read  → acknowledge + ottieni IRQ ID (congelato sulla CPU IF)
 *   EOIR write → end-of-interrupt, rilascia la priority level
 *
 * L'ordine IAR→handler→EOIR garantisce che durante l'esecuzione del
 * handler il GIC non presenti lo stesso IRQ due volte (la priority
 * level della CPU Interface è "bloccata" finché non scriviamo EOIR).
 * ══════════════════════════════════════════════════════════════════ */
void gic_handle_irq(void)
{
    /* Acknowledge: legge IAR. Bit[9:0] = IRQ ID, bit[12:10] = CPU src */
    uint32_t iar = gicc_read(GICC_IAR);
    uint32_t irq = iar & 0x3FFU;

    /* Spurious: il GIC non ha interrupt reali in coda */
    if (irq == GIC_SPURIOUS) {
        irq_spurious_count++;
        return;
    }

    /* IRQ fuori dalla nostra tabella: ACK+EOI e warning */
    if (irq >= GIC_MAX_IRQS) {
        uart_puts("[GIC] IRQ ID fuori range: ");
        pr_dec(irq);
        uart_puts("\n");
        gicc_write(GICC_EOIR, iar);
        return;
    }

    /* Aggiorna contatore diagnostico */
    irq_count[irq]++;

    /* Dispatch O(1): call indiretta nella tabella */
    irq_table[irq].handler(irq, irq_table[irq].data);

    /* End of Interrupt: il valore scritto è lo stesso letto dall'IAR */
    gicc_write(GICC_EOIR, iar);
}

/* ══════════════════════════════════════════════════════════════════
 * gic_send_sgi — IPI via Software Generated Interrupt
 *
 * SGIR layout:
 *   [25:24] TargetListFilter: 00=lista, 01=altri, 10=self, 11=rsvd
 *   [23:16] CPUTargetList (solo se Filter=00): bitmask dei core target
 *   [3:0]   SGIINTID: 0–15
 * ══════════════════════════════════════════════════════════════════ */
void gic_send_sgi(uint32_t target_cpu, uint32_t sgi_id)
{
    if (sgi_id > 15) return;
    /* Filter=00 (lista specificata), CPUTargetList=1<<target_cpu */
    uint32_t val = (0U << 24)
                 | ((1U << (target_cpu & 7)) << 16)
                 | (sgi_id & 0xF);
    gicd_write(GICD_SGIR, val);
}

/* ── Diagnostica ─────────────────────────────────────────────────── */

void gic_stats(void)
{
    uart_puts("[GIC] ── Contatori interrupt ─────────────────────────\n");
    uart_puts("[GIC]   Spurious: ");
    pr_dec(irq_spurious_count);
    uart_puts("\n");

    uint32_t shown = 0;
    for (uint32_t i = 0; i < GIC_MAX_IRQS; i++) {
        if (irq_count[i] == 0) continue;
        uart_puts("[GIC]   IRQ #");
        pr_dec(i);
        uart_puts(": ");
        pr_dec(irq_count[i]);
        uart_puts(" volte\n");
        shown++;
    }
    if (shown == 0)
        uart_puts("[GIC]   (nessun IRQ ricevuto)\n");
    uart_puts("[GIC] ────────────────────────────────────────────────\n");
}
