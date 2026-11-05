#pragma once

// AudioFileStream core (host-only, media group): a parser fed a file's bytes in pieces, as they
// arrive from a network stream, that reports the stream's properties once its header has been
// read and then hands out whole packets.
//
// Streams: WAVE, AIFF, AIFC and CAF holding linear PCM (audio_file_core.hpp; constant bit rate, so
// packets are frames and carry no descriptions), and MPEG audio (layers I-III, MPEG-1, 2 and 2.5)
// with or without ID3v2 tags in front and a Xing/Info frame (its frame and byte counts, and the
// LAME tag's encoder delay and padding as the packet table). MPEG frames are found by their
// headers; a frame counts once the next frame's header agrees with it, so junk and lost
// synchronisation are skipped. Other streams answer kAudioFileStreamError_UnsupportedFileType, and
// ADTS AAC kAudioFileStreamError_UnsupportedDataFormat (no AAC support).
//
// Seeking takes a packet number and answers the byte offset, from the start of the audio data, of
// the bytes to parse next: exact for PCM and for MPEG packets already parsed, otherwise estimated
// from the average packet size (kAudioFileStreamSeekFlag_OffsetIsEstimated).

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace alchemy::gui::completion_kit::media::audio_file_stream {

// kAudioFileStreamError_*
namespace error {
constexpr std::int32_t code(const char (&c)[5]) {
    return static_cast<std::int32_t>((std::uint32_t(std::uint8_t(c[0])) << 24) | (std::uint32_t(std::uint8_t(c[1])) << 16) |
                                     (std::uint32_t(std::uint8_t(c[2])) << 8) | std::uint32_t(std::uint8_t(c[3])));
}
inline constexpr std::int32_t unsupported_file_type = code("typ?"), unsupported_data_format = code("fmt?"), unsupported_property = code("pty?"),
    bad_property_size = code("!siz"), invalid_packet_offset = code("pck?"), invalid_file = code("dta!"), value_unknown = code("unk?"),
    data_unavailable = code("more"), illegal_operation = code("nope");
}

// AudioStreamPacketDescription
struct PacketDescription {
    std::int64_t start{};                // from the start of the batch's bytes
    std::uint32_t variable_frames{}, bytes{};
};

// What a parse produced, in order: a property that became known, or a batch of packets.
struct Event {
    std::uint32_t property{};            // AudioFileStreamPropertyID; 0 for packets
    std::vector<std::uint8_t> bytes;     // the packets, back to back
    std::vector<PacketDescription> descriptions;   // empty for constant-bit-rate (PCM) packets
    std::uint32_t packets{};
};

class Parser {
public:
    explicit Parser(std::uint32_t file_type_hint) : hint_(file_type_hint) {}

    // Takes the next bytes of the stream (`discontinuity`: they need not follow the last ones).
    // Appends what they produced to `out`; an OSStatus, which stays once the stream is known to be
    // unusable.
    std::int32_t parse(std::span<const std::uint8_t> bytes, bool discontinuity, std::vector<Event>& out);
    // Where packet `packet` starts, from the start of the audio data; the next parse is expected to
    // begin there.
    std::int32_t seek(std::int64_t packet, std::int64_t& data_byte_offset, bool& estimated);
    // A property's value (kAudioFileStreamProperty_*). The packet/frame/byte translations read
    // their question from `in` (an AudioFramePacketTranslation or AudioBytePacketTranslation).
    std::int32_t property(std::uint32_t id, std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) const;
    [[nodiscard]] bool known_property(std::uint32_t id) const;
    [[nodiscard]] bool ready() const { return ready_; }

private:
    enum class Kind { unknown, pcm, mpeg };
    std::int32_t sniff();
    std::int32_t parse_pcm(std::vector<Event>& out);
    std::int32_t parse_mpeg(std::vector<Event>& out);
    void skip_pending();
    void announce(std::vector<Event>& out);
    void drop(std::size_t n);
    [[nodiscard]] double average_packet_bytes() const;
    [[nodiscard]] std::uint32_t signature(std::size_t at) const;

    std::uint32_t hint_{};
    Kind kind_{Kind::unknown};
    std::int32_t failed_{};
    bool ready_{};
    std::vector<std::uint8_t> pending_;  // bytes not yet parsed
    std::uint64_t pending_at_{};         // their offset in the stream
    std::uint64_t skip_{};               // bytes still to drop (an ID3v2 tag, the rest of a header)

    // What the header said.
    std::uint32_t file_type_{}, channels_{}, frames_per_packet_{1}, frame_bytes_{};   // frame_bytes_: PCM only
    double rate_{};
    std::vector<std::uint8_t> asbd_, layout_;
    std::uint64_t data_offset_{};
    std::optional<std::uint64_t> byte_count_, packet_count_;
    std::optional<std::uint32_t> priming_, remainder_;

    // Packets.
    std::uint64_t next_packet_{};        // the number of the next packet handed out
    std::uint64_t position_{};           // PCM: bytes of audio data consumed
    std::uint32_t maximum_packet_{};
    std::uint64_t seen_bytes_{}, seen_packets_{};
    // MPEG: the signature of the stream's frames (version, layer, rate, channel count), whether the
    // parser is in step with them, and each parsed packet's offset in the audio data while the
    // numbering is exact.
    std::uint32_t signature_{};
    bool first_frame_{true}, synced_{}, exact_{true};
    std::vector<std::uint64_t> offsets_;
};

} // namespace alchemy::gui::completion_kit::media::audio_file_stream
