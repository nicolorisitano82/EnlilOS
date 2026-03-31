/*
 * NROS Microkernel - Core Implementation
 *
 * Implementazione base del microkernel stile GNU Hurd/Mach:
 *   - Task management (creazione/distruzione processi)
 *   - IPC message passing (comunicazione tra processi)
 *   - Port system (namespace per i servizi)
 *
 * In un sistema completo, i driver (UART, framebuffer, filesystem, rete)
 * girerebbero come server in user-space, comunicando col kernel via IPC.
 */

#include "microkernel.h"
#include "uart.h"

/* Pool di task */
static task_t tasks[MAX_TASKS];
static uint32_t next_tid = 1;

/* Pool di porte */
static port_t ports[MAX_PORTS];
static uint32_t next_port_id = 1;

/* Coda messaggi IPC semplice (ring buffer) */
#define IPC_QUEUE_SIZE 64
static ipc_message_t ipc_queue[IPC_QUEUE_SIZE];
static uint32_t ipc_queue_head = 0;
static uint32_t ipc_queue_tail = 0;

/* Helper per copiare stringhe */
static void mk_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Helper per copiare memoria */
static void mk_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

/* Helper per confrontare stringhe */
static int mk_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *(uint8_t *)a - *(uint8_t *)b;
}

void mk_init(void)
{
    /* Azzera tutte le strutture */
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_FREE;
        tasks[i].tid = 0;
    }
    for (int i = 0; i < MAX_PORTS; i++) {
        ports[i].active = false;
    }

    uart_puts("[NROS] Microkernel inizializzato\n");
    uart_puts("[NROS]   Max tasks: 64\n");
    uart_puts("[NROS]   Max ports: 128\n");
    uart_puts("[NROS]   IPC queue: 64 messaggi\n");

    /* Crea il task kernel (task 0) */
    mk_task_create("kernel", TASK_TYPE_KERNEL, 0);

    /* Crea le porte di sistema */
    mk_port_create(1, "console");
    mk_port_create(1, "framebuffer");
    mk_port_create(1, "memory");
}

task_t *mk_task_create(const char *name, task_type_t type, uint64_t entry)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) {
            tasks[i].tid = next_tid++;
            mk_strcpy(tasks[i].name, name, TASK_NAME_LEN);
            tasks[i].state = TASK_STATE_READY;
            tasks[i].type = type;
            tasks[i].entry_point = entry;
            tasks[i].stack_ptr = 0;

            uart_puts("[NROS] Task creato: ");
            uart_puts(name);
            uart_puts("\n");

            return &tasks[i];
        }
    }
    uart_puts("[NROS] ERRORE: nessuno slot task disponibile\n");
    return NULL;
}

void mk_task_destroy(uint32_t tid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].tid == tid && tasks[i].state != TASK_STATE_FREE) {
            uart_puts("[NROS] Task distrutto: ");
            uart_puts(tasks[i].name);
            uart_puts("\n");
            tasks[i].state = TASK_STATE_DEAD;
            return;
        }
    }
}

task_t *mk_task_get(uint32_t tid)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].tid == tid && tasks[i].state != TASK_STATE_FREE) {
            return &tasks[i];
        }
    }
    return NULL;
}

int mk_ipc_send(uint32_t sender, uint32_t receiver, uint32_t type,
                 const void *data, uint32_t len)
{
    uint32_t next = (ipc_queue_tail + 1) % IPC_QUEUE_SIZE;
    if (next == ipc_queue_head) {
        uart_puts("[NROS] ERRORE: coda IPC piena\n");
        return -1;
    }

    ipc_message_t *msg = &ipc_queue[ipc_queue_tail];
    msg->sender_tid = sender;
    msg->receiver_tid = receiver;
    msg->msg_type = type;
    msg->msg_len = (len > IPC_MSG_MAX_SIZE) ? IPC_MSG_MAX_SIZE : len;
    if (data && len > 0) {
        mk_memcpy(msg->payload, data, msg->msg_len);
    }

    ipc_queue_tail = next;
    return 0;
}

int mk_ipc_receive(uint32_t tid, ipc_message_t *out)
{
    /* Cerca un messaggio per questo tid nella coda */
    uint32_t idx = ipc_queue_head;
    while (idx != ipc_queue_tail) {
        if (ipc_queue[idx].receiver_tid == tid) {
            mk_memcpy(out, &ipc_queue[idx], sizeof(ipc_message_t));
            /* Rimuovi dalla coda (shift semplificato) */
            ipc_queue[idx].receiver_tid = 0;
            if (idx == ipc_queue_head) {
                ipc_queue_head = (ipc_queue_head + 1) % IPC_QUEUE_SIZE;
            }
            return 0;
        }
        idx = (idx + 1) % IPC_QUEUE_SIZE;
    }
    return -1; /* Nessun messaggio */
}

uint32_t mk_port_create(uint32_t owner_tid, const char *name)
{
    for (int i = 0; i < MAX_PORTS; i++) {
        if (!ports[i].active) {
            ports[i].port_id = next_port_id++;
            ports[i].owner_tid = owner_tid;
            ports[i].active = true;
            mk_strcpy(ports[i].name, name, TASK_NAME_LEN);

            uart_puts("[NROS] Porta creata: ");
            uart_puts(name);
            uart_puts("\n");

            return ports[i].port_id;
        }
    }
    return 0;
}

int mk_port_destroy(uint32_t port_id)
{
    for (int i = 0; i < MAX_PORTS; i++) {
        if (ports[i].port_id == port_id && ports[i].active) {
            ports[i].active = false;
            return 0;
        }
    }
    return -1;
}

port_t *mk_port_lookup(const char *name)
{
    for (int i = 0; i < MAX_PORTS; i++) {
        if (ports[i].active && mk_strcmp(ports[i].name, name) == 0) {
            return &ports[i];
        }
    }
    return NULL;
}

void mk_log(const char *prefix, const char *msg)
{
    uart_puts("[NROS:");
    uart_puts(prefix);
    uart_puts("] ");
    uart_puts(msg);
    uart_puts("\n");
}
