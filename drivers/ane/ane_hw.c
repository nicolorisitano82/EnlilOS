/*
 * EnlilOS - ANE Hardware Backend (M3-03)
 *
 * Rilevamento Apple Silicon via MIDR_EL1 e selezione del backend.
 *
 * Apple implementer code in MIDR_EL1[31:24] = 0x61.
 * ARM/QEMU: 0x41 (Cortex-A72 su QEMU virt → SW fallback).
 *
 * Su Apple M-series reale, il driver HW accede all'ANE via:
 *   - MMIO @ 0x26A000000 (M1, da device tree AAPL,arm-performance-monitors)
 *   - Interrupt via AIC (Apple Interrupt Controller, non GIC-400)
 *   - Command buffer DMA nel formato Apple proprietario
 *   - Basato su reverse engineering driver Asahi Linux (apple-ane)
 *
 * In questa versione il backend HW è uno stub: su Apple Silicon reale
 * rileva la presenza dell'hardware e stampa le capacità, ma delega
 * comunque al SW fallback (il driver MMIO completo è M5b-01+).
 * In futuro inference_execute verrà sostituito con la submission
 * reale al command ring ANE.
 */

#include "ane_internal.h"
#include "uart.h"

/* ── Rilevamento Apple Silicon ──────────────────────────────────────── */

/*
 * is_apple_silicon() — legge MIDR_EL1 e controlla l'implementer ID.
 * MIDR_EL1[31:24] = 0x61 → Apple; 0x41 → ARM (QEMU cortex-a72).
 */
static bool is_apple_silicon(void)
{
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    return ((midr >> 24) & 0xFFu) == 0x61u;
}

/*
 * apple_chip_tops_x100() — stima TOPS in base al PartNum MIDR.
 * Valori approssimativi basati su specifiche pubbliche Apple.
 *
 * MIDR_EL1[15:4] = PartNum
 *   0x020/0x021 = M1 Firestorm / Icestorm  → 11.0 TOPS
 *   0x030/0x031 = M2                       → 15.8 TOPS
 *   0x040/0x041 = M3                       → 18.0 TOPS
 *   0x050/0x051 = M4                       → 38.0 TOPS
 */
static uint32_t apple_chip_tops_x100(void)
{
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    uint32_t part = (uint32_t)((midr >> 4) & 0xFFFu);

    if      (part >= 0x050) return 3800;
    else if (part >= 0x040) return 1800;
    else if (part >= 0x030) return 1580;
    else                    return 1100;  /* M1 o precedente */
}

static uint32_t apple_ane_hw_version(void)
{
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    uint32_t part = (uint32_t)((midr >> 4) & 0xFFFu);

    if      (part >= 0x050) return 0x60;
    else if (part >= 0x040) return 0x50;
    else if (part >= 0x030) return 0x40;
    else                    return 0x30;
}

/* ── query_caps (HW) ────────────────────────────────────────────────── */

static void hw_query_caps(ane_caps_t *out)
{
    out->version        = apple_ane_hw_version();
    out->num_cores      = 16;
    out->tops_x100      = apple_chip_tops_x100();
    out->max_model_size = 64u * 1024u * 1024u;   /* 64 MB */
    out->flags          = ANE_CAP_HW | ANE_CAP_REALTIME;
}

/*
 * hw_inference_execute — stub: il driver MMIO completo arriverà con M5b+.
 * Per ora delega al SW backend per non bloccare lo sviluppo.
 */
static int hw_inference_execute(ane_job_entry_t *job)
{
    /* Delega a SW fino all'implementazione MMIO (M5b+) */
    return ane_sw_backend.inference_execute(job);
}

/* ── Esportazione backend ───────────────────────────────────────────── */

const ane_backend_ops_t ane_hw_backend = {
    .query_caps        = hw_query_caps,
    .inference_execute = hw_inference_execute,
};

/* ── Selezione backend ──────────────────────────────────────────────── */

const ane_backend_ops_t *ane_active_backend;

void ane_backend_init(void)
{
    if (is_apple_silicon()) {
        ane_active_backend = &ane_hw_backend;
        uart_puts("[ANE] Hardware: Apple Silicon rilevato");
        uart_puts(" (driver MMIO in M5b+, inference via SW per ora)\n");
    } else {
        ane_active_backend = &ane_sw_backend;
        uart_puts("[ANE] Hardware non rilevato (QEMU) — software fallback attivo\n");
    }
}
