/* Exact instruction required by bundled CPython, without its dependencies. */
.section __TEXT,__text,regular,pure_instructions
.globl _main
.p2align 2
_main:
    adrp x8, _rcpc_value@PAGE
    add x8, x8, _rcpc_value@PAGEOFF
    .inst 0xb8bfc108 /* ldapr w8, [x8] */
    cmp w8, #27
    cset w0, ne
    ret
.section __DATA,__data
.p2align 2
_rcpc_value:
    .long 27
