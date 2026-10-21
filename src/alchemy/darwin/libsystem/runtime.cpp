#include "alchemy/darwin_runtime.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/process.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <utility>


namespace alchemy {
namespace {

constexpr std::string_view system_library = "/usr/lib/libSystem.B.dylib";
constexpr std::string_view cxx_library = "/usr/lib/libc++.1.dylib";
constexpr std::uint32_t darwin_eio = 5;
constexpr std::uint32_t darwin_ebadf = 9;
constexpr std::uint32_t darwin_enomem = 12;
constexpr std::uint32_t darwin_efault = 14;
constexpr std::uint32_t darwin_einval = 22;
constexpr std::uint32_t darwin_epipe = 32;
constexpr std::uint32_t darwin_enosys = 78;
constexpr std::uint32_t carry_flag = 1U << 29;
constexpr std::size_t string_limit = 1024U * 1024U;
constexpr std::size_t transfer_size = 64U * 1024U;

std::uint64_t checked_add(std::uint64_t address, std::uint64_t size) {
    if (size > UINT64_MAX - address) {
        throw std::runtime_error("Darwin guest address overflow");
    }
    return address + size;
}

std::string guest_string(const transmute::GuestMemory& memory,
                         std::uint64_t address) {
    std::string result;
    for (std::size_t index = 0; index < string_limit; ++index) {
        const auto byte = memory.read8(checked_add(address, index));
        if (byte == 0) return result;
        result.push_back(static_cast<char>(byte));
    }
    throw std::runtime_error("Darwin guest string exceeds the 1 MiB limit");
}

void append_text(std::string& output, std::string_view text) {
    if (text.size() > string_limit - output.size()) {
        throw std::runtime_error("Darwin formatted output exceeds the 1 MiB limit");
    }
    output.append(text);
}

std::uint32_t output_error(host::Error error) {
    switch (error) {
    case host::Error::bad_handle: return darwin_ebadf;
    case host::Error::no_memory: return darwin_enomem;
    case host::Error::invalid_argument: return darwin_einval;
    case host::Error::broken_pipe: return darwin_epipe;
    default: return darwin_eio;
    }
}

OutputResult default_output(unsigned descriptor,
                            std::span<const std::uint8_t> bytes) {
    if (descriptor != 1 && descriptor != 2) return {0, darwin_ebadf};
    if (bytes.empty()) return {};
    const auto handle = host::process::standard_handle(descriptor == 1 ? host::process::Standard::output
                                                                        : host::process::Standard::error);
    if (!handle) return {0, darwin_ebadf};
    const auto written = host::files::write(handle, std::as_bytes(bytes));
    if (!written) return {0, output_error(written.error())};
    return {*written, 0};
}

OutputResult checked_output(const OutputWriter& writer, unsigned descriptor,
                            std::span<const std::uint8_t> bytes) {
    const auto result = writer(descriptor, bytes);
    if (result.bytes_written > bytes.size()) {
        throw std::runtime_error("Darwin output writer returned an invalid byte count");
    }
    return result;
}

bool print_output(const OutputWriter& writer, std::string_view output) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(output.data());
    std::size_t position = 0;
    while (position < output.size()) {
        const auto result = checked_output(writer, 1,
            std::span(data + position, output.size() - position));
        position += result.bytes_written;
        if (result.error != 0 || result.bytes_written == 0) return false;
    }
    return true;
}

void print_impl(const OutputWriter& writer, transmute::CpuState& state,
                transmute::GuestMemory& memory, bool puts) {
    const auto format = guest_string(memory, state.x[0]);
    std::string output;
    if (puts) {
        append_text(output, format);
        append_text(output, "\n");
    } else {
        auto argument = state.sp;
        for (std::size_t position = 0; position < format.size(); ++position) {
            const auto character = format[position];
            if (character != '%') {
                append_text(output, std::string_view(&format[position], 1));
                continue;
            }
            if (++position == format.size()) {
                throw std::runtime_error("Darwin printf has an incomplete conversion");
            }
            const auto conversion = format[position];
            if (conversion == '%') {
                append_text(output, "%");
            } else if (conversion == 's') {
                append_text(output, guest_string(memory, memory.read64(argument)));
                argument = checked_add(argument, 8);
            } else if (conversion == 'd') {
                const auto value = std::bit_cast<std::int32_t>(memory.read32(argument));
                std::array<char, 16> buffer{};
                const auto converted = std::to_chars(buffer.data(),
                                                     buffer.data() + buffer.size(), value);
                append_text(output, std::string_view(buffer.data(),
                    static_cast<std::size_t>(converted.ptr - buffer.data())));
                argument = checked_add(argument, 8);
            } else {
                throw std::runtime_error(std::string("unsupported Darwin printf conversion %") +
                                         conversion);
            }
        }
    }
    state.x[0] = print_output(writer, output) ? output.size() : UINT32_MAX;
}

void syscall_error(transmute::CpuState& state, std::uint32_t error) {
    state.x[0] = error;
    state.x[1] = 0;
    state.nzcv |= carry_flag;
}

void syscall_success(transmute::CpuState& state, std::uint64_t count) {
    state.x[0] = count;
    state.x[1] = 0;
    state.nzcv &= ~carry_flag;
}

bool readable_range(const transmute::GuestMemory& memory, std::uint64_t address,
                    std::uint64_t size) {
    if (size > UINT64_MAX - address) return false;
    while (size != 0) {
        const auto region = memory.query_region(address);
        if (!region || !region->readable || address < region->base ||
            address - region->base >= region->size) return false;
        const auto count = std::min(size, region->size - (address - region->base));
        address += count;
        size -= count;
    }
    return true;
}

std::optional<std::uint32_t> supervisor_impl(
    const OutputWriter& writer, const transmute::SupervisorCall& call,
    transmute::CpuState& state, transmute::GuestMemory& memory) {
    if (call.immediate != 0x80) {
        throw std::runtime_error("unsupported Darwin supervisor-call immediate " +
                                 std::to_string(call.immediate));
    }
    const auto number = state.x[16];
    if ((number >> 63) != 0) {
        throw std::runtime_error("unsupported Darwin Mach trap " +
                                 std::to_string(std::bit_cast<std::int64_t>(number)));
    }
    if (number == 1) return static_cast<std::uint32_t>(state.x[0]);
    if (number != 4) {
        syscall_error(state, darwin_enosys);
        return std::nullopt;
    }
    const auto descriptor = static_cast<std::uint32_t>(state.x[0]);
    if (descriptor != 1 && descriptor != 2) {
        syscall_error(state, darwin_ebadf);
        return std::nullopt;
    }
    const auto address = state.x[1];
    const auto size = state.x[2];
    if (size > static_cast<std::uint64_t>(INT64_MAX)) {
        syscall_error(state, darwin_einval);
        return std::nullopt;
    }
    if (!readable_range(memory, address, size)) {
        syscall_error(state, darwin_efault);
        return std::nullopt;
    }
    std::array<std::uint8_t, transfer_size> buffer{};
    std::uint64_t total = 0;
    while (total < size) {
        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
            size - total, buffer.size()));
        try {
            for (std::size_t index = 0; index < count; ++index) {
                buffer[index] = memory.read8(address + total + index);
            }
        } catch (const std::runtime_error&) {
            if (total != 0) syscall_success(state, total);
            else syscall_error(state, darwin_efault);
            return std::nullopt;
        }
        const auto result = checked_output(writer, descriptor,
                                           std::span(buffer.data(), count));
        total += result.bytes_written;
        if (result.error != 0 || result.bytes_written != count) {
            if (total != 0 || result.error == 0) syscall_success(state, total);
            else syscall_error(state, result.error);
            return std::nullopt;
        }
    }
    syscall_success(state, total);
    return std::nullopt;
}

} // namespace

DarwinRuntime::DarwinRuntime(transmute::ReferenceEngine& engine, OutputWriter writer)
    : engine_(engine), writer_(std::make_shared<OutputWriter>(
          writer ? std::move(writer) : OutputWriter(default_output))) {
    // Registry copies and forked supervisor handlers retain the output state.
    // They never borrow the DarwinRuntime object's lifetime.
    printf_address_ = engine_.register_guest_call(
        [output = writer_](transmute::CpuState& state, transmute::GuestMemory& memory) {
            print_impl(*output, state, memory, false);
        });
    try {
        puts_address_ = engine_.register_guest_call(
            [output = writer_](transmute::CpuState& state, transmute::GuestMemory& memory) {
                print_impl(*output, state, memory, true);
            });
        engine_.set_supervisor_call_handler(
            [output = writer_](const transmute::SupervisorCall& call,
                              transmute::CpuState& state, transmute::GuestMemory& memory) {
                return supervisor_impl(*output, call, state, memory);
            });
    } catch (...) {
        if (puts_address_ != 0) engine_.unregister_guest_call(puts_address_);
        engine_.unregister_guest_call(printf_address_);
        throw;
    }
}

DarwinRuntime::~DarwinRuntime() {
    engine_.set_supervisor_call_handler({});
    engine_.unregister_guest_call(printf_address_);
    engine_.unregister_guest_call(puts_address_);
}

bool DarwinRuntime::is_system_library(std::string_view install_name) {
    return install_name == system_library || install_name == cxx_library;
}

std::optional<std::uint64_t> DarwinRuntime::resolve(
    std::string_view install_name, std::string_view symbol) const {
    if (install_name != system_library) return std::nullopt;
    if (symbol == "_printf") return printf_address_;
    if (symbol == "_puts") return puts_address_;
    return std::nullopt;
}

void DarwinRuntime::initialize_startup(std::span<const std::string> arguments,
                                       std::span<const std::string> environment,
                                       std::string_view executable_path) {
    if (arguments.empty() || arguments.size() > static_cast<std::size_t>(INT32_MAX)) {
        throw std::runtime_error("Darwin startup requires argv[0] and a valid argument count");
    }
    if (executable_path.find('\0') != std::string_view::npos) {
        throw std::runtime_error("Darwin executable path contains a NUL byte");
    }
    const std::string apple = "executable_path=" + std::string(executable_path);
    const auto [base, top] = engine_.stack_bounds();
    std::uint64_t needed = 16 + 256;
    const auto account_string = [&needed](std::string_view value) {
        if (value.find('\0') != std::string_view::npos) {
            throw std::runtime_error("Darwin startup string contains a NUL byte");
        }
        needed = checked_add(needed, checked_add(value.size(), 1));
    };
    for (const auto& argument : arguments) account_string(argument);
    for (const auto& variable : environment) account_string(variable);
    account_string(apple);
    // Both pointer vectors have trailing nulls, as does the apple vector.
    if (environment.size() > (UINT64_MAX / 8) - arguments.size() - 4) {
        throw std::runtime_error("Darwin startup pointer count overflows");
    }
    needed = checked_add(needed, (arguments.size() + environment.size() + 4) * 8ULL);
    if (needed > top - base) {
        throw std::runtime_error("Darwin startup data does not fit in the guest stack");
    }
    auto cursor = top;
    transmute::GuestMemory& memory = engine_;
    const auto push_string = [&cursor, &memory](std::string_view value) {
        cursor -= value.size() + 1;
        const auto address = cursor;
        for (std::size_t index = 0; index < value.size(); ++index) {
            memory.write8(address + index, static_cast<std::uint8_t>(value[index]));
        }
        memory.write8(address + value.size(), 0);
        return address;
    };
    std::vector<std::uint64_t> argv;
    std::vector<std::uint64_t> envp;
    argv.reserve(arguments.size());
    envp.reserve(environment.size());
    for (const auto& argument : arguments) argv.push_back(push_string(argument));
    for (const auto& variable : environment) envp.push_back(push_string(variable));
    const std::array apple_entries{push_string(apple)};
    cursor &= ~7ULL;
    const auto push_pointers = [&cursor, &memory](std::span<const std::uint64_t> values) {
        cursor -= (values.size() + 1) * 8ULL;
        for (std::size_t index = 0; index < values.size(); ++index) {
            memory.write64(cursor + index * 8, values[index]);
        }
        memory.write64(cursor + values.size() * 8, 0);
        return cursor;
    };
    const auto apple_pointer = push_pointers(apple_entries);
    const auto envp_pointer = push_pointers(envp);
    const auto argv_pointer = push_pointers(argv);
    engine_.set_initial_register(0, arguments.size());
    engine_.set_initial_register(1, argv_pointer);
    engine_.set_initial_register(2, envp_pointer);
    engine_.set_initial_register(3, apple_pointer);
    engine_.set_initial_stack_pointer(cursor & ~15ULL);
}

std::optional<std::uint32_t> DarwinRuntime::supervisor_call(
    const transmute::SupervisorCall& call, transmute::CpuState& state,
    transmute::GuestMemory& memory) {
    return supervisor_impl(*writer_, call, state, memory);
}

void DarwinRuntime::print(transmute::CpuState& state,
                           transmute::GuestMemory& memory, bool puts) {
    print_impl(*writer_, state, memory, puts);
}

} // namespace alchemy
