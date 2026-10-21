#pragma once

// Helpers shared by the cache test executables (owner: cache).

#include "fx/cache/cache.hpp"
#include "fx/cache/pe_view.hpp"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace transmute::fx::cache_test {

// Fresh directory under %TEMP%, removed (files only, one level) on destruction.
class TempDir {
public:
    explicit TempDir(const wchar_t* tag);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::wstring& path() const { return path_; }
    [[nodiscard]] std::vector<std::wstring> files(const wchar_t* pattern = L"*") const;

private:
    std::wstring path_;
};

// Committed read/write host memory standing in for a mapped guest module.
class FakeModule {
public:
    FakeModule(std::uint64_t size, std::uint32_t seed);
    FakeModule(const FakeModule& copy_contents_from, int);  // same bytes, new base
    ~FakeModule();
    FakeModule(const FakeModule&) = delete;
    FakeModule& operator=(const FakeModule&) = delete;
    [[nodiscard]] std::uint64_t base() const { return reinterpret_cast<std::uint64_t>(memory_); }
    [[nodiscard]] std::uint64_t size() const { return size_; }
    [[nodiscard]] std::uint8_t* bytes() const { return memory_; }
    [[nodiscard]] GuestModule info(std::uint64_t image_hash, const wchar_t* name = L"fake.dll") const;

private:
    std::uint8_t* memory_{};
    std::uint64_t size_{};
};

// Deterministic synthetic region at `base + rva` covering `instructions`
// guest words: entries, exits, fixups (values embedded in the code as the
// JIT produces them), faults, pins and guest ranges all depend only on
// (rva, seed), so the same call with another base is the expected rebase.
TranslatedRegion make_region(std::uint64_t base, std::uint32_t rva, std::uint32_t instructions, std::uint32_t seed);
bool regions_equal(const TranslatedRegion& a, const TranslatedRegion& b);

std::wstring cache_dir_config(CacheConfig& config, const TempDir& dir);

// Stub translator with the shape of real output. Decodes branches: B/BL/
// B.cond/CBZ/TBZ become exits with fixups, BL/BLR continuations become
// entries (unless split_at_calls), loads/stores get fault sites, ADRP results
// become fixups. Stops at B/BR/RET, function bounds or max_instructions.
TranslatedRegion stub_translate(std::uint64_t pc, const CodeSource& code, const TranslateOptions& options);
extern bool g_stub_split_at_calls;

// ARM64 PE images (dll/exe/pyd) under `root`, recursively.
std::vector<std::wstring> find_images(const std::wstring& root);

// Minimal image mapping for tests: headers and raw section data copied into
// fresh memory (no relocations, imports or protection changes).
class MappedImage {
public:
    // `at_preferred_base`: map at the image's preferred base (ok() is false if taken).
    explicit MappedImage(const std::wstring& path, bool at_preferred_base = false);
    ~MappedImage();
    MappedImage(const MappedImage&) = delete;
    MappedImage& operator=(const MappedImage&) = delete;
    [[nodiscard]] bool ok() const { return memory_ != nullptr; }
    [[nodiscard]] GuestModule info() const;
    [[nodiscard]] std::uint64_t base() const { return reinterpret_cast<std::uint64_t>(memory_); }
    [[nodiscard]] std::uint64_t preferred_base() const { return preferred_base_; }

private:
    std::wstring path_;
    std::uint8_t* memory_{};
    std::uint64_t size_{};
    std::uint64_t preferred_base_{};
};

// What the JIT sees for a MappedImage: guest memory of executable sections,
// function bounds from .pdata (sorted and deduplicated like the AOT source).
class ImageMemorySource final : public CodeSource {
public:
    ImageMemorySource(const MappedImage& image, const pe::Image& file);
    bool read(std::uint64_t address, std::uint32_t& word) const override;
    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override;

private:
    std::uint64_t base_;
    std::uint64_t size_;
    std::vector<pe::Function> functions_;
    std::vector<pe::Section> executable_;
};

double now_seconds();

} // namespace transmute::fx::cache_test
