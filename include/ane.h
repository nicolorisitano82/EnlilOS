/*
 * EnlilOS Microkernel - Apple Neural Engine Interface (M3-03)
 *
 * Syscall range 100–109 per accesso all'ANE (Apple Neural Engine).
 * Su QEMU (nessun ANE HW): software fallback automatico via MIDR_EL1.
 * Su Apple M-series: driver HW via MMIO + AIC IRQ.
 *
 * ABI: numero syscall in x8, argomenti x0–x5, ritorno x0.
 *
 * Formato modello HWX (EnlilOS):
 *   Offset 0   magic    = ANE_HWX_MAGIC (0x454e4c58 "ENLX")
 *   Offset 4   version  = 1
 *   Offset 8   input_size  (byte)
 *   Offset 12  output_size (byte)
 *   Offset 16  num_tiles
 *   Offset 20  flags
 *   Offset 24  dati modello
 */

#ifndef ENLILOS_ANE_H
#define ENLILOS_ANE_H

#include "types.h"

/* ── Numeri syscall ─────────────────────────────────────────────────── */

#define SYS_ANE_QUERY_CAPS       100
#define SYS_ANE_BUF_ALLOC        101
#define SYS_ANE_BUF_FREE         102
#define SYS_ANE_MODEL_LOAD       103
#define SYS_ANE_MODEL_UNLOAD     104
#define SYS_ANE_INFERENCE_SUBMIT 105
#define SYS_ANE_INFERENCE_WAIT   106
#define SYS_ANE_INFERENCE_RUN    107
#define SYS_ANE_JOB_CANCEL       108
#define SYS_ANE_JOB_STATUS       109

/* ── Capacità ANE ───────────────────────────────────────────────────── */

#define ANE_CAP_SWFALLBACK  (1u << 0)   /* emulazione CPU attiva */
#define ANE_CAP_REALTIME    (1u << 1)   /* supporta scheduling RT */
#define ANE_CAP_HW          (1u << 2)   /* hardware ANE presente */

typedef struct {
    uint32_t version;        /* versione HW: 0x30=M1, 0x40=M2, 0x50=M3, 0x60=M4 */
    uint32_t num_cores;      /* core ANE (16 su tutti gli M-series) */
    uint32_t tops_x100;      /* TOPS × 100 (1100=11.0, 1580=15.8, 1800=18.0, 3800=38.0) */
    uint32_t max_model_size; /* dimensione massima .hwx accettata (byte) */
    uint32_t flags;          /* ANE_CAP_* */
} ane_caps_t;

/* ── Flag buffer ────────────────────────────────────────────────────── */

#define ANE_BUF_INPUT   (1u << 0)   /* input per l'ANE, scritto dalla CPU */
#define ANE_BUF_OUTPUT  (1u << 1)   /* output dall'ANE, letto dalla CPU   */
#define ANE_BUF_PINNED  (1u << 2)   /* non evictabile — obbligatorio RT   */

/* ── Handle opachi ──────────────────────────────────────────────────── */

typedef uint32_t ane_buf_handle_t;
typedef uint32_t ane_model_handle_t;
typedef uint32_t ane_job_handle_t;

#define ANE_INVALID_HANDLE  ((uint32_t)~0u)

/* ── Stato dei job ──────────────────────────────────────────────────── */

#define ANE_JOB_FREE      0
#define ANE_JOB_PENDING   1
#define ANE_JOB_RUNNING   2
#define ANE_JOB_DONE      3
#define ANE_JOB_ERROR     4
#define ANE_JOB_CANCELLED 5

typedef struct {
    uint32_t state;        /* ANE_JOB_* */
    uint32_t error_code;   /* 0 = OK */
    uint64_t submit_ns;    /* timestamp submit (CNTPCT_EL0) */
    uint64_t done_ns;      /* timestamp completamento */
    uint64_t cycles;       /* cicli stimati (profilazione SW) */
} ane_job_stat_t;

/* ── Header modello HWX (EnlilOS) ───────────────────────────────────── */

#define ANE_HWX_MAGIC    0x454e4c58u   /* "ENLX" little-endian */
#define ANE_HWX_VERSION  1u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t input_size;    /* dimensione attesa input (byte) */
    uint32_t output_size;   /* dimensione attesa output (byte) */
    uint32_t num_tiles;     /* tile HWCX (0 = flat) */
    uint32_t flags;
} ane_hwx_header_t;

/* ── API pubblica ────────────────────────────────────────────────────── */

/*
 * ane_init() — rileva HW/SW backend, registra syscall 100-109.
 * Chiamare da main() dopo syscall_init().
 */
void ane_init(void);

#endif /* ENLILOS_ANE_H */
