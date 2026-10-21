#pragma once

#include "transmute/a64.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy {

struct OutputResult {
    std::size_t bytes_written{};
    std::uint32_t error{}; // Darwin errno; zero on success.
};

using OutputWriter = std::function<OutputResult(unsigned descriptor,
                                               std::span<const std::uint8_t> bytes)>;

class DarwinRuntime {
public:
    explicit DarwinRuntime(transmute::ReferenceEngine& engine, OutputWriter writer = {});
    ~DarwinRuntime();
    DarwinRuntime(const DarwinRuntime&) = delete;
    DarwinRuntime& operator=(const DarwinRuntime&) = delete;
    [[nodiscard]] static bool is_system_library(std::string_view install_name);
    [[nodiscard]] OutputWriter output_writer() const { return *writer_; }
    [[nodiscard]] std::optional<std::uint64_t> resolve(
        std::string_view install_name, std::string_view symbol) const;
    // arguments includes argv[0]. All strings and pointer arrays reside in guest memory.
    void initialize_startup(std::span<const std::string> arguments,
                            std::span<const std::string> environment,
                            std::string_view executable_path);
    [[nodiscard]] std::optional<std::uint32_t> supervisor_call(
        const transmute::SupervisorCall& call, transmute::CpuState& state,
        transmute::GuestMemory& memory);

private:
    void print(transmute::CpuState& state, transmute::GuestMemory& memory, bool puts);
    transmute::ReferenceEngine& engine_;
    std::shared_ptr<OutputWriter> writer_;
    std::uint64_t printf_address_{};
    std::uint64_t puts_address_{};
};

} // namespace alchemy
