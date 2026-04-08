/*
 * EnlilOS Microkernel - MMU Implementation (M1-02)
 * AArch64 Virtual Memory — identity mapping RT-ottimizzato
 *
 * Struttura page table scelta per il real-time:
 *
 *   T0SZ=25 → VA a 39 bit → la traduzione inizia da L1 (non da L0).
 *   Ogni entry L1 copre 1GB → block descriptor → nessun L2/L3.
 *   Risultato: TLB miss = 1 solo memory access (carica entry L1).
 *              WCET di traduzione = costante e minimo.
 *
 * Mappa fisica:
 *   L1[0] = 0x00000000–0x3FFFFFFF (1GB) → MMIO, Device-nGnRnE
 *   L1[1] = 0x40000000–0x7FFFFFFF (1GB) → RAM,  Normal WB
 *
 * L1 ha 512 entry × 8 byte = 4KB. Allineata a 4KB (requisito hardware).
 * Unica tabella necessaria: nessun overhead, O(1) garantito.
 */

#include "mmu.h"
#include "pmm.h"
#include "sched.h"
#include "syscall.h"
#include "uart.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

/* ── Page table L1 ─────────────────────────────────────────────────────
 *
 * Dichiarata in BSS (azzerata dal boot) e allineata a 4KB.
 * 512 entry × 8 byte = 4096 byte esatti.
 *
 * Solo L1 perché usiamo block entries da 1GB (T0SZ=25 → start a L1).
 * Nessun L0, nessun L2/L3 necessario per la mappa kernel.
 */
static uint64_t l1_table[512] __attribute__((aligned(4096)));

#define MMU_MAX_SPACES        SCHED_MAX_TASKS
#define MMU_MAX_SPACE_TABLES  16U
#define MMU_MAX_SPACE_EXTENTS 48U

typedef struct {
    uintptr_t va;
    uint64_t  pa;
    uint32_t  pages;
    uint8_t   order;
    uint8_t   _pad[3];
} mm_extent_t;

struct mm_space {
    uint64_t    root_pa;
    uint64_t   *root;
    uint32_t    refcount;
    uint8_t     in_use;
    uint8_t     is_kernel;
    uint8_t     table_count;
    uint8_t     extent_count;
    uintptr_t   brk;
    uintptr_t   mmap_base;
    uint64_t    table_pages[MMU_MAX_SPACE_TABLES];
    mm_extent_t extents[MMU_MAX_SPACE_EXTENTS];
};

static struct mm_space kernel_space;
static struct mm_space space_pool[MMU_MAX_SPACES];
static mm_space_t     *current_space = &kernel_space;
static uint64_t        signal_tramp_pa;

/* ── Barriere inline ────────────────────────────────────────────────── */

static inline void dsb_sy(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static inline void dsb_ish(void)
{
    __asm__ volatile("dsb ish" ::: "memory");
}

static inline void isb(void)
{
    __asm__ volatile("isb" ::: "memory");
}

static inline void tlbi_vmalle1(void)
{
    __asm__ volatile("tlbi vmalle1" ::: "memory");
}

static void invalidate_icache_range(uintptr_t start, uintptr_t end)
{
    uint64_t ctr;
    uint64_t line_size;
    uint64_t mask;
    uintptr_t addr;

    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    line_size = 4UL << (ctr & 0xFU);
    mask = line_size - 1UL;

    addr = start & ~(uintptr_t)mask;
    while (addr < end) {
        __asm__ volatile("ic ivau, %0" :: "r"(addr) : "memory");
        addr += line_size;
    }
    dsb_ish();
    isb();
}

/* Invalida I-cache e D-cache — necessario prima di abilitare la MMU */
static inline void cache_invalidate_all(void)
{
    __asm__ volatile(
        /* Invalida I-cache fino al PoU (Point of Unification) */
        "ic  iallu          \n"
        /* Invalida TLB */
        "tlbi vmalle1       \n"
        "dsb sy             \n"
        "isb                \n"
        ::: "memory"
    );
}

/*
 * clean_dcache_range — pulisce la D-cache per un range VA (write-back + invalidate)
 * Necessario per assicurare che le page table in RAM siano visibili al MMU walker.
 *
 * RT: usata solo al boot, non nel hot path. WCET dipende dalla dimensione.
 */
static void clean_dcache_range(uintptr_t start, uintptr_t end)
{
    /* Cache line size: leggi da CTR_EL0[19:16] (DminLine) */
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t dcache_line = 4UL << ((ctr >> 16) & 0xF);
    uint64_t mask = dcache_line - 1;

    uintptr_t addr = start & ~mask;
    while (addr < end) {
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
        addr += dcache_line;
    }
    dsb_sy();
}

static uint64_t table_desc(uint64_t pa)
{
    return (pa & PTE_ADDR_MASK) | PTE_VALID | PTE_TABLE;
}

static uint64_t user_leaf_flags(uint32_t prot)
{
    uint64_t flags = PTE_VALID | PTE_TABLE | PTE_AF | PTE_SH_INNER |
                     PTE_ATTRINDX(MT_NORMAL_WB) | PTE_nG | PTE_PXN;

    if (prot & MMU_PROT_USER_W)
        flags |= PTE_AP_ALL_RW;
    else
        flags |= PTE_AP_ALL_RO;

    if (!(prot & MMU_PROT_USER_X))
        flags |= PTE_UXN;

    return flags;
}

static uint32_t largest_order_fit(uint32_t remaining_pages)
{
    uint32_t order = 0U;

    while ((order + 1U) < MAX_ORDER &&
           (1U << (order + 1U)) <= remaining_pages)
        order++;

    return order;
}

static int space_track_table(mm_space_t *space, uint64_t pa)
{
    if (!space || space->table_count >= MMU_MAX_SPACE_TABLES)
        return -1;

    space->table_pages[space->table_count++] = pa;
    return 0;
}

static int space_track_extent(mm_space_t *space, uintptr_t va, uint64_t pa,
                              uint32_t pages, uint8_t order)
{
    mm_extent_t *ext;

    if (!space || pages == 0U || space->extent_count >= MMU_MAX_SPACE_EXTENTS)
        return -1;

    ext = &space->extents[space->extent_count++];
    ext->va    = va;
    ext->pa    = pa;
    ext->pages = pages;
    ext->order = order;
    return 0;
}

static int space_ensure_l3(mm_space_t *space, uintptr_t va, uint64_t **out_l3)
{
    uint32_t   l1i, l2i;
    uint64_t  *root;
    uint64_t  *l2;
    uint64_t  *l3;
    uint64_t   pa;

    if (!space || !out_l3) return -1;

    l1i = (uint32_t)((va >> 30) & 0x1FFU);
    l2i = (uint32_t)((va >> 21) & 0x1FFU);
    root = space->root;

    if ((root[l1i] & PTE_VALID) == 0U) {
        pa = phys_alloc_page();
        memset((void *)(uintptr_t)pa, 0, PAGE_SIZE);
        if (space_track_table(space, pa) < 0) {
            phys_free_page(pa);
            return -1;
        }
        root[l1i] = table_desc(pa);
        clean_dcache_range((uintptr_t)root, (uintptr_t)root + PAGE_SIZE);
    }

    l2 = (uint64_t *)(uintptr_t)(root[l1i] & PTE_ADDR_MASK);
    if ((l2[l2i] & PTE_VALID) == 0U) {
        pa = phys_alloc_page();
        memset((void *)(uintptr_t)pa, 0, PAGE_SIZE);
        if (space_track_table(space, pa) < 0) {
            phys_free_page(pa);
            return -1;
        }
        l2[l2i] = table_desc(pa);
        clean_dcache_range((uintptr_t)l2, (uintptr_t)l2 + PAGE_SIZE);
    }

    l3 = (uint64_t *)(uintptr_t)(l2[l2i] & PTE_ADDR_MASK);
    *out_l3 = l3;
    return 0;
}

static inline int pte_is_table_desc(uint64_t pte)
{
    return (pte & (PTE_VALID | PTE_TABLE)) == (PTE_VALID | PTE_TABLE);
}

static inline int pte_user_is_writable(uint64_t pte)
{
    return (pte & PTE_VALID) && ((pte & (3UL << 6)) == PTE_AP_ALL_RW);
}

static inline uint64_t pte_make_user_rw(uint64_t pte, uint64_t pa)
{
    pte &= ~(PTE_COW | (3UL << 6) | PTE_ADDR_MASK);
    pte |= PTE_AP_ALL_RW;
    pte |= (pa & PTE_ADDR_MASK);
    return pte;
}

static int mmu_lookup_pte(mm_space_t *space, uintptr_t va,
                          uint64_t **pte_out, uint64_t **l3_out)
{
    uint32_t  l1i, l2i, l3i;
    uint64_t *l2;
    uint64_t *l3;

    if (!space || !space->in_use || !pte_out)
        return -1;

    l1i = (uint32_t)((va >> 30) & 0x1FFU);
    l2i = (uint32_t)((va >> 21) & 0x1FFU);
    l3i = (uint32_t)((va >> 12) & 0x1FFU);

    if (!pte_is_table_desc(space->root[l1i]))
        return -1;

    l2 = (uint64_t *)(uintptr_t)(space->root[l1i] & PTE_ADDR_MASK);
    if (!pte_is_table_desc(l2[l2i]))
        return -1;

    l3 = (uint64_t *)(uintptr_t)(l2[l2i] & PTE_ADDR_MASK);
    *pte_out = &l3[l3i];
    if (l3_out)
        *l3_out = l3;
    return 0;
}

static void mmu_sync_table(mm_space_t *space, uintptr_t table_va)
{
    clean_dcache_range(table_va, table_va + PAGE_SIZE);
    dsb_ish();
    if (current_space == space) {
        tlbi_vmalle1();
        dsb_ish();
        isb();
    }
}

static int mmu_make_private_page(mm_space_t *space, uintptr_t va, int force_copy)
{
    uint64_t *pte;
    uint64_t *l3;
    uint64_t  old_pte;
    uint64_t  old_pa;
    uint64_t  new_pa;

    if (mmu_lookup_pte(space, va, &pte, &l3) < 0)
        return -1;

    old_pte = *pte;
    if ((old_pte & PTE_VALID) == 0U)
        return -1;

    if ((old_pte & PTE_COW) == 0U) {
        if (pte_user_is_writable(old_pte))
            return 0;
        return -1;
    }

    old_pa = old_pte & PTE_ADDR_MASK;
    if (!force_copy && phys_page_refcount(old_pa) <= 1U) {
        *pte = pte_make_user_rw(old_pte, old_pa);
        mmu_sync_table(space, (uintptr_t)l3);
        return 0;
    }

    new_pa = phys_alloc_page();
    memcpy((void *)(uintptr_t)new_pa, (const void *)(uintptr_t)old_pa, PAGE_SIZE);
    *pte = pte_make_user_rw(old_pte, new_pa);
    mmu_sync_table(space, (uintptr_t)l3);
    phys_free_page(old_pa);
    return 0;
}

static uint32_t mmu_user_l1_start(void)
{
    return (uint32_t)((MMU_USER_BASE >> 30) & 0x1FFU);
}

static uint32_t mmu_user_l1_end(void)
{
    return (uint32_t)(((MMU_USER_LIMIT - 1ULL) >> 30) & 0x1FFU);
}

static int map_user_pages_raw(mm_space_t *space, uintptr_t va, uint64_t pa,
                              uint32_t pages, uint32_t prot)
{
    uint64_t flags = user_leaf_flags(prot);

    for (uint32_t i = 0U; i < pages; i++) {
        uintptr_t cur_va = va + (uintptr_t)i * PAGE_SIZE;
        uint64_t *l3;
        uint32_t  l3i;

        if (space_ensure_l3(space, cur_va, &l3) < 0)
            return -1;

        l3i = (uint32_t)((cur_va >> 12) & 0x1FFU);
        if (l3[l3i] & PTE_VALID)
            return -1;

        l3[l3i] = ((pa + (uint64_t)i * PAGE_SIZE) & PTE_ADDR_MASK) | flags;
        clean_dcache_range((uintptr_t)l3, (uintptr_t)l3 + PAGE_SIZE);
    }

    dsb_ish();
    isb();
    return 0;
}

int mmu_space_map_signal_trampoline(mm_space_t *space)
{
    static const uint32_t sigtramp_code[] = {
        0xD2800268U, /* movz x8, #SYS_SIGRETURN */
        0xD4000001U, /* svc #0 */
        0xD4200000U, /* brk #0 */
    };
    uintptr_t page_va;
    int       first_map = 0;
    uint64_t *pte;

    if (!space || !space->in_use)
        return -1;

    page_va = MMU_USER_SIGTRAMP_VA & PAGE_MASK;
    if (mmu_lookup_pte(space, page_va, &pte, NULL) == 0)
        return 0;

    if (signal_tramp_pa == 0ULL) {
        signal_tramp_pa = phys_alloc_page();
        if (signal_tramp_pa == 0ULL)
            return -1;
        memset((void *)(uintptr_t)signal_tramp_pa, 0, PAGE_SIZE);
        memcpy((void *)(uintptr_t)signal_tramp_pa,
               sigtramp_code, sizeof(sigtramp_code));
        clean_dcache_range(signal_tramp_pa, signal_tramp_pa + PAGE_SIZE);
        invalidate_icache_range(signal_tramp_pa, signal_tramp_pa + PAGE_SIZE);
        first_map = 1;
    }

    if (!first_map)
        phys_retain_page(signal_tramp_pa);

    if (map_user_pages_raw(space, page_va, signal_tramp_pa, 1U,
                           MMU_PROT_USER_R | MMU_PROT_USER_X) < 0) {
        phys_free_page(signal_tramp_pa);
        return -1;
    }

    return 0;
}

/* ── Costruzione page table ─────────────────────────────────────────── */

static void build_page_tables(void)
{
    /*
     * L1[0]: mappa il primo GB come Device memory.
     * Copre tutto l'MMIO di QEMU virt:
     *   0x08000000 GIC-400
     *   0x09000000 PL011 UART
     *   0x09020000 fw_cfg
     *   0x09050000 PL050 PS/2
     *   0x0a000000 VirtIO
     *
     * PXN+UXN: nessun codice eseguibile in MMIO.
     * SH_NONE: i device non sono in un dominio di coerenza cache.
     */
    l1_table[0] = (uint64_t)0x00000000UL | PTE_DEVICE;

    /*
     * L1[1]: mappa il secondo GB come Normal WB.
     * Copre la RAM di QEMU: 0x40000000–0x5FFFFFFF (512MB).
     *
     * PTE_KERNEL_RWX: il kernel ha R/W/X su tutta la RAM.
     * In una fase successiva (dopo M1-03) si restringe:
     *   - .text   → R/X (PXN=0, UXN=1)
     *   - .rodata → R/O (PXN=1, UXN=1)
     *   - .data   → R/W (PXN=1, UXN=1, no exec)
     * Per ora un'unica regione RWX è sufficiente per il boot.
     */
    l1_table[1] = (uint64_t)0x40000000UL | PTE_KERNEL_RWX;

    /*
     * L1[2..511]: non mappati (entry = 0, bit VALID = 0).
     * Un accesso a VA fuori dalla mappa genera Translation Fault
     * → catturato dalla vector table (M1-01) con dump diagnostico.
     * Questo è il comportamento corretto per un sistema RT: fallire
     * in modo determinato e visibile, mai silenzioso.
     */

    /* Pulisci le page table dalla D-cache per renderle visibili al page walker */
    clean_dcache_range((uintptr_t)l1_table,
                       (uintptr_t)l1_table + sizeof(l1_table));
}

/* ── Abilitazione MMU ───────────────────────────────────────────────── */

static void mmu_enable(void)
{
    uint64_t ttbr0 = (uint64_t)(uintptr_t)l1_table;

    __asm__ volatile(
        /* 1. Invalida cache e TLB prima di riconfigurare */
        "ic   iallu                     \n"
        "tlbi vmalle1                   \n"
        "dsb  sy                        \n"
        "isb                            \n"

        /* 2. Configura MAIR_EL1 (attributi di memoria) */
        "msr  mair_el1,  %[mair]        \n"
        "isb                            \n"

        /* 3. Configura TCR_EL1 (dimensione VA, granularità, cache policy) */
        "msr  tcr_el1,   %[tcr]         \n"
        "isb                            \n"

        /* 4. Carica la tabella L1 in TTBR0_EL1 */
        "msr  ttbr0_el1, %[ttbr0]       \n"
        "isb                            \n"

        /* 5. Invalida TLB dopo aver impostato TTBR0 */
        "tlbi vmalle1                   \n"
        "dsb  ish                       \n"
        "isb                            \n"

        /* 6. Abilita MMU + D-cache + I-cache in SCTLR_EL1 */
        "mrs  x0, sctlr_el1             \n"
        "orr  x0, x0, %[sctlr_bits]    \n"
        "msr  sctlr_el1, x0            \n"

        /* 7. ISB finale: svuota pipeline, da qui ogni istruzione usa la MMU */
        "isb                            \n"

        ::  [mair]       "r"((uint64_t)MAIR_VALUE),
            [tcr]        "r"((uint64_t)TCR_VALUE),
            [ttbr0]      "r"(ttbr0),
            [sctlr_bits] "r"((uint64_t)(SCTLR_M | SCTLR_C | SCTLR_I))
        : "x0", "memory"
    );
}

/* ── mm_space API ───────────────────────────────────────────────────── */

mm_space_t *mmu_kernel_space(void)
{
    return &kernel_space;
}

mm_space_t *mmu_current_space(void)
{
    return current_space ? current_space : &kernel_space;
}

mm_space_t *mmu_space_create(void)
{
    for (uint32_t i = 0U; i < MMU_MAX_SPACES; i++) {
        mm_space_t *space = &space_pool[i];
        uint64_t    pa;

        if (space->in_use)
            continue;

        pa = phys_alloc_page();
        memset((void *)(uintptr_t)pa, 0, PAGE_SIZE);
        memcpy((void *)(uintptr_t)pa, kernel_space.root, PAGE_SIZE);

        space->root_pa     = pa;
        space->root        = (uint64_t *)(uintptr_t)pa;
        space->refcount    = 1U;
        space->in_use      = 1U;
        space->is_kernel   = 0U;
        space->table_count = 0U;
        space->extent_count = 0U;
        space->brk         = 0ULL;
        space->mmap_base   = MMU_USER_BASE;
        clean_dcache_range((uintptr_t)space->root,
                           (uintptr_t)space->root + PAGE_SIZE);
        return space;
    }
    return NULL;
}

mm_space_t *mmu_space_clone_cow(mm_space_t *parent, uintptr_t stack_copy_start)
{
    mm_space_t *child;
    uint32_t    l1_start;
    uint32_t    l1_end;

    if (!parent || !parent->in_use)
        return NULL;

    child = mmu_space_create();
    if (!child)
        return NULL;

    child->brk       = parent->brk;
    child->mmap_base = parent->mmap_base;
    l1_start = mmu_user_l1_start();
    l1_end   = mmu_user_l1_end();

    for (uint32_t l1i = l1_start; l1i <= l1_end; l1i++) {
        uint64_t root_desc = parent->root[l1i];
        uint64_t *parent_l2;

        if (!pte_is_table_desc(root_desc))
            continue;

        parent_l2 = (uint64_t *)(uintptr_t)(root_desc & PTE_ADDR_MASK);
        for (uint32_t l2i = 0U; l2i < 512U; l2i++) {
            uint64_t l2_desc = parent_l2[l2i];
            uint64_t *parent_l3;

            if (!pte_is_table_desc(l2_desc))
                continue;

            parent_l3 = (uint64_t *)(uintptr_t)(l2_desc & PTE_ADDR_MASK);
            for (uint32_t l3i = 0U; l3i < 512U; l3i++) {
                uintptr_t cur_va = ((uintptr_t)l1i << 30) |
                                   ((uintptr_t)l2i << 21) |
                                   ((uintptr_t)l3i << 12);
                uint64_t  pte = parent_l3[l3i];
                uint64_t *child_l3;

                if ((pte & PTE_VALID) == 0U)
                    continue;

                if (space_ensure_l3(child, cur_va, &child_l3) < 0) {
                    mmu_space_destroy(child);
                    return NULL;
                }

                if (pte_user_is_writable(pte) || (pte & PTE_COW)) {
                    pte &= ~(3UL << 6);
                    pte |= PTE_AP_ALL_RO | PTE_COW;
                    parent_l3[l3i] = pte;
                }

                child_l3[l3i] = pte;
                mmu_sync_table(child, (uintptr_t)child_l3);
                phys_retain_page(pte & PTE_ADDR_MASK);
            }

            mmu_sync_table(parent, (uintptr_t)parent_l3);
        }
    }

    if (stack_copy_start >= (MMU_USER_STACK_TOP - MMU_USER_STACK_SIZE) &&
        stack_copy_start < MMU_USER_STACK_TOP) {
        uintptr_t cur = stack_copy_start & PAGE_MASK;

        while (cur < MMU_USER_STACK_TOP) {
            uint64_t *pte;

            if (mmu_lookup_pte(child, cur, &pte, NULL) == 0)
                (void)mmu_make_private_page(child, cur, 1);
            cur += PAGE_SIZE;
        }
    }

    return child;
}

void mmu_activate_space(mm_space_t *space)
{
    uint64_t ttbr0;

    if (!space)
        space = &kernel_space;
    if (current_space == space)
        return;

    ttbr0 = space->root_pa;
    __asm__ volatile("msr ttbr0_el1, %0" :: "r"(ttbr0) : "memory");
    __asm__ volatile("tlbi vmalle1" ::: "memory");
    dsb_ish();
    isb();
    current_space = space;
}

void mmu_space_destroy(mm_space_t *space)
{
    uint32_t l1_start;
    uint32_t l1_end;

    if (!space || !space->in_use || space->is_kernel)
        return;

    if (current_space == space)
        mmu_activate_space(&kernel_space);

    l1_start = mmu_user_l1_start();
    l1_end   = mmu_user_l1_end();

    for (uint32_t l1i = l1_start; l1i <= l1_end; l1i++) {
        uint64_t root_desc = space->root[l1i];
        uint64_t *l2;

        if (!pte_is_table_desc(root_desc))
            continue;

        l2 = (uint64_t *)(uintptr_t)(root_desc & PTE_ADDR_MASK);
        for (uint32_t l2i = 0U; l2i < 512U; l2i++) {
            uint64_t l2_desc = l2[l2i];
            uint64_t *l3;

            if (!pte_is_table_desc(l2_desc))
                continue;

            l3 = (uint64_t *)(uintptr_t)(l2_desc & PTE_ADDR_MASK);
            for (uint32_t l3i = 0U; l3i < 512U; l3i++) {
                uint64_t pte = l3[l3i];

                if ((pte & PTE_VALID) == 0U)
                    continue;

                phys_free_page(pte & PTE_ADDR_MASK);
                l3[l3i] = 0ULL;
            }

            phys_free_page(l2_desc & PTE_ADDR_MASK);
            l2[l2i] = 0ULL;
        }

        phys_free_page(root_desc & PTE_ADDR_MASK);
        space->root[l1i] = 0ULL;
    }

    phys_free_page(space->root_pa);
    memset(space, 0, sizeof(*space));
}

int mmu_map_user_region(mm_space_t *space, uintptr_t start,
                        size_t size, uint32_t prot)
{
    uintptr_t cur_va;
    uint32_t  remaining_pages;

    if (!space || !space->in_use || size == 0U)
        return -1;
    if ((start & (PAGE_SIZE - 1ULL)) != 0ULL || (size & (PAGE_SIZE - 1ULL)) != 0ULL)
        return -1;
    if (start < MMU_USER_BASE || start + size < start || start + size > MMU_USER_LIMIT)
        return -1;

    cur_va = start;
    remaining_pages = (uint32_t)(size / PAGE_SIZE);

    while (remaining_pages > 0U) {
        uint32_t order = largest_order_fit(remaining_pages);
        uint32_t pages = 1U << order;
        uint64_t pa = phys_alloc_pages(order);
        size_t   bytes = (size_t)pages * PAGE_SIZE;

        memset((void *)(uintptr_t)pa, 0, bytes);
        if (space_track_extent(space, cur_va, pa, pages, (uint8_t)order) < 0)
            return -1;
        if (map_user_pages_raw(space, cur_va, pa, pages, prot) < 0)
            return -1;

        cur_va += (uintptr_t)bytes;
        remaining_pages -= pages;
    }

    return 0;
}

int mmu_map_user_anywhere(mm_space_t *space, size_t size,
                          uint32_t prot, uintptr_t *start_out)
{
    uintptr_t start;
    size_t    aligned;

    if (!space || !space->in_use || size == 0U)
        return -1;

    aligned = (size + PAGE_SIZE - 1ULL) & PAGE_MASK;
    if (aligned == 0U)
        return -1;

    start = (space->mmap_base + PAGE_SIZE - 1ULL) & PAGE_MASK;
    if (start < MMU_USER_BASE || start + aligned < start ||
        start + aligned > MMU_USER_SIGTRAMP_VA)
        return -1;

    if (mmu_map_user_region(space, start, aligned, prot) < 0)
        return -1;

    space->mmap_base = start + aligned;
    if (start_out)
        *start_out = start;
    return 0;
}

void *mmu_space_resolve_ptr(mm_space_t *space, uintptr_t va, size_t size)
{
    uintptr_t end = va + size;
    uintptr_t cur = va;
    uintptr_t expected_pa = 0ULL;
    uintptr_t resolved = 0ULL;

    if (!space || !space->in_use || end < va)
        return NULL;

    while (cur < end) {
        uint64_t *pte;
        uintptr_t page_pa;
        uintptr_t page_off;
        uintptr_t chunk;

        if (mmu_lookup_pte(space, cur, &pte, NULL) < 0)
            return NULL;
        if ((*pte & PTE_VALID) == 0U)
            return NULL;

        page_pa = (uintptr_t)(*pte & PTE_ADDR_MASK);
        page_off = cur & (PAGE_SIZE - 1ULL);
        chunk = PAGE_SIZE - page_off;
        if (chunk > (end - cur))
            chunk = end - cur;

        if (cur == va) {
            resolved = page_pa + page_off;
            expected_pa = resolved + chunk;
        } else {
            if (page_pa != (expected_pa & PAGE_MASK))
                return NULL;
            expected_pa += chunk;
        }

        cur += chunk;
    }

    return (void *)(uintptr_t)resolved;
}

int mmu_space_prepare_write(mm_space_t *space, uintptr_t va, size_t size)
{
    uintptr_t end = va + size;
    uintptr_t cur = va & PAGE_MASK;

    if (!space || !space->in_use || end < va)
        return -1;

    while (cur < end) {
        uint64_t *pte;

        if (mmu_lookup_pte(space, cur, &pte, NULL) < 0)
            return -1;
        if ((*pte & PTE_COW) != 0U) {
            if (mmu_make_private_page(space, cur, 0) < 0)
                return -1;
        } else if (!pte_user_is_writable(*pte)) {
            return -1;
        }
        cur += PAGE_SIZE;
    }

    return 0;
}

int mmu_handle_user_fault(mm_space_t *space, uintptr_t far, uint64_t esr)
{
    uintptr_t va = far & PAGE_MASK;
    uint32_t  iss = (uint32_t)ESR_ISS(esr);
    uint32_t  wnr = (iss >> 6) & 1U;

    if (!space || !space->in_use)
        return -1;
    if (!wnr)
        return -1;
    if (va < MMU_USER_BASE || va >= MMU_USER_LIMIT)
        return -1;

    return mmu_make_private_page(space, va, 0);
}

int mmu_prefault_space_range(mm_space_t *space, uintptr_t start, uintptr_t end)
{
    mm_space_t *saved;
    uint64_t    daif_flags;

    if (!space || !space->in_use || end < start)
        return -1;

    /*
     * Disable IRQs for the activate→prefault→restore sequence.
     * A timer-driven context switch between mmu_activate_space(space) and
     * mmu_activate_space(saved) would let schedule_locked re-activate the
     * resuming task's mm_space, so when we continue mmu_prefault_range the
     * TTBR0 points to the wrong address space → EL1 translation fault on
     * user-space VAs.  Disabling IRQs here is safe: the function is called
     * only at task-creation time (not in any RT hot path), and the window
     * is bounded by the number of pages in [start, end].
     */
    __asm__ volatile("mrs %0, daif\n msr daifset, #2\n"
                     : "=r"(daif_flags) :: "memory");
    saved = mmu_current_space();
    mmu_activate_space(space);
    mmu_prefault_range(start, end);
    mmu_activate_space(saved);
    __asm__ volatile("msr daif, %0" :: "r"(daif_flags) : "memory");
    return 0;
}

/* ── API pubblica ───────────────────────────────────────────────────── */

void mmu_init(void)
{
    uart_puts("[EnlilOS] MMU: costruzione page table...\n");

    build_page_tables();

    uart_puts("[EnlilOS] MMU: L1[0] = MMIO  (0x00000000-0x3FFFFFFF) Device-nGnRnE\n");
    uart_puts("[EnlilOS] MMU: L1[1] = RAM   (0x40000000-0x7FFFFFFF) Normal WB\n");
    uart_puts("[EnlilOS] MMU: abilitazione MMU + D-cache + I-cache...\n");

    mmu_enable();

    kernel_space.root_pa      = (uint64_t)(uintptr_t)l1_table;
    kernel_space.root         = l1_table;
    kernel_space.in_use       = 1U;
    kernel_space.is_kernel    = 1U;
    kernel_space.table_count  = 0U;
    kernel_space.extent_count = 0U;
    current_space             = &kernel_space;

    /* Se arriviamo qui la MMU è attiva e il codice gira cacheable */
    uart_puts("[EnlilOS] MMU attiva — cache abilitate\n");
}

/*
 * mmu_prefault_range — warm up del TLB per un range critico.
 *
 * Per ogni pagina nel range: esegui una lettura dal VA per forzare la
 * traduzione e caricare l'entry nel TLB. Usare sui task hard-RT prima
 * del loro avvio per garantire zero TLB miss durante il loro window RT.
 *
 * RT: chiamare questa funzione *fuori* dal window critico (es. setup).
 */
void mmu_prefault_range(uintptr_t start, uintptr_t end)
{
    /* Arrotonda all'inizio della pagina */
    uintptr_t addr = start & ~0xFFFUL;
    volatile uint8_t dummy;

    while (addr < end) {
        /* Lettura dummy: forza il TLB miss (se presente) ora, non dopo */
        dummy = *(volatile uint8_t *)addr;
        (void)dummy;
        addr += 4096;
    }

    /* Barriera per assicurare che tutte le traduzioni siano completate */
    dsb_ish();
    isb();
}

void cache_flush_range(uintptr_t start, size_t size)
{
    /* Legge la dimensione della cache line D da CTR_EL0[19:16] (DminLine).
     * DminLine = log2(words), quindi line_size = 4 << DminLine bytes.     */
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t line = (uintptr_t)(4UL << ((ctr >> 16) & 0xF));
    uintptr_t mask = line - 1;
    uintptr_t addr = start & ~mask;
    uintptr_t end  = start + size;

    while (addr < end) {
        /* DC CIVAC: Clean and Invalidate D-cache line by VA to PoC.
         * "PoC" = Point of Coherency = memoria fisica che tutti i master
         * (CPU, DMA, QEMU) vedono. Dopo questa istruzione la cache line
         * è written-back e la CPU non la leggerà più dalla cache.         */
        __asm__ volatile("dc civac, %0" :: "r"(addr) : "memory");
        addr += line;
    }
    /* DSB SY: aspetta che tutte le DC CIVAC siano completate prima
     * di procedere. Obbligatorio prima di cedere il buffer a DMA.       */
    __asm__ volatile("dsb sy" ::: "memory");
}

void cache_invalidate_range(uintptr_t start, size_t size)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t line = (uintptr_t)(4UL << ((ctr >> 16) & 0xF));
    uintptr_t mask = line - 1U;
    uintptr_t addr = start & ~mask;
    uintptr_t end  = start + size;

    while (addr < end) {
        __asm__ volatile("dc ivac, %0" :: "r"(addr) : "memory");
        addr += line;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

int mmu_enabled(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (sctlr & SCTLR_M) ? 1 : 0;
}
