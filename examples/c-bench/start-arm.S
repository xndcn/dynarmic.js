    .syntax unified
    .arm
    .section .text.start
    .global _start
_start:
    ldr sp, =_stack_top
    bl guest_main
    svc #0
    b .
    .section .note.GNU-stack,"",%progbits
