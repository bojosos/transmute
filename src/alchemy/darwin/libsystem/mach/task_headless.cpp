// alchemy.exe's mach_port_deallocate, task_info, thread_policy_set and
// thread_policy_get of the calling task (mach/task_shell.hpp). alchemy-fx
// builds mach/task_fx.cpp instead: mach/mach_exports.cpp owns these names there.
#include "alchemy/darwin/libsystem/mach/task_shell.hpp"
#include "alchemy/darwin/libsystem/misc/process_usage.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>

namespace alchemy::libsystem {
namespace {

// <mach/kern_return.h>
constexpr std::uint64_t kern_success = 0, kern_invalid_argument = 4, kern_invalid_name = 15, kern_not_supported = 46;
constexpr std::uint32_t task_port = headless_task_port, efault = 14;
constexpr std::uint64_t page_size = 16384;
struct TaskError { std::uint32_t value; };

bool accessible(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size, bool writable) {
    if (!address || size > UINT64_MAX - address) return false;
    while (size) {
        const auto region = memory.query_region(address);
        if (!region || address < region->base || address - region->base >= region->size ||
            !region->readable || (writable && !region->writable)) return false;
        const auto count = std::min(size, region->size - (address - region->base));
        address += count; size -= count;
    }
    return true;
}
void validate(const transmute::GuestMemory& memory, std::uint64_t address, std::uint64_t size, bool writable) {
    if (!accessible(memory, address, size, writable)) throw TaskError{efault};
}

using Operation = std::function<std::uint64_t(transmute::CpuState&, transmute::GuestMemory&)>;
// Mach results: a TaskError becomes the returned code.
void install_code(ProcessContext& context, std::string name, Operation operation) {
    static_cast<void>(context.install_function(std::move(name), [operation = std::move(operation)](auto& s, auto& m) {
        try { s.x[0] = operation(s, m); }
        catch (const TaskError& error) { s.x[0] = error.value; }
    }));
}

} // namespace

void install_mach_task_shell(ProcessContext& context) {
    install_code(context, "_mach_port_deallocate", [](auto& s, auto&) {
        const auto name = static_cast<std::uint32_t>(s.x[1]);
        if (static_cast<std::uint32_t>(s.x[0]) != task_port) return kern_invalid_argument;
        return name == 0 || name == UINT32_MAX ? kern_invalid_name : kern_success;
    });
    install_code(context, "_task_info", [](auto& s, auto& m) {
        if (static_cast<std::uint32_t>(s.x[0]) != task_port || !accessible(m, s.x[3], 4, true)) return kern_invalid_argument;
        const auto flavor = static_cast<std::uint32_t>(s.x[1]), capacity = m.read32(s.x[3]);
        const auto output = s.x[2];
        const auto usage = process_usage();
        const auto seconds = [&](std::uint64_t address, std::uint64_t nanos) {
            m.write32(address, static_cast<std::uint32_t>(nanos / 1'000'000'000)); m.write32(address + 4, static_cast<std::uint32_t>(nanos / 1000 % 1'000'000));
        };
        std::uint32_t count;
        if (flavor == 20) { // MACH_TASK_BASIC_INFO
            count = 12; if (capacity < count || !accessible(m, output, count * 4ULL, true)) return kern_invalid_argument;
            m.write64(output, usage.virtual_size); m.write64(output + 8, usage.resident); m.write64(output + 16, usage.resident_peak);
            seconds(output + 24, usage.user); seconds(output + 32, usage.system); m.write32(output + 40, 1); m.write32(output + 44, 0);
        } else if (flavor == 5) { // TASK_BASIC_INFO_64
            count = 10; if (capacity < count || !accessible(m, output, count * 4ULL, true)) return kern_invalid_argument;
            m.write32(output, 0); m.write64(output + 4, usage.virtual_size); m.write64(output + 12, usage.resident);
            seconds(output + 20, usage.user); seconds(output + 28, usage.system); m.write32(output + 36, 1);
        } else if (flavor == 3) { // TASK_THREAD_TIMES_INFO
            count = 4; if (capacity < count || !accessible(m, output, count * 4ULL, true)) return kern_invalid_argument;
            seconds(output, usage.user); seconds(output + 8, usage.system);
        } else if (flavor == 22) { // TASK_VM_INFO, revisions 0 through 2
            if (capacity < 36) return kern_invalid_argument;
            count = std::min<std::uint32_t>(capacity, 42);
            if (!accessible(m, output, count * 4ULL, true)) return kern_invalid_argument;
            m.fill_bytes(output, 0, count * 4ULL);
            m.write64(output, usage.virtual_size); m.write32(output + 12, static_cast<std::uint32_t>(page_size));
            m.write64(output + 16, usage.resident); m.write64(output + 24, usage.resident_peak);
            m.write64(output + 48, usage.footprint); m.write64(output + 56, usage.footprint);
            if (count >= 38) m.write64(output + 144, usage.footprint);
            if (count >= 42) { m.write64(output + 152, 0x100000000ULL); m.write64(output + 160, 0x7ffffffff000ULL); }
        } else return kern_invalid_argument;
        m.write32(s.x[3], count);
        return kern_success;
    });
    install_code(context, "_thread_policy_set", [](auto& s, auto& m) {
        const auto flavor = static_cast<std::uint32_t>(s.x[1]);
        if (flavor == 1 || flavor == 3) { // extended (timeshare) and precedence: host scheduling hints
            if (static_cast<std::uint32_t>(s.x[3]) < 1 || !accessible(m, s.x[2], 4, false)) return kern_invalid_argument;
            return kern_success;
        }
        return flavor == 2 || flavor == 4 ? kern_not_supported : kern_invalid_argument; // time constraint, affinity
    });
    install_code(context, "_thread_policy_get", [](auto& s, auto& m) {
        const auto flavor = static_cast<std::uint32_t>(s.x[1]);
        if (flavor == 2 || flavor == 4) return kern_not_supported;
        if ((flavor != 1 && flavor != 3) || !accessible(m, s.x[3], 4, true) || m.read32(s.x[3]) < 1 ||
            !accessible(m, s.x[2], 4, true)) return kern_invalid_argument;
        m.write32(s.x[2], flavor == 1 ? 1 : 0); m.write32(s.x[3], 1);
        if (s.x[4]) { validate(m, s.x[4], 4, true); m.write32(s.x[4], 1); }
        return kern_success;
    });
}

} // namespace alchemy::libsystem
