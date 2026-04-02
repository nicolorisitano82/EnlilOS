/*
 * EnlilOS - Kernel semaphores (M8-06)
 */

#ifndef ENLILOS_KSEM_H
#define ENLILOS_KSEM_H

#include "types.h"

struct sched_tcb;
typedef struct sched_tcb sched_tcb_t;

typedef uint32_t ksem_t;

#define KSEM_INVALID         0U
#define KSEM_NAME_MAX        32U
#define KSEM_WAIT_FOREVER_NS 0ULL

#define KSEM_PRIVATE         (1U << 0)
#define KSEM_SHARED          (1U << 1)
#define KSEM_RT              (1U << 2)
#define KSEM_ONESHOT         (1U << 3)

void ksem_init(void);
void ksem_task_cleanup(sched_tcb_t *task);

int  ksem_create_current(const char *name, uint32_t value, uint32_t flags,
                         ksem_t *handle_out);
int  ksem_open_current(const char *name, uint32_t flags, ksem_t *handle_out);
int  ksem_close_current(ksem_t handle);
int  ksem_unlink_current(const char *name);
int  ksem_post_current(ksem_t handle);
int  ksem_wait_current(ksem_t handle);
int  ksem_timedwait_current(ksem_t handle, uint64_t timeout_ns);
int  ksem_trywait_current(ksem_t handle);
int  ksem_getvalue_current(ksem_t handle, int32_t *value_out);
int  ksem_anon_current(uint32_t value, uint32_t flags, ksem_t *handle_out);

#endif /* ENLILOS_KSEM_H */
