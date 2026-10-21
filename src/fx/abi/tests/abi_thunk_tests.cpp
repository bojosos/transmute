// Guest->host thunks against real Windows/UCRT APIs and native test
// functions covering every argument and result class.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/tests/abi_test_support.hpp"
#include "fx/testing/test.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::abi_test;

namespace {

std::uint64_t api(const char* dll, const char* name) {
    HMODULE module = GetModuleHandleA(dll);
    if (!module) module = LoadLibraryA(dll);
    auto* p = module ? GetProcAddress(module, name) : nullptr;
    if (!p) return 0;
    return bind_host_function(dll, name, reinterpret_cast<const void*>(p));
}

std::uint64_t typed(const void* native, const char* sig) {
    return abi::bind_host_pointer(native, abi::intern_signature(sig));
}

std::uint64_t u64(const void* p) { return reinterpret_cast<std::uint64_t>(p); }

double d_of(const GuestContext& ctx, unsigned v) {
    double d;
    std::memcpy(&d, &ctx.v[v][0], 8);
    return d;
}
float f_of(const GuestContext& ctx, unsigned v) {
    float f;
    std::memcpy(&f, &ctx.v[v][0], 4);
    return f;
}
void set_d(GuestContext& ctx, unsigned v, double d) {
    std::memcpy(&ctx.v[v][0], &d, 8);
    ctx.v[v][1] = 0x1111111111111111ULL;   // garbage high half must be ignored
}
void set_f(GuestContext& ctx, unsigned v, float f) {
    ctx.v[v][0] = 0xabcdabcd00000000ULL;
    std::memcpy(&ctx.v[v][0], &f, 4);
    ctx.v[v][1] = 0x2222222222222222ULL;
}
std::uint64_t bits(double d) { std::uint64_t b; std::memcpy(&b, &d, 8); return b; }

// Pushes `count` 8-byte stack arguments below the guest SP.
std::uint64_t* push_stack_args(GuestContext& ctx, unsigned count) {
    ctx.x[31] = (ctx.x[31] - 8ull * count - 64) & ~std::uint64_t{15};
    return reinterpret_cast<std::uint64_t*>(ctx.x[31]);
}

// ---- native test functions -------------------------------------------------------

struct S3 { char a, b, c; };
struct S12 { int a, b, c; };
struct S16 { long long a, b; };
struct S24 { long long a, b, c; };
struct V2f { float x, y; };
struct V3f { float x, y, z; };
struct V4d { double a, b, c, d; };
struct D1 { double v; };
struct F1 { float v; };

extern "C" {
__declspec(noinline) long long native_sum10(long long a, long long b, long long c, long long d, long long e,
                                            long long f, long long g, long long h, long long i, long long j) {
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h + 9 * i + 10 * j;
}
__declspec(noinline) double native_mix10(int a, double b, int c, float d, long long e, double f, int g, float h,
                                         int i, double j) {
    return a + b * 2 + c * 3 + d * 4 + static_cast<double>(e) * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}
__declspec(noinline) S12 native_make12(int a) { return S12{a, a + 1, a + 2}; }
__declspec(noinline) S16 native_make16(long long a) { return S16{a, -a}; }
__declspec(noinline) S24 native_make24(long long a) { return S24{a, a * 2, a * 3}; }
__declspec(noinline) V2f native_make_v2(float x) { return V2f{x, x * 2}; }
__declspec(noinline) V3f native_make_v3(float x) { return V3f{x, x + 1, x + 2}; }
__declspec(noinline) V4d native_make_v4(double x) { return V4d{x, x * 2, x * 3, x * 4}; }
__declspec(noinline) D1 native_make_d1(double x) { return D1{x * 3}; }
__declspec(noinline) F1 native_make_f1(float x) { return F1{x * 5}; }
__declspec(noinline) long long native_take_records(S3 s3, S12 s12, S24 s24, V2f v2, V4d v4, S16 s16) {
    return s3.a + s3.b * 10 + s3.c * 100 + s12.a * 1000 + s12.c * 10000 + s24.c * 100000 +
           static_cast<long long>(v2.y * 1000000) + static_cast<long long>(v4.d * 10000000) + s16.b * 100000000;
}
__declspec(noinline) signed char native_narrow_i8(int x) { return static_cast<signed char>(x); }
__declspec(noinline) unsigned short native_narrow_u16(int x) { return static_cast<unsigned short>(x); }
__declspec(noinline) int native_narrow_i32(long long x) { return static_cast<int>(x); }
__declspec(noinline) unsigned native_narrow_u32(long long x) { return static_cast<unsigned>(x); }
__declspec(noinline) double native_unknown_fp(double a, double b) { return a * b + 0.5; }
}

std::unique_ptr<TestThread> g_thread;
GuestContext& ctx() {
    if (!g_thread) g_thread = make_thread();
    return *g_thread->ctx;
}

} // namespace

FX_TEST(thunk_get_current_thread_id) {
    const auto thunk = api("kernel32.dll", "GetCurrentThreadId");
    FX_REQUIRE(thunk != 0);
    FX_CHECK(is_abi_thunk(thunk));
    auto& c = ctx();
    c.x[0] = 0xffffffffffffffffULL;
    run_guest_call(c, thunk);
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(GetCurrentThreadId()));
    FX_CHECK_EQ(c.pc, kTestReturnPc);   // RSB miss: returned through stub_indirect with pc = x30
    // Cached: same inputs, same thunk.
    FX_CHECK_EQ(api("kernel32.dll", "GetCurrentThreadId"), thunk);
}

FX_TEST(thunk_returns_through_rsb_hit) {
    const auto thunk = api("kernel32.dll", "GetCurrentProcessId");
    auto& c = ctx();
    c.rsb_top = 5;
    push_rsb_hit(c, kTestReturnPc);
    c.pc = 0;
    FX_CHECK(run_guest_call(c, thunk));          // jumped to the RSB host entry
    FX_CHECK_EQ(c.rsb_top, 5u);                  // popped
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(GetCurrentProcessId()));
    FX_CHECK_EQ(c.pc, 0u);                       // the fast path never writes pc
    // A different top entry misses and leaves the RSB alone.
    push_rsb_hit(c, kTestReturnPc + 4);
    FX_CHECK(!run_guest_call(c, thunk));
    FX_CHECK_EQ(c.rsb_top, 6u);
    c.rsb_top = 0;
}

FX_TEST(thunk_lstrlenw_and_environment_buffers) {
    auto& c = ctx();
    const wchar_t text[] = L"hello thunks";
    c.x[0] = u64(text);
    run_guest_call(c, api("kernel32.dll", "lstrlenW"));
    FX_CHECK_EQ(c.x[0], 12u);

    SetEnvironmentVariableW(L"FX_ABI_TEST_VAR", L"guest-visible value");
    wchar_t buffer[64] = {};
    const wchar_t name[] = L"FX_ABI_TEST_VAR";
    c.x[0] = u64(name);
    c.x[1] = u64(buffer);
    c.x[2] = 64;
    run_guest_call(c, api("kernel32.dll", "GetEnvironmentVariableW"));
    FX_CHECK_EQ(c.x[0], 19u);
    FX_CHECK(std::wstring(buffer) == L"guest-visible value");
}

FX_TEST(thunk_mirrors_last_error_into_shadow_teb) {
    auto& c = ctx();
    auto* shadow_last_error = reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(c.shadow_teb) + 0x68);
    c.x[0] = 1234;
    run_guest_call(c, api("kernel32.dll", "SetLastError"));
    FX_CHECK_EQ(*shadow_last_error, 1234u);
    run_guest_call(c, api("kernel32.dll", "GetLastError"));
    FX_CHECK_EQ(c.x[0], 1234u);
    // A failing call reports its error through the guest TEB.
    const wchar_t missing[] = L"Z:\\definitely\\missing\\file.txt";
    c.x[0] = u64(missing);
    run_guest_call(c, api("kernel32.dll", "GetFileAttributesW"));
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(INVALID_FILE_ATTRIBUTES));
    FX_CHECK(*shadow_last_error == ERROR_PATH_NOT_FOUND || *shadow_last_error == ERROR_FILE_NOT_FOUND);
}

FX_TEST(thunk_file_io_roundtrip) {
    auto& c = ctx();
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"fxa", 0, path);
    // CreateFileW: 7 arguments; positions 4-6 go to the Win64 stack.
    c.x[0] = u64(path);
    c.x[1] = GENERIC_READ | GENERIC_WRITE;
    c.x[2] = 0;
    c.x[3] = 0;
    c.x[4] = CREATE_ALWAYS;
    c.x[5] = FILE_ATTRIBUTE_NORMAL;
    c.x[6] = 0;
    run_guest_call(c, api("kernel32.dll", "CreateFileW"));
    const auto handle = c.x[0];
    FX_REQUIRE(handle != u64(INVALID_HANDLE_VALUE));

    const char payload[] = "written through an fx thunk";
    DWORD written = 0;
    c.x[0] = handle;
    c.x[1] = u64(payload);
    c.x[2] = sizeof(payload);
    c.x[3] = u64(&written);
    c.x[4] = 0;
    run_guest_call(c, api("kernel32.dll", "WriteFile"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(written, static_cast<DWORD>(sizeof(payload)));

    c.x[0] = handle;
    c.x[1] = 0;
    c.x[2] = 0;
    c.x[3] = FILE_BEGIN;
    run_guest_call(c, api("kernel32.dll", "SetFilePointer"));
    FX_CHECK_EQ(c.x[0], 0u);

    char back[64] = {};
    DWORD read = 0;
    c.x[0] = handle;
    c.x[1] = u64(back);
    c.x[2] = sizeof(back);
    c.x[3] = u64(&read);
    c.x[4] = 0;
    run_guest_call(c, api("kernel32.dll", "ReadFile"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(read, static_cast<DWORD>(sizeof(payload)));
    FX_CHECK(std::strcmp(back, payload) == 0);

    c.x[0] = handle;
    run_guest_call(c, api("kernel32.dll", "CloseHandle"));
    FX_CHECK_EQ(c.x[0], 1u);
    c.x[0] = u64(path);
    run_guest_call(c, api("kernel32.dll", "DeleteFileW"));
    FX_CHECK_EQ(c.x[0], 1u);
}

FX_TEST(thunk_locks_on_guest_owned_structures) {
    auto& c = ctx();
    CRITICAL_SECTION cs;      // guest memory: ARM64 and x64 layouts are the same
    c.x[0] = u64(&cs);
    run_guest_call(c, api("kernel32.dll", "InitializeCriticalSection"));
    c.x[0] = u64(&cs);
    run_guest_call(c, api("kernel32.dll", "EnterCriticalSection"));
    FX_CHECK(cs.OwningThread == reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(GetCurrentThreadId())));
    FX_CHECK_EQ(cs.RecursionCount, 1);
    c.x[0] = u64(&cs);
    run_guest_call(c, api("kernel32.dll", "LeaveCriticalSection"));
    FX_CHECK_EQ(cs.RecursionCount, 0);
    c.x[0] = u64(&cs);
    run_guest_call(c, api("kernel32.dll", "DeleteCriticalSection"));

    SRWLOCK lock = SRWLOCK_INIT;
    c.x[0] = u64(&lock);
    run_guest_call(c, api("kernel32.dll", "AcquireSRWLockExclusive"));
    FX_CHECK(lock.Ptr != nullptr);
    FX_CHECK(TryAcquireSRWLockShared(&lock) == FALSE);
    c.x[0] = u64(&lock);
    run_guest_call(c, api("kernel32.dll", "TryAcquireSRWLockExclusive"));
    FX_CHECK_EQ(c.x[0] & 0xff, 0u);              // BOOLEAN result, zero-extended
    c.x[0] = u64(&lock);
    run_guest_call(c, api("kernel32.dll", "ReleaseSRWLockExclusive"));
    FX_CHECK(lock.Ptr == nullptr);
}

FX_TEST(thunk_ucrt_floating_point) {
    auto& c = ctx();
    set_d(c, 0, 2.0);
    set_d(c, 1, 10.0);
    run_guest_call(c, api("ucrtbase.dll", "pow"));
    FX_CHECK_EQ(d_of(c, 0), 1024.0);
    FX_CHECK_EQ(c.v[0][1], 0u);                  // scalar result zeroes the upper half

    // ldexp(double, int): v0 -> xmm0 and x0 -> edx.
    set_d(c, 0, 1.5);
    c.x[0] = 0xffffffff00000004ULL;              // garbage upper half of w0
    run_guest_call(c, api("ucrtbase.dll", "ldexp"));
    FX_CHECK_EQ(d_of(c, 0), 24.0);

    set_f(c, 0, 0.5f);
    run_guest_call(c, api("ucrtbase.dll", "sinf"));
    FX_CHECK(std::fabs(f_of(c, 0) - std::sin(0.5f)) < 1e-6f);
    FX_CHECK_EQ(c.v[0][0] >> 32, 0u);

    // frexp(double, int*): FP in, pointer out, FP result.
    int exponent = 0;
    set_d(c, 0, 48.0);
    c.x[0] = u64(&exponent);
    run_guest_call(c, api("ucrtbase.dll", "frexp"));
    FX_CHECK_EQ(d_of(c, 0), 0.75);
    FX_CHECK_EQ(exponent, 6);

    // fma(a, b, c) with three FP registers.
    set_d(c, 0, 3.0);
    set_d(c, 1, 4.0);
    set_d(c, 2, 5.0);
    run_guest_call(c, api("ucrtbase.dll", "fma"));
    FX_CHECK_EQ(d_of(c, 0), 17.0);
}

FX_TEST(thunk_variadic_wsprintf_and_snprintf) {
    auto& c = ctx();
    char buffer[256] = {};
    const char format[] = "%d|%s|%x|%c";
    const char word[] = "abc";
    c.x[0] = u64(buffer);
    c.x[1] = u64(format);
    c.x[2] = static_cast<std::uint64_t>(-42);
    c.x[3] = u64(word);
    c.x[4] = 255;
    c.x[5] = 'Z';
    run_guest_call(c, api("user32.dll", "wsprintfA"));
    FX_CHECK_EQ(std::string(buffer), std::string("-42|abc|ff|Z"));
    FX_CHECK_EQ(c.x[0], 12u);

    // _snprintf with doubles: on Windows ARM64 variadic doubles travel in x
    // registers; Win64 wants them in the GPR slots (and xmm for the first four).
    const auto snprintf_thunk = api("msvcrt.dll", "_snprintf");
    FX_REQUIRE(snprintf_thunk != 0);
    const char fmt2[] = "%.3f|%d|%.1f|%s|%I64d|%d|%d|%.2f|%d";
    std::memset(buffer, 0, sizeof(buffer));
    c.x[0] = u64(buffer);
    c.x[1] = sizeof(buffer);
    c.x[2] = u64(fmt2);
    c.x[3] = bits(3.14159);
    c.x[4] = 7;
    c.x[5] = bits(2.5);
    c.x[6] = u64(word);
    c.x[7] = 1234567890123ULL;
    const auto original_sp = c.x[31];
    auto* stack = push_stack_args(c, 4);          // arguments 9..12 on the guest stack
    stack[0] = 8;
    stack[1] = 9;
    stack[2] = bits(-0.1251);
    stack[3] = 11;
    const auto saved_sp = c.x[31];
    run_guest_call(c, snprintf_thunk);
    FX_CHECK_EQ(std::string(buffer), std::string("3.142|7|2.5|abc|1234567890123|8|9|-0.13|11"));
    FX_CHECK_EQ(c.x[31], saved_sp);
    c.x[31] = original_sp;
}

FX_TEST(thunk_variadic_near_stack_top_does_not_overread) {
    auto& c = ctx();
    // SP 16 bytes below the guest stack base: the thunk may only read two slots.
    const auto saved = c.x[31];
    c.x[31] = c.guest_stack_base - 16;
    auto* top = reinterpret_cast<std::uint64_t*>(c.x[31]);
    top[0] = 77;
    top[1] = 88;
    char buffer[128] = {};
    const char format[] = "%d %d %d %d %d %d %d";
    c.x[0] = u64(buffer);
    c.x[1] = u64(format);
    for (unsigned n = 2; n < 8; ++n) c.x[n] = n;
    run_guest_call(c, api("user32.dll", "wsprintfA"));
    FX_CHECK_EQ(std::string(buffer), std::string("2 3 4 5 6 7 77"));
    c.x[31] = saved;
}

FX_TEST(thunk_ten_arguments) {
    auto& c = ctx();
    const auto thunk = typed(reinterpret_cast<const void*>(&native_sum10), "l(llllllllll)");
    for (unsigned n = 0; n < 8; ++n) c.x[n] = n + 1;
    const auto saved_sp = c.x[31];
    auto* stack = push_stack_args(c, 2);
    stack[0] = 9;
    stack[1] = 10;
    run_guest_call(c, thunk);
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(native_sum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)));
    c.x[31] = saved_sp;

    // Mixed integer/FP positions: d(idifldifid).
    const auto mix = typed(reinterpret_cast<const void*>(&native_mix10), "d(idifldifid)");
    c.x[0] = 1; set_d(c, 0, 2.5); c.x[1] = 3; set_f(c, 1, 4.25f); c.x[2] = 5;
    set_d(c, 2, 6.5); c.x[3] = 7; set_f(c, 3, 8.75f); c.x[4] = 9; set_d(c, 4, 10.5);
    run_guest_call(c, mix);
    FX_CHECK_EQ(d_of(c, 0), native_mix10(1, 2.5, 3, 4.25f, 5, 6.5, 7, 8.75f, 9, 10.5));
}

FX_TEST(thunk_struct_returns) {
    auto& c = ctx();
    // Real API returning a struct in rax (div_t, 8 bytes) and through a hidden
    // pointer (lldiv_t, 16 bytes -> x0/x1 on ARM64).
    c.x[0] = 17;
    c.x[1] = 5;
    run_guest_call(c, api("ucrtbase.dll", "div"));
    FX_CHECK_EQ(c.x[0] & 0xffffffff, 3u);
    FX_CHECK_EQ(c.x[0] >> 32, 2u);
    c.x[0] = static_cast<std::uint64_t>(-17);
    c.x[1] = 5;
    run_guest_call(c, api("ucrtbase.dll", "lldiv"));
    FX_CHECK_EQ(static_cast<std::int64_t>(c.x[0]), -3);
    FX_CHECK_EQ(static_cast<std::int64_t>(c.x[1]), -2);
    // _Cbuild returns _Dcomplex, an HFA of two doubles: Win64 hidden pointer, ARM64 d0/d1.
    set_d(c, 0, 1.25);
    set_d(c, 1, -7.5);
    run_guest_call(c, api("ucrtbase.dll", "_Cbuild"));
    FX_CHECK_EQ(d_of(c, 0), 1.25);
    FX_CHECK_EQ(d_of(c, 1), -7.5);
    // cabs takes the HFA by value: ARM64 d0/d1, Win64 by reference.
    set_d(c, 0, 3.0);
    set_d(c, 1, 4.0);
    run_guest_call(c, api("ucrtbase.dll", "cabs"));
    FX_CHECK_EQ(d_of(c, 0), 5.0);
    // conj: HFA in and out.
    set_d(c, 0, 2.0);
    set_d(c, 1, 9.0);
    run_guest_call(c, api("ucrtbase.dll", "conj"));
    FX_CHECK_EQ(d_of(c, 0), 2.0);
    FX_CHECK_EQ(d_of(c, 1), -9.0);
    // _FCbuild returns _Fcomplex {f2}: Win64 rax, ARM64 s0/s1.
    set_f(c, 0, 1.5f);
    set_f(c, 1, -2.5f);
    run_guest_call(c, api("ucrtbase.dll", "_FCbuild"));
    FX_CHECK_EQ(f_of(c, 0), 1.5f);
    FX_CHECK_EQ(f_of(c, 1), -2.5f);

    // Native functions for the remaining classes.
    c.x[0] = 40;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make12), "{12}(i)"));
    FX_CHECK_EQ(c.x[0], (41ull << 32) | 40u);
    FX_CHECK_EQ(c.x[1] & 0xffffffff, 42u);
    c.x[0] = 99;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make16), "{16}(l)"));
    FX_CHECK_EQ(c.x[0], 99u);
    FX_CHECK_EQ(static_cast<std::int64_t>(c.x[1]), -99);
    S24 big{};
    c.x[0] = 5;
    c.x[8] = u64(&big);                           // ARM64 indirect result register
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make24), "{24}(l)"));
    FX_CHECK(big.a == 5 && big.b == 10 && big.c == 15);
    set_f(c, 0, 1.5f);
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make_v2), "{f2}(f)"));
    FX_CHECK_EQ(f_of(c, 0), 1.5f);
    FX_CHECK_EQ(f_of(c, 1), 3.0f);
    set_f(c, 0, 1.0f);
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make_v3), "{f3}(f)"));
    FX_CHECK(f_of(c, 0) == 1.0f && f_of(c, 1) == 2.0f && f_of(c, 2) == 3.0f);
    set_d(c, 0, 0.5);
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make_v4), "{d4}(d)"));
    FX_CHECK(d_of(c, 0) == 0.5 && d_of(c, 1) == 1.0 && d_of(c, 2) == 1.5 && d_of(c, 3) == 2.0);
    set_d(c, 0, 2.0);
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make_d1), "{d1}(d)"));
    FX_CHECK_EQ(d_of(c, 0), 6.0);
    set_f(c, 0, 2.0f);
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_make_f1), "{f1}(f)"));
    FX_CHECK_EQ(f_of(c, 0), 10.0f);
}

FX_TEST(thunk_struct_arguments) {
    auto& c = ctx();
    // native_take_records(S3, S12, S24, V2f, V4d, S16):
    //   ARM64: S3 x0, S12 x1/x2, S24 -> pointer in x3, V2f s0/s1, V4d d2..d5, S16 x4/x5.
    S3 s3{1, 2, 3};
    S12 s12{4, 5, 6};
    S24 s24{7, 8, 9};
    std::memcpy(&c.x[0], &s3, sizeof(s3));
    std::memcpy(&c.x[1], &s12, 8);
    c.x[2] = static_cast<std::uint32_t>(s12.c);
    c.x[3] = u64(&s24);
    set_f(c, 0, 0.25f);
    set_f(c, 1, 0.5f);
    set_d(c, 2, 0.1);
    set_d(c, 3, 0.2);
    set_d(c, 4, 0.3);
    set_d(c, 5, 0.4);
    c.x[4] = 11;
    c.x[5] = 12;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_take_records), "l({3}{12}{24}{f2}{d4}{16})"));
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(native_take_records(s3, s12, s24, V2f{0.25f, 0.5f},
                                                                       V4d{0.1, 0.2, 0.3, 0.4}, S16{11, 12})));
    // A real API with a POINT by value: PtInRect(const RECT*, POINT).
    RECT rect{0, 0, 100, 50};
    c.x[0] = u64(&rect);
    c.x[1] = (static_cast<std::uint64_t>(20) << 32) | 10u;   // {x = 10, y = 20}
    run_guest_call(c, api("user32.dll", "PtInRect"));
    FX_CHECK_EQ(c.x[0], 1u);
    c.x[0] = u64(&rect);
    c.x[1] = (static_cast<std::uint64_t>(70) << 32) | 10u;
    run_guest_call(c, api("user32.dll", "PtInRect"));
    FX_CHECK_EQ(c.x[0], 0u);
}

FX_TEST(thunk_narrow_results_are_extended) {
    auto& c = ctx();
    c.x[0] = 0x1ff;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_narrow_i8), "c(i)"));
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(-1));
    c.x[0] = 0x12345;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_narrow_u16), "S(i)"));
    FX_CHECK_EQ(c.x[0], 0x2345u);
    c.x[0] = 0xffffffff80000000ULL;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_narrow_i32), "i(l)"));
    FX_CHECK_EQ(c.x[0], 0xffffffff80000000ULL);
    c.x[0] = 0xffffffff80000000ULL;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_narrow_u32), "I(l)"));
    FX_CHECK_EQ(c.x[0], 0x80000000ULL);
}

FX_TEST(thunk_unknown_signature_positional) {
    auto& c = ctx();
    const auto thunk = bind_unknown_host_target(reinterpret_cast<const void*>(&native_sum10));
    FX_REQUIRE(thunk != 0);
    FX_CHECK_EQ(bind_unknown_host_target(reinterpret_cast<const void*>(&native_sum10)), thunk);
    for (unsigned n = 0; n < 8; ++n) c.x[n] = 10 + n;
    const auto saved_sp = c.x[31];
    auto* stack = push_stack_args(c, 2);
    stack[0] = 18;
    stack[1] = 19;
    run_guest_call(c, thunk);
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(native_sum10(10, 11, 12, 13, 14, 15, 16, 17, 18, 19)));
    c.x[31] = saved_sp;
    // v0-v3 -> xmm0-3 and xmm0 -> v0.
    const auto fp = bind_unknown_host_target(reinterpret_cast<const void*>(&native_unknown_fp));
    set_d(c, 0, 3.0);
    set_d(c, 1, 4.0);
    run_guest_call(c, fp);
    FX_CHECK_EQ(d_of(c, 0), 12.5);
    // A named host export gets its database signature.
    const auto pow_thunk = bind_unknown_host_target(
        reinterpret_cast<const void*>(GetProcAddress(GetModuleHandleW(L"ucrtbase.dll"), "pow")));
    set_d(c, 0, 3.0);
    set_d(c, 1, 3.0);
    run_guest_call(c, pow_thunk);
    FX_CHECK_EQ(d_of(c, 0), 27.0);
    FX_CHECK_EQ(abi::find_thunk(pow_thunk)->sig->text, std::string("d(dd)"));
}

FX_TEST(thunk_vcruntime_replacements) {
    auto& c = ctx();
    const auto memcpy_thunk = abi_guest_import_replacement("VCRUNTIME140.dll", "memcpy");
    FX_REQUIRE(memcpy_thunk != 0);
    FX_CHECK(is_abi_thunk(memcpy_thunk));
    FX_CHECK_EQ(abi_guest_import_replacement("vcruntime140.dll", "memcpy"), memcpy_thunk);
    FX_CHECK_EQ(abi_guest_import_replacement("VCRUNTIME140.dll", "__CxxFrameHandler3"), 0u);
    FX_CHECK_EQ(abi_guest_import_replacement("guestlib.dll", "memcpy"), 0u);
    char source[] = "replacement memcpy";
    char target[32] = {};
    c.x[0] = u64(target);
    c.x[1] = u64(source);
    c.x[2] = sizeof(source);
    run_guest_call(c, memcpy_thunk);
    FX_CHECK_EQ(c.x[0], u64(target));
    FX_CHECK(std::strcmp(target, source) == 0);
    const char haystack[] = "find the needle here";
    c.x[0] = u64(haystack);
    c.x[1] = u64("needle");
    run_guest_call(c, abi_guest_import_replacement("VCRUNTIME140.dll", "strstr"));
    FX_CHECK_EQ(c.x[0], u64(haystack + 9));
    for (const char* name : {"memchr", "memcmp", "memmove", "memset", "strchr", "strrchr", "wcschr", "wcsrchr", "wcsstr"})
        FX_CHECK(abi_guest_import_replacement("VCRUNTIME140.dll", name) != 0);

    // fabsf: exported by the ARM64 UCRT, inline-only in the x64 one. The
    // loader asks for a replacement when the host DLL lacks the export; the
    // DLL name is compared case-insensitively.
    const auto fabsf_thunk = abi_guest_import_replacement("API-MS-WIN-CRT-MATH-L1-1-0.DLL", "fabsf");
    FX_REQUIRE(fabsf_thunk != 0);
    FX_CHECK_EQ(abi_guest_import_replacement("api-ms-win-crt-math-l1-1-0.dll", "fabsf"), fabsf_thunk);
    FX_CHECK_EQ(abi_guest_import_replacement("guestlib.dll", "fabsf"), 0u);
    set_f(c, 0, -2.75f);
    run_guest_call(c, fabsf_thunk);
    FX_CHECK_EQ(f_of(c, 0), 2.75f);
    FX_CHECK_EQ(c.v[0][0] >> 32, 0u);
}

// Writes generated code as GNU assembler .byte listings when
// FX_ABI_DUMP_DIR is set (for review with clang -c + llvm-objdump -d).
FX_TEST(thunk_dump_for_review) {
    char dir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("FX_ABI_DUMP_DIR", dir, MAX_PATH) == 0) return;
    const struct { const char* name; const char* sig; } cases[] = {
        {"get_current_thread_id", "I()"}, {"create_file_w", "p(pIIpIIp)"}, {"qsort", "v(pll@i(pp))"},
        {"wsprintf", "i(pp.)"}, {"mix10", "d(idifldifid)"}, {"records", "l({3}{12}{24}{f2}{d4}{16})"},
        {"v4d", "{d4}(d)"}, {"unknown", "?"},
    };
    for (const auto& k : cases) {
        const auto code = abi::generate_host_thunk(*abi::intern_signature(k.sig),
                                                   reinterpret_cast<const void*>(0x1122334455667788ULL));
        const std::string path = std::string(dir) + "\\" + k.name + ".s";
        FILE* f = std::fopen(path.c_str(), "w");
        FX_REQUIRE(f != nullptr);
        std::fprintf(f, ".text\n.globl %s\n%s:\n", k.name, k.name);
        for (auto b : code.bytes) std::fprintf(f, ".byte 0x%02x\n", b);
        std::fclose(f);
    }
}

FX_TEST(thunk_is_abi_thunk_and_ordinals) {
    FX_CHECK(!is_abi_thunk(0));
    FX_CHECK(!is_abi_thunk(reinterpret_cast<std::uint64_t>(&native_sum10)));
    // An ordinal import is typed through the host export table.
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    const auto thunk = bind_host_function("ucrtbase.dll", "#1", reinterpret_cast<const void*>(GetProcAddress(ucrt, "sqrt")));
    const auto* record = abi::find_thunk(thunk);
    FX_REQUIRE(record != nullptr);
    FX_CHECK_EQ(record->sig->text, std::string("d(d)"));
    auto& c = ctx();
    set_d(c, 0, 81.0);
    run_guest_call(c, thunk);
    FX_CHECK_EQ(d_of(c, 0), 9.0);
}
