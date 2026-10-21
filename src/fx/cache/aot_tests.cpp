// fx_cache_tests (AOT part): discovery over the real ARM64 Blender images with
// the stub translator, cache output consumed by the runtime path (owner: cache).

#include "fx/cache/aot.hpp"
#include "fx/cache/cache.hpp"
#include "fx/cache/cache_internal.hpp"
#include "fx/cache/pe_view.hpp"
#include "fx/cache/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::cache_test;

namespace {

const std::wstring kBlender = FX_CACHE_BLENDER_DIR;

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

const std::vector<std::wstring>& blender_images() {
    static const std::vector<std::wstring> images = find_images(kBlender);
    return images;
}

std::wstring blender_image(const wchar_t* file_name) {
    for (const std::wstring& path : blender_images()) {
        const std::wstring name = cache_detail::file_name_of(path);
        if (_wcsicmp(name.c_str(), file_name) == 0) return path;
    }
    return {};
}

std::wstring relative(const std::wstring& path) {
    return path.size() > kBlender.size() ? path.substr(kBlender.size() + 1) : path;
}

} // namespace

FX_TEST(aot_blender_dry_run_covers_pdata) {
    const auto& images = blender_images();
    if (images.empty()) {
        std::printf("  skipped: no ARM64 images under %s\n", narrow(kBlender).c_str());
        return;
    }
    struct Row {
        std::wstring path;
        AotStats stats;
    };
    std::vector<Row> rows;
    std::uint64_t functions = 0, pdata = 0, regions = 0, entries = 0, guest = 0, code = 0, failed = 0, flow = 0;
    std::uint64_t undiscovered_total = 0, uncovered_total = 0;
    double seconds = 0;
    const double start = now_seconds();
    for (const std::wstring& path : images) {
        AotOptions options;
        options.write_cache = false;
        AotStats stats;
        aot_detail::Detail detail;
        std::wstring error;
        aot_detail::translate_image(path, options, &stub_translate, &stats, &detail, &error);
        FX_CHECK(error.empty());
        // Discovery: every .pdata function is a root. Translation: each root ends
        // up covered by a stored region unless its translation failed.
        std::uint64_t undiscovered = 0, uncovered = 0;
        for (std::uint32_t rva : detail.pdata_begins) {
            undiscovered += std::binary_search(detail.static_roots.begin(), detail.static_roots.end(), rva) ? 0 : 1;
            uncovered += detail.is_covered(rva) ? 0 : 1;
        }
        FX_CHECK_EQ(undiscovered, std::uint64_t{0});
        FX_CHECK(uncovered <= stats.failed);
        if (uncovered) {
            std::printf("  %s: %llu of %llu .pdata functions not covered (%llu failed regions)\n",
                        narrow(relative(path)).c_str(), static_cast<unsigned long long>(uncovered),
                        static_cast<unsigned long long>(detail.pdata_begins.size()),
                        static_cast<unsigned long long>(stats.failed));
        }
        undiscovered_total += undiscovered;
        uncovered_total += uncovered;
        functions += stats.functions;
        pdata += stats.pdata;
        regions += stats.regions;
        entries += stats.entries;
        guest += stats.guest_instructions;
        code += stats.code_bytes;
        failed += stats.failed;
        flow += stats.flow_roots;
        seconds += stats.total_seconds;
        rows.push_back({path, stats});
    }
    FX_CHECK_EQ(undiscovered_total, std::uint64_t{0});
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.stats.pdata > b.stats.pdata; });
    for (std::size_t i = 0; i < rows.size() && i < 6; ++i)
        std::printf("  %s: %s\n", narrow(relative(rows[i].path)).c_str(), narrow(aot_format_stats(rows[i].stats)).c_str());
    std::printf("  %zu images: %llu functions (%llu .pdata, all discovered, %llu not covered because translation failed), +%llu by flow, %llu regions, %llu entries, "
                "%llu guest instructions, %llu failed; stub translation %.2f s (wall %.2f s)\n",
                images.size(), static_cast<unsigned long long>(functions), static_cast<unsigned long long>(pdata),
                static_cast<unsigned long long>(uncovered_total), static_cast<unsigned long long>(flow), static_cast<unsigned long long>(regions),
                static_cast<unsigned long long>(entries), static_cast<unsigned long long>(guest),
                static_cast<unsigned long long>(failed), seconds, now_seconds() - start);
    (void)code;
}

FX_TEST(aot_output_is_what_the_runtime_would_translate) {
    if (blender_images().empty()) {
        std::printf("  skipped: no Blender images\n");
        return;
    }
    for (const wchar_t* name : {L"BlendThumb.dll", L"SDL2.dll", L"python311.dll"}) {
        const std::wstring path = blender_image(name);
        if (path.empty()) continue;
        TempDir dir(L"aot");
        cache_detail::testing_reset(false);
        CacheConfig config;
        config.directory = dir.path();
        cache_configure(config);

        AotOptions options;
        AotStats stats;
        aot_detail::Detail detail;
        std::wstring error;
        const std::size_t written = aot_detail::translate_image(path, options, &stub_translate, &stats, &detail, &error);
        FX_CHECK(error.empty());
        FX_CHECK(written > 0);
        FX_CHECK(stats.file_bytes > 0);
        std::printf("  %s: %s\n", narrow(name).c_str(), narrow(aot_format_stats(stats)).c_str());

        MappedImage image(path);
        FX_REQUIRE(image.ok());
        FX_CHECK(image.base() != image.preferred_base());
        const GuestModule info = image.info();
        FX_CHECK(cache_file_path(info) == stats.cache_path);  // runtime computes the same key
        const double load_start = now_seconds();
        cache_module_loaded(info);
        const double load_us = (now_seconds() - load_start) * 1e6;

        pe::MappedFile file;
        pe::Image parsed;
        FX_REQUIRE(file.open(path, false) && parsed.parse(file.data(), file.size(), nullptr));
        ImageMemorySource memory(image, parsed);
        unsigned hits = 0, compared = 0, mismatched = 0;
        TranslatedRegion got;
        const double first_start = now_seconds();
        cache_lookup(image.base() + detail.pdata_begins.front(), got);  // waits for the asynchronous open
        const double first_us = (now_seconds() - first_start) * 1e6;
        const double lookup_start = now_seconds();
        for (std::uint32_t rva : detail.pdata_begins) {
            const std::uint64_t pc = image.base() + rva;
            if (!cache_lookup(pc, got)) continue;
            ++hits;
            bool is_entry = false;
            for (const RegionEntry& e : got.entries) is_entry |= e.guest_pc == pc;
            FX_CHECK(is_entry);
            for (const CodeFixup& f : got.fixups) {
                std::uint64_t slot = 0;
                std::memcpy(&slot, got.code.data() + f.offset, 8);
                FX_CHECK(slot == f.value && f.value >= image.base() && f.value <= image.base() + info.size);
            }
        }
        const double lookup_ns = (now_seconds() - lookup_start) * 1e9 / std::max<std::size_t>(1, detail.pdata_begins.size());
        // Byte-identical to translating the relocated image directly (sampled).
        for (std::size_t i = 0; i < detail.pdata_begins.size(); i += 7) {
            const std::uint64_t pc = image.base() + detail.pdata_begins[i];
            if (!cache_lookup(pc, got) || got.guest_start != pc) continue;
            ++compared;
            if (!regions_equal(got, stub_translate(pc, memory, TranslateOptions{}))) ++mismatched;
        }
        FX_CHECK_EQ(hits, static_cast<unsigned>(detail.pdata_begins.size()));
        FX_CHECK(compared > 0);
        FX_CHECK_EQ(mismatched, 0u);
        std::printf("    runtime: load %.0f us, first lookup %.0f us (open + map of the new file), %u/%zu .pdata "
                    "entries hit (%.0f ns/lookup), %u compared equal to a direct translation at the new base\n",
                    load_us, first_us, hits, detail.pdata_begins.size(), lookup_ns, compared - mismatched);
        cache_detail::testing_reset(false);
    }
}

FX_TEST(aot_follow_flow_finds_call_continuations) {
    const std::wstring path = blender_image(L"SDL2.dll");
    if (path.empty()) {
        std::printf("  skipped: SDL2.dll not found\n");
        return;
    }
    pe::MappedFile file;
    pe::Image image;
    FX_REQUIRE(file.open(path, false) && image.parse(file.data(), file.size(), nullptr));
    const std::vector<pe::Function> functions = pe::pdata_functions(image);
    auto uncovered_continuations = [&](const aot_detail::Detail& detail) {
        unsigned missing = 0;
        for (const pe::Function& f : functions) {
            for (std::uint32_t rva = f.begin; rva + 4 < f.end; rva += 4) {
                std::uint32_t word = 0;
                if (!image.read(rva, word)) break;
                const bool call = (word & 0xFC00'0000u) == 0x9400'0000u || (word & 0xFFFF'FC1Fu) == 0xD63F'0000u;
                if (call && !detail.is_covered(rva + 4)) ++missing;
            }
        }
        return missing;
    };
    g_stub_split_at_calls = true;  // regions end at calls: continuations must be found by AOT
    AotOptions options;
    options.write_cache = false;
    AotStats full, flow_only, static_only;
    aot_detail::Detail detail_full, detail_flow, detail_static;
    aot_detail::translate_image(path, options, &stub_translate, &full, &detail_full, nullptr);
    options.sweep_calls = false;
    aot_detail::translate_image(path, options, &stub_translate, &flow_only, &detail_flow, nullptr);
    options.follow_flow = false;
    aot_detail::translate_image(path, options, &stub_translate, &static_only, &detail_static, nullptr);
    g_stub_split_at_calls = false;
    const unsigned missing_full = uncovered_continuations(detail_full);
    const unsigned missing_flow = uncovered_continuations(detail_flow);
    const unsigned missing_static = uncovered_continuations(detail_static);
    FX_CHECK(full.call_sweep_roots > 0);
    FX_CHECK(flow_only.flow_roots > 0);
    FX_CHECK(missing_full <= full.failed);  // only continuations whose region could not be stored
    FX_CHECK(missing_flow < missing_static);
    std::printf("  SDL2.dll, regions split at calls: %llu static roots; uncovered call continuations: %u with sweep "
                "+ flow (+%llu sweep, +%llu flow roots), %u with flow only (code reached only through jump "
                "tables), %u with neither\n",
                static_cast<unsigned long long>(full.functions), missing_full,
                static_cast<unsigned long long>(full.call_sweep_roots),
                static_cast<unsigned long long>(full.flow_roots), missing_flow, missing_static);
}

FX_TEST(aot_rejects_bad_images) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring error;
    AotOptions options;
    options.write_cache = false;
    FX_CHECK_EQ(aot_detail::translate_image(self, options, &stub_translate, nullptr, nullptr, &error), std::size_t{0});
    FX_CHECK(error.find(L"ARM64") != std::wstring::npos);
    error.clear();
    FX_CHECK_EQ(aot_detail::translate_image(L"Z:\\does\\not\\exist.dll", options, &stub_translate, nullptr, nullptr,
                                            &error),
                std::size_t{0});
    FX_CHECK(!error.empty());
    TempDir dir(L"badpe");
    const std::wstring junk = dir.path() + L"\\junk.dll";
    HANDLE h = CreateFileW(junk.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    std::vector<std::uint8_t> bytes(8192, 0);
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[0x3C] = 0xF0;  // e_lfanew past the end of a plausible header
    bytes[0x3D] = 0xFF;
    DWORD done = 0;
    WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &done, nullptr);
    CloseHandle(h);
    error.clear();
    FX_CHECK_EQ(aot_detail::translate_image(junk, options, &stub_translate, nullptr, nullptr, &error), std::size_t{0});
    FX_CHECK(!error.empty());
    // Truncated copies of a real image never crash the parser or the driver.
    const std::wstring real = blender_image(L"BlendThumb.dll");
    if (!real.empty()) {
        pe::MappedFile file;
        FX_REQUIRE(file.open(real, false));
        for (std::size_t size : {std::size_t{64}, std::size_t{1024}, file.size() / 3, file.size() / 2, file.size() - 1}) {
            const std::wstring cut = dir.path() + L"\\cut.dll";
            h = CreateFileW(cut.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            WriteFile(h, file.data(), static_cast<DWORD>(size), &done, nullptr);
            CloseHandle(h);
            aot_detail::translate_image(cut, options, &stub_translate, nullptr, nullptr, nullptr);
        }
    }
}

FX_TEST(aot_cold_file_is_written_once_and_used_after_main_and_journals) {
    const std::wstring path = blender_image(L"SDL2.dll");
    if (path.empty()) {
        std::printf("  skipped: no SDL2.dll\n");
        return;
    }
    TempDir dir(L"aotcold");
    cache_detail::testing_reset(false);
    CacheConfig config;
    config.directory = dir.path();
    cache_configure(config);
    AotOptions options;
    options.cold_file = true;
    AotStats stats;
    std::wstring error;
    const std::size_t written = aot_detail::translate_image(path, options, &stub_translate, &stats, nullptr, &error);
    FX_CHECK(error.empty());
    FX_CHECK(written > 0);
    FX_CHECK_EQ(dir.files(L"*.fxa").size(), std::size_t{1});
    FX_CHECK_EQ(dir.files(L"*.fxc").size(), std::size_t{0});
    FX_CHECK_EQ(dir.files(L"*.aot.lock").size(), std::size_t{0});   // released (delete on close)
    // Another process (or run) finds it done and skips the work.
    AotStats again;
    FX_CHECK_EQ(aot_detail::translate_image(path, options, &stub_translate, &again, nullptr, &error), std::size_t{0});
    FX_CHECK(error.empty());
    FX_CHECK_EQ(again.regions, std::uint64_t{0});
    FX_CHECK(again.cache_path == stats.cache_path);

    // The runtime: hits from the cold file, which the preloader never walks.
    MappedImage image(path);
    FX_REQUIRE(image.ok());
    const GuestModule info = image.info();
    FX_CHECK(cache_file_path(info).substr(0, cache_file_path(info).size() - 4) + L".fxa" == stats.cache_path);
    cache_module_loaded(info);
    pe::MappedFile file;
    pe::Image parsed;
    FX_REQUIRE(file.open(path, false) && parsed.parse(file.data(), file.size(), nullptr));
    const auto functions = pe::pdata_functions(parsed);
    FX_REQUIRE(!functions.empty());
    TranslatedRegion got;
    unsigned hits = 0;
    for (std::size_t i = 0; i < functions.size(); i += 13) hits += cache_lookup(image.base() + functions[i].begin, got);
    FX_CHECK(hits > 0);
    FX_CHECK(cache_stats().cold_hits >= hits);
    std::uint64_t cursor = 0, pcs[4];
    FX_CHECK_EQ(cache_region_starts(image.base(), cursor, kCacheCursorEnd, pcs, std::size(pcs)), std::size_t{0});
    cache_detail::testing_reset(false);
}
