.text
.intel_syntax noprefix
ucomiss xmm0, xmm1
ucomisd xmm0, xmm1
comiss xmm0, xmm1
comisd xmm0, xmm1
mov r8d, 0x20000000
mov edx, 0x80000000
cmovb r8d, edx
mov edx, 0x60000000
cmove r8d, edx
mov edx, 0x30000000
cmovp r8d, edx
