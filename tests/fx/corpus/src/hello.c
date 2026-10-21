// printf and floating-point formatting.
#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static volatile double v_zero = 0.0;
static volatile double v_one = 1.0;
static volatile double v_tenth = 0.1;
static volatile double v_fifth = 0.2;
static volatile float v_third_f = 1.0f / 3.0f;

int main(void) {
    printf("hello, fx\n");
    puts("puts line");
    fputs("fputs line\n", stdout);
    putchar('c');
    putchar('\n');

    printf("ints: %d %i %u %ld %lld %llu\n", -42, 7, 4000000000u, -123456789L,
           (long long)INT64_MIN, (unsigned long long)UINT64_MAX);
    printf("hex: %x %X %#x %08x %llx %p\n", 0xbeefu, 0xbeefu, 255u, 0x1234u, 0x0123456789abcdefULL,
           (void*)(uintptr_t)0x1000);
    printf("octal: %o %#o\n", 8u, 8u);
    printf("width: [%5d] [%-5d] [%05d] [%+d] [% d]\n", 42, 42, 42, 42, 42);
    printf("char: %c%c%c\n", 'f', 'x', '!');
    printf("str: [%s] [%10s] [%-10s] [%.3s] [%*s]\n", "abc", "right", "left", "truncate", 6, "star");
    printf("sizes: %zu %zu %zu %zu %zu\n", sizeof(void*), sizeof(long), sizeof(long double), sizeof(wchar_t),
           sizeof(size_t));
    printf("char-signed: %d\n", (char)-1 < 0);

    printf("float: %f %.2f %10.4f %-10.1f|\n", 3.14159265358979, 2.5, -1.0 / 3.0, 1e3);
    printf("exp: %e %.3E %g %g %g %G\n", 123456.789, 0.000123456, 1e-5, 123456789.0, 0.1, 1e20);
    printf("hexfloat: %a %A %a\n", 1.0, -0.375, 0.1);
    printf("special: %f %f %g %d\n", v_one / v_zero, -v_one / v_zero, v_zero * -1.0,
           (v_zero / v_zero) != (v_zero / v_zero));
    printf("limits: %.17g %.17g %.9g %.9g\n", DBL_MAX, DBL_MIN, FLT_MAX, FLT_EPSILON);
    printf("denormal: %g %a\n", DBL_MIN / 1024.0, DBL_MIN / 1024.0);
    printf("ties: %.0f %.0f %.0f %.0f %.1f %.2f\n", 0.5, 1.5, 2.5, -0.5, 0.25, 1.005);
    printf("sum: %.17g %.17g\n", v_tenth + v_fifth, (v_tenth + v_fifth) - 0.3);
    printf("float-promote: %.9g %.17g\n", v_third_f, (double)v_third_f);
    printf("big: %.0f %.3f\n", 1e300, 123456789012345.678);
    printf("percent: %5.1f%%\n", 99.5);

    char buf[32];
    int n = snprintf(buf, sizeof buf, "%s-%d-%.3f", "snp", 99, 1.0005);
    printf("snprintf: %d [%s]\n", n, buf);
    n = snprintf(buf, 8, "%s", "truncated output");
    printf("snprintf-trunc: %d [%s]\n", n, buf);
    n = sprintf(buf, "%08.3f|%-8.2e|", 3.14159, 2718.28);
    printf("sprintf: %d [%s]\n", n, buf);

    double parsed = 0;
    int i1 = 0;
    char word[16] = {0};
    int fields = sscanf("12.5e-1 77 token", "%lf %d %15s", &parsed, &i1, word);
    printf("sscanf: %d %.17g %d %s\n", fields, parsed, i1, word);

    long long ll = (long long)-2.75;
    unsigned u = (unsigned)3.99;
    printf("convert: %lld %u %d %lld\n", ll, u, (int)(v_one * 1e9), (long long)(v_one * -9.007199254740993e15));
    printf("wide: %ls %S\n", L"wide-string", L"capital-S");
    fflush(stdout);
    return 0;
}
