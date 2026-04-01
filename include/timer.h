/*
 * EnlilOS Microkernel - ARM Generic Timer (M2-02)
 *
 * ARM ARMv8-A dispone di quattro timer per-core indipendenti, tutti
 * derivati dallo stesso contatore di sistema a 64 bit (CNTPCT_EL0):
 *
 *   EL1 Physical   (CNTP)  — usato da EnlilOS per il tick di sistema
 *   EL1 Virtual    (CNTV)  — per processi user-space con offset virtuale
 *   EL2 Hypervisor (CNTHP) — riservato all'hypervisor
 *   EL3 Secure     (CNTPS) — riservato al monitor TrustZone
 *
 * EnlilOS usa il Physical EL1 Timer (CNTP):
 *   CNTFRQ_EL0  — frequenza del contatore di sistema (Hz, read-only)
 *   CNTPCT_EL0  — contatore corrente, 64 bit, incrementato a CNTFRQ Hz
 *   CNTP_TVAL_EL1 — valore ricaricabile: interrupt quando scende a 0
 *   CNTP_CTL_EL1  — control: ENABLE | IMASK | ISTATUS
 *
 * Design RT:
 *   timer_now_ns()   O(1) — 1 istruzione MRS + shift + moltiplicazione
 *   timer_now_us()   O(1) — come sopra
 *   timer_now_ticks() O(1) — 1 MRS
 *   Tick handler     WCET costante — ricarica TVAL, aggiorna jiffies
 *
 * Tick 1ms (1000 Hz):
 *   Su QEMU virt il timer fisico gira a 62.5 MHz (62500000 Hz).
 *   Tick period = CNTFRQ / 1000 = 62500 cicli per tick.
 *   Ogni tick il kernel aggiorna il contatore `jiffies` (uint64_t,
 *   monotono, in ms dal boot) e chiama `timer_tick_callback` se
 *   registrato (futuro scheduler M2-03).
 */

#ifndef ENLILOS_TIMER_H
#define ENLILOS_TIMER_H

#include "types.h"

/* ── Costanti ───────────────────────────────────────────────────── */

#define TIMER_HZ            1000U           /* tick per secondo       */
#define TIMER_TICK_MS       (1000U / TIMER_HZ) /* ms per tick = 1    */

/* CNTP_CTL_EL1 flags */
#define CNTP_CTL_ENABLE     (1U << 0)   /* abilita il timer           */
#define CNTP_CTL_IMASK      (1U << 1)   /* maschera l'interrupt       */
#define CNTP_CTL_ISTATUS    (1U << 2)   /* 1 = condizione di fire     */

/* ── Tipo callback ──────────────────────────────────────────────── */

/*
 * timer_tick_fn — chiamata a ogni tick (1ms) nel contesto IRQ.
 * NON può allocare memoria, NON può dormire, NON può acquisire lock.
 * WCET deve essere << 1ms (target: < 50µs).
 * 'ticks' = valore corrente di jiffies al momento del tick.
 */
typedef void (*timer_tick_fn)(uint64_t ticks);

/* ═══════════════════════════════════════════════════════════════════
 * API
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * timer_init() — inizializza il timer fisico EL1 e registra l'IRQ PPI
 * nel GIC. Configura il primo intervallo, NON avvia ancora il tick.
 * Chiamare dopo gic_init().
 */
void timer_init(void);

/*
 * timer_start() — ricarica CNTP_TVAL e abilita il timer.
 * Da questo momento il sistema riceve un IRQ ogni 1ms.
 * Chiamare dopo gic_enable_irqs() per evitare IRQ persi.
 */
void timer_start(void);

/*
 * timer_stop() — disabilita il timer (CNTP_CTL_EL1.ENABLE = 0).
 * Non cancella la registrazione GIC.
 */
void timer_stop(void);

/*
 * timer_set_tick_callback(fn) — registra il callback chiamato a ogni tick.
 * Sostituisce il precedente. NULL = nessun callback.
 * Thread-safe solo se chiamato con IRQ disabilitati.
 */
void timer_set_tick_callback(timer_tick_fn fn);

/*
 * timer_now_ticks() — legge CNTPCT_EL0 (contatore raw, monotono).
 * WCET: O(1), 1 istruzione MRS.
 * Risoluzione: 1/CNTFRQ secondi (~16ns a 62.5 MHz).
 */
static inline uint64_t timer_now_ticks(void)
{
    uint64_t val;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

/*
 * timer_now_ns() — tempo in nanosecondi dal boot.
 * WCET: O(1), nessuna divisione (usa moltiplicazione + shift).
 * Precisione: ~1ns, overflow a ~584 anni.
 *
 * Formula: ns = ticks * (1_000_000_000 / CNTFRQ)
 * Implementato come: ns = (ticks * ns_per_tick_frac) >> FRAC_SHIFT
 * con ns_per_tick_frac precalcolato al boot da timer_init().
 */
uint64_t timer_now_ns(void);

/*
 * timer_now_us() — tempo in microsecondi dal boot. O(1).
 */
static inline uint64_t timer_now_us(void)
{
    return timer_now_ns() / 1000ULL;
}

/*
 * timer_now_ms() — ms dal boot (= jiffies). O(1).
 * Nota: jiffies è aggiornato dall'IRQ handler, non dal contatore raw.
 * Risoluzione: 1ms. Usa timer_now_ns() per risoluzione maggiore.
 */
uint64_t timer_now_ms(void);

/*
 * timer_jiffies() — alias di timer_now_ms(), nome storico.
 */
static inline uint64_t timer_jiffies(void) { return timer_now_ms(); }

/*
 * timer_delay_us(us) — busy-wait per 'us' microsecondi.
 * Usa CNTPCT_EL0, non interrompibile.
 * WCET: O(us) — usare solo durante init, MAI in contesti RT.
 */
void timer_delay_us(uint32_t us);

/*
 * timer_cntfrq() — ritorna la frequenza del sistema timer in Hz.
 * Letta da CNTFRQ_EL0 al boot, cached in una variabile statica.
 */
uint64_t timer_cntfrq(void);

/*
 * timer_stats() — stampa statistiche del timer su UART.
 */
void timer_stats(void);

#endif /* ENLILOS_TIMER_H */
