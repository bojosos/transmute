#ifndef ALCHEMY_DYLD_H
#define ALCHEMY_DYLD_H
#include <stdint.h>
struct mach_header;
#ifdef __cplusplus
extern "C" {
#endif
uint32_t _dyld_image_count(void);
const struct mach_header* _dyld_get_image_header(uint32_t);
intptr_t _dyld_get_image_vmaddr_slide(uint32_t);
const char* _dyld_get_image_name(uint32_t);
void _dyld_register_func_for_add_image(void (*)(const struct mach_header*, intptr_t));
void _dyld_register_func_for_remove_image(void (*)(const struct mach_header*, intptr_t));
#ifdef __cplusplus
}
#endif
#endif
