/*
 * EnlilOS - Apple AGX GPU Backend (M3-04)
 *
 * Rilevamento Apple Silicon + selezione del backend GPU.
 *
 * Su Apple M-series reale (MIDR implementer = 0x61):
 *   - Riconosce la generazione dal PartNum MIDR
 *   - Reporta capacità reali (VRAM, compute units, device ID)
 *   - execute_cmdbuf e present delegano al SW per ora
 *     (driver MMIO AGX completo in M5b-01)
 *
 * Apple AGX MMIO su M1 (da device tree):
 *   GPU   @ 0x206400000  (MMIO registers)
 *   UATTT @ 0x200000000  (UAT page tables — IOMMU GPU)
 *   DCP   @ 0x228000000  (Display Coprocessor)
 * Interrupt via AIC (non GIC-400) — IRQ #1086 (M1).
 * Basato su Asahi Linux driver (drivers/gpu/drm/asahi/).
 *
 * virtio-GPU su QEMU: non rilevato qui — gestito da gpu_virtio.c.
 */

#include "gpu_internal.h"
#include "uart.h"

/* ── Rilevamento Apple Silicon ──────────────────────────────────────── */

static bool is_apple_silicon(void)
{
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    return ((midr >> 24) & 0xFFu) == 0x61u;
}

/* ── Capacità per generazione (da PartNum MIDR) ─────────────────────── */

typedef struct {
    uint32_t device_id;
    uint64_t vram_bytes;
    uint32_t compute_units;
} agx_gen_info_t;

static agx_gen_info_t agx_gen_info(void)
{
    uint64_t midr;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    uint32_t part = (uint32_t)((midr >> 4) & 0xFFFu);

    if (part >= 0x050) {
        /* M4 */
        return (agx_gen_info_t){ 0x6030, 24ULL * 1024 * 1024 * 1024, 38 };
    } else if (part >= 0x040) {
        /* M3 */
        return (agx_gen_info_t){ 0x6020, 18ULL * 1024 * 1024 * 1024, 18 };
    } else if (part >= 0x030) {
        /* M2 */
        return (agx_gen_info_t){ 0x6010, 16ULL * 1024 * 1024 * 1024, 19 };
    } else {
        /* M1 */
        return (agx_gen_info_t){ 0x6000, 8ULL * 1024 * 1024 * 1024,  8 };
    }
}

/* ── query_caps (AGX HW) ────────────────────────────────────────────── */

static void agx_query_caps(gpu_caps_t *out)
{
    agx_gen_info_t info = agx_gen_info();
    out->vendor          = GPU_VENDOR_APPLE_AGX;
    out->device_id       = info.device_id;
    out->vram_bytes      = info.vram_bytes;
    out->max_texture_dim = 16384;
    out->compute_units   = info.compute_units;
    out->flags           = GPU_CAP_HW | GPU_CAP_COMPUTE | GPU_CAP_SCANOUT;
}

static void agx_query_scanout(gpu_scanout_info_t *out)
{
    if (!out) return;
    out->width         = 800U;
    out->height        = 600U;
    out->refresh_hz    = 60U;
    out->flags         = GPU_SCANOUT_ACTIVE | GPU_SCANOUT_VSYNC;
    out->front_index   = 0U;
    out->back_index    = 0U;
    out->frame_counter = 0U;
}

/* ── Stub: delega a SW (driver MMIO in M5b-01) ──────────────────────── */

static int agx_execute_cmdbuf(gpu_cmdbuf_entry_t *cb, gpu_fence_entry_t *fence)
{
    return gpu_sw_backend.execute_cmdbuf(cb, fence);
}

static int agx_present(gpu_buf_entry_t *scanout, uint32_t x, uint32_t y,
                       uint32_t w, uint32_t h, gpu_fence_entry_t *fence)
{
    return gpu_sw_backend.present(scanout, x, y, w, h, fence);
}

/* ── Esportazione backend ───────────────────────────────────────────── */

const gpu_backend_ops_t gpu_agx_backend = {
    .query_caps     = agx_query_caps,
    .query_scanout  = agx_query_scanout,
    .execute_cmdbuf = agx_execute_cmdbuf,
    .present        = agx_present,
};

/* ── gpu_backend_init ────────────────────────────────────────────────── */

const gpu_backend_ops_t *gpu_active_backend;

void gpu_backend_init(void)
{
    /* 1. Prova virtio-GPU (QEMU virt machine) */
    if (virtio_gpu_init()) {
        gpu_active_backend = &gpu_virtio_backend;
        uart_puts("[GPU] Backend: VirtIO-GPU (M5b-01)\n");
        return;
    }

    /* 2. Prova Apple AGX (M-series reale) */
    if (is_apple_silicon()) {
        gpu_active_backend = &gpu_agx_backend;
        uart_puts("[GPU] Backend: Apple AGX");
        uart_puts(" (driver MMIO in M5b-02, esecuzione via SW per ora)\n");
        return;
    }

    /* 3. SW fallback */
    gpu_active_backend = &gpu_sw_backend;
    uart_puts("[GPU] Backend: software fallback\n");
}
