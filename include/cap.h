/*
 * EnlilOS Microkernel — Capability System (M9-01)
 *
 * Ogni risorsa protetta viene rappresentata da un token a 64 bit
 * non indovinabile (unforgeable).  I token sono generati combinando
 * il contatore di cicli hardware (CNTPCT_EL0), un salt casuale
 * generato al boot e il PID del proprietario, rendendo impossibile
 * la forgiatura da user-space.
 *
 * Layout memoria (statico, nessuna allocazione dinamica):
 *   cap_pool[CAP_POOL_SIZE]                       — pool globale
 *   cap_table[SCHED_MAX_TASKS][MAX_CAPS_PER_TASK] — per-task
 */

#ifndef ENLILOS_CAP_H
#define ENLILOS_CAP_H

#include "types.h"

/* ── Tipo token ─────────────────────────────────────────────────── */

typedef uint64_t cap_t;
#define CAP_INVALID  0ULL

/* ── Tipi di capability ─────────────────────────────────────────── */

#define CAP_TYPE_NULL    0U   /* non usato */
#define CAP_TYPE_IPC     1U   /* porta IPC (object = port_id) */
#define CAP_TYPE_MEM     2U   /* regione di memoria (object = phys_addr) */
#define CAP_TYPE_FD      3U   /* file descriptor (object = fd index) */
#define CAP_TYPE_MMIO    4U   /* regione MMIO (per driver user-space) */
#define CAP_TYPE_GPU_BUF 5U   /* GPU buffer object */
#define CAP_TYPE_TASK    6U   /* handle su un task (object = pid) */

/* ── Rights bitmask ─────────────────────────────────────────────── */

#define CAP_RIGHT_READ   (1ULL << 0)  /* lettura risorsa */
#define CAP_RIGHT_WRITE  (1ULL << 1)  /* scrittura risorsa */
#define CAP_RIGHT_EXEC   (1ULL << 2)  /* esecuzione */
#define CAP_RIGHT_SEND   (1ULL << 3)  /* può passare ad altro processo */
#define CAP_RIGHT_DERIVE (1ULL << 4)  /* può creare capability figlie */
#define CAP_RIGHT_REVOKE (1ULL << 5)  /* può revocare */
#define CAP_RIGHTS_ALL   0x3FULL

/* ── Costanti pool ──────────────────────────────────────────────── */

#define CAP_POOL_SIZE       256U  /* slot nel pool globale */
#define MAX_CAPS_PER_TASK    64U  /* max capability possedute per task */

/* ── Entry nel pool globale — 40 byte ───────────────────────────── */

typedef struct {
    cap_t    token;      /* token opaco 64-bit (chiave di lookup) */
    uint32_t type;       /* CAP_TYPE_* */
    uint32_t owner_pid;  /* PID del creatore (unico che può revocare) */
    uint64_t rights;     /* bitmask diritti */
    uint64_t object;     /* payload: port_id, phys_addr, fd, pid… */
    uint8_t  valid;      /* 1 = attivo, 0 = libero/revocato */
    uint8_t  _pad[7];
} cap_entry_t;           /* sizeof = 40 byte */

/* ── Info ritornata da cap_query ────────────────────────────────── */

typedef struct {
    uint32_t type;
    uint32_t owner_pid;
    uint64_t rights;
    uint64_t object;
} cap_info_t;

/* ── API kernel ─────────────────────────────────────────────────── */

/* Inizializza pool, table e genera salt da CNTPCT_EL0. */
void  cap_init(void);

/* Alloca una capability per conto del kernel (usabile da altri moduli).
 * Ritorna CAP_INVALID se il pool è pieno o la cap_table del task è piena. */
cap_t cap_alloc_kernel(uint32_t pid, uint32_t type, uint64_t rights,
                       uint64_t object);

/* Verifica token: tipo e diritti richiesti.
 * Ritorna 0 se valida, -EPERM altrimenti. */
int   cap_validate(cap_t token, uint32_t type, uint64_t required_rights);

/* ── Syscall handlers (registrati da syscall_init) ──────────────── */

uint64_t sys_cap_alloc  (uint64_t args[6]);  /* nr 60 */
uint64_t sys_cap_send   (uint64_t args[6]);  /* nr 61 */
uint64_t sys_cap_revoke (uint64_t args[6]);  /* nr 62 */
uint64_t sys_cap_derive (uint64_t args[6]);  /* nr 63 */
uint64_t sys_cap_query  (uint64_t args[6]);  /* nr 64 */

#endif /* ENLILOS_CAP_H */
