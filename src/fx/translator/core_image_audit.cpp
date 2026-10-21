// Diagnostic (owner: translator core; not a CTest test): translates every
// .pdata function of an ARM64 PE image mapped at its preferred base and
//  - runs the cache's relocation audit (cachefmt::unrecorded_address) on each
//    region, printing the bytes around every 8-byte module-range window that
//    is not covered by a fixup;
//  - reports the slowest regions to translate.
//
//   fx_translator_image_audit <image.exe|dll> [dump-dir]
#include "fx/cache/format.hpp"
#include "fx/cache/pe_view.hpp"
#include "fx/translator/region_emitter.hpp"   // core_counters
#include "fx/translator/region_plan.hpp"
#include "fx/translator/translator.hpp"
#include "fx/host/sample.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"
#include "fx/host/vm.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace transmute::fx;

namespace {

class ImageSource final : public CodeSource {
public:
    ImageSource(std::uint64_t base, const pe::Image& image) : base_(base), size_(image.size_of_image()) {
        for (const auto& f : pe::pdata_functions(image))
            if (f.end > f.begin) functions_.push_back(f);
        std::sort(functions_.begin(), functions_.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
        functions_.erase(std::unique(functions_.begin(), functions_.end(),
                                     [](const auto& a, const auto& b) { return a.begin == b.begin; }),
                         functions_.end());
        for (const auto& s : image.sections())
            if (s.executable) sections_.push_back(s);
    }
    bool read(std::uint64_t address, std::uint32_t& word) const override {
        if (address < base_ || address - base_ + 4 > size_ || (address & 3)) return false;
        const auto rva = static_cast<std::uint32_t>(address - base_);
        for (const auto& s : sections_)
            if (rva >= s.rva && std::uint64_t{rva - s.rva} + 4 <= s.raw_size) {
                std::memcpy(&word, reinterpret_cast<const void*>(address), 4);
                return true;
            }
        return false;
    }
    bool function_bounds(std::uint64_t address, std::uint64_t& begin, std::uint64_t& end) const override {
        if (address < base_ || address - base_ >= size_) return false;
        const auto rva = static_cast<std::uint32_t>(address - base_);
        auto it = std::upper_bound(functions_.begin(), functions_.end(), rva,
                                   [](std::uint32_t v, const pe::Function& f) { return v < f.begin; });
        if (it == functions_.begin()) return false;
        --it;
        if (rva >= it->end) return false;
        begin = base_ + it->begin;
        end = base_ + it->end;
        return true;
    }
    bool has_entry(std::uint64_t pc) const override {
        ++has_entry_calls;
        (void)pc;
        return false;
    }
    std::vector<pe::Function> functions_;
    mutable std::atomic<std::uint64_t> has_entry_calls{0};

private:
    std::uint64_t base_, size_;
    std::vector<pe::Section> sections_;
};

struct Hit {
    std::uint32_t rva;
    std::int64_t at;
    std::vector<std::uint8_t> code;
    std::uint32_t insns;
};
struct Slow {
    double us;
    double plan_us;
    std::uint32_t rva;
    std::uint32_t insns;
    std::size_t code;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring path = argc > 1 ? argv[1]
                                 : L"C:\\dev\\transmute\\artifacts\\arm64-stage\\Blender Foundation\\Blender 4.5\\blender.exe";
    const std::wstring dump_dir = argc > 2 ? argv[2] : L"";
    pe::MappedFile file;
    pe::Image image;
    if (!file.open(path, false) || !image.parse(file.data(), file.size(), nullptr)) {
        std::fwprintf(stderr, L"cannot open %ls\n", path.c_str());
        return 1;
    }
    namespace vm = host::vm;
    namespace sample = host::sample;
    auto* memory = static_cast<std::uint8_t*>(
        vm::allocate(reinterpret_cast<void*>(image.image_base()), image.size_of_image(), vm::kReadWrite));
    if (!memory) memory = static_cast<std::uint8_t*>(vm::allocate(nullptr, image.size_of_image(), vm::kReadWrite));
    std::memcpy(memory, file.data(), std::min<std::size_t>(image.size_of_headers(), file.size()));
    for (const auto& s : image.sections())
        if (s.raw_size && std::uint64_t{s.rva} + s.raw_size <= image.size_of_image())
            std::memcpy(memory + s.rva, file.data() + s.raw_offset, s.raw_size);
    const auto base = reinterpret_cast<std::uint64_t>(memory);
    std::printf("mapped at 0x%llx (preferred 0x%llx), size 0x%x\n", static_cast<unsigned long long>(base),
                static_cast<unsigned long long>(image.image_base()), image.size_of_image());
    register_fp_emitters();
    register_neon_emitters();
    register_system_emitters();

    const ImageSource source(base, image);
    const auto& functions = source.functions_;
    if (const char* prof = std::getenv("TRANSMUTE_FX_PLAN_PROFILE")) {
        // Plan one function repeatedly: has_entry calls per plan and a RIP sample profile.
        const auto pc = base + std::strtoull(prof, nullptr, 16);
        core::Plan plan;
        const core::PlanOverrides ov;
        source.has_entry_calls = 0;
        core::build_plan(pc, source, TranslateOptions{}, ov, plan);
        std::printf("plan of rva %s: %zu instructions, %zu entries, %u blocks, %llu has_entry calls\n", prof,
                    plan.insts.size(), plan.entries.size(), plan.blocks,
                    static_cast<unsigned long long>(source.has_entry_calls.load()));
        {
            constexpr int n = 200;
            const auto t0p = std::chrono::steady_clock::now();
            for (int i = 0; i < n; ++i) core::build_plan(pc, source, TranslateOptions{}, ov, plan);
            const auto t1p = std::chrono::steady_clock::now();
            for (int i = 0; i < n / 4; ++i) translate_region(pc, source);
            const auto t2p = std::chrono::steady_clock::now();
            std::printf("unsampled: build_plan %.0f us, translate_region %.0f us\n",
                        std::chrono::duration<double, std::micro>(t1p - t0p).count() / n,
                        std::chrono::duration<double, std::micro>(t2p - t1p).count() / (n / 4));
        }
        const sample::Thread self = sample::open_current_thread();
        std::atomic<bool> stop{false};
        std::vector<std::uint64_t> samples;
        std::thread sampler([&] {
            while (!stop) {
                if (sample::suspend(self)) {
                    sample::Registers registers;
                    if (sample::capture(self, registers, false)) samples.push_back(host::rip(registers.context()));
                    sample::resume(self);
                }
                for (volatile int spin = 0; spin < 50000; ++spin) {}
            }
        });
        const auto a = std::chrono::steady_clock::now();
        constexpr int reps = 400;
        for (int i = 0; i < reps; ++i) core::build_plan(pc, source, TranslateOptions{}, ov, plan);
        const auto b = std::chrono::steady_clock::now();
        for (int i = 0; i < reps / 4; ++i) translate_region(pc, source);
        const auto c = std::chrono::steady_clock::now();
        stop = true;
        sampler.join();
        std::printf("build_plan %.0f us, translate_region %.0f us\n",
                    std::chrono::duration<double, std::micro>(b - a).count() / reps,
                    std::chrono::duration<double, std::micro>(c - b).count() / (reps / 4));
        sample::load_symbols();
        std::map<std::string, unsigned> counts;
        for (const auto rip : samples) {
            const std::string name = sample::symbol(rip);
            counts[name.empty() ? "?" : name]++;
        }
        std::vector<std::pair<unsigned, std::string>> sorted;
        for (const auto& [k, v] : counts) sorted.emplace_back(v, k);
        std::sort(sorted.rbegin(), sorted.rend());
        for (std::size_t i = 0; i < sorted.size() && i < 25; ++i)
            std::printf("  %5.1f%% %s\n", 100.0 * sorted[i].first / std::max<std::size_t>(1, samples.size()),
                        sorted[i].second.c_str());
        return 0;
    }
    if (const char* bench = std::getenv("TRANSMUTE_FX_JIT_BENCH")) {
        // Single-threaded JIT throughput: translate every <stride>-th .pdata
        // function (TRANSMUTE_FX_JIT_BENCH=<stride>[,<reps>][,lazy]), best of
        // <reps> passes, with a hash of the output (code, entries, exits,
        // fixups, faults, fault maps, pins) so that speedups can be checked
        // to generate identical code. TRANSMUTE_FX_JIT_BENCH_SAMPLE=1 adds a
        // RIP sample profile of the timed passes.
        const unsigned stride = std::max(1u, static_cast<unsigned>(std::strtoul(bench, nullptr, 10)));
        const char* comma = std::strchr(bench, ',');
        const unsigned reps = comma ? std::max(1u, static_cast<unsigned>(std::strtoul(comma + 1, nullptr, 10))) : 3;
        TranslateOptions options;
        options.lazy = std::strstr(bench, "lazy") != nullptr;
        std::vector<std::uint64_t> pcs;
        for (std::size_t i = 0; i < functions.size(); i += stride) pcs.push_back(base + functions[i].begin);
        const sample::Thread self = sample::open_current_thread();
        host::set_thread_priority(host::current_thread(), host::ThreadPriority::above_normal);
        if (const char* cpu = std::getenv("TRANSMUTE_FX_JIT_BENCH_CPU"))   // logical processor to run on
            host::set_thread_affinity(host::current_thread(), std::uint64_t{1} << std::strtoul(cpu, nullptr, 10));
        std::atomic<bool> sampling{false}, stop{false};
        std::vector<std::uint64_t> samples;
        samples.reserve(1u << 22);   // the sampler must not allocate: the suspended thread may hold the heap lock
        std::thread sampler;
        if (std::getenv("TRANSMUTE_FX_JIT_BENCH_SAMPLE")) {
            sampler = std::thread([&] {
                while (!stop) {
                    if (sampling && samples.size() < samples.capacity() && sample::suspend(self)) {
                        sample::Registers registers;
                        if (sample::capture(self, registers, false)) samples.push_back(host::rip(registers.context()));
                        sample::resume(self);
                    }
                    host::sleep_ms(0);
                    for (volatile int spin = 0; spin < 20000; ++spin) {}
                }
            });
        }
        double best = 1e30;
        std::uint64_t insns = 0, bytes = 0, hash = 0;
        for (unsigned rep = 0; rep <= reps; ++rep) {   // pass 0 warms up
            std::uint64_t h = 0xcbf29ce484222325ULL;
            const auto mix = [&](const void* data, std::size_t size) {
                const auto* p = static_cast<const std::uint8_t*>(data);
                for (std::size_t k = 0; k < size; ++k) h = (h ^ p[k]) * 0x100000001b3ULL;
            };
            std::uint64_t n_insns = 0, n_bytes = 0;
            sampling = rep > 0;
            const auto a = std::chrono::steady_clock::now();
            static const bool plan_only = std::strstr(bench, "plan") != nullptr;
            for (const auto pc : pcs) {
                if (plan_only) {
                    thread_local core::Plan plan;
                    try {
                        core::build_plan(pc, source, options, core::PlanOverrides{}, plan);
                    } catch (...) {
                        continue;
                    }
                    n_insns += plan.insts.size();
                    if (rep == 1)
                        for (const auto& pi : plan.insts) {
                            mix(&pi.pc, 8);
                            const auto& d = pi.inst;   // fields (the struct has padding)
                            const std::uint8_t ib[] = {static_cast<std::uint8_t>(d.kind), d.rd, d.rn, d.rm, d.ra, d.op,
                                                       d.cond, d.shift, d.amount, d.imms, d.size, d.sf, d.s, d.invert,
                                                       d.imm_form, static_cast<std::uint8_t>(d.mem),
                                                       static_cast<std::uint8_t>(d.mode)};
                            mix(ib, sizeof(ib));
                            mix(&d.word, 4);
                            mix(&d.imm, 8);
                            const bool bits[] = {pi.leader, pi.entry, pi.loop_header, pi.inherit_flags,
                                                 pi.conservative_writes, pi.try_family, pi.flags_live_in,
                                                 pi.flags_live_out, pi.reads_flags, pi.writes_flags, pi.may_fault};
                            mix(bits, sizeof(bits));
                            const std::int32_t ints[] = {static_cast<std::int32_t>(pi.flow), pi.family, pi.taken,
                                                         pi.fall, pi.continuation, pi.preds, pi.first_pred,
                                                         pi.entry_slot};
                            mix(ints, sizeof(ints));
                            const std::uint32_t masks[] = {pi.reads, pi.writes, pi.vreads, pi.vwrites, pi.dirty_in,
                                                           pi.vdirty_in};
                            mix(masks, sizeof(masks));
                        }
                    if (rep == 1) {
                        mix(plan.vpin_host, sizeof(plan.vpin_host));
                        mix(&plan.blocks, 4);
                        mix(&plan.interpret_count, 4);
                        mix(&plan.extra_pins, 1);
                    }
                    if (rep == 1) mix(plan.pin_host, sizeof(plan.pin_host));
                    continue;
                }
                thread_local TranslatedRegion r;   // reused, as the JIT does
                try {
                    translate_region(pc, source, options, r);
                } catch (...) {
                    continue;
                }
                n_insns += r.stats.guest_instructions;
                n_bytes += r.code.size();
                if (rep == 1) {
                    mix(r.code.data(), r.code.size());
                    for (const auto& e : r.entries) { mix(&e.guest_pc, 8); mix(&e.offset, 4); }
                    for (const auto& e : r.exits) { mix(&e.rel32_offset, 4); mix(&e.target_pc, 8); }
                    for (const auto& f : r.fixups) { mix(&f.offset, 4); mix(&f.kind, 1); mix(&f.value, 8); }
                    for (const auto& f : r.faults) { mix(&f.host_begin, 8); mix(&f.guest_pc, 8); mix(&f.flags, 2); mix(&f.map, 2); }
                    for (const auto& m : r.fault_maps) mix(&m, sizeof(m));
                    for (const auto& p : r.fault_map_regs) mix(&p, sizeof(p));
                    for (const auto& p : r.pins) mix(&p, sizeof(p));
                    mix(&r.tier, 1);
                    mix(&r.tier_counter, 4);
                }
            }
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - a).count();
            sampling = false;
            if (rep == 1) hash = h;
            if (rep > 0) best = std::min(best, s);
            insns = n_insns;
            bytes = n_bytes;
        }
        stop = true;
        if (sampler.joinable()) sampler.join();
        std::printf("jit bench: %zu regions, %llu guest instructions, %.1f MiB code, best %.3f s, %.1f ns/instruction, "
                    "output hash %016llx\n",
                    pcs.size(), static_cast<unsigned long long>(insns), static_cast<double>(bytes) / (1 << 20), best,
                    best * 1e9 / static_cast<double>(std::max<std::uint64_t>(1, insns)),
                    static_cast<unsigned long long>(hash));
        if (!samples.empty()) {
            sample::load_symbols();
            std::map<std::string, unsigned> counts;
            for (const auto rip : samples) {
                const std::string name = sample::symbol(rip);
                counts[name.empty() ? "?" : name]++;
            }
            std::vector<std::pair<unsigned, std::string>> sorted;
            for (const auto& [k, v] : counts) sorted.emplace_back(v, k);
            std::sort(sorted.rbegin(), sorted.rend());
            std::printf("%zu samples\n", samples.size());
            for (std::size_t i = 0; i < sorted.size() && i < 40; ++i)
                std::printf("  %5.1f%% %s\n", 100.0 * sorted[i].first / std::max<std::size_t>(1, samples.size()),
                            sorted[i].second.c_str());
            // TRANSMUTE_FX_JIT_BENCH_LINES=<function name substring>: samples by source line.
            if (const char* want = std::getenv("TRANSMUTE_FX_JIT_BENCH_LINES")) {
                std::map<std::pair<std::string, unsigned>, unsigned> lines;
                for (const auto rip : samples) {
                    const std::string name = sample::symbol(rip);
                    if (name.empty() || !std::strstr(name.c_str(), want)) continue;
                    std::string source_file;
                    unsigned line = 0;
                    if (sample::source_line(rip, source_file, line)) lines[{source_file, line}]++;
                }
                std::vector<std::pair<unsigned, std::pair<std::string, unsigned>>> by;
                for (const auto& [k, v] : lines) by.emplace_back(v, k);
                std::sort(by.rbegin(), by.rend());
                for (std::size_t i = 0; i < by.size() && i < 40; ++i)
                    std::printf("    %5u %s:%u\n", by[i].first, by[i].second.first.c_str(), by[i].second.second);
            }
        }
        return 0;
    }
    if (const char* list = std::getenv("TRANSMUTE_FX_AUDIT_DUMP"); list && !dump_dir.empty()) {
        // Translate the regions starting at the listed RVAs (comma-separated hex)
        // and write their code to <dump-dir>\r_<rva>.bin (static code comparisons).
        for (const char* p = list; *p;) {
            char* end = nullptr;
            const auto rva = std::strtoull(p, &end, 16);
            if (end == p) break;
            p = *end ? end + 1 : end;
            try {
                // The host offset of every guest instruction (r_<rva>.map, for tools/fx_loop_count.py).
                std::vector<core::InstructionMapEntry> imap;
                core::instruction_map_sink() = &imap;
                const auto retries_before = core::core_counters().retries.load();
                const auto r = translate_region(base + rva, source);
                core::instruction_map_sink() = nullptr;
                wchar_t name[64];
                std::swprintf(name, 64, L"\\r_%llx.bin", static_cast<unsigned long long>(rva));
                if (FILE* f = _wfopen((dump_dir + name).c_str(), L"wb")) {
                    std::fwrite(r.code.data(), 1, r.code.size(), f);
                    std::fclose(f);
                }
                std::swprintf(name, 64, L"\\r_%llx.map", static_cast<unsigned long long>(rva));
                if (FILE* mf = _wfopen((dump_dir + name).c_str(), L"w")) {
                    std::fprintf(mf, "base %llx hot %u retries %llu\n", static_cast<unsigned long long>(base),
                                 r.stats.hot_bytes ? r.stats.hot_bytes : static_cast<unsigned>(r.code.size()),
                                 static_cast<unsigned long long>(core::core_counters().retries.load() - retries_before));
                    for (const auto& pin : r.pins)
                        std::fprintf(mf, "pin %s%u %u\n", pin.vector ? "v" : "x", pin.guest, pin.host);
                    for (const auto& e : imap)   // pc, block label, own code, leader
                        std::fprintf(mf, "%llx %x %x %d\n", static_cast<unsigned long long>(e.pc - base), e.label, e.begin,
                                     e.leader ? 1 : 0);
                    std::fclose(mf);
                }
                std::printf("rva 0x%llx: %u guest insns, %zu bytes, hot %u, %zu faults, %zu fault maps, %zu entries\n",
                            static_cast<unsigned long long>(rva), r.stats.guest_instructions, r.code.size(),
                            r.stats.hot_bytes ? r.stats.hot_bytes : static_cast<unsigned>(r.code.size()),
                            r.faults.size(), r.fault_maps.size(), r.entries.size());
            } catch (...) {
                std::printf("rva 0x%llx: translation failed\n", static_cast<unsigned long long>(rva));
            }
        }
        return 0;
    }
    std::atomic<std::size_t> next{0};
    std::mutex lock;
    std::vector<Hit> hits;
    std::vector<Slow> slow;
    std::atomic<std::uint64_t> total_insns{0}, total_code{0}, total_hot{0}, literal_fixups{0}, regions{0};
    std::atomic<std::uint64_t> plan_ns{0};
    std::atomic<std::uint64_t> nondeterministic{0};
    const bool check_determinism = std::getenv("TRANSMUTE_FX_DETERMINISM") != nullptr;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> threads;
    // TRANSMUTE_FX_AUDIT_THREADS=<n>: worker threads (default: hardware threads - 2).
    unsigned workers = std::max(1u, std::thread::hardware_concurrency() - 2);
    if (const char* t = std::getenv("TRANSMUTE_FX_AUDIT_THREADS"))
        workers = std::max(1u, static_cast<unsigned>(std::strtoul(t, nullptr, 10)));
    for (unsigned t = 0; t < workers; ++t) {
        threads.emplace_back([&] {
            core::Plan plan;
            const core::PlanOverrides ov;
            for (;;) {
                const auto i = next.fetch_add(1);
                if (i >= functions.size()) return;
                const auto pc = base + functions[i].begin;
                const auto a = std::chrono::steady_clock::now();
                TranslatedRegion r;
                try {
                    r = translate_region(pc, source);
                } catch (...) {
                    continue;
                }
                const auto b = std::chrono::steady_clock::now();
                core::build_plan(pc, source, TranslateOptions{}, ov, plan);
                const auto c = std::chrono::steady_clock::now();
                plan_ns += static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(c - b).count());
                ++regions;
                total_insns += r.stats.guest_instructions;
                total_code += r.code.size();
                total_hot += r.stats.hot_bytes ? r.stats.hot_bytes : r.code.size();
                for (const auto& f : r.fixups) literal_fixups += f.kind == FixupKind::literal_imm64 ? 1 : 0;
                if (check_determinism) {   // a second translation must be byte-identical
                    const auto r2 = translate_region(pc, source);
                    const bool same = r2.code == r.code && r2.fixups.size() == r.fixups.size() &&
                                      r2.exits.size() == r.exits.size() && r2.entries.size() == r.entries.size() &&
                                      r2.faults.size() == r.faults.size() && r2.pins.size() == r.pins.size();
                    if (!same) ++nondeterministic;
                }
                const double us = std::chrono::duration<double, std::micro>(b - a).count();
                const auto at = cachefmt::unrecorded_address(r, base, image.size_of_image());
                std::lock_guard g(lock);
                slow.push_back({us, std::chrono::duration<double, std::micro>(c - b).count(), functions[i].begin,
                                r.stats.guest_instructions, r.code.size()});
                if (at >= 0) hits.push_back({functions[i].begin, at, r.code, r.stats.guest_instructions});
            }
        });
    }
    for (auto& t : threads) t.join();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("%llu regions, %llu guest instructions, %.1f MiB code (%.1f bytes/insn), %llu literal fixups, %.2f s "
                "(%zu threads); plan %.2f s total\n",
                static_cast<unsigned long long>(regions.load()), static_cast<unsigned long long>(total_insns.load()),
                static_cast<double>(total_code.load()) / (1 << 20),
                static_cast<double>(total_code.load()) / static_cast<double>(std::max<std::uint64_t>(1, total_insns.load())),
                static_cast<unsigned long long>(literal_fixups.load()), seconds, threads.size(),
                static_cast<double>(plan_ns.load()) * 1e-9);
    std::printf("code: %llu bytes, body %llu bytes (%.1f%%), cold section %llu bytes\n",
                static_cast<unsigned long long>(total_code.load()), static_cast<unsigned long long>(total_hot.load()),
                100.0 * static_cast<double>(total_hot.load()) / static_cast<double>(std::max<std::uint64_t>(1, total_code.load())),
                static_cast<unsigned long long>(total_code.load() - total_hot.load()));
    std::printf("jump tables dispatched in-region: %llu; inlined calls: %llu\n",
                static_cast<unsigned long long>(core::core_counters().tables.load()),
                static_cast<unsigned long long>(core::core_counters().inlined_calls.load()));
    std::printf("unrecorded module-range windows: %zu regions; has_entry calls %.2f per guest instruction\n", hits.size(),
                static_cast<double>(source.has_entry_calls.load()) / static_cast<double>(std::max<std::uint64_t>(1, total_insns.load())) / 2);
    if (check_determinism)
        std::printf("non-deterministic translations: %llu\n", static_cast<unsigned long long>(nondeterministic.load()));
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.rva < b.rva; });
    for (std::size_t k = 0; k < hits.size() && k < 40; ++k) {
        const auto& h = hits[k];
        std::uint64_t value;
        std::memcpy(&value, h.code.data() + h.at, 8);
        std::printf("  rva 0x%08x code+0x%llx value 0x%llx (rva 0x%llx) insns %u bytes:", h.rva,
                    static_cast<unsigned long long>(h.at), static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(value - base), h.insns);
        const auto from = h.at >= 12 ? h.at - 12 : 0;
        for (auto p = from; p < h.at + 20 && p < static_cast<std::int64_t>(h.code.size()); ++p)
            std::printf(p == h.at ? " [%02x" : (p == h.at + 7 ? " %02x]" : " %02x"), h.code[static_cast<std::size_t>(p)]);
        std::printf("\n");
        if (!dump_dir.empty() && k < 10) {
            wchar_t name[64];
            std::swprintf(name, 64, L"\\hit_%08x_%llx.bin", h.rva, static_cast<unsigned long long>(h.at));
            if (FILE* f = _wfopen((dump_dir + name).c_str(), L"wb")) {
                std::fwrite(h.code.data(), 1, h.code.size(), f);
                std::fclose(f);
            }
        }
    }
    std::sort(slow.begin(), slow.end(), [](const Slow& a, const Slow& b) { return a.us > b.us; });
    std::printf("slowest translations:\n");
    for (std::size_t k = 0; k < slow.size() && k < 12; ++k)
        std::printf("  rva 0x%08x %8.0f us (plan %6.0f us)  %5u insns  %7zu bytes\n", slow[k].rva, slow[k].us,
                    slow[k].plan_us, slow[k].insns, slow[k].code);
    return 0;
}
