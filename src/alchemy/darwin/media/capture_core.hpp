#pragma once

// Video capture devices for AVFoundation capture (av_capture.cpp): cameras with their formats, the
// host's privacy settings for the camera and microphone, and streams of NV12 pictures. The host's
// (capture_host.cpp, over host::camera) in alchemy-fx, a fake in tests. Microphones are the HAL's
// input devices (hal_core.hpp).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::capture {

// What the host's privacy settings say about this application using a device: unknown when they
// say nothing either way (reading them shows no prompt).
enum class Access { unknown, allowed, denied };

struct Format {
    std::uint32_t width{}, height{};
    double min_rate{}, max_rate{};     // frames per second
};

struct Camera {
    std::string id, name;
    std::int64_t position{};           // AVCaptureDevicePosition: unspecified 0, back 1, front 2
    std::vector<Format> formats;       // distinct sizes, largest first
};

// One picture as NV12: `height` rows of `width` luma bytes, then `height / 2` rows of `width`
// interleaved Cb Cr bytes (valid during the call only).
struct Picture {
    std::uint32_t width{}, height{};
    std::uint64_t time_ns{};           // host time (mach_absolute_time) when it was taken
    const std::uint8_t* data{};
    std::size_t size{};
};
using Deliver = std::function<void(const Picture&)>;

// A camera's stream: `deliver` runs on a thread of the stream between start and stop (stop waits).
class VideoStream {
public:
    virtual ~VideoStream() = default;
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual void size(std::uint32_t& width, std::uint32_t& height) const = 0;
};

enum class OpenError { none, not_found, denied, failed };

class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<Camera> cameras() = 0;
    virtual Access camera_access() = 0;
    virtual Access microphone_access() = 0;
    // A stream on camera `id` at the format closest to `width` x `height`, at most `rate` frames per
    // second (0: the format's fastest); nullptr with `error` set when it does not open.
    virtual std::unique_ptr<VideoStream> open(const std::string& id, std::uint32_t width, std::uint32_t height, double rate, Deliver deliver,
                                              OpenError& error) = 0;
};

// The host's cameras (host::camera). In cloaked runs (ALCHEMY_GUI_CLOAK) there are none unless
// ALCHEMY_CAMERA=device; ALCHEMY_CAMERA=none lists none in any run.
std::unique_ptr<Backend> make_host_backend();

} // namespace alchemy::gui::completion_kit::media::capture
