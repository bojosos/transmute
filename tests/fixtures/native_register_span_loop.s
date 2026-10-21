// Fixed process probe for native register spans. The load and back edge bound
// an eight-word pure span plus SUBS. The return checks every simple accumulator.
    .text
    .globl guest_entry
    .p2align 2
guest_entry:
    sub sp, sp, #32
    mov x1, #7
    str x1, [sp, #16]
    add x4, sp, #16
    str x4, [sp]
    mov x4, sp
    mov x0, #5
    mov x5, #0
    mov x6, #0
    mov x7, #0
    mov x8, #0
    mov x9, #0
    mov x10, #0
    mov x11, #0
    movz x3, #0x2d00
    movk x3, #0x131, lsl #16 // 20,000,000 iterations
    ldr x1, [x4]
loop:
    ldr x2, [x1]
    add x0, x0, x2
    add x5, x5, #3
    eor x6, x6, x2
    add x7, x7, #5
    eor x8, x8, x0
    add x9, x9, #11
    eor x10, x10, x5
    add x11, x11, #13
    subs x3, x3, #1
    b.ne loop
    movz x1, #0x3b05
    movk x1, #0x858, lsl #16 // 140,000,005
    cmp x0, x1
    b.ne failed
    movz x1, #0x8700
    movk x1, #0x393, lsl #16 // 60,000,000
    cmp x5, x1
    b.ne failed
    movz x1, #0xe100
    movk x1, #0x5f5, lsl #16 // 100,000,000
    cmp x7, x1
    b.ne failed
    movz x1, #0xef00
    movk x1, #0xd1c, lsl #16 // 220,000,000
    cmp x9, x1
    b.ne failed
    movz x1, #0x4900
    movk x1, #0xf7f, lsl #16 // 260,000,000
    cmp x11, x1
    b.ne failed
    cbnz x6, failed
    mov w0, #0
    b done
failed:
    mov w0, #1
done:
    add sp, sp, #32
    ret
