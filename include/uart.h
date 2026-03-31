/*
 * NROS Microkernel - PL011 UART Driver
 * Comunicazione seriale per debug e output testuale
 */

#ifndef NROS_UART_H
#define NROS_UART_H

#include "types.h"

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);

#endif
