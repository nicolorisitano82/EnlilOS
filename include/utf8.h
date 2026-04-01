/*
 * EnlilOS Microkernel - UTF-8 Decoder (M4-04)
 *
 * Decodifica sequenze UTF-8 in codepoint Unicode (uint32_t).
 *
 * RT compliance:
 *   utf8_decode(): WCET O(1) — al massimo 4 byte per codepoint.
 *   utf8_strlen(): WCET O(n) — n = lunghezza stringa in byte.
 *   Nessuna allocazione, nessun I/O.
 *
 * Sequenze malformate → U+FFFD (Unicode Replacement Character).
 */

#ifndef ENLILOS_UTF8_H
#define ENLILOS_UTF8_H

#include "types.h"

/* Codepoint sostituto per sequenze malformate */
#define UTF8_REPLACEMENT    0xFFFDU

/*
 * utf8_decode(s) — legge un codepoint Unicode da *s (stringa UTF-8).
 *
 * Avanza *s di esattamente N byte (N = lunghezza sequenza UTF-8 valida,
 * oppure 1 byte se la sequenza è malformata).
 * Ritorna il codepoint decodificato, o UTF8_REPLACEMENT su errore.
 *
 * WCET: costante — al massimo 4 byte letti, nessun ciclo.
 */
uint32_t utf8_decode(const char **s);

/*
 * utf8_strlen(s) — conta i codepoint in una stringa UTF-8.
 * Ritorna il numero di caratteri (non byte).
 * WCET: O(n) dove n = strlen in byte.
 */
uint32_t utf8_strlen(const char *s);

#endif /* ENLILOS_UTF8_H */
