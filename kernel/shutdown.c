/*
 * EnlilOS — Graceful system shutdown (kernel-side)
 *
 * Sequenza:
 *  1. SIGTERM a tutti i task user-space non kernel/idle
 *  2. Aspetta che terminino (timeout 2 s con sched_yield)
 *  3. SIGKILL ai superstiti
 *  4. vfs_sync() — journal commit ext4 + sync tutti i mount
 *  5. blk_flush_sync() — flush driver virtio-blk
 *  6. Disabilita IRQ globalmente
 *  7. PSCI SYSTEM_OFF / SYSTEM_RESET / WFE halt
 */

#include "shutdown.h"
#include "psci.h"
#include "sched.h"
#include "signal.h"
#include "vfs.h"
#include "blk.h"
#include "uart.h"
#include "timer.h"
#include "gic.h"

/* ── helper: conta task user-space non zombie ──────────────────── */
static uint32_t count_user_alive(void)
{
    uint32_t n   = sched_task_count_total();
    uint32_t cnt = 0U;

    for (uint32_t i = 0U; i < n; i++) {
        const sched_tcb_t *t = sched_task_at(i);
        if (!t) continue;
        if (t->flags & (TCB_FLAG_KERNEL | TCB_FLAG_IDLE)) continue;
        if (t->state == TCB_STATE_ZOMBIE) continue;
        cnt++;
    }
    return cnt;
}

/* ── helper: invia segnale a tutti i task user-space non zombie ── */
static void signal_all_user(int sig)
{
    uint32_t n = sched_task_count_total();

    for (uint32_t i = 0U; i < n; i++) {
        const sched_tcb_t *t = sched_task_at(i);
        if (!t) continue;
        if (t->flags & (TCB_FLAG_KERNEL | TCB_FLAG_IDLE)) continue;
        if (t->state == TCB_STATE_ZOMBIE) continue;
        (void)signal_send_pid(t->pid, sig);
    }
}

/* ── sequenza ordinata ─────────────────────────────────────────── */
void shutdown_system(int cmd)
{
    uart_puts("\n[shutdown] Avvio sequenza di spegnimento...\n");

    /* 1. SIGTERM a tutti i task user */
    uart_puts("[shutdown] SIGTERM a tutti i task user-space\n");
    signal_all_user(SIGTERM);

    /* 2. Aspetta fino a 2 s che terminino */
    {
        uint64_t deadline = timer_now_ms() + 2000ULL;

        while (timer_now_ms() < deadline && count_user_alive() > 0U)
            sched_yield();
    }

    /* 3. SIGKILL ai superstiti */
    if (count_user_alive() > 0U) {
        uart_puts("[shutdown] SIGKILL ai task rimasti\n");
        signal_all_user(SIGKILL);

        uint64_t deadline = timer_now_ms() + 500ULL;
        while (timer_now_ms() < deadline && count_user_alive() > 0U)
            sched_yield();
    }

    /* 4. Flush filesystem: journal commit ext4 + sync tutti i mount */
    uart_puts("[shutdown] vfs_sync()...\n");
    (void)vfs_sync();

    /* 5. Flush driver block device */
    if (blk_is_ready()) {
        uart_puts("[shutdown] blk_flush_sync()...\n");
        (void)blk_flush_sync();
    }

    /* 6. Disabilita IRQ */
    uart_puts("[shutdown] Disabilito IRQ\n");
    gic_disable_irqs();

    /* 7. PSCI o halt */
    switch (cmd) {
    case SHUTDOWN_REBOOT:
        uart_puts("[shutdown] PSCI SYSTEM_RESET\n");
        psci_system_reset();
        break;
    case SHUTDOWN_HALT:
        uart_puts("[shutdown] Halt (WFE loop)\n");
        while (1)
            __asm__ volatile("wfe");
        break;
    case SHUTDOWN_POWEROFF:
    default:
        uart_puts("[shutdown] PSCI SYSTEM_OFF\n");
        psci_system_off();
        break;
    }
}
