#pragma once

// Host area "media_codec" (owner: fw-media). The rules are in include/alchemy/host/host.hpp. The host
// system's own media encoders and decoders (on Windows, the Media Foundation transforms the system provides; on
// Linux, GStreamer and the plug-ins the system has; nothing is shipped with Alchemy): H.264 and HEVC video from and to NV12 pictures (P010 above 8 bits), AAC
// audio from and to 16-bit PCM. VTCompressionSession, VTDecompressionSession, AudioConverter, the audio files,
// AVAssetReader/Writer, AVPlayer and AVAssetExportSession run on this (src/alchemy/darwin/media/). A machine
// without a given encoder or decoder lists none and fails to open one: callers answer their own documented
// errors. GUI-only: the backends are src/alchemy/host/<backend>/gui/media_codec.cpp, compiled into alchemy-fx.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::media_codec {

enum class Codec : std::uint8_t { h264, hevc, aac };
enum class Direction : std::uint8_t { encode, decode };

// One codec component of the host.
struct Component {
    std::string id;                    // the host's stable identifier for it (for the backend, opaque elsewhere)
    std::string name;                  // for people
    Codec codec{};
    Direction direction{Direction::encode};
    bool hardware{};
    // Audio encoders: what they take (rates in hertz, bit rates in bits per second), as the component
    // itself reports them.
    std::vector<std::uint32_t> sample_rates, channel_counts, bit_rates;
    // Audio encoders: the frames of delay their output starts with (the priming a player skips), as measured
    // for this component (measure_delay below); 0: not known. Audio decoders: the frames they hold back to hide
    // their own delay (given at the drain), as measured; see AudioDecoder.
    std::uint32_t delay{};
};

// The host's encoders of `codec`, the preferred first. Error::unsupported: none on this host's backend.
[[nodiscard]] Result<std::vector<Component>> encoders(Codec codec);
// The host's decoders of `codec`, the preferred first.
[[nodiscard]] Result<std::vector<Component>> decoders(Codec codec);

// An encoded sample: the bytes, times in units of 100 nanoseconds.
struct Sample {
    std::vector<std::uint8_t> data;
    std::int64_t pts{}, dts{}, duration{};
    bool keyframe{};
};

// ---- video ------------------------------------------------------------------------------------------

enum class Profile : std::uint8_t { automatic, h264_baseline, h264_main, h264_high, hevc_main, hevc_main10 };

struct VideoSettings {
    Codec codec{Codec::h264};
    std::uint32_t width{}, height{};       // even
    double frame_rate{30};                 // expected frames per second
    std::uint32_t bit_rate{};              // average bits per second (0: the encoder's choice)
    std::uint32_t keyframe_interval{};     // at most this many frames between keyframes (0: the encoder's choice)
    Profile profile{Profile::automatic};
    // The level as the stream's level_idc (H.264: 10 times the level, 31 for 3.1; HEVC: general_level_idc, 30 times
    // the level, 123 for 4.1); 0: the encoder's choice.
    std::uint32_t level{};
    bool real_time{};                      // favour latency over compression
    bool frame_reordering{};               // allow B-frames (output in decode order, dts before pts)
    bool require_hardware{};
    std::string component;                 // a Component id (empty: the first that accepts the settings)
};

// An NV12 picture: `height` rows of luma, then `height / 2` rows of interleaved Cb Cr (valid during the call).
struct Picture {
    std::uint32_t width{}, height{};
    const std::uint8_t* luma{};
    std::size_t luma_stride{};
    const std::uint8_t* chroma{};
    std::size_t chroma_stride{};
    std::int64_t pts{}, duration{};        // 100 ns units
};

class VideoEncoder;

// Opens an encoder for `settings`. Error::not_found: no encoder of the codec (or the named component, or a
// hardware one when required); Error::invalid_argument: settings no encoder accepts; Error::unsupported:
// no media encoders on this host.
[[nodiscard]] Result<VideoEncoder*> open_video(const VideoSettings& settings);
// The component that encodes.
[[nodiscard]] const Component& component(const VideoEncoder* encoder) noexcept;
// Encodes a picture (a keyframe when `keyframe`); the samples ready so far are appended to `out`, in decode
// order: length-prefixed NAL units (4-byte big-endian lengths), without parameter sets or access-unit
// delimiters.
[[nodiscard]] Status encode(VideoEncoder* encoder, const Picture& picture, bool keyframe, std::vector<Sample>& out);
// No more pictures: every remaining sample is appended.
[[nodiscard]] Status drain(VideoEncoder* encoder, std::vector<Sample>& out);
// The stream's parameter sets as NAL units without start codes or lengths (H.264: SPS then PPS; HEVC:
// VPS, SPS, PPS), once the first keyframe is out.
[[nodiscard]] std::vector<std::vector<std::uint8_t>> parameter_sets(const VideoEncoder* encoder);
void close(VideoEncoder* encoder) noexcept;

// ---- audio ------------------------------------------------------------------------------------------

struct AudioSettings {
    Codec codec{Codec::aac};
    std::uint32_t sample_rate{}, channels{};
    std::uint32_t bit_rate{};              // bits per second (0: the encoder's choice; else the nearest it takes)
    std::string component;                 // a Component id (empty: the first that accepts the settings)
};

class AudioEncoder;

// Error::not_found: no encoder of the codec or the named component; Error::invalid_argument: a rate or
// channel count no encoder takes; Error::unsupported: no media encoders on this host.
[[nodiscard]] Result<AudioEncoder*> open_audio(const AudioSettings& settings);
[[nodiscard]] const Component& component(const AudioEncoder* encoder) noexcept;
// The bit rate it encodes at.
[[nodiscard]] std::uint32_t bit_rate(const AudioEncoder* encoder) noexcept;
// Frames each packet holds (1024 for AAC).
[[nodiscard]] std::uint32_t frames_per_packet(const AudioEncoder* encoder) noexcept;
// The codec configuration (AAC: the AudioSpecificConfig of ISO/IEC 14496-3).
[[nodiscard]] std::vector<std::uint8_t> configuration(const AudioEncoder* encoder);
// Encodes interleaved 16-bit samples; the packets ready so far (raw AAC frames, times as the encoder
// stamps them) are appended to `out`.
[[nodiscard]] Status encode(AudioEncoder* encoder, std::span<const std::int16_t> samples, std::vector<Sample>& out);
// No more samples: every remaining packet is appended.
[[nodiscard]] Status drain(AudioEncoder* encoder, std::vector<Sample>& out);
void close(AudioEncoder* encoder) noexcept;

// Test support (the host test that keeps each component's recorded delay honest): the delay of the encoder
// `settings` opens, measured by decoding what it makes of a noise signal with the host's own decoder and finding
// the shift that matches the input, plus any frames the decoder held back to hide its own delay (a player that
// trims by the priming sees those as delay too). Nothing decoded leaves this call. Error::not_found: no decoder.
[[nodiscard]] Result<std::uint32_t> measure_delay(const AudioSettings& settings);

// ---- video decoding ---------------------------------------------------------------------------------

struct VideoDecoderSettings {
    Codec codec{Codec::h264};
    // The parameter sets as NAL units without start codes or lengths (H.264: SPS, PPS; HEVC: VPS, SPS, PPS).
    std::vector<std::vector<std::uint8_t>> parameter_sets;
    std::uint32_t width{}, height{};       // the coded size when known (some decoders want it up front)
    std::string component;                 // a Component id (empty: the first that accepts the stream)
};

// A decoded picture: NV12 (8 bits) or P010 (above 8 bits: 16-bit little-endian samples, the value in the high bits),
// `height` rows of luma then `height / 2` rows of interleaved Cb Cr, of the displayed size.
struct DecodedPicture {
    std::uint32_t width{}, height{}, bit_depth{8};
    std::vector<std::uint8_t> luma, chroma;
    std::size_t luma_stride{}, chroma_stride{};
    std::int64_t pts{}, duration{};        // 100 ns units, as the samples gave them
};

class VideoDecoder;

// Error::not_found: no decoder of the codec (or the named component); Error::invalid_argument: parameter sets no
// decoder takes; Error::unsupported: no media decoders on this host.
[[nodiscard]] Result<VideoDecoder*> open_video_decoder(const VideoDecoderSettings& settings);
[[nodiscard]] const Component& component(const VideoDecoder* decoder) noexcept;
// Decodes a sample (length-prefixed NAL units, 4-byte big-endian lengths, as encode gives them); the pictures ready
// so far are appended to `out`, in presentation order.
[[nodiscard]] Status decode(VideoDecoder* decoder, const Sample& sample, std::vector<DecodedPicture>& out);
// No more samples: every remaining picture is appended.
[[nodiscard]] Status drain(VideoDecoder* decoder, std::vector<DecodedPicture>& out);
// Forgets every sample it holds (a seek); decoding resumes at a keyframe.
void flush(VideoDecoder* decoder) noexcept;
void close(VideoDecoder* decoder) noexcept;

// ---- audio decoding ---------------------------------------------------------------------------------

struct AudioDecoderSettings {
    Codec codec{Codec::aac};
    std::uint32_t sample_rate{}, channels{};
    std::vector<std::uint8_t> configuration;   // AAC: the AudioSpecificConfig
    std::string component;
};

class AudioDecoder;

// A decoder may hold back frames to hide its own delay: the first `held` frames of a stream come out only at the
// drain (Microsoft's AAC decoder holds one 1024-frame block). component(decoder).delay says how many, as measured
// for it (the host GUI test keeps it honest); 0 when it holds none.
[[nodiscard]] Result<AudioDecoder*> open_audio_decoder(const AudioDecoderSettings& settings);
[[nodiscard]] const Component& component(const AudioDecoder* decoder) noexcept;
// Decodes one packet (a raw AAC frame); interleaved 16-bit samples ready so far are appended to `out`.
[[nodiscard]] Status decode(AudioDecoder* decoder, std::span<const std::uint8_t> packet, std::vector<std::int16_t>& out);
[[nodiscard]] Status drain(AudioDecoder* decoder, std::vector<std::int16_t>& out);
// Forgets what it holds (a seek).
void flush(AudioDecoder* decoder) noexcept;
void close(AudioDecoder* decoder) noexcept;

} // namespace alchemy::host::media_codec
