OUTPUT_FORMAT("elf32-littlearm")
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
}
