/*
 * EnlilOS Microkernel - 80x25 text console for framebuffer/GPU shell mode
 */

#ifndef ENLILOS_TERM80_H
#define ENLILOS_TERM80_H

#include "types.h"

#define TERM80_COLS 80U
#define TERM80_ROWS 25U

void      term80_init(void);
void      term80_activate(uint32_t owner_pid, const char *title);
void      term80_deactivate(void);
int       term80_is_active(void);
uint32_t  term80_owner_pid(void);
const char *term80_title(void);
void      term80_clear(void);
void      term80_putc(char c);
void      term80_write(const char *buf, uint32_t len);
int       term80_take_dirty(void);
void      term80_copy_row(uint32_t row, char *dst, uint32_t cap);
void      term80_get_cursor(uint32_t *row, uint32_t *col);

#endif /* ENLILOS_TERM80_H */
