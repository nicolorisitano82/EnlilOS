/*
 * EnlilOS Microkernel - Context Switch AArch64 (M2-03)
 *
 * sched_context_switch(task_t *prev, task_t *next)
 *   x0 = prev   (task_t*, sp a offset 0)
 *   x1 = next   (task_t*, sp a offset 0)
 *
 * Salva i registri callee-saved di prev sullo stack corrente,
 * poi li ripristina dallo stack di next. Non tocca x0-x18 (caller-saved).
 *
 * Frame salvato (96 byte, 16-byte aligned):
 *   [sp+ 0] x19, x20
 *   [sp+16] x21, x22
 *   [sp+32] x23, x24
 *   [sp+48] x25, x26
 *   [sp+64] x27, x28
 *   [sp+80] x29 (fp), x30 (lr — return address)
 *
 * Per i task alla prima schedulazione, x30 = task_entry_trampoline
 * e x19 = indirizzo della entry function (settato da sched_task_create).
 *
 * RT: WCET = 6 stp + 1 str + 1 ldr + 1 mov + 6 ldp + ret = 16 istruzioni.
 *
 * ─────────────────────────────────────────────────────────────────────
 * task_entry_trampoline
 *
 * Punto di ingresso per tutti i task alla prima esecuzione.
 * Viene chiamato tramite `ret` da sched_context_switch quando il task
 * è schedulato per la prima volta (x30 = task_entry_trampoline nel frame).
 *
 * Operazioni:
 *   1. Abilita IRQ (msr daifclr, #2) — erano disabilitati durante il switch
 *   2. Chiama x19 (entry function del task, salvata nel frame iniziale)
 *   3. Se la entry function ritorna → loop infinito sicuro (non dovrebbe)
 */

.section .text

/* ── sched_context_switch ─────────────────────────────────────────── */

.global sched_context_switch
.type   sched_context_switch, %function
sched_context_switch:
    /* Salva registri callee-saved + LR di prev (96 byte) */
    stp     x19, x20, [sp, #-96]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]

    /* Salva SP corrente in prev->sp (offset 0 in task_t) */
    mov     x2, sp
    str     x2, [x0, #0]

    /* Carica SP di next da next->sp (offset 0 in task_t) */
    ldr     x2, [x1, #0]
    mov     sp, x2

    /* Ripristina registri callee-saved + LR di next */
    ldp     x29, x30, [sp, #80]
    ldp     x27, x28, [sp, #64]
    ldp     x25, x26, [sp, #48]
    ldp     x23, x24, [sp, #32]
    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #96

    /* ret salta a x30:
     * - Prima schedulazione: x30 = task_entry_trampoline
     * - Reschedulazioni successive: x30 = LR salvato in precedenza
     *   (return address nella schedule() o sched_yield() del task) */
    ret

/* ── task_entry_trampoline ───────────────────────────────────────── */

.global task_entry_trampoline
.type   task_entry_trampoline, %function
task_entry_trampoline:
    /* Abilita IRQ: durante il context switch erano disabilitati.
     * Da questo momento il task gira con IRQ abilitati. */
    msr     daifclr, #2

    /* x19 = entry function (salvata da sched_task_create nel frame iniziale) */
    blr     x19

    /* Se la funzione ritorna (non dovrebbe per task kernel permanenti):
     * loop sicuro per evitare esecuzione di codice arbitrario. */
.Ltask_returned:
    b       .Ltask_returned
