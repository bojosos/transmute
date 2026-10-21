#pragma once

// ALCHEMY_PERF: cheap per-frame and startup instrumentation.
//
//   ALCHEMY_PERF=1        counters (Objective-C, object store, synthetic host
//                         calls, Metal, presentation) and startup phases
//   ALCHEMY_PERF=2        also times every synthetic host call (rdtsc)
//   ALCHEMY_PERF_OUT=path the report goes to this file (appended) instead of stderr
//   ALCHEMY_PERF_PERIOD=s seconds between periodic frame reports (default 2)
//   ALCHEMY_PERF_MEMORY=1 each startup phase is also printed at once with the
//                         process's private bytes and host heap (memory_usage)
//
// Reports: a startup phase table at the first presented frame and at exit,
// a periodic per-presented-frame summary (counter deltas divided by the
// frames presented in the period, top synthetic calls by count and time),
// and the same summary over the whole run at exit. Disabled, every hook is
// one predictable branch on a cached flag.
//
// State is header-inline (zero-initialised statics), so the engine library
// (alchemy_fx_engine) and alchemy_core share it without a link dependency;
// reporting lives in src/alchemy/perf.cpp.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#define ALCHEMY_PERF_RDTSC 1
#endif

namespace alchemy::perf {

enum Counter : unsigned {
    // Objective-C runtime.
    objc_send,                // objc_msgSend/Super entries (ALCHEMY_PERF counting stubs)
    objc_slow,                // dispatcher misses that reached the host lookup
    objc_slow_cached,         // ... answered by the host cache probe (guest probe could not)
    objc_slow_filled,         // ... that filled a class cache
    objc_native_direct,       // host->guest sends answered by a native handler directly
    objc_send_guest,          // host->guest sends that entered guest code
    objc_alloc,               // ObjectStore::create
    objc_dealloc,             // ObjectStore records destroyed
    objc_store_lookups,       // ObjectStore find/peek/retain/release lookups
    // Synthetic host calls (fx host callbacks).
    host_calls,               // every thread
    host_calls_main,          // main thread
    host_ticks_main_top,      // main thread, outermost calls only (rdtsc, ALCHEMY_PERF=2)
    // Metal.
    mtl_command_buffers,
    mtl_commits,
    mtl_render_encoders,
    mtl_compute_encoders,
    mtl_blit_encoders,
    mtl_draws,
    mtl_dispatches,
    mtl_blits,
    mtl_set_buffer,
    mtl_set_bytes,
    mtl_set_bytes_bytes,
    mtl_set_texture,
    mtl_set_sampler,
    mtl_set_pipeline,         // setRenderPipelineState:/setComputePipelineState:
    mtl_pipeline_binds,       // vkCmdBindPipeline
    mtl_descriptor_sets,      // vkAllocateDescriptorSets
    mtl_descriptor_writes,    // VkWriteDescriptorSet entries
    mtl_descriptor_set_reuse, // descriptor sets reused instead of rewritten
    mtl_descriptor_pools,     // descriptor pools acquired
    mtl_argument_tables,      // argument tables written
    mtl_upload_bytes,         // per-command-buffer upload memory handed out
    mtl_vk_barriers,          // vkCmdPipelineBarrier2
    mtl_vk_state,             // dynamic state commands (viewport, scissor, depth, stencil, ...)
    mtl_vk_binds,             // descriptor set / vertex / index buffer binds
    mtl_queue_submits,        // vkQueueSubmit2 (Metal) + vkQueueSubmit (present blit)
    mtl_cpu_waits,            // CPU waits for GPU completion (waitUntilCompleted, timelines)
    mtl_cpu_wait_ticks,
    mtl_pipelines_created,    // VkPipeline creations (render variants and compute)
    mtl_pipeline_create_ticks,
    mtl_libraries,            // newLibraryWithSource:/Data:
    mtl_library_ticks,
    msl_compiles,             // MSL compiler invocations (cache misses)
    msl_cache_hits,
    msl_seed_hits,            // ... of those, answered by the seed set
    msl_compile_ticks,
    msl_lookup_ticks,         // cache key + memory/disk lookup
    mtl_encoder_descriptor_ticks, // renderCommandEncoderWithDescriptor: reading the pass descriptor
    mtl_encoder_begin_ticks,      // ... CommandBuffer::begin_render
    mtl_encoder_object_ticks,     // ... creating the encoder object
    mtl_write_set_ticks,          // descriptor set allocation + writes + argument table
    mtl_source_ticks,         // newLibraryWithSource: source string to UTF-8 + options
    // Presentation.
    present_count,
    present_ticks,            // -[CAMetalDrawable present] host work
    present_slot_wait_ticks,  // waiting for the frame slot's previous blit
    present_acquire_ticks,    // vkAcquireNextImageKHR
    present_queue_ticks,      // vkQueueSubmit + vkQueuePresentKHR
    next_drawable,
    next_drawable_ticks,      // -[CAMetalLayer nextDrawable]
    next_drawable_wait_ticks, // ... blocked for a free drawable
    // Run loop.
    runloop_waits,            // MsgWaitForMultipleObjectsEx with a timeout
    runloop_wait_ticks,
    guest_sleeps,             // usleep / nanosleep
    guest_sleep_ticks,
    counter_count
};

constexpr std::size_t slot_count = 65'536;

struct State {
    std::atomic<int> mode{-1};                      // -1 unread, 0 off, 1 counters, 2 timing
    std::atomic<std::uint64_t> counters[counter_count]{};
    std::atomic<std::uint32_t> main_thread{0};
    // Synthetic call slots (index = (pc - base) / stride).
    std::atomic<std::uint64_t> slot_calls[slot_count]{};
    std::atomic<std::uint64_t> slot_calls_main[slot_count]{};
    std::atomic<std::uint64_t> slot_ticks_main[slot_count]{};   // inclusive, main thread
    std::atomic<std::uint64_t> slot_base{0}, slot_stride{0};
};
inline State g_state;

int read_mode() noexcept;   // perf.cpp

[[nodiscard]] inline int mode() noexcept {
    const auto value = g_state.mode.load(std::memory_order_relaxed);
    return value >= 0 ? value : read_mode();
}
[[nodiscard]] inline bool enabled() noexcept { return mode() > 0; }
[[nodiscard]] inline bool timing() noexcept { return mode() > 1; }

inline void add(Counter counter, std::uint64_t value = 1) noexcept {
    if (enabled()) g_state.counters[counter].fetch_add(value, std::memory_order_relaxed);
}

[[nodiscard]] inline std::uint64_t ticks() noexcept {
#ifdef ALCHEMY_PERF_RDTSC
    return __rdtsc();
#else
    return static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

// Adds the elapsed ticks of a scope to `counter` (and one to `count`, when
// given) while ALCHEMY_PERF is on. Ticks convert to seconds in the report.
class Scope {
public:
    explicit Scope(Counter ticks_counter, Counter count_counter = counter_count) noexcept
        : counter_(ticks_counter), count_(count_counter), start_(enabled() ? ticks() : 0) {}
    ~Scope() {
        if (!start_) return;
        g_state.counters[counter_].fetch_add(ticks() - start_, std::memory_order_relaxed);
        if (count_ != counter_count) g_state.counters[count_].fetch_add(1, std::memory_order_relaxed);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
private:
    Counter counter_, count_;
    std::uint64_t start_;
};

[[nodiscard]] bool is_main_thread() noexcept;

// Synthetic call accounting (fx engine). `index` is the slot number.
struct SlotFrame {
    std::uint64_t start{};
    std::uint32_t index{};
    bool main{}, top{};
};
inline thread_local unsigned t_slot_depth = 0;
inline SlotFrame slot_enter(std::uint32_t index) noexcept {
    SlotFrame frame;
    if (!enabled() || index >= slot_count) return frame;
    frame.index = index;
    frame.main = is_main_thread();
    g_state.slot_calls[index].fetch_add(1, std::memory_order_relaxed);
    g_state.counters[host_calls].fetch_add(1, std::memory_order_relaxed);
    if (frame.main) {
        g_state.slot_calls_main[index].fetch_add(1, std::memory_order_relaxed);
        g_state.counters[host_calls_main].fetch_add(1, std::memory_order_relaxed);
        if (timing()) {
            frame.top = t_slot_depth++ == 0;
            frame.start = ticks();
        }
    }
    return frame;
}
inline void slot_leave(const SlotFrame& frame) noexcept {
    if (!frame.start) return;
    const auto elapsed = ticks() - frame.start;
    g_state.slot_ticks_main[frame.index].fetch_add(elapsed, std::memory_order_relaxed);
    if (frame.top) g_state.counters[host_ticks_main_top].fetch_add(elapsed, std::memory_order_relaxed);
    --t_slot_depth;
}

// Startup phases: the time since process creation, recorded once per name
// (the first occurrence wins). Printed with the first presented frame and at
// exit.
void phase(std::string_view name);
// Names a synthetic call slot or address (report labels). Cheap when off.
void name_address(std::uint64_t address, std::string_view name);
// The name given to `address` (empty when unnamed or ALCHEMY_PERF is off):
// error reports of synthetic calls.
std::string address_name(std::uint64_t address);
// The synthetic call layout, so addresses map to slot indices.
void set_slot_layout(std::uint64_t base, std::uint64_t stride) noexcept;
// A counter kept elsewhere (e.g. a guest memory word): `read` is added to
// the counter's value in every report. Cheap when off.
void set_counter_source(Counter counter, std::function<std::uint64_t()> read);
// Folds the source's last value into the counter and removes it (before
// whatever it reads is destroyed).
void retire_counter_source(Counter counter);
// Keyed counts for top-N lists (e.g. Objective-C slow lookups by message);
// takes a lock: only for rare events. Cheap when off.
void count_key(std::string_view category, std::string_view key, std::uint64_t value = 1);
// An input message arrived (window server): `message_tick` is its posting
// time (GetMessageTime), `now_tick` GetTickCount. The next presented frame
// records the input-to-present latency of the first input since the last one.
void input_received(std::uint32_t message_tick, std::uint32_t now_tick);
// A frame was presented (QuartzCore): drives the periodic report.
void frame_presented();
// Prints the run summary (atexit; idempotent).
void report_exit();
// Process start: records the main thread and installs the exit report.
void start();

} // namespace alchemy::perf
