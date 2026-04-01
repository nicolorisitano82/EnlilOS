/*
 * EnlilOS - ANE Software Fallback Backend (M3-03)
 *
 * Emula l'Apple Neural Engine sulla CPU.
 * Attivo quando MIDR_EL1 non identifica Apple Silicon (es. QEMU cortex-a72).
 *
 * Comportamento dell'inferenza SW:
 *   - Se input_size == output_size: copia input → output (identità)
 *   - Altrimenti: azzera il buffer di output
 *   - Registra timing reale con timer_now_ns()
 *
 * Questo produce un'API funzionalmente corretta anche senza hardware ANE.
 * I risultati dell'inferenza non sono significativi (nessun modello ML reale),
 * ma il ciclo submit → wait → leggi output funziona esattamente come su HW.
 */

#include "ane_internal.h"
#include "timer.h"
#include "uart.h"

/* ── query_caps ─────────────────────────────────────────────────────── */

static void sw_query_caps(ane_caps_t *out)
{
    out->version        = 0x00;             /* nessun HW */
    out->num_cores      = 0;
    out->tops_x100      = 0;                /* 0 TOPS reali */
    out->max_model_size = 4u * 1024u * 1024u; /* 4 MB */
    out->flags          = ANE_CAP_SWFALLBACK;
}

/* ── inference_execute ──────────────────────────────────────────────── */

/*
 * Esegue l'inferenza sulla CPU.
 *
 * Per il software fallback:
 *   - Legge dimensioni attese dal modello caricato
 *   - Se input e output hanno la stessa dimensione: copia byte per byte
 *   - Altrimenti: zero-fill del buffer di output
 *   - Stima i cicli come (output_size / 4) × 10 (arbitrario, per profilazione)
 */
static int sw_inference_execute(ane_job_entry_t *job)
{
    uint64_t t0 = timer_now_ns();

    /* Recupera entry buffer di input e output */
    uint32_t iidx = job->in_buf_handle  - 1u;   /* handle è 1-based */
    uint32_t oidx = job->out_buf_handle - 1u;

    ane_buf_entry_t *ibuf = &ane_buf_pool[iidx];
    ane_buf_entry_t *obuf = &ane_buf_pool[oidx];

    /* Recupera dimensioni attese dal modello */
    uint32_t midx = job->model_handle - 1u;
    ane_model_entry_t *model = &ane_model_pool[midx];

    uint32_t in_size  = (ibuf->size  < model->input_size)  ? ibuf->size  : model->input_size;
    uint32_t out_size = (obuf->size < model->output_size) ? obuf->size : model->output_size;

    const uint8_t *src = (const uint8_t *)(uintptr_t)ibuf->phys;
    uint8_t       *dst = (uint8_t       *)(uintptr_t)obuf->phys;

    if (in_size == out_size) {
        /* Identità: copia input → output */
        for (uint32_t i = 0; i < out_size; i++)
            dst[i] = src[i];
    } else {
        /* Dimensioni diverse: zero-fill */
        for (uint32_t i = 0; i < out_size; i++)
            dst[i] = 0;
    }

    uint64_t t1  = timer_now_ns();
    job->done_ns = t1;
    job->cycles  = (uint64_t)(out_size / 4u) * 10u; /* stima SW */
    job->state   = ANE_JOB_DONE;

    (void)t0;
    return 0;
}

/* ── Esportazione backend ───────────────────────────────────────────── */

const ane_backend_ops_t ane_sw_backend = {
    .query_caps        = sw_query_caps,
    .inference_execute = sw_inference_execute,
};
