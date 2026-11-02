OUTPUT_FORMAT("elf32-littlearm")
ENTRY(_start)
SECTIONS
{
    . = 0x00010000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .ARM.exidx : { *(.ARM.exidx*) }
    .data : { *(.data*) }
    .bss (NOLOAD) : { *(.bss*) *(COMMON) }
    _image_end = .;
    _stack_top = 0x00ff0000;
    ASSERT(_image_end < 0x00100000, "ARM C benchmark image exceeds reserved space")
}
