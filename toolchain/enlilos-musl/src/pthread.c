#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "enlil_syscalls.h"
#include "user_svc.h"

#define ENLIL_PTHREAD_MAGIC       0x50544852U
#define ENLIL_PTHREAD_FLAG_DETACH 0x0001U
#define ENLIL_PTHREAD_FLAG_LISTED 0x0002U
#define ENLIL_PTHREAD_FLAG_MAIN   0x0004U
#define ENLIL_PTHREAD_FLAG_JOINED 0x0008U

#define ENLIL_PTHREAD_DEFAULT_STACK (64UL * 1024UL)

#define ENLIL_CLONE_VM              0x00000100U
#define ENLIL_CLONE_FS              0x00000200U
#define ENLIL_CLONE_FILES           0x00000400U
#define ENLIL_CLONE_SIGHAND         0x00000800U
#define ENLIL_CLONE_THREAD          0x00010000U
#define ENLIL_CLONE_SYSVSEM         0x00040000U
#define ENLIL_CLONE_SETTLS          0x00080000U
#define ENLIL_CLONE_CHILD_CLEARTID  0x00200000U
#define ENLIL_CLONE_CHILD_SETTID    0x01000000U

#define ENLIL_FUTEX_WAIT            0
#define ENLIL_FUTEX_WAKE            1
#define ENLIL_FUTEX_PRIVATE_FLAG    128

struct __pthread {
    uint32_t          magic;
    volatile uint32_t tid;
    unsigned int      flags;
    void             *retval;
    void             *(*start_routine)(void *);
    void             *arg;
    void             *stack_base;
    size_t            stack_size;
    void             *tp_stub;
    struct __pthread *next_detached;
};

static struct __pthread main_thread;
static struct __pthread *detached_head;
static volatile unsigned int runtime_lock;
static volatile unsigned int runtime_inited;

static uintptr_t pthread_read_tp(void)
{
    uintptr_t tp = 0U;

    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

static void pthread_yield_cpu(void)
{
    (void)user_svc0(SYS_YIELD);
}

static void pthread_runtime_lock_acquire(void)
{
    while (__atomic_exchange_n(&runtime_lock, 1U, __ATOMIC_ACQUIRE) != 0U)
        pthread_yield_cpu();
}

static void pthread_runtime_lock_release(void)
{
    __atomic_store_n(&runtime_lock, 0U, __ATOMIC_RELEASE);
}

static int pthread_timespec_valid(const struct timespec *ts)
{
    return ts && ts->tv_sec >= 0L && ts->tv_nsec >= 0L && ts->tv_nsec < 1000000000L;
}

static int pthread_deadline_remaining_ns(const struct timespec *abstime,
                                         unsigned long long *out_ns)
{
    struct timeval tv;
    long long      now_ns;
    long long      abs_ns;
    long long      delta;

    if (!pthread_timespec_valid(abstime) || !out_ns)
        return EINVAL;
    if (gettimeofday(&tv, NULL) < 0)
        return errno ? errno : EIO;

    now_ns = ((long long)tv.tv_sec * 1000000000LL)
           + ((long long)tv.tv_usec * 1000LL);
    abs_ns = ((long long)abstime->tv_sec * 1000000000LL)
           + (long long)abstime->tv_nsec;
    delta = abs_ns - now_ns;
    if (delta <= 0LL) {
        *out_ns = 0ULL;
        return ETIMEDOUT;
    }

    *out_ns = (unsigned long long)delta;
    return 0;
}

static struct __pthread *pthread_lookup_current(void)
{
    uintptr_t          tp = pthread_read_tp();
    struct __pthread **slot;
    struct __pthread  *thread;

    if (tp != 0U) {
        slot = (struct __pthread **)(uintptr_t)tp;
        thread = slot ? *slot : (struct __pthread *)0;
        if (thread && thread->magic == ENLIL_PTHREAD_MAGIC)
            return thread;
    }

    if (__atomic_load_n(&runtime_inited, __ATOMIC_ACQUIRE) == 0U)
        return &main_thread;
    return &main_thread;
}

static void pthread_free_thread(struct __pthread *thread)
{
    if (!thread || (thread->flags & ENLIL_PTHREAD_FLAG_MAIN) != 0U)
        return;

    if (thread->stack_base && thread->stack_size != 0U)
        (void)munmap(thread->stack_base, thread->stack_size);
    if (thread->tp_stub)
        free(thread->tp_stub);
    free(thread);
}

static void pthread_reap_detached_locked(void)
{
    struct __pthread *prev = NULL;
    struct __pthread *it = detached_head;

    while (it) {
        struct __pthread *next = it->next_detached;

        if (__atomic_load_n(&it->tid, __ATOMIC_ACQUIRE) == 0U) {
            if (prev)
                prev->next_detached = next;
            else
                detached_head = next;
            it->next_detached = NULL;
            it->flags &= ~ENLIL_PTHREAD_FLAG_LISTED;
            pthread_free_thread(it);
        } else {
            prev = it;
        }
        it = next;
    }
}

static void pthread_reap_detached(void)
{
    pthread_runtime_lock_acquire();
    pthread_reap_detached_locked();
    pthread_runtime_lock_release();
}

static void pthread_detached_list_add_locked(struct __pthread *thread)
{
    if (!thread || (thread->flags & ENLIL_PTHREAD_FLAG_MAIN) != 0U)
        return;
    if ((thread->flags & ENLIL_PTHREAD_FLAG_LISTED) != 0U)
        return;
    thread->next_detached = detached_head;
    detached_head = thread;
    thread->flags |= ENLIL_PTHREAD_FLAG_LISTED;
}

void __enlilos_thread_runtime_init(void)
{
    uintptr_t          tp;
    struct __pthread **slot;

    if (__atomic_load_n(&runtime_inited, __ATOMIC_ACQUIRE) != 0U)
        return;

    pthread_runtime_lock_acquire();
    if (__atomic_load_n(&runtime_inited, __ATOMIC_RELAXED) != 0U) {
        pthread_runtime_lock_release();
        return;
    }

    (void)memset(&main_thread, 0, sizeof(main_thread));
    main_thread.magic = ENLIL_PTHREAD_MAGIC;
    main_thread.flags = ENLIL_PTHREAD_FLAG_MAIN;
    main_thread.tid   = (uint32_t)gettid();
    main_thread.tp_stub = (void *)(uintptr_t)pthread_read_tp();

    tp = (uintptr_t)main_thread.tp_stub;
    if (tp != 0U) {
        slot = (struct __pthread **)(uintptr_t)tp;
        *slot = &main_thread;
    }

    __atomic_store_n(&runtime_inited, 1U, __ATOMIC_RELEASE);
    pthread_runtime_lock_release();
}

static __attribute__((noreturn)) void pthread_child_bootstrap(void)
{
    struct __pthread *self = pthread_lookup_current();
    void             *ret = NULL;

    if (!self || self->magic != ENLIL_PTHREAD_MAGIC)
        user_svc_exit(127, SYS_EXIT);

    ret = self->start_routine ? self->start_routine(self->arg) : NULL;
    self->retval = ret;
    __sync_synchronize();
    user_svc_exit(0, SYS_EXIT);
}

static int pthread_futex_wait(volatile uint32_t *uaddr, uint32_t expected)
{
    long rc = user_svc4(SYS_FUTEX, (long)uaddr,
                        ENLIL_FUTEX_WAIT | ENLIL_FUTEX_PRIVATE_FLAG,
                        (long)expected, 0L);
    return (rc < 0) ? (int)-rc : 0;
}

static int pthread_futex_wake(volatile uint32_t *uaddr, uint32_t count)
{
    long rc = user_svc4(SYS_FUTEX, (long)uaddr,
                        ENLIL_FUTEX_WAKE | ENLIL_FUTEX_PRIVATE_FLAG,
                        (long)count, 0L);
    return (rc < 0) ? (int)-rc : 0;
}

int pthread_attr_init(pthread_attr_t *attr)
{
    if (!attr)
        return EINVAL;
    attr->__stackaddr = NULL;
    attr->__stacksize = ENLIL_PTHREAD_DEFAULT_STACK;
    attr->__detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr)
{
    if (!attr)
        return EINVAL;
    attr->__stackaddr = NULL;
    attr->__stacksize = 0U;
    attr->__detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    if (!attr || stacksize < PTHREAD_STACK_MIN)
        return EINVAL;
    attr->__stacksize = (stacksize + 15U) & ~((size_t)15U);
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
    if (!attr || !stacksize)
        return EINVAL;
    *stacksize = attr->__stacksize;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if (!attr)
        return EINVAL;
    if (detachstate != PTHREAD_CREATE_JOINABLE &&
        detachstate != PTHREAD_CREATE_DETACHED)
        return EINVAL;
    attr->__detachstate = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    if (!attr || !detachstate)
        return EINVAL;
    *detachstate = attr->__detachstate;
    return 0;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg)
{
    static const uint32_t clone_flags =
        ENLIL_CLONE_VM | ENLIL_CLONE_FS | ENLIL_CLONE_FILES |
        ENLIL_CLONE_SIGHAND | ENLIL_CLONE_THREAD | ENLIL_CLONE_SYSVSEM |
        ENLIL_CLONE_SETTLS | ENLIL_CLONE_CHILD_SETTID |
        ENLIL_CLONE_CHILD_CLEARTID;
    const pthread_attr_t default_attr = {
        NULL, ENLIL_PTHREAD_DEFAULT_STACK, PTHREAD_CREATE_JOINABLE
    };
    const pthread_attr_t *use_attr = attr ? attr : &default_attr;
    struct __pthread     *obj;
    size_t                stack_size;
    void                 *stack_base;
    void                 *tp_stub;
    void                 *child_sp;
    long                  tid;

    if (!thread || !start_routine)
        return EINVAL;

    __enlilos_thread_runtime_init();
    pthread_reap_detached();

    stack_size = use_attr->__stacksize ? use_attr->__stacksize : ENLIL_PTHREAD_DEFAULT_STACK;
    stack_size = (stack_size + 15U) & ~((size_t)15U);
    if (stack_size < PTHREAD_STACK_MIN)
        return EINVAL;

    obj = (struct __pthread *)calloc(1U, sizeof(*obj));
    if (!obj)
        return ENOMEM;

    stack_base = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack_base == MAP_FAILED) {
        free(obj);
        return errno ? errno : ENOMEM;
    }

    tp_stub = calloc(1U, 16U);
    if (!tp_stub) {
        (void)munmap(stack_base, stack_size);
        free(obj);
        return ENOMEM;
    }

    obj->magic = ENLIL_PTHREAD_MAGIC;
    obj->flags = (use_attr->__detachstate == PTHREAD_CREATE_DETACHED)
        ? ENLIL_PTHREAD_FLAG_DETACH : 0U;
    obj->start_routine = start_routine;
    obj->arg = arg;
    obj->stack_base = stack_base;
    obj->stack_size = stack_size;
    obj->tp_stub = tp_stub;
    *((struct __pthread **)tp_stub) = obj;

    child_sp = (void *)((uintptr_t)stack_base + stack_size - 16U);
    tid = user_svc6(SYS_CLONE, (long)clone_flags, (long)child_sp,
                    0L, (long)(uintptr_t)tp_stub, (long)&obj->tid, 0L);
    if (tid == 0)
        pthread_child_bootstrap();
    if (tid < 0) {
        int err = (int)-tid;
        free(tp_stub);
        (void)munmap(stack_base, stack_size);
        free(obj);
        return err;
    }

    obj->tid = (uint32_t)tid;

    pthread_runtime_lock_acquire();
    if ((obj->flags & ENLIL_PTHREAD_FLAG_DETACH) != 0U)
        pthread_detached_list_add_locked(obj);
    pthread_runtime_lock_release();

    *thread = obj;
    return 0;
}

int pthread_join(pthread_t thread, void **retval)
{
    uint32_t tid;

    __enlilos_thread_runtime_init();
    pthread_reap_detached();

    if (!thread || thread->magic != ENLIL_PTHREAD_MAGIC)
        return ESRCH;
    if (thread == pthread_self())
        return EDEADLK;

    pthread_runtime_lock_acquire();
    if ((thread->flags & ENLIL_PTHREAD_FLAG_DETACH) != 0U) {
        pthread_runtime_lock_release();
        return EINVAL;
    }
    if ((thread->flags & ENLIL_PTHREAD_FLAG_JOINED) != 0U) {
        pthread_runtime_lock_release();
        return EINVAL;
    }
    thread->flags |= ENLIL_PTHREAD_FLAG_JOINED;
    pthread_runtime_lock_release();

    for (;;) {
        tid = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE);
        if (tid == 0U)
            break;

        {
            int err = pthread_futex_wait(&thread->tid, tid);
            if (err != 0 && err != EAGAIN && err != EINTR)
                return err;
        }
    }

    __sync_synchronize();
    if (retval)
        *retval = thread->retval;
    if ((thread->flags & ENLIL_PTHREAD_FLAG_MAIN) == 0U)
        pthread_free_thread(thread);
    return 0;
}

int pthread_detach(pthread_t thread)
{
    __enlilos_thread_runtime_init();
    pthread_reap_detached();

    if (!thread || thread->magic != ENLIL_PTHREAD_MAGIC)
        return ESRCH;

    pthread_runtime_lock_acquire();
    if ((thread->flags & ENLIL_PTHREAD_FLAG_JOINED) != 0U) {
        pthread_runtime_lock_release();
        return EINVAL;
    }
    if ((thread->flags & ENLIL_PTHREAD_FLAG_DETACH) != 0U) {
        pthread_runtime_lock_release();
        return EINVAL;
    }
    thread->flags |= ENLIL_PTHREAD_FLAG_DETACH;
    pthread_detached_list_add_locked(thread);
    pthread_reap_detached_locked();
    pthread_runtime_lock_release();
    return 0;
}

pthread_t pthread_self(void)
{
    __enlilos_thread_runtime_init();
    return pthread_lookup_current();
}

int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

int pthread_kill(pthread_t thread, int sig)
{
    long rc;

    __enlilos_thread_runtime_init();
    if (!thread || thread->magic != ENLIL_PTHREAD_MAGIC)
        return ESRCH;
    if (__atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE) == 0U)
        return ESRCH;

    rc = user_svc3(SYS_TGKILL, (long)getpid(), (long)thread->tid, (long)sig);
    return (rc < 0) ? (int)-rc : 0;
}

int pthread_sigmask(int how, const sigset_t *set, sigset_t *old)
{
    long rc = user_svc3(SYS_SIGPROCMASK, (long)how, (long)set, (long)old);

    return (rc < 0) ? (int)-rc : 0;
}

void pthread_exit(void *retval)
{
    struct __pthread *self = pthread_lookup_current();

    if (self && self->magic == ENLIL_PTHREAD_MAGIC) {
        self->retval = retval;
        __sync_synchronize();
    }
    user_svc_exit(0, SYS_EXIT);
}

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    if (!attr)
        return EINVAL;
    attr->__dummy = 0;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    if (!attr)
        return EINVAL;
    attr->__dummy = 0;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    (void)attr;
    if (!mutex)
        return EINVAL;
    mutex->__state = 0;
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    if (!mutex)
        return EINVAL;
    if (__atomic_load_n(&mutex->__state, __ATOMIC_ACQUIRE) != 0)
        return EBUSY;
    return 0;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex)
{
    int expected = 0;

    if (!mutex)
        return EINVAL;
    if (__atomic_compare_exchange_n(&mutex->__state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;
    return EBUSY;
}

int pthread_mutex_lock(pthread_mutex_t *mutex)
{
    int expected = 0;

    if (!mutex)
        return EINVAL;
    if (__atomic_compare_exchange_n(&mutex->__state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return 0;

    for (;;) {
        int state = __atomic_load_n(&mutex->__state, __ATOMIC_RELAXED);

        if (state == 0) {
            expected = 0;
            if (__atomic_compare_exchange_n(&mutex->__state, &expected, 2, 0,
                                            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return 0;
            continue;
        }

        if (state == 1) {
            expected = 1;
            (void)__atomic_compare_exchange_n(&mutex->__state, &expected, 2, 0,
                                              __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }

        {
            int err = pthread_futex_wait((volatile uint32_t *)&mutex->__state, 2U);
            if (err != 0 && err != EAGAIN && err != EINTR)
                return err;
        }
    }
}

int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    int prev;

    if (!mutex)
        return EINVAL;
    prev = __atomic_exchange_n(&mutex->__state, 0, __ATOMIC_RELEASE);
    if (prev == 0)
        return EPERM;
    if (prev == 2)
        return pthread_futex_wake((volatile uint32_t *)&mutex->__state, 1U);
    return 0;
}

int pthread_condattr_init(pthread_condattr_t *attr)
{
    if (!attr)
        return EINVAL;
    attr->__dummy = 0;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr)
{
    if (!attr)
        return EINVAL;
    attr->__dummy = 0;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    (void)attr;
    if (!cond)
        return EINVAL;
    cond->__seq = 0U;
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond)
{
    if (!cond)
        return EINVAL;
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    unsigned int seq;
    int          rc;

    if (!cond || !mutex)
        return EINVAL;

    seq = __atomic_load_n(&cond->__seq, __ATOMIC_ACQUIRE);
    rc = pthread_mutex_unlock(mutex);
    if (rc != 0)
        return rc;

    while (__atomic_load_n(&cond->__seq, __ATOMIC_ACQUIRE) == seq) {
        rc = pthread_futex_wait(&cond->__seq, seq);
        if (rc != 0 && rc != EAGAIN && rc != EINTR)
            break;
    }

    {
        int lock_rc = pthread_mutex_lock(mutex);
        if (rc == 0 || rc == EAGAIN || rc == EINTR)
            return lock_rc;
        return (lock_rc != 0) ? lock_rc : rc;
    }
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime)
{
    static const struct timespec sleep_step = { 0L, 1000000L };
    unsigned int seq;
    int          rc;

    if (!cond || !mutex || !abstime)
        return EINVAL;

    seq = __atomic_load_n(&cond->__seq, __ATOMIC_ACQUIRE);
    rc = pthread_mutex_unlock(mutex);
    if (rc != 0)
        return rc;

    while (__atomic_load_n(&cond->__seq, __ATOMIC_ACQUIRE) == seq) {
        unsigned long long remain_ns = 0ULL;
        int                time_rc = pthread_deadline_remaining_ns(abstime, &remain_ns);

        if (time_rc == ETIMEDOUT) {
            rc = ETIMEDOUT;
            break;
        }
        if (time_rc != 0) {
            rc = time_rc;
            break;
        }
        (void)remain_ns;
        (void)nanosleep(&sleep_step, NULL);
    }

    {
        int lock_rc = pthread_mutex_lock(mutex);
        if (rc == 0)
            return lock_rc;
        return (lock_rc != 0) ? lock_rc : rc;
    }
}

int pthread_cond_signal(pthread_cond_t *cond)
{
    if (!cond)
        return EINVAL;
    (void)__atomic_add_fetch(&cond->__seq, 1U, __ATOMIC_RELEASE);
    return pthread_futex_wake(&cond->__seq, 1U);
}

int pthread_cond_broadcast(pthread_cond_t *cond)
{
    if (!cond)
        return EINVAL;
    (void)__atomic_add_fetch(&cond->__seq, 1U, __ATOMIC_RELEASE);
    return pthread_futex_wake(&cond->__seq, 0x7FFFFFFFU);
}
