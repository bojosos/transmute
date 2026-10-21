.text
// These vector 1D arrangements must be rejected by the ARM64 assembler.
add v0.1d, v1.1d, v2.1d
sub v0.1d, v1.1d, v2.1d
neg v0.1d, v1.1d
cmeq v0.1d, v1.1d, v2.1d
cmeq v0.1d, v1.1d, #0
cmtst v0.1d, v1.1d, v2.1d
