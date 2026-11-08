#pragma once

// H.264 and HEVC encoding for the media group (host-only core): the host system's video encoders
// (alchemy/host/media_codec.hpp) behind a backend seam the tests replace with a fake. An encoder takes NV12
// pictures and gives length-prefixed NAL units (4-byte big-endian lengths, without parameter sets) in decode
// order; the parameter sets come separately once the first keyframe is out. Each encoder has an Alchemy encoder
// ID ("com.alchemy.mediafoundation.h264.<n>" and ".hevc.<n>" for the host's, in the host's order): what
// VTCopyVideoEncoderList lists and kVTVideoEncoderSpecification_EncoderID names, never Apple's IDs.

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::video_encoding {

enum class Codec : std::uint8_t { h264, hevc };
enum class Profile : std::uint8_t { automatic, h264_baseline, h264_main, h264_high, hevc_main, hevc_main10 };

struct EncoderInfo {
    std::string id;          // the Alchemy encoder ID
    std::string name;        // for people
    Codec codec{};
    bool hardware{};
};

struct Settings {
    Codec codec{};
    std::uint32_t width{}, height{};   // even
    double frame_rate{30};
    std::uint32_t bit_rate{};          // average bits per second (0: the encoder's choice)
    std::uint32_t keyframe_interval{}; // frames (0: the encoder's choice)
    Profile profile{};
    std::uint32_t level{};             // level_idc (H.264: 10 x the level; HEVC: 30 x); 0: the encoder's choice
    bool real_time{}, frame_reordering{}, require_hardware{}, require_software{};
    std::string encoder_id;            // empty: the first encoder of the codec that takes the settings
};

// An NV12 picture (valid during the call); times in units of 100 nanoseconds.
struct Picture {
    std::uint32_t width{}, height{};
    const std::uint8_t* luma{};
    std::size_t luma_stride{};
    const std::uint8_t* chroma{};
    std::size_t chroma_stride{};
    std::int64_t pts{}, duration{};
};

struct Packet {
    std::vector<std::uint8_t> data;    // length-prefixed NAL units
    std::int64_t pts{}, dts{}, duration{};
    bool keyframe{};
};

class Encoder {
public:
    virtual ~Encoder() = default;
    // Appends the packets ready so far. False on an encoder failure.
    virtual bool encode(const Picture& picture, bool keyframe, std::vector<Packet>& out) = 0;
    // No more pictures for now: every packet still held. The encoder takes pictures again afterwards.
    virtual bool drain(std::vector<Packet>& out) = 0;
    // H.264: SPS then PPS; HEVC: VPS, SPS, PPS (empty before the first keyframe).
    [[nodiscard]] virtual std::vector<std::vector<std::uint8_t>> parameter_sets() const = 0;
    [[nodiscard]] virtual const EncoderInfo& info() const = 0;
};

enum class OpenError : std::uint8_t { none, not_found, invalid_settings };

struct Backend {
    std::function<std::vector<EncoderInfo>()> encoders;
    std::function<std::unique_ptr<Encoder>(const Settings&, OpenError&)> open;
};
// Tests: replaces the host's encoders (an empty Backend restores them).
void set_backend(Backend backend);

// Every encoder, H.264 first.
[[nodiscard]] std::vector<EncoderInfo> encoders();
// Opens an encoder for `settings`: OpenError::not_found when no encoder of the codec (or the named one, or a
// hardware or software one when required) exists, invalid_settings when none of them takes the settings.
[[nodiscard]] std::unique_ptr<Encoder> open(const Settings& settings, OpenError& error);

} // namespace alchemy::gui::completion_kit::media::video_encoding
