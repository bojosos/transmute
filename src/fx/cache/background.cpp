// Background AOT of modules that have no translation cache yet (owner: cache).
//
// cache.cpp calls background_aot_request() when the JIT stored enough regions
// of a module that has neither a main file nor a cold AOT file. Requests are batched:
// a scheduler thread waits until no request came for kQuietMs (at most
// kMaxDelayMs after the first), then starts one helper process for the batch:
//   <program> --aot-background --aot-threads <n> --cache-dir <dir> [--profile] --aot <image>...
// The helper (transmute-fx.exe, src/fx/loader/main.cpp) runs at idle
// CPU priority (not background mode: its memory priority thrashes), writes each
// image's <stem>.fxa atomically (temp file + rename) under <stem>.aot.lock,
// and skips images another process has done or is doing. It is detached: it
// inherits no handles (a parent reading our stdout/stderr pipes never waits
// for it) and breaks away from the job when allowed, so a short guest process
// still leaves a finished AOT file for the next run. When it exits, this
// process maps the new files (cache_load_cold_files).

#include "fx/cache/cache.hpp"
#include "fx/cache/cache_internal.hpp"
#include "fx/host/env.hpp"
#include "fx/host/process.hpp"
#include "fx/host/sync.hpp"
#include "fx/host/thread.hpp"
#include "fx/host/time.hpp"

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace transmute::fx {
namespace {

constexpr std::uint64_t kQuietMs = 500;
// No helper before the guest process has run this long: translation costs a
// short run ~1 s (~350 ns per guest instruction, 2-2.5M instructions for a
// Blender render), which a helper competing for the CPU (15 W laptop: the
// same power budget) costs more than it saves; long sessions are where it pays.
constexpr std::uint64_t kMinProcessAgeMs = 20000;
constexpr std::uint64_t kMaxDelayMs = 3000;
constexpr std::size_t kMaxCommandLine = 30000;

struct Background {
    host::RwLock lock;
    host::CondVar wake;
    std::wstring program;   // empty: disabled
    unsigned threads = 0;
    std::vector<std::wstring> pending;
    std::unordered_set<std::wstring> requested;   // lower-case paths, once per process
    std::uint64_t first_ms = 0, last_ms = 0;
    std::uint64_t earliest_ms = 0;   // first helper not before this (process age)
    std::uint64_t min_age_ms = kMinProcessAgeMs;
    bool scheduler = false;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> requests{0}, helpers{0};
};

Background& B() {
    static Background* const b = new Background();   // never destroyed: usable during process exit
    return *b;
}

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::wstring quoted(const std::wstring& s) {
    // Paths never contain quotes; a trailing backslash must not escape the closing quote.
    std::wstring out = L"\"" + s;
    if (!s.empty() && s.back() == L'\\') out += L'\\';
    return out + L"\"";
}

void helper_exited(void*) {
    if (!B().stopping.load()) cache_load_cold_files();
}

void start_helper(const std::wstring& program, unsigned threads, const std::vector<std::wstring>& images) {
    std::wstring command = quoted(program) + L" --aot-background --aot-threads " + std::to_wstring(threads) +
                           L" --cache-dir " + quoted(cache_detail::effective_directory());
    if (cache_detail::configured_options().profile) command += L" --profile";
    for (const std::wstring& image : images) command += L" --aot " + quoted(image);
    // Detached, idle priority, out of the caller's job if it allows that.
    if (!host::spawn_background_process(program, command, &helper_exited, nullptr)) return;
    B().helpers.fetch_add(1);
}

std::uint32_t scheduler_main(void*) {
    Background& b = B();
    for (;;) {
        std::vector<std::wstring> batch;
        std::wstring program;
        unsigned threads = 0;
        {
            b.lock.lock();
            for (;;) {
                if (b.stopping.load()) {
                    b.lock.unlock();
                    return 0;
                }
                if (!b.pending.empty()) {
                    const std::uint64_t now = host::monotonic_ms();
                    if (now >= b.earliest_ms && (now - b.last_ms >= kQuietMs || now - b.first_ms >= kMaxDelayMs)) break;
                    std::uint64_t wait = std::min(kQuietMs - std::min(kQuietMs, now - b.last_ms),
                                                  kMaxDelayMs - std::min(kMaxDelayMs, now - b.first_ms));
                    if (now < b.earliest_ms) wait = std::max(wait, b.earliest_ms - now);
                    wait = std::max<std::uint64_t>(wait, 1);
                    b.wake.wait_for(b.lock, static_cast<unsigned long>(wait));
                } else {
                    b.wake.wait_for(b.lock, host::kWaitForever);
                }
            }
            // One command line's worth; the rest goes to the next helper.
            std::size_t length = b.program.size() + 256;
            std::size_t n = 0;
            while (n < b.pending.size() && length + b.pending[n].size() + 10 < kMaxCommandLine)
                length += b.pending[n++].size() + 10;
            n = std::max<std::size_t>(n, 1);
            batch.assign(b.pending.begin(), b.pending.begin() + static_cast<std::ptrdiff_t>(n));
            b.pending.erase(b.pending.begin(), b.pending.begin() + static_cast<std::ptrdiff_t>(n));
            b.first_ms = b.last_ms = host::monotonic_ms() - kQuietMs;   // a remainder goes out right away
            program = b.program;
            threads = b.threads;
            b.lock.unlock();
        }
        start_helper(program, threads, batch);
    }
}

// Milliseconds since this process was created.
std::uint64_t process_age_ms() {
    host::ProcessTimes times;
    if (!host::process_times(times)) return 0;
    const std::uint64_t now = host::wall_time();
    return now > times.created ? (now - times.created) / 10000 : 0;
}

unsigned threads_from_environment(unsigned fallback) {
    wchar_t value[32] = {};
    const auto n = host::get_env(L"TRANSMUTE_FX_BACKGROUND_AOT", value, 32);
    if (n == 0 || n >= 32) return fallback;
    const std::wstring v = lower(value);
    if (v == L"off" || v == L"false" || v == L"no") return 0;
    if (v == L"on" || v == L"true" || v == L"yes") return cache_default_background_aot_threads();
    wchar_t* end = nullptr;
    const unsigned long threads = std::wcstoul(value, &end, 10);
    if (end == value) return fallback;
    return static_cast<unsigned>(std::min<unsigned long>(threads, 64));
}

} // namespace

unsigned cache_default_background_aot_threads() noexcept {
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    return std::clamp(hardware / 4, 1u, 4u);
}

void cache_enable_background_aot(const std::wstring& program, unsigned threads) {
    Background& b = B();
    threads = threads_from_environment(threads);
    b.lock.lock();
    b.program = threads ? program : std::wstring();
    b.threads = threads;
    wchar_t delay[32] = {};
    const auto n = host::get_env(L"TRANSMUTE_FX_BACKGROUND_AOT_DELAY", delay, 32);
    if (n > 0 && n < 32) b.min_age_ms = static_cast<std::uint64_t>(std::wcstoul(delay, nullptr, 10)) * 1000;
    const std::uint64_t age = process_age_ms();
    b.earliest_ms = host::monotonic_ms() + (age < b.min_age_ms ? b.min_age_ms - age : 0);
    b.lock.unlock();
}

void cache_prepare_exit() noexcept {
    Background& b = B();
    b.stopping.store(true);
    b.wake.notify_all();
    // In-process translation of external modules: stop between two regions.
    cache_detail::external_aot_stop(200);
}

namespace cache_detail {

void set_background_thread_qos(void* thread) noexcept {
    static const bool eco = [] {
        wchar_t value[16] = {};
        const auto n = host::get_env(L"TRANSMUTE_FX_BACKGROUND_QOS", value, 16);
        return n == 0 || n >= 16 || _wcsicmp(value, L"eco") == 0;
    }();
    if (!eco) return;
    host::set_thread_eco_qos(thread);
}

void background_aot_request(const std::wstring& image_path) {
    Background& b = B();
    if (b.stopping.load() || image_path.empty()) return;
    bool start = false;
    b.lock.lock();
    if (!b.program.empty() && b.requested.insert(lower(image_path)).second) {
        const std::uint64_t now = host::monotonic_ms();
        if (b.pending.empty()) b.first_ms = now;
        b.last_ms = now;
        b.pending.push_back(image_path);
        b.requests.fetch_add(1);
        if (!b.scheduler) b.scheduler = start = true;
    }
    b.lock.unlock();
    if (start) {
        // Never waited for: safe under the host loader lock.
        if (host::ThreadHandle thread = host::create_thread(scheduler_main, nullptr, 128 * 1024,
                                                            host::kThreadStackReservation, nullptr)) {
            host::set_thread_priority(thread, host::ThreadPriority::below_normal);
            set_background_thread_qos(thread);
            host::close_thread(thread);
        } else {
            b.lock.lock();
            b.scheduler = false;
            b.lock.unlock();
        }
    }
    b.wake.notify_all();
}

bool background_aot_quiescent() noexcept {
    if (!B().lock.try_lock()) return false;
    B().lock.unlock();
    return true;
}

bool background_aot_stopping() noexcept { return B().stopping.load(); }

std::uint64_t background_aot_requests() noexcept { return B().requests.load(); }
std::uint64_t background_aot_helpers() noexcept { return B().helpers.load(); }

} // namespace cache_detail
} // namespace transmute::fx
