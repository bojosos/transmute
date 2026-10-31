#pragma once

// Host area "camera" (owner: fw-media). The rules are in include/alchemy/host/host.hpp. Video capture
// devices: enumeration with their formats, the camera's privacy setting, and streams of pictures as
// NV12. AVFoundation capture (src/alchemy/darwin/media/av_capture.cpp) runs on this. GUI-only: the
// backends are src/alchemy/host/<backend>/gui/camera.cpp, compiled into alchemy-fx.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::camera {

// One capture format the device offers natively.
struct Format {
    std::uint32_t width{}, height{};
    double min_rate{}, max_rate{};     // frames per second
    std::uint32_t subtype{};           // the native pixel format as a four-character code ("NV12", "YUY2", "MJPG")
};

struct Camera {
    std::string id;                    // the host's stable identifier
    std::string name;                  // for people
    enum class Position { unspecified, front, back } position{};
    std::vector<Format> formats;       // distinct sizes and pixel formats, the fastest rate of each
};

// The video capture devices. Listing their formats opens each device briefly (its light may blink).
[[nodiscard]] Result<std::vector<Camera>> cameras();

// What the host's privacy settings say about this (desktop) application using the camera: unknown
// when they say nothing either way. Reading them shows no prompt.
enum class Access { unknown, allowed, denied };
[[nodiscard]] Access camera_access();

// One picture as NV12: `height` rows of `width` luma bytes, then `height / 2` rows of `width`
// interleaved Cb Cr bytes, tightly packed (valid during the call only).
struct Picture {
    std::uint32_t width{}, height{};
    std::uint64_t time_ns{};           // time::monotonic_ns when the backend took it
    const std::uint8_t* data{};
    std::size_t size{};
};
// Called on a thread of the stream for each picture; must not call back into the stream.
using Deliver = std::function<void(const Picture&)>;

class Stream;

// Opens the camera `id` at the native format closest to `width` x `height` (0: its first) and at most
// `rate` frames per second (0: its fastest), delivering NV12 pictures of that size.
// Error::not_found: no such camera; Error::access_denied: the privacy settings deny the camera;
// Error::unsupported: no video capture on this host.
[[nodiscard]] Result<Stream*> open(std::string_view id, std::uint32_t width, std::uint32_t height, double rate, Deliver deliver);
// The size pictures come in.
void size(const Stream* stream, std::uint32_t& width, std::uint32_t& height) noexcept;
[[nodiscard]] Status start(Stream* stream);
// Stops; when it returns, `deliver` is not running and is not called again until a start.
void stop(Stream* stream) noexcept;
void close(Stream* stream) noexcept;

} // namespace alchemy::host::camera
