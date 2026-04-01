/*
 * EnlilOS Microkernel - ARM Generic Timer (M2-02)
 *
 * Usa il Physical EL1 Timer (CNTP):
 *
 *   CNTFRQ_EL0   — frequenza di riferimento (Hz), stabilita dal firmware
 *   CNTPCT_EL0   — contatore di sistema, 64 bit, free-running
 *   CNTP_TVAL_EL1 — down-counter: IRQ quando scende a 0, poi wrap
 *   CNTP_CTL_EL1  — [0]=ENABLE [1]=IMASK [2]=ISTATUS(ro)
 *
 * Meccanismo di reload:
 *   All'IRQ, l'handler ricarica CNTP_TVAL con tick_period (= CNTFRQ/HZ).
 *   Questo garantisce che il prossimo IRQ arrivi esattamente tick_period
 *   cicli dopo il fire corrente — nessun drift accumulato nel tempo.
 *   (A differenza di CNTP_CVAL che fissa il confronto in modo assoluto:
 *   TVAL ricaricato dopo la lettura dell'IRQ introduce al massimo la
 *   latenza dell'handler come jitter — accettabile per 1ms tick RT.)
 *
 * RT: l'IRQ handler è < 20 istruzioni (reload + inc jiffies + call cb).
 *     WCET stimato: ~50 cicli = ~800ns @ 62.5 MHz — ampiamente < 1ms.
 */

#include "timer.h"
#include "gic.h"
#include "uart.h"

/* ── Stato globale (BSS, init a 0) ──────────────────────────────── */

static uint64_t cntfrq;             /* Hz letti da CNTFRQ_EL0          */
static uint64_t tick_period;        /* cicli per tick = cntfrq / HZ    */
static uint64_t jiffies;            /* ms dal boot, aggiornato dall'IRQ */
static uint64_t tick_count;         /* contatore totale di IRQ timer    */

/* Precalcolato per timer_now_ns(): evita divisione nel hot path.
 *
 * ns_per_tick_frac = (1_000_000_000 << NS_FRAC_SHIFT) / cntfrq
 * timer_now_ns()   = (cntpct * ns_per_tick_frac) >> NS_FRAC_SHIFT
 *
 * Con NS_FRAC_SHIFT=32 e cntfrq=62500000:
 *   ns_per_tick_frac = (10^9 * 2^32) / 62500000 = 68719476736 / 62.5M ≈ 68.7
 *   Overflow: cntpct max usabile = 2^63 / ns_per_tick_frac ≈ 134e12 ticks
 *   = ~2.1 milioni di secondi (~25 giorni) — sufficiente per un sistema RT.
 */
#define NS_FRAC_SHIFT   32
static uint64_t ns_per_tick_frac;   /* (10^9 << NS_FRAC_SHIFT) / cntfrq */

/* Callback opzionale per lo scheduler (M2-03) */
static timer_tick_fn tick_callback;

/* ── Helpers UART (senza printf) ─────────────────────────────────── */

static void pr_dec(uint64_t v)
{
    if (v == 0) { uart_putc('0'); return; }
    char buf[20]; int len = 0;
    while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = len - 1; i >= 0; i--) uart_putc(buf[i]);
}

/* ── Accesso ai registri di sistema ─────────────────────────────── */

static inline uint64_t read_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_tval(uint64_t v)
{
    __asm__ volatile("msr cntp_tval_el0, %0" :: "r"(v));
    __asm__ volatile("isb");
}

static inline uint64_t read_cntp_ctl(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntp_ctl_el0" : "=r"(v));
    return v;
}

static inline void write_cntp_ctl(uint64_t v)
{
    __asm__ volatile("msr cntp_ctl_el0, %0" :: "r"(v));
    __asm__ volatile("isb");
}

/* ── IRQ handler del timer ────────────────────────────────────────
 *
 * Chiamato da gic_handle_irq() ogni 1ms (IRQ_TIMER_PHYS = PPI #30).
 *
 * Operazioni (WCET costante, nessuna allocazione, nessuna lock):
 *   1. Ricarica CNTP_TVAL per il prossimo tick
 *   2. Aggiorna jiffies e tick_count
 *   3. Chiama il callback dello scheduler (se registrato)
 *
 * Il CNTP_CTL.ISTATUS si abbassa automaticamente quando riscriviamo
 * CNTP_TVAL — il GIC NON genererà un nuovo IRQ prima del prossimo fire.
 */
static void timer_irq_handler(uint32_t irq, void *data)
{
    (void)irq;
    (void)data;

    /* 1. Ricarica il down-counter: prossimo fire fra tick_period cicli */
    write_cntp_tval(tick_period);

    /* 2. Aggiorna contatori monotoni */
    jiffies++;
    tick_count++;

    /* 3. Callback scheduler (M2-03 lo userà per preemption + accounting) */
    if (tick_callback)
        tick_callback(jiffies);
}

/* ══════════════════════════════════════════════════════════════════
 * timer_init
 * ══════════════════════════════════════════════════════════════════ */
void timer_init(void)
{
    uart_puts("[TIMER] Inizializzazione ARM Generic Timer...\n");

    /* ── 1. Leggi la frequenza dal registro di sistema ────────────
     *
     * CNTFRQ_EL0 è scritto dal firmware (QEMU lo imposta a 62500000 Hz).
     * Non è modificabile in EL1.
     */
    cntfrq = read_cntfrq();
    if (cntfrq == 0) {
        uart_puts("[TIMER] PANIC: CNTFRQ_EL0 = 0 — firmware non configurato\n");
        while (1) __asm__ volatile("wfe");
    }

    uart_puts("[TIMER]   CNTFRQ = ");
    pr_dec(cntfrq);
    uart_puts(" Hz (");
    pr_dec(cntfrq / 1000000ULL);
    uart_puts(".");
    pr_dec((cntfrq % 1000000ULL) / 100000ULL);
    uart_puts(" MHz)\n");

    /* ── 2. Calcola il periodo in cicli per TIMER_HZ tick/sec ─────
     *
     * tick_period = cntfrq / TIMER_HZ
     * Arrotondato per eccesso per non perdere cicli.
     */
    tick_period = (cntfrq + TIMER_HZ / 2) / TIMER_HZ;

    uart_puts("[TIMER]   Tick period = ");
    pr_dec(tick_period);
    uart_puts(" cicli (");
    pr_dec(TIMER_HZ);
    uart_puts(" Hz = ");
    pr_dec(TIMER_TICK_MS);
    uart_puts(" ms/tick)\n");

    /* ── 3. Precalcola il fattore di conversione ticks → ns ───────
     *
     * ns_per_tick_frac = (10^9 << NS_FRAC_SHIFT) / cntfrq
     *
     * Usiamo un'operazione a 64 bit: se cntfrq < 2^31, allora
     * (10^9 << 32) = 4.29e18 che sta in uint64_t (max ~1.84e19).
     */
    ns_per_tick_frac = (1000000000ULL << NS_FRAC_SHIFT) / cntfrq;

    uart_puts("[TIMER]   ns/tick_frac = ");
    pr_dec(ns_per_tick_frac);
    uart_puts(" (shift=");
    pr_dec(NS_FRAC_SHIFT);
    uart_puts(")\n");

    /* ── 4. Registra l'IRQ PPI nel GIC ───────────────────────────
     *
     * IRQ_TIMER_PHYS = PPI #30 (per-core, non instradabile tramite SPI).
     * Priorità: GIC_PRIO_REALTIME = 0x40 — interrompe task a bassa priorità,
     * ma non interrompe altri IRQ RT a priorità più alta.
     * Edge-triggered: il timer PPI ARM è edge dal GIC-400.
     */
    gic_register_irq(IRQ_TIMER_PHYS, timer_irq_handler,
                     NULL, GIC_PRIO_REALTIME, GIC_FLAG_EDGE);
    gic_enable_irq(IRQ_TIMER_PHYS);

    /* ── 5. Assicura che il timer sia spento fino a timer_start() ─ */
    write_cntp_ctl(0);   /* ENABLE=0, IMASK=0 */

    uart_puts("[TIMER] Pronto — in attesa di timer_start()\n");
}

/* ══════════════════════════════════════════════════════════════════
 * timer_start / timer_stop
 * ══════════════════════════════════════════════════════════════════ */
void timer_start(void)
{
    /* Carica il down-counter con il primo periodo */
    write_cntp_tval(tick_period);

    /* Abilita il timer con interrupt non mascherato */
    write_cntp_ctl(CNTP_CTL_ENABLE);   /* ENABLE=1, IMASK=0 */

    uart_puts("[TIMER] Avviato — tick ogni ");
    pr_dec(TIMER_TICK_MS);
    uart_puts(" ms\n");
}

void timer_stop(void)
{
    write_cntp_ctl(0);
    uart_puts("[TIMER] Fermato\n");
}

/* ══════════════════════════════════════════════════════════════════
 * timer_set_tick_callback
 * ══════════════════════════════════════════════════════════════════ */
void timer_set_tick_callback(timer_tick_fn fn)
{
    tick_callback = fn;
}

/* ══════════════════════════════════════════════════════════════════
 * timer_now_ns — conversione ticks → nanosecondi, O(1)
 *
 * (cntpct * ns_per_tick_frac) >> NS_FRAC_SHIFT
 *
 * Attenzione: il prodotto cntpct * ns_per_tick_frac può superare 64 bit
 * per uptime > ~25 giorni. Per EnlilOS (sistema embedded RT) è accettabile.
 * ══════════════════════════════════════════════════════════════════ */
uint64_t timer_now_ns(void)
{
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return (ticks * ns_per_tick_frac) >> NS_FRAC_SHIFT;
}

/* ══════════════════════════════════════════════════════════════════
 * timer_now_ms — legge jiffies aggiornato dall'IRQ handler
 * ══════════════════════════════════════════════════════════════════ */
uint64_t timer_now_ms(void)
{
    /* Legge jiffies con barriera di memoria: il compilatore non può
     * riordinare questa lettura rispetto ad accessi precedenti. */
    uint64_t v;
    __asm__ volatile("" ::: "memory");
    v = jiffies;
    __asm__ volatile("" ::: "memory");
    return v;
}

/* ══════════════════════════════════════════════════════════════════
 * timer_cntfrq
 * ══════════════════════════════════════════════════════════════════ */
uint64_t timer_cntfrq(void)
{
    return cntfrq;
}

/* ══════════════════════════════════════════════════════════════════
 * timer_delay_us — busy-wait, solo per uso durante init
 * ══════════════════════════════════════════════════════════════════ */
void timer_delay_us(uint32_t us)
{
    uint64_t start, end, cycles;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
    /* cicli = us * cntfrq / 1_000_000 */
    cycles = ((uint64_t)us * cntfrq) / 1000000ULL;
    end = start + cycles;
    uint64_t now;
    do {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
    } while (now < end);
}

/* ══════════════════════════════════════════════════════════════════
 * timer_stats
 * ══════════════════════════════════════════════════════════════════ */
void timer_stats(void)
{
    uart_puts("[TIMER] ── Statistiche timer ─────────────────────────\n");
    uart_puts("[TIMER]   CNTFRQ    = "); pr_dec(cntfrq);       uart_puts(" Hz\n");
    uart_puts("[TIMER]   Tick Hz   = "); pr_dec(TIMER_HZ);     uart_puts(" Hz\n");
    uart_puts("[TIMER]   Periodo   = "); pr_dec(tick_period);  uart_puts(" cicli\n");
    uart_puts("[TIMER]   Jiffies   = "); pr_dec(jiffies);      uart_puts(" ms\n");
    uart_puts("[TIMER]   IRQ totali= "); pr_dec(tick_count);   uart_puts("\n");

    /* Legge e mostra il CTL corrente */
    uint64_t ctl = read_cntp_ctl();
    uart_puts("[TIMER]   CNTP_CTL  = ");
    uart_puts((ctl & CNTP_CTL_ENABLE)  ? "ENABLE " : "disable ");
    uart_puts((ctl & CNTP_CTL_IMASK)   ? "IMASK "  : "- ");
    uart_puts((ctl & CNTP_CTL_ISTATUS) ? "FIRE\n"  : "\n");
    uart_puts("[TIMER] ─────────────────────────────────────────────\n");
}
