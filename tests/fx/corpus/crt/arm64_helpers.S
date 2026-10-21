// fx corpus ARM64 CRT: assembly helpers that live in the static vcruntime.lib.

    .text

// __chkstk: x15 = allocation size / 16. Touches every page between SP and
// SP - size, top down, so the guard page grows the stack. Preserves every
// register except x16, x17 and the flags; the caller then subtracts x15 * 16.
    .globl  __chkstk
    .p2align 2
__chkstk:
    lsl     x16, x15, #4
    mov     x17, sp
1:
    sub     x17, x17, #4096
    subs    x16, x16, #4096
    ldr     xzr, [x17]
    b.gt    1b
    ret

// _setjmpex(jmp_buf, frame): clang lowers setjmp() on ARM64 Windows to this
// symbol (x1 = SP at function entry). vcruntime140.dll exports the
// implementation as __intrinsic_setjmpex; forward with a plain branch so the
// caller's LR, SP and callee-saved registers reach it untouched.
    .globl  _setjmpex
    .p2align 2
_setjmpex:
    adrp    x16, __imp___intrinsic_setjmpex
    ldr     x16, [x16, :lo12:__imp___intrinsic_setjmpex]
    br      x16

// const type_info::`vftable' (??_7type_info@@6B@). RTTI type descriptors
// point at it; the static vcruntime.lib normally provides it. Slot 0 is the
// scalar deleting destructor; the word before it is the (unused) complete
// object locator.
    .section .rdata,"dr"
    .p2align 3
    .xword  0
    .globl  "??_7type_info@@6B@"
"??_7type_info@@6B@":
    .xword  fx_type_info_deleting_dtor
