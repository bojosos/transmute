#pragma once

// Test-only runtime/abi/cache stand-ins for fx_loader_tests. call_guest runs
// guest code with the reference interpreter on direct host memory; host
// functions bound through bind_host_function become "fake" guest-callable
// addresses whose behaviour is emulated in C++.

#include "transmute/fx/context.hpp"
#include "transmute/fx/process.hpp"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fxl_test {

using transmute::fx::GuestContext;
using HostFn = std::function<std::uint64_t(GuestContext&)>;

struct HostBind {
    std::string dll;
    std::string function;
    const void* host_address;
    std::uint64_t guest_address;
};

struct GuestCall {
    std::uint64_t entry;
    std::vector<std::uint64_t> args;
};

struct Stubs {
    std::recursive_mutex lock;
    std::vector<HostBind> host_binds;
    std::map<std::string, std::uint64_t> replacements;    // function -> replacement
    std::vector<std::string> replacement_queries;          // "dll!function"
    std::map<std::string, HostFn> emulations;             // host function name -> behaviour
    std::vector<std::wstring> cache_loaded;
    std::vector<std::wstring> cache_unloaded;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> invalidated;
    std::vector<GuestCall> guest_calls;
    int runtime_initialized = 0;
    int cache_flushes = 0;
    bool execute = true;               // run guest code (else call_guest returns `result`)
    std::uint64_t result = 1;
    std::uint64_t steps = 0;           // interpreted instructions
    std::uint64_t stack_reserve_seen = 0;   // guest_image_stack_* when a context was created
    std::uint64_t stack_commit_seen = 0;
};

Stubs& stubs();
// Clears recorded calls and emulations (fake addresses stay valid).
void reset_stubs();

// A guest-callable address handled by `fn` (x0 = return value).
std::uint64_t fake_host(HostFn fn);
bool is_fake_host(std::uint64_t address);

// Calls guest code on the calling thread (creates the thread's context).
std::uint64_t run_guest(std::uint64_t entry, std::initializer_list<std::uint64_t> args = {});
// Ends the calling thread's fake context (loader_thread_detach + free).
void end_thread_context();

// Fixture paths.
std::wstring fixture(const wchar_t* name);          // fx_loader_fixtures/<name>
std::wstring legacy_fixture(const wchar_t* name);   // C:\dev\transmute\build\<name>
bool exists(const std::wstring& path);

// Resets the loader and stubs between tests.
void fresh_loader();

} // namespace fxl_test
