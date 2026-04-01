/*
 * EnlilOS Microkernel - PL011 UART Driver
 * Comunicazione seriale per debug e output testuale
 */

#ifndef ENLILOS_UART_H
#define ENLILOS_UART_H

#include "types.h"

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
int uart_getc_nonblock(void);

#endif
