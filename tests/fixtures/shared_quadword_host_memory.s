.intel_syntax noprefix
.text
movq qword ptr [r10], xmm0
movhps qword ptr [r10 + 8], xmm0
movq qword ptr [r10 + 16], xmm1
movhps qword ptr [r10 + 24], xmm1
movq xmm0, qword ptr [rdx]
movhps xmm0, qword ptr [rdx + 8]
movq xmm1, qword ptr [rdx + 16]
movhps xmm1, qword ptr [rdx + 24]
