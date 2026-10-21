// MIT-licensed Alchemy ARM64 message dispatcher. No Apple runtime code.
//
// Three entry points share one per-class method cache probe and one slow path:
//   alchemy_objc_dispatch         objc_msgSend       (x0 = receiver)
//   alchemy_objc_dispatch_super2  objc_msgSendSuper2 (x0 = {receiver, current class})
//   alchemy_objc_dispatch_super   objc_msgSendSuper  (x0 = {receiver, start class})
//
// Method cache (written only by the host runtime, under its lock):
//   class + 16: (mask << 48) | buckets, 0 while the class has no cache
//   class + 24: the class's own address once it has had a cache (a check word
//               that rejects isa values that are not runtime classes)
//   buckets:    mask + 1 entries of {SEL, IMP}; an empty entry has SEL 0.
//               A writer stores the IMP before the SEL and never changes an
//               occupied entry; flushes and growth publish a new bucket array.
// The probe uses only x9-x17 and preserves x0-x8, q0-q7, SP and LR; on a hit
// x0 holds the receiver and control branches to the IMP through x16.
//
// The host lookup helper accepts (receiver, selector, super-mode), returning
// IMP. On a miss x0-x8, q0-q7, the original SP and LR survive the lookup;
// x15 carries the super mode across it. Nil receivers return zero in x0, x1
// and v0-v3; tagged pointers take the slow path, which rejects them.
.text
.p2align 2
.global alchemy_objc_dispatch
alchemy_objc_dispatch:
    mov x15, #0
    cmp x0, #0
    b.le Lnil_or_tagged
    mov x14, x0
    ldr x13, [x0]
Lprobe:
    ldp x10, x11, [x13, #16]
    cmp x11, x13
    b.ne Lslow
    lsr x11, x10, #48
    ands x10, x10, #0xffffffffffff
    b.eq Lslow
    lsr x12, x1, #3
    eor x12, x12, x1, lsr #13
Lloop:
    and x12, x12, x11
    add x9, x10, x12, lsl #4
    ldr x17, [x9]
    cmp x17, x1
    b.ne Lnext
    ldr x16, [x9, #8]
    cbz x16, Lslow
    mov x0, x14
    br x16
Lnext:
    cbz x17, Lslow
    add x12, x12, #1
    b Lloop
Lnil_or_tagged:
    b.lt Lslow
    mov x1, #0
    movi v0.16b, #0
    movi v1.16b, #0
    movi v2.16b, #0
    movi v3.16b, #0
    ret

.global alchemy_objc_dispatch_super2
alchemy_objc_dispatch_super2:
    mov x15, #1
    ldp x14, x13, [x0]
    cbz x14, Lslow
    ldr x13, [x13, #8]
    cbz x13, Lslow
    b Lprobe

.global alchemy_objc_dispatch_super
alchemy_objc_dispatch_super:
    mov x15, #2
    ldp x14, x13, [x0]
    cbz x14, Lslow
    cbz x13, Lslow
    b Lprobe

Lslow:
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x30, [sp, #64]
    stp q0, q1, [sp, #80]
    stp q2, q3, [sp, #112]
    stp q4, q5, [sp, #144]
    stp q6, q7, [sp, #176]
    str x15, [sp, #208]
    mov x2, x15
    ldr x16, lookup_address
    blr x16
    mov x16, x0
    ldr x15, [sp, #208]
    ldp q0, q1, [sp, #80]
    ldp q2, q3, [sp, #112]
    ldp q4, q5, [sp, #144]
    ldp q6, q7, [sp, #176]
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x30, [sp, #64]
    add sp, sp, #224
    cbz x15, Lcall
    ldr x0, [x0]
Lcall:
    br x16
.p2align 3
lookup_address: .quad 0

.global alchemy_objc_nil
alchemy_objc_nil:
    mov x0, #0
    mov x1, #0
    movi v0.16b, #0
    movi v1.16b, #0
    movi v2.16b, #0
    movi v3.16b, #0
    ret
