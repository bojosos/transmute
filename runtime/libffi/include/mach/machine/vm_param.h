/* Darwin arm64 page-size constants (16 KiB pages), for C and assembly. Alchemy-authored. */
#ifndef ALCHEMY_MACH_MACHINE_VM_PARAM_H
#define ALCHEMY_MACH_MACHINE_VM_PARAM_H

#define PAGE_MAX_SHIFT 14
#define PAGE_MAX_SIZE (1 << PAGE_MAX_SHIFT)
#define PAGE_MIN_SHIFT 12
#define PAGE_MIN_SIZE (1 << PAGE_MIN_SHIFT)

#endif
