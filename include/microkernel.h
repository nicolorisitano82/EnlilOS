/*
 * EnlilOS Microkernel - Core del Microkernel
 * Architettura stile GNU Hurd:
 *   - Il kernel gestisce solo: IPC, scheduling, gestione memoria
 *   - I driver girano come task in user-space (server)
 *   - La comunicazione avviene tramite message passing
 */

#ifndef ENLILOS_MICROKERNEL_H
#define ENLILOS_MICROKERNEL_H

#include "types.h"

/* === Task Management === */

#define MAX_TASKS       64
#define TASK_NAME_LEN   32

typedef enum {
    TASK_STATE_FREE = 0,
    TASK_STATE_READY,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    TASK_STATE_DEAD
} task_state_t;

typedef enum {
    TASK_TYPE_KERNEL,       /* Task kernel (ring 0) */
    TASK_TYPE_SERVER,       /* Server di sistema (driver, filesystem, etc.) */
    TASK_TYPE_USER          /* Processo utente */
} task_type_t;

typedef struct {
    uint32_t    tid;
    char        name[TASK_NAME_LEN];
    task_state_t state;
    task_type_t  type;
    uint64_t    stack_ptr;
    uint64_t    entry_point;
} task_t;

/* === IPC (Inter-Process Communication) === */

#define IPC_MSG_MAX_SIZE    256
#define IPC_SMALL_MSG_MAX   64
#define IPC_MSG_FLAG_INLINE (1U << 0)

typedef struct {
    uint32_t    sender_tid;
    uint32_t    receiver_tid;
    uint32_t    msg_type;
    uint32_t    msg_len;
    uint32_t    flags;
    uint32_t    _reserved;
    union {
        uint8_t  payload[IPC_MSG_MAX_SIZE];
        uint64_t mr[IPC_MSG_MAX_SIZE / sizeof(uint64_t)];
    };
} ipc_message_t;

typedef struct {
    uint64_t budget_cycles;
    uint64_t last_call_cycles;
    uint64_t worst_call_cycles;
    uint32_t total_calls;
    uint32_t budget_misses;
    uint32_t inline_calls;
} port_stats_t;

/* Tipi di messaggio IPC */
#define IPC_MSG_PING        1
#define IPC_MSG_PONG        2
#define IPC_MSG_PRINT       3
#define IPC_MSG_FB_DRAW     4
#define IPC_MSG_BLK_REQ     5
#define IPC_MSG_BLK_RESP    6
#define IPC_MSG_VFS_REQ     7
#define IPC_MSG_VFS_RESP    8
#define IPC_MSG_NET_REQ     9
#define IPC_MSG_NET_RESP    10
#define IPC_MSG_SHUTDOWN    0xFF

/* === Port System (stile Mach/Hurd) === */

#define MAX_PORTS       128

typedef struct {
    uint32_t    port_id;
    uint32_t    owner_tid;
    bool        active;
    char        name[TASK_NAME_LEN];
    uint64_t    latency_budget_cycles;
    uint64_t    last_call_cycles;
    uint64_t    worst_call_cycles;
    uint32_t    total_calls;
    uint32_t    budget_misses;
    uint32_t    inline_calls;
    uint32_t    waiting_tid;
    uint32_t    active_client_tid;
    uint64_t    active_start_cycles;
    bool        pending_valid;
    bool        reply_valid;
    ipc_message_t pending_msg;
    ipc_message_t reply_msg;
} port_t;

/* === Funzioni del Microkernel === */

void        mk_init(void);
task_t     *mk_task_create(const char *name, task_type_t type, uint64_t entry);
void        mk_task_destroy(uint32_t tid);
task_t     *mk_task_get(uint32_t tid);

int         mk_ipc_send(uint32_t sender, uint32_t receiver, uint32_t type,
                         const void *data, uint32_t len);
int         mk_ipc_receive(uint32_t tid, ipc_message_t *msg);
int         mk_ipc_call(uint32_t port_id, uint32_t type,
                        const void *data, uint32_t len,
                        ipc_message_t *reply_out);
int         mk_ipc_wait(uint32_t port_id, ipc_message_t *msg);
int         mk_ipc_poll(uint32_t port_id, ipc_message_t *msg);
int         mk_ipc_reply(uint32_t port_id, uint32_t type,
                         const void *data, uint32_t len);

uint32_t    mk_port_create(uint32_t owner_tid, const char *name);
int         mk_port_destroy(uint32_t port_id);
int         mk_port_rebind(uint32_t port_id, uint32_t owner_tid);
port_t     *mk_port_lookup(const char *name);
int         mk_port_set_budget(uint32_t port_id, uint64_t budget_cycles);
int         mk_port_get_stats(uint32_t port_id, port_stats_t *out);

void        mk_log(const char *prefix, const char *msg);

#endif
