// Minimal Raspberry Pi QEMU adapter for the unmodified dynarmic A64 guest.
// CoreMark services: SVC #0 exit, #1 semihosted output, #4 monotonic microseconds.

.section .text.boot
.global _start
_start:
    ldr x0, =vectors
    mrs x1, CurrentEL
    lsr x1, x1, #2
    cmp x1, #3
    b.eq 3f
    cmp x1, #2
    b.eq 2f
    msr VBAR_EL1, x0
    b 1f
2:  msr VBAR_EL2, x0
    b 1f
3:  msr VBAR_EL3, x0
1:  isb
    mov x0, #0x10000
    br x0

.balign 2048
vectors:
.rept 4
    b .
    .balign 128
.endr
    b sync_handler
    .balign 128
.rept 11
    b .
    .balign 128
.endr

sync_handler:
    stp x0, x1, [sp, #-32]!
    stp x2, x3, [sp, #16]
    mrs x2, CurrentEL
    cmp x2, #0xc
    b.eq read_esr_el3
    cmp x2, #0x8
    b.eq read_esr_el2
    mrs x1, ESR_EL1
    b classify
read_esr_el2:
    mrs x1, ESR_EL2
    b classify
read_esr_el3:
    mrs x1, ESR_EL3
classify:
    and x1, x1, #0xffff
    cmp x1, #1
    b.eq putchar
    cmp x1, #4
    b.eq clock_us
    cbz x1, exit_guest
    b .

putchar:
    ldr x1, =output_byte
    strb w0, [x1]
    strb wzr, [x1, #1]
    mov x0, #0x04
    hlt #0xf000
    b done

clock_us:
    mrs x1, CNTPCT_EL0
    mrs x2, CNTFRQ_EL0
    mov x3, #0x4240
    movk x3, #0xf, lsl #16
    mul x1, x1, x3
    udiv x0, x1, x2
    str x0, [sp]

done:
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp], #32
    eret

exit_guest:
    mov x0, #0x18
    mov x1, #0x26
    movk x1, #0x2, lsl #16
    hlt #0xf000
    b .

.section .data
.balign 8
output_byte:
    .byte 0, 0
