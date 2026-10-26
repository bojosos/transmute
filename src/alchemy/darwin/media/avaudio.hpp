#pragma once

// AVFAudio's buffers, files, players and engine (media group-internal):
//   avaudio_buffers.cpp  AVAudioTime, AVAudioPCMBuffer, AVAudioFile, and the helpers below
//   avaudio_player.cpp   AVAudioPlayer
//   avaudio_engine.cpp   AVAudioEngine and its nodes (output, input, mixer, player)
// AVAudioFormat and AVAudioChannelLayout are avfoundation.cpp's; the helpers here use them through
// their messages. The classes are published under AVFAudio and AVFoundation (which re-exports it).

#include "alchemy/darwin/media/audio_file.hpp"
#include "alchemy/darwin/media/media_support.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace alchemy::gui::completion_kit::media::avaudio {

inline constexpr std::string_view av_foundation = "/System/Library/Frameworks/AVFoundation.framework/Versions/A/AVFoundation";
inline constexpr std::string_view avf_audio = "/System/Library/Frameworks/AVFAudio.framework/Versions/A/AVFAudio";

void install_avaudio_buffers(Support& support);
void install_avaudio_player(Support& support);
void install_avaudio_engine(Support& support);

// The class `name` (made when missing) published under AVFAudio and AVFoundation.
Address declare(Support& support, std::string_view name, std::string_view superclass);

// AVAudioPCMBuffer's host state: the frames live in guest memory, one block per plane (one plane
// for interleaved formats, one per channel otherwise).
struct PcmBuffer final : ObjectPayload {
    Address format{};                  // retained AVAudioFormat
    audio_file::Pcm pcm;
    std::uint64_t common{};            // AVAudioCommonFormat (0: other)
    std::uint32_t capacity{}, length{};
    Address data{};                    // the planes, plane_bytes() apart
    Address pointers{};                // one pointer per plane (the channel data arrays)
    Address list{};                    // AudioBufferList over the planes
    [[nodiscard]] std::uint32_t planes() const { return pcm.non_interleaved ? pcm.channels : 1; }
    [[nodiscard]] std::uint64_t plane_bytes() const { return static_cast<std::uint64_t>(capacity) * pcm.frame_bytes(); }
};

// AVAudioFile's host state: a file open for reading (`audio`), or one being written (`writer`).
struct AudioFileState final : ObjectPayload {
    Address url{};                     // retained NSURL
    GuestAudio audio;
    bool open{};
    Address writer{};                  // ExtAudioFileRef of a file opened for writing
    std::uint64_t written{};           // frames written
    Address file_format{}, processing_format{};   // retained AVAudioFormats
    audio_file::Pcm processing;
    std::uint64_t position{};
};

// An AVAudioFormat's stream description (40 bytes; empty when `format` is not one) and its PCM
// layout (nullopt for other formats).
std::vector<std::uint8_t> format_asbd(Support& support, Address format);
std::optional<audio_file::Pcm> format_pcm(Support& support, Address format);
// A +1 AVAudioFormat for `pcm` (more than two channels: with `layout`, an AudioChannelLayout, or
// discrete channels in order).
Address make_format(Support& support, const audio_file::Pcm& pcm, const std::vector<std::uint8_t>& layout = {});
// The deinterleaved float32 format of `rate` and `channels` (+1).
Address standard_format(Support& support, double rate, std::uint32_t channels);
// A +1 AVAudioPCMBuffer of `format` (nil when the format is not linear PCM).
Address make_buffer(Support& support, Address format, std::uint32_t capacity);
// A buffer's first frameLength frames, interleaved in its channels.
std::vector<float> buffer_samples(Support& support, const PcmBuffer& buffer);
// Writes `frames` interleaved frames (`channels` each) into a buffer and sets its frameLength.
void fill_buffer(Support& support, PcmBuffer& buffer, std::span<const double> interleaved, std::uint32_t channels, std::uint32_t frames);

// Audio decoded into float samples.
struct Decoded {
    std::vector<float> samples;        // interleaved
    std::uint32_t channels{};
    double rate{};
    [[nodiscard]] std::uint64_t frames() const { return channels ? samples.size() / channels : 0; }
};
// The frames [start, start + count) of an open file (clamped to its end): an OSStatus.
std::int32_t decode_file(Support& support, const GuestAudio& audio, Decoded& out, std::uint64_t start = 0,
                         std::uint64_t count = std::numeric_limits<std::uint64_t>::max());
// A whole file in memory (WAVE, AIFF, CAF linear PCM): an OSStatus.
std::int32_t decode_bytes(std::span<const std::uint8_t> bytes, Decoded& out);

// A +1 AVAudioTime (either time may be absent).
Address make_time(Support& support, std::optional<std::uint64_t> host_time, std::optional<double> sample_time, double rate);
struct TimeState final : ObjectPayload {
    std::uint64_t host{};
    double sample{};
    double rate{};
    bool host_valid{}, sample_valid{};
};

} // namespace alchemy::gui::completion_kit::media::avaudio
