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
#define PTE_COW         (1UL << 55)     /* Software bit: Copy-on-Write */
#define PTE_ADDR_MASK   0x0000FFFFFFFFF000ULL

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

/* ── Layout user-space minimo (M6-01) ─────────────────────────────── */

#define MMU_USER_BASE        0x0000007FC0000000ULL
#define MMU_USER_LIMIT       0x0000008000000000ULL
#define MMU_USER_STACK_TOP   0x0000007FFF000000ULL
#define MMU_USER_STACK_SIZE  (8ULL * 1024ULL * 1024ULL)
#define MMU_USER_SIGTRAMP_VA (MMU_USER_LIMIT - 0x1000ULL)

#define MMU_PROT_USER_R      (1U << 0)
#define MMU_PROT_USER_W      (1U << 1)
#define MMU_PROT_USER_X      (1U << 2)

typedef struct mm_space mm_space_t;

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
 * cache_invalidate_range(start, size) — esegue DC IVAC (Invalidate by VA
 * to PoC) su ogni cache line del range, senza write-back.
 *
 * Usare quando si ricevono dati via DMA e si vuole che la CPU li legga
 * dalla memoria fisica scartando qualsiasi copia stale in D-cache.
 * NB: le cache line dirty vengono perse — non usare su dati non ancora
 * scritti in RAM dal DMA.
 *
 * WCET: O(size / cache_line_size).
 */
void cache_invalidate_range(uintptr_t start, size_t size);

/*
 * mmu_enabled() — ritorna 1 se la MMU è attiva.
 */
int mmu_enabled(void);

/*
 * Spazio di indirizzamento corrente e kernel root.
 * Tutti gli mm_space condividono le mappature kernel lower-VA, ma possono
 * aggiungere mapping user-space nel window alto [MMU_USER_BASE, MMU_USER_LIMIT).
 */
mm_space_t *mmu_kernel_space(void);
mm_space_t *mmu_current_space(void);
mm_space_t *mmu_space_create(void);
int         mmu_space_map_signal_trampoline(mm_space_t *space);
mm_space_t *mmu_space_clone_cow(mm_space_t *parent, uintptr_t stack_copy_start);
void        mmu_space_destroy(mm_space_t *space);
void        mmu_activate_space(mm_space_t *space);

/*
 * Alloca backing fisico, mappa il range user e lo azzera.
 * start/size devono essere allineati a 4KB e restare nel window user.
 */
int         mmu_map_user_region(mm_space_t *space, uintptr_t start,
                                size_t size, uint32_t prot);
int         mmu_map_user_anywhere(mm_space_t *space, size_t size,
                                  uint32_t prot, uintptr_t *start_out);

/*
 * Traduce un VA user di 'space' in un puntatore kernel valido (identity-mapped
 * sulla RAM fisica backing del range). Ritorna NULL se il range non e'
 * interamente mappato nello stesso extent.
 */
void       *mmu_space_resolve_ptr(mm_space_t *space, uintptr_t va, size_t size);
int         mmu_space_prepare_write(mm_space_t *space, uintptr_t va, size_t size);
int         mmu_handle_user_fault(mm_space_t *space, uintptr_t far, uint64_t esr);

/*
 * Esegue il prefault di un range all'interno di uno specifico mm_space.
 * Fuori dal path RT: usa uno switch temporaneo di TTBR0 per warm-up del TLB.
 */
int         mmu_prefault_space_range(mm_space_t *space,
                                     uintptr_t start, uintptr_t end);

#endif
