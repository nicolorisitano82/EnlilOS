/*
 * EnlilOS Microkernel - Pseudo-Terminal (PTY) subsystem (M11-05f)
 *
 * Coppia master/slave:
 *   open("/dev/ptmx")   -> master fd
 *   open("/dev/pts/N")  -> slave fd  (dopo unlockpt)
 *   write(master) -> line-discipline -> read(slave)
 *   write(slave)  -> OPOST           -> read(master)
 */

#ifndef ENLILOS_PTY_H
#define ENLILOS_PTY_H

#include "types.h"
#include "termios.h"
#include "syscall.h"   /* winsize_t, O_NONBLOCK, EFAULT, EIO, EAGAIN, EINVAL */

#define PTY_MAX       8U
#define PTY_BUF_SIZE  4096U
#define PTY_EDIT_MAX  256U

/* DEV_NODE IDs per devfs (dopo RANDOM=10) */
#define DEV_NODE_PTMX      11U
#define DEV_NODE_PTS_BASE  12U   /* pts/0=12 ... pts/7=19 */
#define DEV_NODE_PTS_DIR   20U   /* /dev/pts directory */

typedef struct {
    uint8_t   in_use;
    uint8_t   master_open;
    uint8_t   slave_open;
    uint8_t   eof_pending;   /* master ha scritto VEOF su riga vuota */
    uint8_t   locked;        /* 1=locked (slave non apribile), 0=unlocked */
    uint8_t   _pad[3];
    uint32_t  idx;           /* indice slot 0..PTY_MAX-1 */
    uint32_t  slave_pgid;    /* pgid foreground slave, per SIGWINCH/segnali */

    /* master→slave: write(master) -> LD -> read(slave) */
    uint8_t  m2s_buf[PTY_BUF_SIZE];
    uint32_t m2s_head, m2s_tail;

    /* slave→master: write(slave) -> OPOST -> read(master) */
    uint8_t  s2m_buf[PTY_BUF_SIZE];
    uint32_t s2m_head, s2m_tail;

    /* buffer canonico per la line discipline master→slave */
    uint8_t  edit_buf[PTY_EDIT_MAX];
    uint16_t edit_len;
    uint8_t  _pad2[2];

    termios_t termios;
    winsize_t winsize;
} pty_t;

/* Lifecycle */
void    pty_init(void);
pty_t  *pty_alloc(uint32_t *idx_out);  /* alloca + master_open=1, locked=1 */
pty_t  *pty_get(uint32_t idx);
void    pty_open_slave(pty_t *p, uint32_t pgid);
void    pty_close_master(pty_t *p);
void    pty_close_slave(pty_t *p);

/* I/O */
ssize_t pty_master_write(pty_t *p, const void *buf, size_t count, uint32_t flags);
ssize_t pty_master_read(pty_t *p, void *buf, size_t count, uint32_t flags);
ssize_t pty_slave_write(pty_t *p, const void *buf, size_t count, uint32_t flags);
ssize_t pty_slave_read(pty_t *p, void *buf, size_t count, uint32_t flags);

int     pty_master_has_data(pty_t *p);
int     pty_slave_has_data(pty_t *p);

/* Attributi */
int     pty_tcgetattr(pty_t *p, termios_t *out);
int     pty_tcsetattr(pty_t *p, int action, const termios_t *in);
int     pty_get_winsize(pty_t *p, winsize_t *out);
int     pty_set_winsize(pty_t *p, const winsize_t *in);  /* invia SIGWINCH */

#endif /* ENLILOS_PTY_H */
