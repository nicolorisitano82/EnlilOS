/*
 * EnlilOS Microkernel - Console Input API
 *
 * Backend attuali:
 *   - VirtIO Input keyboard su QEMU virt (target grafici)
 *   - UART PL011 come fallback su stdio QEMU
 *
 * L'API conserva il nome "keyboard" per non cambiare i call site del kernel e
 * delle syscall.
 *
 * RT design:
 *   keyboard_getc() è O(1), non blocca e non alloca.
 */

#ifndef ENLILOS_KEYBOARD_H
#define ENLILOS_KEYBOARD_H

#include "types.h"

/* Inizializza il backend di input console. */
void keyboard_init(void);

/* Ritorna il prossimo byte disponibile su stdin, oppure -1 se vuoto. */
int keyboard_getc(void);

#endif /* ENLILOS_KEYBOARD_H */
