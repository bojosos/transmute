.section __TEXT,__text,regular,pure_instructions
.globl _main
.p2align 2
_main:
    mov x0, #1
    adrp x1, message@PAGE
    add x1, x1, message@PAGEOFF
    mov x2, #14
    mov x16, #4
    svc #0x80
    b.cs failure
    cmp x0, #14
    b.ne failure
    mov x0, #0
    mov x16, #1
    svc #0x80
    brk #0
failure:
    mov x0, #13
    mov x16, #1
    svc #0x80
    brk #0
.section __TEXT,__cstring,cstring_literals
message:
    .asciz "syscall hello\n"
