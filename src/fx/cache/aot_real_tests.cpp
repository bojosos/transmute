// fx_cache_aot_tests: AOT and cache round trip with the real translator
// (built only when the fx_translator target exists; owner: cache).

#include "fx/cache/aot.hpp"
#include "fx/cache/cache.hpp"
#include "fx/cache/cache_internal.hpp"
#include "fx/cache/format.hpp"
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

void register_families() {
    static const bool once = [] {
#if defined(FX_CACHE_HAVE_FP)
        register_fp_emitters();
#endif
#if defined(FX_CACHE_HAVE_NEON)
        register_neon_emitters();
#endif
#if defined(FX_CACHE_HAVE_SYSTEM)
        register_system_emitters();
#endif
        return true;
    }();
    (void)once;
}

std::wstring blender_image(const wchar_t* relative_path) {
    const std::wstring path = kBlender + L"\\" + relative_path;
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES ? path : std::wstring();
}

void run_image(const wchar_t* relative_path, bool compare) {
    const std::wstring path = blender_image(relative_path);
    if (path.empty()) {
        std::printf("  skipped: %s not found\n", narrow(relative_path).c_str());
        return;
    }
    register_families();
    TempDir dir(L"aot-real");
    cache_detail::testing_reset(false);
    CacheConfig config;
    config.directory = dir.path();
    cache_configure(config);

    AotOptions options;
    AotStats stats;
    aot_detail::Detail detail;
    std::wstring error;
    const std::size_t written = aot_detail::translate_image(path, options, &translate_region, &stats, &detail, &error);
    FX_CHECK(error.empty());
    FX_CHECK(written > 0);
    std::uint64_t uncovered = 0;
    for (std::uint32_t rva : detail.pdata_begins) uncovered += detail.is_covered(rva) ? 0 : 1;
    std::printf("  %s: %s\n", narrow(relative_path).c_str(), narrow(aot_format_stats(stats)).c_str());
    std::printf("    .pdata functions not covered: %llu of %zu\n", static_cast<unsigned long long>(uncovered),
                detail.pdata_begins.size());

    MappedImage image(path);
    FX_REQUIRE(image.ok());
    {
        // Region overlap: translated guest instructions per executable word in the image.
        pe::MappedFile file;
        pe::Image parsed;
        FX_REQUIRE(file.open(path, false) && parsed.parse(file.data(), file.size(), nullptr));
        std::uint64_t words = 0;
        for (const pe::Section& s : parsed.sections())
            if (s.executable) words += s.raw_size / 4;
        // Why regions could not be stored (addresses outside the image).
        ImageMemorySource memory(image, parsed);
        unsigned exit_out = 0, fixup_out = 0, confirmed = 0, other = 0, shown = 0;
        const std::uint64_t lo = image.base(), hi = image.base() + parsed.size_of_image();
        for (std::uint32_t rva : detail.unencodable) {
            TranslatedRegion r;
            try {
                r = translate_region(image.base() + rva, memory, TranslateOptions{});
            } catch (...) {
                ++other;
                continue;
            }
            bool exit_bad = false, fixup_bad = false;
            for (const RegionExit& x : r.exits) exit_bad |= x.target_pc < lo || x.target_pc > hi;
            for (const CodeFixup& f : r.fixups) fixup_bad |= f.value < lo || f.value > hi;
            exit_out += exit_bad;
            fixup_out += fixup_bad && !exit_bad;
            if (!exit_bad && !fixup_bad) {
                // Unrelocatable at the preferred base: confirmed if the same image
                // address is embedded without a fixup at this base too.
                const std::int64_t at = transmute::fx::cachefmt::unrecorded_address(r, lo, hi - lo);
                if (at >= 0) ++confirmed;
                else ++other;
            }
            if (shown < 3 && (exit_bad || fixup_bad)) {
                ++shown;
                for (const CodeFixup& f : r.fixups)
                    if (f.value < lo || f.value > hi)
                        std::printf("      rva 0x%x: address 0x%llx outside the image (rva %lld)\n", rva,
                                    static_cast<unsigned long long>(f.value),
                                    static_cast<long long>(f.value - image.base()));
            }
        }
        // Do regions stay inside their .pdata function (sampled)?
        std::uint64_t inside = 0, outside = 0, sampled = 0, max_len = 0;
        for (std::size_t i = 0; i < detail.pdata_begins.size(); i += 97) {
            const std::uint64_t pc = image.base() + detail.pdata_begins[i];
            std::uint64_t fb = 0, fe = 0;
            if (!memory.function_bounds(pc, fb, fe)) continue;
            TranslatedRegion r;
            try {
                r = translate_region(pc, memory, TranslateOptions{});
            } catch (...) {
                continue;
            }
            ++sampled;
            for (const auto& [b, e] : r.guest_ranges) {
                max_len = std::max(max_len, (e - b) / 4);
                for (std::uint64_t a = b; a < e; a += 4) ++((a >= fb && a < fe) ? inside : outside);
            }
        }
        std::printf("    sampled %llu function regions: %.1f%% of guest words outside the function's .pdata bounds, "
                    "longest range %llu instructions\n",
                    static_cast<unsigned long long>(sampled),
                    100.0 * static_cast<double>(outside) / static_cast<double>(std::max<std::uint64_t>(1, inside + outside)),
                    static_cast<unsigned long long>(max_len));
        std::printf("    overlap: %.2f translated guest instructions per executable word (%llu words); unencodable: "
                    "%u exit outside image, %u fixup outside image, %u embedded image address without a fixup "
                    "(confirmed at the new base too), %u other\n",
                    static_cast<double>(stats.guest_instructions) / static_cast<double>(std::max<std::uint64_t>(1, words)),
                    static_cast<unsigned long long>(words), exit_out, fixup_out, confirmed, other);
    }
    const GuestModule info = image.info();
    FX_CHECK(cache_file_path(info) == stats.cache_path);
    double t0 = now_seconds();
    cache_module_loaded(info);
    const double load_us = (now_seconds() - t0) * 1e6;
    TranslatedRegion got;
    unsigned hits = 0;
    t0 = now_seconds();
    std::vector<std::uint32_t> missed;  // at the new base: uncovered or pinned
    for (std::uint32_t rva : detail.pdata_begins) {
        if (cache_lookup(image.base() + rva, got)) ++hits;
        else missed.push_back(rva);
    }
    const double lookup_ns = (now_seconds() - t0) * 1e9 / std::max<std::size_t>(1, detail.pdata_begins.size());
    // At a base other than the preferred one, only pinned regions may miss.
    const std::uint64_t pinned_skipped = cache_stats().pinned_mismatches;
    FX_CHECK(hits + uncovered <= detail.pdata_begins.size());
    FX_CHECK(hits + uncovered + pinned_skipped >= detail.pdata_begins.size());

    unsigned compared = 0, mismatched = 0;
    double jit_seconds = 0;
    if (compare) {
        pe::MappedFile file;
        pe::Image parsed;
        FX_REQUIRE(file.open(path, false) && parsed.parse(file.data(), file.size(), nullptr));
        ImageMemorySource memory(image, parsed);
        for (std::size_t i = 0; i < detail.pdata_begins.size(); i += 13) {
            const std::uint64_t pc = image.base() + detail.pdata_begins[i];
            if (!cache_lookup(pc, got) || got.guest_start != pc) continue;
            const double j0 = now_seconds();
            TranslatedRegion direct;
            try {
                direct = translate_region(pc, memory, TranslateOptions{});
            } catch (...) {
                continue;
            }
            jit_seconds += now_seconds() - j0;
            // AOT regions stop where another region already starts
            // (CodeSource::has_entry over parallel claims), so their shape can
            // differ from a standalone JIT. Byte-compare same-shaped regions:
            // that is what exposes missing relocations.
            if (got.guest_ranges != direct.guest_ranges) continue;
            ++compared;
            if (regions_equal(got, direct)) continue;
            ++mismatched;
            if (mismatched > 3) continue;
            // Diagnose: which part differs, and does a code difference equal the base delta
            // (an absolute guest address embedded without a fixup)?
            const std::int64_t delta = static_cast<std::int64_t>(image.base() - image.preferred_base());
            std::printf("    MISMATCH at rva 0x%llx: code %zu/%zu bytes, entries %zu/%zu, exits %zu/%zu, fixups %zu/%zu, "
                        "faults %zu/%zu, ranges %zu/%zu\n",
                        static_cast<unsigned long long>(pc - image.base()), got.code.size(), direct.code.size(),
                        got.entries.size(), direct.entries.size(), got.exits.size(), direct.exits.size(),
                        got.fixups.size(), direct.fixups.size(), got.faults.size(), direct.faults.size(),
                        got.guest_ranges.size(), direct.guest_ranges.size());
            if (got.code.size() == direct.code.size()) {
                unsigned shown = 0;
                for (std::size_t b = 0; b < got.code.size() && shown < 4; ++b) {
                    if (got.code[b] == direct.code[b]) continue;
                    std::uint64_t a = 0, d = 0;
                    const std::size_t at = b >= 4 ? b - 4 : 0;
                    std::memcpy(&a, got.code.data() + at, std::min<std::size_t>(8, got.code.size() - at));
                    std::memcpy(&d, direct.code.data() + at, std::min<std::size_t>(8, direct.code.size() - at));
                    std::printf("      code +0x%zx: cached %016llx direct %016llx (difference %lld, base delta %lld)\n", b,
                                static_cast<unsigned long long>(a), static_cast<unsigned long long>(d),
                                static_cast<long long>(d - a), static_cast<long long>(delta));
                    ++shown;
                    b += 7;
                }
            }
        }
        FX_CHECK_EQ(mismatched, 0u);
    }
    std::printf("    runtime at a new base: load %.0f us, %u/%zu hits (%llu pinned regions skipped), %.0f ns/lookup; "
                "%u sampled regions identical to a direct JIT at the new base (JIT %.1f us/region vs cache lookup)\n",
                load_us, hits, detail.pdata_begins.size(), static_cast<unsigned long long>(pinned_skipped), lookup_ns,
                compared - mismatched, compared ? jit_seconds * 1e6 / compared : 0.0);

    // The same file with the image at its preferred base: pinned regions are usable too.
    cache_detail::testing_reset(false);
    MappedImage at_preferred(path, true);
    if (!at_preferred.ok()) {
        std::printf("    preferred base 0x%llx unavailable in this process: pinned check skipped\n",
                    static_cast<unsigned long long>(image.preferred_base()));
        return;
    }
    cache_module_loaded(at_preferred.info());
    unsigned preferred_hits = 0;
    for (std::uint32_t rva : detail.pdata_begins)
        preferred_hits += cache_lookup(at_preferred.base() + rva, got) ? 1 : 0;
    FX_CHECK_EQ(static_cast<std::uint64_t>(preferred_hits) + uncovered,
                static_cast<std::uint64_t>(detail.pdata_begins.size()));
    unsigned preferred_compared = 0, preferred_mismatched = 0, pinned_compared = 0, pinned_mismatched = 0;
    {
        pe::MappedFile file;
        pe::Image parsed;
        FX_REQUIRE(file.open(path, false) && parsed.parse(file.data(), file.size(), nullptr));
        ImageMemorySource memory(at_preferred, parsed);
        auto same_as_jit = [&](std::uint64_t pc, bool& compared_one) {
            compared_one = false;
            if (!cache_lookup(pc, got) || got.guest_start != pc) return true;
            TranslatedRegion direct;
            try {
                direct = translate_region(pc, memory, TranslateOptions{});
            } catch (...) {
                return true;
            }
            if (got.guest_ranges != direct.guest_ranges) return true;   // shape differs (has_entry stop)
            compared_one = true;
            return regions_equal(got, direct);
        };
        const std::size_t stride = compare ? 5 : 97;
        for (std::size_t i = 0; i < detail.pdata_begins.size(); i += stride) {
            bool counted = false;
            const bool same = same_as_jit(at_preferred.base() + detail.pdata_begins[i], counted);
            preferred_compared += counted;
            preferred_mismatched += counted && !same;
        }
        // What the audit flags in pinned regions (for the translator owner).
        unsigned shown = 0;
        std::uint64_t in_exec = 0, in_data = 0, movabs = 0;
        for (std::uint32_t rva : missed) {
            const std::uint64_t pc = at_preferred.base() + rva;
            if (!cache_lookup(pc, got)) continue;
            const std::int64_t at = transmute::fx::cachefmt::unrecorded_address(got, at_preferred.base(),
                                                                                 parsed.size_of_image());
            if (at < 0) continue;
            std::uint64_t value = 0;
            std::memcpy(&value, got.code.data() + at, 8);
            const auto target = static_cast<std::uint32_t>(value - at_preferred.base());
            ++(parsed.executable(target) ? in_exec : in_data);
            movabs += at >= 2 && (got.code[at - 2] & 0xF8) == 0x48 && (got.code[at - 1] & 0xF8) == 0xB8;
            if (shown++ < 3) {
                std::printf("      pinned rva 0x%x: code +0x%llx bytes %02x %02x before value 0x%llx (image rva 0x%x, %s)\n",
                            rva, static_cast<unsigned long long>(at), at >= 2 ? got.code[at - 2] : 0,
                            at >= 1 ? got.code[at - 1] : 0, static_cast<unsigned long long>(value), target,
                            parsed.executable(target) ? "code" : "data");
            }
        }
        if (in_exec + in_data)
            std::printf("      unfixed values point into code %llu times, into data %llu times; %llu are movabs "
                        "immediates\n",
                        static_cast<unsigned long long>(in_exec), static_cast<unsigned long long>(in_data),
                        static_cast<unsigned long long>(movabs));
        // Every region that missed at the new base and hits here is a pinned one.
        for (std::uint32_t rva : missed) {
            bool counted = false;
            const bool same = same_as_jit(at_preferred.base() + rva, counted);
            pinned_compared += counted;
            pinned_mismatched += counted && !same;
        }
        FX_CHECK_EQ(preferred_mismatched, 0u);
        FX_CHECK_EQ(pinned_mismatched, 0u);
    }
    std::printf("    runtime at the preferred base: %u/%zu hits (%llu regions stored pinned); %u sampled regions "
                "and %u of %u pinned regions identical to a direct JIT there\n",
                preferred_hits, detail.pdata_begins.size(), static_cast<unsigned long long>(stats.unrelocatable),
                preferred_compared - preferred_mismatched, pinned_compared - pinned_mismatched, pinned_compared);
    cache_detail::testing_reset(false);
}

} // namespace

FX_TEST(aot_real_small_images) {
    run_image(L"BlendThumb.dll", true);
    run_image(L"blender.shared\\SDL2.dll", true);
    run_image(L"python311.dll", true);
}

FX_TEST(aot_real_blender_exe) {
    run_image(L"blender.exe", false);
}
