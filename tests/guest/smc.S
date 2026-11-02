.text
.global _start
_start:
    mov r4, #100
    mov r5, #0
loop:
    bl func
    add r5, r5, r0
    subs r4, r4, #1
    bne loop
    svc #0
func:
    mov r0, #1
    bx lr
