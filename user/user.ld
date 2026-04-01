ENTRY(_start)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  . = 0x0000007FC1000000 + SIZEOF_HEADERS;

  .text : ALIGN(16)
  {
    *(.text*)
    *(.rodata*)
  } :text

  . = ALIGN(0x1000);
  .data :
  {
    *(.data*)
    *(.bss*)
    *(COMMON)
  } :data
}
