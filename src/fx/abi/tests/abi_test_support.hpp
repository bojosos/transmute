#pragma once

// Test support for fx_abi_tests (owner: abi): a minimal dispatcher that
// enters guest-callable code with r15 set up, fake "guest functions" behind
// the stubbed call_guest_prepared, and records of the runtime/loader/seh
// calls the library made.

#include "transmute/fx/context.hpp"
#include "transmute/fx/process.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace transmute::fx::abi_test {

inline constexpr std::uint64_t kTestReturnPc = 0x0000'1234'5678'9ab0ULL;

// A guest thread context with its own guest stack and shadow TEB.
struct TestThread {
    std::unique_ptr<GuestContext> ctx;
    std::vector<std::uint8_t> stack;    // guest stack memory
    std::vector<std::uint8_t> teb;      // shadow TEB
};
std::unique_ptr<TestThread> make_thread();

// Runs guest-callable code at `code` (a thunk) with the standard entry state
// and x30 = `return_pc`. Returns when the code jumps to stub_indirect (or an
// RSB host entry). Returns true if the RSB hit stub was taken.
bool run_guest_call(GuestContext& ctx, std::uint64_t code, std::uint64_t return_pc = kTestReturnPc);
// Pushes an RSB entry {guest_return, hit stub}.
void push_rsb_hit(GuestContext& ctx, std::uint64_t guest_return);
// Address of the stub that returns to the test (used as stub_indirect).
const void* return_stub();

// Fake guest functions: addresses inside a reserved non-executable range
// that is_guest_code() reports as guest code; call_guest_prepared runs the
// C++ body, then clobbers caller-saved registers like real guest code.
using GuestBody = std::function<void(GuestContext&)>;
std::uint64_t make_guest_function(GuestBody body);
std::uint64_t guest_call_count(std::uint64_t function);

// Records.
struct Records {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> registered_code;   // register_guest_code_memory
    std::vector<std::pair<std::uint64_t, std::uint64_t>> unregistered_code;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> invalidated;
    std::vector<GuestThreadStart> threads;
    std::vector<std::string> seh_calls;
    std::vector<std::string> loader_calls;            // loader.hpp entry points, with arguments
    std::vector<std::uint64_t> delay_load_args;       // last guest_resolve_delay_loaded_api call
    std::uint64_t host_entries{};
    int guest_stacks_allocated{};
    int guest_stacks_freed{};
    std::uint32_t last_exit_code{};
    int process_exits{};
    int process_terminations{};
    int contexts_created{};
};
Records& records();
void reset_records();

// Loader stub configuration.
void set_test_modules(std::vector<GuestModule> modules);
void set_test_stack_reserve(std::uint64_t reserve);

} // namespace transmute::fx::abi_test
