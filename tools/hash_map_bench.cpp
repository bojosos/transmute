// Micro-benchmark of the hash containers behind alchemy::hash_map on the
// access patterns of Alchemy's hot tables: std::unordered_map vs tsl::robin_map
// (third_party/robin-map). Build (x64 Developer prompt):
//   cl /O2 /EHsc /std:c++20 /I include /I third_party/robin-map/include tools/hash_map_bench.cpp
// Patterns:
//   objects   ObjectStore shard: 64k live 16-byte-aligned addresses, find-heavy
//             (9 finds per insert+erase), shared_ptr values
//   malloc    guest malloc table: insert + erase churn around 200k live blocks
//   tlv       per-thread TLV cache: ~300 descriptors, find only
//   exports   loader export index: 100k string keys, string_view lookups (hits and misses)
#include "alchemy/hash_map.hpp"

#include <tsl/robin_map.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
template <class F> double best_ms(F&& run, int repeats = 5) {
    double best = 1e30;
    for (int i = 0; i < repeats; ++i) {
        const auto start = Clock::now();
        run();
        best = std::min(best, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
    }
    return best;
}
volatile std::uint64_t sink;

template <class Map> double objects() {
    std::mt19937_64 random(1);
    std::vector<std::uint64_t> live;
    Map map;
    for (int i = 0; i < 65536; ++i) {
        const auto address = 0x600000000ULL + (random() % (1ULL << 28)) * 16;
        map.emplace(address, std::make_shared<int>(i));
        live.push_back(address);
    }
    return best_ms([&] {
        std::uint64_t hits = 0;
        for (int round = 0; round < 200000; ++round) {
            for (int k = 0; k < 9; ++k) hits += map.find(live[(round * 9 + k) % live.size()]) != map.end();
            const auto victim = round % live.size();
            map.erase(live[victim]);
            live[victim] = 0x600000000ULL + (random() % (1ULL << 28)) * 16;
            map.emplace(live[victim], std::shared_ptr<int>());
        }
        sink = hits;
    });
}

template <class Map> double malloc_table() {
    std::mt19937_64 random(2);
    std::vector<std::uint64_t> live;
    Map map;
    for (int i = 0; i < 200000; ++i) { live.push_back(0x7000000000ULL + i * 64ULL); map.emplace(live.back(), std::pair<std::uint64_t, std::uint64_t>{64, live.back()}); }
    return best_ms([&] {
        std::uint64_t total = 0;
        for (int round = 0; round < 1000000; ++round) {
            const auto victim = random() % live.size();
            const auto found = map.find(live[victim]);
            total += found->second.first;
            map.erase(found);
            live[victim] += 0x10000000ULL;
            map.emplace(live[victim], std::pair<std::uint64_t, std::uint64_t>{64, live[victim]});
        }
        sink = total;
    });
}

template <class Map> double tlv() {
    Map map;
    std::vector<std::uint64_t> keys;
    for (int i = 0; i < 300; ++i) { keys.push_back(0x100000000ULL + i * 24ULL); map.emplace(keys.back(), keys.back() + 1); }
    return best_ms([&] {
        std::uint64_t total = 0;
        for (int round = 0; round < 5000000; ++round) total += map.find(keys[(round * 7) % keys.size()])->second;
        sink = total;
    });
}

template <class Map> double exports() {
    std::vector<std::string> names;
    for (int i = 0; i < 100000; ++i) names.push_back("__ZN7blender3gpu" + std::to_string(i * 7919) + "_symbol_name_E");
    Map map;
    for (std::size_t i = 0; i < names.size(); ++i) map.emplace(names[i], i);
    std::vector<std::string> misses;
    for (int i = 0; i < 1000; ++i) misses.push_back("_objc_missing_" + std::to_string(i));
    return best_ms([&] {
        std::uint64_t total = 0;
        for (int round = 0; round < 1000000; ++round) {
            const std::string_view key = round % 4 == 3 ? std::string_view(misses[round % misses.size()]) : std::string_view(names[(round * 31) % names.size()]);
            const auto found = map.find(key);
            total += found != map.end() ? found->second : 1;
        }
        sink = total;
    });
}
} // namespace

int main() {
    using Address = std::uint64_t;
    using alchemy::address_hash;
    using Object = std::shared_ptr<int>;
    using Block = std::pair<std::uint64_t, std::uint64_t>;
    struct Row { const char* name; double standard, robin; };
    const Row rows[] = {
        {"objects  (9 finds + erase + insert) x200k", objects<std::unordered_map<Address, Object>>(),
         objects<tsl::robin_map<Address, Object, address_hash>>()},
        {"malloc   (find + erase + insert) x1M", malloc_table<std::unordered_map<Address, Block>>(),
         malloc_table<tsl::robin_map<Address, Block, address_hash>>()},
        {"tlv      (find) x5M", tlv<std::unordered_map<Address, Address>>(), tlv<tsl::robin_map<Address, Address, address_hash>>()},
        {"exports  (string_view find, 25% miss) x1M",
         exports<std::unordered_map<std::string, std::size_t, alchemy::string_hash, alchemy::string_equal>>(),
         exports<tsl::robin_map<std::string, std::size_t, alchemy::string_hash, alchemy::string_equal>>()},
    };
    std::printf("%-46s %12s %12s %8s\n", "pattern", "unordered ms", "robin ms", "speedup");
    for (const auto& row : rows)
        std::printf("%-46s %12.1f %12.1f %7.2fx\n", row.name, row.standard, row.robin, row.standard / row.robin);
    // String keys with the stored hash (alchemy::string_hash_map).
    const auto stored = exports<alchemy::string_hash_map<std::size_t>>();
    std::printf("%-46s %12s %12.1f %7.2fx (StoreHash=true vs unordered)\n", "exports  (string_hash_map, StoreHash=true)", "",
                stored, rows[3].standard / stored);
    // The malloc table was a std::map before (darwin_services.cpp).
    const auto tree = malloc_table<std::map<Address, Block>>();
    std::printf("%-46s %12.1f (std::map, the previous malloc table)\n", "malloc   (find + erase + insert) x1M", tree);
}
