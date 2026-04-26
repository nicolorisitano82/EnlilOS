/*
 * EnlilOS Microkernel - Pseudo-Terminal (PTY) subsystem (M11-05f)
 *
 * Semantica:
 *   - Master apre /dev/ptmx -> riceve master fd (locked)
 *   - unlockpt(master) -> slave diventa apribile
 *   - Slave apre /dev/pts/N -> riceve slave fd
 *   - write(master) -> line discipline -> read(slave)
 *   - write(slave) -> OPOST -> read(master)
 *   - isatty(slave) = 1
 *   - tcgetattr/tcsetattr su slave agiscono sulla termios per-PTY
 *   - TIOCSWINSZ su master aggiorna winsize e invia SIGWINCH a slave_pgid
 */

#include "pty.h"
#include "sched.h"
#include "signal.h"

/* ── pool globale ────────────────────────────────────────────────── */

static pty_t pty_pool[PTY_MAX];

/* ── ring buffer helpers ─────────────────────────────────────────── */

static inline int rb_empty(uint32_t head, uint32_t tail)
{
    return head == tail;
}

static inline int rb_full(uint32_t head, uint32_t tail, uint32_t cap)
{
    return ((head + 1U) % cap) == tail;
}

static int m2s_push(pty_t *p, uint8_t c)
{
    if (rb_full(p->m2s_head, p->m2s_tail, PTY_BUF_SIZE)) return 0;
    p->m2s_buf[p->m2s_head] = c;
    p->m2s_head = (p->m2s_head + 1U) % PTY_BUF_SIZE;
    return 1;
}

static int m2s_pop(pty_t *p, uint8_t *out)
{
    if (rb_empty(p->m2s_head, p->m2s_tail)) return 0;
    *out = p->m2s_buf[p->m2s_tail];
    p->m2s_tail = (p->m2s_tail + 1U) % PTY_BUF_SIZE;
    return 1;
}

static int s2m_push(pty_t *p, uint8_t c)
{
    if (rb_full(p->s2m_head, p->s2m_tail, PTY_BUF_SIZE)) return 0;
    p->s2m_buf[p->s2m_head] = c;
    p->s2m_head = (p->s2m_head + 1U) % PTY_BUF_SIZE;
    return 1;
}

static int s2m_pop(pty_t *p, uint8_t *out)
{
    if (rb_empty(p->s2m_head, p->s2m_tail)) return 0;
    *out = p->s2m_buf[p->s2m_tail];
    p->s2m_tail = (p->s2m_tail + 1U) % PTY_BUF_SIZE;
    return 1;
}

/* ── termios defaults ────────────────────────────────────────────── */

static void pty_termios_defaults(termios_t *t)
{
    uint32_t i;
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    t->c_lflag = ECHO | ECHOE | ICANON | ISIG | IEXTEN;
    for (i = 0U; i < 20U; i++) t->c_cc[i] = 0U;
    t->c_cc[VINTR]  = 0x03U;  /* ^C */
    t->c_cc[VEOF]   = 0x04U;  /* ^D */
    t->c_cc[VERASE] = 0x7FU;  /* DEL */
    t->c_cc[VKILL]  = 0x15U;  /* ^U */
    t->c_cc[VMIN]   = 1U;
    t->c_cc[VTIME]  = 0U;
    t->c_cc[VSUSP]  = 0x1AU;  /* ^Z */
}

/* ── line discipline: processa un byte da master verso slave ─────── */

static void pty_ld_byte(pty_t *p, uint8_t c)
{
    /* ICRNL: CR -> NL */
    if ((p->termios.c_iflag & ICRNL) && c == '\r')
        c = '\n';

    /* ISIG: ^C / ^Z */
    if (p->termios.c_lflag & ISIG) {
        if (c == p->termios.c_cc[VINTR]) {
            if (p->slave_pgid)
                (void)signal_send_pgrp(p->slave_pgid, SIGINT);
            if (p->termios.c_lflag & ECHO) {
                (void)s2m_push(p, '^');
                (void)s2m_push(p, 'C');
                (void)s2m_push(p, '\n');
            }
            p->edit_len = 0U;
            return;
        }
        if (c == p->termios.c_cc[VSUSP]) {
            if (p->slave_pgid)
                (void)signal_send_pgrp(p->slave_pgid, SIGTSTP);
            if (p->termios.c_lflag & ECHO) {
                (void)s2m_push(p, '^');
                (void)s2m_push(p, 'Z');
                (void)s2m_push(p, '\n');
            }
            p->edit_len = 0U;
            return;
        }
    }

    if (p->termios.c_lflag & ICANON) {
        /* VERASE / BS */
        if (c == p->termios.c_cc[VERASE] || c == 0x08U) {
            if (p->edit_len > 0U) {
                p->edit_len--;
                if (p->termios.c_lflag & ECHOE) {
                    (void)s2m_push(p, '\b');
                    (void)s2m_push(p, ' ');
                    (void)s2m_push(p, '\b');
                }
            }
            return;
        }
        /* VKILL */
        if (c == p->termios.c_cc[VKILL]) {
            while (p->edit_len > 0U) {
                p->edit_len--;
                if (p->termios.c_lflag & ECHOE) {
                    (void)s2m_push(p, '\b');
                    (void)s2m_push(p, ' ');
                    (void)s2m_push(p, '\b');
                }
            }
            return;
        }
        /* VEOF: ^D */
        if (c == p->termios.c_cc[VEOF]) {
            if (p->edit_len > 0U) {
                uint16_t i;
                for (i = 0U; i < p->edit_len; i++)
                    (void)m2s_push(p, p->edit_buf[i]);
                p->edit_len = 0U;
            } else {
                p->eof_pending = 1U;
            }
            return;
        }
        /* newline: commit riga */
        if (c == '\n') {
            if (p->termios.c_lflag & ECHO) {
                if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR))
                    (void)s2m_push(p, '\r');
                (void)s2m_push(p, '\n');
            }
            {
                uint16_t i;
                for (i = 0U; i < p->edit_len; i++)
                    (void)m2s_push(p, p->edit_buf[i]);
            }
            (void)m2s_push(p, '\n');
            p->edit_len = 0U;
            return;
        }
        /* carattere normale */
        if (c == '\t' || (c >= 32U && c < 127U)) {
            if (p->edit_len < (uint16_t)PTY_EDIT_MAX)
                p->edit_buf[p->edit_len++] = c;
            if (p->termios.c_lflag & ECHO)
                (void)s2m_push(p, c);
        }
        return;
    }

    /* modalita' raw: bypassa LD */
    (void)m2s_push(p, c);
    if (p->termios.c_lflag & ECHO)
        (void)s2m_push(p, c);
}

/* ── lifecycle ───────────────────────────────────────────────────── */

void pty_init(void)
{
    uint32_t i;
    for (i = 0U; i < PTY_MAX; i++) {
        pty_pool[i].in_use = 0U;
        pty_pool[i].idx    = i;
    }
}

pty_t *pty_alloc(uint32_t *idx_out)
{
    uint32_t i;
    for (i = 0U; i < PTY_MAX; i++) {
        if (!pty_pool[i].in_use) {
            pty_t    *p   = &pty_pool[i];
            uint8_t  *raw = (uint8_t *)(void *)p;
            uint32_t  j;

            for (j = 0U; j < (uint32_t)sizeof(pty_t); j++)
                raw[j] = 0U;

            p->in_use      = 1U;
            p->master_open = 1U;
            p->slave_open  = 0U;
            p->locked      = 1U;   /* locked fino a unlockpt() */
            p->idx         = i;
            pty_termios_defaults(&p->termios);
            p->winsize.ws_row    = 25U;
            p->winsize.ws_col    = 80U;
            p->winsize.ws_xpixel = 0U;
            p->winsize.ws_ypixel = 0U;
            if (idx_out) *idx_out = i;
            return p;
        }
    }
    return (pty_t *)0;   /* ENOMEM */
}

pty_t *pty_get(uint32_t idx)
{
    if (idx >= PTY_MAX)            return (pty_t *)0;
    if (!pty_pool[idx].in_use)     return (pty_t *)0;
    return &pty_pool[idx];
}

void pty_open_slave(pty_t *p, uint32_t pgid)
{
    if (!p) return;
    p->slave_open = 1U;
    if (pgid) p->slave_pgid = pgid;
}

void pty_close_master(pty_t *p)
{
    if (!p) return;
    p->master_open = 0U;
    if (!p->slave_open)
        p->in_use = 0U;
}

void pty_close_slave(pty_t *p)
{
    if (!p) return;
    p->slave_open  = 0U;
    p->slave_pgid  = 0U;
    if (!p->master_open)
        p->in_use = 0U;
}

/* ── query ───────────────────────────────────────────────────────── */

int pty_master_has_data(pty_t *p)
{
    return !rb_empty(p->s2m_head, p->s2m_tail);
}

int pty_slave_has_data(pty_t *p)
{
    return p->eof_pending || !rb_empty(p->m2s_head, p->m2s_tail);
}

/* ── I/O ─────────────────────────────────────────────────────────── */

ssize_t pty_master_write(pty_t *p, const void *buf, size_t count, uint32_t flags)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t i;
    (void)flags;

    if (!p || !buf) return -EFAULT;
    if (!p->slave_open) return -EIO;

    for (i = 0U; i < count; i++)
        pty_ld_byte(p, src[i]);

    return (ssize_t)count;
}

ssize_t pty_master_read(pty_t *p, void *buf, size_t count, uint32_t flags)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t   got = 0U;

    if (!p || !buf) return -EFAULT;
    if (!p->master_open) return -EIO;

    if (flags & O_NONBLOCK) {
        while (got < count) {
            uint8_t c;
            if (!s2m_pop(p, &c)) break;
            dst[got++] = c;
        }
        return (got > 0U) ? (ssize_t)got : -EAGAIN;
    }

    /* blocking */
    for (;;) {
        while (got < count) {
            uint8_t c;
            if (!s2m_pop(p, &c)) break;
            dst[got++] = c;
        }
        if (got > 0U)          return (ssize_t)got;
        if (!p->slave_open)    return 0;   /* HUP */
        sched_yield();
    }
}

ssize_t pty_slave_write(pty_t *p, const void *buf, size_t count, uint32_t flags)
{
    const uint8_t *src = (const uint8_t *)buf;
    size_t i;
    (void)flags;

    if (!p || !buf) return -EFAULT;
    if (!p->master_open) return -EIO;

    for (i = 0U; i < count; i++) {
        uint8_t c = src[i];
        /* OPOST + ONLCR: '\n' -> '\r\n' */
        if ((p->termios.c_oflag & OPOST) && (p->termios.c_oflag & ONLCR) && c == '\n')
            (void)s2m_push(p, '\r');
        (void)s2m_push(p, c);
    }
    return (ssize_t)count;
}

ssize_t pty_slave_read(pty_t *p, void *buf, size_t count, uint32_t flags)
{
    uint8_t *dst = (uint8_t *)buf;
    size_t   got = 0U;

    if (!p || !buf) return -EFAULT;
    if (!p->slave_open) return -EIO;

    /* Aggiorna slave_pgid dal task corrente (come tty_adopt_current_session) */
    if (current_task && sched_task_is_user(current_task)) {
        uint32_t pgid = sched_task_pgid(current_task);
        if (pgid) p->slave_pgid = pgid;
    }

    for (;;) {
        /* EOF: master ha inviato VEOF su riga vuota */
        if (p->eof_pending && rb_empty(p->m2s_head, p->m2s_tail)) {
            p->eof_pending = 0U;
            return 0;
        }

        while (got < count) {
            uint8_t c;
            if (!m2s_pop(p, &c)) break;
            dst[got++] = c;
            /* modalita' canonica: ritorna dopo '\n' */
            if ((p->termios.c_lflag & ICANON) && c == '\n')
                return (ssize_t)got;
        }
        if (got > 0U) return (ssize_t)got;

        if (flags & O_NONBLOCK) return -EAGAIN;
        if (!p->master_open)    return 0;   /* HUP */
        sched_yield();
    }
}

/* ── attributi ───────────────────────────────────────────────────── */

int pty_tcgetattr(pty_t *p, termios_t *out)
{
    if (!p || !out) return -EFAULT;
    *out = p->termios;
    return 0;
}

int pty_tcsetattr(pty_t *p, int action, const termios_t *in)
{
    if (!p || !in) return -EFAULT;
    if (action != TCSANOW && action != TCSADRAIN && action != TCSAFLUSH)
        return -EINVAL;
    p->termios = *in;
    /* Valori di default obbligatori */
    if (p->termios.c_cc[VERASE] == 0U) p->termios.c_cc[VERASE] = 0x7FU;
    if (p->termios.c_cc[VINTR]  == 0U) p->termios.c_cc[VINTR]  = 0x03U;
    if (p->termios.c_cc[VSUSP]  == 0U) p->termios.c_cc[VSUSP]  = 0x1AU;
    if (p->termios.c_cc[VMIN]   == 0U) p->termios.c_cc[VMIN]   = 1U;
    if (action == TCSAFLUSH) {
        p->m2s_head = p->m2s_tail = 0U;
        p->edit_len    = 0U;
        p->eof_pending = 0U;
    }
    return 0;
}

int pty_get_winsize(pty_t *p, winsize_t *out)
{
    if (!p || !out) return -EFAULT;
    *out = p->winsize;
    return 0;
}

int pty_set_winsize(pty_t *p, const winsize_t *in)
{
    if (!p || !in) return -EFAULT;
    p->winsize = *in;
    /* Notifica processo foreground sullo slave */
    if (p->slave_pgid)
        (void)signal_send_pgrp(p->slave_pgid, SIGWINCH);
    return 0;
}
