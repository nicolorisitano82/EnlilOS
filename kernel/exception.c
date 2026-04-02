/*
 * EnlilOS Microkernel - Exception Handler (M1-01)
 *
 * Decodifica ESR_EL1, stampa il contesto su UART e halt per eccezioni fatali.
 * Gli IRQ vengono separati per l'estensione al GIC-400 (M2-01).
 */

#include "exception.h"
#include "kdebug.h"
#include "mmu.h"
#include "sched.h"
#include "syscall.h"
#include "uart.h"

/* ── Handler principale ──────────────────────────────────────────────── */
void exception_handler(exc_source_t src, exc_type_t type,
                       exception_frame_t *frame)
{
    uint32_t ec = ESR_EC(frame->esr);

    /* IRQ: percorso veloce, gestito separatamente */
    if (type == EXC_TYPE_IRQ) {
        irq_handler(src, frame);
        return;
    }

    /* FIQ: non usati, ignora */
    if (type == EXC_TYPE_FIQ) {
        return;
    }

    /* SVC da lower EL: dispatch al syscall handler (M3-01) */
    if (type == EXC_TYPE_SYNC && src == EXC_SRC_LOWER64 && ec == EC_SVC_AA64) {
        syscall_dispatch(frame);
        return;
    }

    if (type == EXC_TYPE_SYNC && src == EXC_SRC_LOWER64 && ec == EC_DABT_LOWER) {
        if (mmu_handle_user_fault(sched_task_space(current_task), frame->far, frame->esr) == 0)
            return;
    }

    /* Tutte le altre eccezioni: crash reporter e halt */
    kdebug_exception_report(src, type, frame);
}

/* ── IRQ Handler ─────────────────────────────────────────────────────── */
/*
 * Chiamato da exception_handler() per ogni IRQ ricevuto.
 * Delega a gic_handle_irq() che esegue IAR→dispatch→EOIR in O(1).
 *
 * RT: questo percorso NON deve allocare memoria, NON deve acquisire lock
 * non-preemptibili, NON deve stampare su UART (tranne in handler specifici).
 */
#include "gic.h"

void irq_handler(exc_source_t src, exception_frame_t *frame)
{
    (void)src;
    (void)frame;
    gic_handle_irq();
}

/* ── Inizializzazione ────────────────────────────────────────────────── */
/*
 * exception_init() è chiamata dal boot prima di abilitare gli interrupt.
 * Imposta VBAR_EL1 per puntare alla tabella vettori in vectors.S.
 */
extern void exception_vectors(void); /* simbolo definito in vectors.S */

void exception_init(void)
{
    __asm__ volatile(
        "adr  x0, exception_vectors \n"
        "msr  vbar_el1, x0          \n"
        "isb                        \n"
        ::: "x0"
    );
    uart_puts("[EnlilOS] Exception vectors installati\n");
}
