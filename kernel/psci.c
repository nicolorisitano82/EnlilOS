/*
 * EnlilOS — PSCI via HVC
 *
 * QEMU virt senza -machine virt,secure=on non ha EL3.
 * PSCI è esposto tramite HVC (EL2), non SMC (EL3).
 * Usare smc #0 senza EL3 causa eccezione EC=0x00 a EL1.
 */

#include "psci.h"

void psci_system_off(void)
{
    register unsigned long x0 __asm__("x0") = PSCI_SYSTEM_OFF;
    __asm__ volatile(
        "hvc #0"
        :: "r"(x0)
        : "x1", "x2", "x3", "memory"
    );
    /* Non dovrebbe mai arrivare qui */
    while (1)
        __asm__ volatile("wfe");
}

void psci_system_reset(void)
{
    register unsigned long x0 __asm__("x0") = PSCI_SYSTEM_RESET;
    __asm__ volatile(
        "hvc #0"
        :: "r"(x0)
        : "x1", "x2", "x3", "memory"
    );
    while (1)
        __asm__ volatile("wfe");
}
