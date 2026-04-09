#ifndef ENLILOS_MUSL_PTHREAD_H
#define ENLILOS_MUSL_PTHREAD_H

#include <signal.h>
#include <stddef.h>
#include <time.h>

typedef struct __pthread *pthread_t;

typedef struct {
    void  *__stackaddr;
    size_t __stacksize;
    int    __detachstate;
} pthread_attr_t;

typedef struct {
    volatile int __state;
} pthread_mutex_t;

typedef struct {
    volatile unsigned int __seq;
} pthread_cond_t;

typedef struct {
    int __dummy;
} pthread_mutexattr_t;

typedef struct {
    int __dummy;
} pthread_condattr_t;

#define PTHREAD_CREATE_JOINABLE 0
#define PTHREAD_CREATE_DETACHED 1
#define PTHREAD_STACK_MIN       16384U

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER  { 0U }

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
int pthread_detach(pthread_t thread);
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);
int pthread_kill(pthread_t thread, int sig);
int pthread_sigmask(int how, const sigset_t *set, sigset_t *old);
void pthread_exit(void *retval) __attribute__((noreturn));

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);
int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

#endif
