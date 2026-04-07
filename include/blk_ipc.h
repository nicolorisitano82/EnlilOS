/*
 * EnlilOS - Block Server IPC protocol (M9-03)
 *
 * Protocollo minimo tra client kernel-side e server user-space blkd.
 * Pattern identico a vfs_ipc.h (M9-02).
 *
 * Il backend virtio-blk resta in-kernel per M9-03 v1 (syscall blk_boot_*).
 * blkd serve come server di facciata: riceve richieste IPC e le traduce
 * in chiamate al driver kernel tramite le syscall dedicate.
 */

#ifndef ENLILOS_BLK_IPC_H
#define ENLILOS_BLK_IPC_H

#include "types.h"

/* Numero massimo di settori per singola richiesta IPC (512B × 8 = 4KB) */
#define BLKD_MAX_SECTORS    8U
#define BLKD_SECTOR_SIZE    512U
#define BLKD_IO_BYTES       (BLKD_MAX_SECTORS * BLKD_SECTOR_SIZE)  /* 4096 */

typedef enum {
    BLKD_REQ_READ    = 1,
    BLKD_REQ_WRITE   = 2,
    BLKD_REQ_FLUSH   = 3,
    BLKD_REQ_SECTORS = 4,   /* query: ritorna capacity in settori */
} blkd_req_op_t;

/*
 * blkd_request_t / blkd_response_t devono stare nell'IPC payload (max 256 B).
 * I dati I/O non transitano nell'IPC: blkd usa un buffer statico interno e lo
 * passa direttamente come puntatore alle syscall blk_boot_read/write.
 */
typedef struct {
    uint32_t op;
    uint32_t count;     /* numero di settori */
    uint64_t sector;    /* settore di partenza */
} blkd_request_t;       /* 16 byte — entra nel payload IPC */

typedef struct {
    int32_t  status;    /* 0=OK, negativo=errore */
    uint32_t _pad;
    uint64_t value;     /* per SECTORS: capacity; per READ: byte letti */
} blkd_response_t;      /* 16 byte — entra nel payload IPC */

#endif /* ENLILOS_BLK_IPC_H */
