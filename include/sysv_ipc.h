#ifndef ENLILOS_SYSV_IPC_H
#define ENLILOS_SYSV_IPC_H

#include "types.h"

typedef struct {
    uint16_t sem_num;
    int16_t  sem_op;
    int16_t  sem_flg;
} sysv_sembuf_t;

#define SYSV_SEM_PER_SET_MAX 16U

#define SYSV_IPC_PRIVATE    0U
#define SYSV_IPC_CREAT      01000U
#define SYSV_IPC_EXCL       02000U
#define SYSV_IPC_NOWAIT     04000U

#define SYSV_IPC_RMID       0
#define SYSV_IPC_SET        1
#define SYSV_IPC_STAT       2

#define SYSV_SEM_GETVAL     12
#define SYSV_SEM_SETVAL     16

#define SYSV_SHM_RDONLY     010000U
#define SYSV_SHM_RND        020000U

void sysv_ipc_proc_cleanup(uint32_t proc_slot);

int  sysv_shmget(uint32_t key, size_t size, uint32_t flags, int *out_id);
int  sysv_shmat_current(int shmid, uintptr_t addr, uint32_t flags,
                        uintptr_t *out_addr);
int  sysv_shmdt_current(uintptr_t addr);
int  sysv_shmctl(int shmid, int cmd, void *buf);

int  sysv_semget(uint32_t key, uint32_t nsems, uint32_t flags, int *out_id);
int  sysv_semop_current(int semid, const sysv_sembuf_t *ops, uint32_t nsops,
                        uint64_t timeout_ms, int has_timeout);
int  sysv_semctl(int semid, uint32_t semnum, int cmd, uint64_t arg);

#endif
