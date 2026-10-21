#ifndef ALCHEMY_MACHO_LOADER_H
#define ALCHEMY_MACHO_LOADER_H
/* Mach-O file-format structures as documented by the published Mach-O ABI
   (the format Alchemy's own loader parses). Independently authored
   declarations; no SDK content. */
#include <stdint.h>
typedef int cpu_type_t;
typedef int cpu_subtype_t;
typedef int vm_prot_t;
struct mach_header {
    uint32_t magic;
    cpu_type_t cputype;
    cpu_subtype_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
};
struct mach_header_64 {
    uint32_t magic;
    cpu_type_t cputype;
    cpu_subtype_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
};
#define MH_MAGIC 0xfeedface
#define MH_CIGAM 0xcefaedfe
#define MH_MAGIC_64 0xfeedfacf
#define MH_CIGAM_64 0xcffaedfe
#define MH_OBJECT 0x1
#define MH_EXECUTE 0x2
#define MH_DYLIB 0x6
#define MH_DYLINKER 0x7
#define MH_BUNDLE 0x8
struct load_command { uint32_t cmd; uint32_t cmdsize; };
#define LC_REQ_DYLD 0x80000000
#define LC_SEGMENT 0x1
#define LC_SYMTAB 0x2
#define LC_DYSYMTAB 0xb
#define LC_LOAD_DYLIB 0xc
#define LC_ID_DYLIB 0xd
#define LC_SEGMENT_64 0x19
#define LC_UUID 0x1b
#define LC_RPATH (0x1c | LC_REQ_DYLD)
#define LC_BUILD_VERSION 0x32
struct segment_command_64 {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    vm_prot_t maxprot;
    vm_prot_t initprot;
    uint32_t nsects;
    uint32_t flags;
};
struct section_64 {
    char sectname[16];
    char segname[16];
    uint64_t addr;
    uint64_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
    uint32_t reserved3;
};
struct uuid_command { uint32_t cmd; uint32_t cmdsize; uint8_t uuid[16]; };
struct symtab_command { uint32_t cmd, cmdsize, symoff, nsyms, stroff, strsize; };
struct dysymtab_command {
    uint32_t cmd, cmdsize;
    uint32_t ilocalsym, nlocalsym, iextdefsym, nextdefsym, iundefsym, nundefsym;
    uint32_t tocoff, ntoc, modtaboff, nmodtab, extrefsymoff, nextrefsyms;
    uint32_t indirectsymoff, nindirectsyms, extreloff, nextrel, locreloff, nlocrel;
};
#define INDIRECT_SYMBOL_LOCAL 0x80000000
#define INDIRECT_SYMBOL_ABS 0x40000000
#define SECTION_TYPE 0x000000ff
#define S_NON_LAZY_SYMBOL_POINTERS 0x6
#define S_LAZY_SYMBOL_POINTERS 0x7
#define SEG_TEXT "__TEXT"
#define SEG_DATA "__DATA"
#define SEG_LINKEDIT "__LINKEDIT"
#endif
