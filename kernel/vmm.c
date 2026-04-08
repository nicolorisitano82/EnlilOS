/*
 * EnlilOS Microkernel — Virtual Memory Areas (M8-02)
 *
 * Gestione metadati delle mappature file-backed:
 *   - Pool statico vm_area_t[SCHED_MAX_TASKS][VMA_MAX_PER_TASK]
 *   - Write-back pagine sporche su msync() e munmap() MAP_SHARED
 *
 * Principi RT:
 *   - Nessuna allocazione dinamica: pool statico pre-dimensionato
 *   - vmm_find(): O(VMA_MAX_PER_TASK) = O(16), accettabile fuori dal fast path
 *   - vmm_msync(): O(pages × VFS latency) — mai chiamata da task hard-RT
 */

#include "vmm.h"
#include "pmm.h"
#include "sched.h"
#include "uart.h"

/* ── Pool statico ─────────────────────────────────────────────────── */

static vm_area_t vma_pool[SCHED_MAX_TASKS][VMA_MAX_PER_TASK];

/* ── Utilità interne ──────────────────────────────────────────────── */

/* Indice nel pool per pid (wrapping se > pool size). */
static uint32_t pid_to_idx(uint32_t pid)
{
    return pid % (uint32_t)SCHED_MAX_TASKS;
}

static void vma_zero(vm_area_t *v)
{
    v->active      = 0U;
    v->start       = 0U;
    v->size        = 0U;
    v->flags       = 0U;
    v->file_offset = 0U;
}

/* ── API pubblica ─────────────────────────────────────────────────── */

void vmm_init(void)
{
    for (uint32_t i = 0U; i < (uint32_t)SCHED_MAX_TASKS; i++)
        for (uint32_t j = 0U; j < VMA_MAX_PER_TASK; j++)
            vma_zero(&vma_pool[i][j]);

    uart_puts("[VMM] Virtual Memory Area manager inizializzato\n");
}

int vmm_map_file(uint32_t pid, uintptr_t va, size_t size,
                 uint32_t flags, uint64_t file_offset,
                 const vfs_file_t *file)
{
    uint32_t idx = pid_to_idx(pid);
    vm_area_t *slot = NULL;

    if (!file || size == 0U) return -EINVAL;

    for (uint32_t j = 0U; j < VMA_MAX_PER_TASK; j++) {
        if (!vma_pool[idx][j].active) {
            slot = &vma_pool[idx][j];
            break;
        }
    }
    if (!slot) return -ENOMEM;

    slot->start       = va;
    slot->size        = size;
    slot->flags       = flags;
    slot->file_offset = file_offset;
    slot->file        = *file;   /* shallow copy del handle VFS */
    slot->active      = 1U;
    return 0;
}

vm_area_t *vmm_find(uint32_t pid, uintptr_t va)
{
    uint32_t idx = pid_to_idx(pid);

    for (uint32_t j = 0U; j < VMA_MAX_PER_TASK; j++) {
        vm_area_t *v = &vma_pool[idx][j];
        if (!v->active) continue;
        if (va >= v->start && va < v->start + v->size)
            return v;
    }
    return NULL;
}

/*
 * vmm_msync — scrive le pagine della VMA nel file di backing.
 *
 * Per ogni pagina nell'intervallo [va_start, va_start+size):
 *   1. Risolve VA → kernel pointer tramite mmu_space_resolve_ptr()
 *   2. Setta file.pos = file_offset + (page_va - vma.start)
 *   3. vfs_write() del contenuto della pagina
 */
int vmm_msync(uint32_t pid, mm_space_t *space, uintptr_t va, size_t size)
{
    vm_area_t *vma;
    uintptr_t  sync_start, sync_end, cur;
    vfs_file_t f;

    if (!space || size == 0U) return -EINVAL;

    vma = vmm_find(pid, va);
    if (!vma) return -EINVAL;

    /* Solo MAP_SHARED scrivibile ha bisogno di write-back */
    if (!(vma->flags & VMA_FLAG_SHARED) || !(vma->flags & VMA_FLAG_WRITE))
        return 0;

    /* Clamp all'intervallo della VMA */
    sync_start = va > vma->start ? va : vma->start;
    sync_end   = (va + size) < (vma->start + vma->size)
                 ? (va + size) : (vma->start + vma->size);

    f = vma->file;

    for (cur = sync_start; cur < sync_end; cur += PAGE_SIZE) {
        uint64_t page_off = vma->file_offset + (cur - vma->start);
        void    *kva;

        kva = mmu_space_resolve_ptr(space, cur, PAGE_SIZE);
        if (!kva) continue;

        f.pos = page_off;
        (void)vfs_write(&f, kva, PAGE_SIZE);
    }

    return 0;
}

int vmm_unmap_range(uint32_t pid, mm_space_t *space, uintptr_t va, size_t size)
{
    uint32_t idx = pid_to_idx(pid);
    (void)size;

    for (uint32_t j = 0U; j < VMA_MAX_PER_TASK; j++) {
        vm_area_t *v = &vma_pool[idx][j];

        if (!v->active) continue;
        if (va < v->start || va >= v->start + v->size) continue;

        /* Write-back se MAP_SHARED prima di rimuovere la VMA */
        (void)vmm_msync(pid, space, v->start, v->size);
        vma_zero(v);
        return 0;
    }
    return -EINVAL;
}

void vmm_cleanup_task(uint32_t pid)
{
    uint32_t idx = pid_to_idx(pid);

    for (uint32_t j = 0U; j < VMA_MAX_PER_TASK; j++)
        vma_zero(&vma_pool[idx][j]);
}
