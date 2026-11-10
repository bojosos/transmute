// linux backend of fx/host/sample.hpp and fx/host/diagnostics.hpp.
//
// Thread sampling (the profiler, the image-audit tool and the arena's
// reclaim scan) needs a signal-based suspend (tgkill and a handler that
// copies the thread's ucontext_t and stack), which is not written yet: every
// call reports failure, and the callers already treat that as "no sample".
// Symbols come from dladdr. Core-type statistics have no Linux source.
#include "fx/host/diagnostics.hpp"
#include "fx/host/sample.hpp"
#include "fx/host/time.hpp"

#include <dlfcn.h>

#include <cstdio>

namespace transmute::fx::host {

namespace sample {

Thread open_thread(std::uint32_t) noexcept { return nullptr; }
Thread open_current_thread() noexcept { return nullptr; }
bool suspend(Thread) noexcept { return false; }
void resume(Thread) noexcept {}
bool capture(Thread, Registers&, bool) noexcept { return false; }
std::size_t copy_stack(std::uint64_t, std::uint8_t*, std::size_t) noexcept { return 0; }
void close_thread(Thread) noexcept {}
std::size_t capture_words(Thread, std::uint64_t*, std::size_t, std::uint64_t&) noexcept { return 0; }
bool scan_stack(std::uint64_t, void (*)(void*, std::uint64_t), void*) noexcept { return false; }
unsigned unwind_copy(const Registers&, const std::uint8_t*, std::size_t, std::uint64_t*, unsigned,
                     bool (*)(std::uint64_t)) noexcept {
    return 0;
}
bool load_symbols() noexcept { return true; }

std::string describe(std::uint64_t pc, bool symbols) {
    Dl_info info{};
    if (!dladdr(reinterpret_cast<void*>(pc), &info) || !info.dli_fname) return "?";
    std::string module = info.dli_fname;
    if (const auto slash = module.rfind('/'); slash != std::string::npos) module.erase(0, slash + 1);
    if (symbols && info.dli_sname) return module + "!" + info.dli_sname;
    char offset[32];
    std::snprintf(offset, sizeof(offset), "+0x%llx",
                  static_cast<unsigned long long>(pc - reinterpret_cast<std::uint64_t>(info.dli_fbase)));
    return module + offset;
}

std::string symbol(std::uint64_t pc) {
    Dl_info info{};
    return dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_sname ? info.dli_sname : "";
}

bool source_line(std::uint64_t, std::string&, unsigned&) { return false; }
bool wait_module(std::uint64_t&, std::uint64_t&) noexcept { return false; }
Timer create_timer() noexcept { return nullptr; }
void wait(Timer, std::uint32_t) noexcept { sleep_ms(1); }

} // namespace sample

void corestats_start_if_requested(void (*)(std::vector<std::uint32_t>&)) {}
void corestats_flush() {}

} // namespace transmute::fx::host
