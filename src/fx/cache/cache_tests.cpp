// fx_cache_tests: persistent cache format, runtime load/lookup/store, journals,
// merging, integrity and concurrency (owner: cache).

#include "fx/cache/cache.hpp"
#include "fx/cache/cache_internal.hpp"
#include "fx/cache/format.hpp"
#include "fx/cache/pe_view.hpp"
#include "fx/cache/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::cache_test;
namespace fmt = transmute::fx::cachefmt;

namespace {

constexpr std::uint64_t kImageHash = 0x1234'5678'9abc'def1ULL;

// Starts a simulated process: no modules, fresh listing, private directory.
void start_session(const TempDir& dir, bool merge_always = true, std::uint64_t merge_min_bytes = 1u << 20,
                   bool verify = true) {
    cache_detail::testing_reset(false);
    CacheConfig config;
    config.directory = dir.path();
    config.merge_always = merge_always;
    config.merge_min_bytes = merge_min_bytes;
    config.writer_interval_ms = 20;
    config.verify_guest_code = verify;
    cache_configure(config);
}

struct Planned {
    std::uint32_t rva;
    std::uint32_t instructions;
};

std::vector<Planned> plan(std::uint32_t count, std::uint32_t first_rva, std::uint32_t stride) {
    std::vector<Planned> out;
    for (std::uint32_t i = 0; i < count; ++i) out.push_back({first_rva + i * stride, 4 + (i * 7) % 60});
    return out;
}

void store_all(const FakeModule& m, const std::vector<Planned>& regions, std::uint32_t seed) {
    for (const Planned& p : regions) cache_store(make_region(m.base(), p.rva, p.instructions, seed));
}

// Every entry of every planned region must hit and equal the expected rebase.
unsigned check_all(const FakeModule& m, const std::vector<Planned>& regions, std::uint32_t seed,
                   unsigned* misses = nullptr) {
    unsigned hits = 0, missed = 0;
    TranslatedRegion got;
    for (const Planned& p : regions) {
        const TranslatedRegion expected = make_region(m.base(), p.rva, p.instructions, seed);
        for (const RegionEntry& e : expected.entries) {
            if (!cache_lookup(e.guest_pc, got)) {
                ++missed;
                continue;
            }
            // A mid-region entry may resolve to a region that starts there; ours never overlap.
            FX_CHECK(regions_equal(got, expected));
            ++hits;
        }
    }
    if (misses) *misses = missed;
    return hits;
}

std::size_t entry_count(const FakeModule& m, const std::vector<Planned>& regions, std::uint32_t seed) {
    std::size_t n = 0;
    for (const Planned& p : regions) n += make_region(m.base(), p.rva, p.instructions, seed).entries.size();
    return n;
}

std::vector<std::uint8_t> read_file(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

void write_file(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::uint64_t encode_with_hash(const FakeModule& m, const TranslatedRegion& r, std::vector<std::uint8_t>& out) {
    std::vector<fmt::DiskRange> ranges;
    for (const auto& [b, e] : r.guest_ranges)
        ranges.push_back({static_cast<std::uint32_t>(b - m.base()), static_cast<std::uint32_t>(e - m.base())});
    std::uint64_t hash = 0;
    cache_detail::guest_memory_hash(m.base(), ranges.data(), static_cast<std::uint32_t>(ranges.size()), hash);
    fmt::encode_record(r, m.base(), m.size(), hash, out);
    return hash;
}

} // namespace

FX_TEST(cache_hash64_reference) {
    // xxHash64 reference value for the empty input, seed 0.
    FX_CHECK_EQ(fmt::hash64("", 0, 0), 0xEF46DB3751D8E999ULL);
    std::vector<std::uint8_t> data(1000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 31);
    const std::uint64_t h = fmt::hash64(data.data(), data.size(), 5);
    data[500] ^= 1;
    FX_CHECK(fmt::hash64(data.data(), data.size(), 5) != h);
}

FX_TEST(cache_record_round_trip_and_rebase) {
    FakeModule m(1u << 20, 1);
    FakeModule other(m, 0);
    for (std::uint32_t i = 0; i < 300; ++i) {
        const std::uint32_t rva = 0x400 + i * 0x800;
        const TranslatedRegion r = make_region(m.base(), rva, 4 + i % 70, 9);
        std::vector<std::uint8_t> bytes;
        const std::uint64_t hash = encode_with_hash(m, r, bytes);
        FX_REQUIRE(!bytes.empty());
        fmt::RecordView view;
        FX_REQUIRE(fmt::decode_record(bytes.data(), bytes.size(), true, view));
        FX_CHECK_EQ(view.header->guest_hash, hash);
        FX_CHECK_EQ(view.header->start_rva, rva);
        TranslatedRegion same, moved;
        fmt::rebuild_region(view, m.base(), same);
        fmt::rebuild_region(view, other.base(), moved);
        FX_CHECK(regions_equal(same, r));
        FX_CHECK(regions_equal(moved, make_region(other.base(), rva, 4 + i % 70, 9)));
        // Stored code is base independent: fixup slots hold RVAs.
        for (std::uint32_t f = 0; f < view.header->n_fixups; ++f) {
            std::uint64_t slot = 0;
            std::memcpy(&slot, view.code + view.fixups[f].offset, 8);
            FX_CHECK_EQ(slot, view.fixups[f].rva);
        }
    }
}

FX_TEST(cache_record_rejects_every_corruption) {
    FakeModule m(1u << 16, 2);
    const TranslatedRegion r = make_region(m.base(), 0x1000, 12, 3);
    std::vector<std::uint8_t> bytes;
    encode_with_hash(m, r, bytes);
    FX_REQUIRE(!bytes.empty());
    fmt::RecordView view;
    unsigned accepted = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        for (std::uint8_t bit : {std::uint8_t{1}, std::uint8_t{0x80}}) {
            std::vector<std::uint8_t> copy = bytes;
            copy[i] ^= bit;
            if (fmt::decode_record(copy.data(), copy.size(), true, view)) ++accepted;
        }
    }
    FX_CHECK_EQ(accepted, 0u);
    for (std::size_t size = 0; size < bytes.size(); ++size)
        FX_CHECK(!fmt::decode_record(bytes.data(), size, true, view));
    // Unencodable regions: addresses outside the module, missing entries.
    TranslatedRegion bad = r;
    bad.exits[0].target_pc = m.base() + m.size() + 0x1000;
    std::vector<std::uint8_t> out;
    FX_CHECK(!fmt::encode_record(bad, m.base(), m.size(), 0, out));
    bad = r;
    bad.fixups[0].value = m.base() - 8;
    FX_CHECK(!fmt::encode_record(bad, m.base(), m.size(), 0, out));
    bad = r;
    bad.entries.clear();
    FX_CHECK(!fmt::encode_record(bad, m.base(), m.size(), 0, out));
    bad = r;
    bad.fixups[0].offset = static_cast<std::uint32_t>(bad.code.size() - 4);
    FX_CHECK(!fmt::encode_record(bad, m.base(), m.size(), 0, out));
    FX_CHECK(out.empty());
}

FX_TEST(cache_round_trip_relocated_module) {
    TempDir dir(L"roundtrip");
    start_session(dir);
    FakeModule a(4u << 20, 4);
    const auto regions = plan(500, 0x1000, 0x2000);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, regions, 11);
    cache_flush();
    FX_CHECK_EQ(dir.files(L"*.fxc").size(), std::size_t{1});
    FX_CHECK_EQ(dir.files(L"*.fxj").size(), std::size_t{0});
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{500});

    start_session(dir);
    FakeModule b(a, 0);  // same image at another base
    FX_CHECK(a.base() != b.base());
    cache_module_loaded(b.info(kImageHash));
    unsigned misses = 0;
    const unsigned hits = check_all(b, regions, 11, &misses);
    FX_CHECK_EQ(cache_stats().modules_with_cache, std::uint64_t{1});
    FX_CHECK_EQ(hits, static_cast<unsigned>(entry_count(b, regions, 11)));
    FX_CHECK_EQ(misses, 0u);
    TranslatedRegion got;
    FX_CHECK(!cache_lookup(b.base() + 0x1100, got));          // inside a region, not an entry
    FX_CHECK(!cache_lookup(a.base() + 0x1000, got));          // old base: not a module any more
    FX_CHECK(!cache_lookup(b.base() + b.size() + 0x1000, got));
    // A different image hash maps a different file.
    start_session(dir);
    cache_module_loaded(b.info(kImageHash + 1));
    FX_CHECK(!cache_lookup(b.base() + 0x1000, got));
    cache_detail::testing_reset(false);
}

// Main files are shared read-only file views, not private copies: every
// process using a module shares the file's pages. A merge renames a new main
// file over a mapped one; the open view keeps the old records.
FX_TEST(cache_main_file_is_a_shared_view) {
    TempDir dir(L"mainview");
    start_session(dir);
    FakeModule a(4u << 20, 6);
    const auto regions = plan(200, 0x1000, 0x2000);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, regions, 21);
    cache_flush();

    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    FX_CHECK_EQ(check_all(a, regions, 21), static_cast<unsigned>(entry_count(a, regions, 21)));
    const std::uint8_t* data = nullptr;
    FX_CHECK_EQ(cache_detail::module_cache_state(a.base(), nullptr, nullptr, nullptr, &data), 1);
    FX_CHECK(data != nullptr);
    MEMORY_BASIC_INFORMATION info{};
    FX_CHECK(VirtualQuery(data, &info, sizeof(info)) == sizeof(info));
    FX_CHECK_EQ(info.Type, static_cast<DWORD>(MEM_MAPPED));
    FX_CHECK_EQ(info.Protect, static_cast<DWORD>(PAGE_READONLY));
    // Another process's merge replaces the file under the open view.
    const auto mains = dir.files(L"*.fxc");
    FX_CHECK_EQ(mains.size(), std::size_t{1});
    const std::wstring main = dir.path() + L"\\" + mains[0], temp = main + L".replacement";
    write_file(temp, read_file(main));
    FX_CHECK(MoveFileExW(temp.c_str(), main.c_str(), MOVEFILE_REPLACE_EXISTING) != 0);
    FX_CHECK_EQ(check_all(a, regions, 21), static_cast<unsigned>(entry_count(a, regions, 21)));
    cache_detail::testing_reset(false);
}

FX_TEST(cache_journals_are_indexed_then_merged) {
    TempDir dir(L"journal");
    FakeModule a(4u << 20, 5);
    const auto first = plan(100, 0x1000, 0x4000);
    const auto second = plan(100, 0x3000, 0x4000);

    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, first, 1);
    cache_flush();  // no main file yet: always merged
    FX_CHECK_EQ(dir.files(L"*.fxc").size(), std::size_t{1});
    FX_CHECK_EQ(dir.files(L"*.fxj").size(), std::size_t{0});

    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    FX_CHECK_EQ(check_all(a, first, 1), static_cast<unsigned>(entry_count(a, first, 1)));
    store_all(a, first, 1);  // already cached: ignored, nothing new to write
    store_all(a, second, 1);
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{100});
    cache_flush();  // small journal next to a main file: kept as a side file
    FX_CHECK_EQ(dir.files(L"*.fxj").size(), std::size_t{1});

    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    unsigned misses = 0;
    FX_CHECK_EQ(check_all(a, first, 1), static_cast<unsigned>(entry_count(a, first, 1)));
    FX_CHECK_EQ(check_all(a, second, 1, &misses), static_cast<unsigned>(entry_count(a, second, 1)));
    FX_CHECK_EQ(misses, 0u);

    start_session(dir, true);
    cache_module_loaded(a.info(kImageHash));
    cache_flush();  // forced merge folds the journal in
    FX_CHECK_EQ(dir.files(L"*.fxj").size(), std::size_t{0});
    FX_CHECK_EQ(cache_stats().merges, std::uint64_t{1});

    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    FX_CHECK_EQ(check_all(a, first, 1) + check_all(a, second, 1),
                static_cast<unsigned>(entry_count(a, first, 1) + entry_count(a, second, 1)));
    cache_detail::testing_reset(false);
}

FX_TEST(cache_crash_torn_tail_and_live_journal) {
    TempDir dir(L"crash");
    FakeModule a(4u << 20, 6);
    const auto first = plan(50, 0x1000, 0x8000);
    const auto second = plan(50, 0x5000, 0x8000);
    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, first, 2);
    cache_flush();

    // Process 2 stores; the background writer appends; then it "crashes".
    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, second, 2);
    for (int i = 0; i < 400 && cache_stats().journal_bytes == 0; ++i) Sleep(5);
    FX_CHECK(cache_stats().journal_bytes > 0);
    for (int i = 0; i < 400 && cache_stats().journal_bytes < 50 * 72; ++i) Sleep(5);
    const std::uint64_t written = cache_stats().journal_bytes;
    cache_detail::testing_reset(false);  // crash: handles closed by the "OS", pending dropped
    auto journals = dir.files(L"*.fxj");
    FX_REQUIRE(journals.size() == 1);
    const std::wstring journal_path = dir.path() + L"\\" + journals[0];
    // Torn tail: a record header that promises more bytes than exist.
    {
        std::vector<std::uint8_t> bytes = read_file(journal_path);
        FX_CHECK_EQ(bytes.size(), sizeof(fmt::FileHeader) + written);
        const std::uint32_t torn[] = {fmt::kRecordMagic, 4096, 0xdeadbeef, 0x1234};
        bytes.insert(bytes.end(), reinterpret_cast<const std::uint8_t*>(torn),
                     reinterpret_cast<const std::uint8_t*>(torn) + sizeof(torn));
        write_file(journal_path, bytes);
    }

    start_session(dir, true);
    cache_module_loaded(a.info(kImageHash));
    FX_CHECK_EQ(check_all(a, first, 2), static_cast<unsigned>(entry_count(a, first, 2)));
    unsigned misses = 0;
    check_all(a, second, 2, &misses);
    FX_CHECK_EQ(misses, 0u);  // all 50 were written before the crash

    // A journal still open for writing by another (live) process is not merged.
    const std::wstring stem = cache_file_path(a.info(kImageHash)).substr(0, cache_file_path(a.info(kImageHash)).size() - 4);
    const std::wstring live_path = stem + L".jdeadbeef-0000-000000.fxj";
    std::vector<std::uint8_t> live_bytes = read_file(journal_path);
    HANDLE live = CreateFileW(live_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              CREATE_NEW, 0, nullptr);
    FX_REQUIRE(live != INVALID_HANDLE_VALUE);
    DWORD done = 0;
    WriteFile(live, live_bytes.data(), static_cast<DWORD>(live_bytes.size()), &done, nullptr);
    cache_flush();
    auto left = dir.files(L"*.fxj");
    FX_CHECK_EQ(left.size(), std::size_t{1});
    FX_CHECK(left.size() == 1 && left[0] == L"" + live_path.substr(live_path.find_last_of(L'\\') + 1));
    CloseHandle(live);
    cache_flush();  // owner gone: merged now
    FX_CHECK_EQ(dir.files(L"*.fxj").size(), std::size_t{0});

    start_session(dir, true);
    cache_module_loaded(a.info(kImageHash));
    FX_CHECK_EQ(check_all(a, first, 2) + check_all(a, second, 2),
                static_cast<unsigned>(entry_count(a, first, 2) + entry_count(a, second, 2)));
    cache_detail::testing_reset(false);
}

FX_TEST(cache_rejects_corrupt_truncated_and_mismatched_files) {
    TempDir dir(L"corrupt");
    FakeModule a(4u << 20, 7);
    const auto regions = plan(200, 0x1000, 0x4000);
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, regions, 3);
    cache_flush();
    const std::wstring path = cache_file_path(a.info(kImageHash));
    const std::vector<std::uint8_t> good = read_file(path);
    FX_REQUIRE(good.size() > sizeof(fmt::FileHeader));
    const std::size_t total_entries = entry_count(a, regions, 3);

    auto run_with = [&](const std::vector<std::uint8_t>& bytes, unsigned* misses) {
        cache_detail::testing_reset(false);
        write_file(path, bytes);
        start_session(dir);
        cache_module_loaded(a.info(kImageHash));
        return check_all(a, regions, 3, misses);  // every hit is checked for equality
    };
    unsigned misses = 0;
    FX_CHECK_EQ(run_with(good, &misses), static_cast<unsigned>(total_entries));

    // Truncation anywhere rejects the whole file (exact size is in the header).
    for (std::size_t size : {good.size() - 1, good.size() / 2, sizeof(fmt::FileHeader), std::size_t{100}}) {
        std::vector<std::uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size));
        FX_CHECK_EQ(run_with(cut, &misses), 0u);
        FX_CHECK(cache_stats().files_rejected >= 1);
    }
    // Header corruption.
    std::vector<std::uint8_t> bad = good;
    bad[40] ^= 0x10;
    FX_CHECK_EQ(run_with(bad, &misses), 0u);
    // Version mismatch with a self-consistent header (e.g. older translator ABI).
    for (int field = 0; field < 3; ++field) {
        bad = good;
        fmt::FileHeader h;
        std::memcpy(&h, bad.data(), sizeof(h));
        if (field == 0) h.key.abi_version += 1;
        if (field == 1) h.key.format_version += 1;
        if (field == 2) h.key.cpu_features ^= 1;
        h.header_checksum = fmt::header_checksum(h);
        std::memcpy(bad.data(), &h, sizeof(h));
        FX_CHECK_EQ(run_with(bad, &misses), 0u);
    }
    // One corrupt record: only that region is lost.
    {
        fmt::MainView view;
        fmt::FileHeader h;
        std::memcpy(&h, good.data(), sizeof(h));
        FX_REQUIRE(fmt::open_main(good.data(), good.size(), h.key, view));
        const std::size_t victim = static_cast<std::size_t>(view.records_offset) + 200;  // inside record 0
        bad = good;
        bad[victim] ^= 0x01;
        const unsigned hits = run_with(bad, &misses);
        FX_CHECK(hits > 0 && misses > 0 && misses <= 4);
        FX_CHECK(cache_stats().records_rejected >= 1);
        // Garbage index: lookups may miss, never return a wrong region.
        bad = good;
        for (std::uint64_t i = h.index_offset; i < h.bucket_offset; ++i) bad[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i * 7);
        run_with(bad, &misses);
        bad = good;
        for (std::uint64_t i = h.bucket_offset; i < h.total_size; ++i) bad[static_cast<std::size_t>(i)] = 0xFF;
        run_with(bad, &misses);
    }
    // Fuzz: random flips and truncations never crash and never yield a wrong region.
    std::mt19937 rng(77);
    for (int iteration = 0; iteration < 150; ++iteration) {
        bad = good;
        const int flips = 1 + static_cast<int>(rng() % 8);
        for (int f = 0; f < flips; ++f) bad[rng() % bad.size()] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
        if (rng() % 4 == 0) bad.resize(sizeof(fmt::FileHeader) + rng() % (bad.size() - sizeof(fmt::FileHeader)));
        run_with(bad, &misses);
    }
    // Garbage that is not a cache file at all.
    bad.assign(5000, 0x5A);
    FX_CHECK_EQ(run_with(bad, &misses), 0u);
    cache_detail::testing_reset(false);
}

FX_TEST(cache_detects_changed_guest_code) {
    TempDir dir(L"guest");
    FakeModule a(1u << 20, 8);
    const auto regions = plan(40, 0x1000, 0x4000);
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, regions, 4);
    cache_flush();

    FakeModule patched(a, 0);
    patched.bytes()[regions[5].rva + 8] ^= 0xFF;  // inside region 5's guest range
    start_session(dir);
    cache_module_loaded(patched.info(kImageHash));
    TranslatedRegion got;
    FX_CHECK(!cache_lookup(patched.base() + regions[5].rva, got));
    FX_CHECK(cache_stats().guest_mismatches >= 1);
    FX_CHECK(cache_lookup(patched.base() + regions[6].rva, got));
    // The JIT retranslates and stores the new version; it wins from then on.
    cache_store(make_region(patched.base(), regions[5].rva, regions[5].instructions, 4));
    cache_flush();
    start_session(dir);
    cache_module_loaded(patched.info(kImageHash));
    FX_CHECK(cache_lookup(patched.base() + regions[5].rva, got));
    // Without verification the stale region would have been reused.
    start_session(dir, true, 1u << 20, false);
    FakeModule original(a, 0);
    cache_module_loaded(original.info(kImageHash));
    FX_CHECK(cache_lookup(original.base() + regions[5].rva, got));
    cache_detail::testing_reset(false);
}

FX_TEST(cache_store_ignores_foreign_and_malformed_regions) {
    TempDir dir(L"ignore");
    start_session(dir);
    FakeModule a(1u << 20, 9);
    FakeModule outside(1u << 16, 10);
    cache_module_loaded(a.info(kImageHash));
    cache_store(make_region(outside.base(), 0x100, 8, 1));  // not a guest module
    TranslatedRegion r = make_region(a.base(), 0x1000, 8, 1);
    r.exits[1].target_pc = a.base() + a.size() + 0x10000;  // leaves the module
    cache_store(r);
    r = make_region(a.base(), 0x2000, 8, 1);
    r.entries[0].guest_pc += 4;  // entries[0] must be the region start
    cache_store(r);
    r = make_region(a.base(), 0x3000, 8, 1);
    r.guest_ranges.push_back({a.base() + a.size() - 4, a.base() + a.size() + 64});
    cache_store(r);
    cache_store(make_region(a.base(), 0x4000, 8, 1));
    cache_store(make_region(a.base(), 0x4000, 8, 1));  // duplicate
    const CacheStats s = cache_stats();
    FX_CHECK_EQ(s.stores, std::uint64_t{6});
    FX_CHECK_EQ(s.records_queued, std::uint64_t{1});
    FX_CHECK_EQ(s.stores_ignored, std::uint64_t{5});
    cache_module_unloaded(a.info(kImageHash));
    TranslatedRegion got;
    FX_CHECK(!cache_lookup(a.base() + 0x4000, got));
    cache_store(make_region(a.base(), 0x5000, 8, 1));  // after unload: ignored
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{1});
    // Unload wrote the pending region.
    cache_flush();
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    FX_CHECK(cache_lookup(a.base() + 0x4000, got));
    cache_detail::testing_reset(false);
}

namespace {
// make_region plus a movabs of a module address with no fixup at code offset 100.
TranslatedRegion unrelocatable_region(const FakeModule& m, std::uint32_t rva) {
    TranslatedRegion r = make_region(m.base(), rva, 16, 1);
    const std::uint64_t address = m.base() + 0x2340;
    std::memcpy(r.code.data() + 100, &address, 8);
    return r;
}
} // namespace

FX_TEST(cache_pins_code_with_unrecorded_addresses) {
    TempDir dir(L"pinned");
    FakeModule a(1u << 20, 17);
    FakeModule b(a, 0);  // same image, another base
    TranslatedRegion r = make_region(a.base(), 0x1000, 16, 1);
    FX_CHECK_EQ(fmt::unrecorded_address(r, a.base(), a.size()), std::int64_t{-1});
    r = unrelocatable_region(a, 0x1000);
    FX_CHECK_EQ(fmt::unrecorded_address(r, a.base(), a.size()), std::int64_t{100});
    // Recorded as an address fixup it would be relocatable.
    TranslatedRegion fixed = r;
    fixed.fixups.push_back({100, FixupKind::guest_address_imm64, a.base() + 0x2340});
    FX_CHECK_EQ(fmt::unrecorded_address(fixed, a.base(), a.size()), std::int64_t{-1});

    // Run 1 (module at a): the JIT translates it; it is stored pinned to a.
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    cache_store(r);
    FX_CHECK_EQ(cache_stats().stores_unrelocatable, std::uint64_t{1});
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{1});
    cache_flush();

    // Run 2, same base: reused byte for byte.
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    TranslatedRegion got;
    FX_CHECK(cache_lookup(a.base() + 0x1000, got));
    FX_CHECK(regions_equal(got, r));
    cache_store(r);  // already cached at this base: nothing new
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{0});

    // Run 3, another base: a miss, so the JIT translates again; that version is
    // stored (pinned to b) instead of being deduplicated against a's copy.
    start_session(dir);
    cache_module_loaded(b.info(kImageHash));
    FX_CHECK(!cache_lookup(b.base() + 0x1000, got));
    FX_CHECK(cache_stats().pinned_mismatches >= 1);
    const TranslatedRegion at_b = unrelocatable_region(b, 0x1000);
    cache_store(at_b);
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{1});
    cache_flush();

    // Run 4 at b: hit. The newer (b) version replaced a's.
    start_session(dir);
    cache_module_loaded(b.info(kImageHash));
    FX_CHECK(cache_lookup(b.base() + 0x1000, got));
    FX_CHECK(regions_equal(got, at_b));
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    FX_CHECK(!cache_lookup(a.base() + 0x1000, got));
    cache_detail::testing_reset(false);
}

FX_TEST(cache_unrecorded_address_matches_a_byte_scan) {
    // Reference: every 8-byte window not overlapping a fixup slot.
    const auto reference = [](const TranslatedRegion& r, std::uint64_t base, std::uint64_t span) -> std::int64_t {
        for (std::size_t at = 0; at + 8 <= r.code.size(); ++at) {
            bool fixup = false;
            for (const CodeFixup& f : r.fixups) fixup |= f.offset < at + 8 && at < std::size_t{f.offset} + 8;
            if (fixup) continue;
            std::uint64_t value;
            std::memcpy(&value, r.code.data() + at, 8);
            if (value - base <= span) return static_cast<std::int64_t>(at);
        }
        return -1;
    };
    std::mt19937_64 rng(7);
    unsigned found = 0;
    for (int round = 0; round < 3000; ++round) {
        // Bases just below a 4 GiB boundary too: the range then has two high dwords.
        const std::uint64_t base = (round % 3 == 0) ? 0x7ff6'ffff'f000ull : 0x7ff6'1234'0000ull + (rng() & 0xfff000);
        const std::uint64_t span = 0x10000 + (rng() & 0xfffff);
        TranslatedRegion r;
        r.code.resize(16 + rng() % 400);
        for (auto& b : r.code) b = static_cast<std::uint8_t>(rng() % 5 == 0 ? (base >> 32) & 0xff : rng());
        const unsigned plants = rng() % 4;
        for (unsigned i = 0; i < plants; ++i) {
            const std::size_t at = rng() % (r.code.size() - 7);
            const std::uint64_t value = base + rng() % (span + 2);   // sometimes one past the end
            std::memcpy(r.code.data() + at, &value, 8);
            if (rng() % 2) r.fixups.push_back({static_cast<std::uint32_t>(at), FixupKind::guest_address_imm64, value});
        }
        const std::int64_t want = reference(r, base, span);
        FX_CHECK_EQ(fmt::unrecorded_address(r, base, span), want);
        found += want >= 0;
    }
    FX_CHECK(found > 500);
}

FX_TEST(cache_literal_fixups_are_never_rebased) {
    TempDir dir(L"literal");
    FakeModule a(1u << 20, 18);
    FakeModule b(a, 0);
    // A guest constant that happens to look like an address of the module at a.
    const std::uint64_t constant = a.base() + 0x2340;
    auto with_literal = [&](const FakeModule& m) {
        TranslatedRegion r = make_region(m.base(), 0x1000, 16, 1);
        std::memcpy(r.code.data() + 100, &constant, 8);
        r.fixups.push_back({100, FixupKind::literal_imm64, constant});
        return r;
    };
    const TranslatedRegion r = with_literal(a);
    FX_CHECK_EQ(fmt::unrecorded_address(r, a.base(), a.size()), std::int64_t{-1});  // its window is excluded
    std::vector<std::uint8_t> bytes;
    FX_REQUIRE(fmt::encode_record(r, a.base(), a.size(), 0, bytes));
    fmt::RecordView view;
    FX_REQUIRE(fmt::decode_record(bytes.data(), bytes.size(), true, view));
    FX_CHECK_EQ(view.header->flags, 0u);  // relocatable
    std::uint64_t slot = 0;
    std::memcpy(&slot, view.code + 100, 8);
    FX_CHECK_EQ(slot, constant);  // stored as-is, not as an RVA

    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    cache_store(r);
    FX_CHECK_EQ(cache_stats().stores_unrelocatable, std::uint64_t{0});
    cache_flush();
    start_session(dir);
    cache_module_loaded(b.info(kImageHash));
    TranslatedRegion got;
    FX_REQUIRE(cache_lookup(b.base() + 0x1000, got));
    FX_CHECK(regions_equal(got, with_literal(b)));  // addresses rebased, the constant unchanged
    std::memcpy(&slot, got.code.data() + 100, 8);
    FX_CHECK_EQ(slot, constant);
    FX_CHECK(got.fixups.back().kind == FixupKind::literal_imm64 && got.fixups.back().value == constant);
    cache_detail::testing_reset(false);
}

FX_TEST(cache_disabled_is_a_no_op) {
    TempDir dir(L"disabled");
    cache_detail::testing_reset(false);
    CacheConfig config;
    config.directory = dir.path();
    config.enabled = false;
    cache_configure(config);
    FakeModule a(1u << 20, 11);
    cache_module_loaded(a.info(kImageHash));
    cache_store(make_region(a.base(), 0x1000, 8, 1));
    cache_flush();
    TranslatedRegion got;
    FX_CHECK(!cache_lookup(a.base() + 0x1000, got));
    FX_CHECK(dir.files().empty());
    FX_CHECK_EQ(cache_stats().modules_loaded, std::uint64_t{0});
    start_session(dir);
}

FX_TEST(cache_image_hash_file_matches_mapped_headers) {
    // Our own x64 executable: the Windows loader rewrote ImageBase in memory.
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    pe::MappedFile file;
    FX_REQUIRE(file.open(self, false));
    const auto* image = reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr));
    FX_CHECK_EQ(compute_image_hash(file.data(), file.size()), compute_image_hash(image, 4096));
    std::vector<std::uint8_t> copy(file.data(), file.data() + 4096);
    copy[0x200] ^= 1;  // inside the section table
    FX_CHECK(compute_image_hash(copy.data(), copy.size()) != compute_image_hash(file.data(), file.size()));
    FX_CHECK(compute_image_hash("garbage", 7) != 0);
}

FX_TEST(cache_concurrent_store_lookup_and_module_churn) {
    TempDir dir(L"concurrent");
    FakeModule a(16u << 20, 12);
    const auto cached = plan(4000, 0x1000, 0x1000);
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, cached, 5);
    cache_flush();

    start_session(dir);
    FakeModule b(8u << 20, 13);
    cache_module_loaded(a.info(kImageHash));
    cache_module_loaded(b.info(kImageHash + 7, L"b.dll"));
    std::atomic<unsigned> wrong{0}, lookups_done{0}, stores_done{0}, churn{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng(t);
            TranslatedRegion got;
            for (int i = 0; i < 20000; ++i) {
                const Planned& p = cached[rng() % cached.size()];
                if (!cache_lookup(a.base() + p.rva, got) ||
                    !regions_equal(got, make_region(a.base(), p.rva, p.instructions, 5)))
                    ++wrong;
                ++lookups_done;
            }
        });
    }
    for (unsigned t = 0; t < 4; ++t) {
        threads.emplace_back([&, t] {
            for (std::uint32_t i = 0; i < 1000; ++i) {
                const std::uint32_t rva = 0x1000 + (i * 4 + t) * 0x800;
                cache_store(make_region(b.base(), rva, 4 + i % 40, 6));
                cache_store(make_region(b.base(), rva, 4 + i % 40, 6));  // duplicate
                ++stores_done;
            }
        });
    }
    threads.emplace_back([&] {
        FakeModule c(1u << 20, 14);
        TranslatedRegion got;
        while (!stop.load()) {
            cache_module_loaded(c.info(kImageHash + 9, L"c.dll"));
            if (cache_lookup(c.base() + 0x100, got)) ++wrong;
            cache_store(make_region(c.base(), 0x1000, 8, 1));
            cache_module_unloaded(c.info(kImageHash + 9, L"c.dll"));
            ++churn;
        }
    });
    for (std::size_t i = 0; i + 1 < threads.size(); ++i) threads[i].join();
    stop.store(true);
    threads.back().join();
    FX_CHECK_EQ(wrong.load(), 0u);
    FX_CHECK_EQ(lookups_done.load(), 80000u);
    FX_CHECK_EQ(stores_done.load(), 4000u);
    FX_CHECK(churn.load() > 0);
    std::printf("  concurrent: 80000 lookups, 8000 stores (4000 unique), %u module load/unload cycles\n",
                churn.load());
    cache_flush();

    start_session(dir);
    cache_module_loaded(b.info(kImageHash + 7, L"b.dll"));
    unsigned hits = 0;
    TranslatedRegion got;
    for (unsigned t = 0; t < 4; ++t)
        for (std::uint32_t i = 0; i < 1000; ++i) {
            const std::uint32_t rva = 0x1000 + (i * 4 + t) * 0x800;
            if (cache_lookup(b.base() + rva, got) && regions_equal(got, make_region(b.base(), rva, 4 + i % 40, 6)))
                ++hits;
        }
    FX_CHECK_EQ(hits, 4000u);
    cache_detail::testing_reset(false);
}

FX_TEST(cache_measure_costs) {
    TempDir dir(L"perf");
    FakeModule a(64u << 20, 15);
    const auto regions = plan(10000, 0x1000, 0x1800);
    start_session(dir);
    cache_module_loaded(a.info(kImageHash));
    std::vector<TranslatedRegion> made;
    made.reserve(regions.size());
    std::uint64_t code_bytes = 0, entries = 0;
    for (const Planned& p : regions) {
        made.push_back(make_region(a.base(), p.rva, 16 + p.instructions, 21));
        code_bytes += made.back().code.size();
        entries += made.back().entries.size();
    }
    double t0 = now_seconds();
    for (const TranslatedRegion& r : made) cache_store(r);
    const double store_ns = (now_seconds() - t0) * 1e9 / static_cast<double>(made.size());
    t0 = now_seconds();
    cache_flush();
    const double flush_ms = (now_seconds() - t0) * 1e3;
    const std::uint64_t file_size = read_file(cache_file_path(a.info(kImageHash))).size();

    start_session(dir);
    TranslatedRegion got;
    t0 = now_seconds();
    cache_module_loaded(a.info(kImageHash));
    const double load_us = (now_seconds() - t0) * 1e6;
    t0 = now_seconds();
    FX_CHECK(cache_lookup(made[0].guest_start, got));  // waits for the asynchronous open
    const double first_hit_us = (now_seconds() - t0) * 1e6;
    FakeModule none(1u << 20, 16);
    t0 = now_seconds();
    cache_module_loaded(none.info(kImageHash + 1, L"none.dll"));
    const double load_none_us = (now_seconds() - t0) * 1e6;
    // Steady state: listing taken, file in the OS cache (what each of ~200 DLLs costs at startup).
    double reload_total = 0, reopen_total = 0;
    constexpr int kReloads = 50;
    for (int i = 0; i < kReloads; ++i) {
        cache_module_unloaded(a.info(kImageHash));
        t0 = now_seconds();
        cache_module_loaded(a.info(kImageHash));
        reload_total += now_seconds() - t0;
        t0 = now_seconds();
        cache_lookup(made[i].guest_start, got);
        reopen_total += now_seconds() - t0;
    }
    cache_module_unloaded(a.info(kImageHash));
    cache_module_loaded(a.info(kImageHash));
    Sleep(50);  // let the thread pool finish the open: the common case at startup

    std::vector<std::uint64_t> pcs;
    for (const TranslatedRegion& r : made)
        for (const RegionEntry& e : r.entries) pcs.push_back(e.guest_pc);
    std::mt19937 rng(3);
    std::shuffle(pcs.begin(), pcs.end(), rng);
    unsigned hits = 0;
    t0 = now_seconds();
    for (std::size_t i = 0; i < pcs.size(); ++i) hits += cache_lookup(pcs[i], got) ? 1 : 0;
    const double cold_ns = (now_seconds() - t0) * 1e9 / static_cast<double>(pcs.size());
    FX_CHECK_EQ(hits, static_cast<unsigned>(pcs.size()));
    hits = 0;
    constexpr unsigned kLookups = 400000;
    t0 = now_seconds();
    for (unsigned i = 0; i < kLookups; ++i) hits += cache_lookup(pcs[i % pcs.size()], got) ? 1 : 0;
    const double lookup_ns = (now_seconds() - t0) * 1e9 / kLookups;
    FX_CHECK_EQ(hits, kLookups);
    t0 = now_seconds();
    unsigned miss_hits = 0;
    for (unsigned i = 0; i < kLookups; ++i) miss_hits += cache_lookup(pcs[i % pcs.size()] + 4, got) ? 1 : 0;
    const double miss_ns = (now_seconds() - t0) * 1e9 / kLookups;

    std::atomic<unsigned> mt_hits{0};
    t0 = now_seconds();
    {
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < 8; ++t)
            threads.emplace_back([&, t] {
                TranslatedRegion local;
                unsigned h = 0;
                for (unsigned i = t; i < kLookups; i += 8) h += cache_lookup(pcs[i % pcs.size()], local) ? 1 : 0;
                mt_hits += h;
            });
        for (auto& t : threads) t.join();
    }
    const double mt_ns = (now_seconds() - t0) * 1e9 / kLookups;
    FX_CHECK_EQ(mt_hits.load(), kLookups);

    start_session(dir, true, 1u << 20, false);
    cache_module_loaded(a.info(kImageHash));
    t0 = now_seconds();
    for (unsigned i = 0; i < kLookups; ++i) cache_lookup(pcs[i % pcs.size()], got);
    const double noverify_ns = (now_seconds() - t0) * 1e9 / kLookups;

    std::printf("  file: %zu regions, %llu entries, %.1f MiB code, %.1f MiB file\n", made.size(),
                static_cast<unsigned long long>(entries), static_cast<double>(code_bytes) / 1048576.0,
                static_cast<double>(file_size) / 1048576.0);
    std::printf("  cache_store %.0f ns/region (queue only); cache_flush (journal write + merge) %.1f ms\n", store_ns,
                flush_ms);
    std::printf("  cache_module_loaded (loader thread): %.1f us with file, %.1f us without; steady %.1f us\n",
                load_us, load_none_us, reload_total * 1e6 / kReloads);
    std::printf("  first lookup right after load (waits for the open: listing, map, validate): %.0f us cold, "
                "%.0f us steady\n",
                first_hit_us, reopen_total * 1e6 / kReloads);
    std::printf("  cache_lookup hit: %.0f ns first touch of each entry, %.0f ns repeated (verify on), %.0f ns "
                "(verify off); miss %.0f ns; 8 threads %.0f ns/lookup aggregate\n",
                cold_ns, lookup_ns, noverify_ns, miss_ns, mt_ns);
    (void)miss_hits;
    cache_detail::testing_reset(false);
}


namespace {
std::atomic<std::uint64_t> g_ready_base{0};
std::atomic<int> g_ready_calls{0};
void record_ready(std::uint64_t base, std::uint64_t) {
    g_ready_base.store(base);
    ++g_ready_calls;
}
} // namespace

FX_TEST(cache_region_starts_follow_first_execution_order) {
    TempDir dir(L"starts");
    FakeModule a(4u << 20, 9);
    // Stored (first executed) in descending RVA order: the main file keeps it.
    std::vector<Planned> regions = plan(300, 0x1000, 0x2000);
    std::reverse(regions.begin(), regions.end());
    start_session(dir, true);
    cache_module_loaded(a.info(kImageHash));
    store_all(a, regions, 3);
    cache_flush();
    FX_CHECK_EQ(dir.files(L"*.fxc").size(), std::size_t{1});

    start_session(dir, true);
    g_ready_calls = 0;
    cache_set_module_ready_handler(&record_ready);
    cache_module_loaded(a.info(kImageHash));
    for (int i = 0; i < 5000 && g_ready_calls.load() == 0; ++i) Sleep(1);
    FX_CHECK_EQ(g_ready_calls.load(), 1);
    FX_CHECK_EQ(g_ready_base.load(), a.base());
    std::vector<std::uint64_t> starts;
    std::uint64_t cursor = 0, chunk[7];
    while (const std::size_t n = cache_region_starts(a.base(), cursor, kCacheCursorEnd, chunk, std::size(chunk)))
        starts.insert(starts.end(), chunk, chunk + n);
    FX_REQUIRE(starts.size() == regions.size());
    for (std::size_t i = 0; i < regions.size(); ++i) FX_CHECK_EQ(starts[i], a.base() + regions[i].rva);
    // Every start is a lookup hit; its cursor resumes exactly there and, as an
    // end, stops right before it.
    TranslatedRegion got;
    std::uint64_t previous = kCacheCursorEnd;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        FX_CHECK(cache_lookup(starts[i], got) && got.guest_start == starts[i]);
        const std::uint64_t at = cache_region_cursor(starts[i]);
        FX_REQUIRE(at != kCacheCursorEnd);
        std::uint64_t from = at, one = 0;
        FX_CHECK_EQ(cache_region_starts(a.base(), from, kCacheCursorEnd, &one, 1), std::size_t{1});
        FX_CHECK_EQ(one, starts[i]);
        if (previous != kCacheCursorEnd) {
            std::uint64_t prev = previous;
            std::uint64_t got_pcs[4];
            FX_CHECK_EQ(cache_region_starts(a.base(), prev, at, got_pcs, std::size(got_pcs)), std::size_t{1});
            FX_CHECK_EQ(got_pcs[0], starts[i - 1]);
            FX_CHECK_EQ(prev, at);
        }
        previous = at;
    }
    FX_CHECK_EQ(cache_region_cursor(a.base() + regions[0].rva + 0x1000), kCacheCursorEnd);   // no region there
    std::uint64_t other = 0;
    FX_CHECK_EQ(cache_region_starts(a.base() + 0x1000, other, kCacheCursorEnd, chunk, std::size(chunk)), std::size_t{0});

    // A module without a main file never reports ready.
    FakeModule b(1u << 20, 10);
    g_ready_calls = 0;
    cache_module_loaded(b.info(kImageHash + 1, L"other.dll"));
    TranslatedRegion none;
    FX_CHECK(!cache_lookup(b.base() + 0x1000, none));   // runs (or waits for) the open
    Sleep(20);
    FX_CHECK_EQ(g_ready_calls.load(), 0);
    cache_set_module_ready_handler(nullptr);
    cache_detail::testing_reset(false);
}

namespace {

// Writes the cold AOT file (<stem>.fxa) of `m` with `regions` (seed `seed`).
std::wstring write_cold_file(const FakeModule& m, const GuestModule& info, const std::vector<Planned>& regions,
                             std::uint32_t seed) {
    std::vector<std::vector<std::uint8_t>> records(regions.size());
    std::vector<fmt::RecordRef> refs;
    for (std::size_t i = 0; i < regions.size(); ++i) {
        encode_with_hash(m, make_region(m.base(), regions[i].rva, regions[i].instructions, seed), records[i]);
        refs.push_back({records[i].data(), static_cast<std::uint32_t>(records[i].size())});
    }
    const std::wstring main_path = cache_file_path(info);
    const std::wstring stem = main_path.substr(0, main_path.size() - 4);
    // FakeModule: no path (file size 0), no PE headers (identity = info.image_hash).
    const fmt::Key key = cache_detail::make_key(info.image_hash, 0, cache_detail::configured_options());
    std::wstring error;
    FX_CHECK(cache_detail::write_main(stem, key, refs, false, nullptr, &error, L".fxa"));
    return stem + L".fxa";
}

} // namespace

FX_TEST(cache_cold_aot_file_is_a_fallback_and_promotes_used_records) {
    TempDir dir(L"cold");
    FakeModule a(4u << 20, 21);
    const GuestModule info = a.info(kImageHash);
    const auto aot = plan(200, 0x1000, 0x4000);
    start_session(dir, false);
    write_cold_file(a, info, aot, 5);
    FX_CHECK_EQ(dir.files(L"*.fxa").size(), std::size_t{1});

    // Run 1: everything hits the cold file; nothing is preloadable from it.
    start_session(dir, false);
    cache_module_loaded(info);
    const std::vector<Planned> used(aot.begin() + 50, aot.begin() + 80);
    FX_CHECK_EQ(check_all(a, used, 5), static_cast<unsigned>(entry_count(a, used, 5)));
    FX_CHECK(cache_stats().cold_hits >= used.size());
    FX_CHECK_EQ(cache_stats().cold_files, std::uint64_t{1});
    FX_CHECK_EQ(cache_stats().cold_promoted, std::uint64_t{used.size()});
    std::uint64_t cursor = 0, pcs[8];
    FX_CHECK_EQ(cache_region_starts(a.base(), cursor, kCacheCursorEnd, pcs, std::size(pcs)), std::size_t{0});
    // A JIT store of a promoted region is not stored twice.
    store_all(a, used, 5);
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{0});
    cache_flush();   // no main file yet: the promoted records become it
    FX_CHECK_EQ(dir.files(L"*.fxc").size(), std::size_t{1});

    // Run 2: the used records come from the main file, in first-use order.
    start_session(dir, false);
    cache_module_loaded(info);
    TranslatedRegion got;
    FX_CHECK(cache_lookup(a.base() + used[0].rva, got));
    std::vector<std::uint64_t> starts;
    cursor = 0;
    while (const std::size_t n = cache_region_starts(a.base(), cursor, kCacheCursorEnd, pcs, std::size(pcs)))
        starts.insert(starts.end(), pcs, pcs + n);
    FX_REQUIRE(starts.size() == used.size());
    for (std::size_t i = 0; i < used.size(); ++i) FX_CHECK_EQ(starts[i], a.base() + used[i].rva);
    const auto before = cache_stats();
    FX_CHECK_EQ(check_all(a, used, 5), static_cast<unsigned>(entry_count(a, used, 5)));
    FX_CHECK_EQ(cache_stats().cold_hits, before.cold_hits);           // main file first
    FX_CHECK(cache_lookup(a.base() + aot[0].rva, got));               // the rest still from the cold file
    FX_CHECK_EQ(cache_stats().cold_hits, before.cold_hits + 1);
    cache_detail::testing_reset(false);
}

FX_TEST(cache_cold_file_written_later_is_mapped_by_load_cold_files) {
    TempDir dir(L"coldlate");
    FakeModule a(4u << 20, 22);
    const GuestModule info = a.info(kImageHash);
    const auto aot = plan(50, 0x1000, 0x4000);
    start_session(dir, false);
    cache_module_loaded(info);
    TranslatedRegion got;
    FX_CHECK(!cache_lookup(a.base() + aot[0].rva, got));   // nothing cached (open task done)
    write_cold_file(a, info, aot, 6);                      // a background helper finishes
    FX_CHECK(!cache_lookup(a.base() + aot[0].rva, got));
    cache_load_cold_files();
    FX_CHECK_EQ(check_all(a, aot, 6), static_cast<unsigned>(entry_count(a, aot, 6)));
    cache_load_cold_files();                                // idempotent
    FX_CHECK_EQ(cache_stats().cold_files, std::uint64_t{1});
    cache_detail::testing_reset(false);
}

namespace {
TranslatedRegion tier0_of(TranslatedRegion r) {
    r.tier = 0;
    r.tier_counter = 4;
    r.tier_counter_slot = static_cast<std::uint32_t>(r.code.size() - 4);
    return r;
}
} // namespace

// Tier-0 (provisional) records round-trip their counter fields; a tier-up's
// tier-1 record for the same start is stored next to the tier-0 one, wins in
// lookups and merges, and keeps the first-use position of the start.
FX_TEST(cache_tier1_records_replace_tier0_records) {
    TempDir dir(L"tiers");
    FakeModule a(4u << 20, 31);
    const std::uint32_t r_hot = 0x3000, r_other = 0x1000;
    {
        const TranslatedRegion t0 = tier0_of(make_region(a.base(), r_hot, 12, 1));
        std::vector<std::uint8_t> bytes;
        encode_with_hash(a, t0, bytes);
        fmt::RecordView view;
        FX_REQUIRE(fmt::decode_record(bytes.data(), bytes.size(), true, view));
        TranslatedRegion back;
        fmt::rebuild_region(view, a.base(), back);
        FX_CHECK(regions_equal(back, t0));
        TranslatedRegion bad = t0;
        bad.tier_counter = static_cast<std::uint32_t>(bad.code.size());
        std::vector<std::uint8_t> out;
        FX_CHECK(!fmt::encode_record(bad, a.base(), a.size(), 0, out));
    }
    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    cache_store(tier0_of(make_region(a.base(), r_hot, 12, 1)));      // first use
    cache_store(make_region(a.base(), r_other, 8, 1));
    cache_store(make_region(a.base(), r_hot, 40, 2));                // its tier-up (same start)
    cache_store(tier0_of(make_region(a.base(), r_hot, 12, 1)));      // duplicate: ignored
    FX_CHECK_EQ(cache_stats().records_queued, std::uint64_t{3});
    cache_flush();   // no main file yet: merged

    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    TranslatedRegion got;
    FX_REQUIRE(cache_lookup(a.base() + r_hot, got));
    FX_CHECK_EQ(got.tier, 1u);
    FX_CHECK(regions_equal(got, make_region(a.base(), r_hot, 40, 2)));
    // A later tier-0 record of the same start (another process that had not
    // seen the tier-up) does not shadow it, in the journal or after a merge.
    cache_store(tier0_of(make_region(a.base(), r_hot, 14, 3)));
    cache_flush();
    for (const bool merge : {false, true}) {
        start_session(dir, merge);
        cache_module_loaded(a.info(kImageHash));
        FX_REQUIRE(cache_lookup(a.base() + r_hot, got));
        FX_CHECK_EQ(got.tier, 0u);   // journals come first: the newest record (it tiers up again)
        cache_flush();               // the second pass merges the journal in
    }
    start_session(dir, false);
    cache_module_loaded(a.info(kImageHash));
    FX_REQUIRE(cache_lookup(a.base() + r_hot, got));
    FX_CHECK_EQ(got.tier, 1u);
    FX_CHECK(regions_equal(got, make_region(a.base(), r_hot, 40, 2)));
    // File order: the hot start keeps its first-use position, ahead of r_other.
    std::vector<std::uint64_t> starts;
    std::uint64_t cursor = 0, chunk[8];
    while (const std::size_t n = cache_region_starts(a.base(), cursor, kCacheCursorEnd, chunk, std::size(chunk)))
        starts.insert(starts.end(), chunk, chunk + n);
    FX_REQUIRE(starts.size() == 2);
    FX_CHECK_EQ(starts[0], a.base() + r_hot);
    FX_CHECK_EQ(starts[1], a.base() + r_other);
    cache_detail::testing_reset(false);
}
