#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace transmute {

// Metadata only. The caller holds an execution lease while reading a replay
// and exclusive execution ownership while appending backing/view changes.
class NativeTopologyHistory final {
public:
    static constexpr std::size_t capacity = 512;
    struct Stamp {
        std::uint64_t sequence{};
        std::uint64_t era{};
        bool operator==(const Stamp&) const = default;
    };
    struct Change {
        std::uint64_t sequence{};
        std::uint64_t base{};
        std::uint64_t size{}; // Zero means no borrowed backing changed.
    };
    struct Replay {
        const NativeTopologyHistory* owner{};
        std::uint64_t first{};
        std::size_t count{};
        bool complete{};
        [[nodiscard]] const Change& operator[](std::size_t index) const noexcept {
            assert(complete && index < count);
            return owner->changes_[(first + index) & (capacity - 1)];
        }
    };

    // Seedable for wrap fixtures; an engine's history starts at zero.
    explicit NativeTopologyHistory(std::uint64_t sequence = 0,
                                   std::uint64_t era = 0) noexcept
        : stamp_{sequence, era} {}

    [[nodiscard]] Stamp stamp() const noexcept { return stamp_; }
    [[nodiscard]] bool usable() const noexcept { return usable_; }

    void append(std::uint64_t base = 0, std::uint64_t size = 0) noexcept {
        if (++stamp_.sequence == 0) {
            changes_ = {};
            if (stamp_.era == std::numeric_limits<std::uint64_t>::max()) {
                // Never recycle an era after exhaustion. Every subsequent
                // refresh must take the conservative full-invalidation path.
                usable_ = false;
            } else {
                ++stamp_.era;
            }
        }
        changes_[stamp_.sequence & (capacity - 1)] = {stamp_.sequence, base, size};
    }

    [[nodiscard]] Replay since(Stamp previous) const noexcept {
        if (!usable_ || previous.era != stamp_.era || previous.sequence > stamp_.sequence ||
            stamp_.sequence - previous.sequence > capacity)
            return {this, 0, 0, false};
        const auto count = static_cast<std::size_t>(stamp_.sequence - previous.sequence);
        // Do not trust a sequence distance alone: every retained slot must
        // still identify exactly the requested change before replay starts.
        for (std::size_t index = 0; index < count; ++index) {
            const auto sequence = previous.sequence + index + 1;
            if (changes_[sequence & (capacity - 1)].sequence != sequence)
                return {this, 0, 0, false};
        }
        return {this, count == 0 ? 0 : previous.sequence + 1, count, true};
    }

    [[nodiscard]] static bool overlaps(std::uint64_t first, std::uint64_t size,
                                       const Change& change) noexcept {
        if (size == 0 || change.size == 0) return false;
        // Ordered subtraction avoids an overflowing end-address addition.
        return first <= change.base ? change.base - first < size
                                    : first - change.base < change.size;
    }

private:
    std::array<Change, capacity> changes_{};
    Stamp stamp_;
    bool usable_{true};
};

} // namespace transmute
