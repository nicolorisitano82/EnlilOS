/*
 * EnlilOS Microkernel - 80x25 text console
 *
 * Usato da M7-02 per visualizzare la shell EL0 sul framebuffer/GPU
 * senza togliere il fallback seriale su UART.
 */

#include "term80.h"

static char     term80_cells[TERM80_ROWS][TERM80_COLS + 1U];
static char     term80_title_buf[32];
static uint32_t term80_owner;
static uint32_t term80_row;
static uint32_t term80_col;
static uint8_t  term80_active;
static uint8_t  term80_dirty;

static void term80_fill_spaces(char *row)
{
    for (uint32_t i = 0U; i < TERM80_COLS; i++)
        row[i] = ' ';
    row[TERM80_COLS] = '\0';
}

static void term80_scroll(void)
{
    for (uint32_t row = 1U; row < TERM80_ROWS; row++) {
        for (uint32_t col = 0U; col < TERM80_COLS + 1U; col++)
            term80_cells[row - 1U][col] = term80_cells[row][col];
    }
    term80_fill_spaces(term80_cells[TERM80_ROWS - 1U]);
    term80_row = TERM80_ROWS - 1U;
}

static void term80_newline(void)
{
    term80_col = 0U;
    if (term80_row + 1U >= TERM80_ROWS)
        term80_scroll();
    else
        term80_row++;
}

static void term80_title_copy(const char *title)
{
    uint32_t i = 0U;

    while (title && title[i] != '\0' && i + 1U < (uint32_t)sizeof(term80_title_buf)) {
        term80_title_buf[i] = title[i];
        i++;
    }
    term80_title_buf[i] = '\0';
}

void term80_init(void)
{
    for (uint32_t row = 0U; row < TERM80_ROWS; row++)
        term80_fill_spaces(term80_cells[row]);

    term80_owner = 0U;
    term80_row = 0U;
    term80_col = 0U;
    term80_active = 0U;
    term80_dirty = 1U;
    term80_title_copy("nsh");
}

void term80_activate(uint32_t owner_pid, const char *title)
{
    term80_owner = owner_pid;
    term80_row = 0U;
    term80_col = 0U;
    term80_active = 1U;
    term80_dirty = 1U;
    term80_title_copy(title ? title : "nsh");

    for (uint32_t row = 0U; row < TERM80_ROWS; row++)
        term80_fill_spaces(term80_cells[row]);
}

void term80_deactivate(void)
{
    term80_active = 0U;
    term80_owner = 0U;
    term80_dirty = 1U;
}

int term80_is_active(void)
{
    return term80_active ? 1 : 0;
}

uint32_t term80_owner_pid(void)
{
    return term80_owner;
}

const char *term80_title(void)
{
    return term80_title_buf;
}

void term80_clear(void)
{
    for (uint32_t row = 0U; row < TERM80_ROWS; row++)
        term80_fill_spaces(term80_cells[row]);

    term80_row = 0U;
    term80_col = 0U;
    term80_dirty = 1U;
}

void term80_putc(char c)
{
    if (!term80_active)
        return;

    switch ((unsigned char)c) {
    case '\f':
        term80_clear();
        return;
    case '\r':
        term80_col = 0U;
        term80_dirty = 1U;
        return;
    case '\n':
        term80_newline();
        term80_dirty = 1U;
        return;
    case '\b':
    case 0x7F:
        if (term80_col > 0U) {
            term80_col--;
            term80_cells[term80_row][term80_col] = ' ';
        }
        term80_dirty = 1U;
        return;
    case '\t':
        do {
            term80_putc(' ');
        } while ((term80_col & 3U) != 0U);
        return;
    case 0x1B:
        /* Le escape ANSI vengono ignorate: clear usa anche '\f'. */
        return;
    default:
        break;
    }

    if ((unsigned char)c < 32U || (unsigned char)c > 126U)
        c = '?';

    term80_cells[term80_row][term80_col] = c;
    if (term80_col + 1U >= TERM80_COLS)
        term80_newline();
    else
        term80_col++;

    term80_dirty = 1U;
}

void term80_write(const char *buf, uint32_t len)
{
    if (!buf || len == 0U)
        return;

    for (uint32_t i = 0U; i < len; i++)
        term80_putc(buf[i]);
}

int term80_take_dirty(void)
{
    int dirty = term80_dirty ? 1 : 0;
    term80_dirty = 0U;
    return dirty;
}

void term80_copy_row(uint32_t row, char *dst, uint32_t cap)
{
    uint32_t end = TERM80_COLS;

    if (!dst || cap == 0U)
        return;

    dst[0] = '\0';
    if (row >= TERM80_ROWS)
        return;

    while (end > 0U && term80_cells[row][end - 1U] == ' ')
        end--;

    if (end + 1U > cap)
        end = cap - 1U;

    for (uint32_t i = 0U; i < end; i++)
        dst[i] = term80_cells[row][i];
    dst[end] = '\0';
}

void term80_get_cursor(uint32_t *row, uint32_t *col)
{
    if (row)
        *row = term80_row;
    if (col)
        *col = term80_col;
}
