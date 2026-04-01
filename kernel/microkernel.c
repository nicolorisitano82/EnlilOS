/*
 * EnlilOS Microkernel - Core Implementation
 *
 * M7-01 sposta l'IPC dal vecchio ring buffer asincrono a un modello
 * sincrono request/reply per-port con rendez-vous, donation di priorita'
 * al server owner e budget di latenza per canale.
 */

#include "microkernel.h"
#include "sched.h"
#include "syscall.h"
#include "timer.h"
#include "uart.h"

/* Pool di task logici del microkernel */
static task_t tasks[MAX_TASKS];
static uint32_t next_tid = 1;

/* Pool di porte */
static port_t ports[MAX_PORTS];
static uint32_t next_port_id = 1;

/* Mailbox compatibile per la vecchia API send/receive non-blocking */
static ipc_message_t compat_mailbox[MAX_TASKS];
static uint8_t       compat_mailbox_valid[MAX_TASKS];

/* Helpers locali */
static void mk_strcpy(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0U;
    while (src && src[i] && i + 1U < max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void mk_memcpy(void *dst, const void *src, uint32_t n)
{
    uint8_t       *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0U)
        *d++ = *s++;
}

static int mk_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *(const uint8_t *)a - *(const uint8_t *)b;
}

static inline uint64_t mk_irq_save(void)
{
    uint64_t flags;
    __asm__ volatile(
        "mrs %0, daif\n"
        "msr daifset, #2\n"
        : "=r"(flags) :: "memory"
    );
    return flags;
}

static inline void mk_irq_restore(uint64_t flags)
{
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
}

static port_t *mk_port_by_id(uint32_t port_id)
{
    for (uint32_t i = 0U; i < MAX_PORTS; i++) {
        if (ports[i].active && ports[i].port_id == port_id)
            return &ports[i];
    }
    return NULL;
}

static int mk_task_slot(uint32_t tid)
{
    for (uint32_t i = 0U; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_STATE_FREE && tasks[i].tid == tid)
            return (int)i;
    }
    return -1;
}

static uint32_t mk_current_tid(void)
{
    return current_task ? current_task->pid : 0U;
}

static void mk_ipc_build_msg(ipc_message_t *msg,
                             uint32_t sender_tid, uint32_t receiver_tid,
                             uint32_t type, const void *data, uint32_t len)
{
    uint32_t copy_len = (len > IPC_MSG_MAX_SIZE) ? IPC_MSG_MAX_SIZE : len;

    msg->sender_tid = sender_tid;
    msg->receiver_tid = receiver_tid;
    msg->msg_type = type;
    msg->msg_len = copy_len;
    msg->flags = (copy_len <= IPC_SMALL_MSG_MAX) ? IPC_MSG_FLAG_INLINE : 0U;
    msg->_reserved = 0U;

    if (copy_len == 0U || !data)
        return;

    if (msg->flags & IPC_MSG_FLAG_INLINE)
        mk_memcpy(msg->mr, data, copy_len);
    else
        mk_memcpy(msg->payload, data, copy_len);
}

static void mk_ipc_copy_msg(ipc_message_t *dst, const ipc_message_t *src)
{
    mk_memcpy(dst, src, (uint32_t)sizeof(*dst));
}

static void mk_ipc_reset_active(port_t *port)
{
    port->active_client_tid = 0U;
    port->active_start_cycles = 0ULL;
    port->reply_valid = false;
}

void mk_init(void)
{
    for (uint32_t i = 0U; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_FREE;
        tasks[i].tid = 0U;
        compat_mailbox_valid[i] = 0U;
    }
    for (uint32_t i = 0U; i < MAX_PORTS; i++)
        ports[i].active = false;

    uart_puts("[EnlilOS] Microkernel inizializzato\n");
    uart_puts("[EnlilOS]   Max tasks: 64\n");
    uart_puts("[EnlilOS]   Max ports: 128\n");
    uart_puts("[EnlilOS]   IPC: rendez-vous sync + donation + budget per-porta\n");

    mk_task_create("kernel", TASK_TYPE_KERNEL, 0);

    mk_port_create(0, "console");
    mk_port_create(0, "framebuffer");
    mk_port_create(0, "memory");
    mk_port_create(0, "block");
    mk_port_create(0, "vfs");
}

task_t *mk_task_create(const char *name, task_type_t type, uint64_t entry)
{
    for (uint32_t i = 0U; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_FREE) {
            tasks[i].tid = next_tid++;
            mk_strcpy(tasks[i].name, name, TASK_NAME_LEN);
            tasks[i].state = TASK_STATE_READY;
            tasks[i].type = type;
            tasks[i].entry_point = entry;
            tasks[i].stack_ptr = 0ULL;

            uart_puts("[EnlilOS] Task creato: ");
            uart_puts(name);
            uart_puts("\n");
            return &tasks[i];
        }
    }

    uart_puts("[EnlilOS] ERRORE: nessuno slot task disponibile\n");
    return NULL;
}

void mk_task_destroy(uint32_t tid)
{
    for (uint32_t i = 0U; i < MAX_TASKS; i++) {
        if (tasks[i].tid == tid && tasks[i].state != TASK_STATE_FREE) {
            uart_puts("[EnlilOS] Task distrutto: ");
            uart_puts(tasks[i].name);
            uart_puts("\n");
            tasks[i].state = TASK_STATE_DEAD;
            return;
        }
    }
}

task_t *mk_task_get(uint32_t tid)
{
    for (uint32_t i = 0U; i < MAX_TASKS; i++) {
        if (tasks[i].tid == tid && tasks[i].state != TASK_STATE_FREE)
            return &tasks[i];
    }
    return NULL;
}

int mk_ipc_send(uint32_t sender, uint32_t receiver, uint32_t type,
                const void *data, uint32_t len)
{
    int slot = mk_task_slot(receiver);

    if (slot < 0 || compat_mailbox_valid[slot])
        return -1;

    mk_ipc_build_msg(&compat_mailbox[slot], sender, receiver, type, data, len);
    compat_mailbox_valid[slot] = 1U;
    return 0;
}

int mk_ipc_receive(uint32_t tid, ipc_message_t *out)
{
    int slot;

    if (!out) return -1;
    slot = mk_task_slot(tid);
    if (slot < 0 || !compat_mailbox_valid[slot])
        return -1;

    mk_ipc_copy_msg(out, &compat_mailbox[slot]);
    compat_mailbox_valid[slot] = 0U;
    return 0;
}

int mk_ipc_call(uint32_t port_id, uint32_t type,
                const void *data, uint32_t len,
                ipc_message_t *reply_out)
{
    port_t      *port;
    sched_tcb_t *owner_task;
    uint32_t     sender_tid = mk_current_tid();
    uint64_t     flags;
    ipc_message_t msg;
    uint32_t     waiter_tid = 0U;
    int          rc = 0;

    if (!reply_out || !current_task)
        return -EFAULT;

    port = mk_port_by_id(port_id);
    if (!port)
        return -ENOENT;
    if (sender_tid == 0U || sender_tid == port->owner_tid)
        return -EINVAL;

    owner_task = sched_task_find(port->owner_tid);
    if (!owner_task)
        return -ENOENT;

    mk_ipc_build_msg(&msg, sender_tid, port->owner_tid, type, data, len);

    flags = mk_irq_save();
    if (port->active_client_tid != 0U || port->pending_valid || port->reply_valid) {
        mk_irq_restore(flags);
        return -EBUSY;
    }

    port->active_client_tid = sender_tid;
    port->active_start_cycles = timer_now_ticks();
    if (msg.flags & IPC_MSG_FLAG_INLINE)
        port->inline_calls++;

    mk_ipc_copy_msg(&port->pending_msg, &msg);
    port->pending_valid = true;

    if (port->waiting_tid != 0U) {
        waiter_tid = port->waiting_tid;
        port->waiting_tid = 0U;
    }
    mk_irq_restore(flags);

    (void)sched_task_donate_priority(owner_task,
                                     sched_task_effective_priority(current_task));

    if (waiter_tid != 0U)
        sched_unblock(owner_task);

    sched_block();

    flags = mk_irq_save();
    if (port->active_client_tid != sender_tid || !port->reply_valid) {
        rc = -EAGAIN;
    } else {
        mk_ipc_copy_msg(reply_out, &port->reply_msg);
        mk_ipc_reset_active(port);
    }
    mk_irq_restore(flags);
    return rc;
}

int mk_ipc_wait(uint32_t port_id, ipc_message_t *msg)
{
    port_t   *port;
    uint64_t  flags;
    uint32_t  server_tid = mk_current_tid();

    if (!msg || !current_task)
        return -EFAULT;

    port = mk_port_by_id(port_id);
    if (!port)
        return -ENOENT;
    if (server_tid != port->owner_tid)
        return -EPERM;

    flags = mk_irq_save();
    if (port->active_client_tid != 0U && !port->pending_valid) {
        mk_irq_restore(flags);
        return -EBUSY;
    }

    if (port->pending_valid) {
        mk_ipc_copy_msg(msg, &port->pending_msg);
        port->pending_valid = false;
        mk_irq_restore(flags);
        return 0;
    }

    port->waiting_tid = server_tid;
    mk_irq_restore(flags);

    sched_block();

    flags = mk_irq_save();
    if (!port->pending_valid) {
        mk_irq_restore(flags);
        return -EAGAIN;
    }

    mk_ipc_copy_msg(msg, &port->pending_msg);
    port->pending_valid = false;
    mk_irq_restore(flags);
    return 0;
}

int mk_ipc_reply(uint32_t port_id, uint32_t type,
                 const void *data, uint32_t len)
{
    port_t       *port;
    sched_tcb_t  *client_task;
    sched_tcb_t  *owner_task;
    uint64_t      flags;
    uint64_t      elapsed;
    uint32_t      server_tid = mk_current_tid();
    uint32_t      client_tid;
    ipc_message_t reply;

    if (!current_task)
        return -EFAULT;

    port = mk_port_by_id(port_id);
    if (!port)
        return -ENOENT;
    if (server_tid != port->owner_tid)
        return -EPERM;

    flags = mk_irq_save();
    if (port->active_client_tid == 0U || port->reply_valid) {
        mk_irq_restore(flags);
        return -EAGAIN;
    }

    client_tid = port->active_client_tid;
    mk_ipc_build_msg(&reply, server_tid, client_tid, type, data, len);
    mk_ipc_copy_msg(&port->reply_msg, &reply);
    port->reply_valid = true;

    elapsed = timer_now_ticks() - port->active_start_cycles;
    port->last_call_cycles = elapsed;
    if (elapsed > port->worst_call_cycles)
        port->worst_call_cycles = elapsed;
    port->total_calls++;
    if (port->latency_budget_cycles != 0ULL &&
        elapsed > port->latency_budget_cycles) {
        port->budget_misses++;
    }

    mk_irq_restore(flags);

    owner_task = sched_task_find(port->owner_tid);
    if (owner_task)
        sched_task_clear_donation(owner_task);

    client_task = sched_task_find(client_tid);
    if (client_task)
        sched_unblock(client_task);

    return 0;
}

uint32_t mk_port_create(uint32_t owner_tid, const char *name)
{
    for (uint32_t i = 0U; i < MAX_PORTS; i++) {
        if (!ports[i].active) {
            port_t *p = &ports[i];

            p->port_id = next_port_id++;
            p->owner_tid = owner_tid;
            p->active = true;
            mk_strcpy(p->name, name, TASK_NAME_LEN);
            p->latency_budget_cycles = timer_cntfrq() / 100ULL; /* 10ms default */
            p->last_call_cycles = 0ULL;
            p->worst_call_cycles = 0ULL;
            p->total_calls = 0U;
            p->budget_misses = 0U;
            p->inline_calls = 0U;
            p->waiting_tid = 0U;
            p->active_client_tid = 0U;
            p->active_start_cycles = 0ULL;
            p->pending_valid = false;
            p->reply_valid = false;

            uart_puts("[EnlilOS] Porta creata: ");
            uart_puts(name);
            uart_puts("\n");
            return p->port_id;
        }
    }

    return 0U;
}

int mk_port_destroy(uint32_t port_id)
{
    port_t *p = mk_port_by_id(port_id);

    if (!p)
        return -ENOENT;
    if (p->active_client_tid != 0U || p->pending_valid ||
        p->reply_valid || p->waiting_tid != 0U)
        return -EBUSY;

    p->active = false;
    return 0;
}

port_t *mk_port_lookup(const char *name)
{
    for (uint32_t i = 0U; i < MAX_PORTS; i++) {
        if (ports[i].active && mk_strcmp(ports[i].name, name) == 0)
            return &ports[i];
    }
    return NULL;
}

int mk_port_set_budget(uint32_t port_id, uint64_t budget_cycles)
{
    port_t *p = mk_port_by_id(port_id);
    if (!p) return -ENOENT;
    p->latency_budget_cycles = budget_cycles;
    return 0;
}

int mk_port_get_stats(uint32_t port_id, port_stats_t *out)
{
    port_t *p = mk_port_by_id(port_id);

    if (!p || !out)
        return -ENOENT;

    out->budget_cycles = p->latency_budget_cycles;
    out->last_call_cycles = p->last_call_cycles;
    out->worst_call_cycles = p->worst_call_cycles;
    out->total_calls = p->total_calls;
    out->budget_misses = p->budget_misses;
    out->inline_calls = p->inline_calls;
    return 0;
}

void mk_log(const char *prefix, const char *msg)
{
    uart_puts("[EnlilOS:");
    uart_puts(prefix);
    uart_puts("] ");
    uart_puts(msg);
    uart_puts("\n");
}
