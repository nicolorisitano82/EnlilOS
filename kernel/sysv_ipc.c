#include "sysv_ipc.h"

#include "mmu.h"
#include "pmm.h"
#include "sched.h"
#include "signal.h"
#include "syscall.h"
#include "timer.h"

extern void *memset(void *dst, int value, size_t n);

#define SYSV_SHM_MAX            64U
#define SYSV_SHM_ATTACH_MAX     256U
#define SYSV_SEMSET_MAX         32U
#define SYSV_SEM_VALUE_MAX      32767

typedef struct {
    uint8_t   in_use;
    uint8_t   marked_delete;
    uint16_t  seq;
    uint32_t  key;
    uint32_t  pages;
    uint32_t  attach_count;
    uint64_t  base_pa;
    size_t    size;
    size_t    aligned_size;
    uint8_t   order;
    uint8_t   _pad[7];
} sysv_shm_seg_t;

typedef struct {
    uint8_t   in_use;
    uint8_t   _pad0[3];
    uint32_t  proc_slot;
    uint32_t  seg_slot;
    uintptr_t addr;
    size_t    size;
} sysv_shm_attach_t;

typedef struct {
    uint8_t   in_use;
    uint8_t   marked_delete;
    uint16_t  seq;
    uint32_t  key;
    uint32_t  nsems;
    uint32_t  waiter_count;
    int32_t   values[SYSV_SEM_PER_SET_MAX];
} sysv_semset_t;

static sysv_shm_seg_t    shm_table[SYSV_SHM_MAX];
static sysv_shm_attach_t shm_attach_table[SYSV_SHM_ATTACH_MAX];
static sysv_semset_t     sem_table[SYSV_SEMSET_MAX];

static inline uint64_t sysv_irq_save(void)
{
    uint64_t flags;

    __asm__ volatile(
        "mrs %0, daif\n"
        "msr daifset, #2\n"
        : "=r"(flags) :: "memory");
    return flags;
}

static inline void sysv_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

static uint32_t sysv_pages_to_order(uint32_t pages)
{
    uint32_t order = 0U;

    if (pages == 0U)
        return MAX_ORDER;
    while (order < MAX_ORDER && (1U << order) < pages)
        order++;
    return order;
}

static int sysv_shm_id_from_slot(uint32_t slot, uint16_t seq)
{
    return (int)(((uint32_t)(seq + 1U) << 16) | ((slot + 1U) & 0xFFFFU));
}

static int sysv_sem_id_from_slot(uint32_t slot, uint16_t seq)
{
    return (int)(((uint32_t)(seq + 1U) << 16) | ((slot + 1U) & 0xFFFFU));
}

static int sysv_shm_slot_from_id(int shmid, uint32_t *slot_out)
{
    uint32_t slot;
    uint32_t seq;

    if (shmid <= 0 || !slot_out)
        return -EINVAL;

    slot = ((uint32_t)shmid & 0xFFFFU);
    seq  = (((uint32_t)shmid >> 16) & 0xFFFFU);
    if (slot == 0U || seq == 0U)
        return -EINVAL;
    slot--;
    seq--;
    if (slot >= SYSV_SHM_MAX)
        return -EINVAL;
    if (!shm_table[slot].in_use || shm_table[slot].seq != (uint16_t)seq)
        return -EINVAL;
    *slot_out = slot;
    return 0;
}

static int sysv_sem_slot_from_id(int semid, uint32_t *slot_out)
{
    uint32_t slot;
    uint32_t seq;

    if (semid <= 0 || !slot_out)
        return -EINVAL;

    slot = ((uint32_t)semid & 0xFFFFU);
    seq  = (((uint32_t)semid >> 16) & 0xFFFFU);
    if (slot == 0U || seq == 0U)
        return -EINVAL;
    slot--;
    seq--;
    if (slot >= SYSV_SEMSET_MAX)
        return -EINVAL;
    if (!sem_table[slot].in_use || sem_table[slot].seq != (uint16_t)seq)
        return -EINVAL;
    *slot_out = slot;
    return 0;
}

static int sysv_find_shm_by_key(uint32_t key)
{
    for (uint32_t i = 0U; i < SYSV_SHM_MAX; i++) {
        if (shm_table[i].in_use &&
            !shm_table[i].marked_delete &&
            shm_table[i].key == key)
            return (int)i;
    }
    return -1;
}

static int sysv_find_sem_by_key(uint32_t key)
{
    for (uint32_t i = 0U; i < SYSV_SEMSET_MAX; i++) {
        if (sem_table[i].in_use &&
            !sem_table[i].marked_delete &&
            sem_table[i].key == key)
            return (int)i;
    }
    return -1;
}

static int sysv_alloc_attach_record(uint32_t proc_slot, uint32_t seg_slot,
                                    uintptr_t addr, size_t size)
{
    for (uint32_t i = 0U; i < SYSV_SHM_ATTACH_MAX; i++) {
        if (shm_attach_table[i].in_use)
            continue;
        shm_attach_table[i].in_use = 1U;
        shm_attach_table[i].proc_slot = proc_slot;
        shm_attach_table[i].seg_slot = seg_slot;
        shm_attach_table[i].addr = addr;
        shm_attach_table[i].size = size;
        return 0;
    }
    return -ENOSPC;
}

static int sysv_find_attach(uint32_t proc_slot, uintptr_t addr)
{
    for (uint32_t i = 0U; i < SYSV_SHM_ATTACH_MAX; i++) {
        if (!shm_attach_table[i].in_use)
            continue;
        if (shm_attach_table[i].proc_slot == proc_slot &&
            shm_attach_table[i].addr == addr)
            return (int)i;
    }
    return -1;
}

static void sysv_try_free_shm_slot(uint32_t slot)
{
    sysv_shm_seg_t *seg;

    if (slot >= SYSV_SHM_MAX)
        return;
    seg = &shm_table[slot];
    if (!seg->in_use || !seg->marked_delete || seg->attach_count != 0U)
        return;

    if (seg->base_pa != 0ULL)
        phys_free_pages(seg->base_pa, seg->order);
    memset(seg, 0, sizeof(*seg));
}

static void sysv_try_free_sem_slot(uint32_t slot)
{
    sysv_semset_t *set;

    if (slot >= SYSV_SEMSET_MAX)
        return;
    set = &sem_table[slot];
    if (!set->in_use || !set->marked_delete || set->waiter_count != 0U)
        return;
    memset(set, 0, sizeof(*set));
}

int sysv_shmget(uint32_t key, size_t size, uint32_t flags, int *out_id)
{
    int idx;

    if (!out_id)
        return -EINVAL;

    if (key != SYSV_IPC_PRIVATE) {
        idx = sysv_find_shm_by_key(key);
        if (idx >= 0) {
            if ((flags & (SYSV_IPC_CREAT | SYSV_IPC_EXCL)) ==
                (SYSV_IPC_CREAT | SYSV_IPC_EXCL))
                return -EEXIST;
            if (size != 0U && size > shm_table[idx].size)
                return -EINVAL;
            *out_id = sysv_shm_id_from_slot((uint32_t)idx, shm_table[idx].seq);
            return 0;
        }
        if ((flags & SYSV_IPC_CREAT) == 0U)
            return -ENOENT;
    }

    if (size == 0U)
        return -EINVAL;

    for (uint32_t slot = 0U; slot < SYSV_SHM_MAX; slot++) {
        sysv_shm_seg_t *seg;
        uint16_t        next_seq;
        uint32_t        pages;
        uint32_t        order;
        size_t          aligned;
        size_t          alloc_size;
        uint64_t        pa;

        if (shm_table[slot].in_use)
            continue;

        aligned = (size + PAGE_SIZE - 1ULL) & PAGE_MASK;
        pages = (uint32_t)(aligned / PAGE_SIZE);
        order = sysv_pages_to_order(pages);
        if (order >= MAX_ORDER)
            return -ENOMEM;

        pa = phys_alloc_pages(order);
        if (pa == 0ULL)
            return -ENOMEM;

        alloc_size = (size_t)(PAGE_SIZE << order);
        memset((void *)(uintptr_t)pa, 0, alloc_size);

        seg = &shm_table[slot];
        next_seq = (uint16_t)(seg->seq + 1U);
        if (next_seq == 0U)
            next_seq = 1U;
        memset(seg, 0, sizeof(*seg));
        seg->in_use        = 1U;
        seg->marked_delete = 0U;
        seg->seq           = next_seq;
        seg->key           = key;
        seg->pages         = pages;
        seg->attach_count  = 0U;
        seg->base_pa       = pa;
        seg->size          = size;
        seg->aligned_size  = aligned;
        seg->order         = (uint8_t)order;
        *out_id = sysv_shm_id_from_slot(slot, seg->seq);
        return 0;
    }

    return -ENOSPC;
}

int sysv_shmat_current(int shmid, uintptr_t addr, uint32_t flags,
                       uintptr_t *out_addr)
{
    uint32_t    slot;
    uint32_t    proc_slot;
    mm_space_t *space;
    uintptr_t   attach_addr = addr;
    uint32_t    prot = MMU_PROT_USER_R;
    int         rc;

    if (!out_addr || !current_task || !sched_task_is_user(current_task))
        return -EINVAL;
    if ((flags & ~(SYSV_SHM_RDONLY | SYSV_SHM_RND)) != 0U)
        return -EINVAL;
    if (sysv_shm_slot_from_id(shmid, &slot) < 0)
        return -EINVAL;
    if (shm_table[slot].marked_delete)
        return -EIDRM;

    space = sched_task_space(current_task);
    if (!space)
        return -EINVAL;
    proc_slot = sched_task_proc_slot(current_task);

    if ((flags & SYSV_SHM_RDONLY) == 0U)
        prot |= MMU_PROT_USER_W;

    if (attach_addr != 0U) {
        if ((flags & SYSV_SHM_RND) != 0U)
            attach_addr &= PAGE_MASK;
        if ((attach_addr & (PAGE_SIZE - 1ULL)) != 0ULL)
            return -EINVAL;
        rc = sysv_alloc_attach_record(proc_slot, slot, attach_addr,
                                      shm_table[slot].aligned_size);
        if (rc < 0)
            return rc;
        rc = mmu_map_user_phys_region(space, attach_addr,
                                      shm_table[slot].base_pa,
                                      shm_table[slot].aligned_size,
                                      prot, 1);
        if (rc < 0) {
            (void)sysv_find_attach(proc_slot, attach_addr);
            for (uint32_t i = 0U; i < SYSV_SHM_ATTACH_MAX; i++) {
                if (shm_attach_table[i].in_use &&
                    shm_attach_table[i].proc_slot == proc_slot &&
                    shm_attach_table[i].seg_slot == slot &&
                    shm_attach_table[i].addr == attach_addr) {
                    memset(&shm_attach_table[i], 0, sizeof(shm_attach_table[i]));
                    break;
                }
            }
            return -EINVAL;
        }
    } else {
        uintptr_t mapped = 0U;

        rc = mmu_map_user_phys_anywhere(space,
                                        shm_table[slot].base_pa,
                                        shm_table[slot].aligned_size,
                                        prot, &mapped, 1);
        if (rc < 0)
            return -ENOMEM;
        rc = sysv_alloc_attach_record(proc_slot, slot, mapped,
                                      shm_table[slot].aligned_size);
        if (rc < 0) {
            (void)mmu_unmap_user_region(space, mapped, shm_table[slot].aligned_size);
            return rc;
        }
        attach_addr = mapped;
    }

    shm_table[slot].attach_count++;
    *out_addr = attach_addr;
    return 0;
}

int sysv_shmdt_current(uintptr_t addr)
{
    int         attach_idx;
    uint32_t    proc_slot;
    uint32_t    seg_slot;
    mm_space_t *space;

    if (!current_task || !sched_task_is_user(current_task))
        return -EINVAL;
    if ((addr & (PAGE_SIZE - 1ULL)) != 0ULL)
        return -EINVAL;

    proc_slot = sched_task_proc_slot(current_task);
    attach_idx = sysv_find_attach(proc_slot, addr);
    if (attach_idx < 0)
        return -EINVAL;

    space = sched_task_space(current_task);
    seg_slot = shm_attach_table[attach_idx].seg_slot;
    (void)mmu_unmap_user_region(space, addr, shm_attach_table[attach_idx].size);
    memset(&shm_attach_table[attach_idx], 0, sizeof(shm_attach_table[attach_idx]));

    if (seg_slot < SYSV_SHM_MAX && shm_table[seg_slot].in_use &&
        shm_table[seg_slot].attach_count > 0U)
        shm_table[seg_slot].attach_count--;
    sysv_try_free_shm_slot(seg_slot);
    return 0;
}

int sysv_shmctl(int shmid, int cmd, void *buf)
{
    uint32_t slot;

    (void)buf;

    if (sysv_shm_slot_from_id(shmid, &slot) < 0)
        return -EINVAL;

    switch (cmd) {
    case SYSV_IPC_RMID:
        shm_table[slot].marked_delete = 1U;
        sysv_try_free_shm_slot(slot);
        return 0;
    default:
        return -EINVAL;
    }
}

static int sysv_sem_can_apply(const sysv_semset_t *set,
                              const sysv_sembuf_t *ops, uint32_t nsops,
                              int32_t *next_values)
{
    uint32_t i;

    if (!set || !ops || !next_values)
        return -EINVAL;

    for (i = 0U; i < set->nsems; i++)
        next_values[i] = set->values[i];

    for (i = 0U; i < nsops; i++) {
        uint32_t sem_num;
        int32_t  cur;

        sem_num = ops[i].sem_num;
        if (sem_num >= set->nsems)
            return -EINVAL;

        cur = next_values[sem_num];
        if (ops[i].sem_op < 0) {
            if ((cur + (int32_t)ops[i].sem_op) < 0)
                return 1;
            next_values[sem_num] = cur + (int32_t)ops[i].sem_op;
        } else if (ops[i].sem_op == 0) {
            if (cur != 0)
                return 1;
        } else {
            if ((cur + (int32_t)ops[i].sem_op) > SYSV_SEM_VALUE_MAX)
                return -ERANGE;
            next_values[sem_num] = cur + (int32_t)ops[i].sem_op;
        }
    }

    return 0;
}

int sysv_semget(uint32_t key, uint32_t nsems, uint32_t flags, int *out_id)
{
    int idx;

    if (!out_id)
        return -EINVAL;

    if (key != SYSV_IPC_PRIVATE) {
        idx = sysv_find_sem_by_key(key);
        if (idx >= 0) {
            if ((flags & (SYSV_IPC_CREAT | SYSV_IPC_EXCL)) ==
                (SYSV_IPC_CREAT | SYSV_IPC_EXCL))
                return -EEXIST;
            if (nsems != 0U && nsems > sem_table[idx].nsems)
                return -EINVAL;
            *out_id = sysv_sem_id_from_slot((uint32_t)idx, sem_table[idx].seq);
            return 0;
        }
        if ((flags & SYSV_IPC_CREAT) == 0U)
            return -ENOENT;
    }

    if (nsems == 0U || nsems > SYSV_SEM_PER_SET_MAX)
        return -EINVAL;

    for (uint32_t slot = 0U; slot < SYSV_SEMSET_MAX; slot++) {
        sysv_semset_t *set = &sem_table[slot];
        uint16_t       next_seq;

        if (set->in_use)
            continue;
        next_seq = (uint16_t)(set->seq + 1U);
        if (next_seq == 0U)
            next_seq = 1U;
        memset(set, 0, sizeof(*set));
        set->in_use = 1U;
        set->marked_delete = 0U;
        set->seq = next_seq;
        set->key = key;
        set->nsems = nsems;
        *out_id = sysv_sem_id_from_slot(slot, set->seq);
        return 0;
    }

    return -ENOSPC;
}

int sysv_semop_current(int semid, const sysv_sembuf_t *ops, uint32_t nsops,
                       uint64_t timeout_ms, int has_timeout)
{
    uint32_t slot;
    uint64_t deadline_ms = 0ULL;
    int      waiting = 0;

    if (!ops || nsops == 0U || nsops > SYSV_SEM_PER_SET_MAX)
        return -EINVAL;
    if (sysv_sem_slot_from_id(semid, &slot) < 0)
        return -EINVAL;
    if (has_timeout)
        deadline_ms = timer_now_ms() + timeout_ms;

    for (;;) {
        uint64_t       flags;
        sysv_semset_t *set;
        int32_t        next_values[SYSV_SEM_PER_SET_MAX];
        int            rc;

        flags = sysv_irq_save();
        set = &sem_table[slot];
        if (!set->in_use || set->marked_delete) {
            if (waiting && set->waiter_count > 0U) {
                set->waiter_count--;
                sysv_try_free_sem_slot(slot);
            }
            sysv_irq_restore(flags);
            return -EIDRM;
        }

        rc = sysv_sem_can_apply(set, ops, nsops, next_values);
        if (rc == 0) {
            for (uint32_t i = 0U; i < set->nsems; i++)
                set->values[i] = next_values[i];
            if (waiting && set->waiter_count > 0U)
                set->waiter_count--;
            sysv_irq_restore(flags);
            return 0;
        }
        if (rc < 0) {
            if (waiting && set->waiter_count > 0U)
                set->waiter_count--;
            sysv_irq_restore(flags);
            return rc;
        }

        for (uint32_t i = 0U; i < nsops; i++) {
            if ((ops[i].sem_flg & (int16_t)SYSV_IPC_NOWAIT) != 0) {
                if (waiting && set->waiter_count > 0U)
                    set->waiter_count--;
                sysv_irq_restore(flags);
                return -EAGAIN;
            }
        }

        if (!waiting) {
            set->waiter_count++;
            waiting = 1;
        }
        sysv_irq_restore(flags);

        if (signal_has_unblocked_pending(current_task)) {
            if (waiting) {
                flags = sysv_irq_save();
                if (sem_table[slot].in_use && sem_table[slot].waiter_count > 0U)
                    sem_table[slot].waiter_count--;
                sysv_try_free_sem_slot(slot);
                sysv_irq_restore(flags);
            }
            return -EINTR;
        }
        if (has_timeout && timer_now_ms() >= deadline_ms) {
            if (waiting) {
                flags = sysv_irq_save();
                if (sem_table[slot].in_use && sem_table[slot].waiter_count > 0U)
                    sem_table[slot].waiter_count--;
                sysv_try_free_sem_slot(slot);
                sysv_irq_restore(flags);
            }
            return -ETIMEDOUT;
        }
        sched_yield();
    }
}

int sysv_semctl(int semid, uint32_t semnum, int cmd, uint64_t arg)
{
    uint32_t       slot;
    uint64_t       flags;
    sysv_semset_t *set;
    int            rc = 0;

    if (sysv_sem_slot_from_id(semid, &slot) < 0)
        return -EINVAL;

    flags = sysv_irq_save();
    set = &sem_table[slot];
    if (!set->in_use) {
        sysv_irq_restore(flags);
        return -EINVAL;
    }

    switch (cmd) {
    case SYSV_IPC_RMID:
        set->marked_delete = 1U;
        sysv_try_free_sem_slot(slot);
        rc = 0;
        break;
    case SYSV_SEM_GETVAL:
        if (semnum >= set->nsems) {
            rc = -EINVAL;
            break;
        }
        rc = set->values[semnum];
        break;
    case SYSV_SEM_SETVAL:
        if (semnum >= set->nsems || arg > (uint64_t)SYSV_SEM_VALUE_MAX) {
            rc = -EINVAL;
            break;
        }
        set->values[semnum] = (int32_t)arg;
        rc = 0;
        break;
    default:
        rc = -EINVAL;
        break;
    }

    sysv_irq_restore(flags);
    return rc;
}

void sysv_ipc_proc_cleanup(uint32_t proc_slot)
{
    for (uint32_t i = 0U; i < SYSV_SHM_ATTACH_MAX; i++) {
        uint32_t seg_slot;

        if (!shm_attach_table[i].in_use ||
            shm_attach_table[i].proc_slot != proc_slot)
            continue;

        seg_slot = shm_attach_table[i].seg_slot;
        memset(&shm_attach_table[i], 0, sizeof(shm_attach_table[i]));
        if (seg_slot < SYSV_SHM_MAX && shm_table[seg_slot].in_use &&
            shm_table[seg_slot].attach_count > 0U)
            shm_table[seg_slot].attach_count--;
    }

    for (uint32_t i = 0U; i < SYSV_SHM_MAX; i++)
        sysv_try_free_shm_slot(i);
}
