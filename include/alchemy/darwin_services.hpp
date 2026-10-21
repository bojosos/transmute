#pragma once

#include "alchemy/process_context.hpp"

#include <memory>

namespace alchemy {

inline constexpr std::uint64_t darwin_arm64_page_size = 16384;

// Independently authored Darwin services. Host objects never enter guest memory.
class DarwinServices {
public:
    explicit DarwinServices(ProcessContext& context);
    ~DarwinServices();
    DarwinServices(const DarwinServices&) = delete;
    DarwinServices& operator=(const DarwinServices&) = delete;
    void install();
    [[nodiscard]] bool handles_syscall(std::uint64_t number) const noexcept;
    [[nodiscard]] std::optional<std::uint32_t> supervisor_call(
        const transmute::SupervisorCall& call, transmute::CpuState& state,
        transmute::GuestMemory& memory);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace alchemy
