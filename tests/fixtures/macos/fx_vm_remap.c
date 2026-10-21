/* alchemy-fx engine fixture: Mach vm_remap of writable memory is a real
 * shared alias (fx shared guest memory), including code executed through a
 * read/execute alias while it is rewritten through the writable one. No SDK
 * headers. Prints "fx vm_remap: ok" and returns 0, else returns the failed
 * step number. */
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
    puts("fx vm_remap: ok");
    return 0;
}
