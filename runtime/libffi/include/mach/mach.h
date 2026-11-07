/* The Mach VM interface libffi's trampoline table uses (vm_allocate, vm_remap, vm_protect,
   vm_deallocate on the calling task), as the Darwin arm64 LP64 ABI declares it. Alchemy-authored
   declarations; the functions are served by Alchemy's libSystem provider. */
#ifndef ALCHEMY_MACH_MACH_H
#define ALCHEMY_MACH_MACH_H

#include <stdint.h>
#include <mach/vm_param.h>

typedef int kern_return_t;
typedef int boolean_t;
typedef unsigned int mach_port_t;
typedef mach_port_t vm_map_t;
typedef uintptr_t vm_address_t;
typedef uintptr_t vm_size_t;
typedef int vm_prot_t;
typedef unsigned int vm_inherit_t;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#define KERN_SUCCESS 0
#define VM_FLAGS_FIXED 0x0000
#define VM_FLAGS_ANYWHERE 0x0001
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_PROT_NONE 0x0
#define VM_PROT_READ 0x1
#define VM_PROT_WRITE 0x2
#define VM_PROT_EXECUTE 0x4
#define VM_INHERIT_SHARE 0
#define VM_INHERIT_COPY 1
#define VM_INHERIT_NONE 2

extern mach_port_t mach_task_self_;
#define mach_task_self() mach_task_self_

kern_return_t vm_allocate(vm_map_t target_task, vm_address_t* address, vm_size_t size, int flags);
kern_return_t vm_deallocate(vm_map_t target_task, vm_address_t address, vm_size_t size);
kern_return_t vm_protect(vm_map_t target_task, vm_address_t address, vm_size_t size, boolean_t set_maximum,
    vm_prot_t new_protection);
kern_return_t vm_remap(vm_map_t target_task, vm_address_t* target_address, vm_size_t size, vm_address_t mask,
    int flags, vm_map_t src_task, vm_address_t src_address, boolean_t copy, vm_prot_t* cur_protection,
    vm_prot_t* max_protection, vm_inherit_t inheritance);

#endif
