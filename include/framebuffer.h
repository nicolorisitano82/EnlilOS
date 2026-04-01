/*
 * EnlilOS Microkernel - Framebuffer Driver
 * Output grafico tramite framebuffer RAMFB (QEMU)
 */

#ifndef ENLILOS_FRAMEBUFFER_H
#define ENLILOS_FRAMEBUFFER_H

#include "types.h"

#define FB_WIDTH   800
#define FB_HEIGHT  600
#define FB_BPP     4    /* 32-bit BGRA */

void fb_init(void);
void fb_clear(uint32_t color);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void fb_draw_string(uint32_t x, uint32_t y, const char *s, uint32_t fg, uint32_t bg);
void fb_draw_string_centered(const char *s, uint32_t fg, uint32_t bg);

/*
 * API UTF-8 (M4-04) — rendering di testo Unicode completo.
 *
 * fb_draw_char_utf8:           renderizza un singolo codepoint Unicode.
 * fb_draw_string_utf8:         renderizza stringa UTF-8 a partire da (x, y).
 * fb_draw_string_centered_utf8: renderizza stringa UTF-8 centrata nello schermo.
 *
 * Font: ASCII O(1), Latin-1 Supplement O(log N), sconosciuto → box U+FFFD.
 */
void fb_draw_char_utf8(uint32_t x, uint32_t y, uint32_t codepoint,
                       uint32_t fg, uint32_t bg);
void fb_draw_string_utf8(uint32_t x, uint32_t y, const char *utf8_str,
                         uint32_t fg, uint32_t bg);
void fb_draw_string_centered_utf8(const char *utf8_str, uint32_t fg, uint32_t bg);

/*
 * fb_flush() — forza il write-back della D-cache per l'intero framebuffer.
 * Chiamare dopo ogni sessione di disegno per rendere visibile il contenuto
 * a QEMU (che legge la RAM fisica, non la cache della CPU).
 * WCET: ~30000 DC CIVAC per 800×600×4 = 1.92MB con cache line 64B.
 */
void fb_flush(void);

/*
 * fb_get_ptr() — ritorna il puntatore al buffer del framebuffer in memoria.
 * Usato da gpu_sw.c per scrivere direttamente i pixel di scanout.
 */
uint32_t *fb_get_ptr(void);

#endif
