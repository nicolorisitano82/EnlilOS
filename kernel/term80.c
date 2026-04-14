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
static uint8_t  term80_esc_state;
static uint8_t  term80_csi_len;
static char     term80_csi_buf[16];

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

static void term80_parser_reset(void)
{
    term80_esc_state = 0U;
    term80_csi_len   = 0U;
}

static uint32_t term80_parse_param(uint32_t index, uint32_t default_value)
{
    uint32_t current = 0U;
    uint32_t seen    = 0U;
    uint32_t which   = 0U;

    for (uint32_t i = 0U; i < term80_csi_len; i++) {
        char ch = term80_csi_buf[i];

        if (ch >= '0' && ch <= '9') {
            current = current * 10U + (uint32_t)(ch - '0');
            seen = 1U;
        } else if (ch == ';') {
            if (which == index)
                return seen ? current : default_value;
            which++;
            current = 0U;
            seen = 0U;
        }
    }

    if (which == index)
        return seen ? current : default_value;
    return default_value;
}

static void term80_clear_to_eol(void)
{
    for (uint32_t col = term80_col; col < TERM80_COLS; col++)
        term80_cells[term80_row][col] = ' ';
    term80_dirty = 1U;
}

static void term80_clear_from_sol(void)
{
    for (uint32_t col = 0U; col <= term80_col && col < TERM80_COLS; col++)
        term80_cells[term80_row][col] = ' ';
    term80_dirty = 1U;
}

static void term80_clear_screen_from_cursor(void)
{
    term80_clear_to_eol();
    for (uint32_t row = term80_row + 1U; row < TERM80_ROWS; row++)
        term80_fill_spaces(term80_cells[row]);
    term80_dirty = 1U;
}

static void term80_clear_screen_to_cursor(void)
{
    for (uint32_t row = 0U; row < term80_row; row++)
        term80_fill_spaces(term80_cells[row]);
    term80_clear_from_sol();
    term80_dirty = 1U;
}

static void term80_handle_csi(char final_ch)
{
    uint32_t n = term80_parse_param(0U, 1U);

    switch (final_ch) {
    case 'A':
        if (n > term80_row)
            term80_row = 0U;
        else
            term80_row -= n;
        term80_dirty = 1U;
        break;
    case 'B':
        term80_row += n;
        if (term80_row >= TERM80_ROWS)
            term80_row = TERM80_ROWS - 1U;
        term80_dirty = 1U;
        break;
    case 'C':
        term80_col += n;
        if (term80_col >= TERM80_COLS)
            term80_col = TERM80_COLS - 1U;
        term80_dirty = 1U;
        break;
    case 'D':
        if (n > term80_col)
            term80_col = 0U;
        else
            term80_col -= n;
        term80_dirty = 1U;
        break;
    case 'G':
        if (n == 0U)
            n = 1U;
        term80_col = (n - 1U < TERM80_COLS) ? (n - 1U) : (TERM80_COLS - 1U);
        term80_dirty = 1U;
        break;
    case 'H':
    case 'f': {
        uint32_t row = term80_parse_param(0U, 1U);
        uint32_t col = term80_parse_param(1U, 1U);

        if (row == 0U) row = 1U;
        if (col == 0U) col = 1U;
        term80_row = (row - 1U < TERM80_ROWS) ? (row - 1U) : (TERM80_ROWS - 1U);
        term80_col = (col - 1U < TERM80_COLS) ? (col - 1U) : (TERM80_COLS - 1U);
        term80_dirty = 1U;
        break;
    }
    case 'J':
        switch (term80_parse_param(0U, 0U)) {
        case 0U:
            term80_clear_screen_from_cursor();
            break;
        case 1U:
            term80_clear_screen_to_cursor();
            break;
        default:
            term80_clear();
            break;
        }
        break;
    case 'K':
        switch (term80_parse_param(0U, 0U)) {
        case 0U:
            term80_clear_to_eol();
            break;
        case 1U:
            term80_clear_from_sol();
            break;
        default:
            term80_fill_spaces(term80_cells[term80_row]);
            term80_dirty = 1U;
            break;
        }
        break;
    case 'm':
        /* SGR/color: ignorato per il terminale 80x25 monocromatico. */
        break;
    default:
        break;
    }
}

static void term80_put_visible(char c)
{
    if ((unsigned char)c < 32U || (unsigned char)c > 126U)
        c = '?';

    term80_cells[term80_row][term80_col] = c;
    if (term80_col + 1U >= TERM80_COLS)
        term80_newline();
    else
        term80_col++;

    term80_dirty = 1U;
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
    term80_parser_reset();
}

void term80_activate(uint32_t owner_pid, const char *title)
{
    term80_owner = owner_pid;
    term80_row = 0U;
    term80_col = 0U;
    term80_active = 1U;
    term80_dirty = 1U;
    term80_title_copy(title ? title : "nsh");
    term80_parser_reset();

    for (uint32_t row = 0U; row < TERM80_ROWS; row++)
        term80_fill_spaces(term80_cells[row]);
}

void term80_deactivate(void)
{
    term80_active = 0U;
    term80_owner = 0U;
    term80_dirty = 1U;
    term80_parser_reset();
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
    term80_parser_reset();
}

void term80_putc(char c)
{
    if (!term80_active)
        return;

    if (term80_esc_state == 1U) {
        if (c == '[') {
            term80_esc_state = 2U;
            term80_csi_len = 0U;
            return;
        }
        term80_parser_reset();
        return;
    }

    if (term80_esc_state == 2U) {
        unsigned char uc = (unsigned char)c;

        if ((uc >= '0' && uc <= '9') || c == ';' || c == '?') {
            if (term80_csi_len + 1U < (uint32_t)sizeof(term80_csi_buf))
                term80_csi_buf[term80_csi_len++] = c;
            return;
        }

        if (uc >= 0x40U && uc <= 0x7EU) {
            term80_handle_csi(c);
            term80_parser_reset();
            return;
        }

        term80_parser_reset();
        return;
    }

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
        term80_esc_state = 1U;
        return;
    default:
        break;
    }

    term80_put_visible(c);
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
