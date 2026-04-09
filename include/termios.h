/*
 * EnlilOS Microkernel - Minimal termios subset (M8-08c)
 *
 * Sottoinsieme sufficiente per shell interattive, raw mode e isatty().
 */

#ifndef ENLILOS_TERMIOS_H
#define ENLILOS_TERMIOS_H

#include "types.h"

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_cc[20];
} termios_t;

/* c_iflag */
#define ICRNL   (1U << 0)
#define IXON    (1U << 1)

/* c_oflag */
#define OPOST   (1U << 0)
#define ONLCR   (1U << 1)

/* c_cflag */
#define CS8     (1U << 0)
#define CREAD   (1U << 1)

/* c_lflag */
#define ECHO    (1U << 0)
#define ECHOE   (1U << 1)
#define ICANON  (1U << 2)
#define ISIG    (1U << 3)
#define IEXTEN  (1U << 4)

/* c_cc indices */
#define VINTR   0
#define VEOF    1
#define VERASE  2
#define VKILL   3
#define VMIN    4
#define VTIME   5
#define VSUSP   6

/* tcsetattr actions */
#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

#endif /* ENLILOS_TERMIOS_H */
