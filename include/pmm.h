/*
 * EnlilOS Microkernel - Physical Memory Manager (M1-03)
 *
 * Due allocatori con WCET diversi, scelti in base al contesto:
 *
 *  BUDDY ALLOCATOR  — granularità pagina (4KB–4MB)
 *  ─────────────────────────────────────────────────────────────────
 *  Struttura: 11 ordini (0=4KB … 10=4MB), free list circolare per ordine.
 *  phys_alloc_page()        O(≤ MAX_ORDER=11)  ← hot path RT
 *  phys_alloc_pages(n)      O(≤ MAX_ORDER)
 *  phys_free_page(pa)       O(≤ MAX_ORDER)  con coalescing buddy
 *
 *  SLAB ALLOCATOR  — oggetti kernel (32B–2048B)
 *  ─────────────────────────────────────────────────────────────────
 *  Struttura: 7 classi di dimensione (32,64,128,256,512,1024,2048).
 *  Free list intrusive per classe; intestazione di pagina per kfree.
 *  kmalloc(size)   O(1)  ← hot path RT (lista pre-calda)
 *  kfree(ptr)      O(1)  ← sempre
 *  slab_warm(sz,n) O(n)  ← chiamare a boot, fuori da sezioni RT
 */

#ifndef ENLILOS_PMM_H
#define ENLILOS_PMM_H

#include "types.h"

/* ── Costanti globali ──────────────────────────────────────────────── */

#define PAGE_SIZE       4096UL
#define PAGE_SHIFT      12
#define PAGE_MASK       (~(PAGE_SIZE - 1))

/* RAM fisica: QEMU virt, 512 MB a partire da 0x40000000 */
#define PMM_BASE        0x40000000UL
#define PMM_END         0x60000000UL
#define PMM_SIZE        (PMM_END - PMM_BASE)        /* 512 MB */
#define NUM_PAGES       (PMM_SIZE / PAGE_SIZE)      /* 131072 */

/* Indici pagina */
#define PA_TO_IDX(pa)   (((pa) - PMM_BASE) >> PAGE_SHIFT)
#define IDX_TO_PA(i)    (PMM_BASE + ((uint64_t)(i) << PAGE_SHIFT))

/* Ordini buddy: 0=1pag … 10=1024pag */
#define MAX_ORDER       11

/* ── Stato di ciascuna pagina fisica ───────────────────────────────── */
#define PAGE_FREE       0x00    /* nella free list del buddy          */
#define PAGE_ALLOC      0x01    /* allocata (buddy o slab refill)     */
#define PAGE_RESERVED   0x02    /* kernel image, stack, tabelle MMU   */
#define PAGE_SLAB       0x03    /* allocata a una slab cache          */

/* ── API Buddy ─────────────────────────────────────────────────────── */

/*
 * pmm_init() — inizializza buddy e slab; marca le pagine riservate;
 * aggiunge la RAM libera alle free list; pre-popola le slab cache.
 * Chiamare una sola volta al boot, dopo mmu_init().
 */
void pmm_init(void);

/*
 * phys_alloc_page() — alloca una singola pagina fisica (4KB).
 * WCET: ≤ MAX_ORDER=11 iterazioni (split da ordine superiore).
 * Ritorna l'indirizzo fisico, o 0 in caso di OOM (kernel panic).
 */
uint64_t phys_alloc_page(void);

/*
 * phys_alloc_pages(n) — alloca 2^order pagine contigue (n deve essere
 * potenza di 2). Utile per page table, DMA buffer, stack task.
 * WCET: O(MAX_ORDER).
 */
uint64_t phys_alloc_pages(uint32_t order);
void     phys_retain_page(uint64_t pa);
uint32_t phys_page_refcount(uint64_t pa);

/*
 * phys_free_page(pa) — libera una pagina precedentemente allocata.
 * Tenta il coalescing con il buddy fino all'ordine massimo.
 * WCET: O(MAX_ORDER).
 */
void phys_free_page(uint64_t pa);

/*
 * phys_free_pages(pa, order) — libera un blocco di 2^order pagine.
 */
void phys_free_pages(uint64_t pa, uint32_t order);

/* ── API Slab (kmalloc/kfree) ──────────────────────────────────────── */

/*
 * kmalloc(size) — alloca un oggetto di dimensione ≤ 2048 byte.
 * La dimensione viene arrotondata alla classe di slab successiva.
 * WCET hot path (cache calda): O(1).
 * WCET slow path (slab refill): O(oggetti_per_pagina) — solo al boot.
 * Ritorna NULL se size > 2048 (usa phys_alloc_pages per blocchi grandi).
 */
void *kmalloc(uint32_t size);

/*
 * kfree(ptr) — libera un oggetto allocato con kmalloc.
 * Legge la classe dalla intestazione della pagina slab.
 * WCET: O(1) — sempre.
 */
void kfree(void *ptr);

/*
 * pmm_debug_check_ptr(ptr) — verifica magic/canary dell'allocazione
 * senza liberarla. Ritorna 0 se valida, negativo in caso di corruzione.
 */
int pmm_debug_check_ptr(const void *ptr);

/*
 * slab_warm(size, count) — pre-popola la slab cache per la classe
 * corrispondente a 'size' con almeno 'count' oggetti liberi.
 * Chiamare al boot, fuori da sezioni real-time.
 * Garantisce che le prime 'count' chiamate a kmalloc(size) siano O(1).
 */
void slab_warm(uint32_t size, uint32_t count);

/* ── Diagnostica ────────────────────────────────────────────────────── */
void pmm_stats(void);

#endif
