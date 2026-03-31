/*
 * NROS Microkernel - PL011 UART Driver
 * Target: QEMU virt machine (PL011 @ 0x09000000)
 *
 * In un microkernel maturo, questo driver girerebbe in user-space
 * come server. Per il boot iniziale, lo includiamo nel kernel.
 */

#include "uart.h"

/* PL011 UART base address per QEMU virt */
#define UART0_BASE  0x09000000

/* Registri PL011 */
#define UART_DR     (UART0_BASE + 0x00)  /* Data Register */
#define UART_FR     (UART0_BASE + 0x18)  /* Flag Register */
#define UART_IBRD   (UART0_BASE + 0x24)  /* Integer Baud Rate */
#define UART_FBRD   (UART0_BASE + 0x28)  /* Fractional Baud Rate */
#define UART_LCR_H  (UART0_BASE + 0x2C)  /* Line Control */
#define UART_CR     (UART0_BASE + 0x30)  /* Control Register */

/* Flag bits */
#define UART_FR_TXFF  (1 << 5)  /* Transmit FIFO full */

void uart_init(void)
{
    /* Disabilita UART */
    MMIO_WRITE32(UART_CR, 0x00);

    /* Baud rate 115200 con clock 24MHz:
     * Divisor = 24000000 / (16 * 115200) = 13.0208
     * IBRD = 13, FBRD = round(0.0208 * 64) = 1 */
    MMIO_WRITE32(UART_IBRD, 13);
    MMIO_WRITE32(UART_FBRD, 1);

    /* 8 bit, no parity, 1 stop bit, FIFO abilitato */
    MMIO_WRITE32(UART_LCR_H, (3 << 5) | (1 << 4));

    /* Abilita UART, TX e RX */
    MMIO_WRITE32(UART_CR, (1 << 0) | (1 << 8) | (1 << 9));
}

void uart_putc(char c)
{
    /* Attendi che il TX FIFO non sia pieno */
    while (MMIO_READ32(UART_FR) & UART_FR_TXFF)
        ;
    MMIO_WRITE32(UART_DR, (uint32_t)c);
}

void uart_puts(const char *s)
{
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}
