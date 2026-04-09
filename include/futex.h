#ifndef ENLILOS_FUTEX_H
#define ENLILOS_FUTEX_H

#include "sched.h"
#include "types.h"

void futex_init(void);

int  futex_wait_current(uintptr_t uaddr, uint32_t expected, uintptr_t timeout_uva);
int  futex_wake_current(uintptr_t uaddr, uint32_t count);
int  futex_requeue_current(uintptr_t uaddr,
                           uint32_t wake_count,
                           uint32_t requeue_count,
                           uintptr_t uaddr2,
                           int cmp_enabled,
                           uint32_t cmp_value);
int  futex_wake_task_uaddr(const sched_tcb_t *task, uintptr_t uaddr, uint32_t count);
void futex_task_cleanup(sched_tcb_t *task);

#endif
