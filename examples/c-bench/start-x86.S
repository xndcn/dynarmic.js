    .section .multiboot,"a"
    .align 4
    .long 0x1badb002
    .long 0x00000003
    .long -(0x1badb002 + 0x00000003)
    .section .text.start,"ax"
    .code32
    .global _start
_start:
    cli
    cld
    movl %cr0, %eax
    andl $~0x0c, %eax
    orl $0x22, %eax
    movl %eax, %cr0
    movl %cr4, %eax
    orl $0x600, %eax
    movl %eax, %cr4
    fninit
    fldcw x87_control
    ldmxcsr sse_control
    movl $_stack_top, %esp
    andl $-16, %esp
    xorl %ebp, %ebp
    call guest_main
    movw $0xe208, %dx
    outl %eax, %dx
1:
    hlt
    jmp 1b
    .section .rodata,"a"
    .align 4
sse_control: .long 0x1f80
x87_control: .word 0x027f
    .section .note.GNU-stack,"",@progbits
