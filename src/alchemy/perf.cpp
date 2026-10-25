#include "alchemy/perf.hpp"

#include "alchemy/host/process.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>


// ALCHEMY_PERF reporting (see include/alchemy/perf.hpp).
namespace alchemy::perf {
namespace {

std::string environment(const char* name) {
#ifdef _MSC_VER
    char* buffer = nullptr;
    std::size_t length = 0;
    std::string value;
    if (_dupenv_s(&buffer, &length, name) == 0 && buffer) {
        value = buffer;
        std::free(buffer);
    }
    return value;
#else
    const char* value = std::getenv(name);
    return value ? value : "";
#endif
}

using Clock = std::chrono::steady_clock;

struct Reporter {
    std::mutex mutex;
    FILE* out{stderr};
    bool owns_out{};
    double period{2.0};
    // Calibration: rdtsc against the steady clock since start().
    Clock::time_point start_clock{Clock::now()};
    std::uint64_t start_ticks{ticks()};
    // Milliseconds from process creation to start().
    double created_ms{};
    // Phases, in order of first occurrence: name -> ms since process creation.
    std::vector<std::pair<std::string, double>> phases;
    bool phases_printed{};
    // Names of synthetic call addresses.
    std::unordered_map<std::uint64_t, std::string> names;
    // Periodic report: the previous snapshot.
    std::vector<std::uint64_t> last_counters = std::vector<std::uint64_t>(counter_count);
    std::vector<std::uint64_t> last_calls_main = std::vector<std::uint64_t>(slot_count);
    std::vector<std::uint64_t> last_ticks_main = std::vector<std::uint64_t>(slot_count);
    std::vector<std::uint64_t> last_calls = std::vector<std::uint64_t>(slot_count);
    Clock::time_point last_report{};
    std::uint64_t first_frame_counters[counter_count]{};
    bool first_frame_seen{};
    Clock::time_point first_frame_time{};
    std::vector<std::uint64_t> first_calls_main, first_ticks_main, first_calls;
    bool exit_reported{};
    std::function<std::uint64_t()> sources[counter_count];
    // Keyed counts: category -> key -> count (current and at the last report).
    std::mutex keys_mutex;
    std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>> keys, last_keys, first_keys;
    // Input-to-present latency (input_mutex): the first input not yet
    // followed by a present, and samples (ms) of the period and the run.
    std::mutex input_mutex;
    bool input_pending{};
    Clock::time_point input_arrival{};
    std::uint32_t input_message_tick{};
    std::vector<double> latency_arrival, latency_posted, run_latency_arrival, run_latency_posted;
};

Reporter& reporter() {
    static auto* value = new Reporter;   // leaked: used from atexit and late threads
    return *value;
}

double tick_seconds() {
    auto& r = reporter();
#ifdef ALCHEMY_PERF_RDTSC
    const auto elapsed = std::chrono::duration<double>(Clock::now() - r.start_clock).count();
    const auto delta = ticks() - r.start_ticks;
    if (elapsed < 0.05 || !delta) {
        // Too early to calibrate: measure 20 ms now.
        const auto c0 = Clock::now();
        const auto t0 = ticks();
        while (std::chrono::duration<double>(Clock::now() - c0).count() < 0.02) {}
        return std::chrono::duration<double>(Clock::now() - c0).count() / static_cast<double>(ticks() - t0);
    }
    return elapsed / static_cast<double>(delta);
#else
    static_cast<void>(r);
    return std::chrono::duration<double>(Clock::duration(1)).count();
#endif
}

double now_ms_since_creation() {
    auto& r = reporter();
    return r.created_ms + std::chrono::duration<double, std::milli>(Clock::now() - r.start_clock).count();
}

const char* counter_name(unsigned counter) {
    static constexpr const char* names[] = {
        "objc_send", "objc_slow", "objc_slow_cached", "objc_slow_filled", "objc_native_direct", "objc_send_guest",
        "objc_alloc", "objc_dealloc", "objc_store_lookups", "host_calls", "host_calls_main", "host_ticks_main_top",
        "mtl_command_buffers", "mtl_commits", "mtl_render_encoders", "mtl_compute_encoders", "mtl_blit_encoders",
        "mtl_draws", "mtl_dispatches", "mtl_blits", "mtl_set_buffer", "mtl_set_bytes", "mtl_set_bytes_bytes",
        "mtl_set_texture", "mtl_set_sampler", "mtl_set_pipeline", "mtl_pipeline_binds", "mtl_descriptor_sets",
        "mtl_descriptor_writes", "mtl_descriptor_set_reuse", "mtl_descriptor_pools", "mtl_argument_tables",
        "mtl_upload_bytes", "mtl_vk_barriers", "mtl_vk_state", "mtl_vk_binds", "mtl_queue_submits", "mtl_cpu_waits",
        "mtl_cpu_wait_ticks", "mtl_pipelines_created", "mtl_pipeline_create_ticks", "mtl_libraries",
        "mtl_library_ticks", "msl_compiles", "msl_cache_hits", "msl_seed_hits", "msl_compile_ticks", "msl_lookup_ticks",
        "mtl_encoder_descriptor_ticks", "mtl_encoder_begin_ticks",
        "mtl_encoder_object_ticks", "mtl_write_set_ticks", "mtl_source_ticks", "present_count", "present_ticks",
        "present_slot_wait_ticks", "present_acquire_ticks", "present_queue_ticks", "next_drawable",
        "next_drawable_ticks", "next_drawable_wait_ticks", "runloop_waits", "runloop_wait_ticks", "guest_sleeps",
        "guest_sleep_ticks"};
    static_assert(std::size(names) == counter_count, "one name per perf::Counter");
    return counter < counter_count ? names[counter] : "?";
}
bool is_ticks(unsigned counter) {
    const std::string_view name = counter_name(counter);
    return name.ends_with("_ticks") || name.ends_with("_top");
}

std::string slot_name(std::size_t index) {
    auto& r = reporter();
    const auto base = g_state.slot_base.load(), stride = g_state.slot_stride.load();
    const auto address = base + stride * index;
    if (const auto found = r.names.find(address); found != r.names.end()) return found->second;
    char text[48];
    std::snprintf(text, sizeof(text), "slot#%zu", index);
    return text;
}

// One summary over `frames` frames and `seconds` of wall time from counter
// and slot deltas. Caller holds the mutex.
void write_summary(FILE* out, const char* title, double seconds, std::uint64_t frames, const std::uint64_t* counters,
                   const std::vector<std::uint64_t>& calls_main, const std::vector<std::uint64_t>& ticks_main,
                   const std::vector<std::uint64_t>& calls) {
    const double per = frames ? 1.0 / static_cast<double>(frames) : 0.0;
    const double ms_per_tick = tick_seconds() * 1000.0;
    std::fprintf(out, "[alchemy perf] %s: %.2f s, %llu frames presented (%.1f fps)\n", title, seconds,
                 static_cast<unsigned long long>(frames), seconds > 0 ? static_cast<double>(frames) / seconds : 0.0);
    if (!frames) return;
    std::fprintf(out, "[alchemy perf]   per frame:");
    unsigned column = 0;
    for (unsigned c = 0; c < counter_count; ++c) {
        if (!counters[c]) continue;
        if (column++ % 6 == 0) std::fprintf(out, "\n[alchemy perf]    ");
        if (is_ticks(c)) std::fprintf(out, " %s=%.3fms", counter_name(c), static_cast<double>(counters[c]) * per * ms_per_tick);
        else std::fprintf(out, " %s=%.1f", counter_name(c), static_cast<double>(counters[c]) * per);
    }
    std::fprintf(out, "\n");
    // Top synthetic calls, main thread: by time (timing mode) and by count.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < calls_main.size(); ++i) if (calls_main[i] || calls[i]) order.push_back(i);
    if (timing()) {
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return ticks_main[a] > ticks_main[b]; });
        std::fprintf(out, "[alchemy perf]   main-thread host calls by inclusive time (ms/frame, calls/frame, us/call):\n");
        for (std::size_t k = 0; k < order.size() && k < 30; ++k) {
            const auto i = order[k];
            if (!ticks_main[i]) break;
            const double ms = static_cast<double>(ticks_main[i]) * ms_per_tick;
            std::fprintf(out, "[alchemy perf]     %8.3f %9.1f %8.2f  %s\n", ms * per, static_cast<double>(calls_main[i]) * per,
                         calls_main[i] ? ms * 1000.0 / static_cast<double>(calls_main[i]) : 0.0, slot_name(i).c_str());
        }
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return calls[a] > calls[b]; });
    std::fprintf(out, "[alchemy perf]   host calls by count, all threads (calls/frame, main calls/frame):\n");
    for (std::size_t k = 0; k < order.size() && k < 40; ++k) {
        const auto i = order[k];
        if (!calls[i]) break;
        std::fprintf(out, "[alchemy perf]     %9.1f %9.1f  %s\n", static_cast<double>(calls[i]) * per,
                     static_cast<double>(calls_main[i]) * per, slot_name(i).c_str());
    }
}

void write_phases(FILE* out, const char* when) {
    auto& r = reporter();
    std::fprintf(out, "[alchemy perf] startup phases (%s), ms since process creation:\n", when);
    for (const auto& [name, ms] : r.phases) std::fprintf(out, "[alchemy perf]   %10.1f  %s\n", ms, name.c_str());
    std::fflush(out);
}

// Input-to-present latency samples: count, median, p90, max.
void write_latency(FILE* out, const char* what, std::vector<double> samples) {
    if (samples.empty()) return;
    std::sort(samples.begin(), samples.end());
    const auto at = [&](double q) { return samples[std::min(samples.size() - 1, static_cast<std::size_t>(q * samples.size()))]; };
    std::fprintf(out, "[alchemy perf]   input-to-present (%s): %zu samples, median %.1f ms, p90 %.1f ms, max %.1f ms\n", what,
                 samples.size(), at(0.5), at(0.9), samples.back());
}

using Keys = std::unordered_map<std::string, std::unordered_map<std::string, std::uint64_t>>;
// Top keys per category of `now - before`.
void write_keys(FILE* out, const Keys& now, const Keys& before, double per) {
    for (const auto& [category, values] : now) {
        std::vector<std::pair<std::string, std::uint64_t>> rows;
        const auto previous = before.find(category);
        for (const auto& [key, count] : values) {
            std::uint64_t old = 0;
            if (previous != before.end())
                if (const auto found = previous->second.find(key); found != previous->second.end()) old = found->second;
            if (count > old) rows.emplace_back(key, count - old);
        }
        if (rows.empty()) continue;
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::fprintf(out, "[alchemy perf]   %s (per frame, top 20 of %zu):\n", category.c_str(), rows.size());
        for (std::size_t k = 0; k < rows.size() && k < 20; ++k)
            std::fprintf(out, "[alchemy perf]     %9.2f  %s\n", static_cast<double>(rows[k].second) * per, rows[k].first.c_str());
    }
}
Keys copy_keys() {
    auto& r = reporter();
    std::lock_guard lock(r.keys_mutex);
    return r.keys;
}

void snapshot(std::uint64_t* counters, std::vector<std::uint64_t>& calls_main, std::vector<std::uint64_t>& ticks_main,
              std::vector<std::uint64_t>& calls) {
    auto& r = reporter();
    for (unsigned c = 0; c < counter_count; ++c) {
        counters[c] = g_state.counters[c].load(std::memory_order_relaxed);
        if (r.sources[c]) {
            try { counters[c] += r.sources[c](); } catch (...) {}
        }
    }
    calls_main.resize(slot_count);
    ticks_main.resize(slot_count);
    calls.resize(slot_count);
    for (std::size_t i = 0; i < slot_count; ++i) {
        calls_main[i] = g_state.slot_calls_main[i].load(std::memory_order_relaxed);
        ticks_main[i] = g_state.slot_ticks_main[i].load(std::memory_order_relaxed);
        calls[i] = g_state.slot_calls[i].load(std::memory_order_relaxed);
    }
}

} // namespace

int read_mode() noexcept {
    int value = 0;
    try {
        const auto text = environment("ALCHEMY_PERF");
        if (!text.empty() && text != "0") value = text == "2" ? 2 : 1;
    } catch (...) {
        value = 0;
    }
    int expected = -1;
    if (!g_state.mode.compare_exchange_strong(expected, value)) return expected;
    return value;
}

bool is_main_thread() noexcept {
    return static_cast<std::uint32_t>(host::process::current_thread_id()) == g_state.main_thread.load(std::memory_order_relaxed);
}

void start() {
    if (!enabled()) return;
    auto& r = reporter();
    g_state.main_thread.store(static_cast<std::uint32_t>(host::process::current_thread_id()));
    if (const auto created = host::process::start_time())
        r.created_ms = static_cast<double>(host::time::realtime_ns() - static_cast<std::int64_t>(*created)) / 1'000'000.0;
    const auto out = environment("ALCHEMY_PERF_OUT");
    if (!out.empty()) {
        FILE* file = nullptr;
#ifdef _MSC_VER
        if (fopen_s(&file, out.c_str(), "a") != 0) file = nullptr;
#else
        file = std::fopen(out.c_str(), "a");
#endif
        if (file) { r.out = file; r.owns_out = true; }
    }
    const auto period = environment("ALCHEMY_PERF_PERIOD");
    if (!period.empty()) r.period = std::max(0.25, std::atof(period.c_str()));
    r.last_report = Clock::now();
    phase("alchemy main");
    std::atexit(report_exit);
}

void phase(std::string_view name) {
    if (!enabled()) return;
    auto& r = reporter();
    const auto ms = now_ms_since_creation();
    // Every occurrence, repeats included (a later dlopen passes the loader's phases again).
    static const bool memory = environment("ALCHEMY_PERF_MEMORY") == "1";
    if (memory) {
        if (const auto usage = host::process::memory_usage())
            std::fprintf(stderr, "[alchemy perf memory] pid %u %8.1f ms private %llu KiB host heap %llu KiB: %.*s\n",
                         host::process::current_pid(), ms, static_cast<unsigned long long>(usage->private_bytes / 1024),
                         static_cast<unsigned long long>(usage->heap_committed_bytes / 1024), static_cast<int>(name.size()),
                         name.data());
    }
    std::lock_guard lock(r.mutex);
    for (const auto& item : r.phases) if (item.first == name) return;
    r.phases.emplace_back(std::string(name), ms);
}

void name_address(std::uint64_t address, std::string_view name) {
    if (!enabled()) return;
    auto& r = reporter();
    std::lock_guard lock(r.mutex);
    r.names.insert_or_assign(address, std::string(name));
}

std::string address_name(std::uint64_t address) {
    if (!enabled()) return {};
    auto& r = reporter();
    std::lock_guard lock(r.mutex);
    const auto found = r.names.find(address);
    return found != r.names.end() ? found->second : std::string{};
}

void set_counter_source(Counter counter, std::function<std::uint64_t()> read) {
    if (!enabled() || counter >= counter_count) return;
    auto& r = reporter();
    std::lock_guard lock(r.mutex);
    r.sources[counter] = std::move(read);
}

void retire_counter_source(Counter counter) {
    if (!enabled() || counter >= counter_count) return;
    auto& r = reporter();
    std::lock_guard lock(r.mutex);
    if (!r.sources[counter]) return;
    try { g_state.counters[counter].fetch_add(r.sources[counter]()); } catch (...) {}
    r.sources[counter] = {};
}

void count_key(std::string_view category, std::string_view key, std::uint64_t value) {
    if (!enabled()) return;
    auto& r = reporter();
    std::lock_guard lock(r.keys_mutex);
    r.keys[std::string(category)][std::string(key)] += value;
}

void input_received(std::uint32_t message_tick, std::uint32_t now_tick) {
    if (!enabled()) return;
    auto& r = reporter();
    std::lock_guard lock(r.input_mutex);
    if (r.input_pending) return;
    r.input_pending = true;
    r.input_arrival = Clock::now();
    // Posting time on the arrival clock: GetMessageTime is a tick count (ms).
    r.input_message_tick = now_tick - std::min<std::uint32_t>(now_tick - message_tick, 10'000);
}

void set_slot_layout(std::uint64_t base, std::uint64_t stride) noexcept {
    g_state.slot_base.store(base);
    g_state.slot_stride.store(stride);
}

void frame_presented() {
    if (!enabled()) return;
    auto& r = reporter();
    g_state.counters[present_count].fetch_add(1, std::memory_order_relaxed);
    const auto now = Clock::now();
    {
        std::lock_guard lock(r.input_mutex);
        if (r.input_pending) {
            r.input_pending = false;
            const double arrival = std::chrono::duration<double, std::milli>(now - r.input_arrival).count();
            double posted = arrival;
            posted = static_cast<double>(host::time::tick_ms() - r.input_message_tick);
            r.latency_arrival.push_back(arrival);
            r.latency_posted.push_back(posted);
            r.run_latency_arrival.push_back(arrival);
            r.run_latency_posted.push_back(posted);
        }
    }
    std::unique_lock lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!r.first_frame_seen) {
        r.first_frame_seen = true;
        r.first_frame_time = now;
        r.phases.emplace_back("first frame presented", now_ms_since_creation());
        snapshot(r.first_frame_counters, r.first_calls_main, r.first_ticks_main, r.first_calls);
        for (unsigned c = 0; c < counter_count; ++c) r.last_counters[c] = r.first_frame_counters[c];
        r.last_calls_main = r.first_calls_main;
        r.last_ticks_main = r.first_ticks_main;
        r.last_calls = r.first_calls;
        r.last_report = now;
        r.first_keys = r.last_keys = copy_keys();
        write_phases(r.out, "first frame");
        // Work done before the first frame (startup), totals.
        const double ms_per_tick = tick_seconds() * 1000.0;
        std::fprintf(r.out, "[alchemy perf] startup totals:");
        unsigned column = 0;
        for (unsigned c = 0; c < counter_count; ++c) {
            const auto value = r.first_frame_counters[c];
            if (!value) continue;
            if (column++ % 6 == 0) std::fprintf(r.out, "\n[alchemy perf]   ");
            if (is_ticks(c)) std::fprintf(r.out, " %s=%.1fms", counter_name(c), static_cast<double>(value) * ms_per_tick);
            else std::fprintf(r.out, " %s=%llu", counter_name(c), static_cast<unsigned long long>(value));
        }
        std::fprintf(r.out, "\n");
        std::fflush(r.out);
        return;
    }
    const double seconds = std::chrono::duration<double>(now - r.last_report).count();
    if (seconds < r.period) return;
    std::uint64_t counters[counter_count];
    std::vector<std::uint64_t> calls_main, ticks_main, calls;
    snapshot(counters, calls_main, ticks_main, calls);
    std::uint64_t delta[counter_count];
    for (unsigned c = 0; c < counter_count; ++c) delta[c] = counters[c] - r.last_counters[c];
    std::vector<std::uint64_t> d_calls_main(slot_count), d_ticks_main(slot_count), d_calls(slot_count);
    for (std::size_t i = 0; i < slot_count; ++i) {
        d_calls_main[i] = calls_main[i] - r.last_calls_main[i];
        d_ticks_main[i] = ticks_main[i] - r.last_ticks_main[i];
        d_calls[i] = calls[i] - r.last_calls[i];
    }
    char title[64];
    std::snprintf(title, sizeof(title), "period at %.1f s", now_ms_since_creation() / 1000.0);
    write_summary(r.out, title, seconds, delta[present_count], delta, d_calls_main, d_ticks_main, d_calls);
    auto keys = copy_keys();
    if (delta[present_count]) write_keys(r.out, keys, r.last_keys, 1.0 / static_cast<double>(delta[present_count]));
    r.last_keys = std::move(keys);
    {
        std::lock_guard input(r.input_mutex);
        write_latency(r.out, "from arrival in the window procedure", std::exchange(r.latency_arrival, {}));
        write_latency(r.out, "from the input's posting time", std::exchange(r.latency_posted, {}));
    }
    std::fflush(r.out);
    for (unsigned c = 0; c < counter_count; ++c) r.last_counters[c] = counters[c];
    r.last_calls_main = std::move(calls_main);
    r.last_ticks_main = std::move(ticks_main);
    r.last_calls = std::move(calls);
    r.last_report = now;
}

void report_exit() {
    if (!enabled()) return;
    auto& r = reporter();
    std::lock_guard lock(r.mutex);
    if (r.exit_reported) return;
    r.exit_reported = true;
    r.phases.emplace_back("exit report", now_ms_since_creation());
    write_phases(r.out, "exit");
    std::uint64_t counters[counter_count];
    std::vector<std::uint64_t> calls_main, ticks_main, calls;
    snapshot(counters, calls_main, ticks_main, calls);
    if (r.first_frame_seen) {
        std::uint64_t delta[counter_count];
        for (unsigned c = 0; c < counter_count; ++c) delta[c] = counters[c] - r.first_frame_counters[c];
        for (std::size_t i = 0; i < slot_count; ++i) {
            calls_main[i] -= r.first_calls_main[i];
            ticks_main[i] -= r.first_ticks_main[i];
            calls[i] -= r.first_calls[i];
        }
        const double seconds = std::chrono::duration<double>(Clock::now() - r.first_frame_time).count();
        write_summary(r.out, "since the first frame", seconds, delta[present_count], delta, calls_main, ticks_main, calls);
        if (delta[present_count]) write_keys(r.out, copy_keys(), r.first_keys, 1.0 / static_cast<double>(delta[present_count]));
        std::lock_guard input(r.input_mutex);
        write_latency(r.out, "run, from arrival", r.run_latency_arrival);
        write_latency(r.out, "run, from posting", r.run_latency_posted);
    } else {
        std::fprintf(r.out, "[alchemy perf] no frame was presented; totals:\n");
        for (unsigned c = 0; c < counter_count; ++c)
            if (counters[c]) std::fprintf(r.out, "[alchemy perf]   %s=%llu\n", counter_name(c), static_cast<unsigned long long>(counters[c]));
        // Headless runs (renders, tests): the busiest synthetic calls of the whole run.
        std::vector<std::size_t> order;
        for (std::size_t i = 0; i < calls.size(); ++i)
            if (calls[i]) order.push_back(i);
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return calls[a] > calls[b]; });
        std::fprintf(r.out, "[alchemy perf]   host calls by count, all threads (calls, main-thread calls):\n");
        for (std::size_t k = 0; k < order.size() && k < 40; ++k)
            std::fprintf(r.out, "[alchemy perf]     %12llu %12llu  %s\n", static_cast<unsigned long long>(calls[order[k]]),
                         static_cast<unsigned long long>(calls_main[order[k]]), slot_name(order[k]).c_str());
    }
    std::fflush(r.out);
}

} // namespace alchemy::perf
