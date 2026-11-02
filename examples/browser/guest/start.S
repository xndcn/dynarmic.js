// Bare-metal A32 entry: set up a stack, call guest_main, then svc #0 with
// r0 = guest_main's return value as the exit status.

    .syntax unified
    .arm
    .section .text.start
    .global _start
_start:
    ldr sp, =_stack_top
    bl  guest_main
    svc #0
    b   .
