OUTPUT_FORMAT("elf32-littlearm")
ENTRY(_start)
SECTIONS
{
    . = 0x10000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .ARM.exidx : { *(.ARM.exidx*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    _heap_start = .;
    _heap_end = 0xe00000;
    _stack_top = 0xff0000;
    ASSERT(_heap_start < _heap_end, "QuickJS image exceeds guest heap")
}
