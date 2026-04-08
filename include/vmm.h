/*
 * EnlilOS Microkernel — Virtual Memory Area (M8-02)
 *
 * Gestione VMA (vm_area_t) per mmap() file-backed.
 * Separato da mmu.c per mantenere la responsabilità chiara:
 *   mmu.c  — page table, TLB, mapping fisico
 *   vmm.c  — metadati logici delle mappature file-backed
 */

#ifndef ENLILOS_VMM_H
#define ENLILOS_VMM_H

#include "types.h"
#include "vfs.h"
#include "mmu.h"

/* Numero massimo di VMA file-backed per task */
#define VMA_MAX_PER_TASK    16U

/* Flag VMA */
#define VMA_FLAG_SHARED     (1U << 0)   /* MAP_SHARED — write-back su msync/munmap */
#define VMA_FLAG_WRITE      (1U << 1)   /* PROT_WRITE — mappatura scrivibile       */
#define VMA_FLAG_EXEC       (1U << 2)   /* PROT_EXEC                               */

/*
 * vm_area_t — descrittore di una mappatura file-backed.
 *
 * Registrata da sys_mmap() quando il fd è valido (non MAP_ANONYMOUS).
 * Usata da sys_msync() e sys_munmap() per il write-back delle pagine sporche.
 */
typedef struct {
    uintptr_t   start;          /* VA page-aligned del primo byte */
    size_t      size;           /* lunghezza in byte, page-aligned */
    uint32_t    flags;          /* VMA_FLAG_* */
    uint64_t    file_offset;    /* offset nel file corrispondente a start */
    vfs_file_t  file;           /* copia del handle VFS (per read-back/write-back) */
    uint8_t     active;         /* 1 = slot in uso */
    uint8_t     _pad[7];
} vm_area_t;

/* ── API pubblica ─────────────────────────────────────────────────── */

/* Inizializza il pool (chiamata una volta al boot). */
void        vmm_init(void);

/*
 * vmm_map_file — registra una VMA file-backed per il task 'pid'.
 *
 * Ritorna 0 se OK, -ENOMEM se il pool è esaurito.
 * Non alloca pagine fisiche — quello è compito di mmu_map_user_anywhere().
 */
int         vmm_map_file(uint32_t pid, uintptr_t va, size_t size,
                         uint32_t flags, uint64_t file_offset,
                         const vfs_file_t *file);

/*
 * vmm_find — trova la VMA che contiene 'va' per il task 'pid'.
 * Ritorna NULL se non trovata.
 */
vm_area_t  *vmm_find(uint32_t pid, uintptr_t va);

/*
 * vmm_msync — scrive le pagine della VMA [va, va+size) nel file di backing.
 * Solo per VMA con VMA_FLAG_SHARED | VMA_FLAG_WRITE.
 * 'space' è lo spazio di indirizzamento del task (per risolvere VA → PA).
 */
int         vmm_msync(uint32_t pid, mm_space_t *space,
                      uintptr_t va, size_t size);

/*
 * vmm_unmap_range — rimuove la VMA che copre 'va'.
 * Se MAP_SHARED scrivibile, scrive le pagine sporche prima della rimozione.
 */
int         vmm_unmap_range(uint32_t pid, mm_space_t *space,
                            uintptr_t va, size_t size);

/*
 * vmm_cleanup_task — rimuove tutte le VMA del task 'pid'.
 * Chiamata da sched_task_exit_with_code() (tramite syscall exit).
 * Non esegue write-back (le risorse sono già liberate).
 */
void        vmm_cleanup_task(uint32_t pid);

#endif /* ENLILOS_VMM_H */
