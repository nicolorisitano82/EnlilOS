/*
 * Minimal AArch64 setjmp/longjmp for EnlilOS musl bootstrap.
 *
 * jmp_buf layout (22 x u64):
 *   0..11  -> x19..x30
 *   12     -> sp
 *   13     -> reserved/padding
 *   14..21 -> d8..d15
 */

    .text
    .align  2

    .global setjmp
    .type   setjmp, %function
setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x1, sp
    str x1, [x0, #96]
    str xzr, [x0, #104]
    stp d8,  d9,  [x0, #112]
    stp d10, d11, [x0, #128]
    stp d12, d13, [x0, #144]
    stp d14, d15, [x0, #160]
    mov x0, #0
    ret

    .global longjmp
    .type   longjmp, %function
longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    ldp d8,  d9,  [x0, #112]
    ldp d10, d11, [x0, #128]
    ldp d12, d13, [x0, #144]
    ldp d14, d15, [x0, #160]
    mov sp, x2
    mov x0, #1
    cmp x1, #0
    csel x0, x1, x0, ne
    ret
