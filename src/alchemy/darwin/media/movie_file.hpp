#pragma once

// Movie files being written (host-only core, media group): the multi-track ISO base media file (ISO/IEC 14496-12)
// and QuickTime movie layout AVAssetWriter writes, around samples that arrive in each track's decode order.
// - 'ftyp': MPEG-4 ('mp42', compatible with 'isom'), Apple M4V ('M4V ', with 'M4A ', 'mp42', 'isom') or QuickTime
//   ('qt  '); then an 'mdat' with a 64-bit size that finish() sets.
// - Samples are kept per track and written in chunks: once a track holds half a second, every track's pending
//   chunk goes out, the earliest first, so the tracks interleave by time.
// - finish() adds 'moov' after the data, or (moov_first, shouldOptimizeForNetworkUse) asks the caller to move the
//   data up by the moov's size and puts the moov between 'ftyp' and 'mdat'. Each track has tkhd (its transform
//   as the matrix), an edit list when its presentation does not start at media time 0 (audio priming, reordered
//   video), mdhd ("und"), hdlr, vmhd/smhd, a self-contained dref, and the sample tables: stsd, stts, ctts (when
//   presentation and decode times differ), stss (unless every sample is a sync sample), stsc, stsz, and stco (co64
//   past 4 GiB). Times take version 1 boxes when they need 64 bits.
// - Sample entries:
//   - 'avc1' and 'hvc1' VisualSampleEntries with the 'avcC' or 'hvcC' record;
//   - 'mp4a' with its ES descriptor in 'esds' (QuickTime: a version 1 sound description whose 'wave' holds 'frma',
//     'mp4a', 'esds' and the terminator, as the QuickTime File Format describes for MPEG-4 audio);
//   - 'alac' with the ALACSpecificConfig in an 'alac' box (QuickTime: in 'wave' after 'frma', as Apple's
//     ALACMagicCookieDescription describes).

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace alchemy::gui::completion_kit::media::movie_file {

enum class Brand : std::uint8_t { mp4, m4v, quicktime };

struct TrackFormat {
    bool video{};
    std::uint32_t codec{};                         // 'avc1', 'hvc1', 'mp4a' or 'alac'
    std::uint32_t timescale{};                     // the media's (audio: its sample rate)
    std::uint32_t width{}, height{};               // video
    std::uint32_t channels{}, bits{16};            // audio (ALAC: the source bit depth)
    double sample_rate{};                          // audio
    std::array<double, 6> transform{1, 0, 0, 1, 0, 0};   // CGAffineTransform a, b, c, d, tx, ty
};

using Writes = std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>>;

class Writer {
public:
    explicit Writer(Brand brand);

    // The bytes at the start of the file: 'ftyp' and the 'mdat' header. Samples follow.
    [[nodiscard]] const std::vector<std::uint8_t>& header() const { return header_; }

    // A track, before its first sample: its index.
    std::size_t add_track(const TrackFormat& format);
    // The track's format once known (a track of compressed samples learns it from the first), before its first sample.
    void set_format(std::size_t track, const TrackFormat& format);
    // The codec configuration: the 'avcC' or 'hvcC' record, the ES descriptor (from its tag 0x03), the
    // ALACSpecificConfig. It may change until finish() (ALAC's grows with the packets).
    void set_config(std::size_t track, std::vector<std::uint8_t> config);
    // Audio: the media frames before the presentation starts (the encoder's priming) and the frames presented
    // (0: every frame after the priming). They make the track's edit list.
    void set_trim(std::size_t track, std::uint64_t priming, std::uint64_t presented);

    // A sample in the track's decode order, times in its timescale: the writes to make now.
    Writes add_sample(std::size_t track, std::vector<std::uint8_t> bytes, std::int64_t dts, std::int64_t pts, std::uint32_t duration, bool sync);

    [[nodiscard]] std::size_t track_count() const { return tracks_.size(); }
    [[nodiscard]] std::uint64_t sample_count(std::size_t track) const { return tracks_[track].samples.size() + tracks_[track].pending.size(); }

    // What completes the file. When `move_by` is set the caller first moves the bytes from `move_from` to the end
    // of the file (as written so far) up by `move_by`, then makes the writes (the last chunks among them).
    struct Finish {
        Writes writes;
        std::uint64_t move_from{}, move_by{};
    };
    [[nodiscard]] Finish finish(bool moov_first);

private:
    struct Sample {
        std::uint32_t size{};
        std::int64_t dts{}, pts{};
        std::uint32_t duration{};
        bool sync{};
    };
    struct Chunk {
        std::uint64_t offset{};
        std::uint32_t count{};
    };
    struct Track {
        TrackFormat format;
        std::vector<std::uint8_t> config;
        std::uint64_t priming{}, presented{};
        std::vector<Sample> samples;               // written
        std::vector<Chunk> chunks;
        std::vector<std::pair<Sample, std::vector<std::uint8_t>>> pending;   // not yet written
        std::uint64_t pending_duration{};
    };

    Writes flush();
    [[nodiscard]] std::vector<std::uint8_t> moov(std::uint64_t offset_shift) const;
    // A track's 'trak' (empty without samples), and its presentation's length in the movie's timescale.
    [[nodiscard]] std::vector<std::uint8_t> trak(std::size_t index, std::uint64_t offset_shift, std::uint64_t& movie_length) const;
    [[nodiscard]] std::uint32_t movie_timescale() const { return brand_ == Brand::quicktime ? 600 : 1000; }

    Brand brand_;
    std::vector<std::uint8_t> header_;
    std::vector<Track> tracks_;
    std::uint64_t data_end_{};
};

} // namespace alchemy::gui::completion_kit::media::movie_file
