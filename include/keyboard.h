/*
 * EnlilOS Microkernel - Console Input API
 *
 * Backend attuali:
 *   - VirtIO Input keyboard su QEMU virt (target grafici)
 *   - UART PL011 come fallback su stdio QEMU
 *
 * L'API legacy resta disponibile via keyboard_getc(), ma M8-08g aggiunge
 * anche un layer layout-aware con eventi keycode/modifiers/keysym/unicode.
 *
 * RT design:
 *   - keyboard_getc() è O(1), non blocca e non alloca
 *   - keyboard_get_event() è O(1), non blocca e non alloca
 *   - la traduzione layout usa tabelle statiche O(1)
 */

#ifndef ENLILOS_KEYBOARD_H
#define ENLILOS_KEYBOARD_H

#include "types.h"

typedef enum {
    KBD_LAYOUT_US = 0,
    KBD_LAYOUT_IT = 1,
} keyboard_layout_t;

enum {
    KBD_MOD_SHIFT = 1U << 0,
    KBD_MOD_CTRL  = 1U << 1,
    KBD_MOD_ALT   = 1U << 2,
    KBD_MOD_ALTGR = 1U << 3,
};

typedef struct {
    uint16_t keycode;
    uint16_t modifiers;
    uint32_t keysym;
    uint32_t unicode;
    uint8_t  pressed;
    uint8_t  repeat;
    uint8_t  reserved0;
    uint8_t  reserved1;
} keyboard_event_t;

/* Inizializza il backend di input console. */
void keyboard_init(void);

/* Ritorna il prossimo byte disponibile su stdin, oppure -1 se vuoto. */
int keyboard_getc(void);

/* Espone il prossimo evento tastiera layout-aware, oppure 0 se vuoto. */
int keyboard_get_event(keyboard_event_t *out);

/* Layout tastiera attivo (v1: console globale corrente). */
int               keyboard_set_layout(keyboard_layout_t layout);
int               keyboard_set_layout_name(const char *name);
keyboard_layout_t keyboard_get_layout(void);
const char       *keyboard_get_layout_name(void);

/* Self-test kernel-side per mapping/layout e path UTF-8. */
int keyboard_selftest_run(void);

#endif /* ENLILOS_KEYBOARD_H */
