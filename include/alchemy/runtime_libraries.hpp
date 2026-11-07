#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace alchemy {

// Apple system-library install names that a project-built guest runtime
// library may claim. Every other /usr/lib or /System/Library identity must be
// served by an Alchemy host provider. An allow-listed identity is accepted
// only from a --runtime-dir directory whose provenance.json names the entry's
// pinned upstream source and lists that file's SHA-256 (written by the
// corresponding scripts/build_alchemy_*.ps1).
struct RuntimeLibraryIdentity {
    std::string_view install_name;
    std::string_view upstream;   // provenance.json "source"
    std::string_view build_script;
    // A declaration-only identity: without a runtime directory, imports from it
    // are resolved by host providers and fail individually when unsupported.
    bool provider_fallback{};
};

[[nodiscard]] const RuntimeLibraryIdentity* runtime_library_identity(std::string_view install_name) noexcept;

// The runtime identity whose exports answer the names a host-provider identity
// lacks (its "companion"), or empty. Apps built for macOS 14 and later bind
// Foundation's Swift overlay ($s10Foundation...) through Foundation.framework
// itself, older ones through libswiftFoundation. The loader loads a companion
// only for an image importing a name the provider lacks, and searches it after
// the provider (docs/ALCHEMY_SWIFT.md).
[[nodiscard]] std::string_view companion_identity(std::string_view provider) noexcept;

// The file a runtime identity resolves to in the given runtime directories, by
// the install name's file name. Returns an empty path when none exists.
[[nodiscard]] std::filesystem::path find_runtime_library(const RuntimeLibraryIdentity& identity,
    std::span<const std::filesystem::path> runtime_directories);

// Throws std::runtime_error naming the failed requirement unless `file` lies in
// one of `runtime_directories` and the provenance.json beside it records the
// identity's upstream source and the file's exact SHA-256.
void verify_runtime_library(const RuntimeLibraryIdentity& identity, const std::filesystem::path& file,
    std::span<const std::filesystem::path> runtime_directories);

// Lower-case hexadecimal SHA-256 of a file's contents.
[[nodiscard]] std::string sha256_file(const std::filesystem::path& file);

} // namespace alchemy
