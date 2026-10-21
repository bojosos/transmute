#ifndef ALCHEMY_DLFCN_H
#define ALCHEMY_DLFCN_H
typedef struct { const char* dli_fname; void* dli_fbase; const char* dli_sname; void* dli_saddr; } Dl_info;
#ifdef __cplusplus
extern "C" {
#endif
void* dlopen(const char*, int);
void* dlsym(void*, const char*);
int dlclose(void*);
char* dlerror(void);
int dladdr(const void*, Dl_info*);
#ifdef __cplusplus
}
#endif
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 4
#define RTLD_GLOBAL 8
#define RTLD_NOLOAD 16
#define RTLD_NODELETE 128
#define RTLD_FIRST 256
#define RTLD_DEFAULT ((void*)-2)
#define RTLD_NEXT ((void*)-1)
#endif
