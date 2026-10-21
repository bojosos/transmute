// Thunks for imports that could not be resolved. The load never fails on a
// missing import; calling one reports "<dll>!<function>" instead.

#include "fx/loader/loader_internal.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace transmute::fx {
namespace loader {
namespace {

struct MissingImport {
    std::string dll;
    std::string function;
};

std::atomic<MissingImportHandler> g_handler{nullptr};

std::mutex& missing_lock() {
    static std::mutex* m = new std::mutex;
    return *m;
}

std::unordered_map<std::string, std::uint64_t>& thunk_cache() {
    static auto* cache = new std::unordered_map<std::string, std::uint64_t>;
    return *cache;
}

std::uint64_t default_handler(const char* dll, const char* function) {
    static std::mutex* lock = new std::mutex;
    static std::set<std::string>* reported = new std::set<std::string>;
    bool first = false;
    {
        std::lock_guard guard(*lock);
        first = reported->insert(std::string(dll) + "!" + function).second;
    }
    if (first) {
        std::fprintf(stderr, "transmute-fx: guest called unresolved import %s!%s\n", dll, function);
        std::fflush(stderr);
    }
    char mode[16] = {};
    if (GetEnvironmentVariableA("TRANSMUTE_FX_MISSING_IMPORT", mode, sizeof(mode)) &&
        _stricmp(mode, "abort") == 0) {
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(0xC0000139L));   // STATUS_ENTRYPOINT_NOT_FOUND
    }
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

// Win64 target of every missing-import thunk (rcx = MissingImport*).
std::uint64_t report_missing(const MissingImport* info) {
    MissingImportHandler handler = g_handler.load(std::memory_order_acquire);
    if (!handler) handler = default_handler;
    return handler(info->dll.c_str(), info->function.c_str());
}

} // namespace

std::uint64_t missing_import_thunk(const std::string& dll, const std::string& function) {
    std::lock_guard guard(missing_lock());
    const std::string key = dll + "!" + function;
    auto& cache = thunk_cache();
    if (const auto it = cache.find(key); it != cache.end()) return it->second;
    // Host stub: mov rcx, imm64(info); mov rax, imm64(report); jmp rax. It
    // ignores the guest arguments; bind_unknown_host_target makes it
    // guest-callable.
    auto* info = new MissingImport{dll, function};   // lives for the process
    auto* code = static_cast<std::uint8_t*>(allocate_code(32, 16));
    if (!code) return 0;
    const std::uint64_t info_value = reinterpret_cast<std::uint64_t>(info);
    const std::uint64_t target = reinterpret_cast<std::uint64_t>(&report_missing);
    std::uint8_t stub[22] = {0x48, 0xB9};
    std::memcpy(stub + 2, &info_value, 8);
    stub[10] = 0x48;
    stub[11] = 0xB8;
    std::memcpy(stub + 12, &target, 8);
    stub[20] = 0xFF;
    stub[21] = 0xE0;
    std::memcpy(code, stub, sizeof(stub));
    FlushInstructionCache(GetCurrentProcess(), code, sizeof(stub));
    const std::uint64_t guest = bind_unknown_host_target(code);
    cache.emplace(key, guest);
    return guest;
}

void reset_missing_for_testing() {
    std::lock_guard guard(missing_lock());
    thunk_cache().clear();
}

} // namespace loader

void loader_set_missing_import_handler(MissingImportHandler handler) {
    loader::g_handler.store(handler, std::memory_order_release);
}

} // namespace transmute::fx
