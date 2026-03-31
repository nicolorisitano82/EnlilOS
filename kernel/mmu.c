/*
 * NROS Microkernel - MMU Implementation (M1-02)
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
#include "uart.h"

/* ── Page table L1 ─────────────────────────────────────────────────────
 *
 * Dichiarata in BSS (azzerata dal boot) e allineata a 4KB.
 * 512 entry × 8 byte = 4096 byte esatti.
 *
 * Solo L1 perché usiamo block entries da 1GB (T0SZ=25 → start a L1).
 * Nessun L0, nessun L2/L3 necessario per la mappa kernel.
 */
static uint64_t l1_table[512] __attribute__((aligned(4096)));

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

/* ── API pubblica ───────────────────────────────────────────────────── */

void mmu_init(void)
{
    uart_puts("[NROS] MMU: costruzione page table...\n");

    build_page_tables();

    uart_puts("[NROS] MMU: L1[0] = MMIO  (0x00000000-0x3FFFFFFF) Device-nGnRnE\n");
    uart_puts("[NROS] MMU: L1[1] = RAM   (0x40000000-0x7FFFFFFF) Normal WB\n");
    uart_puts("[NROS] MMU: abilitazione MMU + D-cache + I-cache...\n");

    mmu_enable();

    /* Se arriviamo qui la MMU è attiva e il codice gira cacheable */
    uart_puts("[NROS] MMU attiva — cache abilitate\n");
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

int mmu_enabled(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    return (sctlr & SCTLR_M) ? 1 : 0;
}
