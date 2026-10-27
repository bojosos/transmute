// Null host backend, area "debug": no crash hook, no stack, no modules
// (include/alchemy/host/host.hpp).
#include "alchemy/host/debug.hpp"

#include <cstdio>

namespace alchemy::host::debug {

void on_fatal_fault(void (*)(const Fault&)) {}
std::size_t capture_stack(std::span<std::uintptr_t>, unsigned) noexcept { return 0; }
std::vector<ThreadStack> other_thread_stacks(std::size_t) { return {}; }
std::string describe_address(std::uintptr_t address) {
    char out[48];
    std::snprintf(out, sizeof(out), "0x%llx (no module)", static_cast<unsigned long long>(address));
    return out;
}

} // namespace alchemy::host::debug
