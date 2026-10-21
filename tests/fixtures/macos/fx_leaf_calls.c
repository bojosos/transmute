/* alchemy-fx engine fixture: libSystem's pure memory/string calls (memcpy,
 * memmove, bcopy, memset, bzero, memcmp, strlen), which the fx engine runs on
 * the guest registers without their handler while no byte faults
 * (FxEngine::set_guest_call_leaf). No SDK headers. Every case is called
 * several times: the first call of a function takes the SVC slot, later ones
 * its native entry. Prints "fx leaf calls: ok" and exits 0, or prints the
 * failing case and exits with its number.
 *  1 memcpy/memmove/bcopy results for sizes 0..300 at mixed alignments, overlap both ways
 *  2 memset/bzero results
 *  3 memcmp sign and value (the byte difference) for sizes 0..300, equal and unequal
 *  4 strlen for lengths 0..300
 *  5 registers: memcpy/memmove/memset/memcmp/strlen return in x0 with x1 = 0;
 *    bzero/bcopy leave x0 as it was, x1 = 0; callee-saved registers kept
 *  6 strlen of an unreadable address: -1 and errno EFAULT (the handler path) */
typedef unsigned long size_t;
extern void* memcpy(void*, const void*, size_t);
extern void* memmove(void*, const void*, size_t);
extern void bcopy(const void*, void*, size_t);
extern void* memset(void*, int, size_t);
extern void bzero(void*, size_t);
extern int memcmp(const void*, const void*, size_t);
extern size_t strlen(const char*);
extern int* __error(void);
extern int puts(const char*);

/* Called through volatile pointers: the compiler neither inlines nor folds them. */
static void* (*volatile p_memcpy)(void*, const void*, size_t) = memcpy;
static void* (*volatile p_memmove)(void*, const void*, size_t) = memmove;
static void (*volatile p_bcopy)(const void*, void*, size_t) = bcopy;
static void* (*volatile p_memset)(void*, int, size_t) = memset;
static void (*volatile p_bzero)(void*, size_t) = bzero;
static int (*volatile p_memcmp)(const void*, const void*, size_t) = memcmp;
static size_t (*volatile p_strlen)(const char*) = strlen;

static unsigned char a[1024], b[1024], expect[1024];

/* The reference loops go through volatile pointers, so the compiler cannot
 * turn them into calls of the functions under test. */
static void fill(unsigned char* p, size_t n, unsigned seed) {
    volatile unsigned char* v = p;
    for (size_t i = 0; i < n; ++i) v[i] = (unsigned char)(seed * 131u + i * 7u + (i >> 3));
}
static int same(const unsigned char* x, const unsigned char* y, size_t n) {
    const volatile unsigned char *vx = x, *vy = y;
    for (size_t i = 0; i < n; ++i) if (vx[i] != vy[i]) return 0;
    return 1;
}
static void set(unsigned char* d, const unsigned char* s, size_t n) {
    volatile unsigned char* v = d;
    for (size_t i = 0; i < n; ++i) v[i] = s[i];
}
static void ref_move(unsigned char* d, const unsigned char* s, size_t n) {
    unsigned char t[1024];
    set(t, s, n);
    set(d, t, n);
}

/* x0..x2 in, x0/x1 out, through blr; x19 must survive. */
static unsigned long call3(void* fn, unsigned long r0, unsigned long r1, unsigned long r2, unsigned long* r1_out,
                           int* x19_kept) {
    register unsigned long x0 __asm__("x0") = r0;
    register unsigned long x1 __asm__("x1") = r1;
    register unsigned long x2 __asm__("x2") = r2;
    register void* x16 __asm__("x16") = fn;
    register unsigned long x19 __asm__("x19") = 0x5eed1234abcdULL;
    __asm__ volatile("blr x16"
                     : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x19)
                     : "r"(x16)
                     : "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17",
                       "x30", "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v16", "v17", "v18", "v19", "v20",
                       "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31", "cc", "memory");
    *r1_out = x1;
    *x19_kept = x19 == 0x5eed1234abcdULL;
    return x0;
}

static int copies(void) {
    for (int rep = 0; rep < 3; ++rep)
        for (size_t n = 0; n <= 300; n += (n < 40 ? 1 : 13))
            for (size_t off = 0; off < 8; off += 3) {
                fill(a, sizeof a, (unsigned)(n + off)); fill(b, sizeof b, (unsigned)(n * 3 + 1));
                set(expect, b, sizeof b);
                ref_move(expect + off, a + 5, n);
                if (p_memcpy(b + off, a + 5, n) != b + off || !same(b, expect, sizeof b)) return 0;
                /* memmove forward and backward overlap inside one buffer */
                fill(a, sizeof a, (unsigned)n);
                set(expect, a, sizeof a);
                ref_move(expect + 64 + off, expect + 64, n);
                if (p_memmove(a + 64 + off, a + 64, n) != a + 64 + off || !same(a, expect, sizeof a)) return 0;
                fill(a, sizeof a, (unsigned)n + 9);
                set(expect, a, sizeof a);
                ref_move(expect + 64, expect + 64 + off + 1, n);
                p_bcopy(a + 64 + off + 1, a + 64, n);
                if (!same(a, expect, sizeof a)) return 0;
            }
    return 1;
}

static int fills(void) {
    for (int rep = 0; rep < 3; ++rep)
        for (size_t n = 0; n <= 300; n += (n < 40 ? 1 : 11)) {
            fill(b, sizeof b, (unsigned)n);
            set(expect, b, sizeof b);
            for (size_t i = 0; i < n; ++i) ((volatile unsigned char*)expect)[3 + i] = (unsigned char)(0x1a5 + n);
            if (p_memset(b + 3, (int)(0x1a5 + n), n) != b + 3 || !same(b, expect, sizeof b)) return 0;
            for (size_t i = 0; i < n; ++i) ((volatile unsigned char*)expect)[7 + i] = 0;
            p_bzero(b + 7, n);
            if (!same(b, expect, sizeof b)) return 0;
        }
    return 1;
}

static int compares(void) {
    for (int rep = 0; rep < 3; ++rep)
        for (size_t n = 0; n <= 300; n += (n < 40 ? 1 : 7)) {
            fill(a, sizeof a, 4); fill(b, sizeof b, 4);
            if (p_memcmp(a, b, n) != 0) return 0;
            if (n == 0) continue;
            const size_t at = (n * 5 / 7) % n;
            b[at] = (unsigned char)(a[at] + 1 + (n & 0x7f));
            const int want = (int)a[at] - (int)b[at];
            if (p_memcmp(a, b, n) != want || p_memcmp(b, a, n) != -want) return 0;
        }
    return 1;
}

static int lengths(void) {
    for (int rep = 0; rep < 3; ++rep)
        for (size_t n = 0; n <= 300; ++n) {
            for (size_t i = 0; i < n; ++i) ((volatile unsigned char*)a)[i] = (unsigned char)(1 + (i % 200));
            a[n] = 0;
            if (p_strlen((const char*)a) != n) return 0;
        }
    return 1;
}

static int registers(void) {
    for (int rep = 0; rep < 3; ++rep) {
        unsigned long x1 = 0;
        int kept = 0;
        if (call3((void*)p_memcpy, (unsigned long)b, (unsigned long)a, 16, &x1, &kept) != (unsigned long)b || x1 || !kept) return 0;
        if (call3((void*)p_memmove, (unsigned long)b, (unsigned long)a, 16, &x1, &kept) != (unsigned long)b || x1 || !kept) return 0;
        if (call3((void*)p_memset, (unsigned long)b, 7, 16, &x1, &kept) != (unsigned long)b || x1 || !kept) return 0;
        if (call3((void*)p_bzero, (unsigned long)b, 16, 0x777, &x1, &kept) != (unsigned long)b || x1 || !kept) return 0;
        if (call3((void*)p_bcopy, (unsigned long)a, (unsigned long)b, 16, &x1, &kept) != (unsigned long)a || x1 || !kept) return 0;
        a[0] = 9; b[0] = 2;
        if (call3((void*)p_memcmp, (unsigned long)a, (unsigned long)b, 16, &x1, &kept) != 7 || x1 || !kept) return 0;
        a[5] = 0;
        if (call3((void*)p_strlen, (unsigned long)a, 0x55, 0x66, &x1, &kept) != 5 || x1 || !kept) return 0;
    }
    return 1;
}

static int unreadable(void) {
    for (int rep = 0; rep < 3; ++rep) {
        *__error() = 0;
        if (p_strlen((const char*)16) != (size_t)-1 || *__error() != 14) return 0;
    }
    return 1;
}

int main(void) {
    if (!copies()) { puts("fx leaf calls: case 1 (copies) failed"); return 1; }
    if (!fills()) { puts("fx leaf calls: case 2 (fills) failed"); return 2; }
    if (!compares()) { puts("fx leaf calls: case 3 (memcmp) failed"); return 3; }
    if (!lengths()) { puts("fx leaf calls: case 4 (strlen) failed"); return 4; }
    if (!registers()) { puts("fx leaf calls: case 5 (registers) failed"); return 5; }
    if (!unreadable()) { puts("fx leaf calls: case 6 (strlen of an unreadable address) failed"); return 6; }
    puts("fx leaf calls: ok");
    return 0;
}
