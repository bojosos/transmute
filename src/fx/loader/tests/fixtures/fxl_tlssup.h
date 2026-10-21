// Minimal TLS support (normally the CRT's tlssup.obj) for loader fixtures
// built with /nodefaultlib. Define FXL_TLS_ZERO_FILL before including.
#ifndef FXL_TLS_ZERO_FILL
#define FXL_TLS_ZERO_FILL 0
#endif

typedef void (*fxl_tls_callback)(void* module, unsigned long reason, void* reserved);

typedef struct {
    void* start;
    void* end;
    unsigned long* index;
    fxl_tls_callback* callbacks;
    unsigned long zero_fill;
    unsigned long characteristics;
} fxl_tls_directory;

#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
#pragma section(".CRT$XLA", read)
#pragma section(".CRT$XLB", read)
#pragma section(".CRT$XLZ", read)

unsigned long _tls_index = 0;
__declspec(allocate(".tls")) char _tls_start = 0;
__declspec(allocate(".tls$ZZZ")) char _tls_end = 0;
__declspec(allocate(".CRT$XLA")) fxl_tls_callback __xl_a = 0;
__declspec(allocate(".CRT$XLZ")) fxl_tls_callback __xl_z = 0;

const fxl_tls_directory _tls_used = {
    &_tls_start, &_tls_end, &_tls_index, &__xl_a + 1, FXL_TLS_ZERO_FILL, 0,
};

#define FXL_TLS_CALLBACK(fn) \
    __declspec(allocate(".CRT$XLB")) fxl_tls_callback fn##_entry = fn
