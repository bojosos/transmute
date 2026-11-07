/* A darwintest-compatible harness for running Apple's open-source unit tests (Libc, libpthread)
 * as conformance guests. darwintest itself is not open source; this implements the interface as
 * the open-source tests use it:
 *
 *   T_DECL(name, "description", T_META_...) { body }   a test case; the body sees argc/argv
 *   T_HELPER_DECL(name, "description") { body }         a helper process, run by name
 *   T_ASSERT_<check>(..., fmt, ...)                     on failure: log, fail the case, end it
 *   T_EXPECT_<check>(..., fmt, ...)                     on failure: log, fail the case, continue
 *   T_PASS / T_FAIL / T_SKIP / T_END / T_LOG / T_QUIET / T_MAYFAIL / T_EXPECTFAIL / T_ATEND
 *
 * The program runs one case per process: `prog <name> [args]`. It prints "[PASS] name",
 * "[FAIL] ..." lines, or "[SKIP] name: reason" and exits 0 (pass or skip) or 1 (fail).
 * T_META_* options are read from the source by scripts/conformance/suites/darwintest_suites.py
 * (skips, environment, timeouts); here they are ignored. Project-authored, MIT. */
#ifndef CONFORMANCE_DARWINTEST_H
#define CONFORMANCE_DARWINTEST_H

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Some tests use assert, NSEC_PER_MSEC and vm_page_size without including their headers, relying
 * on darwintest.h making them visible. */
#include <assert.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The return type of helper functions that may end the case (T_ASSERT_* inside them). */
#define DT_TEST_RETURN void

typedef void (*_t_fn)(int argc, char **argv);
void _t_register(const char *name, _t_fn fn, int helper);
void _t_result(int ok, int assert, int is_expect_value, const char *file, int line, const char *expr,
               long long lhs, long long rhs, const char *fmt, ...) __attribute__((format(printf, 9, 10)));
void _t_result_str(int ok, int assert, const char *file, int line, const char *expr, const char *lhs,
                   const char *rhs, const char *fmt, ...) __attribute__((format(printf, 8, 9)));
void _t_result_errno(int ok, int assert, const char *file, int line, const char *expr, int err,
                     const char *fmt, ...) __attribute__((format(printf, 7, 8)));
void _t_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void _t_pass(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void _t_fail(int end, const char *file, int line, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void _t_skip(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void _t_end(void) __attribute__((noreturn));
void _t_quiet(void);
void _t_mayfail(void);
void _t_expectfail(void);
void _t_with_errno(void);
void _t_atend(void (*fn)(void));
void _t_setup(int begin);
const char *_t_name(void);

#define _T_CAT2(a, b) a##b
#define _T_CAT(a, b) _T_CAT2(a, b)

#define T_DECL(name, desc, ...) \
    static void _t_decl_##name(int argc, char **argv); \
    __attribute__((constructor)) static void _t_reg_##name(void) { _t_register(#name, _t_decl_##name, 0); } \
    static void _t_decl_##name(__attribute__((unused)) int argc, __attribute__((unused)) char **argv)
#define T_HELPER_DECL(name, desc, ...) \
    static void _t_decl_##name(int argc, char **argv); \
    __attribute__((constructor)) static void _t_reg_##name(void) { _t_register(#name, _t_decl_##name, 1); } \
    static void _t_decl_##name(__attribute__((unused)) int argc, __attribute__((unused)) char **argv)
#define T_GLOBAL_META(...) _Static_assert(1, "T_GLOBAL_META")
#define T_META(...)
#define T_NAME _t_name()

#define T_LOG(...) _t_log(__VA_ARGS__)
#define T_LOG_VERBOSE(...) _t_log(__VA_ARGS__)
#define T_PASS(...) _t_pass(__VA_ARGS__)
#define T_FAIL(...) _t_fail(0, __FILE__, __LINE__, __VA_ARGS__)
#define T_ASSERT_FAIL(...) _t_fail(1, __FILE__, __LINE__, __VA_ARGS__)
#define T_SKIP(...) _t_skip(__VA_ARGS__)
#define T_END _t_end()
#define T_QUIET _t_quiet();
#define T_MAYFAIL _t_mayfail();
#define T_MAYFAIL_WITH_REASON(...) _t_mayfail();
#define T_EXPECTFAIL _t_expectfail();
#define T_EXPECTFAIL_WITH_REASON(...) _t_expectfail();
#define T_WITH_ERRNO _t_with_errno();
#define T_ATEND(fn) _t_atend(fn)
#define T_SETUPBEGIN _t_setup(1)
#define T_SETUPEND _t_setup(0)

/* Comparisons. Values are logged as integers; pointers and enums convert. */
#define _T_CMP(assert, op, lhs, rhs, ...) do { \
        __typeof__(lhs) _t_l = (lhs); __typeof__(rhs) _t_r = (rhs); \
        _t_result(_t_l op _t_r, assert, 1, __FILE__, __LINE__, #lhs " " #op " " #rhs, \
                  (long long)(_t_l), (long long)(_t_r), __VA_ARGS__); \
    } while (0)
#define _T_CMPT(assert, type, op, lhs, rhs, ...) do { \
        type _t_l = (type)(lhs); type _t_r = (type)(rhs); \
        _t_result(_t_l op _t_r, assert, 1, __FILE__, __LINE__, #lhs " " #op " " #rhs, \
                  (long long)(_t_l), (long long)(_t_r), __VA_ARGS__); \
    } while (0)
#define _T_STR(assert, eq, op, lhs, rhs, ...) do { \
        const char *_t_l = (lhs), *_t_r = (rhs); \
        int _t_same = (_t_l && _t_r) ? strcmp(_t_l, _t_r) == 0 : _t_l == _t_r; \
        _t_result_str(eq ? _t_same : !_t_same, assert, __FILE__, __LINE__, #lhs " " op " " #rhs, \
                      _t_l, _t_r, __VA_ARGS__); \
    } while (0)
#define _T_BOOL(assert, cond, text, ...) \
    _t_result(!!(cond), assert, 0, __FILE__, __LINE__, text, 0, 0, __VA_ARGS__)

#define T_EXPECT_EQ(l, r, ...) _T_CMP(0, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE(l, r, ...) _T_CMP(0, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT(l, r, ...) _T_CMP(0, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE(l, r, ...) _T_CMP(0, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT(l, r, ...) _T_CMP(0, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE(l, r, ...) _T_CMP(0, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ(l, r, ...) _T_CMP(1, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE(l, r, ...) _T_CMP(1, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT(l, r, ...) _T_CMP(1, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE(l, r, ...) _T_CMP(1, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT(l, r, ...) _T_CMP(1, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE(l, r, ...) _T_CMP(1, >=, l, r, __VA_ARGS__)

/* Typed comparisons: T_{EXPECT,ASSERT}_<op>_<type>. */
#define T_EXPECT_EQ_INT(l, r, ...) _T_CMPT(0, int, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_INT(l, r, ...) _T_CMPT(0, int, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_INT(l, r, ...) _T_CMPT(0, int, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_INT(l, r, ...) _T_CMPT(0, int, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_INT(l, r, ...) _T_CMPT(0, int, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_INT(l, r, ...) _T_CMPT(0, int, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_UINT(l, r, ...) _T_CMPT(0, unsigned int, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_UINT(l, r, ...) _T_CMPT(0, unsigned int, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_UINT(l, r, ...) _T_CMPT(0, unsigned int, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_UINT(l, r, ...) _T_CMPT(0, unsigned int, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_UINT(l, r, ...) _T_CMPT(0, unsigned int, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_UINT(l, r, ...) _T_CMPT(0, unsigned int, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_LONG(l, r, ...) _T_CMPT(0, long, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_LONG(l, r, ...) _T_CMPT(0, long, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_LONG(l, r, ...) _T_CMPT(0, long, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_LONG(l, r, ...) _T_CMPT(0, long, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_LONG(l, r, ...) _T_CMPT(0, long, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_LONG(l, r, ...) _T_CMPT(0, long, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_ULONG(l, r, ...) _T_CMPT(0, unsigned long, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_ULONG(l, r, ...) _T_CMPT(0, unsigned long, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_ULONG(l, r, ...) _T_CMPT(0, unsigned long, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_ULONG(l, r, ...) _T_CMPT(0, unsigned long, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_ULONG(l, r, ...) _T_CMPT(0, unsigned long, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_ULONG(l, r, ...) _T_CMPT(0, unsigned long, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_LLONG(l, r, ...) _T_CMPT(0, long long, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_LLONG(l, r, ...) _T_CMPT(0, long long, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_LLONG(l, r, ...) _T_CMPT(0, long long, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_LLONG(l, r, ...) _T_CMPT(0, long long, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_LLONG(l, r, ...) _T_CMPT(0, long long, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_LLONG(l, r, ...) _T_CMPT(0, long long, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_ULLONG(l, r, ...) _T_CMPT(0, unsigned long long, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_ULLONG(l, r, ...) _T_CMPT(0, unsigned long long, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_ULLONG(l, r, ...) _T_CMPT(0, unsigned long long, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_ULLONG(l, r, ...) _T_CMPT(0, unsigned long long, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_ULLONG(l, r, ...) _T_CMPT(0, unsigned long long, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_ULLONG(l, r, ...) _T_CMPT(0, unsigned long long, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_CHAR(l, r, ...) _T_CMPT(0, char, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_CHAR(l, r, ...) _T_CMPT(0, char, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_CHAR(l, r, ...) _T_CMPT(0, char, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_CHAR(l, r, ...) _T_CMPT(0, char, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_CHAR(l, r, ...) _T_CMPT(0, char, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_CHAR(l, r, ...) _T_CMPT(0, char, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_UCHAR(l, r, ...) _T_CMPT(0, unsigned char, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_UCHAR(l, r, ...) _T_CMPT(0, unsigned char, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_UCHAR(l, r, ...) _T_CMPT(0, unsigned char, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_UCHAR(l, r, ...) _T_CMPT(0, unsigned char, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_UCHAR(l, r, ...) _T_CMPT(0, unsigned char, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_UCHAR(l, r, ...) _T_CMPT(0, unsigned char, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_SHORT(l, r, ...) _T_CMPT(0, short, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_SHORT(l, r, ...) _T_CMPT(0, short, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_SHORT(l, r, ...) _T_CMPT(0, short, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_SHORT(l, r, ...) _T_CMPT(0, short, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_SHORT(l, r, ...) _T_CMPT(0, short, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_SHORT(l, r, ...) _T_CMPT(0, short, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_USHORT(l, r, ...) _T_CMPT(0, unsigned short, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_USHORT(l, r, ...) _T_CMPT(0, unsigned short, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_USHORT(l, r, ...) _T_CMPT(0, unsigned short, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_USHORT(l, r, ...) _T_CMPT(0, unsigned short, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_USHORT(l, r, ...) _T_CMPT(0, unsigned short, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_USHORT(l, r, ...) _T_CMPT(0, unsigned short, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_SIZE(l, r, ...) _T_CMPT(0, size_t, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_SIZE(l, r, ...) _T_CMPT(0, size_t, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_SIZE(l, r, ...) _T_CMPT(0, size_t, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_SIZE(l, r, ...) _T_CMPT(0, size_t, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_SIZE(l, r, ...) _T_CMPT(0, size_t, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_SIZE(l, r, ...) _T_CMPT(0, size_t, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_DOUBLE(l, r, ...) _T_CMPT(0, double, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_DOUBLE(l, r, ...) _T_CMPT(0, double, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_DOUBLE(l, r, ...) _T_CMPT(0, double, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_DOUBLE(l, r, ...) _T_CMPT(0, double, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_DOUBLE(l, r, ...) _T_CMPT(0, double, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_DOUBLE(l, r, ...) _T_CMPT(0, double, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_FLOAT(l, r, ...) _T_CMPT(0, float, ==, l, r, __VA_ARGS__)
#define T_EXPECT_NE_FLOAT(l, r, ...) _T_CMPT(0, float, !=, l, r, __VA_ARGS__)
#define T_EXPECT_LT_FLOAT(l, r, ...) _T_CMPT(0, float, <, l, r, __VA_ARGS__)
#define T_EXPECT_LE_FLOAT(l, r, ...) _T_CMPT(0, float, <=, l, r, __VA_ARGS__)
#define T_EXPECT_GT_FLOAT(l, r, ...) _T_CMPT(0, float, >, l, r, __VA_ARGS__)
#define T_EXPECT_GE_FLOAT(l, r, ...) _T_CMPT(0, float, >=, l, r, __VA_ARGS__)
#define T_EXPECT_EQ_PTR(l, r, ...) _T_CMPT(0, uintptr_t, ==, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_EXPECT_NE_PTR(l, r, ...) _T_CMPT(0, uintptr_t, !=, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_EXPECT_LT_PTR(l, r, ...) _T_CMPT(0, uintptr_t, <, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_EXPECT_LE_PTR(l, r, ...) _T_CMPT(0, uintptr_t, <=, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_EXPECT_GT_PTR(l, r, ...) _T_CMPT(0, uintptr_t, >, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_EXPECT_GE_PTR(l, r, ...) _T_CMPT(0, uintptr_t, >=, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_ASSERT_EQ_INT(l, r, ...) _T_CMPT(1, int, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_INT(l, r, ...) _T_CMPT(1, int, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_INT(l, r, ...) _T_CMPT(1, int, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_INT(l, r, ...) _T_CMPT(1, int, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_INT(l, r, ...) _T_CMPT(1, int, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_INT(l, r, ...) _T_CMPT(1, int, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_UINT(l, r, ...) _T_CMPT(1, unsigned int, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_UINT(l, r, ...) _T_CMPT(1, unsigned int, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_UINT(l, r, ...) _T_CMPT(1, unsigned int, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_UINT(l, r, ...) _T_CMPT(1, unsigned int, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_UINT(l, r, ...) _T_CMPT(1, unsigned int, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_UINT(l, r, ...) _T_CMPT(1, unsigned int, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_LONG(l, r, ...) _T_CMPT(1, long, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_LONG(l, r, ...) _T_CMPT(1, long, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_LONG(l, r, ...) _T_CMPT(1, long, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_LONG(l, r, ...) _T_CMPT(1, long, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_LONG(l, r, ...) _T_CMPT(1, long, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_LONG(l, r, ...) _T_CMPT(1, long, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_ULONG(l, r, ...) _T_CMPT(1, unsigned long, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_ULONG(l, r, ...) _T_CMPT(1, unsigned long, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_ULONG(l, r, ...) _T_CMPT(1, unsigned long, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_ULONG(l, r, ...) _T_CMPT(1, unsigned long, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_ULONG(l, r, ...) _T_CMPT(1, unsigned long, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_ULONG(l, r, ...) _T_CMPT(1, unsigned long, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_LLONG(l, r, ...) _T_CMPT(1, long long, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_LLONG(l, r, ...) _T_CMPT(1, long long, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_LLONG(l, r, ...) _T_CMPT(1, long long, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_LLONG(l, r, ...) _T_CMPT(1, long long, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_LLONG(l, r, ...) _T_CMPT(1, long long, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_LLONG(l, r, ...) _T_CMPT(1, long long, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_ULLONG(l, r, ...) _T_CMPT(1, unsigned long long, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_ULLONG(l, r, ...) _T_CMPT(1, unsigned long long, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_ULLONG(l, r, ...) _T_CMPT(1, unsigned long long, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_ULLONG(l, r, ...) _T_CMPT(1, unsigned long long, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_ULLONG(l, r, ...) _T_CMPT(1, unsigned long long, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_ULLONG(l, r, ...) _T_CMPT(1, unsigned long long, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_CHAR(l, r, ...) _T_CMPT(1, char, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_CHAR(l, r, ...) _T_CMPT(1, char, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_CHAR(l, r, ...) _T_CMPT(1, char, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_CHAR(l, r, ...) _T_CMPT(1, char, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_CHAR(l, r, ...) _T_CMPT(1, char, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_CHAR(l, r, ...) _T_CMPT(1, char, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_UCHAR(l, r, ...) _T_CMPT(1, unsigned char, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_UCHAR(l, r, ...) _T_CMPT(1, unsigned char, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_UCHAR(l, r, ...) _T_CMPT(1, unsigned char, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_UCHAR(l, r, ...) _T_CMPT(1, unsigned char, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_UCHAR(l, r, ...) _T_CMPT(1, unsigned char, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_UCHAR(l, r, ...) _T_CMPT(1, unsigned char, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_SHORT(l, r, ...) _T_CMPT(1, short, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_SHORT(l, r, ...) _T_CMPT(1, short, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_SHORT(l, r, ...) _T_CMPT(1, short, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_SHORT(l, r, ...) _T_CMPT(1, short, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_SHORT(l, r, ...) _T_CMPT(1, short, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_SHORT(l, r, ...) _T_CMPT(1, short, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_USHORT(l, r, ...) _T_CMPT(1, unsigned short, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_USHORT(l, r, ...) _T_CMPT(1, unsigned short, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_USHORT(l, r, ...) _T_CMPT(1, unsigned short, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_USHORT(l, r, ...) _T_CMPT(1, unsigned short, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_USHORT(l, r, ...) _T_CMPT(1, unsigned short, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_USHORT(l, r, ...) _T_CMPT(1, unsigned short, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_SIZE(l, r, ...) _T_CMPT(1, size_t, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_SIZE(l, r, ...) _T_CMPT(1, size_t, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_SIZE(l, r, ...) _T_CMPT(1, size_t, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_SIZE(l, r, ...) _T_CMPT(1, size_t, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_SIZE(l, r, ...) _T_CMPT(1, size_t, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_SIZE(l, r, ...) _T_CMPT(1, size_t, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_DOUBLE(l, r, ...) _T_CMPT(1, double, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_DOUBLE(l, r, ...) _T_CMPT(1, double, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_DOUBLE(l, r, ...) _T_CMPT(1, double, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_DOUBLE(l, r, ...) _T_CMPT(1, double, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_DOUBLE(l, r, ...) _T_CMPT(1, double, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_DOUBLE(l, r, ...) _T_CMPT(1, double, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_FLOAT(l, r, ...) _T_CMPT(1, float, ==, l, r, __VA_ARGS__)
#define T_ASSERT_NE_FLOAT(l, r, ...) _T_CMPT(1, float, !=, l, r, __VA_ARGS__)
#define T_ASSERT_LT_FLOAT(l, r, ...) _T_CMPT(1, float, <, l, r, __VA_ARGS__)
#define T_ASSERT_LE_FLOAT(l, r, ...) _T_CMPT(1, float, <=, l, r, __VA_ARGS__)
#define T_ASSERT_GT_FLOAT(l, r, ...) _T_CMPT(1, float, >, l, r, __VA_ARGS__)
#define T_ASSERT_GE_FLOAT(l, r, ...) _T_CMPT(1, float, >=, l, r, __VA_ARGS__)
#define T_ASSERT_EQ_PTR(l, r, ...) _T_CMPT(1, uintptr_t, ==, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_ASSERT_NE_PTR(l, r, ...) _T_CMPT(1, uintptr_t, !=, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_ASSERT_LT_PTR(l, r, ...) _T_CMPT(1, uintptr_t, <, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_ASSERT_LE_PTR(l, r, ...) _T_CMPT(1, uintptr_t, <=, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_ASSERT_GT_PTR(l, r, ...) _T_CMPT(1, uintptr_t, >, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_ASSERT_GE_PTR(l, r, ...) _T_CMPT(1, uintptr_t, >=, (uintptr_t)(l), (uintptr_t)(r), __VA_ARGS__)
#define T_EXPECT_EQ_STR(l, r, ...) _T_STR(0, 1, "==", l, r, __VA_ARGS__)
#define T_ASSERT_EQ_STR(l, r, ...) _T_STR(1, 1, "==", l, r, __VA_ARGS__)
#define T_EXPECT_NE_STR(l, r, ...) _T_STR(0, 0, "!=", l, r, __VA_ARGS__)
#define T_ASSERT_NE_STR(l, r, ...) _T_STR(1, 0, "!=", l, r, __VA_ARGS__)

#define T_EXPECT_TRUE(c, ...) _T_BOOL(0, (c), #c, __VA_ARGS__)
#define T_ASSERT_TRUE(c, ...) _T_BOOL(1, (c), #c, __VA_ARGS__)
#define T_EXPECT_FALSE(c, ...) _T_BOOL(0, !(c), "!(" #c ")", __VA_ARGS__)
#define T_ASSERT_FALSE(c, ...) _T_BOOL(1, !(c), "!(" #c ")", __VA_ARGS__)
#define T_EXPECT_NULL(p, ...) _T_BOOL(0, (p) == NULL, #p " == NULL", __VA_ARGS__)
#define T_ASSERT_NULL(p, ...) _T_BOOL(1, (p) == NULL, #p " == NULL", __VA_ARGS__)
#define T_EXPECT_NOTNULL(p, ...) _T_BOOL(0, (p) != NULL, #p " != NULL", __VA_ARGS__)
#define T_ASSERT_NOTNULL(p, ...) _T_BOOL(1, (p) != NULL, #p " != NULL", __VA_ARGS__)

/* POSIX conventions: ZERO for functions returning an error number (pthread_*), SUCCESS for
 * functions returning -1 with errno, FAILURE for an expected -1 with a given errno. */
#define _T_POSIX_ZERO(assert, e, ...) do { \
        int _t_e = (int)(e); \
        _t_result_errno(_t_e == 0, assert, __FILE__, __LINE__, #e " == 0", _t_e, __VA_ARGS__); \
    } while (0)
#define _T_POSIX_SUCCESS(assert, e, ...) do { \
        errno = 0; long long _t_v = (long long)(e); int _t_err = errno; \
        _t_result_errno(_t_v != -1, assert, __FILE__, __LINE__, #e " != -1", _t_err, __VA_ARGS__); \
    } while (0)
#define _T_POSIX_FAILURE(assert, e, want, ...) do { \
        errno = 0; long long _t_v = (long long)(e); int _t_err = errno; \
        _t_result_errno(_t_v == -1 && _t_err == (want), assert, __FILE__, __LINE__, \
                        #e " == -1 && errno == " #want, _t_err, __VA_ARGS__); \
    } while (0)
#define _T_POSIX_NOTNULL(assert, e, ...) do { \
        errno = 0; const void *_t_p = (const void *)(e); int _t_err = errno; \
        _t_result_errno(_t_p != NULL, assert, __FILE__, __LINE__, #e " != NULL", _t_err, __VA_ARGS__); \
    } while (0)
#define T_EXPECT_POSIX_ZERO(e, ...) _T_POSIX_ZERO(0, e, __VA_ARGS__)
#define T_ASSERT_POSIX_ZERO(e, ...) _T_POSIX_ZERO(1, e, __VA_ARGS__)
#define T_EXPECT_POSIX_SUCCESS(e, ...) _T_POSIX_SUCCESS(0, e, __VA_ARGS__)
#define T_ASSERT_POSIX_SUCCESS(e, ...) _T_POSIX_SUCCESS(1, e, __VA_ARGS__)
#define T_EXPECT_POSIX_FAILURE(e, want, ...) _T_POSIX_FAILURE(0, e, want, __VA_ARGS__)
#define T_ASSERT_POSIX_FAILURE(e, want, ...) _T_POSIX_FAILURE(1, e, want, __VA_ARGS__)
#define T_EXPECT_POSIX_NOTNULL(e, ...) _T_POSIX_NOTNULL(0, e, __VA_ARGS__)
#define T_ASSERT_POSIX_NOTNULL(e, ...) _T_POSIX_NOTNULL(1, e, __VA_ARGS__)
#define T_EXPECT_MACH_SUCCESS(e, ...) _T_CMPT(0, int, ==, e, 0, __VA_ARGS__)
#define T_ASSERT_MACH_SUCCESS(e, ...) _T_CMPT(1, int, ==, e, 0, __VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif
