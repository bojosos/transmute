#include "transmute/a64.hpp"
#include "test_runner.hpp"

#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

constexpr std::uint64_t code_base = 0x100000000ULL;
constexpr std::uint64_t data_base = 0x200000000ULL;
constexpr std::uint32_t ret = 0xd65f03c0U;

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

template<class Action>
void require_error(Action&& action, std::string_view expected) {
    try {
        action();
    } catch (const std::exception& error) {
        require(std::string_view(error.what()).find(expected) != std::string_view::npos,
                "unexpected error diagnostic");
        return;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}

void map_code(transmute::ReferenceEngine& engine,
              std::initializer_list<std::uint32_t> instructions) {
    engine.map_memory(code_base, instructions.size() * 4);
    auto address = code_base;
    for (const auto instruction : instructions) {
        engine.write32(address, instruction);
        address += 4;
    }
    engine.set_memory_access(code_base, true, false, true);
}

void neutral_return() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    map_code(engine, {0x52800540U, ret}); // MOV W0,#42; RET
    const auto result = engine.run();
    require(result.exit_code == 42 && result.instructions == 2,
            "mapped executable did not return through the engine sentinel");
    require_error([&] { engine.write32(code_base, ret); }, "read-only");
}

void stack_bounds_and_permissions() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    const auto [base, top] = engine.stack_bounds();
    const auto stack = engine.query_region(base);
    require(stack && stack->readable && stack->writable && !stack->executable,
            "neutral stack permissions differ from the PE stack");
    require(!engine.is_range_available(base, 16), "stack overlaps are available");
    require_error([&] { engine.map_memory(base, 16); }, "invalid guest memory mapping");
    require_error([&] { engine.set_initial_stack_pointer(top - 1); }, "aligned stack bounds");
    require_error([&] { engine.set_initial_stack_pointer(base - 16); }, "aligned stack bounds");
    require_error([&] { engine.set_initial_stack_pointer(top + 16); }, "aligned stack bounds");
    engine.set_initial_stack_pointer(top - 32);
    require(engine.state().sp == top - 32, "initial stack pointer was not applied");
    require_error([&] { engine.initialize_dlls({}); }, "requires a PE executable");

    transmute::ReferenceEngine stack_execution(transmute::ExecutionStart{base});
    require_error([&] { static_cast<void>(stack_execution.run()); }, "non-code memory");
}

void zero_backing_reservations() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    engine.reserve_memory(0, 0x100000000ULL);
    const auto reserved = engine.query_region(0x1000);
    require(reserved && reserved->base == 0 && reserved->size == 0x100000000ULL &&
            !reserved->mapped && !reserved->image && !reserved->readable &&
            !reserved->writable && !reserved->executable,
            "page-zero reservation has accessible backing");
    require(!engine.is_range_available(0x1000, 16), "reserved range is available");
    require(engine.is_range_available(code_base, 16), "adjacent reservation blocks code");
    require_error([&] { engine.map_memory(0x1000, 16); }, "invalid guest memory mapping");
    require_error([&] { engine.reserve_memory(0x1000, 16); }, "invalid guest memory reservation");
    require_error([&] { engine.reserve_memory(UINT64_MAX - 7, 16); }, "invalid guest memory reservation");
    require_error([&] { static_cast<void>(engine.read32(0x1000)); }, "outside mapped memory");
    engine.unmap_memory(0);
    require(!engine.query_region(0x1000) && engine.is_range_available(0x1000, 16),
            "released reservation remains unavailable");
    engine.map_memory(0x1000, 16);
    engine.write32(0x1000, 42);
    require(engine.read32(0x1000) == 42, "released reservation cannot be mapped");
}

void supervisor_calls(bool jit) {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    map_code(engine, {0x528000e0U, 0xd4001001U, 0x91000400U,
                      0xd4000841U, 0x52800000U, ret});
    // MOV W0,#7; SVC #0x80; ADD X0,X0,#1; SVC #0x42; MOV W0,#0; RET
    engine.set_jit_enabled(jit);
    unsigned calls = 0;
    engine.set_supervisor_call_handler(
        [&](const transmute::SupervisorCall& call, transmute::CpuState& state,
            transmute::GuestMemory& memory) -> std::optional<std::uint32_t> {
            ++calls;
            require(state.pc == call.pc, "trap state lost its instruction PC");
            if (calls == 1) {
                require(call.pc == code_base + 4 && call.immediate == 0x80 && state.x[0] == 7,
                        "first supervisor call was decoded incorrectly");
                memory.map_memory(data_base, 16);
                // A second host thread must be able to acquire checked memory
                // while this handler is active; retaining a lease deadlocks.
                std::thread writer([&] { memory.write64(data_base, 0x1234); });
                writer.join();
                return std::nullopt;
            }
            require(calls == 2 && call.pc == code_base + 12 &&
                    call.immediate == 0x42 && state.x[0] == 8,
                    "supervisor call repeated or resumed at the wrong instruction");
            require(memory.read64(data_base) == 0x1234, "handler memory write was lost");
            return 17;
        });
    const auto result = engine.run();
    require(result.exit_code == 17 && result.instructions == 4 && calls == 2,
            "trap exit or instruction accounting is wrong");
    if (jit) {
        require(engine.jit_executed_instructions() > 0,
                "JIT supervisor test did not execute translated x64 instructions");
    }
}

void supervisor_budget_and_recovery() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    map_code(engine, {0xd4001001U, ret});
    unsigned calls = 0;
    engine.set_supervisor_call_handler(
        [&](const transmute::SupervisorCall&, transmute::CpuState&,
            transmute::GuestMemory&) -> std::optional<std::uint32_t> {
            ++calls;
            return std::nullopt;
        });
    require_error([&] { static_cast<void>(engine.run(1)); }, "instruction limit");
    require(calls == 1 && engine.state().pc == code_base + 4,
            "trap did not consume exactly one instruction");
    require(engine.run(1).instructions == 1 && calls == 1,
            "resuming after the budget repeated a supervisor call");
}

void repeated_supervisor_calls(bool jit) {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    map_code(engine, {0xd2800000U, 0x91000400U, 0xd4001001U,
                      0xf1000c1fU, 0x54ffffa1U, ret});
    // MOV X0,#0; ADD X0,X0,#1; SVC #0x80; CMP X0,#3; B.NE ADD; RET
    engine.set_jit_enabled(jit);
    unsigned calls = 0;
    engine.set_supervisor_call_handler(
        [&](const transmute::SupervisorCall& call, transmute::CpuState& state,
            transmute::GuestMemory&) -> std::optional<std::uint32_t> {
            ++calls;
            require(call.pc == code_base + 8 && state.x[0] == calls,
                    "cached loop replayed or skipped a supervisor call");
            return std::nullopt;
        });
    const auto result = engine.run();
    require(result.exit_code == 3 && result.instructions == 14 && calls == 3,
            "cached trap loop returned the wrong result or instruction count");
    if (jit) {
        require(engine.jit_executed_instructions() > 0,
                "cached trap loop did not execute translated instructions");
    }
}

void supervisor_failure_pc() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    map_code(engine, {0xd4001001U, ret});
    require_error([&] { static_cast<void>(engine.run()); }, "supervisor call #0x80");
    require(engine.state().pc == code_base, "unhandled trap skipped its instruction");
    engine.set_supervisor_call_handler(
        [](const transmute::SupervisorCall&, transmute::CpuState& state,
           transmute::GuestMemory& memory) -> std::optional<std::uint32_t> {
            memory.unmap_memory(code_base);
            state.pc = code_base + 4;
            throw std::runtime_error("personality failure");
        });
    require_error([&] { static_cast<void>(engine.run()); }, "personality failure");
    require(engine.state().pc == code_base, "failed trap did not restore its instruction PC");
}

void worker_supervisor_handler() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    map_code(engine, {0xd4001001U, ret});
    engine.set_supervisor_call_handler(
        [](const transmute::SupervisorCall&, transmute::CpuState& state,
           transmute::GuestMemory&) -> std::optional<std::uint32_t> {
            return static_cast<std::uint32_t>(state.x[0]);
        });
    auto worker = engine.fork_thread(code_base, 23);
    require(worker->stack_bounds() != engine.stack_bounds(), "worker shares its parent's stack");
    const auto result = worker->run();
    require(result.exit_code == 23 && result.instructions == 1,
            "worker did not inherit the supervisor call handler");
}

} // namespace

int main(int argc, char** argv) {
    try {
        transmute::test_detail::TestRunner runner;
        runner.configure(argc, argv);
        runner.run("neutral_return", neutral_return);
        runner.run("neutral_stack", stack_bounds_and_permissions);
        runner.run("zero_backing_reservations", zero_backing_reservations);
        runner.run("supervisor_interpreter", [] { supervisor_calls(false); });
        runner.run("repeated_supervisor_interpreter", [] { repeated_supervisor_calls(false); });
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        runner.run("supervisor_jit", [] { supervisor_calls(true); });
        runner.run("repeated_supervisor_jit", [] { repeated_supervisor_calls(true); });
#endif
        runner.run("supervisor_budget", supervisor_budget_and_recovery);
        runner.run("supervisor_failure_pc", supervisor_failure_pc);
        runner.run("worker_supervisor_handler", worker_supervisor_handler);
        runner.finish();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
