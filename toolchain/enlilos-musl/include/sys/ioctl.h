#ifndef ENLILOS_MUSL_SYS_IOCTL_H
#define ENLILOS_MUSL_SYS_IOCTL_H

#include <stdint.h>

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#define TCGETS              0x5401UL
#define TCSETS              0x5402UL
#define TIOCGPGRP           0x540FUL
#define TIOCSPGRP           0x5410UL
#define TIOCGWINSZ          0x5413UL
#define TIOCSWINSZ          0x5414UL
#define FIONBIO             0x5421UL
#define TIOCGPTN            0x80045430UL   /* get PTY slave number */
#define TIOCSPTLCK          0x40045431UL   /* lock/unlock PTY slave */

int ioctl(int fd, unsigned long req, ...);

#endif
