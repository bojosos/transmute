/* ARMv8.5 FRINTTS and ARMv8.2 FHM instructions reached by Embree and Blender
 * (Apple M1 baseline). No SDK or library imports. The exit status is a mask:
 * bit 0 frint32x v.4s, 1 frint32z v.4s, 2 frint64x v.2d, 3 frint32x v.2d,
 * 4 frint32x s (FPCR.RMode = RP), 5 fmlal, 6 fmlal2, 7 fmlsl. 0 = all exact. */
.macro check bit
    cmeq v2.4s, v0.4s, v1.4s
    uminv s2, v2.4s
    fmov w9, s2
    cmn w9, #1
    cset w10, ne
    orr w11, w11, w10, lsl #\bit
.endm

.section __TEXT,__text,regular,pure_instructions
.globl _main
.p2align 2
_main:
    mov w11, #0
    adrp x8, Ldata@PAGE
    add x8, x8, Ldata@PAGEOFF
    ldr q0, [x8, #0x00]
    .inst 0x6e21e800            /* frint32x v0.4s, v0.4s */
    ldr q1, [x8, #0x10]
    check 0
    ldr q0, [x8, #0x20]
    .inst 0x4e21e800            /* frint32z v0.4s, v0.4s */
    ldr q1, [x8, #0x30]
    check 1
    ldr q0, [x8, #0x40]
    .inst 0x6e61f800            /* frint64x v0.2d, v0.2d */
    ldr q1, [x8, #0x50]
    check 2
    ldr q0, [x8, #0x60]
    .inst 0x6e61e800            /* frint32x v0.2d, v0.2d */
    ldr q1, [x8, #0x70]
    check 3
    mrs x12, fpcr
    orr x13, x12, #0x400000     /* RMode = RP (toward +infinity) */
    msr fpcr, x13
    ldr q0, [x8, #0x80]
    .inst 0x1e28c000            /* frint32x s0, s0 */
    msr fpcr, x12
    ldr q1, [x8, #0x90]
    check 4
    ldr q4, [x8, #0xb0]
    ldr q5, [x8, #0xc0]
    ldr q0, [x8, #0xa0]
    .inst 0x4e25ec80            /* fmlal v0.4s, v4.4h, v5.4h */
    ldr q1, [x8, #0xd0]
    check 5
    movi v0.2d, #0
    .inst 0x6e25cc80            /* fmlal2 v0.4s, v4.4h, v5.4h */
    ldr q1, [x8, #0xe0]
    check 6
    ldr q0, [x8, #0xa0]
    .inst 0x4ea5ec80            /* fmlsl v0.4s, v4.4h, v5.4h */
    ldr q1, [x8, #0xf0]
    check 7
    mov w0, w11
    ret

.section __TEXT,__const
.p2align 4
Ldata:
    .long 0x40200000, 0xbfc00000, 0x4f32d05e, 0x7fc00000     /* 2.5, -1.5, 3e9, NaN */
    .long 0x40000000, 0xc0000000, 0xcf000000, 0xcf000000     /* 2, -2, -2^31, -2^31 */
    .long 0x402ccccd, 0xc02ccccd, 0x4effffff, 0x4f000000     /* 2.7, -2.7, 2^31-128, 2^31 */
    .long 0x40000000, 0xc0000000, 0x4effffff, 0xcf000000     /* 2, -2, 2^31-128, -2^31 */
    .quad 0x4004000000000000, 0x43e158e460913d00             /* 2.5, 1e19 */
    .quad 0x4000000000000000, 0xc3e0000000000000             /* 2, -2^63 */
    .quad 0xbfe0000000000000, 0x41f0000000000000             /* -0.5, 2^32 */
    .quad 0x8000000000000000, 0xc1e0000000000000             /* -0, -2^31 */
    .long 0x40200000, 0x11111111, 0x22222222, 0x33333333     /* 2.5 (upper lanes cleared) */
    .long 0x40400000, 0, 0, 0                                /* 3 */
    .long 0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000     /* accumulator 1.0 */
    .short 0x3e00, 0x4000, 0xc200, 0x3800, 0x3c00, 0x4000, 0x4200, 0x4400   /* 1.5 2 -3 .5 | 1 2 3 4 */
    .short 0x4000, 0x3400, 0x4400, 0x4200, 0x4000, 0x4000, 0x4000, 0x4000   /* 2 .25 4 3 | 2 2 2 2 */
    .long 0x40800000, 0x3fc00000, 0xc1300000, 0x40200000     /* fmlal: 4, 1.5, -11, 2.5 */
    .long 0x40000000, 0x40800000, 0x40c00000, 0x41000000     /* fmlal2: 2, 4, 6, 8 */
    .long 0xc0000000, 0x3f000000, 0x41500000, 0xbf000000     /* fmlsl: -2, .5, 13, -.5 */
