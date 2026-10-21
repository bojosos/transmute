#include "alchemy/darwin_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t data_base = 0x200000000ULL;
constexpr std::uint64_t code_base = 0x100000000ULL;
constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::uint32_t carry_flag = 1U << 29;

void require(bool condition, std::string_view description) {
    if (!condition) throw std::runtime_error(std::string(description));
}

template <typename Operation>
void require_error(Operation operation, std::string_view expected) {
    try {
        operation();
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).find(expected) != std::string_view::npos,
                std::string("wrong failure diagnostic: ") + error.what());
        return;
    }
    throw std::runtime_error("operation unexpectedly succeeded");
}

void write_string(transmute::GuestMemory& memory, std::uint64_t address,
                  std::string_view text) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        memory.write8(address + index, static_cast<std::uint8_t>(text[index]));
    }
    memory.write8(address + text.size(), 0);
}

std::string read_string(const transmute::GuestMemory& memory, std::uint64_t address) {
    std::string result;
    for (unsigned count = 0; count < 4096; ++count) {
        const auto byte = memory.read8(address + count);
        if (byte == 0) return result;
        result.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("test string is unterminated");
}

struct Capture {
    std::string output;
    std::vector<unsigned> descriptors;
    std::size_t per_call = std::numeric_limits<std::size_t>::max();
    std::uint32_t error{};
    std::size_t fail_on_call{};

    alchemy::OutputResult operator()(unsigned descriptor,
                                     std::span<const std::uint8_t> bytes) {
        descriptors.push_back(descriptor);
        const auto count = fail_on_call == descriptors.size() ? 0 :
            std::min(per_call, bytes.size());
        output.append(reinterpret_cast<const char*>(bytes.data()), count);
        return {count, error};
    }
};

void test_formatting_and_registry() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{0});
    Capture capture;
    alchemy::DarwinRuntime runtime(engine, [&capture](auto descriptor, auto bytes) {
        return capture(descriptor, bytes);
    });
    engine.map_memory(data_base, 4096);
    const auto printf_address = runtime.resolve(system_library, "_printf");
    const auto puts_address = runtime.resolve(system_library, "_puts");
    require(printf_address && puts_address, "replacement exports missing");
    require(alchemy::DarwinRuntime::is_system_library(system_library),
            "libSystem provider missing");
    require(alchemy::DarwinRuntime::is_system_library("/usr/lib/libc++.1.dylib"),
            "limited C++ provider missing");
    require(!runtime.resolve("/usr/lib/libc++.1.dylib", "_printf"),
            "C++ provider incorrectly exports printf");
    require(!runtime.resolve("other.dylib", "_printf") &&
            !runtime.resolve(system_library, "_malloc"), "unsupported export resolved");

    write_string(engine, data_base, "number=%d string=%s second=%d %%\n");
    write_string(engine, data_base + 128, "hello");
    const auto call_sp = (engine.state().sp - 256) & ~15ULL;
    engine.write64(call_sp, 0xfeedface80000000ULL);
    engine.write64(call_sp + 8, data_base + 128);
    engine.write64(call_sp + 16, 0x123456780000001bULL);
    const std::string expected = "number=-2147483648 string=hello second=27 %\n";
    const auto printed = engine.invoke_callback(*printf_address, data_base);
    require(capture.output == expected, "printf ignored Apple stack slots or int width");
    require(printed == expected.size(), "printf returned the wrong byte count");
    require(capture.descriptors == std::vector<unsigned>{1}, "printf descriptor wrong");

    capture.output.clear();
    capture.descriptors.clear();
    capture.per_call = 2;
    const auto put = engine.invoke_callback(*puts_address, data_base + 128);
    require(capture.output == "hello\n" && put == 6,
            "puts or partial-write retry produced incorrect bytes");
    require(capture.descriptors.size() == 3, "puts did not retry partial output");

    capture.output.clear();
    write_string(engine, data_base, "before %x");
    require_error([&] { static_cast<void>(engine.invoke_callback(*printf_address, data_base)); },
                  "unsupported Darwin printf conversion %x");
    require(capture.output.empty(), "unsupported formatting wrote partial output");
    write_string(engine, data_base, "trailing %");
    require_error([&] { static_cast<void>(engine.invoke_callback(*printf_address, data_base)); },
                  "incomplete conversion");
    write_string(engine, data_base, "%s");
    engine.write64(call_sp, 0);
    require_error([&] { static_cast<void>(engine.invoke_callback(*printf_address, data_base)); },
                  "memory");
    require_error([&] { static_cast<void>(engine.invoke_callback(*puts_address, 0)); },
                  "memory");

    write_string(engine, data_base, "text");
    capture.error = 32;
    capture.per_call = 0;
    require(engine.invoke_callback(*printf_address, data_base) == UINT32_MAX,
            "printf output failure did not return -1");

    std::vector<std::uint8_t> unterminated(1024U * 1024U, 'a');
    engine.map_external_memory(data_base + 0x400000, unterminated.data(),
                               unterminated.size(), false);
    require_error([&] {
        static_cast<void>(engine.invoke_callback(*puts_address, data_base + 0x400000));
    }, "1 MiB limit");
    engine.unmap_memory(data_base + 0x400000);
}

void test_startup() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{0});
    alchemy::DarwinRuntime runtime(engine);
    const std::array<std::string, 3> arguments{"program", "one", ""};
    const std::array<std::string, 2> environment{"NAME=value", "SECOND=two"};
    runtime.initialize_startup(arguments, environment, "/authored/program");
    const auto& state = engine.state();
    const auto [base, top] = engine.stack_bounds();
    require(state.x[0] == 3, "startup argc wrong");
    require((state.sp & 15U) == 0 && state.sp >= base && state.sp < top,
            "startup stack is not aligned within bounds");
    require(state.sp <= state.x[1] && state.sp <= state.x[2] && state.sp <= state.x[3],
            "startup vectors overlap the active stack");
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        require(read_string(engine, engine.read64(state.x[1] + index * 8)) == arguments[index],
                "startup argv string wrong");
    }
    require(engine.read64(state.x[1] + arguments.size() * 8) == 0,
            "argv is not null terminated");
    for (std::size_t index = 0; index < environment.size(); ++index) {
        require(read_string(engine, engine.read64(state.x[2] + index * 8)) == environment[index],
                "startup environment string wrong");
    }
    require(engine.read64(state.x[2] + environment.size() * 8) == 0,
            "envp is not null terminated");
    require(read_string(engine, engine.read64(state.x[3])) ==
                "executable_path=/authored/program" && engine.read64(state.x[3] + 8) == 0,
            "apple vector wrong");
    const auto preserved = state;
    const std::array<std::string, 1> too_large{std::string(1024U * 1024U, 'a')};
    require_error([&] { runtime.initialize_startup(too_large, {}, "program"); },
                  "does not fit");
    require(engine.state().sp == preserved.sp && engine.state().x == preserved.x,
            "invalid startup changed guest registers");
    const std::array<std::string, 1> embedded_nul{std::string("a\0b", 3)};
    require_error([&] { runtime.initialize_startup(embedded_nul, {}, "program"); }, "NUL");
    require_error([&] { runtime.initialize_startup({}, {}, "program"); }, "argv[0]");

    const std::array<std::string, 1> simple{"program"};
    runtime.initialize_startup(simple, {}, "program");
    require(engine.read64(engine.state().x[2]) == 0, "empty environment has no terminator");
}

transmute::CpuState write_state(std::uint64_t address, std::uint64_t size,
                                unsigned descriptor = 1) {
    transmute::CpuState state{};
    state.x[0] = descriptor;
    state.x[1] = address;
    state.x[2] = size;
    state.x[16] = 4;
    state.nzcv = 0xf0000000U;
    return state;
}

void test_syscalls() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{0});
    Capture capture;
    alchemy::DarwinRuntime runtime(engine, [&capture](auto descriptor, auto bytes) {
        return capture(descriptor, bytes);
    });
    engine.map_memory(data_base, 131072);
    write_string(engine, data_base, std::string_view("exact\r\nbytes\0", 13));
    const transmute::SupervisorCall call{code_base, 0x80};
    auto state = write_state(data_base, 13, 2);
    require(!runtime.supervisor_call(call, state, engine), "write unexpectedly exited");
    require(capture.output == std::string("exact\r\nbytes\0", 13),
            "write transformed output bytes");
    require(capture.descriptors == std::vector<unsigned>{2}, "write stderr descriptor wrong");
    require(state.x[0] == 13 && state.x[1] == 0 && state.nzcv == 0xd0000000U,
            "write success registers or flags wrong");

    capture.output.clear();
    capture.descriptors.clear();
    state = write_state(data_base, 5, 3);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 9 && state.x[1] == 0 && (state.nzcv & carry_flag) != 0,
            "bad descriptor errno or carry wrong");
    require(capture.descriptors.empty(), "bad descriptor reached the output writer");

    for (const auto [address, size] : std::array<std::pair<std::uint64_t, std::uint64_t>, 3>{
            std::pair{std::uint64_t{0}, std::uint64_t{1}},
            std::pair{UINT64_MAX - 1, std::uint64_t{4}},
            std::pair{data_base + 131071, std::uint64_t{2}}}) {
        state = write_state(address, size);
        static_cast<void>(runtime.supervisor_call(call, state, engine));
        require(state.x[0] == 14 && (state.nzcv & carry_flag) != 0,
                "invalid guest range did not return EFAULT");
    }
    require(capture.output.empty(), "invalid guest range emitted output");
    engine.set_memory_access(data_base, false, true, false);
    state = write_state(data_base, 1);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 14, "unreadable guest buffer did not return EFAULT");
    engine.set_memory_access(data_base, true, true, false);
    state = write_state(0, 0);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 0 && (state.nzcv & carry_flag) == 0,
            "zero length write rejected an unused guest pointer");

    state = write_state(data_base, static_cast<std::uint64_t>(INT64_MAX) + 1);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 22, "write count outside ssize_t did not return EINVAL");
    state.x[16] = 1234;
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 78 && (state.nzcv & carry_flag) != 0,
            "unknown BSD syscall did not return ENOSYS");
    state.x[16] = UINT64_MAX - 30;
    require_error([&] { static_cast<void>(runtime.supervisor_call(call, state, engine)); },
                  "unsupported Darwin Mach trap -31");
    require_error([&] {
        static_cast<void>(runtime.supervisor_call({code_base, 0}, state, engine));
    }, "supervisor-call immediate");
    state.x[16] = 1;
    state.x[0] = 37;
    require(runtime.supervisor_call(call, state, engine) == 37U, "exit status wrong");

    capture.per_call = 3;
    state = write_state(data_base, 5);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 3 && (state.nzcv & carry_flag) == 0 && capture.output == "exa",
            "partial output count wrong");
    capture.output.clear();
    capture.error = 32;
    state = write_state(data_base, 5);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 3 && (state.nzcv & carry_flag) == 0,
            "progress followed by output error discarded the partial count");
    capture.per_call = 0;
    state = write_state(data_base, 5);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 32 && state.x[1] == 0 && (state.nzcv & carry_flag) != 0,
            "output error did not return Darwin errno");

    capture = {};
    capture.fail_on_call = 2;
    capture.error = 0;
    state = write_state(data_base, 65540);
    static_cast<void>(runtime.supervisor_call(call, state, engine));
    require(state.x[0] == 65536 && capture.descriptors.size() == 2,
            "multi-chunk partial write lost progress");
}

void test_syscall_execution(bool jit) {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{code_base});
    Capture capture;
    alchemy::DarwinRuntime runtime(engine, [&capture](auto descriptor, auto bytes) {
        return capture(descriptor, bytes);
    });
    engine.map_memory(code_base, 4096);
    engine.map_memory(data_base, 4096);
    write_string(engine, data_base, "once\n");
    // Arithmetic loop becomes translated x64 code. SVC must leave it once.
    const std::array<std::uint32_t, 10> words{
        0xd2801005U, // mov x5, #128
        0xf10004a5U, // subs x5, x5, #1
        0x54ffffe1U, // b.ne to subs
        0xd2800080U, // mov x0, #4
        0xd280008fU, // mov x15, #4
        0xd2800030U, // mov x16, #1
        0xaa0f03f0U, // mov x16, x15
        0xd2800020U, // mov x0, #1
        0xd4001001U, // svc #0x80
        0xd65f03c0U  // ret
    };
    for (std::size_t index = 0; index < words.size(); ++index) {
        engine.write32(code_base + index * 4, words[index]);
    }
    engine.set_memory_access(code_base, true, false, true);
    engine.set_initial_register(1, data_base);
    engine.set_initial_register(2, 5);
    if (jit) engine.set_jit_enabled(true);
    const auto result = engine.run();
    require(result.exit_code == 5 && capture.output == "once\n" &&
            capture.descriptors.size() == 1, "SVC execution wrote output more than once");
    if (jit) require(engine.jit_executed_instructions() != 0,
                     "JIT SVC test did not execute translated instructions");
}

void test_runtime_cleanup() {
    transmute::ReferenceEngine engine(transmute::ExecutionStart{0});
    engine.map_memory(code_base, 4096);
    engine.map_memory(data_base, 4096);
    engine.write32(code_base, 0xd4001001U); // svc #0x80
    engine.write32(code_base + 4, 0xd65f03c0U); // ret
    engine.set_memory_access(code_base, true, false, true);
    write_string(engine, data_base, "retained");
    Capture capture;
    std::uint64_t address = 0;
    std::unique_ptr<transmute::ReferenceEngine> worker;
    {
        alchemy::DarwinRuntime runtime(engine, [&capture](auto descriptor, auto bytes) {
            return capture(descriptor, bytes);
        });
        address = *runtime.resolve(system_library, "_puts");
        worker = engine.fork_thread(code_base, 1);
        worker->set_initial_register(1, data_base);
        worker->set_initial_register(2, 8);
        worker->set_initial_register(16, 4);
    }
    require_error([&] { static_cast<void>(engine.invoke_callback(address, 0)); },
                  "unknown synthetic guest call address");
    require(worker->run().exit_code == 8 && capture.output == "retained",
            "forked supervisor handler borrowed destroyed runtime state");
}

} // namespace

int main() {
    try {
        test_formatting_and_registry();
        test_startup();
        test_syscalls();
        test_syscall_execution(false);
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
        test_syscall_execution(true);
#endif
        test_runtime_cleanup();
        std::cout << "Darwin runtime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Darwin runtime test failure: " << error.what() << '\n';
        return 1;
    }
}
