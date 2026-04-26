#ifndef ENLILOS_MUSL_PTY_H
#define ENLILOS_MUSL_PTY_H

#include <termios.h>
#include <sys/ioctl.h>  /* struct winsize */

/* BSD openpty: alloca e configura coppia master/slave in un passo solo */
int openpty(int *amaster, int *aslave, char *name,
            const struct termios *termp, const struct winsize *winp);

#endif /* ENLILOS_MUSL_PTY_H */
