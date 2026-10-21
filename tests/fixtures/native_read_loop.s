// A fixed dependent-load loop for comparing whole runtime checkpoints.
// All memory is guest stack memory; no host bridge calls enter the hot loop.
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
    movz x3, #0xe100
    movk x3, #0x5f5, lsl #16 // 100,000,000 iterations
    ldr x1, [x4]
loop:
    ldr x2, [x1]
    add x0, x0, x2
    subs x3, x3, #1
    b.ne loop
    movz x1, #0x2705
    movk x1, #0x29b9, lsl #16 // expected sum 700,000,005
    cmp x0, x1
    cset w0, ne
    add sp, sp, #32
    ret
