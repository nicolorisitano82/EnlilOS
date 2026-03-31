/*
 * NROS Microkernel - Boot Code
 * Target: AArch64 (ARMv8-A)
 *
 * Questo è il primo codice eseguito dal processore.
 * Inizializza lo stack, azzera la sezione .bss e salta al kernel C.
 */

.section .text.boot
.global _start

_start:
    /* Solo il core 0 procede, gli altri vanno in sleep */
    mrs     x0, mpidr_el1
    and     x0, x0, #0xFF
    cbz     x0, core0_boot
    b       halt

core0_boot:
    /* Disabilita le interruzioni */
    msr     daifset, #0xF

    /* Abilita FPU/SIMD (NEON) - necessario per codice compilato con -O2 */
    mrs     x0, cpacr_el1
    orr     x0, x0, #(3 << 20)   /* FPEN: abilita accesso FP/SIMD a EL1/EL0 */
    msr     cpacr_el1, x0
    isb

    /* Imposta lo stack pointer */
    ldr     x0, =__stack_top
    mov     sp, x0

    /* Azzera la sezione .bss */
    ldr     x0, =__bss_start
    ldr     x1, =__bss_end
bss_clear:
    cmp     x0, x1
    b.ge    bss_done
    str     xzr, [x0], #8
    b       bss_clear
bss_done:

    /* Installa la vector table PRIMA di abilitare qualsiasi interrupt */
    adr     x0, exception_vectors
    msr     vbar_el1, x0
    isb

    /* Salta al kernel main in C */
    bl      kernel_main

    /* Se kernel_main ritorna, halt */
halt:
    wfe
    b       halt
