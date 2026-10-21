// fx host seams, win32 backend: the seams agree with the Windows API they wrap.
// This translation unit includes windows.h and the inline backend headers
// together, which also checks that win32/kernel32.hpp's declarations match
// the SDK's.

#include <windows.h>

#include "fx/host/fault.hpp"
#include "fx/host/sync.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/unwind.hpp"
#include "fx/host/vm.hpp"
#include "fx/testing/test.hpp"

#include <string>

namespace transmute::fx {

const host::UnwindEntry* lookup_function_entry_for_tests(std::uint64_t pc, std::uint64_t* image_base) {
    DWORD64 base = 0;
    const auto* entry = RtlLookupFunctionEntry(pc, &base, nullptr);
    *image_base = base;
    return reinterpret_cast<const host::UnwindEntry*>(entry);
}

void remove_directory_for_tests(const std::wstring& path) { RemoveDirectoryW(path.c_str()); }

namespace {

FX_TEST(host_win32_inline_calls_match) {
    FX_CHECK_EQ(host::current_thread_id(), static_cast<std::uint32_t>(GetCurrentThreadId()));
    FX_CHECK(host::thread_block() == NtCurrentTeb());
    FX_CHECK_EQ(host::thread_stack_base(),
                reinterpret_cast<std::uint64_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase));
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    FX_CHECK_EQ(host::tick_frequency(), static_cast<std::uint64_t>(frequency.QuadPart));
    host::RwLock lock;
    static_assert(sizeof(host::RwLock) == sizeof(SRWLOCK) && sizeof(host::CondVar) == sizeof(CONDITION_VARIABLE));
    lock.lock();
    FX_CHECK(!TryAcquireSRWLockShared(lock.native()));
    lock.unlock();
}

FX_TEST(host_win32_context_accessors) {
    CONTEXT context{};
    context.Rax = 1;
    context.Rsp = 2;
    context.R10 = 3;
    context.R15 = 4;
    context.Rip = 5;
    context.EFlags = 0x246;
    context.Xmm7.Low = 0x0102030405060708ull;
    auto& c = *reinterpret_cast<host::CpuContext*>(&context);
    FX_CHECK_EQ(host::gpr(c, 0), 1u);
    FX_CHECK_EQ(host::gpr(c, 4), 2u);
    FX_CHECK_EQ(host::rsp(c), 2u);
    FX_CHECK_EQ(host::gpr(c, 10), 3u);
    FX_CHECK_EQ(host::gpr(c, 15), 4u);
    FX_CHECK_EQ(host::rip(c), 5u);
    FX_CHECK_EQ(host::eflags(c), 0x246u);
    FX_CHECK_EQ(host::xmm(c, 7)[0], 0x08);
    host::rip(c) = 6;
    FX_CHECK_EQ(context.Rip, 6u);
}

FX_TEST(host_win32_protection_codes) {
    void* page = host::vm::allocate(nullptr, 0x1000, host::vm::kExecuteRead);
    MEMORY_BASIC_INFORMATION info{};
    FX_CHECK(VirtualQuery(page, &info, sizeof(info)) == sizeof(info));
    FX_CHECK_EQ(info.Protect, static_cast<DWORD>(PAGE_EXECUTE_READ));
    FX_CHECK(host::vm::release(page));
}

} // namespace
} // namespace transmute::fx
