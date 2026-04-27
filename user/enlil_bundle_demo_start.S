    .text
    .global _start
    .type _start, %function

_start:
    mov x0, sp
    bl  enlil_bundle_main
1:
    wfe
    b 1b
