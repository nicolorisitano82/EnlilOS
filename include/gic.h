/*
 * EnlilOS Microkernel - GIC-400 Interrupt Controller (M2-01)
 *
 * ARM Generic Interrupt Controller v2 (GIC-400) su QEMU virt machine.
 *
 * Topologia:
 *   GICD (Distributor) @ 0x08000000  — configurazione globale interrupt
 *   GICC (CPU Interface) @ 0x08010000 — acknowledge/EOI per Core0
 *
 * Classi di interrupt:
 *   SGI  (0–15)   Software Generated Interrupt — IPI tra core
 *   PPI  (16–31)  Private Peripheral — timer, watchdog (per-core)
 *   SPI  (32–287) Shared Peripheral  — UART, keyboard, virtio, …
 *
 * Tabella di dispatch:
 *   irq_table[GIC_MAX_IRQS] — array statico, indicizzato per IRQ ID.
 *   Lookup O(1), nessuna allocazione nel hot path.
 *
 * RT compliance:
 *   gic_handle_irq(): 2 MMIO read/write + 1 array load = WCET costante.
 *   gic_register_irq(): O(1), chiamare al boot fuori da sezioni RT.
 *   DAIF.I abbassato nel handler → interrupt nesting hardware dal GIC.
 */

#ifndef ENLILOS_GIC_H
#define ENLILOS_GIC_H

#include "types.h"

/* ── Indirizzi fisici QEMU virt ──────────────────────────────────── */
#define GICD_BASE           0x08000000UL    /* Distributor              */
#define GICC_BASE           0x08010000UL    /* CPU Interface (core 0)   */

/* ── Offset registri GICD ────────────────────────────────────────── */
#define GICD_CTLR           0x000   /* Distributor Control              */
#define GICD_TYPER          0x004   /* Interrupt Controller Type        */
#define GICD_IIDR           0x008   /* Implementer Identification       */
#define GICD_ISENABLER(n)   (0x100 + (n)*4) /* Set-Enable               */
#define GICD_ICENABLER(n)   (0x180 + (n)*4) /* Clear-Enable             */
#define GICD_ISPENDR(n)     (0x200 + (n)*4) /* Set-Pending              */
#define GICD_ICPENDR(n)     (0x280 + (n)*4) /* Clear-Pending            */
#define GICD_ISACTIVER(n)   (0x300 + (n)*4) /* Set-Active               */
#define GICD_ICACTIVER(n)   (0x380 + (n)*4) /* Clear-Active             */
#define GICD_IPRIORITYR(n)  (0x400 + (n)*4) /* Priority (1 byte/IRQ)   */
#define GICD_ITARGETSR(n)   (0x800 + (n)*4) /* CPU Target (1 byte/IRQ) */
#define GICD_ICFGR(n)       (0xC00 + (n)*4) /* Config edge/level        */
#define GICD_SGIR           0xF00   /* Software Generated Interrupt     */

/* ── Offset registri GICC ────────────────────────────────────────── */
#define GICC_CTLR           0x000   /* CPU Interface Control            */
#define GICC_PMR            0x004   /* Priority Mask                    */
#define GICC_BPR            0x008   /* Binary Point                     */
#define GICC_IAR            0x00C   /* Interrupt Acknowledge            */
#define GICC_EOIR           0x010   /* End of Interrupt                 */
#define GICC_RPR            0x014   /* Running Priority                 */
#define GICC_HPPIR          0x018   /* Highest Priority Pending         */
#define GICC_AIAR           0x020   /* Aliased IAR (Group 1)            */
#define GICC_AEOIR          0x024   /* Aliased EOIR (Group 1)           */

/* ── Costanti IRQ ────────────────────────────────────────────────── */
#define GIC_SGI_BASE        0       /* SGI: 0–15                        */
#define GIC_PPI_BASE        16      /* PPI: 16–31                       */
#define GIC_SPI_BASE        32      /* SPI: 32–287                      */
#define GIC_SPURIOUS        1023    /* ID spurious (nessun IRQ reale)   */
#define GIC_MAX_IRQS        256     /* Numero massimo gestiti da EnlilOS   */

/* ── IRQ noti su QEMU virt ───────────────────────────────────────── */
#define IRQ_TIMER_PHYS      30      /* ARM Generic Timer CNTP, PPI      */
#define IRQ_TIMER_VIRT      27      /* ARM Generic Timer CNTV, PPI      */
#define IRQ_TIMER_HYP       26      /* ARM Generic Timer CNTHP, PPI     */
#define IRQ_UART0           33      /* PL011 UART0 @ 0x09000000, SPI 1  */
#define IRQ_UART1           34      /* PL011 UART1, SPI 2               */
#define IRQ_KBD             44      /* PL050 PS/2 keyboard @ 0x09060000, SPI 12 */
#define IRQ_MOUSE           45      /* PL050 PS/2 mouse,   SPI 13               */
#define IRQ_VIRTIO(n)       (48+(n))/* VirtIO-mmio bus, SPI 16+n                */

/* ── Priorità GIC (0=massima, 255=minima) ────────────────────────── */
#define GIC_PRIO_MAX        0x00    /* Priorità massima — NMI-like      */
#define GIC_PRIO_REALTIME   0x40    /* Task hard-RT, timer              */
#define GIC_PRIO_DRIVER     0x80    /* Driver: UART, keyboard, block    */
#define GIC_PRIO_SOFTIRQ    0xB0    /* Deferred work                    */
#define GIC_PRIO_MIN        0xF0    /* Minima                           */
#define GIC_PMR_THRESHOLD   0xF0    /* CPU Interface: accetta tutto < 0xF0 */

/* ── Flag per gic_register_irq() ────────────────────────────────── */
#define GIC_FLAG_LEVEL      0       /* Level-triggered (default)        */
#define GIC_FLAG_EDGE       1       /* Edge-triggered                   */

/* ── Tipo handler ────────────────────────────────────────────────── */
typedef void (*irq_handler_fn)(uint32_t irq, void *data);

/* ═══════════════════════════════════════════════════════════════════
 * API
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * gic_init() — inizializza GICD e GICC.
 *
 * Operazioni:
 *   1. Disabilita Distributor durante configurazione
 *   2. Determina N interrupt da TYPER.ITLinesNumber
 *   3. Disabilita/de-penda/de-attiva tutti gli IRQ
 *   4. Priorità uniforme GIC_PRIO_MIN, target CPU0 per SPI
 *   5. Level-triggered come default
 *   6. Abilita Distributor (GICD_CTLR=1)
 *   7. Imposta soglia GICC_PMR, abilita CPU Interface (GICC_CTLR=1)
 *   8. Popola irq_table con handler di default
 *
 * NON abilita IRQ globali (DAIF.I): usare gic_enable_irqs() dopo.
 * Chiamare dopo exception_init() e mmu_init().
 */
void gic_init(void);

/*
 * gic_register_irq(irq, handler, data, priority, flags)
 *
 * Registra un handler nella tabella di dispatch per l'IRQ 'irq'.
 * Configura priorità e modalità trigger nel GICD.
 * NON abilita l'IRQ: chiamare gic_enable_irq() separatamente.
 *
 * WCET: O(1) — scrittura array + 3 MMIO.
 *
 * Parametri:
 *   irq      — ID interrupt (0 .. GIC_MAX_IRQS-1)
 *   handler  — callback void(*)(uint32_t irq, void *data), NULL=default
 *   data     — puntatore opaco passato al callback
 *   priority — GIC_PRIO_* (0=massima, 0xFF=minima)
 *   flags    — GIC_FLAG_LEVEL | GIC_FLAG_EDGE
 */
void gic_register_irq(uint32_t irq, irq_handler_fn handler,
                      void *data, uint8_t priority, uint32_t flags);

/*
 * gic_enable_irq(irq) / gic_disable_irq(irq)
 *
 * Abilita/disabilita un singolo IRQ nel GICD (ISENABLER/ICENABLER).
 * WCET: O(1) — 1 MMIO read-modify-write.
 */
void gic_enable_irq(uint32_t irq);
void gic_disable_irq(uint32_t irq);

/*
 * gic_irq_is_pending(irq) — ritorna 1 se l'IRQ è in coda nel GICD.
 * Utile per polling in sezioni con IRQ mascherati.
 */
int gic_irq_is_pending(uint32_t irq);

/*
 * gic_handle_irq() — percorso hot, chiamato da irq_handler() in exception.c.
 *
 * 1. GICC_IAR  → legge IRQ ID (acknowledge, blocca priority level)
 * 2. Filtra spurious (1023)
 * 3. irq_table[id].handler(id, data)   [O(1)]
 * 4. GICC_EOIR → segnala fine gestione (deassert priority)
 *
 * WCET: 2 MMIO + 1 load + call + 1 MMIO = costante.
 */
void gic_handle_irq(void);

/*
 * gic_send_sgi(target_cpu, sgi_id) — invia SGI a una CPU target.
 * sgi_id deve essere 0–15.
 * Utile per IPI in futuro SMP.
 */
void gic_send_sgi(uint32_t target_cpu, uint32_t sgi_id);

/*
 * gic_enable_irqs() / gic_disable_irqs()
 *
 * Abilita/disabilita IRQ globali su questo core (DAIF.I).
 * Inline per WCET minimo nelle sezioni critiche.
 *
 * RT: usare gic_disable_irqs() + gic_enable_irqs() per sezioni
 * critiche brevi (< deadline minimo tra due IRQ).
 */
static inline void gic_enable_irqs(void)
{
    __asm__ volatile("msr daifclr, #2" ::: "memory");
}

static inline void gic_disable_irqs(void)
{
    __asm__ volatile("msr daifset, #2" ::: "memory");
}

/*
 * gic_stats() — stampa contatori per IRQ ricevuti su UART.
 */
void gic_stats(void);

#endif /* ENLILOS_GIC_H */
