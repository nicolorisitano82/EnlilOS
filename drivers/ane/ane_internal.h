/*
 * EnlilOS - ANE Driver Internal Header
 *
 * Condiviso da ane_hw.c, ane_sw.c e kernel/ane_syscall.c.
 * Non includere da header pubblici.
 */

#ifndef ANE_INTERNAL_H
#define ANE_INTERNAL_H

#include "ane.h"
#include "types.h"

/* ── Dimensioni pool statici ────────────────────────────────────────── */

#define ANE_MAX_BUFS    64   /* buffer DMA allocabili contemporaneamente */
#define ANE_MAX_MODELS  16   /* modelli .hwx caricati contemporaneamente */
#define ANE_MAX_JOBS    32   /* job in coda / in esecuzione              */

/* ── Voci dei pool ──────────────────────────────────────────────────── */

typedef struct {
    uint64_t phys;      /* indirizzo fisico (== virtuale, identity map) */
    uint32_t size;      /* dimensione in byte                           */
    uint32_t flags;     /* ANE_BUF_*                                    */
    bool     in_use;
} ane_buf_entry_t;

typedef struct {
    uint32_t buf_handle;    /* handle del buffer che contiene i dati .hwx */
    uint32_t input_size;    /* dal header hwx: dimensione input attesa    */
    uint32_t output_size;   /* dal header hwx: dimensione output attesa   */
    bool     in_use;
} ane_model_entry_t;

typedef struct {
    uint32_t model_handle;
    uint32_t in_buf_handle;
    uint32_t out_buf_handle;
    uint8_t  priority;      /* ereditata dal task chiamante               */
    uint8_t  state;         /* ANE_JOB_*                                  */
    uint16_t error_code;
    uint64_t submit_ns;
    uint64_t done_ns;
    uint64_t cycles;        /* cicli CPU stimati (SW) o ANE reali (HW)    */
    bool     in_use;
} ane_job_entry_t;

/* ── Pool globali (definiti in ane_syscall.c) ───────────────────────── */

extern ane_buf_entry_t   ane_buf_pool  [ANE_MAX_BUFS];
extern ane_model_entry_t ane_model_pool[ANE_MAX_MODELS];
extern ane_job_entry_t   ane_job_pool  [ANE_MAX_JOBS];

/* ── Backend ops ────────────────────────────────────────────────────── */

/*
 * ane_backend_ops_t — vtable del backend hardware o software.
 *
 * query_caps:        popola ane_caps_t con le capacità del dispositivo
 * inference_execute: esegue sincrono (SW) o sottomette (HW) l'inferenza
 *                    per il job puntato; imposta job->state = DONE/ERROR
 */
typedef struct {
    void (*query_caps)       (ane_caps_t *out);
    int  (*inference_execute)(ane_job_entry_t *job);
} ane_backend_ops_t;

/* Backend registrati */
extern const ane_backend_ops_t ane_sw_backend;
extern const ane_backend_ops_t ane_hw_backend;

/* Backend attivo (selezionato da ane_backend_init) */
extern const ane_backend_ops_t *ane_active_backend;

/*
 * ane_backend_init() — rileva se l'ANE hardware è presente
 * (legge MIDR_EL1 per identificare Apple Silicon) e imposta
 * ane_active_backend di conseguenza.
 */
void ane_backend_init(void);

#endif /* ANE_INTERNAL_H */
