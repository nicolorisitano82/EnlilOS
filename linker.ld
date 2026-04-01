/*
 * EnlilOS Microkernel - Linker Script
 * Target: AArch64 (ARMv8-A)
 * Machine: QEMU virt (compatibile con architettura Apple M-series)
 */

ENTRY(_start)

MEMORY
{
    /* QEMU virt machine loads kernel at 0x40000000 */
    RAM (rwx) : ORIGIN = 0x40000000, LENGTH = 128M
}

SECTIONS
{
    . = ORIGIN(RAM);

    .text : {
        KEEP(*(.text.boot))    /* Boot code first */
        *(.text .text.*)
    } > RAM

    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    } > RAM

    .data : ALIGN(4096) {
        *(.data .data.*)
    } > RAM

    .bss : ALIGN(4096) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    } > RAM

    . = ALIGN(4096);
    __stack_top = . + 0x10000;   /* 64KB kernel stack */
    __heap_start = __stack_top;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
    }
}
