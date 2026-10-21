#pragma once

// Project hash containers. alchemy::hash_map / alchemy::hash_set are
// tsl::robin_map / tsl::robin_set (open addressing, robin hood hashing,
// third_party/robin-map, MIT) when ALCHEMY_ROBIN_MAP is on (the default),
// and std::unordered_map / std::unordered_set otherwise (bisecting).
//
// Unlike std::unordered_map, robin_map has no node stability: an insert or
// rehash moves elements and invalidates every iterator, pointer and
// reference into the table; erase invalidates too. A mutable mapped value is
// reached through it.value() (it->second is const). Use these aliases only
// where no pointer or reference to an element outlives the next insert or
// erase (store values, or pointers to objects owned elsewhere).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#if defined(ALCHEMY_ROBIN_MAP) && ALCHEMY_ROBIN_MAP
#include <tsl/robin_map.h>
#include <tsl/robin_set.h>
#else
#include <unordered_map>
#include <unordered_set>
#endif

namespace alchemy {

#if defined(ALCHEMY_ROBIN_MAP) && ALCHEMY_ROBIN_MAP
template <class Key, class T, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
using hash_map = tsl::robin_map<Key, T, Hash, KeyEqual>;
template <class Key, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
using hash_set = tsl::robin_set<Key, Hash, KeyEqual>;
inline constexpr bool hash_map_is_robin = true;
#else
template <class Key, class T, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
using hash_map = std::unordered_map<Key, T, Hash, KeyEqual>;
template <class Key, class Hash = std::hash<Key>, class KeyEqual = std::equal_to<Key>>
using hash_set = std::unordered_set<Key, Hash, KeyEqual>;
inline constexpr bool hash_map_is_robin = false;
#endif

// The mapped value of an iterator, writable with either implementation.
template <class Iterator>
auto& mapped(Iterator it) {
#if defined(ALCHEMY_ROBIN_MAP) && ALCHEMY_ROBIN_MAP
    return it.value();
#else
    return it->second;
#endif
}

// Integer keys (addresses): a cheap 64-bit mix. robin_map's power-of-two
// tables index by the low hash bits, which an identity hash (libstdc++)
// leaves zero for aligned addresses; MSVC's byte-wise FNV-1a is slower.
struct address_hash {
    std::size_t operator()(std::uint64_t value) const noexcept {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdULL;
        value ^= value >> 33;
        return static_cast<std::size_t>(value);
    }
};

// String keys looked up by std::string_view without building a std::string
// (heterogeneous lookup: hash_map<std::string, T, string_hash, string_equal>).
struct string_hash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        std::uint64_t hash = 0xcbf29ce484222325ULL;   // FNV-1a
        for (const auto c : value) hash = (hash ^ static_cast<unsigned char>(c)) * 0x100000001b3ULL;
        return static_cast<std::size_t>(hash ^ (hash >> 29));
    }
};
struct string_equal {
    using is_transparent = void;
    bool operator()(std::string_view left, std::string_view right) const noexcept { return left == right; }
};

// String-keyed tables: heterogeneous std::string_view lookup, and (robin_map)
// the stored hash, so probes compare hashes before strings and rehashing
// never rehashes a string.
#if defined(ALCHEMY_ROBIN_MAP) && ALCHEMY_ROBIN_MAP
template <class T>
using string_hash_map = tsl::robin_map<std::string, T, string_hash, string_equal,
                                       std::allocator<std::pair<std::string, T>>, /*StoreHash=*/true>;
using string_hash_set = tsl::robin_set<std::string, string_hash, string_equal, std::allocator<std::string>, /*StoreHash=*/true>;
// Keys owned elsewhere (strings that outlive the table, e.g. an image's export names).
template <class T>
using string_view_hash_map = tsl::robin_map<std::string_view, T, string_hash, string_equal,
                                            std::allocator<std::pair<std::string_view, T>>, /*StoreHash=*/true>;
#else
template <class T>
using string_view_hash_map = std::unordered_map<std::string_view, T, string_hash, string_equal>;
template <class T>
using string_hash_map = std::unordered_map<std::string, T, string_hash, string_equal>;
using string_hash_set = std::unordered_set<std::string, string_hash, string_equal>;
#endif

} // namespace alchemy
