.text
ldp w4, w5, [x0]
stp w4, w5, [x0]
ldp x4, x5, [x0]
stp x4, x5, [x0]
ldr b4, [x0]
str b4, [x0]
ldr h4, [x0]
str h4, [x0]
ldr s4, [x0]
str s4, [x0]
ldr d4, [x0]
str d4, [x0]
ldp s4, s5, [x0]
stp s4, s5, [x0]
ldp d4, d5, [x0]
stp d4, d5, [x0]
ldr q4, [x0]
str q4, [x0]
ldp q4, q5, [x0]
stp q4, q5, [x0]
fmov x4, d4
fmov x5, d5
fmov d4, x1
fmov d5, x1
fmov d4, x2
fmov d5, x2
mov d6, v4.d[1]
fmov x7, d6
mov d7, v5.d[1]
fmov x8, d7
dup v4.2d, x1
dup v5.2d, x1
dup v4.2d, x2
dup v5.2d, x2
