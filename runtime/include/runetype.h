#ifndef ALCHEMY_RUNETYPE_H
#define ALCHEMY_RUNETYPE_H
#include <stdint.h>
typedef struct { int minimum, maximum, mapping; uint32_t* types; } _RuneEntry;
typedef struct { int count; _RuneEntry* ranges; } _RuneRange;
typedef struct {
    char magic[8];
    char encoding[32];
    int (*getrune)(const char*, unsigned long, const char**);
    int (*putrune)(int, char*, unsigned long, char**);
    int invalid;
    uint32_t __runetype[256];
    int __maplower[256];
    int __mapupper[256];
    _RuneRange runetype_ext, maplower_ext, mapupper_ext;
    void* variable;
    int variable_length;
    int charclass_count;
    void* charclasses;
} _RuneLocale;
#ifdef __cplusplus
extern "C" {
#endif
extern _RuneLocale _DefaultRuneLocale;
#ifdef __cplusplus
}
#endif
#endif
