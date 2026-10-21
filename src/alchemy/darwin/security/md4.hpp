#pragma once

// MD4 message digest (RFC 1320), project-authored, for CommonCrypto's
// CC_MD4_* functions. The running state lives in the caller's CC_MD4_CTX
// (92 bytes: A..D, the 64-bit bit count, a 64-byte block and its fill).

#include "transmute/a64.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

namespace alchemy::export_fill {

class Md4 {
public:
    static constexpr std::uint64_t context_size = 92;

    void update(std::span<const std::uint8_t> bytes) {
        bits_ += static_cast<std::uint64_t>(bytes.size()) * 8;
        for (const auto byte : bytes) {
            block_[fill_++] = byte;
            if (fill_ == 64) { transform(); fill_ = 0; }
        }
    }
    std::array<std::uint8_t, 16> finish() {
        const auto bits = bits_;
        const std::uint8_t pad = 0x80;
        update(std::span(&pad, 1));
        const std::uint8_t zero = 0;
        while (fill_ != 56) update(std::span(&zero, 1));
        for (int i = 0; i < 8; ++i) { block_[fill_++] = static_cast<std::uint8_t>(bits >> (8 * i)); }
        transform();
        fill_ = 0;
        std::array<std::uint8_t, 16> digest{};
        for (int word = 0; word < 4; ++word)
            for (int i = 0; i < 4; ++i) digest[static_cast<std::size_t>(word * 4 + i)] = static_cast<std::uint8_t>(state_[static_cast<std::size_t>(word)] >> (8 * i));
        return digest;
    }

    void store(transmute::GuestMemory& memory, std::uint64_t address) const {
        std::array<std::uint8_t, context_size> bytes{};
        std::memcpy(bytes.data(), state_.data(), 16);
        std::memcpy(bytes.data() + 16, &bits_, 8);
        std::memcpy(bytes.data() + 24, block_.data(), 64);
        const auto fill = static_cast<std::uint32_t>(fill_);
        std::memcpy(bytes.data() + 88, &fill, 4);
        memory.write_bytes(address, bytes);
    }
    static Md4 load(const transmute::GuestMemory& memory, std::uint64_t address) {
        std::array<std::uint8_t, context_size> bytes{};
        memory.read_bytes(address, bytes);
        Md4 value;
        std::memcpy(value.state_.data(), bytes.data(), 16);
        std::memcpy(&value.bits_, bytes.data() + 16, 8);
        std::memcpy(value.block_.data(), bytes.data() + 24, 64);
        std::uint32_t fill{};
        std::memcpy(&fill, bytes.data() + 88, 4);
        if (fill >= 64) throw std::runtime_error("Alchemy CC_MD4: corrupt context");
        value.fill_ = fill;
        return value;
    }

private:
    static std::uint32_t rotate(std::uint32_t value, int count) { return (value << count) | (value >> (32 - count)); }
    void transform() {
        std::array<std::uint32_t, 16> x{};
        for (std::size_t i = 0; i < 16; ++i)
            x[i] = static_cast<std::uint32_t>(block_[i * 4]) | static_cast<std::uint32_t>(block_[i * 4 + 1]) << 8 |
                   static_cast<std::uint32_t>(block_[i * 4 + 2]) << 16 | static_cast<std::uint32_t>(block_[i * 4 + 3]) << 24;
        auto a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        const auto f = [](std::uint32_t x1, std::uint32_t y, std::uint32_t z) { return (x1 & y) | (~x1 & z); };
        const auto g = [](std::uint32_t x1, std::uint32_t y, std::uint32_t z) { return (x1 & y) | (x1 & z) | (y & z); };
        const auto h = [](std::uint32_t x1, std::uint32_t y, std::uint32_t z) { return x1 ^ y ^ z; };
        // Round 1.
        constexpr int s1[] = {3, 7, 11, 19};
        for (int i = 0; i < 16; ++i) {
            const auto t = a + f(b, c, d) + x[static_cast<std::size_t>(i)];
            a = d; d = c; c = b; b = rotate(t, s1[i % 4]);
        }
        // Round 2.
        constexpr int order2[] = {0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15};
        constexpr int s2[] = {3, 5, 9, 13};
        for (int i = 0; i < 16; ++i) {
            const auto t = a + g(b, c, d) + x[static_cast<std::size_t>(order2[i])] + 0x5a827999U;
            a = d; d = c; c = b; b = rotate(t, s2[i % 4]);
        }
        // Round 3.
        constexpr int order3[] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
        constexpr int s3[] = {3, 9, 11, 15};
        for (int i = 0; i < 16; ++i) {
            const auto t = a + h(b, c, d) + x[static_cast<std::size_t>(order3[i])] + 0x6ed9eba1U;
            a = d; d = c; c = b; b = rotate(t, s3[i % 4]);
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    }

    std::array<std::uint32_t, 4> state_{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};
    std::uint64_t bits_{};
    std::array<std::uint8_t, 64> block_{};
    std::size_t fill_{};
};

} // namespace alchemy::export_fill
