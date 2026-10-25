ENTRY(_start)
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.head)) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) }
  .data : { *(.data .data.*) }
  . = ALIGN(16);
  _bss_start = .;
  .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) }
  . = ALIGN(16);
  _bss_end = .;
  .stack (NOLOAD) : { . += 16384; }
  _stack_top = .;
  _image_end = .;
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
