// Synthetic probe for supported one-instruction prefixes separated by MSR.
// Includes profiled Blender ADRP/ADD words and a repeatedly called lone RET.
        .text
        .globl guest_entry
        .p2align 2
guest_entry:
        mov x22, x30
        movz x1, #0x0d40
        movk x1, #3, lsl #16
        movz x20, #0xa000, lsl #16
singleton_loop:
        .inst 0xf0002231
        msr nzcv, x20
        .inst 0x9100e108
        msr nzcv, x20
        .inst 0x910103ff
        msr nzcv, x20
        sub sp, sp, #64
        msr nzcv, x20
        .inst 0x90001568
        msr nzcv, x20
        bl singleton_return
        msr nzcv, x20
        sub x1, x1, #1
        msr nzcv, x20
        cbnz x1, singleton_loop
        mov x30, x22
        movz x0, #0
        ret
singleton_return:
        ret
