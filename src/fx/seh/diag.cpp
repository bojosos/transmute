// Diagnostics and process termination for unhandled guest exceptions.

#include "fx/seh/dispatch.hpp"
#include "fx/seh/seh.hpp"
#include "fx/host/process.hpp"
#include "fx/host/text.hpp"

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

namespace transmute::fx::seh {
namespace {

TerminationHook g_termination_hook = nullptr;

std::string narrow(const std::wstring& text) { return host::to_utf8(text); }

} // namespace

std::string describe_guest_address(std::uint64_t address) {
    std::ostringstream out;
    out << "0x" << std::hex << address << std::dec;
    if (const GuestModule* module = find_guest_module(address)) {
        out << " (" << narrow(module->name) << "+0x" << std::hex << (address - module->base) << std::dec << ")";
    }
    return out.str();
}

std::string format_guest_stack(const Arm64Context& context, unsigned max_frames) {
    std::ostringstream out;
    out << "  stack trace:\n";
    Arm64Context walk = context;
    std::size_t limit = boundary_count();
    for (unsigned frame = 0; frame < max_frames; ++frame) {
        // Continue through nested calls (handlers, catch blocks) to the frames
        // that made them.
        for (unsigned crossings = 0; walk.Pc == kReturnToHostPc; ++crossings) {
            if (crossings > 64 || !cross_boundary(walk, limit)) return out.str();
        }
        const std::uint64_t control_pc = walk.Pc;
        if (control_pc == 0) break;
        out << "    #" << frame << ' ' << describe_guest_address(control_pc) << " sp=0x" << std::hex << walk.Sp
            << std::dec << '\n';
        std::uint64_t image_base = 0;
        const bool unwound = (walk.ContextFlags & kCtxUnwoundToCall) != 0;
        const std::uint64_t function_entry =
            lookup_function_entry(unwound && control_pc >= 4 ? control_pc - 4 : control_pc, &image_base);
        if (!function_entry) {
            if (walk.Lr == 0 || walk.Lr == control_pc) break;
            walk.Pc = walk.Lr;
            walk.ContextFlags |= kCtxUnwoundToCall;
            continue;
        }
        Arm64RuntimeFunction entry;
        if (!read_guest_value(function_entry, entry)) break;
        VirtualUnwindResult unwind =
            virtual_unwind(kHandlerTypeNone, image_base, control_pc, entry, walk, nullptr);
        if (unwind.status != UnwindStatus::ok || walk.Pc == control_pc) break;
    }
    return out.str();
}

TerminationHook set_termination_hook(TerminationHook hook) {
    TerminationHook previous = g_termination_hook;
    g_termination_hook = hook;
    return previous;
}

void terminate_process(std::uint32_t exit_code, const std::string& report) {
    if (g_termination_hook) {
        g_termination_hook(exit_code, report);
        // A hook that returns means the test handled it; abort as a backstop.
    }
    std::fwrite(report.data(), 1, report.size(), stderr);
    std::fflush(stderr);
    host::terminate_process(exit_code);
    std::abort();
}

} // namespace transmute::fx::seh
