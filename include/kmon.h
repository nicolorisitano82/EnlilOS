/*
 * EnlilOS - Kernel monitor (M8-07)
 */

#ifndef ENLILOS_KMON_H
#define ENLILOS_KMON_H

#include "types.h"

struct sched_tcb;
typedef struct sched_tcb sched_tcb_t;

typedef uint32_t kmon_t;

#define KMON_INVALID         0U
#define KMON_MAX_COND        8U
#define KMON_WAIT_FOREVER_NS 0ULL

#define KMON_HOARE           (1U << 0)
#define KMON_MESA            (1U << 1) /* riservato: v1 usa sempre handoff Hoare */
#define KMON_RT              (1U << 2)

void kmon_init(void);
void kmon_task_cleanup(sched_tcb_t *task);

int  kmon_create_current(uint32_t prio_ceiling, uint32_t flags, kmon_t *handle_out);
int  kmon_destroy_current(kmon_t handle);
int  kmon_enter_current(kmon_t handle);
int  kmon_exit_current(kmon_t handle);
int  kmon_wait_current(kmon_t handle, uint8_t cond, uint64_t timeout_ns);
int  kmon_signal_current(kmon_t handle, uint8_t cond);
int  kmon_broadcast_current(kmon_t handle, uint8_t cond);

#endif /* ENLILOS_KMON_H */
