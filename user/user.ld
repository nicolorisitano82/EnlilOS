ENTRY(_start)

PHDRS
{
  text PT_LOAD FILEHDR PHDRS FLAGS(5);
  data PT_LOAD FLAGS(6);
  tls  PT_TLS FLAGS(4);
}

SECTIONS
{
  . = 0x0000007FC1000000 + SIZEOF_HEADERS;

  .text : ALIGN(16)
  {
    *(.text*)
    *(.rodata*)
    KEEP(*(.init))
    KEEP(*(.fini))
  } :text

  . = ALIGN(0x1000);
  .tdata : ALIGN(16)
  {
    PROVIDE_HIDDEN(__tdata_start = .);
    *(.tdata*)
    PROVIDE_HIDDEN(__tdata_end = .);
  } :data :tls

  . = ALIGN(16);
  .tbss : ALIGN(16)
  {
    PROVIDE_HIDDEN(__tbss_start = .);
    *(.tbss*)
    *(.tcommon)
    PROVIDE_HIDDEN(__tbss_end = .);
  } :data :tls

  . = ADDR(.tbss) + SIZEOF(.tbss);
  . = ALIGN(16);
  .preinit_array : ALIGN(16)
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array))
    KEEP(*(.preinit_array.*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } :data

  . = ALIGN(16);
  .init_array : ALIGN(16)
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
    KEEP(*(.init_array))
    PROVIDE_HIDDEN(__init_array_end = .);
  } :data

  . = ALIGN(16);
  .fini_array : ALIGN(16)
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
    KEEP(*(.fini_array))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } :data

  .data :
  {
    *(.data*)
    *(.bss*)
    *(COMMON)
  } :data
}
