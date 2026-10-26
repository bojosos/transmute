/* alchemy-fx engine fixture: anonymous memory Darwin commits on first touch
 * (V8's code range: a MAP_JIT reservation made read/write/execute in one
 * mprotect) and Mach vm_remap of image text over part of such a reservation
 * (V8 remapping its embedded builtins into the code range). Resident pages
 * come from mach_vm_region_recurse. No SDK headers. Prints "fx vm lazy: ok"
 * and returns 0, else returns the failed step number. */
typedef unsigned long size_t;
typedef unsigned long vm_address_t;
typedef unsigned long vm_size_t;
typedef int kern_return_t;
typedef unsigned mach_port_t;
typedef void* pthread_t;
extern mach_port_t mach_task_self_;
extern void* mmap(void*, size_t, int, int, int, long);
extern int munmap(void*, size_t);
extern int mprotect(void*, size_t, int);
extern int madvise(void*, size_t, int);
extern int puts(const char*);
extern int open(const char*, int, ...);
extern int close(int);
extern long pwrite(int, const void*, size_t, long);
extern long pread(int, void*, size_t, long);
extern int unlink(const char*);
extern int getpid(void);
extern int pthread_create(pthread_t*, const void*, void* (*)(void*), void*);
extern int pthread_join(pthread_t, void**);
extern kern_return_t mach_vm_region_recurse(mach_port_t, unsigned long*, unsigned long*, unsigned*, int*, unsigned*);
extern kern_return_t vm_remap(mach_port_t, vm_address_t*, vm_size_t, vm_address_t, int, mach_port_t, vm_address_t,
                              int, int*, int*, unsigned);

#define PAGE 16384ul
#define MiB (1ul << 20)
#define GiB (1ul << 30)
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define RWX (PROT_READ | PROT_WRITE | PROT_EXEC)
#define MAP_PRIVATE 0x2
#define MAP_JIT 0x800
#define MAP_ANON 0x1000
#define MAP_FAILED ((void*)-1)
#define MADV_FREE 5
#define ANON (MAP_PRIVATE | MAP_ANON)
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2

typedef int (*function)(void);

/* A page of image text that never changes, as V8's embedded builtins. */
__asm__(".text\n"
        ".p2align 14\n"
        ".globl _builtin_page\n"
        "_builtin_page:\n"
        "  mov w0, #77\n"
        "  ret\n"
        ".p2align 14\n");
extern int builtin_page(void);

static void put_code(volatile unsigned* at, unsigned value) {
    at[0] = 0x52800000u | (value << 5); /* mov w0, #value */
    at[1] = 0xd65f03c0u;                /* ret */
}

/* The region holding `address`: its base, size and protection; returns its
 * resident (committed) pages, or -1. */
static int region(unsigned long address, unsigned long* base, unsigned long* size, int* protection) {
    int info[19] = {0};
    unsigned depth = 0, count = 19;
    *base = address;
    if (mach_vm_region_recurse(mach_task_self_, base, size, &depth, info, &count) != 0) return -1;
    *protection = info[0];
    return info[6]; /* pages_resident */
}

static int resident(char* address) {
    unsigned long base, size;
    int protection;
    return region((unsigned long)address, &base, &size, &protection);
}

/* Four threads write their values into the same untouched pages at once. */
static volatile char* shared_pages;
static void* toucher(void* argument) {
    const unsigned long id = (unsigned long)argument;
    for (unsigned long page = 0; page < 64; ++page) shared_pages[page * PAGE + id * 8] = (char)(id + page);
    return 0;
}

int main(void) {
    const mach_port_t task = mach_task_self_;
    unsigned long base = 0, size = 0;
    int protection = 0;

    /* 1. A 1 GiB MAP_JIT reservation made RWX in one call commits nothing. */
    char* range = mmap(0, GiB, PROT_NONE, ANON | MAP_JIT, -1, 0);
    if (range == MAP_FAILED) return 1;
    if (mprotect(range, GiB, RWX) != 0) return 2;
    if (region((unsigned long)range, &base, &size, &protection) != 0) return 3;
    if (base != (unsigned long)range || size != GiB || protection != RWX) return 4;

    /* 2. Three touched pages are resident, and read back. */
    range[5] = 1;
    range[512 * MiB + 7] = 2;
    range[GiB - PAGE + 9] = 3;
    if (range[5] != 1 || range[512 * MiB + 7] != 2 || range[GiB - PAGE + 9] != 3) return 5;
    if (resident(range) != 3) return 6;
    /* A read of an untouched page sees zeros. */
    if (range[256 * MiB + 100] != 0 || resident(range) != 4) return 7;

    /* 3. Code written into a touched page and into an untouched one runs. */
    put_code((volatile unsigned*)(range + 512 * MiB + 64), 11);
    if (((function)(range + 512 * MiB + 64))() != 11) return 8;
    put_code((volatile unsigned*)(range + 768 * MiB), 12);
    if (((function)(range + 768 * MiB))() != 12) return 9;
    put_code((volatile unsigned*)(range + 768 * MiB), 13);   /* rewritten after translation */
    if (((function)(range + 768 * MiB))() != 13) return 10;
    if (resident(range) != 5) return 11;

    /* 4. Providers read and write untouched pages (pwrite from one, pread into another). */
    {
        static char name[] = "/tmp/fx_vm_lazy_00000000.bin";
        int pid = getpid();
        for (int i = 0; i < 8; ++i) name[sizeof(name) - 6 - i] = (char)('0' + (pid >> (4 * i)) % 10);
        int fd = open(name, 0x2 | 0x200 | 0x400, 0600);   /* O_RDWR | O_CREAT | O_TRUNC */
        if (fd < 0) return 12;
        if (pwrite(fd, range + 128 * MiB, 64, 0) != 64) return 13;   /* zeros from an untouched page */
        static const char text[] = "lazy pages";
        if (pwrite(fd, text, sizeof(text), 64) != (long)sizeof(text)) return 14;
        if (pread(fd, range + 384 * MiB + 3, 64 + sizeof(text), 0) != 64 + (long)sizeof(text)) return 15;
        for (unsigned i = 0; i < 64; ++i)
            if (range[384 * MiB + 3 + i] != 0) return 16;
        for (unsigned i = 0; i < sizeof(text); ++i)
            if (range[384 * MiB + 3 + 64 + i] != text[i]) return 17;
        close(fd);
        unlink(name);
    }
    if (resident(range) != 7) return 18;

    /* 5. Threads racing on the first touch of the same pages. */
    shared_pages = range + 600 * MiB;
    {
        pthread_t threads[4];
        for (unsigned long i = 0; i < 4; ++i)
            if (pthread_create(&threads[i], 0, toucher, (void*)i) != 0) return 19;
        for (unsigned i = 0; i < 4; ++i) pthread_join(threads[i], 0);
        for (unsigned long page = 0; page < 64; ++page)
            for (unsigned long id = 0; id < 4; ++id)
                if (shared_pages[page * PAGE + id * 8] != (char)(id + page)) return 20;
    }
    if (resident(range) != 7 + 64) return 21;

    /* 6. MADV_FREE gives a touched page back; it reads as before or as zeros. */
    if (madvise(range + GiB - PAGE, PAGE, MADV_FREE) != 0) return 22;
    if (resident(range) != 7 + 64 - 1) return 23;
    if (range[GiB - PAGE + 9] != 3 && range[GiB - PAGE + 9] != 0) return 24;
    range[GiB - PAGE + 9] = 4;
    if (range[GiB - PAGE + 9] != 4) return 25;

    /* 7. Protection changes cover touched and untouched pages alike. */
    if (mprotect(range, GiB, PROT_READ | PROT_EXEC) != 0) return 26;
    if (region((unsigned long)range + 100 * MiB, &base, &size, &protection) < 0 || protection != (PROT_READ | PROT_EXEC))
        return 27;
    if (range[5] != 1 || range[300 * MiB] != 0 || ((function)(range + 768 * MiB))() != 13) return 28;
    if (mprotect(range + 300 * MiB, PAGE, PROT_READ | PROT_WRITE) != 0) return 29;
    range[300 * MiB + 1] = 6;
    if (range[300 * MiB + 1] != 6) return 30;
    if (region((unsigned long)range + 300 * MiB, &base, &size, &protection) < 0 ||
        base != (unsigned long)range + 300 * MiB || size != PAGE || (protection & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE))
        return 31;
    if (mprotect(range + 2 * PAGE, PAGE, PROT_NONE) != 0) return 32;
    if (region((unsigned long)range + 2 * PAGE, &base, &size, &protection) < 0 || protection != PROT_NONE ||
        base != (unsigned long)range + 2 * PAGE || size != PAGE) return 33;
    if (munmap(range, GiB) != 0) return 34;

    /* 8. A large anonymous mapping with access commits on first touch too.
     * Placed where the code range was: none of its pages is code any more
     * (also the ones that were never touched). */
    char* data = mmap(range + 64 * MiB, 64 * MiB, PROT_READ | PROT_WRITE, ANON, -1, 0);
    if (data == MAP_FAILED || resident(data) != 0) return 35;
    data[40 * MiB] = 9;
    if (resident(data) != 1 || data[40 * MiB] != 9) return 36;
    if (region((unsigned long)data, &base, &size, &protection) != 1 || base != (unsigned long)data || size != 64 * MiB ||
        protection != (PROT_READ | PROT_WRITE)) return 49;
    if (munmap(data, 64 * MiB) != 0) return 50;

    /* 9. V8's embedded builtins: vm_remap(VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
     * copy = FALSE, VM_INHERIT_NONE) of image text over part of its code range. */
    char* code = mmap(0, 32 * MiB, PROT_NONE, ANON | MAP_JIT, -1, 0);
    if (code == MAP_FAILED || mprotect(code, 32 * MiB, RWX) != 0) return 37;
    vm_address_t target = (vm_address_t)(code + MiB);
    int current = 0, maximum = 0;
    const vm_address_t source = (vm_address_t)&builtin_page;
    if (source % PAGE) return 38;
    if (vm_remap(task, &target, PAGE, 0, VM_FLAGS_OVERWRITE, task, source, 0, &current, &maximum, VM_INHERIT_NONE) != 0) return 39;
    if (target != (vm_address_t)(code + MiB) || current != (PROT_READ | PROT_EXEC)) return 40;
    if (((volatile unsigned*)target)[0] != ((volatile unsigned*)source)[0]) return 41;
    if (((function)target)() != 77) return 42;
    if (region(target, &base, &size, &protection) < 0 || base != target || size != PAGE ||
        protection != (PROT_READ | PROT_EXEC)) return 43;
    if (region((unsigned long)code, &base, &size, &protection) < 0 || protection != RWX || base != (unsigned long)code ||
        size != MiB) return 44;
    /* The copy is ordinary mmap'd memory afterwards. */
    if (mprotect((void*)target, PAGE, RWX) != 0) return 45;
    put_code((volatile unsigned*)target, 78);
    if (((function)target)() != 78) return 46;
    /* A copy-on-write remap (copy = TRUE) is a copy as of the remap. */
    target = (vm_address_t)(code + 2 * MiB);
    if (vm_remap(task, &target, PAGE, 0, VM_FLAGS_OVERWRITE, task, source, 1, &current, &maximum, VM_INHERIT_NONE) != 0 ||
        target != (vm_address_t)(code + 2 * MiB) || current != (PROT_READ | PROT_EXEC) ||
        ((volatile unsigned*)target)[0] != ((volatile unsigned*)source)[0] || ((function)target)() != 77) return 47;
    if (munmap(code, 32 * MiB) != 0) return 48;

    puts("fx vm lazy: ok");
    return 0;
}
