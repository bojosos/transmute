    .intel_syntax noprefix
    .text
    .globl simd_pair_encoding_proof
simd_pair_encoding_proof:
    vmovdqu ymm1, ymmword ptr [rcx + 0x138]
    vmovdqu ymm2, ymmword ptr [rcx + 0x158]
    vpand ymm0, ymm1, ymm2
    vpandn ymm0, ymm2, ymm1
    vpor ymm0, ymm1, ymm2
    vpxor ymm0, ymm1, ymm2
    vpaddb ymm0, ymm1, ymm2
    vpaddw ymm0, ymm1, ymm2
    vpaddd ymm0, ymm1, ymm2
    vpaddq ymm0, ymm1, ymm2
    vpsubb ymm0, ymm1, ymm2
    vpsubw ymm0, ymm1, ymm2
    vpsubd ymm0, ymm1, ymm2
    vpsubq ymm0, ymm1, ymm2
    vmovdqu ymmword ptr [rcx + 0xf8], ymm0
    vzeroupper
    ret
