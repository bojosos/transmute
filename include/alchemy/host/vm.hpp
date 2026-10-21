#pragma once

// Host area "vm" (owner: r1). The rules are in include/alchemy/host/host.hpp. Virtual memory of
// this process: page size, region queries and locking. fx owns reserve, commit and views of guest
// memory behind its own seams (src/fx/host/); this area covers what the macOS layer needs
// directly.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>

namespace alchemy::host::vm {

struct PageInfo {
    std::size_t page_size{};              // the host page (4 KiB on Windows)
    std::size_t allocation_granularity{}; // reservations and views align to this (64 KiB on Windows)
};
[[nodiscard]] PageInfo page_info() noexcept;

enum class Protection : std::uint8_t { none = 0, read = 1, write = 2, execute = 4 };
[[nodiscard]] constexpr Protection operator|(Protection a, Protection b) noexcept {
    return static_cast<Protection>(static_cast<std::uint8_t>(a) | static_cast<std::uint8_t>(b));
}
[[nodiscard]] constexpr bool has(Protection set, Protection bit) noexcept {
    return (static_cast<std::uint8_t>(set) & static_cast<std::uint8_t>(bit)) != 0;
}

// The region containing `address` (VirtualQuery, /proc/self/maps).
struct Region {
    std::uintptr_t base{};
    std::size_t size{};
    Protection protection{};
    bool committed{}; // backed by memory, not just reserved or free
};
[[nodiscard]] Result<Region> query(std::uintptr_t address);

// mlock / munlock. lock fails with Error::would_block past the host's limit
// on locked memory; unlocking pages that are not locked succeeds.
[[nodiscard]] Status lock(std::uintptr_t address, std::size_t size);
[[nodiscard]] Status unlock(std::uintptr_t address, std::size_t size);

} // namespace alchemy::host::vm
