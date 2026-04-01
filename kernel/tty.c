/*
 * EnlilOS Microkernel - Terminal Line Discipline (M4-03)
 *
 * Implementazione minimale stile TTY:
 *   - modalita' canonica: la read ritorna solo linee terminate da '\n'
 *   - echo su UART del testo digitato
 *   - backspace locale con editing della linea corrente
 *   - CTRL+C: invalida la linea corrente e segnala un "SIGINT" minimale
 *             al task foreground; la successiva read ritorna -EINTR
 *
 * Nota: non esiste ancora un sottosistema segnali completo. Qui SIGINT e'
 * modellato come pending interrupt per task di foreground, sufficiente per
 * interrompere la read sulla console e sbloccare shell/programmi cooperativi.
 */

#include "tty.h"

#include "keyboard.h"
#include "sched.h"
#include "syscall.h"
#include "term80.h"
#include "uart.h"

#define TTY_EDIT_MAX    256U
#define TTY_READY_MAX   512U

#define TTY_CTRL_C      0x03
#define TTY_BS          0x08
#define TTY_DEL         0x7F

static uint8_t tty_edit_buf[TTY_EDIT_MAX];
static uint16_t tty_edit_len;

static uint8_t tty_ready_buf[TTY_READY_MAX];
static uint16_t tty_ready_head;
static uint16_t tty_ready_tail;

static uint8_t tty_sigint_pending[SCHED_MAX_TASKS];
static uint32_t tty_foreground_pid;

static inline uint32_t tty_task_slot(uint32_t pid)
{
    return pid % SCHED_MAX_TASKS;
}

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

static void tty_signal_foreground_sigint(void)
{
    tty_sigint_pending[tty_task_slot(tty_foreground_pid)] = 1U;
}

static int tty_take_sigint_current(void)
{
    uint32_t pid = current_task ? current_task->pid : tty_foreground_pid;
    uint32_t slot = tty_task_slot(pid);

    if (!tty_sigint_pending[slot]) return 0;

    tty_sigint_pending[slot] = 0U;
    return 1;
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

static void tty_handle_input_char(uint8_t c)
{
    if (c == '\r')
        c = '\n';

    if (c == TTY_CTRL_C) {
        tty_edit_len = 0U;
        tty_signal_foreground_sigint();
        uart_putc('^');
        uart_putc('C');
        tty_echo_char('\n');
        return;
    }

    if (c == TTY_BS || c == TTY_DEL) {
        if (tty_edit_len > 0U) {
            tty_edit_len--;
            tty_echo_backspace();
        }
        return;
    }

    if (c == '\n') {
        tty_echo_char('\n');
        tty_commit_line();
        return;
    }

    if (c == '\t' || (c >= 32U && c < 127U)) {
        if (tty_edit_len < TTY_EDIT_MAX) {
            tty_edit_buf[tty_edit_len++] = c;
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

void tty_init(void)
{
    tty_edit_len = 0U;
    tty_ready_head = 0U;
    tty_ready_tail = 0U;
    tty_foreground_pid = current_task ? current_task->pid : 0U;

    for (uint32_t i = 0; i < SCHED_MAX_TASKS; i++)
        tty_sigint_pending[i] = 0U;

    uart_puts("[TTY] Line discipline: echo + canonical + ^C pronto\n");
}

uint64_t tty_read(char *buf, uint64_t cnt)
{
    if (!buf || cnt == 0U) return ERR(EFAULT);

    tty_foreground_pid = current_task ? current_task->pid : tty_foreground_pid;
    tty_pump_input();

    if (tty_take_sigint_current())
        return ERR(EINTR);

    if (tty_ready_empty())
        return ERR(EAGAIN);

    uint64_t got = 0U;
    while (got < cnt && !tty_ready_empty()) {
        uint8_t c;
        if (!tty_ready_pop(&c))
            break;

        buf[got++] = (char)c;
        if (c == '\n')
            break;
    }

    return got ? got : ERR(EAGAIN);
}
