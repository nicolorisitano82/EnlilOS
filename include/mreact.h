/*
 * EnlilOS - Reactive memory subscriptions (M8-05)
 */

#ifndef ENLILOS_MREACT_H
#define ENLILOS_MREACT_H

#include "types.h"

struct sched_tcb;
typedef struct sched_tcb sched_tcb_t;

typedef uint32_t mreact_handle_t;

typedef enum {
    MREACT_EQ = 1,
    MREACT_NEQ,
    MREACT_GT,
    MREACT_LT,
    MREACT_BITMASK_SET,
    MREACT_BITMASK_CLEAR,
    MREACT_CHANGED,
    MREACT_RANGE_IN,
    MREACT_RANGE_OUT,
} mreact_pred_t;

#define MREACT_MAX_SUBS        8U
#define MREACT_ONE_SHOT        (1U << 0)
#define MREACT_PERSISTENT      (1U << 1)
#define MREACT_EDGE            (1U << 2)
#define MREACT_LEVEL           (1U << 3)
#define MREACT_SAMPLE_SHIFT    16U
#define MREACT_SAMPLE_MASK     0xFFU
#define MREACT_SAMPLE(n)       (((uint32_t)((n) & MREACT_SAMPLE_MASK)) << MREACT_SAMPLE_SHIFT)
#define MREACT_SAMPLE_GET(f)   (((f) >> MREACT_SAMPLE_SHIFT) & MREACT_SAMPLE_MASK)

#define MREACT_WAIT_FOREVER_NS 0ULL

typedef struct {
    void         *addr;
    size_t        size;
    mreact_pred_t pred;
    uint64_t      value;
    uint32_t      flags;
    uint32_t      _pad;
} mreact_sub_t;

void mreact_init(void);
void mreact_tick(uint64_t now_ms);
void mreact_task_cleanup(sched_tcb_t *task);

int  mreact_subscribe_current(const mreact_sub_t *subs, uint32_t count,
                              int require_all, uint32_t common_flags,
                              mreact_handle_t *handle_out);
int  mreact_wait_current(mreact_handle_t handle, uint64_t timeout_ns);
int  mreact_cancel_current(mreact_handle_t handle);

#endif
