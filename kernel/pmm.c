/*
 * EnlilOS Microkernel - Physical Memory Manager (M1-03)
 *
 * Implementazione buddy + slab orientata al real-time.
 *
 * Strutture dati:
 *
 *   page_state[NUM_PAGES]   — uint8_t, stato di ciascuna pagina fisica
 *   page_ord[NUM_PAGES]     — uint8_t, ordine del blocco buddy (testa)
 *   buddy_sent[MAX_ORDER]   — sentinelle circolari (blk_node_t)
 *   slab_caches[7]          — slab_cache_t, una per classe di dimensione
 *
 * Entrambi gli array sono in .bss (inizializzati a 0 dal boot code).
 * La RAM fisica riservata al kernel (~2MB) non entra mai nella free list.
 */

#include "pmm.h"
#include "kdebug.h"
#include "uart.h"

/* ── Liste circolari doppie — struttura intrusive ──────────────────────
 *
 * blk_node_t è posta all'offset 0 di ogni blocco libero del buddy.
 * La sentinella (buddy_sent[order]) non rappresenta un blocco reale:
 * buddy_sent[o].next == &buddy_sent[o]  ⟺  lista vuota.
 *
 * WCET: insert = O(1), remove = O(1), pop = O(1).
 */
typedef struct blk_node {
    struct blk_node *prev;
    struct blk_node *next;
} blk_node_t;

static inline void list_init(blk_node_t *s)
{
    s->prev = s->next = s;
}

static inline int list_empty(const blk_node_t *s)
{
    return s->next == s;
}

/* Inserisce 'n' dopo la sentinella (push-front) */
static inline void list_push(blk_node_t *s, blk_node_t *n)
{
    n->next       = s->next;
    n->prev       = s;
    s->next->prev = n;
    s->next       = n;
}

/* Rimuove 'n' dalla lista (qualsiasi posizione) */
static inline void list_remove(blk_node_t *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
    n->prev = n->next = n;          /* isola il nodo rimosso */
}

/* Estrae il primo elemento; NULL se vuota */
static inline blk_node_t *list_pop(blk_node_t *s)
{
    if (list_empty(s)) return NULL;
    blk_node_t *n = s->next;
    list_remove(n);
    return n;
}

/* ── Metadati delle pagine fisiche ─────────────────────────────────── */

static uint8_t page_state[NUM_PAGES];   /* PAGE_FREE / PAGE_ALLOC / … */
static uint8_t page_ord[NUM_PAGES];     /* ordine buddy (solo per testa) */
static uint16_t page_refs[NUM_PAGES];   /* refcount fisico per fork/COW   */

/* ── Buddy free lists ──────────────────────────────────────────────── */

static blk_node_t buddy_sent[MAX_ORDER];
static uint32_t   buddy_count[MAX_ORDER];   /* blocchi liberi per ordine */
static uint32_t   total_free_pages;

/* ── Slab allocator ────────────────────────────────────────────────── */

/*
 * Intestazione posta all'offset 0 di ogni pagina allocata dal kernel.
 *
 *  class_idx:
 *    0–6       slab generico (indice in slab_caches[])
 *    0xFE      named cache (aux = indice in named_caches[])
 *    0xFF      large allocation da phys_alloc_pages (aux = order)
 *
 *  magic:
 *    SLAB_MAGIC_LIVE  → allocazione valida
 *    SLAB_MAGIC_FREE  → già liberata (double-free detector)
 */
#define SLAB_MAGIC_LIVE  0xBA5E
#define SLAB_MAGIC_FREE  0xDEAD
#define SLAB_CLASS_NAMED 0xFE
#define SLAB_CLASS_LARGE 0xFF
#define SLAB_CANARY_WORD 0xDEADC0DEU
#define SLAB_REDZONE_SZ  4U

typedef struct {
    uint8_t  class_idx;
    uint8_t  aux;        /* named: cache index; large: order */
    uint16_t magic;      /* SLAB_MAGIC_LIVE o SLAB_MAGIC_FREE */
    uint32_t _reserved;
} slab_hdr_t;            /* 8 byte — non cambia l'offset dei dati */

/* Classi di dimensione slab: 32, 64, 128, 256, 512, 1024, 2048 */
#define SLAB_NUM_CLASSES    7
static const uint32_t slab_sizes[SLAB_NUM_CLASSES] = {
    32, 64, 128, 256, 512, 1024, 2048
};

typedef struct {
    void    *free_head;     /* lista libera (intrusive: ptr nei primi 8B) */
    uint32_t obj_size;
    uint32_t free_count;
    uint32_t total_count;
} slab_cache_t;

static slab_cache_t slab_caches[SLAB_NUM_CLASSES];

static inline uint32_t slab_usable_size(int ci)
{
    return slab_sizes[ci] - (2U * SLAB_REDZONE_SZ);
}

static inline uint8_t *slab_raw_from_user_ptr(const void *ptr)
{
    return (uint8_t *)(uintptr_t)((uint64_t)(uintptr_t)ptr - SLAB_REDZONE_SZ);
}

static inline void *slab_user_from_raw(void *raw)
{
    return (void *)(uintptr_t)((uint64_t)(uintptr_t)raw + SLAB_REDZONE_SZ);
}

static void slab_write_canaries(int ci, void *raw)
{
    uint8_t  *slot = (uint8_t *)raw;
    uint32_t *head = (uint32_t *)(void *)slot;
    uint32_t *tail = (uint32_t *)(void *)(slot + slab_caches[ci].obj_size - SLAB_REDZONE_SZ);

    *head = SLAB_CANARY_WORD;
    *tail = SLAB_CANARY_WORD;
}

static int slab_validate_canaries(const void *ptr, int ci)
{
    const uint8_t  *slot = slab_raw_from_user_ptr(ptr);
    const uint32_t *head = (const uint32_t *)(const void *)slot;
    const uint32_t *tail = (const uint32_t *)(const void *)
                           (slot + slab_caches[ci].obj_size - SLAB_REDZONE_SZ);

    if (*head != SLAB_CANARY_WORD || *tail != SLAB_CANARY_WORD)
        return -1;
    return 0;
}

/* ── Helpers numerici (no divisione, no modulo) ─────────────────────── */

/* Stampa un numero decimale su UART (senza printf) */
static void print_dec(uint64_t v)
{
    if (v == 0) { uart_putc('0'); return; }
    char buf[20];
    int  len = 0;
    while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = len - 1; i >= 0; i--) uart_putc(buf[i]);
}

/* ── Operazioni primitive del buddy ────────────────────────────────── */

/*
 * buddy_add_block — aggiunge il blocco fisico 'pa' all'ordine 'order'.
 * Usata sia nell'init (pmm_free_range) che in phys_free_page.
 * Non tenta coalescing: il chiamante lo gestisce esternamente.
 */
static void buddy_add_block(uint64_t pa, uint32_t order)
{
    uint32_t idx = (uint32_t)PA_TO_IDX(pa);
    page_state[idx] = PAGE_FREE;
    page_ord[idx]   = (uint8_t)order;

    blk_node_t *node = (blk_node_t *)(uintptr_t)pa;
    list_push(&buddy_sent[order], node);
    buddy_count[order]++;
    total_free_pages += (1U << order);
}

/*
 * buddy_remove_block — rimuove un blocco dalla free list dato il suo PA.
 * WCET: O(1) grazie alla lista doppia.
 */
static void buddy_remove_block(uint64_t pa, uint32_t order)
{
    blk_node_t *node = (blk_node_t *)(uintptr_t)pa;
    list_remove(node);
    buddy_count[order]--;
    total_free_pages -= (1U << order);

    uint32_t idx = (uint32_t)PA_TO_IDX(pa);
    page_state[idx] = PAGE_ALLOC;
    page_ord[idx]   = 0xFF;
}

/*
 * buddy_addr — calcola l'indirizzo fisico del buddy.
 * Per un blocco di 2^order pagine all'indirizzo PA:
 *   buddy_idx = page_idx XOR (1 << order)
 */
static uint64_t buddy_addr(uint64_t pa, uint32_t order)
{
    uint32_t idx       = (uint32_t)PA_TO_IDX(pa);
    uint32_t buddy_idx = idx ^ (1U << order);
    return IDX_TO_PA(buddy_idx);
}

/* ── Inizializzazione: libera un range fisico nella free list buddy ─── */

/*
 * pmm_free_range — aggiunge il range [start, end) alla free list.
 * Strategia: ad ogni passo trova il maggiore ordine con cui il blocco
 * è allineato E che non sfora 'end'. Questo produce il minimo numero
 * di blocchi e la frammentazione minima.
 *
 * WCET: O(NUM_PAGES) solo al boot, non nel hot path RT.
 */
static void pmm_free_range(uint64_t start, uint64_t end)
{
    while (start < end) {
        uint32_t idx   = (uint32_t)PA_TO_IDX(start);
        uint32_t order = 0;

        /* Trova il maggiore ordine per cui:
         * 1. idx è allineato a 2^order
         * 2. il blocco non supera 'end'                        */
        while (order < MAX_ORDER - 1) {
            uint32_t next_order = order + 1;
            uint64_t block_size = (uint64_t)PAGE_SIZE << next_order;

            /* Controlla allineamento */
            if (idx & (1U << next_order)) break;
            /* Controlla che non sfori end */
            if (start + block_size > end) break;

            order = next_order;
        }

        buddy_add_block(start, order);
        start += (uint64_t)PAGE_SIZE << order;
    }
}

/* ── API pubblica: Buddy ─────────────────────────────────────────────── */

/*
 * phys_alloc_page — alloca una singola pagina (ordine 0).
 *
 * Algoritmo:
 *   1. Se ordine 0 non è vuoto → pop, ritorna. [O(1)]
 *   2. Altrimenti cerca il minimo ordine N > 0 con blocchi liberi. [O(MAX_ORDER)]
 *   3. Splitta il blocco N in due blocchi N-1, rimette il secondo in lista.
 *   4. Ripeti dal passo 3 con N-1 fino a ordine 0.
 *
 * WCET: ≤ MAX_ORDER = 11 iterazioni — deterministico.
 */
uint64_t phys_alloc_page(void)
{
    /* Cerca il minimo ordine non vuoto */
    uint32_t order = 0;
    while (order < MAX_ORDER && list_empty(&buddy_sent[order]))
        order++;

    if (order == MAX_ORDER) {
        uart_puts("[PMM] PANIC: OOM — nessuna pagina libera\n");
        while (1) __asm__ volatile("wfe");
    }

    /* Splitta da 'order' fino a 0, reinserendo la metà superiore */
    while (order > 0) {
        blk_node_t *node = list_pop(&buddy_sent[order]);
        uint64_t pa      = (uint64_t)(uintptr_t)node;

        buddy_count[order]--;
        total_free_pages -= (1U << order);

        /* Marca la testa come non-free (sarà ri-settato se torna libera) */
        uint32_t idx = (uint32_t)PA_TO_IDX(pa);
        page_state[idx] = PAGE_ALLOC;
        page_ord[idx]   = 0xFF;

        order--;
        uint64_t half = (uint64_t)PAGE_SIZE << order;

        /* Rimetti la metà superiore nell'ordine inferiore */
        buddy_add_block(pa + half, order);
    }

    /* Ora ordine 0 ha almeno un blocco */
    blk_node_t *node = list_pop(&buddy_sent[0]);
    uint64_t pa      = (uint64_t)(uintptr_t)node;

    buddy_count[0]--;
    total_free_pages--;

    uint32_t idx = (uint32_t)PA_TO_IDX(pa);
    page_state[idx] = PAGE_ALLOC;
    page_ord[idx]   = 0xFF;
    page_refs[idx]  = 1U;

    return pa;
}

/*
 * phys_alloc_pages — alloca 2^order pagine contigue.
 * WCET: O(MAX_ORDER).
 */
uint64_t phys_alloc_pages(uint32_t order)
{
    if (order >= MAX_ORDER) {
        uart_puts("[PMM] PANIC: ordine troppo grande\n");
        while (1) __asm__ volatile("wfe");
    }

    /* Cerca il minimo ordine >= richiesto con blocchi disponibili */
    uint32_t found = order;
    while (found < MAX_ORDER && list_empty(&buddy_sent[found]))
        found++;

    if (found == MAX_ORDER) {
        uart_puts("[PMM] PANIC: OOM — nessun blocco contiguo\n");
        while (1) __asm__ volatile("wfe");
    }

    /* Splitta da 'found' fino a 'order' */
    while (found > order) {
        blk_node_t *node = list_pop(&buddy_sent[found]);
        uint64_t pa      = (uint64_t)(uintptr_t)node;

        buddy_count[found]--;
        total_free_pages -= (1U << found);

        uint32_t idx = (uint32_t)PA_TO_IDX(pa);
        page_state[idx] = PAGE_ALLOC;
        page_ord[idx]   = 0xFF;

        found--;
        buddy_add_block(pa + ((uint64_t)PAGE_SIZE << found), found);
    }

    blk_node_t *node = list_pop(&buddy_sent[order]);
    uint64_t pa      = (uint64_t)(uintptr_t)node;

    buddy_count[order]--;
    total_free_pages -= (1U << order);

    uint32_t idx = (uint32_t)PA_TO_IDX(pa);
    page_state[idx] = PAGE_ALLOC;
    page_ord[idx]   = 0xFF;
    page_refs[idx]  = 1U;

    /* Marca tutte le pagine del blocco come allocate */
    uint32_t n = 1U << order;
    for (uint32_t i = 1; i < n; i++) {
        page_state[idx + i] = PAGE_ALLOC;
        page_ord[idx + i]   = 0xFF;
        page_refs[idx + i]  = 1U;
    }

    return pa;
}

void phys_retain_page(uint64_t pa)
{
    uint32_t idx;

    if (pa < PMM_BASE || pa >= PMM_END)
        return;

    idx = (uint32_t)PA_TO_IDX(pa & PAGE_MASK);
    if (page_state[idx] == PAGE_RESERVED || page_state[idx] == PAGE_FREE)
        return;
    if (page_refs[idx] == 0U)
        page_refs[idx] = 1U;
    else
        page_refs[idx]++;
}

uint32_t phys_page_refcount(uint64_t pa)
{
    uint32_t idx;

    if (pa < PMM_BASE || pa >= PMM_END)
        return 0U;

    idx = (uint32_t)PA_TO_IDX(pa & PAGE_MASK);
    return page_refs[idx];
}

/*
 * phys_free_page — libera una pagina e tenta coalescing col buddy.
 *
 * Algoritmo di coalescing (ripetuto ≤ MAX_ORDER volte):
 *   1. Calcola buddy_pa = pa XOR (order * PAGE_SIZE)
 *   2. Se buddy è libero allo stesso ordine E non è riservato → merge:
 *      a. Rimuovi buddy dalla free list [O(1)]
 *      b. Il blocco merged inizia a min(pa, buddy_pa)
 *      c. Incrementa order, ripeti
 *   3. Aggiungi il blocco (eventualmente merged) alla free list
 *
 * WCET: ≤ MAX_ORDER = 11 iterazioni — deterministico.
 */
void phys_free_page(uint64_t pa)
{
    uint32_t order = 0;
    uint32_t idx;

    if (pa < PMM_BASE || pa >= PMM_END)
        return;

    pa &= PAGE_MASK;
    idx = (uint32_t)PA_TO_IDX(pa);

    if (page_state[idx] == PAGE_RESERVED || page_state[idx] == PAGE_FREE)
        return;

    if (page_refs[idx] > 1U) {
        page_refs[idx]--;
        return;
    }
    page_refs[idx] = 0U;

    while (order < MAX_ORDER - 1) {
        uint64_t b_pa  = buddy_addr(pa, order);
        uint32_t b_idx = (uint32_t)PA_TO_IDX(b_pa);

        /* Il buddy è fuori dalla RAM gestita? */
        if (b_pa < PMM_BASE || b_pa >= PMM_END) break;

        /* Il buddy è libero allo stesso ordine? */
        if (page_state[b_idx] != PAGE_FREE) break;
        if (page_ord[b_idx]   != order)     break;
        if (page_refs[b_idx]  != 0U)        break;
        /* Il buddy è riservato? (non dovrebbe, ma controlla) */
        if (b_pa < PMM_BASE)                break;

        /* Merge: rimuovi il buddy, prendi l'indirizzo minore */
        buddy_remove_block(b_pa, order);
        if (b_pa < pa) pa = b_pa;
        order++;
    }

    buddy_add_block(pa, order);
}

void phys_free_pages(uint64_t pa, uint32_t order)
{
    /* Per semplicità: libera le pagine una alla volta.
     * In futuro: aggiungere direttamente all'ordine corretto. */
    uint32_t n = 1U << order;
    for (uint32_t i = 0; i < n; i++) {
        phys_free_page(pa + (uint64_t)i * PAGE_SIZE);
    }
}

/* ── Slab allocator ──────────────────────────────────────────────────── */

/*
 * slab_class_for — ritorna l'indice della classe di slab per 'size'.
 * Usa la potenza di 2 successiva arrotondata al minimo della classe.
 * WCET: O(1) — confronti su array di 7 elementi.
 */
static int slab_class_for(uint32_t size)
{
    for (int i = 0; i < SLAB_NUM_CLASSES; i++) {
        if (size <= slab_usable_size(i)) return i;
    }
    return -1;  /* size > 2048: usa phys_alloc_pages */
}

/*
 * slab_refill — alloca una nuova pagina dal buddy e la divide in
 * oggetti della classe 'ci'. Aggiunge tutti gli oggetti alla free list.
 *
 * Layout della pagina slab:
 *   [0..7]            slab_hdr_t  { class_idx, pad[7] }
 *   [align..align+S-1] oggetto 0
 *   [align+S..…]       oggetto 1 …
 *
 * Gli oggetti sono allineati alla loro dimensione (power of 2).
 *
 * WCET: O(oggetti_per_pagina) — slow path, chiamare solo al boot
 *       tramite slab_warm() per garantire O(1) in runtime RT.
 */
static void slab_refill(int ci)
{
    uint64_t pa = phys_alloc_page();
    if (!pa) return;

    /* Marca la pagina come slab e scrivi l'intestazione */
    page_state[PA_TO_IDX(pa)] = PAGE_SLAB;

    slab_hdr_t *hdr = (slab_hdr_t *)(uintptr_t)pa;
    hdr->class_idx = (uint8_t)ci;
    hdr->aux       = 0;
    hdr->magic     = SLAB_MAGIC_LIVE;

    uint32_t obj_size = slab_caches[ci].obj_size;

    /* Primo oggetto: allineato a obj_size dopo l'intestazione */
    uint64_t offset = ((sizeof(slab_hdr_t) + obj_size - 1) / obj_size) * obj_size;
    uint64_t end    = pa + PAGE_SIZE;
    uint32_t added  = 0;

    while (pa + offset + obj_size <= end) {
        void *obj   = (void *)(uintptr_t)(pa + offset);
        /* I primi 8 byte dell'oggetto libero sono il puntatore "next" */
        *(void **)obj               = slab_caches[ci].free_head;
        slab_caches[ci].free_head   = obj;
        slab_caches[ci].free_count++;
        slab_caches[ci].total_count++;
        offset += obj_size;
        added++;
    }

    (void)added;
}

/* ── API pubblica: Slab ──────────────────────────────────────────────── */

/*
 * kmalloc — O(1) se la cache è calda, O(N_obj) al primo refill.
 * Per garantire O(1) sui task RT: chiamare slab_warm() al boot.
 */
void *kmalloc(uint32_t size)
{
    if (size == 0) return NULL;

    int ci = slab_class_for(size);

    /* ── Percorso slab: size <= 2048 ─────────────────────────────── */
    if (ci >= 0) {
        if (!slab_caches[ci].free_head)
            slab_refill(ci);

        if (!slab_caches[ci].free_head) {
            uart_puts("[PMM] PANIC: slab OOM per size ");
            print_dec(size);
            uart_puts(" — chiama slab_warm() al boot\n");
            while (1) __asm__ volatile("wfe");
        }

        void *raw = slab_caches[ci].free_head;
        slab_caches[ci].free_head = *(void **)raw;
        slab_caches[ci].free_count--;
        slab_write_canaries(ci, raw);
        return slab_user_from_raw(raw);
    }

    /* ── Percorso large: size > 2048, usa phys_alloc_pages ───────── *
     *
     * Layout della pagina:
     *   [0..7]   slab_hdr_t { SLAB_CLASS_LARGE, order, MAGIC, 0 }
     *   [8..]    dati utente
     *
     * RT: questo percorso NON è O(1) — chiamare solo fuori da sezioni RT.
     */
    uint32_t need  = size + (uint32_t)sizeof(slab_hdr_t);
    uint32_t pages = (need + (uint32_t)PAGE_SIZE - 1) >> PAGE_SHIFT;

    /* Calcola il minimo ordine che copre 'pages' pagine */
    uint32_t order = 0;
    while ((1U << order) < pages) order++;

    if (order >= MAX_ORDER) {
        uart_puts("[PMM] PANIC: kmalloc richiesta troppo grande\n");
        while (1) __asm__ volatile("wfe");
    }

    uint64_t pa = phys_alloc_pages(order);

    slab_hdr_t *hdr = (slab_hdr_t *)(uintptr_t)pa;
    hdr->class_idx  = SLAB_CLASS_LARGE;
    hdr->aux        = (uint8_t)order;
    hdr->magic      = SLAB_MAGIC_LIVE;
    hdr->_reserved  = 0;

    return (void *)(uintptr_t)(pa + sizeof(slab_hdr_t));
}

/*
 * kfree — O(1) per slab, O(≤MAX_ORDER) per large.
 *
 * Legge la classe e il magic dalla intestazione della pagina:
 *  - SLAB_MAGIC_FREE  → double-free → kernel panic
 *  - SLAB_MAGIC_LIVE + SLAB_CLASS_LARGE → phys_free_pages(order)
 *  - SLAB_MAGIC_LIVE + class 0..6      → push nella free list slab
 */
void kfree(void *ptr)
{
    uint8_t    *raw;
    if (!ptr) return;

    uint64_t pa     = (uint64_t)(uintptr_t)ptr & PAGE_MASK;
    slab_hdr_t *hdr = (slab_hdr_t *)(uintptr_t)pa;

    /* ── Magic check: double-free / heap overflow detector ────────── */
    if (hdr->magic == SLAB_MAGIC_FREE) {
        uart_puts("[PMM] PANIC: kfree — double-free rilevato\n");
        while (1) __asm__ volatile("wfe");
    }
    if (hdr->magic != SLAB_MAGIC_LIVE) {
        uart_puts("[PMM] PANIC: kfree — magic corrotto (heap overflow?)\n");
        while (1) __asm__ volatile("wfe");
    }

    /* ── Percorso large: libera tramite buddy ─────────────────────── */
    if (hdr->class_idx == SLAB_CLASS_LARGE) {
        uint32_t order = hdr->aux;
        hdr->magic = SLAB_MAGIC_FREE;       /* marca le pagine come liberate */
        phys_free_pages(pa, order);
        return;
    }

    /* ── Percorso slab normale ────────────────────────────────────── */
    if (hdr->class_idx >= SLAB_NUM_CLASSES) {
        uart_puts("[PMM] PANIC: kfree — class_idx invalido\n");
        while (1) __asm__ volatile("wfe");
    }

    /* La pagina rimane LIVE (altri oggetti potrebbero essere in uso).
     * Il magic viene resettato solo quando l'intera pagina slab viene
     * restituita al buddy (non implementato in v0.1). */
    int ci = hdr->class_idx;
    if (slab_validate_canaries(ptr, ci) < 0)
        kdebug_panic("heap/slab canary corrotto");

    raw = slab_raw_from_user_ptr(ptr);
    *(void **)raw               = slab_caches[ci].free_head;
    slab_caches[ci].free_head   = raw;
    slab_caches[ci].free_count++;
}

int pmm_debug_check_ptr(const void *ptr)
{
    uint64_t          pa;
    const slab_hdr_t *hdr;

    if (!ptr)
        return -1;

    pa = (uint64_t)(uintptr_t)ptr & PAGE_MASK;
    hdr = (const slab_hdr_t *)(uintptr_t)pa;

    if (hdr->magic != SLAB_MAGIC_LIVE)
        return -1;
    if (hdr->class_idx == SLAB_CLASS_LARGE)
        return 0;
    if (hdr->class_idx >= SLAB_NUM_CLASSES)
        return -1;

    return slab_validate_canaries(ptr, hdr->class_idx);
}

/*
 * slab_warm — pre-popola la cache fino a 'count' oggetti liberi.
 * Chiamare al boot per ogni struttura usata nei task RT.
 */
void slab_warm(uint32_t size, uint32_t count)
{
    int ci = slab_class_for(size);
    if (ci < 0) return;

    while (slab_caches[ci].free_count < count)
        slab_refill(ci);
}

/* ── Diagnostica ─────────────────────────────────────────────────────── */

void pmm_stats(void)
{
    uart_puts("[PMM] ── Statistiche memoria ──────────────────────\n");

    uint64_t free_bytes  = (uint64_t)total_free_pages * PAGE_SIZE;
    uint64_t total_bytes = PMM_SIZE;

    uart_puts("[PMM]   RAM totale : "); print_dec(total_bytes >> 20); uart_puts(" MB\n");
    uart_puts("[PMM]   Libera     : "); print_dec(free_bytes  >> 20); uart_puts(" MB (");
    print_dec(total_free_pages); uart_puts(" pagine)\n");
    uart_puts("[PMM]   Usata      : ");
    print_dec((total_bytes - free_bytes) >> 10); uart_puts(" KB\n");

    uart_puts("[PMM]   Buddy free list per ordine:\n");
    for (uint32_t o = 0; o < MAX_ORDER; o++) {
        if (buddy_count[o] == 0) continue;
        uart_puts("[PMM]     ordine ");
        uart_putc('0' + (int)(o / 10));
        uart_putc('0' + (int)(o % 10));
        uart_puts(" (");
        print_dec(PAGE_SIZE >> 10 << o); uart_puts(" KB): ");
        print_dec(buddy_count[o]); uart_puts(" blocchi\n");
    }

    uart_puts("[PMM]   Slab cache:\n");
    for (int ci = 0; ci < SLAB_NUM_CLASSES; ci++) {
        if (slab_caches[ci].total_count == 0) continue;
        uart_puts("[PMM]     ");
        print_dec(slab_sizes[ci]);
        uart_puts("B: ");
        print_dec(slab_caches[ci].free_count);
        uart_puts(" liberi / ");
        print_dec(slab_caches[ci].total_count);
        uart_puts(" totali\n");
    }
    uart_puts("[PMM] ─────────────────────────────────────────────\n");
}

/* ── Inizializzazione ─────────────────────────────────────────────────── */

/*
 * Simboli esportati dal linker script:
 *   __heap_start — prima pagina libera dopo stack kernel
 */
extern uint8_t __heap_start;

void pmm_init(void)
{
    uart_puts("[PMM] Inizializzazione allocatori buddy + slab...\n");

    /* 1. Inizializza le sentinelle delle free list */
    for (int i = 0; i < MAX_ORDER; i++)
        list_init(&buddy_sent[i]);

    /* 2. Segna le pagine del kernel come RESERVED
     *    Da PMM_BASE fino a __heap_start (esclusa) */
    uint64_t kernel_end = (uint64_t)(uintptr_t)&__heap_start;
    uint32_t reserved_pages = (uint32_t)PA_TO_IDX(kernel_end);
    for (uint32_t i = 0; i < reserved_pages; i++)
        page_state[i] = PAGE_RESERVED;

    uart_puts("[PMM]   Pagine riservate (kernel): ");
    print_dec(reserved_pages);
    uart_puts(" (");
    print_dec((uint64_t)reserved_pages * PAGE_SIZE >> 10);
    uart_puts(" KB, 0x40000000–0x");
    /* Stampa hex semplificata: solo la parte variabile */
    uint32_t top = reserved_pages * (uint32_t)(PAGE_SIZE >> 12);
    (void)top;
    uart_puts("...)\n");

    /* 3. Aggiungi la RAM libera alla free list buddy */
    pmm_free_range(kernel_end, PMM_END);

    uart_puts("[PMM]   Pagine libere aggiunte al buddy: ");
    print_dec(total_free_pages);
    uart_puts(" (");
    print_dec((uint64_t)total_free_pages * PAGE_SIZE >> 20);
    uart_puts(" MB)\n");

    /* 4. Inizializza le slab cache */
    for (int ci = 0; ci < SLAB_NUM_CLASSES; ci++) {
        slab_caches[ci].obj_size    = slab_sizes[ci];
        slab_caches[ci].free_head   = NULL;
        slab_caches[ci].free_count  = 0;
        slab_caches[ci].total_count = 0;
    }

    /* 5. Pre-popola le slab cache per le strutture kernel critiche:
     *    task_t (~64B), port_t (~44B), ipc_message_t (~268B)
     *    Garantisce O(1) per le prime N allocazioni RT.          */
    slab_warm(64,  256);    /* task_t, port_t → classe 64B, 256 oggetti */
    slab_warm(512, 128);    /* ipc_message_t  → classe 512B, 128 oggetti */
    slab_warm(128, 64);     /* uso generico   → classe 128B, 64 oggetti  */

    uart_puts("[PMM] Buddy + Slab pronti\n");
    pmm_stats();
}
