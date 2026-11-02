// Bare-metal A64 entry: set up a stack, call guest_main, then svc #0 with
// x0 = guest_main's return value as the exit status.

    .section .text.start
    .global _start
_start:
    ldr x0, =_stack_top
    mov sp, x0
    bl  guest_main
    svc #0
1:  b   1b
