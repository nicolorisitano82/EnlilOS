/*
 * NROS Microkernel - Kernel Heap: Named Typed Caches (M1-04)
 *
 * Livello sopra il slab allocator di pmm.c.
 * Ogni cache ha un nome, una dimensione fissa, e una propria free list.
 *
 * Design RT:
 *  - kmem_cache_alloc()  O(1) se cache calda, O(≤MAX_ORDER) al refill
 *  - kmem_cache_free()   O(1) — sempre (push su lista intrusive)
 *  - kmem_cache_warm()   O(n) — chiamare al boot, fuori da sezioni RT
 *
 * Struttura interna:
 *  - ogni oggetto libero usa i propri primi 8 byte come puntatore next
 *  - nessun overhead per oggetto allocato (zero byte aggiunti)
 *  - la backing memory viene da kmalloc() (→ slab, O(1) hot path)
 */

#ifndef NROS_KHEAP_H
#define NROS_KHEAP_H

#include "types.h"

/* Numero massimo di named cache registrate */
#define KMEM_MAX_CACHES     16

/*
 * kmem_cache_t — descrittore di una named typed cache.
 *
 * Campi diagnostici (alloc_count, max_objects) utili per:
 *  - verificare che nessun task RT esaurisca la cache
 *  - rilevare leak (alloc_count cresce senza kfree corrispondenti)
 */
typedef struct {
    const char *name;           /* stringa di debug, es. "task_cache"  */
    uint32_t    obj_size;       /* dimensione oggetto (arrotondata)     */
    uint32_t    max_objects;    /* limite: 0 = illimitato               */
    void       *free_head;      /* lista libera intrusive               */
    uint32_t    free_count;     /* oggetti liberi in cache              */
    uint32_t    total_count;    /* oggetti totali allocati alla cache   */
    uint32_t    alloc_count;    /* contatore lifetime di alloc          */
} kmem_cache_t;

/* ── API ─────────────────────────────────────────────────────────────── */

/*
 * kheap_init() — crea le cache pre-definite del microkernel.
 * Chiamare dopo pmm_init(), prima di mk_init().
 *
 *   task_cache  — strutture task_t        (~64B,  64 oggetti pre-caldi)
 *   port_cache  — strutture port_t        (~64B, 128 oggetti pre-caldi)
 *   ipc_cache   — strutture ipc_message_t (~512B, 128 oggetti pre-caldi)
 */
void kheap_init(void);

/*
 * kmem_cache_create(name, obj_size, prealloc) — registra una nuova cache.
 * Ritorna il puntatore alla cache, o NULL se il registro è pieno.
 * max_objects = 0 → illimitato.
 * prealloc > 0   → riscalda la cache con 'prealloc' oggetti al boot.
 */
kmem_cache_t *kmem_cache_create(const char *name,
                                uint32_t    obj_size,
                                uint32_t    prealloc);

/*
 * kmem_cache_alloc(c) — alloca un oggetto dalla cache 'c'.
 * WCET hot path: O(1).
 * WCET cold path (refill): O(≤MAX_ORDER) — solo se cache esaurita.
 * Kernel panic se max_objects > 0 e il limite è raggiunto.
 */
void *kmem_cache_alloc(kmem_cache_t *c);

/*
 * kmem_cache_free(c, ptr) — restituisce un oggetto alla cache.
 * WCET: O(1) — sempre.
 */
void kmem_cache_free(kmem_cache_t *c, void *ptr);

/*
 * kmem_cache_warm(c, count) — pre-popola la cache fino a 'count' oggetti.
 * Chiamare al boot per garantire O(1) nelle sezioni RT.
 * WCET: O(count × alloc_cost).
 */
void kmem_cache_warm(kmem_cache_t *c, uint32_t count);

/*
 * kmem_cache_stats(c) — stampa statistiche della cache su UART.
 */
void kmem_cache_stats(const kmem_cache_t *c);

/*
 * kheap_stats() — stampa statistiche di tutte le cache su UART.
 */
void kheap_stats(void);

/* ── Cache pre-definite del microkernel ──────────────────────────────── */

extern kmem_cache_t *task_cache;    /* strutture task_t                 */
extern kmem_cache_t *port_cache;    /* strutture IPC port_t             */
extern kmem_cache_t *ipc_cache;     /* strutture ipc_message_t          */

#endif /* NROS_KHEAP_H */
