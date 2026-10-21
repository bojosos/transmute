// ALCHEMY_FX_PROFILE_WINDOW: fx sampling-profiler window tied to presented
// frames (include/alchemy/fx_profile_window.hpp).

#include "alchemy/fx_profile_window.hpp"

#include "fx/host/env.hpp"
#include "fx/host/time.hpp"
#include "transmute/fx/process.hpp"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace alchemy::fx_profile_window {
namespace {

enum class Mode { unread, off, first_frame, period };

struct Config {
    Mode mode = Mode::off;
    std::uint64_t seconds_ms = 0;   // first-frame: window length; period: period
};

std::atomic<Mode> g_mode{Mode::unread};
Config g_config;
std::mutex g_lock;
std::uint64_t g_window_start = 0;   // 0 = no frame yet
bool g_stopped = false;

Config read_config() {
    Config config;
    char value[64]{};
    const auto n = transmute::fx::host::get_env("ALCHEMY_FX_PROFILE_WINDOW", value, sizeof(value));
    if (n == 0 || n >= sizeof(value)) return config;
    if (std::strncmp(value, "first-frame", 11) == 0) {
        config.mode = Mode::first_frame;
        if (value[11] == ',') config.seconds_ms = static_cast<std::uint64_t>(std::atof(value + 12) * 1000.0);
    } else if (std::strncmp(value, "period=", 7) == 0) {
        config.seconds_ms = static_cast<std::uint64_t>(std::atof(value + 7) * 1000.0);
        if (config.seconds_ms) config.mode = Mode::period;
    }
    return config;
}

} // namespace

void frame_presented() noexcept {
    Mode mode = g_mode.load(std::memory_order_acquire);
    if (mode == Mode::off) return;
    std::lock_guard lock(g_lock);
    if (mode == Mode::unread) {
        g_config = read_config();
        g_mode.store(g_config.mode, std::memory_order_release);
        mode = g_config.mode;
        if (mode == Mode::off) return;
    }
    const std::uint64_t now = transmute::fx::host::monotonic_ms();
    try {
        if (g_window_start == 0) {
            g_window_start = now;
            transmute::fx::profiler_reset();
            transmute::fx::profiler_set_enabled(true);
            return;
        }
        if (mode == Mode::first_frame) {
            if (!g_stopped && g_config.seconds_ms && now - g_window_start >= g_config.seconds_ms) {
                g_stopped = true;
                transmute::fx::profiler_set_enabled(false);
                g_mode.store(Mode::off, std::memory_order_release);   // done
            }
        } else if (now - g_window_start >= g_config.seconds_ms) {
            g_window_start = now;
            transmute::fx::profiler_reset();
        }
    } catch (...) {
        // The profiler never throws here; frames must not fail because of it.
    }
}

} // namespace alchemy::fx_profile_window
