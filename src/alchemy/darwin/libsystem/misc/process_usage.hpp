#pragma once

// CPU times and memory of the calling process, in Darwin units
// (nanoseconds, bytes): task_info and proc_pid_rusage (misc/libsystem_extra.cpp,
// mach/task_headless.cpp), over host::process::usage.
#include "alchemy/host/process.hpp"

#include <cstdint>

namespace alchemy::libsystem {

struct ProcessUsage { std::uint64_t user{}, system{}, resident{}, resident_peak{}, footprint{}, virtual_size{}; };

inline ProcessUsage process_usage() {
    ProcessUsage value;
    if (const auto usage = host::process::usage()) {
        value.user = usage->user_ns; value.system = usage->system_ns;
        value.resident = usage->resident; value.resident_peak = usage->resident_peak;
        value.footprint = usage->private_bytes;
        value.virtual_size = usage->virtual_size;
    }
    return value;
}

} // namespace alchemy::libsystem
