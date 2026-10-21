/* Exact CRC instruction reached by bundled zlib. No SDK or library imports. */
.section __TEXT,__text,regular,pure_instructions
.globl _main
.p2align 2
_main:
    mov w8, #-1
    movz x13, #0x3231
    movk x13, #0x3433, lsl #16
    movk x13, #0x3635, lsl #32
    movk x13, #0x3837, lsl #48
    .inst 0x9acd4d08 /* crc32x w8, w8, x13, bytes "12345678" */
    mvn w8, w8
    movz w9, #0xdaaf
    movk w9, #0x9ae0, lsl #16
    cmp w8, w9
    cset w0, ne
    ret
