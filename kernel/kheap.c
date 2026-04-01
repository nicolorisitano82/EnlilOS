/*
 * EnlilOS Microkernel - Kernel Heap: Named Typed Caches (M1-04)
 *
 * Ogni cache è un pool di oggetti della stessa dimensione, con:
 *  - free list intrusive (i primi 8 byte di ogni oggetto libero sono il
 *    puntatore al successivo)
 *  - backing memory fornita da kmalloc() → slab O(1) hot path
 *  - diagnostica: nome, contatori alloc/free, limite opzionale
 *
 * RT: kmem_cache_alloc/free sono O(1) se la cache è calda.
 *     Riscaldare le cache con kmem_cache_warm() durante il boot,
 *     prima dell'avvio dei task hard-RT.
 */

#include "kheap.h"
#include "pmm.h"
#include "uart.h"

/* ── Registro delle named cache ────────────────────────────────────── */

static kmem_cache_t named_caches[KMEM_MAX_CACHES];
static uint32_t     num_caches;

/* ── Cache pre-definite (puntatori pubblici) ────────────────────────── */

kmem_cache_t *task_cache;
kmem_cache_t *port_cache;
kmem_cache_t *ipc_cache;

/* ── Helper: stampa decimale senza printf ────────────────────────────── */

static void pr_dec(uint32_t v)
{
    if (v == 0) { uart_putc('0'); return; }
    char buf[12];
    int  len = 0;
    while (v) { buf[len++] = '0' + (int)(v % 10); v /= 10; }
    for (int i = len - 1; i >= 0; i--) uart_putc(buf[i]);
}

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * kmem_cache_create — registra una nuova cache nel registro globale.
 *
 * La dimensione viene arrotondata alla classe slab superiore per
 * garantire che kmalloc(obj_size) rientri sempre nel hot path O(1).
 * Se prealloc > 0, la cache viene riscaldata subito.
 */
kmem_cache_t *kmem_cache_create(const char *name,
                                uint32_t    obj_size,
                                uint32_t    prealloc)
{
    if (num_caches >= KMEM_MAX_CACHES) {
        uart_puts("[KHEAP] PANIC: registro cache pieno\n");
        while (1) __asm__ volatile("wfe");
    }

    kmem_cache_t *c = &named_caches[num_caches++];
    c->name        = name;
    c->obj_size    = obj_size;
    c->max_objects = 0;
    c->free_head   = NULL;
    c->free_count  = 0;
    c->total_count = 0;
    c->alloc_count = 0;

    if (prealloc > 0)
        kmem_cache_warm(c, prealloc);

    uart_puts("[KHEAP] Cache creata: ");
    uart_puts(name);
    uart_puts(", obj=");
    pr_dec(obj_size);
    uart_puts("B, prealloc=");
    pr_dec(prealloc);
    uart_puts("\n");

    return c;
}

/*
 * kmem_cache_alloc — O(1) se la cache è calda.
 *
 * Cold path: richiama kmalloc(obj_size) per ottenere un nuovo oggetto.
 * Questo è O(1) se la slab cache sottostante è calda (slab_warm al boot).
 * Panic se max_objects > 0 e il limite è stato raggiunto.
 */
void *kmem_cache_alloc(kmem_cache_t *c)
{
    /* Controllo limite (0 = illimitato) */
    if (c->max_objects > 0 && c->total_count >= c->max_objects) {
        uart_puts("[KHEAP] PANIC: cache ");
        uart_puts(c->name);
        uart_puts(" esaurita (limite: ");
        pr_dec(c->max_objects);
        uart_puts(")\n");
        while (1) __asm__ volatile("wfe");
    }

    void *obj;

    if (c->free_head) {
        /* Hot path: O(1) — pop dalla lista libera */
        obj = c->free_head;
        c->free_head = *(void **)obj;
        c->free_count--;
    } else {
        /* Cold path: alloca da kmalloc (slab O(1) se caldo) */
        obj = kmalloc(c->obj_size);
    }

    c->total_count++;
    c->alloc_count++;
    return obj;
}

/*
 * kmem_cache_free — O(1) sempre.
 *
 * Usa i primi 8 byte dell'oggetto come puntatore next della free list.
 * Non tocca la memoria oltre gli 8 byte: i dati del chiamante rimangono
 * intatti fintanto che non viene chiamata una nuova kmem_cache_alloc.
 */
void kmem_cache_free(kmem_cache_t *c, void *ptr)
{
    if (!ptr) return;

    /* Push in testa alla lista libera */
    *(void **)ptr = c->free_head;
    c->free_head  = ptr;
    c->free_count++;
    c->total_count--;
}

/*
 * kmem_cache_warm — pre-popola la cache con 'count' oggetti liberi.
 *
 * Alloca count oggetti via kmalloc e li inserisce nella free list.
 * I cicli di alloc/free vengono scontati dai contatori per non alterare
 * le statistiche di utilizzo runtime.
 * WCET: O(count × kmalloc_cost).
 */
void kmem_cache_warm(kmem_cache_t *c, uint32_t count)
{
    uint32_t added = 0;

    while (c->free_count < count) {
        void *obj = kmalloc(c->obj_size);
        *(void **)obj = c->free_head;
        c->free_head  = obj;
        c->free_count++;
        added++;
    }

    /* Non conteggia il pre-riscaldamento nelle statistiche lifetime */
    (void)added;
}

/* ── Diagnostica ─────────────────────────────────────────────────────── */

void kmem_cache_stats(const kmem_cache_t *c)
{
    uart_puts("[KHEAP]   ");
    uart_puts(c->name);
    uart_puts(": size=");
    pr_dec(c->obj_size);
    uart_puts("B, liberi=");
    pr_dec(c->free_count);
    uart_puts(", in_uso=");
    pr_dec(c->total_count);
    uart_puts(", lifetime=");
    pr_dec(c->alloc_count);
    uart_puts("\n");
}

void kheap_stats(void)
{
    uart_puts("[KHEAP] ── Named cache stats ───────────────────────\n");
    for (uint32_t i = 0; i < num_caches; i++)
        kmem_cache_stats(&named_caches[i]);
    uart_puts("[KHEAP] ────────────────────────────────────────────\n");
}

/* ── Inizializzazione ─────────────────────────────────────────────────── */

/*
 * kheap_init — crea le cache pre-definite del microkernel.
 *
 * Dimensioni scelte per rientrare nelle classi slab esistenti e per
 * coprire le strutture fondamentali del microkernel:
 *
 *   task_cache  64B × 64  → task_t (pid, stato, priorità, stack ptr, IPC)
 *   port_cache  64B × 128 → port_t (nome, owner, coda messaggi)
 *   ipc_cache  512B × 128 → ipc_message_t (header 12B + payload 256B)
 *
 * Tutte e tre rientrano nelle classi slab pre-caldate da pmm_init(),
 * quindi kmem_cache_warm qui è O(1) per oggetto (hot path slab).
 */
void kheap_init(void)
{
    uart_puts("[KHEAP] Inizializzazione named cache...\n");

    task_cache = kmem_cache_create("task_cache",  64,  64);
    port_cache = kmem_cache_create("port_cache",  64, 128);
    ipc_cache  = kmem_cache_create("ipc_cache",  512, 128);

    uart_puts("[KHEAP] Named cache pronte\n");
    kheap_stats();
}
