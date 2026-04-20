/*
 * EnlilOS Microkernel - Terminal Line Discipline (M4-03)
 *
 * Implementazione minimale stile TTY:
 *   - modalita' canonica: la read ritorna solo linee terminate da '\n'
 *   - echo su UART del testo digitato
 *   - backspace locale con editing della linea corrente
 *   - CTRL+C / CTRL+Z: invalida la linea corrente e genera segnali al foreground pgrp
 *   - le read canoniche tornano -EINTR se esistono segnali non mascherati
 */

#include "tty.h"

#include "keyboard.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "term80.h"
#include "uart.h"

#define TTY_EDIT_MAX    256U
#define TTY_READY_MAX   512U

#define TTY_CTRL_C      0x03
#define TTY_CTRL_Z      0x1A
#define TTY_BS          0x08
#define TTY_DEL         0x7F

static uint8_t tty_edit_buf[TTY_EDIT_MAX];
static uint16_t tty_edit_len;

static uint8_t tty_ready_buf[TTY_READY_MAX];
static uint16_t tty_ready_head;
static uint16_t tty_ready_tail;
static uint8_t tty_eof_pending;

static uint32_t tty_foreground_pgid;
static uint32_t tty_session_sid;
static termios_t tty_termios;

static inline int tty_ready_empty(void)
{
    return tty_ready_head == tty_ready_tail;
}

static inline int tty_ready_full(void)
{
    return (uint16_t)((tty_ready_head + 1U) % TTY_READY_MAX) == tty_ready_tail;
}

static int tty_ready_push(uint8_t c)
{
    if (tty_ready_full()) return 0;

    tty_ready_buf[tty_ready_head] = c;
    tty_ready_head = (uint16_t)((tty_ready_head + 1U) % TTY_READY_MAX);
    return 1;
}

static int tty_ready_pop(uint8_t *out)
{
    if (tty_ready_empty()) return 0;

    *out = tty_ready_buf[tty_ready_tail];
    tty_ready_tail = (uint16_t)((tty_ready_tail + 1U) % TTY_READY_MAX);
    return 1;
}

static void tty_echo_char(uint8_t c)
{
    if (c == '\n') {
        term80_putc('\n');
        if ((tty_termios.c_oflag & OPOST) && (tty_termios.c_oflag & ONLCR))
            uart_putc('\r');
        uart_putc('\n');
        return;
    }
    term80_putc((char)c);
    uart_putc((char)c);
}

static void tty_echo_backspace(void)
{
    term80_putc('\b');
    uart_putc('\b');
    uart_putc(' ');
    uart_putc('\b');
}

static uint32_t tty_current_pgid(void)
{
    return current_task ? sched_task_pgid(current_task) : 0U;
}

static uint32_t tty_current_sid(void)
{
    return current_task ? sched_task_sid(current_task) : 0U;
}

static void tty_adopt_current_session(void)
{
    uint32_t sid = tty_current_sid();
    uint32_t pgid = tty_current_pgid();

    if (sid == 0U || pgid == 0U)
        return;

    if (tty_session_sid == 0U || !sched_task_has_session(tty_session_sid)) {
        tty_session_sid = sid;
        tty_foreground_pgid = pgid;
        return;
    }

    if (tty_session_sid == sid &&
        !sched_task_has_pgrp(tty_session_sid, tty_foreground_pgid))
        tty_foreground_pgid = pgid;
}

static int tty_is_background_current(void)
{
    uint32_t sid = tty_current_sid();
    uint32_t pgid = tty_current_pgid();

    if (!current_task || !sched_task_is_user(current_task))
        return 0;
    if (tty_session_sid == 0U || sid == 0U)
        return 0;
    if (sid != tty_session_sid)
        return 0;
    return pgid != 0U && pgid != tty_foreground_pgid;
}

static void tty_signal_foreground(int sig)
{
    if (tty_foreground_pgid != 0U)
        (void)signal_send_pgrp(tty_foreground_pgid, sig);
}

static void tty_signal_current_group(int sig)
{
    uint32_t pgid = tty_current_pgid();

    if (pgid != 0U)
        (void)signal_send_pgrp(pgid, sig);
}

static void tty_commit_line(void)
{
    for (uint16_t i = 0; i < tty_edit_len; i++) {
        if (!tty_ready_push(tty_edit_buf[i]))
            break;
    }
    (void)tty_ready_push('\n');
    tty_edit_len = 0U;
}

static void tty_commit_partial_line(void)
{
    for (uint16_t i = 0; i < tty_edit_len; i++) {
        if (!tty_ready_push(tty_edit_buf[i]))
            break;
    }
    tty_edit_len = 0U;
}

static int tty_is_canonical(void)
{
    return (tty_termios.c_lflag & ICANON) != 0U;
}

static int tty_is_echo_enabled(void)
{
    return (tty_termios.c_lflag & ECHO) != 0U;
}

static void tty_termios_defaults(termios_t *t)
{
    if (!t)
        return;

    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    t->c_lflag = ECHO | ECHOE | ICANON | ISIG | IEXTEN;
    for (uint32_t i = 0U; i < (uint32_t)sizeof(t->c_cc); i++)
        t->c_cc[i] = 0U;

    t->c_cc[VINTR]  = TTY_CTRL_C;
    t->c_cc[VEOF]   = 0x04U;
    t->c_cc[VERASE] = TTY_DEL;
    t->c_cc[VKILL]  = 0x15U;
    t->c_cc[VMIN]   = 1U;
    t->c_cc[VTIME]  = 0U;
    t->c_cc[VSUSP]  = TTY_CTRL_Z;
}

static void tty_handle_input_char(uint8_t c)
{
    if ((tty_termios.c_iflag & ICRNL) && c == '\r')
        c = '\n';

    if (tty_termios.c_lflag & ISIG) {
        if (c == tty_termios.c_cc[VINTR]) {
            tty_edit_len = 0U;
            tty_signal_foreground(SIGINT);
            if (tty_is_echo_enabled()) {
                uart_putc('^');
                uart_putc('C');
                tty_echo_char('\n');
            }
            return;
        }

        if (c == tty_termios.c_cc[VSUSP]) {
            tty_edit_len = 0U;
            tty_signal_foreground(SIGTSTP);
            if (tty_is_echo_enabled()) {
                uart_putc('^');
                uart_putc('Z');
                tty_echo_char('\n');
            }
            return;
        }
    }

    if (tty_is_canonical()) {
        if (c == tty_termios.c_cc[VERASE] || c == TTY_BS) {
            if (tty_edit_len > 0U) {
                tty_edit_len--;
                if (tty_termios.c_lflag & ECHOE)
                    tty_echo_backspace();
            }
            return;
        }

        if (c == tty_termios.c_cc[VKILL]) {
            while (tty_edit_len > 0U) {
                tty_edit_len--;
                if (tty_termios.c_lflag & ECHOE)
                    tty_echo_backspace();
            }
            return;
        }

        if (c == tty_termios.c_cc[VEOF]) {
            if (tty_edit_len > 0U)
                tty_commit_partial_line();
            else
                tty_eof_pending = 1U;
            return;
        }

        if (c == '\n') {
            if (tty_is_echo_enabled())
                tty_echo_char('\n');
            tty_commit_line();
            return;
        }

        if (c == '\t' || (c >= 32U && c < 127U)) {
            if (tty_edit_len < TTY_EDIT_MAX) {
                tty_edit_buf[tty_edit_len++] = c;
                if (tty_is_echo_enabled())
                    tty_echo_char(c);
            }
        }
        return;
    }

    if (c == '\r' && (tty_termios.c_iflag & ICRNL))
        c = '\n';

    if (tty_ready_push(c) && tty_is_echo_enabled()) {
        if (c == TTY_DEL || c == TTY_BS) {
            tty_echo_backspace();
        } else {
            tty_echo_char(c);
        }
    }
}

static void tty_pump_input(void)
{
    int c;
    while ((c = keyboard_getc()) >= 0)
        tty_handle_input_char((uint8_t)c);
}

void tty_poll_input(void)
{
    tty_pump_input();
}

int tty_has_input(void)
{
    tty_pump_input();
    if (tty_eof_pending && tty_ready_empty())
        return 1;
    return tty_ready_empty() ? 0 : 1;
}

void tty_init(void)
{
    tty_edit_len = 0U;
    tty_ready_head = 0U;
    tty_ready_tail = 0U;
    tty_eof_pending = 0U;
    tty_foreground_pgid = 0U;
    tty_session_sid = 0U;
    tty_termios_defaults(&tty_termios);

    uart_puts("[TTY] Line discipline: echo + canonical + ^C/^Z pronto\n");
}

uint64_t tty_read(char *buf, uint64_t cnt)
{
    uint64_t got = 0U;

    if (!buf || cnt == 0U) return ERR(EFAULT);

    for (;;) {
        tty_adopt_current_session();
        if (tty_is_background_current()) {
            tty_signal_current_group(SIGTTIN);
            return ERR(EINTR);
        }

        tty_pump_input();

        if (signal_has_unblocked_pending(current_task))
            return ERR(EINTR);

        if (tty_eof_pending && tty_ready_empty()) {
            tty_eof_pending = 0U;
            return 0U;
        }

        if (!tty_ready_empty())
            break;

        sched_yield();
    }

    while (got < cnt && !tty_ready_empty()) {
        uint8_t c;

        if (!tty_ready_pop(&c))
            break;

        buf[got++] = (char)c;
        if (tty_is_canonical() && c == '\n')
            break;
    }

    return got ? got : ERR(EAGAIN);
}

int tty_check_output_current(void)
{
    if (tty_is_background_current()) {
        tty_signal_current_group(SIGTTOU);
        return -EINTR;
    }
    return 0;
}

int tty_tcsetpgrp_current(uint32_t pgid)
{
    uint32_t sid;

    if (!current_task || !sched_task_is_user(current_task))
        return -EPERM;

    sid = tty_current_sid();
    if (sid == 0U || pgid == 0U)
        return -EINVAL;
    if (tty_session_sid == 0U || !sched_task_has_session(tty_session_sid))
        tty_session_sid = sid;
    if (sid != tty_session_sid)
        return -EPERM;
    if (!sched_task_has_pgrp(sid, pgid))
        return -EPERM;

    tty_foreground_pgid = pgid;
    return 0;
}

uint32_t tty_tcgetpgrp(void)
{
    tty_adopt_current_session();
    return tty_foreground_pgid;
}

int tty_tcgetattr(termios_t *out)
{
    if (!out)
        return -EFAULT;

    *out = tty_termios;
    return 0;
}

int tty_tcsetattr(int action, const termios_t *in)
{
    if (!in)
        return -EFAULT;
    if (action != TCSANOW && action != TCSADRAIN && action != TCSAFLUSH)
        return -EINVAL;

    tty_termios = *in;
    if (tty_termios.c_cc[VERASE] == 0U)
        tty_termios.c_cc[VERASE] = TTY_DEL;
    if (tty_termios.c_cc[VINTR] == 0U)
        tty_termios.c_cc[VINTR] = TTY_CTRL_C;
    if (tty_termios.c_cc[VSUSP] == 0U)
        tty_termios.c_cc[VSUSP] = TTY_CTRL_Z;
    if (tty_termios.c_cc[VMIN] == 0U)
        tty_termios.c_cc[VMIN] = 1U;

    if (action == TCSAFLUSH) {
        tty_ready_head = tty_ready_tail = 0U;
        tty_edit_len = 0U;
        tty_eof_pending = 0U;
    }

    return 0;
}
