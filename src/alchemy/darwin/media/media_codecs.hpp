#pragma once

// Media decoders (host-only core, media group) over the vendored libraries in third_party/
// (cmake/darwin/media_codecs.cmake): packets in, as the containers in media_container.hpp cut
// them, and decoded media out.
// - Audio: MPEG audio layers I-III ('.mp1', '.mp2', '.mp3', minimp3), Opus ('opus', libopus,
//   always 48 kHz), FLAC ('flac', libFLAC), Apple Lossless ('alac', Apple's open-source ALAC
//   codec: 16, 20, 24 or 32 bits, 1-8 channels) and AAC ('mp4a' or 'aac ', through the host
//   system's decoder, host/media_codec.hpp, where it has one), as interleaved 32-bit float PCM.
//   An AAC packet decodes to its 1024 frames as a decoder that hides nothing would give them (the
//   first block after an open or reset is the codec's own delay), so the container's priming trims
//   it exactly.
// The codec configuration is what the container stores: dOps or OpusHead for Opus; dfLa, a
// Matroska "fLaC" CodecPrivate or a bare STREAMINFO block for FLAC; for ALAC the
// ALACSpecificConfig (the magic cookie, optionally after 'frma' and 'alac' atoms, or the MP4
// 'alac' box payload with its version and flags); for AAC an MP4 esds payload, an ES descriptor (CAF's
// and AudioConverter's magic cookie) or the AudioSpecificConfig itself (Matroska); nothing for MPEG
// audio. Codecs without a decoder here (H.264, HEVC, ProRes, AAC on a host without one, ...) are not
// supported; callers answer their own "format not supported" error for them.
// - Audio encoding: ALAC only (AudioEncoder below).

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

// An audio encoder. Samples go in interleaved, as integers left-justified in 32 bits (a 16-bit
// sample is its value << 16), at most `frames_per_packet` frames per call; only a stream's last
// packet may be shorter. Each call makes one packet, as a container stores it.
class AudioEncoder {
public:
    virtual ~AudioEncoder() = default;
    // Replaces `packet` with the encoded frames. False, with `why`, when they cannot be encoded.
    virtual bool encode(std::span<const std::int32_t> samples, std::vector<std::uint8_t>& packet, std::string& why) = 0;
    // The magic cookie describing the packets encoded so far (ALAC: the ALACSpecificConfig with
    // their largest size and average bit rate, then the channel layout info above two channels).
    virtual std::vector<std::uint8_t> cookie() = 0;

    double rate{};
    std::uint32_t channels{};
    std::uint32_t bits{};               // the source bit depth the encoder keeps
    std::uint32_t frames_per_packet{};
    std::uint32_t max_packet_bytes{};   // no packet is larger
};

bool audio_encoder_available(std::uint32_t codec);

// An encoder for `codec`: ALAC ('alac') of 16, 20, 24 or 32 bits, 1-8 channels, an integer rate
// and 1-16384 frames per packet (0: the codec's default, 4096). Null, with `why`, otherwise.
std::unique_ptr<AudioEncoder> make_audio_encoder(std::uint32_t codec, double rate, std::uint32_t channels, std::uint32_t bits,
                                                 std::uint32_t frames_per_packet, std::string& why);

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

// Video: VP8 ('vp08') and VP9 ('vp09', profiles 0-3, libvpx), AV1 ('av01', dav1d), and H.264 ('avc1') and HEVC
// ('hvc1', 8 and 10 bits) through the host system's decoders (host/media_codec.hpp) where it has them; their
// configuration is the avcC or hvcC record, their packets length-prefixed NAL units of its length size. Packets go in
// with their presentation times; pictures come out in presentation order. The host's decoders give no colour
// description, so H.264 and HEVC pictures carry none (unspecified: the size decides the matrix) and video range.
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
    // Whether a packet's picture may come out only after later packets (reordering, decoder latency); false: a packet
    // that gives no picture at once shows none (a hidden frame).
    [[nodiscard]] virtual bool holds_pictures() const { return false; }
};

bool video_decoder_available(std::uint32_t codec);
std::unique_ptr<VideoDecoder> make_video_decoder(std::uint32_t codec, std::span<const std::uint8_t> config, std::string& why);

} // namespace alchemy::gui::completion_kit::media::codecs
