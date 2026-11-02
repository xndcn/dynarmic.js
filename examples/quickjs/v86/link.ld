OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}
SECTIONS
{
    . = 0x00100000;
    .multiboot : { KEEP(*(.multiboot)) } :text
    .text : { *(.text.start) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data : { *(.data*) } :data
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } :data
    . = ALIGN(16);
    _heap_start = .;
    _heap_end = 0x00e00000;
    _stack_top = 0x00ff0000;
    ASSERT(_heap_start < _heap_end, "QuickJS image exceeds the default guest heap")
    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
