/*
 * EnlilOS Microkernel - VirtIO MMIO shared definitions
 *
 * Costanti di registro, bit di stato, feature flags e strutture vring
 * condivisi tra tutti i driver che usano il transport virtio-mmio:
 *   drivers/keyboard.c, drivers/mouse.c, drivers/gpu/gpu_virtio.c
 *
 * Specifica: virtio-v1.2, sezione 4.2 (MMIO transport).
 */

#ifndef ENLILOS_VIRTIO_MMIO_H
#define ENLILOS_VIRTIO_MMIO_H

#include "types.h"

/* ── MMIO base address e layout slot ────────────────────────────────── */

#define VMMIO_BASE           0x0a000000UL   /* prima slot QEMU virt */
#define VMMIO_SLOT_SIZE      0x200UL        /* 512 byte per slot */
#define VMMIO_MAX_SLOTS      32U

/* ── Offset registro MMIO ────────────────────────────────────────────── */

#define VMMIO_MAGIC          0x000U
#define VMMIO_VERSION        0x004U
#define VMMIO_DEVICE_ID      0x008U
#define VMMIO_VENDOR_ID      0x00CU
#define VMMIO_DEV_FEATURES   0x010U
#define VMMIO_DEV_FEAT_SEL   0x014U
#define VMMIO_DRV_FEATURES   0x020U
#define VMMIO_DRV_FEAT_SEL   0x024U
#define VMMIO_QUEUE_SEL      0x030U
#define VMMIO_QUEUE_NUM_MAX  0x034U
#define VMMIO_QUEUE_NUM      0x038U
#define VMMIO_QUEUE_READY    0x044U
#define VMMIO_QUEUE_NOTIFY   0x050U
#define VMMIO_IRQ_STATUS     0x060U
#define VMMIO_IRQ_ACK        0x064U
#define VMMIO_STATUS         0x070U
#define VMMIO_QUEUE_DESC_LO  0x080U
#define VMMIO_QUEUE_DESC_HI  0x084U
#define VMMIO_QUEUE_DRV_LO   0x090U
#define VMMIO_QUEUE_DRV_HI   0x094U
#define VMMIO_QUEUE_DEV_LO   0x0A0U
#define VMMIO_QUEUE_DEV_HI   0x0A4U
#define VMMIO_CONFIG         0x100U

/* ── Magic value e device IDs ────────────────────────────────────────── */

#define VMMIO_MAGIC_VALUE    0x74726976UL   /* "virt" little-endian */

#define VIRTIO_DEVICE_NET    1U
#define VIRTIO_DEVICE_BLOCK  2U
#define VIRTIO_DEVICE_GPU    16U
#define VIRTIO_DEVICE_INPUT  18U

/* ── Device status bits (VMMIO_STATUS) ──────────────────────────────── */

#define VSTAT_ACKNOWLEDGE    1U
#define VSTAT_DRIVER         2U
#define VSTAT_DRIVER_OK      4U
#define VSTAT_FEATURES_OK    8U
#define VSTAT_NEEDS_RESET    64U
#define VSTAT_FAILED         128U

/* ── Feature bits ────────────────────────────────────────────────────── */

#define VIRTIO_F_VERSION_1   (1U << 0)   /* page 1 feature bit */

/* ── Split vring structs (generici, size Q come parametro) ──────────── */

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed)) vring_desc_t;

/* vring_avail e vring_used usano ring[] con dimensione fissa definita
 * dal driver nel suo header locale (VIQ_SIZE / MIQ_SIZE / VQ_SIZE).
 * Dichiararli con dimensione 1 per il puntatore cast — il driver alloca
 * il buffer reale con la dimensione corretta. */

typedef struct {
    uint32_t id;
    uint32_t len;
} __attribute__((packed)) vring_used_elem_t;

/* ── Virtio-input event structure ────────────────────────────────────── */

typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((packed)) virtio_input_event_t;

/* ── Virtio-input config selectors ──────────────────────────────────── */

#define VIRTIO_INPUT_CFG_ID_NAME   0x01U
#define VIRTIO_INPUT_CFG_EV_BITS   0x11U
#define VIRTIO_INPUT_CFG_ABS_INFO  0x12U

/* ── Linux input event types ─────────────────────────────────────────── */

#define EV_SYN   0U
#define EV_KEY   1U
#define EV_REL   2U
#define EV_ABS   3U

#define SYN_REPORT  0U

/* REL axes */
#define REL_X        0U
#define REL_Y        1U
#define REL_WHEEL    8U

/* ABS axes */
#define ABS_X        0U
#define ABS_Y        1U

/* Button codes */
#define BTN_LEFT     272U
#define BTN_RIGHT    273U
#define BTN_MIDDLE   274U

/* Keyboard key codes used by keyboard driver */
#define KEY_ENTER       28U
#define KEY_A           30U
#define KEY_LEFTCTRL    29U
#define KEY_LEFTSHIFT   42U
#define KEY_RIGHTSHIFT  54U
#define KEY_SPACE       57U
#define KEY_RIGHTCTRL   97U

/* Vring descriptor flags */
#define VRING_DESC_F_NEXT   1U   /* descriptor continua nella catena */
#define VRING_DESC_F_WRITE  2U   /* buffer scrivibile dal device */

#endif /* ENLILOS_VIRTIO_MMIO_H */
