#ifndef ENLILOS_MUSL_TERMIOS_H
#define ENLILOS_MUSL_TERMIOS_H

#include <stdint.h>

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[20];
};

typedef struct termios termios_t;

#define ICRNL   (1U << 0)
#define IXON    (1U << 1)

#define OPOST   (1U << 0)
#define ONLCR   (1U << 1)

#define CS8     (1U << 0)
#define CREAD   (1U << 1)

#define ECHO    (1U << 0)
#define ECHOE   (1U << 1)
#define ICANON  (1U << 2)
#define ISIG    (1U << 3)
#define IEXTEN  (1U << 4)

#define VINTR   0
#define VEOF    1
#define VERASE  2
#define VKILL   3
#define VMIN    4
#define VTIME   5
#define VSUSP   6

#define TCSANOW     0
#define TCSADRAIN   1
#define TCSAFLUSH   2

int tcgetattr(int fd, struct termios *term);
int tcsetattr(int fd, int action, const struct termios *term);

#endif
