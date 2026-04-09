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
#include "termios.h"

void tty_init(void);
uint64_t tty_read(char *buf, uint64_t cnt);
int tty_check_output_current(void);
int tty_tcsetpgrp_current(uint32_t pgid);
uint32_t tty_tcgetpgrp(void);
int tty_tcgetattr(termios_t *out);
int tty_tcsetattr(int action, const termios_t *in);

#endif /* ENLILOS_TTY_H */
