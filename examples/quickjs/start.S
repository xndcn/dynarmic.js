    .syntax unified
    .arm
    .section .text.start
    .global _start
_start:
    ldr sp, =_stack_top
    ldr r0, =0xf000
    ldr r1, [r0]
    cmp r1, #2
    ldreq r1, [r0, #20]
    moveq sp, r1
    bl guest_main
    svc #0
    b .
