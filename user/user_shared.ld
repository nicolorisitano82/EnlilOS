PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  data PT_LOAD FLAGS(6);
  dynamic PT_DYNAMIC FLAGS(6);
}

SECTIONS
{
  . = SIZEOF_HEADERS;

  .text : ALIGN(16)
  {
    *(.text*)
    *(.rodata*)
  } :text

  . = ALIGN(0x1000);
  .dynamic : { *(.dynamic) } :data :dynamic
  .got : { *(.got*) } :data

  .data :
  {
    *(.data*)
    *(.bss*)
    *(COMMON)
  } :data
}
