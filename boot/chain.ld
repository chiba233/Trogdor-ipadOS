/* SPDX-License-Identifier: BSD-2-Clause */
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
  . = ALIGN(0x200000);
  _linux_entry = .;
  .linux : { KEEP(*(.linux)) }
  _image_end = _linux_entry + LINUX_MEMORY_SIZE;
  ASSERT(SIZEOF(.linux) <= LINUX_MEMORY_SIZE, "Linux file exceeds its memory size")
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
