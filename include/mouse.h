/*
 * EnlilOS Microkernel - Mouse Input API (M4-05)
 *
 * Backend attuale:
 *   - VirtIO Input pointer su QEMU virt (`virtio-mouse-device`)
 *
 * RT design:
 *   mouse_get_event() e' O(1), non blocca e non alloca.
 */

#ifndef ENLILOS_MOUSE_H
#define ENLILOS_MOUSE_H

#include "types.h"

#define MOUSE_BTN_LEFT    (1U << 0)
#define MOUSE_BTN_RIGHT   (1U << 1)
#define MOUSE_BTN_MIDDLE  (1U << 2)

#define MOUSE_EVT_MOVE    (1U << 0)
#define MOUSE_EVT_ABS     (1U << 1)
#define MOUSE_EVT_WHEEL   (1U << 2)
#define MOUSE_EVT_BUTTON  (1U << 3)

typedef struct {
    int32_t  dx;
    int32_t  dy;
    int32_t  wheel;
    uint32_t x;
    uint32_t y;
    uint32_t buttons;
    uint32_t flags;
} mouse_event_t;

/* Inizializza il backend mouse. */
void mouse_init(void);

/* Ritorna 1 se il backend mouse e' pronto, 0 altrimenti. */
int mouse_is_ready(void);

/* Estrae il prossimo evento mouse, 1=ok 0=vuoto. */
int mouse_get_event(mouse_event_t *out);

#endif /* ENLILOS_MOUSE_H */
