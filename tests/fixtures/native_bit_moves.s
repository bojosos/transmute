.text
.globl guest_entry
guest_entry:
    dup v0.16b, w1
    dup v2.8b, v3.b[15]
    dup v4.4h, v5.h[7]
    dup v6.2s, v7.s[3]
    dup v8.2d, v9.d[1]
    dup b10, v11.b[15]
    dup h12, v13.h[7]
    dup s14, v15.s[3]
    dup d16, v17.d[1]
    movi d16, #0
    movi v18.4s, #0x55, lsl #24
    mvni v19.4s, #0x80, msl #16
    orr v20.8h, #0xa5, lsl #8
    bic v21.4s, #0xff, lsl #16
    fmov s22, #1.0
    fmov d23, #-2.0
    fmov s24, s25
    fmov d26, d27
    fmov s28, wzr
    fmov d29, xzr
    fmov wzr, s30
    fmov xzr, d31
    fabs s0, s1
    fneg d2, d3
    fcsel s0, s16, s0, eq
    fcsel d4, d5, d6, gt
    fcsel d7, d8, d9, nv
    ret
