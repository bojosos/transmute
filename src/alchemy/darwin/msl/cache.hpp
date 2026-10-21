#pragma once

// Compile-result cache for the MSL compiler (owner: A6). Two levels: an
// in-process map and files under detail::cache_root()/results (disabled with
// ALCHEMY_MSL_CACHE=0). The key folds in compiler_version(), every
// CompileOptions field and the source, so a hit is indistinguishable from a
// fresh compile. Results with errors are cached in memory only.

#include "alchemy/msl/compiler.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::msl::detail {

[[nodiscard]] std::string cache_key(std::string_view source, const CompileOptions& options);
[[nodiscard]] std::optional<CompileResult> cache_lookup(const std::string& key);
void cache_store(const std::string& key, const CompileResult& result);
// Loads every disk result whose key has this compiler_version() into memory.
std::size_t cache_preload();

// Serialisation used by the disk cache (exposed for tests).
[[nodiscard]] std::vector<std::uint8_t> serialize(const CompileResult& result);
[[nodiscard]] std::optional<CompileResult> deserialize(const std::vector<std::uint8_t>& bytes);

struct CacheStatistics { std::uint64_t memory_hits{}, disk_hits{}, misses{}, seed_hits{}; };
// Read-only seed results consulted after a disk miss (msl::set_seed_directory).
void set_seed_directory(const std::filesystem::path& directory);
[[nodiscard]] CacheStatistics cache_statistics();
void clear_memory_cache();   // tests: force the disk path

} // namespace alchemy::msl::detail
