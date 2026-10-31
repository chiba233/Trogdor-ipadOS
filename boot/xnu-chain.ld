/* SPDX-License-Identifier: BSD-2-Clause */
ENTRY(_start)
/* Screen images replace this: a prelinked kernel needs more, and Depthcharge allows it. */
PROVIDE(XNU_IMAGE_LIMIT = 80 * 1024 * 1024);
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.head)) *(.text .text.*) }
  .rodata : { *(.rodata .rodata.*) KEEP(*(.xnu_payload)) }
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
  . = _linux_entry + LINUX_MEMORY_SIZE;
  . = ALIGN(0x200000);
  _xnu_window = .;
  .xnu_window (NOLOAD) : { . += 24 * 1024 * 1024; }
  _image_end = .;
  ASSERT(SIZEOF(.linux) <= LINUX_MEMORY_SIZE, "Linux file exceeds memory size")
  ASSERT(_image_end <= XNU_IMAGE_LIMIT, "Checkpoint exceeds bounded Image memory")
  /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
