// Sampling profiler:
//   TRANSMUTE_FX_SAMPLE=<file|1>[,<interval_us>][,main][,stacks][,bg]
//                       [,delay=<seconds>][,duration=<seconds>][,paused]
//
// Flags: `main` samples only the thread that initialized the runtime (the
// loader's main thread), also before it first runs guest code. `stacks`
// copies the top of the host stack of samples outside translated code
// (while suspended: a memcpy, no locks) and unwinds the copy afterwards,
// so host time is attributed to runtime/loader functions (inclusive) and
// to short folded stacks. `bg` also samples runtime background threads
// (preloaders; profiler_note_background_thread).
//
// Measurement window: `delay=<s>` starts sampling <s> seconds after
// runtime_initialize (e.g. after startup), `duration=<s>` stops it <s>
// seconds after it started (the report is written then and keeps that
// window), `paused` starts with sampling off. Seconds may be fractional.
// fx::profiler_set_enabled / fx::profiler_reset (process.hpp) open and
// close windows at run time (an explicit enable cancels a pending delay).
//
// A background thread suspends every registered guest thread at a fixed
// interval and records its host RIP (and the guest LR, x30). Samples in
// translated code are mapped to the guest function (.pdata) and guest pc
// (nearest fault site); samples elsewhere are attributed to the host module
// and symbol (DbgHelp) and, for waits in ntdll, to the guest caller (x30).
// The report is rewritten every two seconds while samples arrive, so it
// survives a killed process: each file is written to a temporary file in the
// same directory and renamed over the old one, so a process killed while
// writing leaves the previous report intact. Nothing is allocated or locked
// while a thread is suspended.

#include "fx/runtime/profiler_window.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/env.hpp"
#include "fx/host/file.hpp"
#include "fx/host/process.hpp"
#include "fx/host/sample.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "dbghelp.lib")

namespace transmute::fx::rt {
namespace {

struct ThreadProfile {
    std::uint64_t samples = 0;
    std::uint64_t translated = 0;
    std::uint64_t stubs = 0;
    std::uint64_t ntdll = 0;
    std::uint64_t host_other = 0;
};

constexpr std::size_t kStackCopy = 32 * 1024;
constexpr unsigned kStackDepth = 24;

struct Profile {
    std::wstring path;
    unsigned interval_us = 1000;
    std::uint32_t main_thread = 0;
    bool main_only = false;
    bool background = false;   // also sample runtime background threads (preloaders)
    bool stacks = false;
    std::map<std::vector<std::uint64_t>, std::uint64_t> folded;   // host frames (leaf first) -> samples
    std::uint64_t stack_samples = 0;
    std::uint64_t ticks = 0;
    std::uint64_t samples = 0;
    std::uint64_t stub_samples = 0;
    std::uint64_t arena_unknown = 0;
    std::unordered_map<std::uint64_t, std::uint64_t> guest_pcs;   // guest pc -> samples
    std::unordered_map<std::uint64_t, std::uint64_t> regions;     // region code address -> samples
    std::unordered_map<std::uint64_t, RegionSample> region_info;
    std::unordered_map<std::uint64_t, std::uint64_t> host_rips;   // host rip -> samples
    std::unordered_map<std::uint64_t, std::uint64_t> arena_rips;  // translated-code rip -> samples
    std::unordered_map<std::uint64_t, std::uint64_t> host_callers; // guest x30 of host samples -> samples
    std::unordered_map<std::uint64_t, std::uint64_t> wait_callers; // guest x30 of ntdll samples -> samples
    std::map<std::uint32_t, ThreadProfile> threads;
    std::unordered_map<std::uint32_t, host::sample::Thread> handles;
    std::uint64_t ntdll_begin = 0, ntdll_end = 0;
    bool symbols = false;
    // Measurement window (g_profile_lock).
    SampleWindow window;
    std::uint64_t start_at = 0;         // tick count when the delayed start happens (0 = done)
    std::uint64_t enabled_at = 0;       // tick count of the last enable
    std::uint64_t stop_at = 0;          // tick count when `duration` ends (0 = none)
    std::uint64_t enabled_ms = 0;       // sampling time of the current window before enabled_at
    bool enabled = false;
    bool dirty = false;              // samples or a reset since the last report
    unsigned resets = 0;
    unsigned windows = 0;            // enables
};

Profile* g_profile = nullptr;
std::atomic<std::uint32_t> g_background_threads[16]{};
std::atomic<unsigned> g_background_count{0};
std::mutex g_profile_lock;   // sampler loop vs profiler_flush and the window API
bool g_profile_stopped = false;
host::AutoResetEvent* g_profile_wake = nullptr;   // the window changed (sampler re-evaluates)

// g_profile_lock held.
void set_enabled_locked(Profile& p, bool enabled) {
    const std::uint64_t now = host::monotonic_ms();
    p.start_at = 0;   // an explicit change replaces a pending delayed start
    if (enabled == p.enabled) return;
    if (enabled) {
        p.enabled_at = now;
        ++p.windows;
    } else {
        p.enabled_ms += now - p.enabled_at;
    }
    p.enabled = enabled;
    p.dirty = true;
}

// g_profile_lock held: forgets every sample (handles and symbols stay).
void reset_locked(Profile& p) {
    p.folded.clear();
    p.stack_samples = 0;
    p.ticks = 0;
    p.samples = 0;
    p.stub_samples = 0;
    p.arena_unknown = 0;
    p.guest_pcs.clear();
    p.regions.clear();
    p.region_info.clear();
    p.host_rips.clear();
    p.arena_rips.clear();
    p.host_callers.clear();
    p.wait_callers.clear();
    p.threads.clear();
    p.enabled_ms = 0;
    p.enabled_at = host::monotonic_ms();
    ++p.resets;
    p.dirty = true;
}

// A report file is written to <path>.tmp<pid> and renamed over <path>, so a
// reader (or a process killed mid-write) never sees a truncated report.
struct ReportFile {
    std::wstring path, temp;
    FILE* f = nullptr;
    ReportFile(const std::wstring& target, const wchar_t* mode) : path(target) {
        temp = path + L".tmp" + std::to_wstring(host::process_id());
        f = _wfopen(temp.c_str(), mode);
    }
    // Closes and renames into place; false (temp removed) if anything failed.
    bool commit() {
        if (!f) return false;
        const bool written = std::ferror(f) == 0;
        const bool closed = std::fclose(f) == 0;
        f = nullptr;
        if (written && closed && replace_file_atomically(temp, path)) return true;
        host::file::remove(temp);
        return false;
    }
    ~ReportFile() {
        if (f) {
            std::fclose(f);
            host::file::remove(temp);
        }
    }
    ReportFile(const ReportFile&) = delete;
    ReportFile& operator=(const ReportFile&) = delete;
};

std::uint64_t read_lr(const GuestContext* ctx) noexcept {
    std::uint64_t lr = 0;
    return host::try_read64(reinterpret_cast<std::uint64_t>(&ctx->x[30]), lr) ? lr : 0;
}

std::string narrow(const std::wstring& text) {
    std::string out;
    for (wchar_t c : text) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

std::string host_location(std::uint64_t rip) { return host::sample::describe(rip, g_profile->symbols); }

std::string guest_location(std::uint64_t pc, std::uint64_t* function_begin = nullptr,
                           std::uint64_t* function_end = nullptr) {
    const GuestModule* m = find_guest_module(pc);
    char text[512];
    if (!m) {
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(pc));
        if (function_begin) *function_begin = pc;
        if (function_end) *function_end = pc + 4;
        return text;
    }
    std::uint64_t begin = 0, end = 0;
    if (!pdata_function_bounds(*m, pc, begin, end)) begin = end = 0;
    if (function_begin) *function_begin = begin ? begin : pc;
    if (function_end) *function_end = end ? end : pc + 4;
    if (begin)
        std::snprintf(text, sizeof(text), "%s+0x%llx (fn +0x%llx size 0x%llx)", narrow(m->name).c_str(),
                      static_cast<unsigned long long>(pc - m->base), static_cast<unsigned long long>(begin - m->base),
                      static_cast<unsigned long long>(end - begin));
    else
        std::snprintf(text, sizeof(text), "%s+0x%llx", narrow(m->name).c_str(),
                      static_cast<unsigned long long>(pc - m->base));
    return text;
}

template <typename Map>
std::vector<std::pair<typename Map::key_type, std::uint64_t>> sorted(const Map& map) {
    std::vector<std::pair<typename Map::key_type, std::uint64_t>> out(map.begin(), map.end());
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    return out;
}

void write_callers(FILE* f, const char* title, const std::unordered_map<std::uint64_t, std::uint64_t>& callers,
                   double total) {
    std::fprintf(f, "\n== %s (guest x30, top 40)\n", title);
    unsigned shown = 0;
    for (const auto& [lr, n] : sorted(callers)) {
        if (++shown > 40) break;
        std::fprintf(f, "%6.2f%% %8llu  %s\n", 100.0 * n / total, static_cast<unsigned long long>(n),
                     guest_location(lr).c_str());
    }
}

// Hottest regions: host code bytes (<report>.r<N>.bin) and an index with the
// per-offset sample counts and fault sites (host offset -> guest pc).
void dump_regions(const std::vector<std::pair<std::uint64_t, std::uint64_t>>& hot) {
    Profile& p = *g_profile;
    std::unordered_map<std::uint64_t, std::vector<std::pair<std::uint32_t, std::uint64_t>>> offsets;
    for (const auto& [rip, n] : p.arena_rips) {
        RegionSample r{};
        if (!sample_region(rip, r)) continue;
        offsets[r.code].emplace_back(r.offset, n);
    }
    ReportFile index_file(p.path + L".regions.txt", L"w");
    FILE* index = index_file.f;
    if (!index) return;
    unsigned shown = 0;
    for (const auto& [code, n] : hot) {
        if (++shown > 24) break;
        const RegionSample& r = p.region_info[code];
        ReportFile bin(p.path + L".r" + std::to_wstring(shown) + L".bin", L"wb");
        if (bin.f) {
            std::fwrite(reinterpret_cast<const void*>(r.code), 1, r.size, bin.f);
            bin.commit();
        }
        const GuestModule* m = find_guest_module(r.guest_start);
        std::fprintf(index, "region %u %s guest 0x%llx module_base 0x%llx host 0x%llx size %u samples %llu hot %u\n", shown,
                     m ? narrow(m->name).c_str() : "?", static_cast<unsigned long long>(r.guest_start),
                     static_cast<unsigned long long>(m ? m->base : 0), static_cast<unsigned long long>(r.code), r.size,
                     static_cast<unsigned long long>(n), r.hot_size);
        auto& list = offsets[code];
        std::sort(list.begin(), list.end());
        for (const auto& [offset, c] : list) std::fprintf(index, "  sample %u %llu\n", offset, static_cast<unsigned long long>(c));
        std::vector<FaultSite> faults;
        if (region_fault_sites(r.code, faults))
            for (const FaultSite& site : faults)
                std::fprintf(index, "  fault %u %u 0x%llx\n", site.host_begin, site.host_end,
                             static_cast<unsigned long long>(site.guest_pc));
    }
    index_file.commit();
}

// Unwinding a stack copy stops at translated code (host::sample::unwind_copy).
bool arena_pc(std::uint64_t pc) { return in_arena(pc); }

bool system_frame(const std::string& s) {
    for (const char* prefix : {"ntdll", "KERNEL", "ucrtbase", "VCRUNTIME", "MSVCP", "win32u", "USER32", "GDI32"})
        if (s.rfind(prefix, 0) == 0) return true;
    return false;
}

void write_stacks(FILE* f, double total) {
    Profile& p = *g_profile;
    if (!p.stacks) return;
    // Inclusive: every symbol on a sampled stack counts once per sample.
    std::map<std::string, std::uint64_t> inclusive, folded, first_own, runtime_path;
    std::unordered_map<std::uint64_t, std::string> names;
    const auto name = [&](std::uint64_t rip) -> const std::string& {
        auto it = names.find(rip);
        if (it == names.end()) it = names.emplace(rip, host_location(rip)).first;
        return it->second;
    };
    for (const auto& [frames, n] : p.folded) {
        std::vector<const std::string*> seen;
        std::string key, path;
        unsigned fx_frames = 0;
        bool own = false;
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const std::string& s = name(frames[i]);
            if (std::find_if(seen.begin(), seen.end(), [&](const std::string* x) { return *x == s; }) == seen.end()) {
                seen.push_back(&s);
                inclusive[s] += n;
            }
            if (i < 6) {
                if (!key.empty()) key += " <- ";
                key += s;
            }
            if (!own && !system_frame(s)) {
                own = true;
                first_own[s] += n;
            }
            // The innermost two transmute-fx frames: which runtime/loader path.
            if (fx_frames < 2 && s.rfind("transmute-fx", 0) == 0) {
                path += fx_frames++ ? " <- " : "";
                path += s.substr(s.find('!') == std::string::npos ? 0 : s.find('!') + 1);
            }
        }
        runtime_path[name(frames[0]).substr(0, 40) + " | " + (path.empty() ? std::string("-") : path)] += n;
        if (!own) first_own["(system frames only)"] += n;
        folded[key.empty() ? std::string("(no host frames)") : key] += n;
    }
    std::fprintf(f, "\n== host stacks: %llu samples unwound; first non-system frame (top 50)\n",
                 static_cast<unsigned long long>(p.stack_samples));
    unsigned shown = 0;
    for (const auto& [s, n] : sorted(first_own)) {
        if (++shown > 50) break;
        std::fprintf(f, "%6.2f%% %8llu  %s\n", 100.0 * n / total, static_cast<unsigned long long>(n), s.c_str());
    }
    std::fprintf(f, "\n== host stacks: leaf | innermost runtime frames (top 60)\n");
    shown = 0;
    for (const auto& [s, n] : sorted(runtime_path)) {
        if (++shown > 60) break;
        std::fprintf(f, "%6.2f%% %8llu  %s\n", 100.0 * n / total, static_cast<unsigned long long>(n), s.c_str());
    }
    std::fprintf(f, "\n== host stacks: inclusive by symbol (top 80)\n");
    shown = 0;
    for (const auto& [s, n] : sorted(inclusive)) {
        if (++shown > 80) break;
        std::fprintf(f, "%6.2f%% %8llu  %s\n", 100.0 * n / total, static_cast<unsigned long long>(n), s.c_str());
    }
    std::fprintf(f, "\n== host stacks: folded (leaf first, 6 frames; top 80)\n");
    shown = 0;
    for (const auto& [s, n] : sorted(folded)) {
        if (++shown > 80) break;
        std::fprintf(f, "%6.2f%% %8llu  %s\n", 100.0 * n / total, static_cast<unsigned long long>(n), s.c_str());
    }
}

// Hot code footprint: the hottest regions that cover 50/90/99% of the
// translated samples, their host code bytes and 4 KiB pages, the guest bytes
// they translate (sum over regions) and the distinct guest bytes (union).
// sum > union means the same guest code runs from more than one host copy
// (overlapping regions), which costs instruction cache, iTLB and L2 space.
// host-hot is the regions' body bytes (TranslationStats::hot_bytes; the rest
// is their cold section: exit stubs and tails, rare lookups, entry stubs,
// constant pools), hot-pages the 4 KiB pages those bodies touch.
// Then the guest functions whose code the hot regions translate more than
// once (sum of their ranges inside the function - union).
void write_footprint(FILE* f, const std::vector<std::pair<std::uint64_t, std::uint64_t>>& hot) {
    Profile& p = *g_profile;
    std::uint64_t all = 0;
    for (const auto& [code, n] : hot) all += n;
    if (!all) return;
    std::fprintf(f, "\n== hot code footprint: share regions host-bytes host-pages guest-bytes(sum) guest-bytes(union) "
                    "expansion(host/union) host-hot hot-pages expansion(hot/union)\n");
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    std::vector<Range> guest;
    std::vector<std::uint64_t> pages, hot_pages;
    std::uint64_t covered = 0, host_bytes = 0, hot_bytes = 0, guest_sum = 0;
    std::size_t index = 0;
    std::size_t at90 = 0;
    for (const double share : {0.5, 0.9, 0.99}) {
        while (index < hot.size() && covered < share * all) {
            const auto& [code, n] = hot[index++];
            covered += n;
            const RegionSample& r = p.region_info[code];
            host_bytes += r.size;
            const std::uint32_t body = r.hot_size && r.hot_size <= r.size ? r.hot_size : r.size;
            hot_bytes += body;
            for (std::uint64_t page = r.code >> 12; page <= (r.code + r.size - 1) >> 12; ++page) pages.push_back(page);
            for (std::uint64_t page = r.code >> 12; body && page <= (r.code + body - 1) >> 12; ++page)
                hot_pages.push_back(page);
            std::vector<Range> ranges;
            if (region_guest_ranges(r.code, ranges))
                for (const auto& range : ranges) {
                    guest.push_back(range);
                    guest_sum += range.second - range.first;
                }
        }
        if (share == 0.9) at90 = index;
        std::sort(pages.begin(), pages.end());
        pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
        std::sort(hot_pages.begin(), hot_pages.end());
        hot_pages.erase(std::unique(hot_pages.begin(), hot_pages.end()), hot_pages.end());
        std::vector<Range> merged = guest;
        std::sort(merged.begin(), merged.end());
        std::uint64_t guest_union = 0, lo = 0, hi = 0;
        bool open = false;
        for (const auto& [a, b] : merged) {
            if (open && a <= hi) { hi = std::max(hi, b); continue; }
            if (open) guest_union += hi - lo;
            lo = a; hi = b; open = true;
        }
        if (open) guest_union += hi - lo;
        std::fprintf(f, "  %3.0f%% %6zu %9llu %6zu %9llu %9llu %5.2f %9llu %6zu %5.2f\n", 100.0 * covered / all, index,
                     static_cast<unsigned long long>(host_bytes), pages.size(), static_cast<unsigned long long>(guest_sum),
                     static_cast<unsigned long long>(guest_union),
                     guest_union ? static_cast<double>(host_bytes) / guest_union : 0.0,
                     static_cast<unsigned long long>(hot_bytes), hot_pages.size(),
                     guest_union ? static_cast<double>(hot_bytes) / guest_union : 0.0);
    }

    // Duplicated guest code among the regions covering 90%, by function.
    struct Dup {
        std::uint64_t sum = 0;
        std::vector<Range> ranges;
        unsigned regions = 0;
    };
    std::map<std::uint64_t, Dup> by_function;
    for (std::size_t i = 0; i < at90 && i < hot.size(); ++i) {
        const RegionSample& r = p.region_info[hot[i].first];
        std::vector<Range> ranges;
        if (!region_guest_ranges(r.code, ranges)) continue;
        std::set<std::uint64_t> seen;
        for (const auto& range : ranges) {
            std::uint64_t begin = 0, end = 0;
            guest_location(range.first, &begin, &end);
            Dup& d = by_function[begin];
            d.sum += range.second - range.first;
            d.ranges.push_back(range);
            if (seen.insert(begin).second) ++d.regions;
        }
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dups;   // (duplicated bytes, function)
    std::uint64_t total_dup = 0;
    for (auto& [fn, d] : by_function) {
        std::sort(d.ranges.begin(), d.ranges.end());
        std::uint64_t u = 0, lo = 0, hi = 0;
        bool open = false;
        for (const auto& [a, b] : d.ranges) {
            if (open && a <= hi) { hi = std::max(hi, b); continue; }
            if (open) u += hi - lo;
            lo = a; hi = b; open = true;
        }
        if (open) u += hi - lo;
        if (d.sum > u) {
            dups.emplace_back(d.sum - u, fn);
            total_dup += d.sum - u;
        }
    }
    std::sort(dups.rbegin(), dups.rend());
    std::fprintf(f, "\n== guest code translated more than once by the regions covering 90%%: %llu bytes "
                    "(duplicated-bytes regions function)\n", static_cast<unsigned long long>(total_dup));
    for (std::size_t k = 0; k < dups.size() && k < 20; ++k)
        std::fprintf(f, "  %8llu %4u  %s\n", static_cast<unsigned long long>(dups[k].first), by_function[dups[k].second].regions,
                     guest_location(dups[k].second).c_str());
}

void write_report() {
    Profile& p = *g_profile;
    p.dirty = false;
    ReportFile report(p.path, L"w");
    FILE* f = report.f;
    if (!f) return;
    const double total = p.samples ? static_cast<double>(p.samples) : 1.0;
    std::uint64_t arena = 0;
    for (const auto& [pc, n] : p.guest_pcs) arena += n;
    std::uint64_t host = 0;
    for (const auto& [rip, n] : p.host_rips) host += n;
    std::fprintf(f, "fx sampling profile: %llu samples, %llu ticks, interval %u us\n",
                 static_cast<unsigned long long>(p.samples), static_cast<unsigned long long>(p.ticks), p.interval_us);
    std::fprintf(f, "translated %.1f%%  stubs %.1f%%  arena-unknown %.1f%%  host %.1f%%\n", 100.0 * arena / total,
                 100.0 * p.stub_samples / total, 100.0 * p.arena_unknown / total, 100.0 * host / total);
    const std::uint64_t sampled_ms = p.enabled_ms + (p.enabled ? host::monotonic_ms() - p.enabled_at : 0);
    std::fprintf(f, "window: %.1f s sampled, %s, %u enable(s), %u reset(s)%s\n", sampled_ms / 1000.0,
                 p.enabled ? "sampling" : "paused", p.windows, p.resets,
                 p.start_at ? " (delayed start pending)" : "");

    std::fprintf(f, "\n== threads: samples translated%% stubs%% ntdll%% host-other%%\n");
    for (const auto& [id, t] : p.threads) {
        const double n = t.samples ? static_cast<double>(t.samples) : 1.0;
        std::fprintf(f, "  %6lu %8llu  %5.1f %5.1f %5.1f %5.1f\n", id, static_cast<unsigned long long>(t.samples),
                     100.0 * t.translated / n, 100.0 * t.stubs / n, 100.0 * t.ntdll / n, 100.0 * t.host_other / n);
    }

    // Host side: by module, then by symbol.
    std::map<std::string, std::uint64_t> host_modules, host_symbols;
    for (const auto& [rip, n] : p.host_rips) {
        const std::string where = host_location(rip);
        host_symbols[where] += n;
        host_modules[where.substr(0, where.find_first_of("!+"))] += n;
    }
    std::fprintf(f, "\n== host modules\n");
    for (const auto& [name, n] : sorted(host_modules))
        std::fprintf(f, "%6.2f%% %8llu  %s\n", 100.0 * n / total, static_cast<unsigned long long>(n), name.c_str());
    std::fprintf(f, "\n== host symbols (top 60)\n");
    unsigned shown = 0;
    for (const auto& [name, n] : sorted(host_symbols)) {
        if (++shown > 60) break;
        std::fprintf(f, "%6.2f%% %8llu  %s\n", 100.0 * n / total, static_cast<unsigned long long>(n), name.c_str());
    }
    write_callers(f, "ntdll samples by guest caller", p.wait_callers, total);
    write_callers(f, "other host samples by guest caller", p.host_callers, total);
    write_stacks(f, total);

    // Guest side: by function.
    struct Function { std::uint64_t samples = 0; std::uint64_t end = 0; std::vector<std::pair<std::uint64_t, std::uint64_t>> pcs; };
    std::map<std::uint64_t, Function> functions;
    for (const auto& [pc, n] : p.guest_pcs) {
        std::uint64_t begin = 0, end = 0;
        guest_location(pc, &begin, &end);
        Function& fn = functions[begin];
        fn.samples += n;
        fn.end = end;
        fn.pcs.emplace_back(pc, n);
    }
    std::vector<std::pair<std::uint64_t, std::uint64_t>> by_samples;
    for (const auto& [begin, fn] : functions) by_samples.emplace_back(begin, fn.samples);
    std::sort(by_samples.begin(), by_samples.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    std::fprintf(f, "\n== guest functions (top 80): %% of all samples, cumulative, %% of translated\n");
    shown = 0;
    double cumulative = 0;
    const double translated = arena ? static_cast<double>(arena) : 1.0;
    for (const auto& [begin, n] : by_samples) {
        if (++shown > 80) break;
        cumulative += n;
        std::fprintf(f, "%6.2f%% %6.2f%% %6.2f%% %8llu  %s\n", 100.0 * n / total, 100.0 * cumulative / total,
                     100.0 * n / translated, static_cast<unsigned long long>(n), guest_location(begin).c_str());
    }
    std::fprintf(f, "\n== hot guest pcs in the top 25 functions\n");
    shown = 0;
    for (const auto& [begin, n] : by_samples) {
        if (++shown > 25) break;
        Function& fn = functions[begin];
        std::sort(fn.pcs.begin(), fn.pcs.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::fprintf(f, "-- %s  %.2f%%\n", guest_location(begin).c_str(), 100.0 * n / total);
        unsigned k = 0;
        for (const auto& [pc, c] : fn.pcs) {
            if (++k > 12) break;
            const GuestModule* m = find_guest_module(pc);
            std::fprintf(f, "     %6.2f%%  +0x%llx\n", 100.0 * c / total,
                         static_cast<unsigned long long>(m ? pc - m->base : pc));
        }
    }
    std::fprintf(f, "\n== hot regions (top 40): guest_start code size samples\n");
    shown = 0;
    const auto hot_regions = sorted(p.regions);
    dump_regions(hot_regions);
    for (const auto& [code, n] : hot_regions) {
        if (++shown > 40) break;
        const RegionSample& r = p.region_info[code];
        std::fprintf(f, "%6.2f%%  %s  host 0x%llx size %u hot %u\n", 100.0 * n / total, guest_location(r.guest_start).c_str(),
                     static_cast<unsigned long long>(r.code), r.size, r.hot_size);
    }
    write_footprint(f, hot_regions);
    report.commit();
}

struct Raw {
    std::uint32_t thread;
    std::uint64_t rip;
    std::uint64_t lr;
    unsigned depth;                       // host frames in `frames` (stacks)
    std::uint64_t frames[kStackDepth];
};

std::uint32_t sampler_main(void*) {
    namespace sample = host::sample;
    Profile& p = *g_profile;
    p.symbols = sample::load_symbols();
    sample::wait_module(p.ntdll_begin, p.ntdll_end);
    const sample::Timer timer = sample::create_timer();
    std::vector<std::pair<std::uint32_t, GuestContext*>> threads;
    std::vector<Raw> raw;
    std::vector<std::uint8_t> stack_copy(p.stacks ? kStackCopy : 0);
    std::uint64_t last_report = host::monotonic_ms();
    const std::uint64_t arena = reinterpret_cast<std::uint64_t>(arena_base());
    bool sampling = false;   // last loop's state (outside the lock)
    for (;;) {
        if (sampling) {
            sample::wait(timer, p.interval_us);
        } else {
            // Paused: wake for a window change, a delayed start, or the next report.
            unsigned long wait = 2000;
            {
                std::lock_guard lock(g_profile_lock);
                const std::uint64_t now = host::monotonic_ms();
                if (p.start_at)
                    wait = static_cast<unsigned long>(std::min<std::uint64_t>(wait, p.start_at > now ? p.start_at - now : 0));
            }
            g_profile_wake->wait_for(wait);
        }
        std::lock_guard lock(g_profile_lock);
        if (g_profile_stopped) return 0;
        const std::uint64_t now = host::monotonic_ms();
        if (p.start_at && now >= p.start_at) {
            set_enabled_locked(p, true);   // clears start_at
            if (p.window.duration_seconds > 0)
                p.stop_at = now + static_cast<std::uint64_t>(p.window.duration_seconds * 1000.0);
        }
        if (p.stop_at && now >= p.stop_at && p.enabled) {
            p.stop_at = 0;
            set_enabled_locked(p, false);
            write_report();   // the window is complete: keep exactly it on disk
            last_report = now;
        }
        sampling = p.enabled;
        if (!sampling) {
            if (p.dirty && now - last_report >= 2000) {
                write_report();
                last_report = now;
            }
            continue;
        }
        p.dirty = true;
        ++p.ticks;
        registered_threads(threads);
        if (p.main_only) {
            GuestContext* main_ctx = nullptr;
            for (const auto& [id, ctx] : threads)
                if (id == p.main_thread) main_ctx = ctx;
            threads.assign(1, {p.main_thread, main_ctx});
        }
        if (p.background) {
            const unsigned n = std::min(g_background_count.load(std::memory_order_acquire), 16u);
            for (unsigned i = 0; i < n; ++i)
                if (const std::uint32_t id = g_background_threads[i].load(std::memory_order_relaxed)) threads.emplace_back(id, nullptr);
        }
        raw.clear();
        raw.reserve(threads.size());
        for (const auto& [id, ctx] : threads) {
            auto it = p.handles.find(id);
            if (it == p.handles.end()) it = p.handles.emplace(id, sample::open_thread(id)).first;
            const sample::Thread h = it->second;
            if (!h) continue;
            if (!sample::suspend(h)) continue;
            sample::Registers registers;
            const bool ok = sample::capture(h, registers, p.stacks);
            const std::uint64_t pc = ok ? host::rip(registers.context()) : 0;
            const bool in_host = ok && !in_arena(pc);
            const std::uint64_t lr = in_host && ctx ? read_lr(ctx) : 0;
            std::size_t copied = 0;
            if (in_host && p.stacks) copied = sample::copy_stack(host::rsp(registers.context()), stack_copy.data(), kStackCopy);
            sample::resume(h);
            if (!ok) continue;
            raw.push_back(Raw{id, pc, lr, 0, {}});
            // Unwound after the resume: the host's unwinder may take loader locks.
            if (copied)
                raw.back().depth = sample::unwind_copy(registers, stack_copy.data(), copied, raw.back().frames, kStackDepth,
                                                       &arena_pc);
        }
        for (const Raw& s : raw) {
            ++p.samples;
            ThreadProfile& t = p.threads[s.thread];
            ++t.samples;
            if (in_arena(s.rip)) {
                if (s.rip - arena < kStubAreaSize) { ++p.stub_samples; ++t.stubs; continue; }
                RegionSample r{};
                if (!sample_region(s.rip, r)) { ++p.arena_unknown; ++t.stubs; continue; }
                ++t.translated;
                ++p.arena_rips[s.rip];
                ++p.guest_pcs[r.guest_pc];
                if (++p.regions[r.code] == 1) p.region_info[r.code] = r;
            } else {
                ++p.host_rips[s.rip];
                if (s.depth) {
                    ++p.stack_samples;
                    ++p.folded[std::vector<std::uint64_t>(s.frames, s.frames + s.depth)];
                }
                if (s.rip >= p.ntdll_begin && s.rip < p.ntdll_end) {
                    ++t.ntdll;
                    ++p.wait_callers[s.lr];
                } else {
                    ++t.host_other;
                    ++p.host_callers[s.lr];
                }
            }
        }
        if (now - last_report >= 2000) {
            write_report();
            last_report = host::monotonic_ms();
        }
    }
}

} // namespace

void profiler_note_background_thread() noexcept {
    const unsigned i = g_background_count.fetch_add(1, std::memory_order_acq_rel);
    if (i < 16) g_background_threads[i].store(host::current_thread_id(), std::memory_order_release);
}

void profiler_flush() {
    if (!g_profile) return;
    std::lock_guard lock(g_profile_lock);
    if (g_profile_stopped) return;
    g_profile_stopped = true;
    write_report();
}

void profiler_start_if_requested() {
    constexpr std::size_t kValue = 260;   // MAX_PATH
    wchar_t value[kValue]{};
    const auto n = host::get_env(L"TRANSMUTE_FX_SAMPLE", value, kValue);
    if (n == 0 || n >= kValue) return;
    SampleWindow window;
    if (!parse_sample_setting(std::wstring_view(value, n), window)) return;
    auto* p = new Profile();
    p->window = window;
    p->interval_us = window.interval_us;
    p->main_only = window.main_only;
    p->stacks = window.stacks;
    p->background = window.background;
    p->main_thread = host::current_thread_id();
    std::wstring text = window.path;
    if (text == L"1") {
        text = host::file::temp_directory() + L"fx-profile-" + std::to_wstring(host::process_id()) + L".txt";
    }
    p->path = text;
    const std::uint64_t now = host::monotonic_ms();
    if (window.paused) {
        // Off until fx::profiler_set_enabled(true).
    } else if (window.delay_seconds > 0) {
        p->start_at = now + std::max<std::uint64_t>(1, static_cast<std::uint64_t>(window.delay_seconds * 1000.0));
    } else {
        p->enabled = true;
        p->enabled_at = now;
        p->windows = 1;
        if (window.duration_seconds > 0) p->stop_at = now + static_cast<std::uint64_t>(window.duration_seconds * 1000.0);
    }
    g_profile_wake = new host::AutoResetEvent();   // never destroyed, like the profile
    g_profile = p;
    if (host::ThreadHandle thread = host::create_thread(&sampler_main, nullptr, 0, 0, nullptr)) {
        host::set_thread_priority(thread, host::ThreadPriority::highest);
        host::close_thread(thread);
    }
}

} // namespace transmute::fx::rt

namespace transmute::fx {

// Public window control (process.hpp). No-ops unless TRANSMUTE_FX_SAMPLE
// started the profiler; safe from any thread, also guest threads (one that
// waits here for g_profile_lock is suspended and resumed by the sampler like
// any other; it never holds the lock while the sampler needs it).
void profiler_reset() {
    if (!rt::g_profile) return;
    {
        std::lock_guard lock(rt::g_profile_lock);
        if (rt::g_profile_stopped) return;
        rt::reset_locked(*rt::g_profile);
    }
    rt::g_profile_wake->set();
}

void profiler_set_enabled(bool enabled) {
    if (!rt::g_profile) return;
    {
        std::lock_guard lock(rt::g_profile_lock);
        if (rt::g_profile_stopped) return;
        rt::Profile& p = *rt::g_profile;
        rt::set_enabled_locked(p, enabled);
        if (!enabled) p.stop_at = 0;
    }
    rt::g_profile_wake->set();
}

} // namespace transmute::fx
