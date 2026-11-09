#pragma once

// The guest mappings ProcessContext::release_guest unmapped most recently (4096 of them), with the
// provider slot (pc) and guest return address (lr) of the call that released each, so that a fault
// on one can say whose memory it was: a stale pointer into a block a provider already gave back.
// Header-only, process-wide, cheap to record (one lock, a ring).

#include <algorithm>
#include <array>
#include <cstdint>
#include <mutex>
#include <optional>

namespace alchemy::released_ranges {

struct Entry {
    std::uint64_t address{}, size{}, pc{}, lr{}, serial{};
};

namespace detail {
struct Ring {
    std::mutex mutex;
    std::array<Entry, 4096> entries{};
    std::uint64_t count{};
};
inline Ring& ring() {
    static Ring value;
    return value;
}
} // namespace detail

inline void record(std::uint64_t address, std::uint64_t size, std::uint64_t pc, std::uint64_t lr) noexcept {
    auto& ring = detail::ring();
    std::lock_guard lock(ring.mutex);
    ring.entries[ring.count % ring.entries.size()] = {address, size, pc, lr, ring.count + 1};
    ++ring.count;
}

// The latest recorded release whose range holds `address`, and how many releases came after it.
inline std::optional<std::pair<Entry, std::uint64_t>> find(std::uint64_t address) {
    auto& ring = detail::ring();
    std::lock_guard lock(ring.mutex);
    const auto kept = std::min<std::uint64_t>(ring.count, ring.entries.size());
    for (std::uint64_t back = 1; back <= kept; ++back) {
        const auto& entry = ring.entries[(ring.count - back) % ring.entries.size()];
        if (address >= entry.address && address - entry.address < entry.size) return std::pair{entry, back - 1};
    }
    return std::nullopt;
}

} // namespace alchemy::released_ranges
