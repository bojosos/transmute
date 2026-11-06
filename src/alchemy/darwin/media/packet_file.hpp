#pragma once

// Compressed audio files being written (host-only core, media group): the layout of CAF and
// MPEG-4 audio files around a stream of packets, for AudioFile, ExtAudioFile, AVAudioFile and
// AVAssetWriter (audio_file.cpp, av_asset_writer.cpp). The caller writes the header, then each
// packet at the data's end, then what finish() answers.
// - CAF ('caff'): the file header, 'desc', then a 'data' chunk of unknown size (-1) while
//   packets go in. finish() gives the data chunk its size and adds 'kuki' (the magic cookie) and
//   'pakt' (the packet count, valid frames, priming and remainder, and each packet's size, and its
//   frames when they vary, as BER integers) after the data, as the CAF specification allows. A
//   shorter last packet keeps the frames per packet constant and counts in the remainder.
// - MPEG-4 ('m4af', 'mp4f'): 'ftyp' (M4A or MP4 brands), then an 'mdat' with a 64-bit size;
//   finish() sets that size and adds 'moov': one sound track whose sample entry is the format's
//   ('alac', with the cookie in its 'alac' box), and the sample tables (stts, stsc, stsz, stco: one
//   chunk), version 1 times when they need 64 bits.
// Formats: Apple Lossless ('alac') and AAC-LC ('aac ': an 'mp4a' sample entry whose 'esds' holds the ES
// descriptor the cookie is) in either container. MPEG-4 files present the frames between the priming and the
// remainder through an edit list.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace alchemy::gui::completion_kit::media::packet_file {

struct Format {
    std::uint32_t file_type{};          // 'caff', 'm4af' or 'mp4f'
    double rate{};
    std::uint32_t format_id{};          // 'alac' or 'aac '
    std::uint32_t flags{};              // ALAC: kAppleLosslessFormatFlag_* (1-4: 16, 20, 24, 32-bit source); AAC: 0 or 2 (LC)
    std::uint32_t frames_per_packet{};
    std::uint32_t channels{};
};

// Whether `format_id` can be written into `file_type`.
bool writable(std::uint32_t file_type, std::uint32_t format_id);

class Writer {
public:
    // Null, with `why`, for a container or format this does not write, or an unusable format.
    static std::optional<Writer> make(const Format& format, std::string& why);

    [[nodiscard]] const Format& format() const { return format_; }
    // The bytes at the start of the file; packets follow at data_offset().
    [[nodiscard]] const std::vector<std::uint8_t>& header() const { return header_; }
    [[nodiscard]] std::uint64_t data_offset() const { return header_.size(); }
    // Where the next packet goes.
    [[nodiscard]] std::uint64_t data_end() const { return data_offset() + data_bytes_; }

    // A packet written at data_end(): its size, and its frames (0: frames_per_packet).
    void add_packet(std::uint32_t bytes, std::uint32_t frames = 0);
    void set_cookie(std::vector<std::uint8_t> cookie) { cookie_ = std::move(cookie); }
    // kAudioFilePropertyPacketTableInfo: frames to drop at the start and the end.
    void set_priming(std::uint32_t priming, std::uint32_t remainder) { priming_ = priming; remainder_ = remainder; }

    [[nodiscard]] const std::vector<std::uint8_t>& cookie() const { return cookie_; }
    [[nodiscard]] std::uint64_t packet_count() const { return sizes_.size(); }
    [[nodiscard]] std::uint64_t data_bytes() const { return data_bytes_; }
    [[nodiscard]] std::uint64_t frames() const { return frames_; }   // every packet's frames
    [[nodiscard]] std::uint32_t maximum_packet_bytes() const { return maximum_; }
    [[nodiscard]] std::uint32_t priming() const { return priming_; }
    [[nodiscard]] std::uint32_t remainder() const { return remainder_; }
    [[nodiscard]] std::uint32_t packet_bytes(std::size_t index) const { return sizes_[index]; }
    [[nodiscard]] std::uint32_t packet_frames(std::size_t index) const { return durations_[index]; }

    // The writes that complete the file as it stands, in order: (offset, bytes). Writing them again
    // after more packets is allowed (each finish describes everything so far).
    [[nodiscard]] std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> finish() const;

private:
    Writer() = default;
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> finish_caf() const;
    std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> finish_mp4() const;

    Format format_;
    std::vector<std::uint8_t> header_, cookie_;
    std::uint64_t data_bytes_{}, frames_{};
    std::vector<std::uint32_t> sizes_, durations_;
    std::uint32_t maximum_{}, priming_{}, remainder_{};
    std::uint64_t size_field_{};                     // CAF: the data chunk's size; MP4: mdat's 64-bit size
};

} // namespace alchemy::gui::completion_kit::media::packet_file
