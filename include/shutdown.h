/*
 * EnlilOS — Graceful system shutdown
 *
 * Sequenza ordinata: SIGTERM → drain → vfs_sync → blk_flush → PSCI.
 */

#ifndef ENLILOS_SHUTDOWN_H
#define ENLILOS_SHUTDOWN_H

#define SHUTDOWN_POWEROFF   0   /* PSCI SYSTEM_OFF  */
#define SHUTDOWN_REBOOT     1   /* PSCI SYSTEM_RESET */
#define SHUTDOWN_HALT       2   /* WFE halt loop (no PSCI) */

/* Esegue la sequenza completa di shutdown. Non ritorna mai. */
void shutdown_system(int cmd) __attribute__((noreturn));

#endif /* ENLILOS_SHUTDOWN_H */
