#pragma once

// Media decoders (host-only core, media group) over the vendored libraries in third_party/
// (cmake/darwin/media_codecs.cmake): packets in, as the containers in media_container.hpp cut
// them, and decoded media out.
// - Audio: MPEG audio layers I-III ('.mp1', '.mp2', '.mp3', minimp3), Opus ('opus', libopus,
//   always 48 kHz) and FLAC ('flac', libFLAC), as interleaved 32-bit float PCM.
// The codec configuration is what the container stores: dOps or OpusHead for Opus; dfLa, a
// Matroska "fLaC" CodecPrivate or a bare STREAMINFO block for FLAC; nothing for MPEG audio.
// Codecs without a decoder here (AAC, ALAC, H.264, HEVC, ProRes, ...) are not supported;
// callers answer their own "format not supported" error for them.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::codecs {

class AudioDecoder {
public:
    virtual ~AudioDecoder() = default;
    // Appends the packet's frames, interleaved, to `out`. False, with `why`, for a packet the
    // decoder rejects (it stays usable for the next one).
    virtual bool decode(std::span<const std::uint8_t> packet, std::vector<float>& out, std::string& why) = 0;
    // Forgets the history (after a seek).
    virtual void reset() = 0;

    double rate{};                      // output frames per second
    std::uint32_t channels{};           // output channels
    std::uint32_t bits{};               // the source's bits per sample (0: not meaningful)
    std::uint32_t max_frames{};         // the most frames one packet decodes to
};

bool audio_decoder_available(std::uint32_t codec);

// A decoder for `codec` (a container::Track codec fourcc). `rate` and `channels` are the
// container's; the configuration's values take precedence where it has them. Null, with `why`,
// for unsupported codecs or unusable configurations.
std::unique_ptr<AudioDecoder> make_audio_decoder(std::uint32_t codec, double rate, std::uint32_t channels,
                                                 std::span<const std::uint8_t> config, std::string& why);

// A decoded picture: Y'CbCr planes of the displayed size, one byte per sample for 8 bits and two
// (little endian) above that. Chroma planes are subsampled by the shifts; a monochrome picture
// has only the luma plane. Colour description codes are those of ISO/IEC 23091-2 (2: unspecified).
struct VideoFrame {
    struct Plane {
        std::vector<std::uint8_t> data;
        std::uint32_t width{}, height{}, stride{};   // samples, rows, bytes per row
    };
    std::uint32_t width{}, height{}, bit_depth{8};
    std::uint32_t chroma_shift_x{1}, chroma_shift_y{1};
    bool monochrome{}, full_range{};
    std::uint8_t primaries{2}, transfer{2}, matrix{2};
    std::int64_t pts{};
    Plane planes[3];
};

// Video: VP8 ('vp08') and VP9 ('vp09', profiles 0-3, libvpx), AV1 ('av01', dav1d). Packets go in
// with their presentation times; pictures come out in presentation order.
class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // Decodes one packet (a sample of the container). False, with `why`, when it is rejected.
    virtual bool send(std::span<const std::uint8_t> packet, std::int64_t pts, std::string& why) = 0;
    // The next decoded picture, if one is ready.
    virtual bool receive(VideoFrame& out) = 0;
    // No more packets: what the decoder holds becomes receivable.
    virtual void drain() = 0;
    // Forgets everything (after a seek); decoding resumes at a keyframe.
    virtual void reset() = 0;
};

bool video_decoder_available(std::uint32_t codec);
std::unique_ptr<VideoDecoder> make_video_decoder(std::uint32_t codec, std::span<const std::uint8_t> config, std::string& why);

} // namespace alchemy::gui::completion_kit::media::codecs
