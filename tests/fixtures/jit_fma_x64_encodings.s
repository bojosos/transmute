.intel_syntax noprefix
.text
vfmadd213ss xmm0, xmm1, xmm2
vfmadd213sd xmm0, xmm1, xmm2
vfnmadd213ss xmm0, xmm1, xmm2
vfnmadd213sd xmm0, xmm1, xmm2
vfnmsub213ss xmm0, xmm1, xmm2
vfnmsub213sd xmm0, xmm1, xmm2
vfmsub213ss xmm0, xmm1, xmm2
vfmsub213sd xmm0, xmm1, xmm2
vfmadd213ps xmm0, xmm1, xmm2
vfmadd213pd xmm0, xmm1, xmm2
vfnmadd213ps xmm0, xmm1, xmm2
vfnmadd213pd xmm0, xmm1, xmm2
vzeroupper
