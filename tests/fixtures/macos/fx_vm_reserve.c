/* alchemy-fx engine fixture: the Darwin VM patterns PartitionAlloc and V8
 * rely on (advisory mmap hints, MAP_FIXED inside an own reservation, partial
 * munmap, MAP_JIT, large PROT_NONE reservations, madvise, code coherence
 * across unmap/replace, mach_vm_region_recurse, mach_vm_purgable_control).
 * No SDK headers. Prints "fx vm reserve: ok" plus a "large reservation:"
 * line and returns 0, else returns the failed step number. */
typedef unsigned long size_t;
typedef unsigned long vm_address_t;
typedef unsigned long vm_size_t;
typedef int kern_return_t;
typedef unsigned mach_port_t;
extern mach_port_t mach_task_self_;
extern void* mmap(void*, size_t, int, int, int, long);
extern int munmap(void*, size_t);
extern int mprotect(void*, size_t, int);
extern int madvise(void*, size_t, int);
extern int* __error(void);
extern int puts(const char*);
extern int open(const char*, int, ...);
extern int close(int);
extern long pwrite(int, const void*, size_t, long);
extern int unlink(const char*);
extern int getpid(void);
extern kern_return_t vm_allocate(mach_port_t, vm_address_t*, vm_size_t, int);
extern kern_return_t vm_deallocate(mach_port_t, vm_address_t, vm_size_t);
extern kern_return_t mach_vm_region_recurse(mach_port_t, unsigned long*, unsigned long*, unsigned*, int*, unsigned*);
extern kern_return_t mach_vm_purgable_control(mach_port_t, unsigned long, int, int*);
extern kern_return_t mach_vm_region(mach_port_t, unsigned long*, unsigned long*, int, int*, unsigned*, mach_port_t*);

#define PAGE 16384ul
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x1
#define MAP_PRIVATE 0x2
#define MAP_FIXED 0x10
#define MAP_NORESERVE 0x40
#define MAP_JIT 0x800
#define MAP_ANON 0x1000
#define MAP_FAILED ((void*)-1)
#define MADV_DONTNEED 4
#define MADV_FREE 5
#define MADV_FREE_REUSABLE 7
#define MADV_FREE_REUSE 8
#define ANON (MAP_PRIVATE | MAP_ANON)
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_PURGABLE 2
#define VM_PURGABLE_SET_STATE 0
#define VM_PURGABLE_GET_STATE 1
#define VM_PURGABLE_PURGE_ALL 2
#define VM_PURGABLE_SET_STATE_FROM_KERNEL 3
#define VM_PURGABLE_NONVOLATILE 0
#define VM_PURGABLE_VOLATILE 1
#define VM_PURGABLE_EMPTY 2
#define KERN_SUCCESS 0
#define KERN_INVALID_ADDRESS 1
#define KERN_INVALID_ARGUMENT 4
/* VM_MAKE_TAG(VM_MEMORY_...) as PartitionAlloc passes it in the fd of an anonymous mapping. */
#define TAG(n) ((n) << 24)

typedef int (*function)(void);
static int global_word = 5;
/* Image data with pages of its own (base::ProtectedMemory keeps such data read-only). */
static char data_pages[3 * 16384] __attribute__((aligned(16384))) = {1};

static void put_code(volatile unsigned* at, unsigned value) {
    at[0] = 0x52800000u | (value << 5); /* mov w0, #value */
    at[1] = 0xd65f03c0u;                /* ret */
}

static int zeros(volatile unsigned char* at, size_t size) {
    for (size_t i = 0; i < size; i += 512)
        if (at[i]) return 0;
    return 1;
}

static int region(unsigned long address, unsigned long* base, unsigned long* size, int* protection, unsigned count) {
    int info[19] = {0};
    unsigned depth = 0;
    *base = address;
    kern_return_t result = mach_vm_region_recurse(mach_task_self_, base, size, &depth, info, &count);
    *protection = info[0];
    return result == KERN_SUCCESS && depth == 0 ? (int)count : -(int)result - 1;
}

int main(void) {
    const mach_port_t task = mach_task_self_;
    /* 1. An advisory hint in free space is taken as is; an occupied one is
     * not an error, and neither is one below 4 GiB. */
    char* hint = (char*)0x5a0000000000ul;
    char* a = mmap(hint, 4 * PAGE, PROT_READ | PROT_WRITE, ANON, TAG(254), 0);
    if (a == MAP_FAILED) return 1;
    if (a != hint) return 2;
    a[5] = 7;
    char* b = mmap(hint, PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    if (b == MAP_FAILED || b == hint || a[5] != 7) return 3;
    char* low = mmap((void*)0x40000ul, PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    if (low == MAP_FAILED || (unsigned long)low < 0x100000000ul) return 4;
    if (munmap(b, PAGE) || munmap(low, PAGE)) return 5;

    /* 2. Partial munmap: a hole in the middle keeps both sides and their bytes. */
    a[3 * PAGE + 1] = 9;
    if (munmap(a + PAGE, PAGE) != 0) return 6;
    if (a[5] != 7 || a[3 * PAGE + 1] != 9) return 7;
    unsigned long base = 0, size = 0;
    int protection = 0;
    if (region((unsigned long)a + PAGE, &base, &size, &protection, 19) != 19) return 8;
    if (base != (unsigned long)a + 2 * PAGE || size != 2 * PAGE || protection != (PROT_READ | PROT_WRITE)) return 9;
    if (region((unsigned long)a, &base, &size, &protection, 12) != 12 || base != (unsigned long)a || size != PAGE) return 10;
    {
        unsigned count = 5, depth = 0;
        int info[19];
        base = (unsigned long)a;
        if (mach_vm_region_recurse(task, &base, &size, &depth, info, &count) != KERN_INVALID_ARGUMENT) return 11;
    }
    /* The hole is free again: MAP_FIXED maps fresh zero pages into it. */
    char* hole = mmap(a + PAGE, PAGE, PROT_READ | PROT_WRITE, ANON | MAP_FIXED, -1, 0);
    if (hole != a + PAGE || !zeros((volatile unsigned char*)hole, PAGE)) return 12;
    if (munmap(a, 4 * PAGE) != 0) return 13;

    /* 3. PartitionAlloc's aligned reservation: over-reserve, trim the head
     * and tail slack, then commit and decommit inside with MAP_FIXED. */
    const unsigned long align = 2ul << 20, length = 4ul << 20;
    char* raw = mmap(0, length + align - PAGE, PROT_NONE, ANON | MAP_NORESERVE, TAG(254), 0);
    if (raw == MAP_FAILED) return 14;
    char* aligned = (char*)(((unsigned long)raw + align - 1) & ~(align - 1));
    if (aligned > raw && munmap(raw, (size_t)(aligned - raw)) != 0) return 15;
    char* tail = aligned + length;
    if ((unsigned long)tail < (unsigned long)raw + length + align - PAGE &&
        munmap(tail, (size_t)((unsigned long)raw + length + align - PAGE - (unsigned long)tail)) != 0) return 16;
    if (mprotect(aligned, 2 * PAGE, PROT_READ | PROT_WRITE) != 0) return 17;
    aligned[100] = 1;
    aligned[PAGE + 100] = 2;
    /* Decommit (DecommitAndZeroSystemPages) and recommit: zero-filled. */
    if (mmap(aligned, PAGE, PROT_NONE, ANON | MAP_FIXED, TAG(254), 0) != aligned) return 18;
    if (region((unsigned long)aligned, &base, &size, &protection, 19) != 19 || protection != 0 || base != (unsigned long)aligned) return 19;
    if (mprotect(aligned, PAGE, PROT_READ | PROT_WRITE) != 0 || !zeros((volatile unsigned char*)aligned, PAGE)) return 20;
    if (aligned[PAGE + 100] != 2) return 21;
    if (mmap(aligned, PAGE, PROT_READ | PROT_WRITE, ANON | MAP_FIXED, -1, 0) != aligned || aligned[100] != 0) return 22;
    /* Discard (DiscardSystemPages): contents may go, the pages stay usable. */
    if (madvise(aligned + PAGE, PAGE, MADV_FREE_REUSABLE) != 0) return 23;
    if (aligned[PAGE + 100] != 0 && aligned[PAGE + 100] != 2) return 24;
    if (madvise(aligned + PAGE, PAGE, MADV_FREE_REUSE) != 0 || madvise(aligned, PAGE, MADV_DONTNEED) != 0 ||
        madvise(aligned, PAGE, MADV_FREE) != 0) return 25;
    aligned[PAGE + 100] = 3;
    if (aligned[PAGE + 100] != 3) return 26;
    *__error() = 0;
    if (madvise(aligned, PAGE, 99) != -1 || *__error() != 22) return 27;
    /* The trimmed head is free address space again (no longer the guest's). */
    if (munmap(aligned, length) != 0) return 28;

    /* 4. MAP_FIXED over memory Alchemy did not map for mmap is refused. */
    *__error() = 0;
    void* page_of_global = (void*)((unsigned long)&global_word & ~(PAGE - 1));
    if (mmap(page_of_global, PAGE, PROT_READ | PROT_WRITE, ANON | MAP_FIXED, -1, 0) != MAP_FAILED || *__error() != 12) return 29;
    if (global_word != 5) return 30;
    *__error() = 0;
    if (mmap((void*)((unsigned long)hint + 1), PAGE, PROT_READ, ANON | MAP_FIXED, -1, 0) != MAP_FAILED || *__error() != 22) return 31;

    /* 5. MAP_JIT code; translations do not survive MAP_FIXED or munmap. */
    char* jit = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, ANON | MAP_JIT, -1, 0);
    if (jit == MAP_FAILED) return 32;
    function run = (function)jit;
    put_code((volatile unsigned*)jit, 1);
    if (run() != 1 || run() != 1) return 33;
    if (mmap(jit, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, ANON | MAP_FIXED | MAP_JIT, -1, 0) != jit) return 34;
    if (!zeros((volatile unsigned char*)jit, PAGE)) return 35;
    put_code((volatile unsigned*)jit, 2);
    if (run() != 2) return 36;
    if (mprotect(jit, PAGE, PROT_READ | PROT_EXEC) != 0 || run() != 2) return 37;
    if (munmap(jit, PAGE) != 0) return 38;
    char* again = mmap(jit, PAGE, PROT_READ | PROT_WRITE, ANON | MAP_FIXED, -1, 0);
    if (again != jit) return 39;
    put_code((volatile unsigned*)again, 3);
    if (mprotect(again, PAGE, PROT_READ | PROT_EXEC) != 0 || run() != 3) return 40;
    if (munmap(again, PAGE) != 0) return 41;

    /* 6. A V8-sized sandbox reservation (1 TiB plus 32 GiB guards each side):
     * reserved, or a clean ENOMEM for V8's fallback. Beyond the address space
     * it is always ENOMEM. */
    const unsigned long sandbox = (1ul << 40) + (64ul << 30);
    *__error() = 0;
    char* cage = mmap(0, sandbox, PROT_NONE, ANON | MAP_NORESERVE, -1, 0);
    if (cage == MAP_FAILED) {
        if (*__error() != 12) return 42;
        puts("large reservation: ENOMEM");
    } else {
        char* inside = cage + (32ul << 30) + (512ul << 30);
        if (mprotect(inside, PAGE, PROT_READ | PROT_WRITE) != 0) return 43;
        inside[1] = 4;
        if (mmap(inside, PAGE, PROT_NONE, ANON | MAP_FIXED, -1, 0) != inside) return 44;
        if (munmap(cage, sandbox) != 0) return 45;
        puts("large reservation: ok");
    }
    *__error() = 0;
    if (mmap(0, 1ul << 50, PROT_NONE, ANON | MAP_NORESERVE, -1, 0) != MAP_FAILED || *__error() != 12) return 46;

    /* 7. Purgeable memory (base::DiscardableSharedMemory's protocol). */
    vm_address_t p = 0;
    if (vm_allocate(task, &p, 4 * PAGE, VM_FLAGS_ANYWHERE | VM_FLAGS_PURGABLE) != KERN_SUCCESS) return 47;
    ((volatile char*)p)[10] = 6;
    int state = VM_PURGABLE_VOLATILE;
    if (mach_vm_purgable_control(task, p + PAGE, VM_PURGABLE_SET_STATE, &state) != KERN_SUCCESS ||
        state != VM_PURGABLE_NONVOLATILE) return 48;
    state = -1;
    if (mach_vm_purgable_control(task, p, VM_PURGABLE_GET_STATE, &state) != KERN_SUCCESS || state != VM_PURGABLE_VOLATILE) return 49;
    if (((volatile char*)p)[10] != 6) return 50;
    if (mach_vm_purgable_control(task, 0, VM_PURGABLE_PURGE_ALL, &state) != KERN_SUCCESS) return 51;
    state = VM_PURGABLE_NONVOLATILE;
    if (mach_vm_purgable_control(task, p, VM_PURGABLE_SET_STATE, &state) != KERN_SUCCESS || state != VM_PURGABLE_EMPTY) return 52;
    if (((volatile char*)p)[10] != 0) return 53;
    state = 1 << 20;
    if (mach_vm_purgable_control(task, p, VM_PURGABLE_SET_STATE, &state) != KERN_INVALID_ARGUMENT) return 54;
    state = 0;
    if (mach_vm_purgable_control(task, p, VM_PURGABLE_SET_STATE_FROM_KERNEL, &state) != KERN_INVALID_ARGUMENT) return 55;
    if (mach_vm_purgable_control(task, (unsigned long)&global_word, VM_PURGABLE_GET_STATE, &state) != KERN_INVALID_ARGUMENT) return 56;
    if (vm_deallocate(task, p, 4 * PAGE) != KERN_SUCCESS) return 57;
    char* gone = mmap(0, PAGE, PROT_READ, ANON, -1, 0);
    if (gone == MAP_FAILED || munmap(gone, PAGE) != 0) return 58;
    if (mach_vm_purgable_control(task, (unsigned long)gone, VM_PURGABLE_GET_STATE, &state) != KERN_INVALID_ADDRESS) return 59;

    /* 8. Regions split where protection changes, as Darwin's map entries do
     * (mach_vm_region and mach_vm_region_recurse): part of an mmap, image
     * data, and a code page fx write-protected after translating it. */
    char* split = mmap(0, 4 * PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
    if (split == MAP_FAILED || mprotect(split + PAGE, PAGE, PROT_READ) != 0) return 60;
    if (region((unsigned long)split + PAGE + 100, &base, &size, &protection, 19) != 19 ||
        base != (unsigned long)split + PAGE || size != PAGE || protection != PROT_READ) return 61;
    if (region((unsigned long)split, &base, &size, &protection, 19) != 19 || base != (unsigned long)split ||
        size != PAGE || protection != (PROT_READ | PROT_WRITE)) return 62;
    if (region((unsigned long)split + 2 * PAGE, &base, &size, &protection, 19) != 19 ||
        base != (unsigned long)split + 2 * PAGE || size != 2 * PAGE || protection != (PROT_READ | PROT_WRITE)) return 63;
    {
        int info[9] = {0};
        unsigned count = 9;
        mach_port_t object = 0;
        base = (unsigned long)split + PAGE;
        if (mach_vm_region(task, &base, &size, 9, info, &count, &object) != KERN_SUCCESS ||
            base != (unsigned long)split + PAGE || size != PAGE || info[0] != PROT_READ) return 64;
        base = (unsigned long)data_pages + PAGE;
        if (mprotect(data_pages + PAGE, 0x82, PROT_READ) != 0) return 65;
        count = 9;
        if (mach_vm_region(task, &base, &size, 9, info, &count, &object) != KERN_SUCCESS ||
            base != (unsigned long)data_pages + PAGE || size != PAGE || info[0] != PROT_READ) return 66;
        if (mprotect(data_pages + PAGE, PAGE, PROT_READ | PROT_WRITE) != 0) return 67;
        data_pages[PAGE] = 2;
    }
    if (munmap(split, 4 * PAGE) != 0) return 68;
    char* code = mmap(0, PAGE, PROT_READ | PROT_WRITE | PROT_EXEC, ANON | MAP_JIT, -1, 0);
    if (code == MAP_FAILED) return 69;
    put_code((volatile unsigned*)code, 5);
    run = (function)code;
    if (run() != 5) return 70;
    if (region((unsigned long)code, &base, &size, &protection, 19) != 19 ||
        protection != (PROT_READ | PROT_WRITE | PROT_EXEC)) return 71;
    if (munmap(code, PAGE) != 0) return 72;

    /* 9. File and shared mappings: SQLite's in-place remap (unixRemapfile
     * keeps the whole pages of its MAP_SHARED view, unmaps the partial last
     * page and maps the file's next bytes right after it at that address),
     * holes, a range spanning a file view and anonymous memory, and private
     * file copies (hints, MAP_FIXED, partial munmap). */
    {
        static char name[] = "/tmp/fx_vm_reserve_00000000.bin";
        int pid = getpid();
        for (int i = 0; i < 8; ++i) name[sizeof(name) - 6 - i] = (char)('0' + (pid >> (4 * i)) % 10);
        int fd = open(name, 0x2 | 0x200 | 0x400, 0600);   /* O_RDWR | O_CREAT | O_TRUNC */
        if (fd < 0) return 73;
        static unsigned char bytes[0x10000];
        for (unsigned i = 0; i < sizeof(bytes); ++i) bytes[i] = (unsigned char)(i / 256 * 7 + 1);
        if (pwrite(fd, bytes, 0x5000, 0) != 0x5000) return 74;
        unsigned char* view = mmap(0, 0x5000, PROT_READ, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED || view[0x4100] != bytes[0x4100]) return 75;
        if (munmap(view + 0x4000, 0x1000) != 0) return 76;
        if (view[0x100] != bytes[0x100] || view[0x3f00] != bytes[0x3f00]) return 77;
        if (pwrite(fd, bytes + 0x5000, 0x5000, 0x5000) != 0x5000) return 78;
        unsigned char* grown = mmap(view + 0x4000, 0x6000, PROT_READ, MAP_SHARED, fd, 0x4000);
        if (grown == MAP_FAILED) return 79;
        if (grown != view + 0x4000) return 80;   /* in place: the view is contiguous again */
        if (view[0x4100] != bytes[0x4100] || view[0x9100] != bytes[0x9100]) return 81;
        unsigned char changed = 0xab;
        if (pwrite(fd, &changed, 1, 0x100) != 1 || view[0x100] != 0xab) return 82;   /* MAP_SHARED sees the file */
        if (munmap(view, 0xa000) != 0) return 83;   /* spans both views */
        if (region((unsigned long)view, &base, &size, &protection, 19) == 19 && base <= (unsigned long)view + 0x4000) return 84;
        if (munmap(view, 0xa000) != 0) return 85;   /* nothing mapped there any more: still 0 */
        /* A hole in a three-page shared view. */
        if (pwrite(fd, bytes, 0xc000, 0) != 0xc000) return 86;
        view = mmap(0, 0xc000, PROT_READ, MAP_SHARED, fd, 0);
        if (view == MAP_FAILED || munmap(view + PAGE, PAGE) != 0) return 87;
        if (view[0x100] != bytes[0x100] || view[2 * PAGE + 0x100] != bytes[2 * PAGE + 0x100]) return 88;
        if (region((unsigned long)view + PAGE, &base, &size, &protection, 19) != 19 || base != (unsigned long)view + 2 * PAGE) return 89;
        /* One munmap over a shared view and anonymous memory right after it. */
        unsigned char* after = mmap(view + 0x10000, PAGE, PROT_READ | PROT_WRITE, ANON, -1, 0);
        if (after == MAP_FAILED) return 90;
        if (after == view + 0x10000) {
            after[1] = 1;
            if (munmap(view, 0x10000 + PAGE) != 0) return 91;
            if (region((unsigned long)view, &base, &size, &protection, 19) == 19 && base < (unsigned long)view + 0x10000 + PAGE) return 92;
            puts("spanning munmap: ok");
        } else {
            if (munmap(view, 0xc000) != 0 || munmap(after, PAGE) != 0) return 93;
            puts("spanning munmap: hint not honoured, not tested");
        }
        /* Private copies: writable without reaching the file, partial munmap, MAP_FIXED into an own reservation. */
        unsigned char* copy = mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        if (copy == MAP_FAILED || copy[PAGE + 5] != bytes[PAGE + 5]) return 94;
        copy[5] = 0xcd;
        if (munmap(copy + PAGE, PAGE) != 0 || copy[5] != 0xcd) return 95;
        unsigned char* again2 = mmap(0, PAGE, PROT_READ, MAP_SHARED, fd, 0);
        if (again2 == MAP_FAILED || again2[5] != bytes[5] || munmap(again2, PAGE) != 0) return 96;
        unsigned char* slot = mmap(0, 2 * PAGE, PROT_NONE, ANON, -1, 0);
        if (slot == MAP_FAILED) return 97;
        if (mmap(slot + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, PAGE) != slot + PAGE || slot[PAGE + 7] != bytes[PAGE + 7]) return 98;
        if (munmap(slot, 2 * PAGE) != 0 || munmap(copy, PAGE) != 0) return 99;
        close(fd);
        unlink(name);
    }

    puts("fx vm reserve: ok");
    return 0;
}
