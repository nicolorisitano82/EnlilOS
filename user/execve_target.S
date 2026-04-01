.section .text

.equ SYS_WRITE, 1
.equ SYS_EXIT,  3

.global _start
_start:
    mov     x0, #1
    adrp    x1, msg
    add     x1, x1, :lo12:msg
    mov     x2, #msg_end - msg
    mov     x8, #SYS_WRITE
    svc     #0

    mov     x0, #0
    mov     x8, #SYS_EXIT
    svc     #0

1:
    wfe
    b       1b

.section .rodata
msg:
    .ascii  "[EL0] execve target avviato correttamente\n"
msg_end:
