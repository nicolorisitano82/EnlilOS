/*
 * EnlilOS — PSCI via SMC
 *
 * Su QEMU virt, PSCI_SYSTEM_OFF (0x84000008) via smc #0 termina la VM.
 * Se la chiamata SMC non ritorna (non dovrebbe), l'halt WFE garantisce
 * che la CPU non esegua codice casuale.
 */

#include "psci.h"

void psci_system_off(void)
{
    register unsigned long x0 __asm__("x0") = PSCI_SYSTEM_OFF;
    __asm__ volatile(
        "smc #0"
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
        "smc #0"
        :: "r"(x0)
        : "x1", "x2", "x3", "memory"
    );
    while (1)
        __asm__ volatile("wfe");
}
