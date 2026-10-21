// Background translation of external (non-PE) modules (owner: cache):
// flow-ahead from roots and from regions the guest uses, pool lookups and
// promotion to the journal, warm modules left alone, the static phase and its
// cold file. Stub translator (test_support.cpp).

#include "fx/cache/cache.hpp"
#include "fx/cache/cache_internal.hpp"
#include "fx/cache/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <cstring>
#include <iterator>

using namespace transmute::fx;
using namespace transmute::fx::cache_test;

namespace {

constexpr std::uint64_t kImageHash = 0x5eed'0123'4567'89abULL;
constexpr std::uint32_t kAdd = 0x91000400;   // add x0, x0, #1
constexpr std::uint32_t kRet = 0xD65F03C0;

std::uint32_t bl(std::uint32_t from_rva, std::uint32_t to_rva) {
    return 0x94000000u | ((static_cast<std::uint32_t>(static_cast<std::int32_t>(to_rva - from_rva) / 4)) & 0x03FF'FFFFu);
}

void put(const FakeModule& m, std::uint32_t rva, std::initializer_list<std::uint32_t> words) {
    for (const std::uint32_t w : words) {
        std::memcpy(m.bytes() + rva, &w, 4);
        rva += 4;
    }
}

// A: add; bl B; add; ret     B: add; add; ret     C (right after B): add; ret
// D: add; bl E; ret          E: ret   (laid out before A: unreachable from A, B, C)
constexpr std::uint32_t kD = 0x0400, kE = 0x0800, kA = 0x1000, kB = 0x2000, kC = 0x2100;

void write_program(const FakeModule& m) {
    put(m, kA, {kAdd, bl(kA + 4, kB), kAdd, kRet});
    put(m, kB, {kAdd, kAdd, kRet});
    put(m, kC, {kAdd, kRet});
    put(m, kD, {kAdd, bl(kD + 4, kE), kRet});
    put(m, kE, {kRet});
}

void start_session(const TempDir& dir) {
    cache_detail::testing_reset(false);
    CacheConfig config;
    config.directory = dir.path();
    config.merge_always = true;
    config.writer_interval_ms = 20;
    cache_configure(config);
}

ExternalAotRequest request_for(const FakeModule& m, bool with_roots) {
    ExternalAotRequest r;
    r.base = m.base();
    r.size = m.size();
    for (const std::uint32_t f : {kA, kB, kC, kD, kE}) r.function_starts.push_back(m.base() + f);
    if (with_roots) r.roots.push_back(m.base() + kA);
    return r;
}

// The JIT's view of the module: its memory, function bounds from the starts.
class ModuleMemory final : public CodeSource {
public:
    explicit ModuleMemory(const FakeModule& m) : m_(m) {}
    bool read(std::uint64_t address, std::uint32_t& word) const override {
        if (address < m_.base() || address + 4 > m_.base() + m_.size()) return false;
        std::memcpy(&word, m_.bytes() + (address - m_.base()), 4);
        return true;
    }
    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override {
        static constexpr std::uint32_t starts[] = {kD, kE, kA, kB, kC, 0x5000};
        for (std::size_t i = 0; i + 1 < std::size(starts); ++i)
            if (address >= m_.base() + starts[i] && address < m_.base() + starts[i + 1]) {
                begin = m_.base() + starts[i];
                end = m_.base() + starts[i + 1];
                return true;
            }
        return false;
    }

private:
    const FakeModule& m_;
};

struct EnvScope {
    const wchar_t* name;
    EnvScope(const wchar_t* n, const wchar_t* value) : name(n) { SetEnvironmentVariableW(n, value); }
    ~EnvScope() { SetEnvironmentVariableW(name, nullptr); }
};

} // namespace

FX_TEST(external_aot_flow_ahead_serves_lookups_and_promotes_used_regions) {
    TempDir dir(L"xaot");
    FakeModule m(64 * 1024, 41);
    write_program(m);
    const GuestModule info = m.info(kImageHash, L"external.dylib");
    start_session(dir);
    cache_module_loaded(info);
    cache_detail::external_aot_start(request_for(m, true), &stub_translate);
    cache_detail::external_aot_testing_wait_idle(10000);
    // The root A, its call target B and the function laid out after it, C;
    // nothing reaches D or E yet.
    FX_CHECK_EQ(cache_external_aot_stats().regions, std::uint64_t{3});
    TranslatedRegion got;
    FX_CHECK(cache_lookup(m.base() + kB, got));
    FX_CHECK_EQ(got.guest_start, m.base() + kB);
    FX_CHECK_EQ(cache_external_aot_stats().hits, std::uint64_t{1});
    FX_CHECK_EQ(cache_stats().cold_promoted, std::uint64_t{1});   // to this process's journal
    FX_CHECK(!cache_lookup(m.base() + kD, got));

    // The guest JITs D: its call target E is translated ahead.
    cache_store(stub_translate(m.base() + kD, ModuleMemory(m), TranslateOptions{}));
    cache_detail::external_aot_testing_wait_idle(10000);
    FX_CHECK(cache_lookup(m.base() + kE, got));
    FX_CHECK_EQ(cache_external_aot_stats().hits, std::uint64_t{2});

    // Guest code changed after the translation: the record is not used.
    const auto before = cache_stats().guest_mismatches;
    put(m, kC, {kAdd, kAdd});
    FX_CHECK(!cache_lookup(m.base() + kC, got));
    FX_CHECK_EQ(cache_stats().guest_mismatches, before + 1);

    // The used regions reach the main file; a warm start translates nothing.
    cache_flush();
    FX_CHECK_EQ(dir.files(L"*.fxc").size(), std::size_t{1});
    start_session(dir);
    cache_module_loaded(info);
    FX_CHECK(cache_lookup(m.base() + kB, got));   // from the main file
    cache_detail::external_aot_start(request_for(m, true), &stub_translate);
    cache_detail::external_aot_testing_wait_idle(10000);
    FX_CHECK_EQ(cache_external_aot_stats().regions, std::uint64_t{0});
    cache_detail::testing_reset(false);
}

FX_TEST(external_aot_static_phase_writes_the_cold_file) {
    EnvScope delay(L"TRANSMUTE_FX_BACKGROUND_AOT_DELAY", L"0");
    TempDir dir(L"xaotstatic");
    FakeModule m(64 * 1024, 42);
    write_program(m);
    const GuestModule info = m.info(kImageHash ^ 1, L"static.dylib");
    start_session(dir);
    cache_module_loaded(info);
    cache_detail::external_aot_start(request_for(m, false), &stub_translate);
    for (int i = 0; i < 1000 && cache_external_aot_stats().cold_files == 0; ++i) Sleep(5);
    FX_CHECK_EQ(cache_external_aot_stats().cold_files, std::uint64_t{1});
    FX_CHECK_EQ(cache_external_aot_stats().regions, std::uint64_t{5});
    FX_CHECK_EQ(dir.files(L"*.fxa").size(), std::size_t{1});
    TranslatedRegion got;
    FX_CHECK(cache_lookup(m.base() + kD, got));   // the cold file, mapped by this process
    FX_CHECK(cache_stats().cold_hits >= 1);

    // A later process finds every function in the cold file and translates nothing.
    start_session(dir);
    cache_module_loaded(info);
    FX_CHECK(cache_lookup(m.base() + kE, got));
    FX_CHECK_EQ(cache_stats().cold_hits, std::uint64_t{1});
    cache_detail::external_aot_start(request_for(m, false), &stub_translate);
    cache_detail::external_aot_testing_wait_idle(10000);
    Sleep(50);
    cache_detail::external_aot_testing_wait_idle(10000);
    FX_CHECK_EQ(cache_external_aot_stats().regions, std::uint64_t{0});
    cache_detail::testing_reset(false);
}

FX_TEST(external_aot_disabled_by_environment) {
    EnvScope off(L"TRANSMUTE_FX_EXTERNAL_AOT", L"0");
    TempDir dir(L"xaotoff");
    FakeModule m(64 * 1024, 43);
    write_program(m);
    start_session(dir);
    cache_module_loaded(m.info(kImageHash ^ 2, L"off.dylib"));
    cache_detail::external_aot_start(request_for(m, true), &stub_translate);
    FX_CHECK(!cache_detail::external_aot_active());
    TranslatedRegion got;
    FX_CHECK(!cache_lookup(m.base() + kA, got));
    cache_detail::testing_reset(false);
}
