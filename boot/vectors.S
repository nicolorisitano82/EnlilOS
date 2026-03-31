/*
 * NROS Microkernel - AArch64 Exception Vector Table (M1-01)
 *
 * Ogni entry della vector table è 128 byte (0x80) = max 32 istruzioni.
 * save_context da sola è ~30 istruzioni → NON entra in 128 byte.
 *
 * Soluzione standard: ogni entry fa solo 5-6 istruzioni (branch a handler
 * out-of-line). Il salvataggio completo avviene nell'handler __exc_common.
 *
 * Schema per ogni entry:
 *   1. Salva temporaneamente x16/x17 su SP (16 byte, no clobber dei dati)
 *   2. Carica src e typ in x16/x17
 *   3. Branch a __exc_common
 *
 * __exc_common:
 *   1. Alloca il frame completo (288 byte)
 *   2. Trasferisce x16/x17 (src/typ) nel frame e recupera x16/x17 originali
 *   3. Salva tutti i registri
 *   4. Chiama exception_handler(src, typ, frame)
 *   5. Ripristina e eret
 */

.section .text

/* ────────────────────────────────────────────────────────────────────
 * Macro: ogni slot da 128 byte
 * Usa x16/x17 come scratch (ABI: "IP0/IP1", liberi tra chiamate).
 * ────────────────────────────────────────────────────────────────── */
.macro vector_entry src, typ
    .align 7                        /* allinea questa entry a 128 byte */
    /* Salva x16/x17 in cima allo stack PRIMA di usarli come scratch */
    sub     sp,  sp,  #16
    stp     x16, x17, [sp]
    /* Carica src e typ come immediati */
    mov     x16, #\src
    mov     x17, #\typ
    b       __exc_common            /* salta all'handler condiviso */
    /* Le istruzioni sopra = 5 × 4 = 20 byte su 128 disponibili */
.endm

/* ────────────────────────────────────────────────────────────────────
 * Vector Table — deve essere allineata a 2KB (2^11)
 * ────────────────────────────────────────────────────────────────── */
.global exception_vectors
.align 11
exception_vectors:

    /* Sorgente 0: Current EL, SP_EL0 (non dovremmo arrivarci) */
    vector_entry 0, 0   /* +0x000 Sync    */
    vector_entry 0, 1   /* +0x080 IRQ     */
    vector_entry 0, 2   /* +0x100 FIQ     */
    vector_entry 0, 3   /* +0x180 SError  */

    /* Sorgente 1: Current EL, SP_ELx (handler del kernel — il più comune) */
    vector_entry 1, 0   /* +0x200 Sync    */
    vector_entry 1, 1   /* +0x280 IRQ     */
    vector_entry 1, 2   /* +0x300 FIQ     */
    vector_entry 1, 3   /* +0x380 SError  */

    /* Sorgente 2: Lower EL, AArch64 (user-space futuro) */
    vector_entry 2, 0   /* +0x400 Sync    */
    vector_entry 2, 1   /* +0x480 IRQ     */
    vector_entry 2, 2   /* +0x500 FIQ     */
    vector_entry 2, 3   /* +0x580 SError  */

    /* Sorgente 3: Lower EL, AArch32 (non supportato) */
    vector_entry 3, 0   /* +0x600 Sync    */
    vector_entry 3, 1   /* +0x680 IRQ     */
    vector_entry 3, 2   /* +0x700 FIQ     */
    vector_entry 3, 3   /* +0x780 SError  */

/* ────────────────────────────────────────────────────────────────────
 * __exc_common — handler condiviso out-of-line
 *
 * All'ingresso:
 *   SP+0  = x16 originale (salvato dall'entry)
 *   SP+8  = x17 originale (salvato dall'entry)
 *   x16   = src  (exc_source_t)
 *   x17   = typ  (exc_type_t)
 *
 * Frame sullo stack dopo questo codice (offset da SP nuovo):
 *   0..240  x0..x30
 *   248     sp_el0
 *   256     elr_el1
 *   264     spsr_el1
 *   272     esr_el1
 *   280     far_el1
 *
 * Totale frame = 288 byte. Sopra di esso ci sono altri 16 byte
 * (x16/x17 originali salvati dalla entry). Li recuperiamo e
 * li mettiamo al posto giusto nel frame.
 * ────────────────────────────────────────────────────────────────── */
__exc_common:
    /*
     * Al momento SP punta a: [x17_orig | x16_orig | ...]
     * (lo stack cresce verso il basso, stp x16,x17,[sp] dopo sub sp,sp,#16)
     *
     * Alloca il frame sotto i 16 byte già riservati.
     * Frame = 288 byte + 16 già usati → allochiamo altri 288.
     */
    sub     sp,  sp,  #288

    /* Salva x0–x15, x18–x30 (saltiamo x16/x17 per ora) */
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    /* x16/x17: salva da area temporanea (a SP+288) */
    ldp     x0,  x1,  [sp, #288]   /* recupera x16_orig, x17_orig */
    stp     x0,  x1,  [sp, #128]   /* metti x16, x17 nel frame */
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]

    /* Salva SP_EL0, ELR_EL1, SPSR_EL1 */
    mrs     x0, sp_el0
    str     x0, [sp, #248]
    mrs     x0, elr_el1
    str     x0, [sp, #256]
    mrs     x0, spsr_el1
    str     x0, [sp, #264]

    /* Salva ESR_EL1, FAR_EL1 */
    mrs     x0, esr_el1
    str     x0, [sp, #272]
    mrs     x0, far_el1
    str     x0, [sp, #280]

    /* Chiama exception_handler(src, typ, frame) */
    mov     x0, x16         /* arg0: src (ancora in x16) */
    mov     x1, x17         /* arg1: typ (ancora in x17) */
    add     x2, sp, #0      /* arg2: puntatore al frame */
    bl      exception_handler

    /* ── Ripristino ────────────────────────────────────────────────── */
    /* Ripristina ELR_EL1 e SPSR_EL1 */
    ldr     x0, [sp, #256]
    msr     elr_el1, x0
    ldr     x0, [sp, #264]
    msr     spsr_el1, x0

    /* Ripristina x0–x30 */
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldp     x18, x19, [sp, #144]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]

    /* Dealloca frame (288) + area temporanea x16/x17 (16) */
    add     sp, sp, #304
    eret
