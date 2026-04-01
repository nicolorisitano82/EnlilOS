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

typedef struct {
    uint32_t    sender_tid;
    uint32_t    receiver_tid;
    uint32_t    msg_type;
    uint32_t    msg_len;
    uint8_t     payload[IPC_MSG_MAX_SIZE];
} ipc_message_t;

/* Tipi di messaggio IPC */
#define IPC_MSG_PING        1
#define IPC_MSG_PONG        2
#define IPC_MSG_PRINT       3
#define IPC_MSG_FB_DRAW     4
#define IPC_MSG_SHUTDOWN    0xFF

/* === Port System (stile Mach/Hurd) === */

#define MAX_PORTS       128

typedef struct {
    uint32_t    port_id;
    uint32_t    owner_tid;
    bool        active;
    char        name[TASK_NAME_LEN];
} port_t;

/* === Funzioni del Microkernel === */

void        mk_init(void);
task_t     *mk_task_create(const char *name, task_type_t type, uint64_t entry);
void        mk_task_destroy(uint32_t tid);
task_t     *mk_task_get(uint32_t tid);

int         mk_ipc_send(uint32_t sender, uint32_t receiver, uint32_t type,
                         const void *data, uint32_t len);
int         mk_ipc_receive(uint32_t tid, ipc_message_t *msg);

uint32_t    mk_port_create(uint32_t owner_tid, const char *name);
int         mk_port_destroy(uint32_t port_id);
port_t     *mk_port_lookup(const char *name);

void        mk_log(const char *prefix, const char *msg);

#endif
