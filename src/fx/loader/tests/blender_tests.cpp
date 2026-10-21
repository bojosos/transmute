// Parse-only coverage over the ARM64 Blender 4.5 distribution: every ARM64
// DLL/PYD (and blender.exe) is mapped, relocated and bound with stub thunks;
// no guest code runs. Also measures blender.exe mapping time.

#include "test_support.hpp"

#include "fx/loader/loader_internal.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

using namespace transmute::fx;
using namespace transmute::fx::loader;
using namespace fxl_test;

namespace {

const std::wstring kBlender = L"" FXL_BLENDER_DIR;

bool blender_present() { return exists(kBlender + L"\\blender.exe"); }

double ms_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

FX_TEST(blender_parse_only_coverage) {
    if (!blender_present()) {
        std::printf("  skipped: %ls not found\n", kBlender.c_str());
        return;
    }
    fresh_loader();
    stubs().execute = false;
    LoaderConfig config;
    config.application_path = kBlender + L"\\blender.exe";
    config.run_initializers = false;
    loader_configure(config);

    const auto start = std::chrono::steady_clock::now();
    std::string error;
    bool init_failed = false;
    const std::uint64_t exe = load_exe(config.application_path, error, init_failed);
    FX_CHECK(exe != 0);
    const double exe_ms = ms_since(start);
    const auto after_exe = loader_stats();

    std::vector<std::wstring> candidates;
    std::size_t non_arm64 = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             kBlender, std::filesystem::directory_options::skip_permission_denied)) {
        if (!entry.is_regular_file()) continue;
        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        if (ext != L".dll" && ext != L".pyd") continue;
        if (probe_pe_file(entry.path().wstring()) != kMachineArm64) {
            ++non_arm64;
            continue;
        }
        candidates.push_back(entry.path().wstring());
    }
    std::vector<std::wstring> failures;
    const auto all_start = std::chrono::steady_clock::now();
    for (const auto& path : candidates) {
        std::string why;
        const std::uint64_t h = guest_load_internal(
            path, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, false, &why);
        if (!h || !find_guest_module(h)) {
            failures.push_back(path + L": " + widen(why));
        }
    }
    const double all_ms = ms_since(all_start);
    const auto stats = loader_stats();

    // Private assemblies resolve to blender.shared / blender.crt.
    const GuestModule* tbb = nullptr;
    const GuestModule* vcrt = nullptr;
    for (const auto* m : guest_modules()) {
        if (_wcsicmp(m->name.c_str(), L"tbb12.dll") == 0) tbb = m;
        if (_wcsicmp(m->name.c_str(), L"vcruntime140.dll") == 0) vcrt = m;
    }
    FX_CHECK(tbb && tbb->path.find(L"blender.shared") != std::wstring::npos);
    FX_CHECK(vcrt && vcrt->path.find(L"blender.crt") != std::wstring::npos);

    std::map<std::string, std::set<std::string>> missing_functions;
    std::set<std::string> missing_dlls, bad_images;
    std::size_t cycles = 0;
    for (const auto& d : loader_diagnostics()) {
        switch (d.kind) {
        case LoaderDiagnostic::Kind::missing_function: missing_functions[d.dll].insert(d.function); break;
        case LoaderDiagnostic::Kind::missing_dll: missing_dlls.insert(d.dll); break;
        case LoaderDiagnostic::Kind::bad_image: bad_images.insert(d.dll + ": " + d.message); break;
        case LoaderDiagnostic::Kind::import_cycle: ++cycles; break;
        default: break;
        }
    }
    std::map<std::wstring, int> by_path;
    for (const auto* m : guest_modules()) ++by_path[lower(m->path)];
    std::size_t duplicates = 0;
    for (const auto& [path, n] : by_path) {
        if (n > 1) {
            ++duplicates;
            std::printf("  blender: mapped %d times: %ls\n", n, path.c_str());
        }
    }
    FX_CHECK_EQ(duplicates, 0u);
    std::size_t delay_dlls = 0;
    for (const auto* m : guest_modules()) {
        const Module* record = find_loaded(m->path);
        FX_CHECK(record != nullptr);
        if (!record) {
            std::printf("  blender: find_loaded(%ls) failed\n", m->path.c_str());
            continue;
        }
        const PeDirectory delay = record->image.headers.dir(dir_delay_import);
        if (delay.size >= sizeof(IMAGE_DELAYLOAD_DESCRIPTOR)) {
            delay_dlls += delay.size / sizeof(IMAGE_DELAYLOAD_DESCRIPTOR) - 1;
        }
    }

    std::printf("  blender: %zu ARM64 DLL/PYD files (+blender.exe), %zu non-ARM64 skipped\n",
                candidates.size(), non_arm64);
    std::printf("  blender: %llu guest modules mapped (%llu relocated), %llu host DLLs\n",
                static_cast<unsigned long long>(stats.modules_mapped),
                static_cast<unsigned long long>(stats.modules_relocated),
                static_cast<unsigned long long>(stats.host_dlls));
    std::printf("  blender: imports bound: %llu to guest DLLs, %llu to host DLLs, %llu replaced, "
                "%llu missing; %llu forwarders\n",
                static_cast<unsigned long long>(stats.guest_imports),
                static_cast<unsigned long long>(stats.host_imports),
                static_cast<unsigned long long>(stats.replaced_imports),
                static_cast<unsigned long long>(stats.missing_imports),
                static_cast<unsigned long long>(stats.forwarders));
    std::printf("  blender.exe closure: %llu modules, %llu guest / %llu host imports, %.1f ms "
                "(map %.1f ms, bind %.1f ms)\n",
                static_cast<unsigned long long>(after_exe.modules_mapped),
                static_cast<unsigned long long>(after_exe.guest_imports),
                static_cast<unsigned long long>(after_exe.host_imports), exe_ms,
                after_exe.map_seconds * 1e3, after_exe.bind_seconds * 1e3);
    std::printf("  blender: everything: %.1f ms total (map %.1f ms, bind %.1f ms), %zu delay-load "
                "descriptors, %zu import cycles\n",
                exe_ms + all_ms, stats.map_seconds * 1e3, stats.bind_seconds * 1e3, delay_dlls, cycles);
    for (const auto& d : missing_dlls) std::printf("  blender: missing DLL %s\n", d.c_str());
    for (const auto& [dll, functions] : missing_functions) {
        std::string list;
        std::size_t shown = 0;
        for (const auto& f : functions) {
            if (shown++ < 12) list += (list.empty() ? "" : ", ") + f;
        }
        if (functions.size() > 12) list += ", ...";
        std::printf("  blender: %zu missing from %s: %s\n", functions.size(), dll.c_str(), list.c_str());
    }
    for (const auto& b : bad_images) std::printf("  blender: bad image %s\n", b.c_str());
    for (const auto& f : failures) std::printf("  blender: FAILED %ls\n", f.c_str());
    FX_CHECK(failures.empty());
    FX_CHECK(bad_images.empty());
    FX_CHECK(stats.guest_imports > 10000);
    FX_CHECK(stats.host_imports > 1000);
    fresh_loader();
}

FX_TEST(blender_exe_mapping_time) {
    const std::wstring path = kBlender + L"\\blender.exe";
    if (!blender_present()) {
        std::printf("  skipped: %ls not found\n", path.c_str());
        return;
    }
    for (const bool relocate : {false, true}) {
        std::vector<double> times;
        std::int64_t relocations = 0;
        for (int run = 0; run < 5; ++run) {
            MappedImage image;
            std::string error;
            MapOptions options;
            options.force_relocation = relocate;
            const auto start = std::chrono::steady_clock::now();
            const bool ok = map_image(path, options, image, error) && protect_image(image, error);
            times.push_back(ms_since(start));
            FX_REQUIRE(ok);
            relocations = image.relocations_applied;
            unmap_image(image);
        }
        std::sort(times.begin(), times.end());
        std::printf("  blender.exe map+protect%s: median %.1f ms (min %.1f, max %.1f), %lld relocations\n",
                    relocate ? " (relocated)" : "", times[2], times[0], times[4],
                    static_cast<long long>(relocations));
        FX_CHECK(times[2] < 2000.0);
    }
}

namespace {

// Image bytes with every base-relocated slot rebased to 0 and the header
// ImageBase cleared, so images mapped at different bases compare equal.
std::vector<std::uint8_t> normalized_image(const MappedImage& image) {
    const auto& h = image.headers;
    std::vector<std::uint8_t> bytes(image.base, image.base + h.size_of_image);
    const auto base = reinterpret_cast<std::uint64_t>(image.base);
    std::memset(bytes.data() + h.nt_offset + 24 + 24, 0, 8);
    const PeDirectory dir = h.dir(dir_basereloc);
    for (std::uint64_t at = dir.rva; at + 8 <= std::uint64_t{dir.rva} + dir.size;) {
        std::uint32_t page = 0, block = 0;
        std::memcpy(&page, bytes.data() + at, 4);
        std::memcpy(&block, bytes.data() + at + 4, 4);
        if (block < 8) break;
        for (std::uint64_t e = at + 8; e + 2 <= at + block; e += 2) {
            std::uint16_t entry = 0;
            std::memcpy(&entry, bytes.data() + e, 2);
            const std::uint64_t slot = std::uint64_t{page} + (entry & 0xfff);
            if ((entry >> 12) == IMAGE_REL_BASED_DIR64 && slot + 8 <= bytes.size()) {
                std::uint64_t value = 0;
                std::memcpy(&value, bytes.data() + slot, 8);
                value -= base;
                std::memcpy(bytes.data() + slot, &value, 8);
            } else if ((entry >> 12) == IMAGE_REL_BASED_HIGHLOW && slot + 4 <= bytes.size()) {
                std::uint32_t value = 0;
                std::memcpy(&value, bytes.data() + slot, 4);
                value -= static_cast<std::uint32_t>(base);
                std::memcpy(bytes.data() + slot, &value, 4);
            }
        }
        at += block;
    }
    return bytes;
}

} // namespace

// Image-section views (kernel layout and relocation) must hold exactly the
// bytes the copying mapper produces, at any base.
FX_TEST(blender_image_views_match_copied_images) {
    if (!blender_present()) {
        std::printf("  skipped: %ls not found\n", kBlender.c_str());
        return;
    }
    const wchar_t* names[] = {L"python311.dll", L"blender.shared/opencolorio_2_4.dll",
                              L"blender.shared/tbb12.dll", L"blender.crt/msvcp140.dll", L"blender.exe"};
    for (const wchar_t* name : names) {
        const std::wstring path = kBlender + L"\\" + name;
        if (!exists(path)) continue;
        for (const bool relocate : {false, true}) {
            MappedImage view, copy;
            std::string error;
            MapOptions options;
            options.force_relocation = relocate;
            FX_REQUIRE(map_image(path, options, view, error));
            options.image_view = false;
            FX_REQUIRE(map_image(path, options, copy, error));
            FX_CHECK(view.view);
            FX_CHECK(!copy.view);
            FX_CHECK(view.base != copy.base);
            FX_CHECK_EQ(view.size, copy.size);
            FX_CHECK_EQ(view.image_hash, copy.image_hash);
            FX_CHECK_EQ(view.text_begin - reinterpret_cast<std::uint64_t>(view.base),
                        copy.text_begin - reinterpret_cast<std::uint64_t>(copy.base));
            std::uint64_t actual = 0;
            std::memcpy(&actual, view.base + view.headers.nt_offset + 24 + 24, 8);
            FX_CHECK_EQ(actual, reinterpret_cast<std::uint64_t>(view.base));
            const auto a = normalized_image(view), b = normalized_image(copy);
            FX_CHECK(a == b);
            if (a != b) {
                std::size_t i = 0;
                while (i < a.size() && a[i] == b[i]) ++i;
                std::printf("  %ls%s: first difference at rva 0x%zx\n", name, relocate ? " (relocated)" : "", i);
            }
            FX_CHECK(protect_image(view, error));
            unmap_image(view);
            unmap_image(copy);
            FX_CHECK(view.base == nullptr);
        }
    }
}
