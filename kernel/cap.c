/*
 * EnlilOS Microkernel — Capability System (M9-01)
 *
 * Strutture dati (tutte statiche, nessuna allocazione dinamica):
 *   cap_pool[256]           — pool globale di cap_entry_t
 *   cap_table[32][64]       — per-task: token posseduti dal task[pid%32]
 *
 * Generazione token (unforgeable):
 *   CNTPCT_EL0 ^ (pid << 32) ^ cap_salt ^ pid
 *
 * Lookup pool: scansione lineare O(256) — cap_alloc/derive non sono hot-path.
 *
 * RT policy:
 *   cap_send / cap_revoke / cap_query: SYSCALL_FLAG_RT (nessuna alloc)
 *   cap_alloc / cap_derive: senza flag RT (scansione pool)
 */

#include "cap.h"
#include "mmu.h"
#include "pmm.h"
#include "sched.h"
#include "syscall.h"
#include "uart.h"
#include "types.h"

extern void *memcpy(void *dst, const void *src, size_t n);
extern void *memset(void *dst, int value, size_t n);

/* ════════════════════════════════════════════════════════════════════
 * Strutture dati statiche
 * ════════════════════════════════════════════════════════════════════ */

static cap_entry_t cap_pool[CAP_POOL_SIZE];
static cap_t       cap_table[SCHED_MAX_TASKS][MAX_CAPS_PER_TASK];
static uint64_t    cap_salt;

/* ════════════════════════════════════════════════════════════════════
 * Helpers interni
 * ════════════════════════════════════════════════════════════════════ */

static cap_t cap_make_token(uint32_t pid)
{
    uint64_t cntpct;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cntpct));
    return (cntpct ^ ((uint64_t)pid << 32) ^ cap_salt ^ (uint64_t)pid) &
           0x7FFFFFFFFFFFFFFFULL;
}

static int cap_store_user(uintptr_t uva, const void *src, size_t size)
{
    mm_space_t    *space;
    const uint8_t *in = (const uint8_t *)src;
    size_t         copied = 0U;

    if (size == 0U)
        return 0;
    if (!current_task || !sched_task_is_user(current_task))
        return -EFAULT;

    space = sched_task_space(current_task);
    while (copied < size) {
        uintptr_t cur = uva + copied;
        size_t    page_off = (size_t)(cur & (PAGE_SIZE - 1ULL));
        size_t    chunk = PAGE_SIZE - page_off;
        void     *dst;

        if (chunk > size - copied)
            chunk = size - copied;
        if (mmu_space_prepare_write(space, cur, chunk) < 0)
            return -EFAULT;

        dst = mmu_space_resolve_ptr(space, cur, chunk);
        if (!dst)
            return -EFAULT;

        memcpy(dst, in + copied, chunk);
        copied += chunk;
    }

    return 0;
}

/* O(CAP_POOL_SIZE) — trova entry valida con token corrispondente. */
static cap_entry_t *cap_pool_find(cap_t token)
{
    for (uint32_t i = 0; i < CAP_POOL_SIZE; i++) {
        if (cap_pool[i].valid && cap_pool[i].token == token)
            return &cap_pool[i];
    }
    return (void *)0;
}

/* Verifica che pid abbia token nella sua cap_table. */
static int cap_table_has(uint32_t pid, cap_t token)
{
    uint32_t idx = pid % SCHED_MAX_TASKS;
    for (uint32_t i = 0; i < MAX_CAPS_PER_TASK; i++) {
        if (cap_table[idx][i] == token)
            return 1;
    }
    return 0;
}

/* Aggiunge token alla cap_table di pid. Ritorna 0 o -ENOMEM. */
static int cap_table_add(uint32_t pid, cap_t token)
{
    uint32_t idx = pid % SCHED_MAX_TASKS;
    for (uint32_t i = 0; i < MAX_CAPS_PER_TASK; i++) {
        if (cap_table[idx][i] == CAP_INVALID) {
            cap_table[idx][i] = token;
            return 0;
        }
    }
    return -ENOMEM;
}

/* ════════════════════════════════════════════════════════════════════
 * cap_init
 * ════════════════════════════════════════════════════════════════════ */

void cap_init(void)
{
    memset(cap_pool,  0, sizeof(cap_pool));
    memset(cap_table, 0, sizeof(cap_table));

    uint64_t cntpct;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(cntpct));
    cap_salt = cntpct ^ 0xDEADBEEFCAFEBABEULL;
}

/* ════════════════════════════════════════════════════════════════════
 * cap_alloc_kernel — alloca capability per conto del kernel
 * ════════════════════════════════════════════════════════════════════ */

cap_t cap_alloc_kernel(uint32_t pid, uint32_t type, uint64_t rights,
                       uint64_t object)
{
    /* Trova slot libero nel pool. */
    cap_entry_t *entry = (void *)0;
    for (uint32_t i = 0; i < CAP_POOL_SIZE; i++) {
        if (!cap_pool[i].valid) {
            entry = &cap_pool[i];
            break;
        }
    }
    if (!entry)
        return CAP_INVALID;

    /* Genera token non-zero. */
    cap_t token = cap_make_token(pid);
    if (token == CAP_INVALID)
        token = cap_make_token(pid) | 1ULL;

    entry->token     = token;
    entry->type      = type;
    entry->owner_pid = pid;
    entry->rights    = rights & CAP_RIGHTS_ALL;
    entry->object    = object;
    entry->valid     = 1;

    if (cap_table_add(pid, token) != 0) {
        entry->valid = 0;
        return CAP_INVALID;
    }

    return token;
}

/* ════════════════════════════════════════════════════════════════════
 * cap_validate — API kernel interna (usata da altri moduli)
 * ════════════════════════════════════════════════════════════════════ */

int cap_validate(cap_t token, uint32_t type, uint64_t required_rights)
{
    cap_entry_t *e = cap_pool_find(token);
    if (!e)
        return -EPERM;
    if (e->type != type)
        return -EPERM;
    if ((e->rights & required_rights) != required_rights)
        return -EPERM;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════
 * Syscall handlers
 * ════════════════════════════════════════════════════════════════════ */

/*
 * sys_cap_alloc — nr 60
 * args[0] = type (CAP_TYPE_*), args[1] = rights bitmask
 * Ritorna: cap_t token o -errno
 */
uint64_t sys_cap_alloc(uint64_t args[6])
{
    uint32_t type   = (uint32_t)args[0];
    uint64_t rights = args[1] & CAP_RIGHTS_ALL;

    if (type > CAP_TYPE_TASK)
        return ERR(EINVAL);

    sched_tcb_t *cur = current_task;
    if (!cur)
        return ERR(ESRCH);

    cap_t token = cap_alloc_kernel(cur->pid, type, rights, 0ULL);
    if (token == CAP_INVALID)
        return ERR(ENOMEM);

    return token;
}

/*
 * sys_cap_send — nr 61 (RT-safe)
 * args[0] = dst_pid, args[1] = cap_t
 * Ritorna: 0 o -errno
 * Il token è duplicato: mittente e destinatario lo possiedono entrambi.
 */
uint64_t sys_cap_send(uint64_t args[6])
{
    uint32_t dst_pid = (uint32_t)args[0];
    cap_t    token   = args[1];

    sched_tcb_t *cur = current_task;
    if (!cur)
        return ERR(ESRCH);

    cap_entry_t *e = cap_pool_find(token);
    if (!e)
        return ERR(EINVAL);

    if (!cap_table_has(cur->pid, token))
        return ERR(EPERM);
    if (!(e->rights & CAP_RIGHT_SEND))
        return ERR(EPERM);

    if (cap_table_add(dst_pid, token) != 0)
        return ERR(ENOMEM);

    return 0;
}

/*
 * sys_cap_revoke — nr 62 (RT-safe)
 * args[0] = cap_t
 * Solo l'owner_pid può revocare.
 * Invalida il pool entry e rimuove il token da tutte le cap_table.
 * Complessità: O(SCHED_MAX_TASKS * MAX_CAPS_PER_TASK) = O(2048).
 */
uint64_t sys_cap_revoke(uint64_t args[6])
{
    cap_t token = args[0];

    sched_tcb_t *cur = current_task;
    if (!cur)
        return ERR(ESRCH);

    cap_entry_t *e = cap_pool_find(token);
    if (!e)
        return ERR(EINVAL);

    if (!cap_table_has(cur->pid, token))
        return ERR(EPERM);
    if (e->owner_pid != cur->pid)
        return ERR(EPERM);
    if ((e->rights & CAP_RIGHT_REVOKE) == 0U)
        return ERR(EPERM);

    /* Invalida nel pool. */
    e->valid = 0;

    /* Rimuove da tutte le cap_table. */
    for (uint32_t t = 0; t < SCHED_MAX_TASKS; t++) {
        for (uint32_t i = 0; i < MAX_CAPS_PER_TASK; i++) {
            if (cap_table[t][i] == token)
                cap_table[t][i] = CAP_INVALID;
        }
    }

    return 0;
}

/*
 * sys_cap_derive — nr 63
 * args[0] = src_cap, args[1] = rights_mask
 * Crea una capability figlia con rights = src.rights & rights_mask.
 * Richiede CAP_RIGHT_DERIVE sul token sorgente.
 * Il figlio ha un token distinto e owner = current->pid.
 */
uint64_t sys_cap_derive(uint64_t args[6])
{
    cap_t    src   = args[0];
    uint64_t rmask = args[1] & CAP_RIGHTS_ALL;

    sched_tcb_t *cur = current_task;
    if (!cur)
        return ERR(ESRCH);

    cap_entry_t *se = cap_pool_find(src);
    if (!se)
        return ERR(EINVAL);

    if (!cap_table_has(cur->pid, src))
        return ERR(EPERM);
    if (!(se->rights & CAP_RIGHT_DERIVE))
        return ERR(EPERM);

    /* I diritti del figlio non possono superare quelli del padre. */
    uint64_t new_rights = se->rights & rmask;

    cap_t child = cap_alloc_kernel(cur->pid, se->type, new_rights, se->object);
    if (child == CAP_INVALID)
        return ERR(ENOMEM);

    return child;
}

/*
 * sys_cap_query — nr 64 (RT-safe)
 * args[0] = cap_t, args[1] = uintptr_t cap_info_t*
 * Copia cap_info_t nel buffer utente.
 * Il chiamante deve possedere il token.
 */
uint64_t sys_cap_query(uint64_t args[6])
{
    cap_t       token = args[0];
    uintptr_t   ubuf  = (uintptr_t)args[1];
    cap_info_t  info;
    int         rc;

    if (!ubuf)
        return ERR(EFAULT);

    sched_tcb_t *cur = current_task;
    if (!cur)
        return ERR(ESRCH);

    cap_entry_t *e = cap_pool_find(token);
    if (!e)
        return ERR(EINVAL);

    if (!cap_table_has(cur->pid, token))
        return ERR(EPERM);

    info.type      = e->type;
    info.owner_pid = e->owner_pid;
    info.rights    = e->rights;
    info.object    = e->object;

    rc = cap_store_user(ubuf, &info, sizeof(info));
    if (rc < 0)
        return ERR(-rc);

    return 0;
}
