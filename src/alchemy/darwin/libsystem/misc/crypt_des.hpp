#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// DES (FIPS 46-3, from its published tables) and the crypt(3) password hashes built on it: the traditional form
// (a 2-character salt, 25 encryptions) and the extended BSDi form ("_" + 4-character count + 4-character salt).
// Written bit by bit for clarity; crypt's iteration counts keep it fast enough.
namespace alchemy::libsystem::des {

inline constexpr std::array<std::uint8_t, 64> initial_permutation{
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3, 61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};
inline constexpr std::array<std::uint8_t, 64> final_permutation{
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27, 34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25};
inline constexpr std::array<std::uint8_t, 48> expansion{
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};
inline constexpr std::array<std::uint8_t, 32> permutation{
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10, 2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25};
inline constexpr std::array<std::uint8_t, 56> permuted_choice1{
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4};
inline constexpr std::array<std::uint8_t, 48> permuted_choice2{
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};
inline constexpr std::array<std::uint8_t, 16> rotations{1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};
inline constexpr std::array<std::array<std::uint8_t, 64>, 8> sboxes{{
    {14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13},
    {15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9},
    {10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12},
    {7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14},
    {2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3},
    {12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13},
    {4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12},
    {13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11}}};

// Bit `n` (1 = the most significant) of a 64-bit value.
inline std::uint64_t bit(std::uint64_t value, unsigned n, unsigned width) { return (value >> (width - n)) & 1; }
template <std::size_t N>
std::uint64_t permute(std::uint64_t value, unsigned width, const std::array<std::uint8_t, N>& table) {
    std::uint64_t result{};
    for (const auto source : table) result = (result << 1) | bit(value, source, width);
    return result;
}

// The 16 round keys (48 bits each) of a 64-bit key (its parity bits ignored).
inline std::array<std::uint64_t, 16> schedule(std::uint64_t key) {
    const auto chosen = permute(key, 64, permuted_choice1);
    auto c = static_cast<std::uint32_t>(chosen >> 28) & 0x0fffffff, d = static_cast<std::uint32_t>(chosen) & 0x0fffffff;
    std::array<std::uint64_t, 16> keys{};
    for (std::size_t round = 0; round < 16; ++round) {
        for (unsigned k = 0; k < rotations[round]; ++k) {
            c = ((c << 1) | (c >> 27)) & 0x0fffffff;
            d = ((d << 1) | (d >> 27)) & 0x0fffffff;
        }
        keys[round] = permute((static_cast<std::uint64_t>(c) << 28) | d, 56, permuted_choice2);
    }
    return keys;
}

// One block with the round keys in order (encryption) or reversed (decryption). `salt` (crypt's, 24 bits) swaps
// bits i and i + 24 of each expanded half block wherever its bit 23 - i is set (bit 0 of the salt is the first
// expanded bit).
inline std::uint64_t cipher(std::uint64_t block, const std::array<std::uint64_t, 16>& keys, bool decrypt, std::uint32_t salt = 0) {
    std::uint64_t salt_mask{};
    for (unsigned i = 0; i < 24; ++i) if (salt & (1U << i)) salt_mask |= 1ULL << (23 - i);
    const auto permuted = permute(block, 64, initial_permutation);
    auto left = static_cast<std::uint32_t>(permuted >> 32), right = static_cast<std::uint32_t>(permuted);
    for (std::size_t round = 0; round < 16; ++round) {
        auto expanded = permute(right, 32, expansion);
        if (salt_mask) {
            const auto high = expanded >> 24, low = expanded & 0xffffff;
            const auto swap = (high ^ low) & salt_mask;
            expanded = ((high ^ swap) << 24) | (low ^ swap);
        }
        expanded ^= keys[decrypt ? 15 - round : round];
        std::uint32_t substituted{};
        for (unsigned box = 0; box < 8; ++box) {
            const auto six = static_cast<unsigned>((expanded >> (42 - 6 * box)) & 0x3f);
            const auto row = ((six & 0x20) >> 4) | (six & 1), column = (six >> 1) & 0xf;
            substituted = (substituted << 4) | sboxes[box][row * 16 + column];
        }
        const auto f = static_cast<std::uint32_t>(permute(substituted, 32, permutation));
        const auto next = left ^ f;
        left = right;
        right = next;
    }
    return permute((static_cast<std::uint64_t>(right) << 32) | left, 64, final_permutation);
}

inline constexpr std::string_view crypt_alphabet = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
// crypt's character value (an invalid character counts as 0).
inline std::uint32_t crypt_value(char ch) {
    if (ch > 'z') return 0;
    if (ch >= 'a') return static_cast<std::uint32_t>(ch - 'a' + 38);
    if (ch > 'Z') return 0;
    if (ch >= 'A') return static_cast<std::uint32_t>(ch - 'A' + 12);
    if (ch > '9') return 0;
    if (ch >= '.') return static_cast<std::uint32_t>(ch - '.');
    return 0;
}

// crypt(3) with a DES setting: "ss" (traditional) or "_ccccssss" (extended); nullopt for a short extended setting.
inline std::optional<std::string> crypt(std::string_view key, std::string_view setting) {
    // The first 8 characters, each shifted left one bit (the low bit is DES's parity bit).
    std::uint64_t key_block{};
    std::size_t used = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        key_block = (key_block << 8) | static_cast<std::uint8_t>((used < key.size() ? key[used] : 0) << 1);
        if (used < key.size()) ++used;
    }
    std::uint32_t salt{}, count{};
    std::string output;
    if (!setting.empty() && setting[0] == '_') {
        if (setting.size() < 9) return std::nullopt;
        for (std::size_t i = 0; i < 4; ++i) count |= crypt_value(setting[1 + i]) << (6 * i);
        for (std::size_t i = 0; i < 4; ++i) salt |= crypt_value(setting[5 + i]) << (6 * i);
        // Longer keys: the key block is encrypted with itself, then the next 8 characters are folded in.
        while (used < key.size()) {
            key_block = cipher(key_block, schedule(key_block), false);
            for (std::size_t i = 0; i < 8 && used < key.size(); ++i, ++used)
                key_block ^= static_cast<std::uint64_t>(static_cast<std::uint8_t>(key[used] << 1)) << (56 - 8 * i);
        }
        output = std::string(setting.substr(0, 9));
    } else {
        const char first = setting.empty() ? '\0' : setting[0], second = setting.size() > 1 ? setting[1] : '\0';
        salt = crypt_value(first) | crypt_value(second) << 6;
        count = 25;
        output.push_back(first);
        output.push_back(second ? second : first);
    }
    const auto keys = schedule(key_block);
    std::uint64_t block{};
    for (std::uint32_t round = 0; round < count; ++round) block = cipher(block, keys, false, salt);
    // 64 bits as 11 characters of 6 bits (the last padded with two zero bits).
    for (int shift = 58; shift > -6; shift -= 6) {
        const auto value = shift >= 0 ? (block >> shift) & 0x3f : (block << -shift) & 0x3f;
        output.push_back(crypt_alphabet[static_cast<std::size_t>(value)]);
    }
    return output;
}

} // namespace alchemy::libsystem::des
