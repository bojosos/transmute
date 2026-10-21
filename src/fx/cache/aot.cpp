// Ahead-of-time translation of whole ARM64 PE images (owner: cache).
//
// The image is mapped as data. Static roots: entry point, exports, every
// .pdata BeginAddress and DIR64 relocation targets inside executable
// sections. Workers translate roots in parallel; with follow_flow each
// translated region also seeds direct branch/call targets and BL/BLR
// continuations that no region covers yet (depth-first, per worker). The
// output goes to the module's main cache file under the key the runtime
// computes, merged with whatever that file already holds.

#include "fx/cache/aot.hpp"

#include "fx/cache/cache_internal.hpp"
#include "fx/cache/format.hpp"
#include "fx/cache/pe_view.hpp"
#include "fx/host/file.hpp"
#include "fx/host/time.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace transmute::fx::aot_detail {
namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

class AtomicBitmap {
public:
    explicit AtomicBitmap(std::size_t bits)
        : words_((bits + 63) / 64 + 1), data_(new std::atomic<std::uint64_t>[words_]) {
        for (std::size_t i = 0; i < words_; ++i) data_[i].store(0, std::memory_order_relaxed);
    }
    [[nodiscard]] bool test(std::size_t bit) const {
        return (data_[bit / 64].load(std::memory_order_relaxed) >> (bit % 64)) & 1;
    }
    // True if the bit was clear.
    bool set(std::size_t bit) {
        const std::uint64_t mask = std::uint64_t{1} << (bit % 64);
        return (data_[bit / 64].fetch_or(mask, std::memory_order_relaxed) & mask) == 0;
    }
    [[nodiscard]] std::vector<std::uint64_t> snapshot() const {
        std::vector<std::uint64_t> out(words_);
        for (std::size_t i = 0; i < words_; ++i) out[i] = data_[i].load(std::memory_order_relaxed);
        return out;
    }

private:
    std::size_t words_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> data_;
};

class ImageSource final : public CodeSource {
public:
    ImageSource(const pe::Image& image, std::vector<pe::Function> functions)
        : image_(image), base_(image.image_base()), functions_(std::move(functions)) {
        for (const pe::Section& s : image.sections()) {
            if (s.executable) executable_.push_back(s);
            // Read-only data (.rdata: jump and label tables), for read_data.
            constexpr std::uint32_t kRead = 0x40000000u, kWrite = 0x80000000u;
            if (!s.executable && (s.characteristics & kRead) && !(s.characteristics & kWrite)) read_only_.push_back(s);
        }
    }

    bool read(std::uint64_t address, std::uint32_t& word) const override { return read_from(executable_, address, word); }
    bool read_data(std::uint64_t address, std::uint32_t& word) const override {
        return read_from(executable_, address, word) || read_from(read_only_, address, word);
    }

    bool read_from(const std::vector<pe::Section>& sections, std::uint64_t address, std::uint32_t& word) const {
        std::uint32_t rva = 0;
        if (!to_rva(address, rva) || (rva & 3) != 0) return false;
        for (const pe::Section& s : sections) {
            if (rva >= s.rva && std::uint64_t{rva - s.rva} + 4 <= s.raw_size) {
                std::memcpy(&word, image_.data() + s.raw_offset + (rva - s.rva), 4);
                return true;
            }
        }
        return false;
    }

    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override {
        std::uint32_t rva = 0;
        const pe::Function* f = to_rva(address, rva) ? function_of(rva) : nullptr;
        if (!f) return false;
        begin = base_ + f->begin;
        end = base_ + f->end;
        return true;
    }

    [[nodiscard]] const pe::Function* function_of(std::uint32_t rva) const {
        auto it = std::upper_bound(functions_.begin(), functions_.end(), rva,
                                   [](std::uint32_t value, const pe::Function& f) { return value < f.begin; });
        if (it == functions_.begin()) return nullptr;
        --it;
        return rva < it->end ? &*it : nullptr;
    }

    [[nodiscard]] const std::vector<pe::Function>& functions() const { return functions_; }

    // Regions stop where another region begins: static roots, or pcs already
    // claimed/translated by any worker. Avoids re-translating covered code
    // from late roots inside large functions.
    void set_claims(const std::vector<std::uint32_t>* static_roots, const AtomicBitmap* claimed,
                    const AtomicBitmap* covered) {
        static_roots_ = static_roots;
        claimed_ = claimed;
        covered_ = covered;
    }
    bool has_entry(std::uint64_t pc) const override {
        std::uint32_t rva = 0;
        if (!to_rva(pc, rva) || (rva & 3) != 0) return false;
        if (static_roots_ && std::binary_search(static_roots_->begin(), static_roots_->end(), rva)) return true;
        return (claimed_ && claimed_->test(rva / 4)) || (covered_ && covered_->test(rva / 4));
    }

    [[nodiscard]] bool executable(std::uint32_t rva) const {
        for (const pe::Section& s : executable_) {
            const std::uint32_t extent = std::max(s.virtual_size, s.raw_size);
            if (rva >= s.rva && rva - s.rva < extent) return true;
        }
        return false;
    }

private:
    bool to_rva(std::uint64_t address, std::uint32_t& rva) const {
        if (address < base_ || address - base_ > 0xFFFF'FFFFull) return false;
        rva = static_cast<std::uint32_t>(address - base_);
        return true;
    }
    const pe::Image& image_;
    std::uint64_t base_;
    std::vector<pe::Function> functions_;  // sorted by begin, non-empty
    std::vector<pe::Section> executable_;
    std::vector<pe::Section> read_only_;
    const std::vector<std::uint32_t>* static_roots_{};
    const AtomicBitmap* claimed_{};
    const AtomicBitmap* covered_{};
};

struct Work {
    const pe::Image& image;
    const ImageSource& source;
    TranslateFn translate;
    TranslateOptions options;
    bool follow;
    std::uint64_t base;
    std::uint32_t span;
    const std::vector<std::uint32_t>* roots{};  // current phase
    std::atomic<std::size_t> next{0};
    AtomicBitmap claimed;
    AtomicBitmap covered;
    AtomicBitmap swept;  // roots that came from the .pdata call sweep
};

struct WorkerResult {
    std::vector<std::uint8_t> records;
    std::vector<std::pair<std::size_t, std::uint32_t>> refs;  // offset, size
    std::vector<std::uint32_t> deferred;                      // flow roots for the next phase
    std::vector<std::uint32_t> unencodable_roots;
    std::uint64_t regions{}, entries{}, code_bytes{}, guest_instructions{}, interpreted{};
    std::uint64_t translate_failed{}, unencodable{}, unrelocatable{}, sweep{}, flow{};
};

// Translates the roots of one phase. Flow targets found in translated regions
// are deferred to the next phase, so they are skipped when a region translated
// meanwhile already has them as entries (e.g. call continuations).
class Worker {
public:
    Worker(Work& work, WorkerResult& out, bool late_phase) : w_(work), out_(out), late_(late_phase) {}

    void run() {
        for (;;) {
            const std::size_t i = w_.next.fetch_add(1);
            if (i >= w_.roots->size()) return;
            process((*w_.roots)[i]);
        }
    }

private:
    void defer(std::uint64_t target) {
        if (target < w_.base) return;
        const std::uint64_t rel = target - w_.base;
        if (rel >= w_.span || (rel & 3) != 0) return;
        const auto rva = static_cast<std::uint32_t>(rel);
        if (w_.covered.test(rva / 4) || w_.claimed.test(rva / 4) || !w_.source.executable(rva)) return;
        out_.deferred.push_back(rva);
    }

    // Continuation of a call at `call_pc`: only inside the caller's function
    // when its bounds are known (a call to a noreturn function may be last).
    void defer_continuation(std::uint64_t call_pc) {
        std::uint64_t begin = 0, end = 0;
        if (w_.source.function_bounds(call_pc, begin, end) && call_pc + 4 >= end) return;
        defer(call_pc + 4);
    }

    void process(std::uint32_t rva) {
        if (w_.covered.test(rva / 4) || !w_.claimed.set(rva / 4)) return;
        TranslatedRegion region;
        try {
            region = w_.translate(w_.base + rva, w_.source, w_.options);
        } catch (...) {
            ++out_.translate_failed;
            return;
        }
        // Guest-code hash over the file bytes (what the runtime will see unless relocated).
        std::uint64_t hash = cachefmt::guest_hash_init(region.guest_ranges.size());
        bool readable = true;
        for (const auto& [begin, end] : region.guest_ranges) {
            if (begin < w_.base || end < begin || end - w_.base > w_.span) {
                readable = false;
                break;
            }
            const auto begin_rva = static_cast<std::uint32_t>(begin - w_.base);
            const auto size = static_cast<std::uint32_t>(end - begin);
            const std::uint8_t* bytes = size ? w_.image.at_rva(begin_rva, size) : nullptr;
            if (size && !bytes) {
                readable = false;
                break;
            }
            hash = cachefmt::guest_hash_step(hash, begin_rva, bytes, size);
        }
        // Unrelocatable code is stored pinned to the preferred base: used when the
        // loader maps the image there, a miss (JIT) otherwise.
        const bool pinned = readable && cachefmt::unrecorded_address(region, w_.base, w_.span) >= 0;
        const std::size_t offset = out_.records.size();
        if (!readable || !cachefmt::encode_record(region, w_.base, w_.span, hash, out_.records, pinned)) {
            ++out_.unencodable;
            out_.unencodable_roots.push_back(rva);
            return;
        }
        out_.refs.emplace_back(offset, static_cast<std::uint32_t>(out_.records.size() - offset));
        if (pinned) ++out_.unrelocatable;
        // Only stored regions cover their entries.
        for (const RegionEntry& e : region.entries) {
            const std::uint64_t rel = e.guest_pc - w_.base;
            if (rel < w_.span && (rel & 3) == 0) w_.covered.set(static_cast<std::size_t>(rel / 4));
        }
        ++out_.regions;
        out_.entries += region.entries.size();
        out_.code_bytes += region.code.size();
        out_.guest_instructions += region.stats.guest_instructions;
        out_.interpreted += region.stats.interpreted_instructions;
        if (late_) ++(w_.swept.test(rva / 4) ? out_.sweep : out_.flow);

        if (!w_.follow) return;
        for (const RegionExit& x : region.exits) defer(x.target_pc);
        for (const auto& [begin, end] : region.guest_ranges) {
            for (std::uint64_t pc = begin; pc + 4 <= end; pc += 4) {
                std::uint32_t word = 0;
                if (!w_.source.read(pc, word)) break;
                if ((word & 0xFC00'0000u) == 0x9400'0000u) {  // BL
                    const std::int64_t offset26 = static_cast<std::int32_t>(word << 6) >> 6;
                    defer(pc + static_cast<std::uint64_t>(offset26 * 4));
                    defer_continuation(pc);
                } else if ((word & 0xFFFF'FC1Fu) == 0xD63F'0000u) {  // BLR
                    defer_continuation(pc);
                }
            }
        }
    }

    Work& w_;
    WorkerResult& out_;
    bool late_;
};

// <stem>.aot.lock, held while one process translates the cold file of a
// module (delete-on-close: a crashed holder releases it).
host::file::File try_claim(const std::wstring& stem) {
    namespace file = host::file;
    return file::open(stem + L".aot.lock", file::kRead | file::kWrite, file::kShareNone, file::Create::open_always,
                      file::Flags::lock_file);
}

bool valid_cold_file(const std::wstring& stem, const cachefmt::Key& key) {
    pe::MappedFile file;
    cachefmt::MainView view;
    return file.open(stem + L".fxa", false) && cachefmt::open_main(file.data(), file.size(), key, view);
}

// Cold-file mode: true if this process should translate the image (holding
// `claim`), false if a valid cold file exists (perhaps after waiting for
// another process that was writing it).
bool claim_cold_file(const std::wstring& stem, const cachefmt::Key& key, host::file::File& claim) {
    claim = nullptr;
    const std::uint64_t deadline = host::monotonic_ms() + 30ull * 60 * 1000;
    for (;;) {
        if (valid_cold_file(stem, key)) return false;
        claim = try_claim(stem);
        if (claim) {
            if (!valid_cold_file(stem, key)) return true;   // ours to write
            host::file::close(claim);
            claim = nullptr;
            return false;
        }
        if (host::monotonic_ms() >= deadline) return false;
        host::sleep_ms(1000);   // another process is translating it
    }
}

void sort_unique(std::vector<std::uint32_t>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

void export_roots(const pe::Image& image, const ImageSource& source, std::vector<std::uint32_t>& out) {
    const pe::DataDirectory dir = image.directory(0);
    if (dir.rva == 0 || dir.size < 40) return;
    std::uint32_t count = 0, functions = 0;
    if (!image.read(dir.rva + 20, count) || !image.read(dir.rva + 28, functions)) return;
    if (count > (1u << 20)) return;
    const std::uint8_t* table = image.at_rva(functions, count * 4);
    if (!table) return;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t rva = 0;
        std::memcpy(&rva, table + i * 4, 4);
        if (rva == 0 || (rva >= dir.rva && rva - dir.rva < dir.size)) continue;  // empty or forwarder
        if ((rva & 3) == 0 && source.executable(rva)) out.push_back(rva);
    }
}

void relocation_roots(const pe::Image& image, const ImageSource& source, std::vector<std::uint32_t>& out) {
    const pe::DataDirectory dir = image.directory(5);  // IMAGE_DIRECTORY_ENTRY_BASERELOC
    if (dir.rva == 0 || dir.size < 8) return;
    const std::uint8_t* table = image.at_rva(dir.rva, dir.size);
    if (!table) return;
    std::uint32_t offset = 0;
    while (offset + 8 <= dir.size) {
        std::uint32_t page = 0, block = 0;
        std::memcpy(&page, table + offset, 4);
        std::memcpy(&block, table + offset + 4, 4);
        if (block < 8 || block > dir.size - offset) break;
        for (std::uint32_t at = 8; at + 2 <= block; at += 2) {
            std::uint16_t entry = 0;
            std::memcpy(&entry, table + offset + at, 2);
            if ((entry >> 12) != 10) continue;  // IMAGE_REL_BASED_DIR64
            std::uint64_t value = 0;
            if (!image.read(page + (entry & 0xFFF), value) || value < image.image_base()) continue;
            const std::uint64_t rel = value - image.image_base();
            if (rel > 0xFFFF'FFFFull || (rel & 3) != 0) continue;
            if (source.executable(static_cast<std::uint32_t>(rel))) out.push_back(static_cast<std::uint32_t>(rel));
        }
        offset += block;
    }
}

} // namespace

std::size_t translate_image(const std::wstring& path, const AotOptions& options, TranslateFn translate,
                            AotStats* stats_out, Detail* detail, std::wstring* error) {
    const auto start = Clock::now();
    AotStats stats;
    pe::MappedFile file;
    pe::Image image;
    if (!file.open(path, false, error)) return 0;
    if (!image.parse(file.data(), file.size(), error)) return 0;
    if (image.machine() != pe::kMachineArm64) {
        if (error) *error = L"not an ARM64 image: " + path;
        return 0;
    }
    if (image.size_of_image() == 0) {
        if (error) *error = L"image has no size: " + path;
        return 0;
    }
    // AOT cost does not matter: always full (tier-1) regions, never first-tier ones.
    const TranslateOptions translate_options = [&] {
        TranslateOptions o = options.translate_options ? *options.translate_options : cache_detail::configured_options();
        o.lazy = false;
        return o;
    }();
    const cachefmt::Key key = cache_detail::make_key(pe::image_identity(file.data(), file.size()), file.size(),
                                                     translate_options);
    const std::wstring directory =
        options.cache_directory.empty() ? cache_detail::effective_directory() : options.cache_directory;
    const std::wstring stem = cache_detail::stem_path(directory, cache_detail::file_name_of(path), key);
    struct Claim {
        host::file::File handle = nullptr;
        ~Claim() { host::file::close(handle); }
    } claim;
    if (options.write_cache && options.cold_file) {
        if (!cache_detail::ensure_directory(directory)) {
            if (error) *error = L"cannot create cache directory " + directory;
            return 0;
        }
        if (!claim_cold_file(stem, key, claim.handle)) {
            stats.cache_path = stem + L".fxa";
            stats.total_seconds = seconds_since(start);
            if (stats_out) *stats_out = std::move(stats);
            return 0;   // done by this or another process
        }
    }

    // ---- discovery ----------------------------------------------------------
    std::vector<pe::Function> pdata = pe::pdata_functions(image);
    std::vector<pe::Function> bounded;
    bounded.reserve(pdata.size());
    for (const pe::Function& f : pdata)
        if (f.end > f.begin) bounded.push_back(f);
    std::sort(bounded.begin(), bounded.end(),
              [](const pe::Function& a, const pe::Function& b) { return a.begin < b.begin; });
    bounded.erase(std::unique(bounded.begin(), bounded.end(),
                              [](const pe::Function& a, const pe::Function& b) { return a.begin == b.begin; }),
                  bounded.end());
    ImageSource source(image, std::move(bounded));

    std::vector<std::uint32_t> entry, exports, pdata_roots, relocations;
    if (image.entry_rva() != 0 && (image.entry_rva() & 3) == 0 && source.executable(image.entry_rva()))
        entry.push_back(image.entry_rva());
    export_roots(image, source, exports);
    for (const pe::Function& f : pdata)
        if ((f.begin & 3) == 0 && source.executable(f.begin)) pdata_roots.push_back(f.begin);
    relocation_roots(image, source, relocations);
    for (auto* v : {&entry, &exports, &pdata_roots, &relocations}) sort_unique(*v);
    stats.entry_point = entry.size();
    stats.exports = exports.size();
    stats.pdata = pdata_roots.size();
    stats.relocation_targets = relocations.size();
    std::vector<std::uint32_t> roots;
    roots.reserve(entry.size() + exports.size() + pdata_roots.size() + relocations.size());
    for (auto* v : {&entry, &exports, &pdata_roots, &relocations}) roots.insert(roots.end(), v->begin(), v->end());
    sort_unique(roots);
    stats.functions = roots.size();
    std::vector<std::uint32_t> sweep_roots;
    if (options.sweep_calls) {
        std::vector<std::uint32_t> calls;
        for (const pe::Function& f : source.functions()) {
            if (!source.executable(f.begin)) continue;
            for (std::uint32_t rva = f.begin; rva + 4 <= f.end; rva += 4) {
                std::uint32_t word = 0;
                if (!image.read(rva, word)) break;
                if ((word & 0xFC00'0000u) == 0x9400'0000u) {  // BL: target and continuation
                    const std::int64_t target =
                        std::int64_t{rva} + (static_cast<std::int64_t>(static_cast<std::int32_t>(word << 6) >> 6) * 4);
                    if (target >= 0 && target < image.size_of_image() && (target & 3) == 0 &&
                        source.executable(static_cast<std::uint32_t>(target)))
                        calls.push_back(static_cast<std::uint32_t>(target));
                } else if ((word & 0xFFFF'FC1Fu) != 0xD63F'0000u) {  // not BLR either
                    continue;
                }
                if (rva + 4 < f.end) calls.push_back(rva + 4);
            }
        }
        sort_unique(calls);
        sweep_roots = std::move(calls);
    }
    stats.discover_seconds = seconds_since(start);

    // ---- translation --------------------------------------------------------
    // Phase 1: function roots. Later phases: sweep roots and deferred flow roots
    // that no stored region covers yet, until nothing new turns up.
    const auto translate_start = Clock::now();
    const std::uint32_t span = image.size_of_image();
    Work work{image, source, translate, translate_options, options.follow_flow, image.image_base(), span, nullptr,
              {},    AtomicBitmap(span / 4 + 1), AtomicBitmap(span / 4 + 1), AtomicBitmap(span / 4 + 1)};
    for (std::uint32_t rva : sweep_roots) work.swept.set(rva / 4);
    source.set_claims(&roots, &work.claimed, &work.covered);
    const unsigned hardware = options.threads ? options.threads : std::max(1u, std::thread::hardware_concurrency());
    std::vector<WorkerResult> results(hardware);
    std::vector<std::uint32_t> phase = roots;
    for (unsigned round = 0; !phase.empty(); ++round) {
        const auto threads =
            static_cast<unsigned>(std::min<std::size_t>(hardware, std::max<std::size_t>(1, phase.size() / 16)));
        stats.threads = std::max(stats.threads, threads);
        work.roots = &phase;
        work.next.store(0);
        {
            std::vector<std::thread> pool;
            for (unsigned t = 1; t < threads; ++t)
                pool.emplace_back([&work, &results, t, round] { Worker(work, results[t], round > 0).run(); });
            Worker(work, results[0], round > 0).run();
            for (std::thread& t : pool) t.join();
        }
        std::vector<std::uint32_t> next;
        if (round == 0) next = sweep_roots;
        for (WorkerResult& r : results) {
            next.insert(next.end(), r.deferred.begin(), r.deferred.end());
            r.deferred.clear();
        }
        sort_unique(next);
        std::erase_if(next,
                      [&](std::uint32_t rva) { return work.covered.test(rva / 4) || work.claimed.test(rva / 4); });
        phase = std::move(next);
    }
    std::vector<cachefmt::RecordRef> refs;
    for (const WorkerResult& r : results) {
        stats.regions += r.regions;
        stats.entries += r.entries;
        stats.code_bytes += r.code_bytes;
        stats.guest_instructions += r.guest_instructions;
        stats.interpreted_instructions += r.interpreted;
        stats.translate_failed += r.translate_failed;
        stats.unencodable += r.unencodable;
        stats.unrelocatable += r.unrelocatable;
        stats.call_sweep_roots += r.sweep;
        stats.flow_roots += r.flow;
        for (const auto& [offset, size] : r.refs) refs.push_back({r.records.data() + offset, size});
    }
    stats.failed = stats.translate_failed + stats.unencodable;
    stats.translate_seconds = seconds_since(translate_start);

    // ---- output ---------------------------------------------------------------
    std::size_t written = refs.size();
    if (options.write_cache) {
        const auto write_start = Clock::now();
        const wchar_t* extension = options.cold_file ? L".fxa" : L".fxc";
        cachefmt::BuildResult built;
        // A cold file holds AOT output only; the main file merges with what it has.
        if (!cache_detail::write_main(stem, key, refs, !options.cold_file, &built, error, extension)) return 0;
        written = built.regions;
        stats.file_bytes = built.file_size;
        stats.cache_path = stem + extension;
        stats.write_seconds = seconds_since(write_start);
    }
    stats.total_seconds = seconds_since(start);
    if (detail) {
        detail->image_base = image.image_base();
        detail->size_of_image = span;
        detail->pdata_begins = std::move(pdata_roots);
        roots.insert(roots.end(), sweep_roots.begin(), sweep_roots.end());
        sort_unique(roots);
        detail->static_roots = std::move(roots);
        detail->covered = work.covered.snapshot();
        detail->unencodable.clear();
        for (const WorkerResult& r : results)
            detail->unencodable.insert(detail->unencodable.end(), r.unencodable_roots.begin(), r.unencodable_roots.end());
        std::sort(detail->unencodable.begin(), detail->unencodable.end());
    }
    if (stats_out) *stats_out = std::move(stats);
    return written;
}

} // namespace transmute::fx::aot_detail

namespace transmute::fx {

std::wstring aot_format_stats(const AotStats& s) {
    wchar_t line[1024];
    std::swprintf(line, 1024,
                  L"functions %llu (pdata %llu, exports %llu, relocations %llu, entry %llu); regions %llu = "
                  L"functions + %llu call-sweep + %llu flow; "
                  L"entries %llu, guest instructions %llu (%llu interpreted), code %.1f MiB, "
                  L"failed %llu (%llu threw, %llu unencodable); %llu pinned to the preferred base; %u threads; discover %.3f s, translate %.3f s, "
                  L"write %.3f s, total %.3f s",
                  static_cast<unsigned long long>(s.functions), static_cast<unsigned long long>(s.pdata),
                  static_cast<unsigned long long>(s.exports), static_cast<unsigned long long>(s.relocation_targets),
                  static_cast<unsigned long long>(s.entry_point), static_cast<unsigned long long>(s.regions),
                  static_cast<unsigned long long>(s.call_sweep_roots), static_cast<unsigned long long>(s.flow_roots),
                  static_cast<unsigned long long>(s.entries), static_cast<unsigned long long>(s.guest_instructions),
                  static_cast<unsigned long long>(s.interpreted_instructions),
                  static_cast<double>(s.code_bytes) / (1024.0 * 1024.0), static_cast<unsigned long long>(s.failed),
                  static_cast<unsigned long long>(s.translate_failed), static_cast<unsigned long long>(s.unencodable),
                  static_cast<unsigned long long>(s.unrelocatable),
                  s.threads, s.discover_seconds, s.translate_seconds, s.write_seconds, s.total_seconds);
    std::wstring out = line;
    if (!s.cache_path.empty()) {
        std::swprintf(line, 1024, L"; wrote %.1f MiB to ", static_cast<double>(s.file_bytes) / (1024.0 * 1024.0));
        out += line;
        out += s.cache_path;
    }
    return out;
}

} // namespace transmute::fx
