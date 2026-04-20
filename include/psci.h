/*
 * EnlilOS — PSCI (Power State Coordination Interface) AArch64
 *
 * Chiama il firmware via SMC #0 per power-off e reset.
 * Su QEMU virt, PSCI è esposto tramite smc (default).
 */

#ifndef ENLILOS_PSCI_H
#define ENLILOS_PSCI_H

/* PSCI 0.2 / 1.0 function IDs (32-bit calling convention, SMC32) */
#define PSCI_VERSION            0x84000000U
#define PSCI_CPU_OFF            0x84000002U
#define PSCI_SYSTEM_OFF         0x84000008U
#define PSCI_SYSTEM_RESET       0x84000009U

void psci_system_off(void)   __attribute__((noreturn));
void psci_system_reset(void) __attribute__((noreturn));

#endif /* ENLILOS_PSCI_H */
