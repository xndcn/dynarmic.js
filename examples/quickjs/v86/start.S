/* Multiboot v1 ELF entry. v86 supplies protected mode and flat segments. */
    .section .multiboot,"a"
    .align 4
    .long 0x1badb002
    .long 0x00000003
    .long -(0x1badb002 + 0x00000003)

    .section .text.start,"ax"
    .code32
    .global _start
    .type _start, @function
_start:
    cli
    cld
    movl %cr0, %eax
    andl $~0x0c, %eax              /* Clear EM and TS. */
    orl $0x22, %eax                /* Set MP and NE. */
    movl %eax, %cr0
    movl %cr4, %eax
    orl $0x600, %eax               /* OSFXSR and OSXMMEXCPT: permit SSE2. */
    movl %eax, %cr4
    fninit
    fldcw x87_control
    ldmxcsr sse_control

    movl $_stack_top, %esp
    cmpl $2, 0xf000
    jne 1f
    movl 0xf014, %esp              /* Same host-selected mailbox stack as ARM. */
1:
    andl $-16, %esp
    xorl %ebp, %ebp
    call guest_main
    subl $12, %esp
    pushl %eax
    call _exit
2:
    hlt
    jmp 2b
    .size _start, .-_start

    .section .rodata,"a"
    .align 4
sse_control:
    .long 0x1f80                  /* Round nearest; no flush-to-zero/DAZ. */
x87_control:
    .word 0x027f                  /* Mask exceptions, binary64 significand. */
    .section .note.GNU-stack,"",@progbits
