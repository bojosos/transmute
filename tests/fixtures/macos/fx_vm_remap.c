/* alchemy-fx engine fixture: Mach vm_remap of writable memory is a real
 * shared alias (fx shared guest memory), including code executed through a
 * read/execute alias while it is rewritten through the writable one; shared
 * memory remapped over mmap'd pages inside a large PROT_NONE reservation (as
 * Chrome maps a GPU ring buffer into V8's sandbox), with mprotect, munmap and
 * MAP_FIXED reuse there and code rewritten through either view; copy = TRUE
 * remaps (copy-on-write: a copy at the time of the remap), anywhere and over
 * Mach pages; vm_deallocate of mmap'd pages; Chrome's sequence for shared
 * memory in V8's sandbox (a memory entry mapped with mach_vm_map,
 * VM_FLAGS_OVERWRITE, over mmap'd pages, then its maximum protection set).
 * No SDK headers. Prints
 * "fx vm_remap: ok" and returns 0, else returns the failed step number. */
typedef unsigned long vm_address_t;
typedef unsigned long vm_size_t;
typedef int kern_return_t;
typedef unsigned mach_port_t;
extern mach_port_t mach_task_self_;
extern kern_return_t vm_allocate(mach_port_t, vm_address_t*, vm_size_t, int);
extern kern_return_t vm_deallocate(mach_port_t, vm_address_t, vm_size_t);
extern kern_return_t vm_protect(mach_port_t, vm_address_t, vm_size_t, int, int);
extern kern_return_t vm_remap(mach_port_t, vm_address_t*, vm_size_t, vm_address_t, int, mach_port_t, vm_address_t,
                              int, int*, int*, unsigned);
extern int puts(const char*);
extern void* mmap(void*, unsigned long, int, int, int, long);
extern int munmap(void*, unsigned long);
extern int mprotect(void*, unsigned long, int);
extern void sys_icache_invalidate(void*, unsigned long);
extern kern_return_t mach_make_memory_entry_64(mach_port_t, unsigned long*, unsigned long, int, mach_port_t*, mach_port_t);
extern kern_return_t mach_vm_map(mach_port_t, unsigned long*, unsigned long, unsigned long, int, mach_port_t, unsigned long,
                                 int, int, int, unsigned);
extern kern_return_t mach_port_deallocate(mach_port_t, mach_port_t);

#define PAGE 16384ul
#define ANYWHERE 1
#define OVERWRITE 0x4000
#define READ 1
#define WRITE 2
#define EXECUTE 4

typedef int (*function)(void);

static void put_code(volatile unsigned* at, unsigned value) {
    at[0] = 0x52800000u | (value << 5); /* mov w0, #value */
    at[1] = 0xd65f03c0u;                /* ret */
}

int main(void) {
    const mach_port_t task = mach_task_self_;
    vm_address_t a = 0, c = 0;
    int current = 0, maximum = 0;
    /* a: two pages; the second one becomes an alias of the first. */
    if (vm_allocate(task, &a, 2 * PAGE, ANYWHERE) != 0) return 1;
    volatile unsigned* first = (volatile unsigned*)a;
    volatile unsigned* second = (volatile unsigned*)(a + PAGE);
    first[3] = 0x1234;
    vm_address_t target = a + PAGE;
    if (vm_remap(task, &target, PAGE, 0, OVERWRITE, task, a, 0, &current, &maximum, 0) != 0) return 2;
    if (second[3] != 0x1234 || current != (READ | WRITE)) return 3;
    second[5] = 0x5678;
    if (first[5] != 0x5678) return 4;
    /* c: an alias anywhere, made read/execute: code runs from it. */
    put_code(first + 64, 42);
    if (vm_remap(task, &c, PAGE, 0, ANYWHERE, task, a, 0, &current, &maximum, 0) != 0) return 5;
    if (c == a || c == a + PAGE) return 6;
    if (vm_protect(task, c, PAGE, 0, READ | EXECUTE) != 0) return 7;
    function run = (function)(c + 64 * 4);
    if (run() != 42 || run() != 42) return 8;
    /* Rewritten through the writable mapping: the translation of the
     * read/execute alias must not survive. */
    put_code(first + 64, 7);
    if (run() != 7) return 9;
    /* The original pages go away; both aliases keep the memory. */
    if (vm_deallocate(task, a, PAGE) != 0) return 10;
    if (second[3] != 0x1234 || run() != 7) return 11;
    put_code(second + 64, 9);
    if (run() != 9) return 12;
    if (vm_deallocate(task, a + PAGE, PAGE) != 0 || vm_deallocate(task, c, PAGE) != 0) return 13;

    /* Shared memory over mmap'd pages: a 256 MiB PROT_NONE reservation (V8's
     * sandbox), some of it in use, and 64 KiB of shared memory remapped into
     * its untouched middle. */
    {
        const unsigned long big = 256ul << 20, ring = 4 * PAGE;
        char* space = (char*)mmap(0, big, 0 /* PROT_NONE */, 0x1002 /* MAP_PRIVATE | MAP_ANON */, -1, 0);
        if (space == (char*)-1) return 14;
        if (mprotect(space, 16 * PAGE, READ | WRITE) != 0) return 15;
        ((volatile unsigned*)space)[7] = 0x77;
        vm_address_t shm = 0;
        if (vm_allocate(task, &shm, ring, ANYWHERE) != 0) return 16;
        volatile unsigned* ring_source = (volatile unsigned*)shm;
        ring_source[1] = 0x5a5a0001;
        vm_address_t at = (vm_address_t)(space + (128ul << 20));
        if (vm_remap(task, &at, ring, 0, OVERWRITE, task, shm, 0, &current, &maximum, 0) != 0) return 17;
        if (at != (vm_address_t)(space + (128ul << 20))) return 18;
        volatile unsigned* ring_target = (volatile unsigned*)at;
        if (ring_target[1] != 0x5a5a0001 || current != (READ | WRITE)) return 19;
        ring_target[2] = 0x7a26e7;
        if (ring_source[2] != 0x7a26e7) return 20;
        if (((volatile unsigned*)space)[7] != 0x77) return 21;   /* the rest of the reservation kept its page */
        /* Code in the shared pages, run through the mmap'd view made
         * read/execute; rewritten through the Mach view. */
        put_code(ring_source + 256, 31);
        if (mprotect((void*)at, ring, READ | EXECUTE) != 0) return 22;
        function from_ring = (function)(at + 256 * 4);
        if (from_ring() != 31 || from_ring() != 31) return 23;
        put_code(ring_source + 256, 32);
        sys_icache_invalidate((void*)(at + 256 * 4), 8);
        if (from_ring() != 32) return 24;
        put_code(ring_source + 256, 33);   /* no flush: the write fault through the alias invalidates */
        if (from_ring() != 33) return 25;
        /* munmap of the alias leaves the reservation's range; MAP_FIXED takes it again, zero-filled. */
        if (munmap((void*)at, ring) != 0) return 26;
        if (ring_source[2] != 0x7a26e7) return 27;   /* the Mach view keeps the pages */
        char* again = (char*)mmap((void*)at, ring, READ | WRITE, 0x1012 /* MAP_PRIVATE | MAP_ANON | MAP_FIXED */, -1, 0);
        if (again != (char*)at || ((volatile unsigned*)again)[2] != 0) return 28;
        ((volatile unsigned*)again)[2] = 1;
        if (ring_source[2] != 0x7a26e7) return 29;
        /* A second alias, released with vm_deallocate (mmap'd pages: munmap). */
        vm_address_t at2 = (vm_address_t)(space + (64ul << 20));
        if (vm_remap(task, &at2, ring, 0, OVERWRITE, task, shm, 0, &current, &maximum, 0) != 0) return 30;
        if (((volatile unsigned*)at2)[1] != 0x5a5a0001) return 31;
        if (vm_deallocate(task, at2, ring) != 0) return 32;
        if (vm_deallocate(task, shm, ring) != 0) return 33;
        /* A memory entry mapped over mmap'd pages (mach_vm_map does vm_remap
         * and then vm_protect, current and maximum, on the target). */
        unsigned long entry_size = ring;
        mach_port_t entry = 0;
        if (mach_make_memory_entry_64(task, &entry_size, 0, 0x20003 /* MAP_MEM_NAMED_CREATE | RW */, &entry, 0) != 0) return 42;
        unsigned long mapped = (unsigned long)(space + (96ul << 20));
        if (mach_vm_map(task, &mapped, ring, 0, OVERWRITE, entry, 0, 0, READ | WRITE, READ | WRITE, 1) != 0) return 43;
        if (mapped != (unsigned long)(space + (96ul << 20))) return 44;
        ((volatile unsigned*)mapped)[3] = 0xbeef;
        unsigned long view = 0;
        if (mach_vm_map(task, &view, ring, 0, ANYWHERE, entry, 0, 0, READ, READ, 1) != 0) return 45;
        if (((volatile unsigned*)view)[3] != 0xbeef) return 46;   /* one memory, two mappings */
        if (vm_protect(task, mapped, ring, 1 /* set maximum */, READ) != 0) return 47;
        if (vm_deallocate(task, view, ring) != 0 || vm_deallocate(task, mapped, ring) != 0) return 48;
        mach_port_deallocate(task, entry);
        if (munmap(space, big) != 0) return 34;
    }

    /* copy = TRUE: a copy as of the remap, anywhere and over Mach pages. */
    {
        vm_address_t src = 0, dst = 0, over = 0;
        if (vm_allocate(task, &src, PAGE, ANYWHERE) != 0 || vm_allocate(task, &over, PAGE, ANYWHERE) != 0) return 35;
        ((volatile unsigned*)src)[4] = 0xc0ffee;
        if (vm_remap(task, &dst, PAGE, 0, ANYWHERE, task, src, 1 /* copy */, &current, &maximum, 0) != 0) return 36;
        if (((volatile unsigned*)dst)[4] != 0xc0ffee || current != (READ | WRITE)) return 37;
        ((volatile unsigned*)src)[4] = 1;
        ((volatile unsigned*)dst)[5] = 2;
        if (((volatile unsigned*)dst)[4] != 0xc0ffee || ((volatile unsigned*)src)[5] != 0) return 38;
        vm_address_t fixed = over;
        if (vm_remap(task, &fixed, PAGE, 0, OVERWRITE, task, src, 1, &current, &maximum, 0) != 0 || fixed != over) return 39;
        if (((volatile unsigned*)over)[4] != 1) return 40;
        if (vm_deallocate(task, src, PAGE) != 0 || vm_deallocate(task, dst, PAGE) != 0 ||
            vm_deallocate(task, over, PAGE) != 0) return 41;
    }
    puts("fx vm_remap: ok");
    return 0;
}
