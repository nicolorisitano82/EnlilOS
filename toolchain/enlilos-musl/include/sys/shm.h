#ifndef ENLILOS_MUSL_SYS_SHM_H
#define ENLILOS_MUSL_SYS_SHM_H

#include <sys/ipc.h>
#include <sys/types.h>
#include <stdint.h>

#define SHM_RDONLY  010000
#define SHM_RND    020000
#define SHMLBA     4096

struct shmid_ds {
    struct ipc_perm shm_perm;
    size_t          shm_segsz;
    long            shm_atime;
    long            shm_dtime;
    long            shm_ctime;
    pid_t           shm_cpid;
    pid_t           shm_lpid;
    int             shm_nattch;
};

int   shmget(key_t key, size_t size, int shmflg);
void *shmat(int shmid, const void *shmaddr, int shmflg);
int   shmdt(const void *shmaddr);
int   shmctl(int shmid, int cmd, struct shmid_ds *buf);

#endif /* ENLILOS_MUSL_SYS_SHM_H */
