/*
 * EnlilOS Microkernel - Terminal Line Discipline (M4-03)
 *
 * Fornisce:
 *   - echo dei caratteri digitati
 *   - modalita' canonica (read ritorna linee complete)
 *   - editing con backspace
 *   - segnale console minimale per CTRL+C
 */

#ifndef ENLILOS_TTY_H
#define ENLILOS_TTY_H

#include "types.h"

void tty_init(void);
uint64_t tty_read(char *buf, uint64_t cnt);

#endif /* ENLILOS_TTY_H */
