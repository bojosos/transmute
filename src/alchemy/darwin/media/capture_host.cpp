#include "alchemy/darwin/media/capture_core.hpp"

#include "alchemy/host/audio.hpp"
#include "alchemy/host/camera.hpp"
#include "alchemy/host/process.hpp"

#include <algorithm>
#include <iostream>
#include <map>

// AVFoundation capture's devices from the host (host::camera): the host's camera list, read once,
// its formats merged by size (the fastest rate of each; pixel formats are the host's concern, the
// pictures come as NV12), and the camera and microphone privacy settings as the host reports them.
namespace alchemy::gui::completion_kit::media::capture {
namespace {

bool no_cameras() {
    static const bool none = [] {
        const auto camera = host::process::environment("ALCHEMY_CAMERA").value_or("");
        if (camera == "none") return true;
        if (camera == "device") return false;
        const auto cloak = host::process::environment("ALCHEMY_GUI_CLOAK").value_or("");
        return !cloak.empty() && cloak != "0";
    }();
    return none;
}

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
        switch (host::camera::camera_access()) {
        case host::camera::Access::allowed: return Access::allowed;
        case host::camera::Access::denied: return Access::denied;
        default: return Access::unknown;
        }
    }
    Access microphone_access() override {
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
