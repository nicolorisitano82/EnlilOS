.section .text

.equ SYS_WRITE,  1
.equ SYS_EXECVE, 10

.global _start
_start:
    adrp    x0, path_exec2
    add     x0, x0, :lo12:path_exec2
    adrp    x1, argv_exec2
    add     x1, x1, :lo12:argv_exec2
    adrp    x2, envp_empty
    add     x2, x2, :lo12:envp_empty
    mov     x8, #SYS_EXECVE
    svc     #0

    mov     x0, #1
    adrp    x1, fail_msg
    add     x1, x1, :lo12:fail_msg
    mov     x2, #fail_msg_end - fail_msg
    mov     x8, #SYS_WRITE
    svc     #0

1:
    wfe
    b       1b

.section .rodata
path_exec2:
    .asciz "/EXEC2.ELF"
arg0_exec2:
    .asciz "/EXEC2.ELF"
argv_exec2:
    .quad   arg0_exec2
    .quad   0
envp_empty:
    .quad   0
fail_msg:
    .ascii  "[EL0] execve fallita\n"
fail_msg_end:
