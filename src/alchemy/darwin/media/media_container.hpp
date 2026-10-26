#pragma once

// Media containers (host-only core, media group): the tracks and sample tables of
// - ISO base media files (MP4, M4A, MOV, 3GP): moov/trak/stbl sample tables, edit lists (an initial
//   empty edit and the media start), and fragments (moof/traf/trun with the trex/tfhd defaults);
// - Matroska and WebM: Tracks and Clusters, SimpleBlocks and BlockGroups, Xiph, fixed and EBML
//   lacing, and elements of unknown size (live streams);
// - Ogg streams of Opus or FLAC;
// - native FLAC streams and MPEG audio (MP3) streams with or without an ID3v2 tag.
// Each track names its codec as the sample-entry fourcc CoreMedia uses ('avc1', 'hvc1', 'av01',
// 'vp08', 'vp09', 'mp4a', '.mp3', 'opus', 'flac', 'alac', 'lpcm', ...) and carries the codec's
// configuration as the container stores it (avcC, hvcC, av1C, vpcC, esds, dOps, dfLa, the
// Matroska CodecPrivate). Times are in the track's timescale. Samples are byte ranges of the file.

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::container {

constexpr std::uint32_t fourcc(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

struct Sample {
    // The bytes: [offset, offset + size) of the file, or with `pieces` > 0 the track's
    // pieces[offset .. offset + pieces) in order (an Ogg packet spanning pages).
    std::uint64_t offset{}, size{};
    std::uint32_t pieces{};
    std::int64_t pts{}, dts{};         // in the track's timescale
    std::uint32_t duration{};
    bool keyframe{};
};

struct Track {
    enum class Kind { video, audio, other } kind{Kind::other};
    std::uint32_t id{};
    std::uint32_t codec{};             // sample-entry fourcc (see above)
    std::string codec_name;            // the container's own name (Matroska CodecID, sample entry, "OpusHead", ...)
    std::uint32_t timescale{1};
    std::int64_t duration{};           // in the timescale (0: unknown)
    std::int64_t start{};              // the presentation start (edit list / codec delay), in the timescale
    // Video
    std::uint32_t width{}, height{};
    // Audio
    double sample_rate{};
    std::uint32_t channels{}, bits{};
    std::uint32_t priming{};           // samples to drop at the start (Opus pre-skip, MP4 edit/priming)
    std::vector<std::uint8_t> config;  // codec configuration (the box payload or CodecPrivate)
    std::vector<Sample> samples;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pieces;   // (offset, size) runs of scattered samples
};

struct Movie {
    std::uint32_t brand{};             // 'isom', 'qt  ', 'webm', 'matr', 'OggS', 'fLaC', 'mp3 ' ...
    std::uint32_t timescale{1000};
    std::int64_t duration{};           // in `timescale` (0: unknown)
    std::vector<Track> tracks;
};

using ReadAt = std::function<std::size_t(std::uint64_t offset, std::span<std::uint8_t> out)>;

// Parses `size` bytes through `read`. Answers false, with `why`, for files none of the parsers
// recognises or that are malformed beyond use.
bool parse(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why);

// The individual parsers (parse() picks by the leading bytes).
bool parse_iso(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why);
bool parse_matroska(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why);
bool parse_ogg(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why);
bool parse_flac(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why);
bool parse_mpeg_audio(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why);

// A sample's bytes (empty when the file is shorter than its table says).
std::vector<std::uint8_t> read_sample(const ReadAt& read, const Track& track, const Sample& sample);

// An MPEG audio frame header at `p` (4 bytes): its length in bytes (0: not a frame), samples,
// rate and channels.
struct MpegFrame { std::uint32_t bytes{}, samples{}, rate{}, channels{}; };
MpegFrame mpeg_frame(const std::uint8_t* p);

} // namespace alchemy::gui::completion_kit::media::container
