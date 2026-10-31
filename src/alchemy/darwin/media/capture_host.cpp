#include "alchemy/darwin/media/capture_core.hpp"

#include "alchemy/host/audio.hpp"
#include "alchemy/host/camera.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/time.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// AVFoundation capture's devices from the host (host::camera): the host's camera list, read once,
// its formats merged by size (the fastest rate of each; pixel formats are the host's concern, the
// pictures come as NV12), and the camera and microphone privacy settings as the host reports them.
// Test devices (opt-in, for page checks; no real device is opened): with ALCHEMY_CAMERA=pattern the
// only camera is "Alchemy Test Pattern" (1280x720 and 640x480, up to 30 frames per second: colour
// bars and a moving white square in NV12, video range), and with ALCHEMY_AUDIO_INPUT=tone the only
// microphone is hal_host.cpp's test tone. Access to a test device is allowed: the host's privacy
// settings are about its real devices, and none is involved.
namespace alchemy::gui::completion_kit::media::capture {
namespace {

enum class CameraMode { device, none, pattern };

CameraMode camera_mode() {
    static const CameraMode mode = [] {
        const auto camera = host::process::environment("ALCHEMY_CAMERA").value_or("");
        if (camera == "none") return CameraMode::none;
        if (camera == "pattern") return CameraMode::pattern;
        if (camera == "device") return CameraMode::device;
        const auto cloak = host::process::environment("ALCHEMY_GUI_CLOAK").value_or("");
        return !cloak.empty() && cloak != "0" ? CameraMode::none : CameraMode::device;
    }();
    return mode;
}
bool no_cameras() { return camera_mode() == CameraMode::none; }
bool tone_microphone() { return host::process::environment("ALCHEMY_AUDIO_INPUT").value_or("") == "tone"; }

constexpr const char* pattern_id = "alchemy-test-pattern";

// The test pattern: eight colour bars (BT.601 video-range Y'CbCr) and a white square moving across them.
class PatternStream final : public VideoStream {
public:
    PatternStream(std::uint32_t width, std::uint32_t height, double rate, Deliver deliver)
        : width_(width), height_(height), rate_(rate > 0 ? std::min(rate, 30.0) : 30.0), deliver_(std::move(deliver)) {}
    ~PatternStream() override { stop(); }
    bool start() override {
        std::lock_guard lock(mutex_);
        if (thread_.joinable()) return true;
        running_ = true;
        thread_ = std::thread([this] { run(); });
        return true;
    }
    void stop() override {
        std::lock_guard lock(mutex_);
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    void size(std::uint32_t& width, std::uint32_t& height) const override { width = width_; height = height_; }

private:
    void run() {
        // White, yellow, cyan, green, magenta, red, blue, black.
        static constexpr std::uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                                    {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
        std::vector<std::uint8_t> picture(static_cast<std::size_t>(width_) * height_ * 3 / 2);
        const auto period = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / rate_));
        auto next = std::chrono::steady_clock::now();
        for (std::uint64_t frame = 0; running_; ++frame) {
            const auto square = height_ / 4, sx = static_cast<std::uint32_t>((frame * 8) % (width_ > square ? width_ - square : 1)), sy = height_ / 2 - square / 2;
            for (std::uint32_t y = 0; y < height_; ++y)
                for (std::uint32_t x = 0; x < width_; ++x) {
                    const bool in_square = x >= sx && x < sx + square && y >= sy && y < sy + square;
                    picture[static_cast<std::size_t>(y) * width_ + x] = in_square ? 235 : bars[x * 8 / width_][0];
                }
            auto* chroma = picture.data() + static_cast<std::size_t>(width_) * height_;
            for (std::uint32_t y = 0; y < height_ / 2; ++y)
                for (std::uint32_t x = 0; x < width_ / 2; ++x) {
                    const bool in_square = 2 * x >= sx && 2 * x < sx + square && 2 * y >= sy && 2 * y < sy + square;
                    const auto& bar = bars[2 * x * 8 / width_];
                    chroma[static_cast<std::size_t>(y) * width_ + 2 * x] = in_square ? 128 : bar[1];
                    chroma[static_cast<std::size_t>(y) * width_ + 2 * x + 1] = in_square ? 128 : bar[2];
                }
            if (running_) deliver_({width_, height_, host::time::monotonic_ns(), picture.data(), picture.size()});
            next += period;
            std::this_thread::sleep_until(next);
        }
    }

    std::uint32_t width_, height_;
    double rate_;
    Deliver deliver_;
    std::mutex mutex_;
    std::atomic<bool> running_{};
    std::thread thread_;
};

class HostStream final : public VideoStream {
public:
    explicit HostStream(host::camera::Stream* stream) : stream_(stream) {}
    ~HostStream() override { host::camera::close(stream_); }
    bool start() override { return static_cast<bool>(host::camera::start(stream_)); }
    void stop() override { host::camera::stop(stream_); }
    void size(std::uint32_t& width, std::uint32_t& height) const override { host::camera::size(stream_, width, height); }

private:
    host::camera::Stream* stream_;
};

class HostCameras final : public Backend {
public:
    std::vector<Camera> cameras() override {
        if (no_cameras()) return {};
        if (camera_mode() == CameraMode::pattern) return {{pattern_id, "Alchemy Test Pattern", 0, {{1280, 720, 1, 30}, {640, 480, 1, 30}}}};
        const auto listed = host::camera::cameras();
        if (!listed) return {};
        std::vector<Camera> out;
        for (const auto& c : *listed) {
            Camera camera;
            camera.id = c.id;
            camera.name = c.name.empty() ? "Camera" : c.name;
            camera.position = c.position == host::camera::Camera::Position::back ? 1 : c.position == host::camera::Camera::Position::front ? 2 : 0;
            std::map<std::pair<std::uint32_t, std::uint32_t>, Format> by_size;
            for (const auto& f : c.formats) {
                auto& slot = by_size[{f.width, f.height}];
                if (!slot.width) slot = {f.width, f.height, f.min_rate, f.max_rate};
                slot.min_rate = std::min(slot.min_rate, f.min_rate);
                slot.max_rate = std::max(slot.max_rate, f.max_rate);
            }
            for (const auto& [size, format] : by_size) camera.formats.push_back(format);
            std::sort(camera.formats.begin(), camera.formats.end(), [](const Format& a, const Format& b) {
                return static_cast<std::uint64_t>(a.width) * a.height > static_cast<std::uint64_t>(b.width) * b.height;
            });
            out.push_back(std::move(camera));
        }
        return out;
    }
    Access camera_access() override {
        if (camera_mode() == CameraMode::pattern) return Access::allowed;
        switch (host::camera::camera_access()) {
        case host::camera::Access::allowed: return Access::allowed;
        case host::camera::Access::denied: return Access::denied;
        default: return Access::unknown;
        }
    }
    Access microphone_access() override {
        if (tone_microphone()) return Access::allowed;
        switch (host::audio::microphone_access()) {
        case host::audio::Access::allowed: return Access::allowed;
        case host::audio::Access::denied: return Access::denied;
        default: return Access::unknown;
        }
    }
    std::unique_ptr<VideoStream> open(const std::string& id, std::uint32_t width, std::uint32_t height, double rate, Deliver deliver,
                                      OpenError& error) override {
        error = OpenError::none;
        if (no_cameras()) { error = OpenError::not_found; return nullptr; }
        if (camera_mode() == CameraMode::pattern) {
            if (id != pattern_id) { error = OpenError::not_found; return nullptr; }
            const bool small = width && height && static_cast<std::uint64_t>(width) * height <= 640ULL * 480;
            return std::make_unique<PatternStream>(small ? 640 : 1280, small ? 480 : 720, rate, std::move(deliver));
        }
        auto opened = host::camera::open(id, width, height, rate, [deliver = std::move(deliver)](const host::camera::Picture& p) {
            deliver(Picture{p.width, p.height, p.time_ns, p.data, p.size});
        });
        if (!opened) {
            error = opened.error() == host::Error::access_denied ? OpenError::denied : opened.error() == host::Error::not_found ? OpenError::not_found : OpenError::failed;
            std::cerr << "alchemy: camera did not open (" << host::name(opened.error()) << ")" << std::endl;
            return nullptr;
        }
        return std::make_unique<HostStream>(*opened);
    }
};

} // namespace

std::unique_ptr<Backend> make_host_backend() { return std::make_unique<HostCameras>(); }

} // namespace alchemy::gui::completion_kit::media::capture
