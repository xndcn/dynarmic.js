OUTPUT_FORMAT("elf64-littleaarch64")
ENTRY(_start)
SECTIONS
{
    . = 0x00010000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    . = 0x00040000;
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    . = 0x00070000;
    _stack_top = .;
    /* Unwind tables and notes are never used by the flat binary. */
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
