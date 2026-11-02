OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)
PHDRS { text PT_LOAD FLAGS(5); data PT_LOAD FLAGS(6); }
SECTIONS
{
    . = 0x00100000;
    .multiboot : { KEEP(*(.multiboot)) } :text
    .text : { *(.text.start) *(.text*) } :text
    .rodata : { *(.rodata*) } :text
    . = ALIGN(4096);
    .data : { *(.data*) } :data
    .bss (NOLOAD) : { *(.bss*) *(COMMON) } :data
    _image_end = .;
    _stack_top = 0x00ff0000;
    ASSERT(_image_end < 0x00200000, "x86 C benchmark image exceeds reserved space")
    /DISCARD/ : { *(.eh_frame*) *(.comment) }
}
