/*
 * EnlilOS Microkernel - VirtIO Block Device API (M5-01)
 *
 * Driver kernel per virtio-blk su virtio-mmio (QEMU virt machine).
 *
 * RT design:
 *   - Task hard-RT non chiamano mai blk_read/write direttamente.
 *     Tutte le operazioni I/O passano da un server blk dedicato
 *     a priorità bassa; i task RT comunicano col server via IPC
 *     con timeout (graceful degradation se disco non risponde).
 *   - `blk_request_sync()` è sincrono con busy-wait bounded
 *     (WCET = BLK_POLL_TIMEOUT cicli) — usato SOLO da server blk,
 *     mai da task RT.
 *   - Buffer descriptors e vring pre-allocati staticamente al boot.
 *   - Nessuna allocazione dinamica nel path I/O.
 *
 * QEMU cmdline:
 *   -drive format=raw,file=disk.img,if=none,id=blk0
 *   -device virtio-blk-device,drive=blk0
 */

#ifndef ENLILOS_BLK_H
#define ENLILOS_BLK_H

#include "types.h"

/* Dimensione settore virtio-blk (sempre 512 byte per specifica) */
#define BLK_SECTOR_SIZE     512U

/* Numero massimo di richieste I/O in volo contemporaneamente */
#define BLK_QUEUE_DEPTH     16U

/* Timeout busy-wait per completamento richiesta (cicli CPU) */
#define BLK_POLL_TIMEOUT    5000000U   /* ~80ms a 62.5 MHz */

/* Codici di stato richiesta */
#define BLK_OK              0
#define BLK_ERR_NOT_READY  -1   /* device non inizializzato */
#define BLK_ERR_IO         -2   /* errore I/O segnalato dal device */
#define BLK_ERR_TIMEOUT    -3   /* timeout polling */
#define BLK_ERR_RANGE      -4   /* settore fuori range */
#define BLK_ERR_BUSY       -5   /* nessun descriptor libero */

/*
 * blk_init() — rilevamento e inizializzazione virtio-blk.
 * Chiamata una volta al boot da kernel_main().
 * Ritorna 1 se il device è stato trovato e inizializzato, 0 altrimenti.
 * Su QEMU senza -device virtio-blk-device ritorna 0 silenziosamente.
 */
int blk_init(void);

/*
 * blk_is_ready() — ritorna 1 se il device è pronto per I/O, 0 altrimenti.
 */
int blk_is_ready(void);

/*
 * blk_sector_count() — numero totale di settori da 512 byte del disco.
 * Ritorna 0 se il device non è pronto.
 */
uint64_t blk_sector_count(void);

/*
 * blk_read_sync(sector, buf, count) — legge `count` settori da `sector`
 * nel buffer `buf` (deve essere count × 512 byte).
 *
 * Blocca con busy-wait per al massimo BLK_POLL_TIMEOUT cicli.
 * NON chiamare da task hard-RT.
 *
 * Ritorna BLK_OK (0) o un codice BLK_ERR_*.
 */
int blk_read_sync(uint64_t sector, void *buf, uint32_t count);

/*
 * blk_write_sync(sector, buf, count) — scrive `count` settori da `buf`
 * a partire da `sector`.
 *
 * Blocca con busy-wait per al massimo BLK_POLL_TIMEOUT cicli.
 * NON chiamare da task hard-RT.
 *
 * Ritorna BLK_OK (0) o un codice BLK_ERR_*.
 */
int blk_write_sync(uint64_t sector, const void *buf, uint32_t count);
int blk_flush_sync(void);

#endif /* ENLILOS_BLK_H */
