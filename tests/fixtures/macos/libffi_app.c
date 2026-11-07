/* Project-authored fixture: calls through the guest libffi runtime (/usr/lib/libffi.dylib built by
 * scripts/build_alchemy_libffi.ps1). Covers the calls GLib's GObject, Python's ctypes and
 * LibreOffice's UNO bridge make: integer, floating-point, by-value structure and variadic calls,
 * closures called from guest code, more closures than one trampoline table holds, and a closure
 * used as a qsort comparator (called back from Alchemy's libSystem provider). The libffi
 * declarations below are written from libffi's documented API. */
typedef unsigned long size_t;
typedef struct ffi_type {
    size_t size;
    unsigned short alignment;
    unsigned short type;
    struct ffi_type** elements;
} ffi_type;
/* ffi_cif plus room for the target's extra fields. */
typedef struct {
    int abi;
    unsigned nargs;
    ffi_type** arg_types;
    ffi_type* rtype;
    unsigned bytes;
    unsigned flags;
    unsigned long extra[4];
} ffi_cif;
enum { FFI_OK = 0, FFI_DEFAULT_ABI = 1, FFI_TYPE_STRUCT = 13 };
extern ffi_type ffi_type_void, ffi_type_sint32, ffi_type_sint64, ffi_type_double, ffi_type_float, ffi_type_pointer;
extern int ffi_prep_cif(ffi_cif* cif, int abi, unsigned nargs, ffi_type* rtype, ffi_type** atypes);
extern int ffi_prep_cif_var(ffi_cif* cif, int abi, unsigned nfixed, unsigned ntotal, ffi_type* rtype, ffi_type** atypes);
extern void ffi_call(ffi_cif* cif, void (*fn)(void), void* rvalue, void** avalue);
extern void* ffi_closure_alloc(size_t size, void** code);
extern void ffi_closure_free(void* closure);
extern int ffi_prep_closure_loc(void* closure, ffi_cif* cif, void (*fun)(ffi_cif*, void*, void**, void*), void* user,
    void* codeloc);
extern size_t ffi_get_closure_size(void);
extern const char* ffi_get_version(void);
extern int printf(const char*, ...);
extern void qsort(void* base, size_t count, size_t width, int (*compare)(const void*, const void*));

static int add(int a, int b) { return a + b * 1000; }
static double mix(double a, int b, float c) { return a * b + c; }
typedef struct { double x, y; } point;
static point scaled(point p, double k) { point r = { p.x * k, p.y * k + 1 }; return r; }
static long vsum(int count, ...) {
    __builtin_va_list list;
    __builtin_va_start(list, count);
    long total = 0;
    for (int index = 0; index < count; ++index) total += __builtin_va_arg(list, long) * (index + 1);
    __builtin_va_end(list);
    return total;
}

/* Closure handlers: (int, int) -> int, scaled by the user data. */
static void combine(ffi_cif* cif, void* ret, void** args, void* user) {
    (void)cif;
    *(long*)ret = (long)(*(int*)args[0] * 100 + *(int*)args[1]) * *(int*)user;
}
static void compare(ffi_cif* cif, void* ret, void** args, void* user) {
    (void)cif;
    const int a = **(const int**)args[0], b = **(const int**)args[1];
    ++*(int*)user;
    *(long*)ret = (a > b) - (a < b);
}

enum { CLOSURES = 2600 };   /* 16 KiB / 16-byte trampolines = 1024 per table: three tables */
static void* closures[CLOSURES];
static void* codes[CLOSURES];
static int scales[CLOSURES];

int main(void) {
    ffi_cif cif;
    ffi_type* two_ints[] = { &ffi_type_sint32, &ffi_type_sint32 };
    int a = 7, b = 42, sum = 0;
    void* values[] = { &a, &b };
    long wide = 0;
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint32, two_ints) != FFI_OK) return 1;
    ffi_call(&cif, (void (*)(void))add, &wide, values);
    sum = (int)wide;

    ffi_type* mixed[] = { &ffi_type_double, &ffi_type_sint32, &ffi_type_float };
    double d = 1.5, mixed_result = 0;
    int n = 4;
    float f = 0.25f;
    void* mixed_values[] = { &d, &n, &f };
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 3, &ffi_type_double, mixed) != FFI_OK) return 2;
    ffi_call(&cif, (void (*)(void))mix, &mixed_result, mixed_values);

    ffi_type* point_elements[] = { &ffi_type_double, &ffi_type_double, 0 };
    ffi_type point_type = { 0, 0, FFI_TYPE_STRUCT, point_elements };
    ffi_type* point_args[] = { &point_type, &ffi_type_double };
    point p = { 3, -2 }, q = { 0, 0 };
    double k = 2.5;
    void* point_values[] = { &p, &k };
    if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI, 2, &point_type, point_args) != FFI_OK) return 3;
    ffi_call(&cif, (void (*)(void))scaled, &q, point_values);

    ffi_type* variadic[] = { &ffi_type_sint32, &ffi_type_sint64, &ffi_type_sint64, &ffi_type_sint64 };
    int three = 3;
    long v1 = 10, v2 = 20, v3 = 30, total = 0;
    void* variadic_values[] = { &three, &v1, &v2, &v3 };
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, 1, 4, &ffi_type_sint64, variadic) != FFI_OK) return 4;
    ffi_call(&cif, (void (*)(void))vsum, &total, variadic_values);
    printf("libffi %s: add %d mix %.3f point %.2f %.2f vsum %ld\n", ffi_get_version(), sum, mixed_result, q.x, q.y, total);

    ffi_cif closure_cif;
    if (ffi_prep_cif(&closure_cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint32, two_ints) != FFI_OK) return 5;
    long checksum = 0;
    for (int index = 0; index < CLOSURES; ++index) {
        scales[index] = index + 1;
        closures[index] = ffi_closure_alloc(ffi_get_closure_size(), &codes[index]);
        if (!closures[index]) return 6;
        if (ffi_prep_closure_loc(closures[index], &closure_cif, combine, &scales[index], codes[index]) != FFI_OK) return 7;
    }
    for (int index = 0; index < CLOSURES; ++index) checksum += ((int (*)(int, int))codes[index])(index % 7, 3);
    int distinct_pages = 1;
    for (int index = 1; index < CLOSURES; ++index)
        if (((unsigned long)codes[index] >> 14) != ((unsigned long)codes[index - 1] >> 14)) ++distinct_pages;
    for (int index = 0; index < CLOSURES; ++index) ffi_closure_free(closures[index]);
    printf("closures %d on %d trampoline pages: checksum %ld\n", CLOSURES, distinct_pages, checksum);

    ffi_type* two_pointers[] = { &ffi_type_pointer, &ffi_type_pointer };
    ffi_cif compare_cif;
    void* compare_code = 0;
    int calls = 0;
    void* comparator = ffi_closure_alloc(ffi_get_closure_size(), &compare_code);
    if (!comparator || ffi_prep_cif(&compare_cif, FFI_DEFAULT_ABI, 2, &ffi_type_sint32, two_pointers) != FFI_OK) return 8;
    if (ffi_prep_closure_loc(comparator, &compare_cif, compare, &calls, compare_code) != FFI_OK) return 9;
    int numbers[] = { 42, -7, 19, 0, 1000, 3, 3, -250, 64, 8 };
    qsort(numbers, 10, sizeof(int), (int (*)(const void*, const void*))compare_code);
    ffi_closure_free(comparator);
    printf("qsort through a closure:");
    for (int index = 0; index < 10; ++index) printf(" %d", numbers[index]);
    printf(" (%s)\n", calls > 0 ? "comparator called" : "comparator not called");
    return 0;
}
