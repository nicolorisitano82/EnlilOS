#ifndef ENLILOS_MUSL_SEMAPHORE_H
#define ENLILOS_MUSL_SEMAPHORE_H

#include <sys/types.h>
#include <time.h>

typedef struct {
    unsigned int __handle;
    unsigned int __flags;
} sem_t;

#define SEM_FAILED ((sem_t *)0)

int   sem_init(sem_t *sem, int pshared, unsigned int value);
int   sem_destroy(sem_t *sem);
sem_t *sem_open(const char *name, int oflag, ...);
int   sem_close(sem_t *sem);
int   sem_unlink(const char *name);
int   sem_post(sem_t *sem);
int   sem_wait(sem_t *sem);
int   sem_trywait(sem_t *sem);
int   sem_timedwait(sem_t *sem, const struct timespec *abstime);
int   sem_getvalue(sem_t *sem, int *value);

#endif
