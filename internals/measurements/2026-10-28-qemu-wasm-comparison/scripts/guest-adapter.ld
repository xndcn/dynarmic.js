ENTRY(_start)
SECTIONS
{
    . = 0x00080000;
    .text : { *(.text*) }
    /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
