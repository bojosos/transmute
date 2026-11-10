// fx host seams, linux backend: the inline accessors agree with the kernel's
// structures, and the address-space table agrees with /proc/self/maps.

#include <pthread.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "fx/host/fault.hpp"
#include "fx/host/guard.hpp"
#include "fx/host/sync.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/vm.hpp"
#include "fx/testing/test.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace transmute::fx {

void remove_directory_for_tests(const std::wstring& path) {
    std::string narrow;
    for (const auto c : path) narrow += static_cast<char>(c);   // the test directory name is ASCII
    rmdir(narrow.c_str());
}

namespace {

// The permissions column of /proc/self/maps for the mapping holding `address`.
std::string maps_permissions(const void* address) {
    const auto at = reinterpret_cast<unsigned long long>(address);
    FILE* maps = std::fopen("/proc/self/maps", "r");
    char line[512];
    std::string found;
    while (maps && std::fgets(line, sizeof(line), maps)) {
        unsigned long long begin = 0, end = 0;
        char perms[8]{};
        if (std::sscanf(line, "%llx-%llx %7s", &begin, &end, perms) == 3 && at >= begin && at < end) {
            found = perms;
            break;
        }
    }
    if (maps) std::fclose(maps);
    return found;
}

FX_TEST(host_linux_inline_calls_match) {
    FX_CHECK_EQ(host::current_thread_id(), static_cast<std::uint32_t>(syscall(SYS_gettid)));
    FX_CHECK(host::thread_block() == nullptr);
    pthread_attr_t attributes;
    FX_CHECK(pthread_getattr_np(pthread_self(), &attributes) == 0);
    void* low = nullptr;
    std::size_t size = 0;
    pthread_attr_getstack(&attributes, &low, &size);
    pthread_attr_destroy(&attributes);
    FX_CHECK_EQ(host::thread_stack_base(), reinterpret_cast<std::uint64_t>(low) + size);
    int local = 0;
    FX_CHECK(reinterpret_cast<std::uint64_t>(&local) < host::thread_stack_base());
    FX_CHECK_EQ(host::tick_frequency(), 1000000000u);
    host::RwLock lock;
    lock.lock();
    FX_CHECK(!lock.try_lock_shared());
    lock.unlock();
    FX_CHECK(lock.try_lock_shared());
    lock.unlock_shared();
}

FX_TEST(host_linux_context_accessors) {
    ucontext_t context{};
    _libc_fpstate fpstate{};
    context.uc_mcontext.fpregs = &fpstate;
    context.uc_mcontext.gregs[REG_RAX] = 1;
    context.uc_mcontext.gregs[REG_RSP] = 2;
    context.uc_mcontext.gregs[REG_R10] = 3;
    context.uc_mcontext.gregs[REG_R15] = 4;
    context.uc_mcontext.gregs[REG_RIP] = 5;
    context.uc_mcontext.gregs[REG_EFL] = 0x246;
    context.uc_mcontext.gregs[REG_RSI] = 6;
    fpstate._xmm[7].element[0] = 0x05060708u;
    auto& c = *reinterpret_cast<host::CpuContext*>(&context);
    FX_CHECK_EQ(host::gpr(c, 0), 1u);
    FX_CHECK_EQ(host::gpr(c, 4), 2u);
    FX_CHECK_EQ(host::rsp(c), 2u);
    FX_CHECK_EQ(host::gpr(c, 6), 6u);
    FX_CHECK_EQ(host::gpr(c, 10), 3u);
    FX_CHECK_EQ(host::gpr(c, 15), 4u);
    FX_CHECK_EQ(host::rip(c), 5u);
    FX_CHECK_EQ(host::eflags(c), 0x246u);
    FX_CHECK_EQ(host::xmm(c, 7)[0], 0x08);
    host::rip(c) = 6;
    FX_CHECK_EQ(context.uc_mcontext.gregs[REG_RIP], 6);
}

FX_TEST(host_linux_protection_codes) {
    namespace vm = host::vm;
    void* page = vm::allocate(nullptr, 0x1000, vm::kExecuteRead);
    FX_CHECK(page != nullptr);
    FX_CHECK(maps_permissions(page).substr(0, 3) == "r-x");
    FX_CHECK(vm::protect(page, 0x1000, vm::kReadWrite));
    FX_CHECK(maps_permissions(page).substr(0, 3) == "rw-");
    vm::RegionInfo info;
    FX_CHECK(vm::query(page, info));
    FX_CHECK(info.committed());
    FX_CHECK_EQ(info.protect, vm::kReadWrite);
    FX_CHECK_EQ(info.allocation_base, reinterpret_cast<std::uint64_t>(page));
    FX_CHECK_EQ(reinterpret_cast<std::uint64_t>(page) & 0xffff, 0u);   // 64 KiB granules, as on Windows
    FX_CHECK(vm::release(page));
    FX_CHECK(maps_permissions(page).empty());
}

FX_TEST(host_linux_guard_page_fires_once) {
    namespace vm = host::vm;
    auto* page = static_cast<volatile std::uint8_t*>(vm::allocate(nullptr, 0x1000, vm::kReadWrite | vm::kGuard));
    FX_CHECK(page != nullptr);
    std::uint32_t code = 0;
    host::FaultRecord fault{};
    FX_CHECK(!host::call_guarded([&] { page[0] = 1; }, fault));
    code = fault.code;
    FX_CHECK_EQ(code, host::kFaultGuardPage);
    page[0] = 2;   // the guard is gone
    FX_CHECK_EQ(static_cast<int>(page[0]), 2);
    vm::release(const_cast<std::uint8_t*>(page));
}

FX_TEST(host_linux_placeholder_views) {
    namespace vm = host::vm;
    auto* base = static_cast<std::uint8_t*>(vm::reserve_placeholder(nullptr, 0x20000));
    FX_CHECK(base != nullptr);
    FX_CHECK(vm::split_placeholder(base, 0x10000));
    const auto section = vm::create_section(0x10000);
    FX_CHECK(section != nullptr);
    auto* first = static_cast<std::uint8_t*>(vm::map_view_into_placeholder(section, base, 0, 0x10000, vm::kReadWrite));
    auto* second =
        static_cast<std::uint8_t*>(vm::map_view_into_placeholder(section, base + 0x10000, 0, 0x10000, vm::kReadWrite));
    FX_CHECK(first == base && second == base + 0x10000);
    first[5] = 42;
    FX_CHECK_EQ(static_cast<int>(second[5]), 42);   // two views of one section alias
    vm::RegionInfo info;
    FX_CHECK(vm::query(second, info));
    FX_CHECK(info.type == vm::Type::mapped);
    FX_CHECK_EQ(info.allocation_base, reinterpret_cast<std::uint64_t>(second));
    FX_CHECK(vm::unmap_view_to_placeholder(second));
    FX_CHECK(vm::query(second, info));
    FX_CHECK(info.state == vm::State::reserved);
    FX_CHECK(maps_permissions(second).substr(0, 3) == "---");   // the address stays taken
    FX_CHECK(vm::unmap_view_to_placeholder(first));
    FX_CHECK(vm::coalesce_placeholders(base, 0x20000));
    vm::close_section(section);
}

} // namespace
} // namespace transmute::fx
