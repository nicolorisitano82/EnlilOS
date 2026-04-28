/*
 * EnlilOS musl bootstrap — SysV SHM wrappers (M12-01)
 */
#include "enlil_syscalls.h"
#include <sys/shm.h>
#include <errno.h>

/* inline SVC helpers */
static long __svc3(long nr, long a0, long a1, long a2)
{
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x8 asm("x8") = nr;
    asm volatile("svc #0"
                 : "+r"(x0), "+r"(x1), "+r"(x2)
                 : "r"(x8)
                 : "x3","x4","x5","x6","x7",
                   "x9","x10","x11","x12","x13","x14","x15",
                   "x16","x17","x18","memory","cc");
    return x0;
}

int shmget(key_t key, size_t size, int shmflg)
{
    long r = __svc3(SYS_SHMGET, (long)key, (long)size, (long)shmflg);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}

void *shmat(int shmid, const void *shmaddr, int shmflg)
{
    long r = __svc3(SYS_SHMAT, (long)shmid, (long)shmaddr, (long)shmflg);
    if (r < 0 && r > -4096L) { errno = (int)(-r); return (void *)-1L; }
    return (void *)r;
}

int shmdt(const void *shmaddr)
{
    register long x0 asm("x0") = (long)shmaddr;
    register long x8 asm("x8") = SYS_SHMDT;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8)
                 : "x1","x2","x3","x4","x5","x6","x7",
                   "x9","x10","x11","x12","x13","x14","x15",
                   "x16","x17","x18","memory","cc");
    if (x0 < 0) { errno = (int)(-x0); return -1; }
    return 0;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf)
{
    long r = __svc3(SYS_SHMCTL, (long)shmid, (long)cmd, (long)buf);
    if (r < 0) { errno = (int)(-r); return -1; }
    return (int)r;
}
