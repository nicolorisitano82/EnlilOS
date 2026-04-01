/*
 * EnlilOS Microkernel - MMU (M1-02)
 * AArch64 Virtual Memory — design orientato al real-time
 *
 * Scelte RT:
 *   - Block entries da 1GB (L1 direct): un solo livello di page table walk
 *     → TLB miss WCET deterministico e minimo
 *   - Kernel identity-mapped: VA == PA per tutto il kernel
 *   - TTBR0: kernel + MMIO (lower VA)
 *   - TTBR1: disabilitato ora, userà ASID per context switch O(1)
 *   - Cache: Write-Back per RAM, Device-nGnRnE per MMIO
 *   - Tutte le pagine kernel pre-faultate al boot: zero page fault in runtime
 *
 * Layout VA/PA (identità):
 *   0x00000000_00000000 – 0x00000000_3FFFFFFF  (1GB) MMIO  → Device memory
 *   0x00000000_40000000 – 0x00000000_7FFFFFFF  (1GB) RAM   → Normal WB cacheable
 */

#ifndef ENLILOS_MMU_H
#define ENLILOS_MMU_H

#include "types.h"

/* ── MAIR_EL1 — attributi di memoria ──────────────────────────────────
 *
 * MAIR è un registro a 64 bit. Ogni byte definisce un "tipo" di memoria.
 * Si referenzia negli entry di page table con AttrIndx[2:0].
 *
 *  Index 0 (MT_DEVICE):     0x00 = Device-nGnRnE (no gather, no reorder, no early write)
 *  Index 1 (MT_NORMAL_WB):  0xFF = Normal, Inner+Outer Write-Back, RA+WA
 *  Index 2 (MT_NORMAL_NC):  0x44 = Normal, Inner+Outer Non-Cacheable
 *                                   (per RT shared memory a latenza deterministica)
 */
#define MT_DEVICE       0
#define MT_NORMAL_WB    1
#define MT_NORMAL_NC    2

#define MAIR_ATTR(idx, val)     ((uint64_t)(val) << ((idx) * 8))
#define MAIR_VALUE  (MAIR_ATTR(MT_DEVICE,    0x00UL) | \
                     MAIR_ATTR(MT_NORMAL_WB, 0xFFUL) | \
                     MAIR_ATTR(MT_NORMAL_NC, 0x44UL))

/* ── TCR_EL1 — Translation Control Register ───────────────────────────
 *
 * T0SZ=25 → 39-bit VA per TTBR0 (range 0x0–0x7FFFFFFFFF, 512GB)
 *            Sufficiente per coprire tutto l'indirizzo fisico di QEMU.
 *            Inizia da L1 (nessun L0 necessario): semplice e RT-friendly.
 *
 * EPD1=1   → TTBR1 disabilitato (no kernel/user split ancora)
 * TG0=00   → granularità 4KB per TTBR0
 * IRGN0=01 → Inner cache WB, RA, WA
 * ORGN0=01 → Outer cache WB, RA, WA
 * SH0=11   → Inner Shareable (coerente tra core)
 * IPS=001  → PA size 40-bit (1TB) — più che sufficiente per QEMU
 */
#define TCR_T0SZ(n)     ((uint64_t)(n) << 0)
#define TCR_IRGN0_WBWA  (1UL << 8)
#define TCR_ORGN0_WBWA  (1UL << 10)
#define TCR_SH0_INNER   (3UL << 12)
#define TCR_TG0_4K      (0UL << 14)
#define TCR_EPD1        (1UL << 23)     /* Disabilita TTBR1 */
#define TCR_IPS_40BIT   (2UL << 32)

#define TCR_VALUE   (TCR_T0SZ(25)       | \
                     TCR_IRGN0_WBWA     | \
                     TCR_ORGN0_WBWA     | \
                     TCR_SH0_INNER      | \
                     TCR_TG0_4K         | \
                     TCR_EPD1           | \
                     TCR_IPS_40BIT)

/* ── SCTLR_EL1 — bit di controllo ─────────────────────────────────────*/
#define SCTLR_M         (1UL << 0)   /* Enable MMU          */
#define SCTLR_C         (1UL << 2)   /* Enable D-cache      */
#define SCTLR_I         (1UL << 12)  /* Enable I-cache      */

/* ── Attributi degli entry di page table ──────────────────────────────
 *
 * Formato descriptor AArch64 (4KB granule, L1 block = 1GB):
 *
 *  [1:0]    = 01 (block) o 11 (table/page)
 *  [4:2]    = AttrIndx — indice nel MAIR
 *  [5]      = NS (Non-Secure) — 0
 *  [7:6]    = AP  — Access Permission
 *  [9:8]    = SH  — Shareability
 *  [10]     = AF  — Access Flag (deve essere 1, altrimenti Access Flag fault)
 *  [11]     = nG  — not Global (0=globale, in TLB senza ASID)
 *  [47:30]  = PA[47:30] (per block L1 da 1GB)
 *  [53]     = PXN — Privileged Execute Never
 *  [54]     = UXN — Unprivileged Execute Never
 */
#define PTE_VALID       (1UL << 0)
#define PTE_TABLE       (1UL << 1)      /* 1 = table/page, 0 = block */
#define PTE_BLOCK       (0UL << 1)
#define PTE_AF          (1UL << 10)     /* Access Flag — obbligatorio */
#define PTE_nG          (1UL << 11)     /* not Global */
#define PTE_PXN         (1UL << 53)     /* Privileged Execute Never */
#define PTE_UXN         (1UL << 54)     /* Unprivileged Execute Never */

/* Shareability */
#define PTE_SH_NONE     (0UL << 8)
#define PTE_SH_OUTER    (2UL << 8)
#define PTE_SH_INNER    (3UL << 8)

/* Access Permissions */
#define PTE_AP_EL1_RW   (0UL << 6)     /* EL1 R/W, EL0 nessun accesso */
#define PTE_AP_ALL_RW   (1UL << 6)     /* EL0+EL1 R/W */
#define PTE_AP_EL1_RO   (2UL << 6)     /* EL1 R/O */
#define PTE_AP_ALL_RO   (3UL << 6)     /* EL0+EL1 R/O */

/* AttrIndx nel descriptor */
#define PTE_ATTRINDX(n) ((uint64_t)(n) << 2)

/* Combinazioni pronte all'uso */
#define PTE_KERNEL_RWX  (PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER | \
                         PTE_ATTRINDX(MT_NORMAL_WB) | PTE_AP_EL1_RW | PTE_UXN)

#define PTE_KERNEL_RW   (PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_INNER | \
                         PTE_ATTRINDX(MT_NORMAL_WB) | PTE_AP_EL1_RW | \
                         PTE_PXN | PTE_UXN)

#define PTE_DEVICE      (PTE_VALID | PTE_BLOCK | PTE_AF | PTE_SH_NONE | \
                         PTE_ATTRINDX(MT_DEVICE) | PTE_AP_EL1_RW | \
                         PTE_PXN | PTE_UXN)

/* ── API pubblica ──────────────────────────────────────────────────── */

/*
 * mmu_init() — configura e abilita la MMU.
 * Deve essere chiamata una volta sola, prima di qualsiasi accesso utente.
 * Dopo il ritorno: I-cache, D-cache e MMU sono attivi.
 *
 * RT: questa funzione non è nel hot path — è chiamata una volta al boot.
 *     Al termine, tutta la memoria kernel è mappata e cacheable.
 */
void mmu_init(void);

/*
 * mmu_prefault_range(start, end) — assicura che un range di VA sia
 * presente nel TLB. Usare prima di avviare task hard-RT per eliminare
 * TLB miss durante l'esecuzione critica.
 */
void mmu_prefault_range(uintptr_t start, uintptr_t end);

/*
 * cache_flush_range(start, size) — esegue DC CIVAC (Clean + Invalidate
 * by VA to PoC) su ogni cache line del range.
 *
 * Necessario quando la CPU scrive in RAM e un'altra entità (QEMU DMA,
 * driver DMA, altro core) deve leggere la versione aggiornata dalla
 * memoria fisica — bypassando la D-cache.
 *
 * Casi d'uso:
 *   - Framebuffer: dopo ogni operazione di disegno, prima che QEMU
 *     legga il buffer per aggiornare il display.
 *   - DMA descriptors: prima di dare un buffer a un device DMA.
 *   - Strutture IPC condivise tra EL1 e EL0.
 *
 * WCET: O(size / cache_line_size). Per 800×600×4=1.92MB ≈ 30000 ops.
 */
void cache_flush_range(uintptr_t start, size_t size);

/*
 * mmu_enabled() — ritorna 1 se la MMU è attiva.
 */
int mmu_enabled(void);

#endif
