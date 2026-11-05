#pragma once

// Audio sources for AVFoundation's asset readers (host-only core, media group). A Source gives
// random access to frames of interleaved samples in [-1, 1]: an audio file (the reader's
// file-backed source, avfoundation.cpp, reads exactly as before), or a Sequence of segments of
// other sources placed on a timeline (what compositions are made of).
//
// A Sequence has its own rate and channel count. Each segment takes `frames` frames of the
// timeline from `target_start`, reading its source from `source_start`: its channels mapped to the
// sequence's (mono to every channel, several to mono averaged, otherwise channel by channel with
// missing ones silent) and, for a source at another rate, resampled (resampler.hpp; reads that
// continue the previous one continue the resampler's stream). Where no segment plays the sequence
// is silent; where segments overlap they add.

#include "alchemy/darwin/media/resampler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace alchemy::gui::completion_kit::media::audio_source {

class Source {
public:
    virtual ~Source() = default;
    [[nodiscard]] virtual double rate() const = 0;
    [[nodiscard]] virtual std::uint32_t channels() const = 0;
    [[nodiscard]] virtual std::uint64_t frames() const = 0;
    // Appends frames [start, start + count) (clipped to the source) to `out`, interleaved; the
    // frames appended.
    virtual std::size_t read(std::uint64_t start, std::size_t count, std::vector<double>& out) = 0;
    // Releases what the source holds open (a file); reads after it return nothing.
    virtual void close() {}
};

// `frames` interleaved frames of `from` channels into `to` channels (see above), added to `out`.
void add_mapped(const double* in, std::uint32_t from, double* out, std::uint32_t to, std::size_t frames);

struct Segment {
    std::shared_ptr<Source> source;    // null: an empty segment (silence)
    std::uint64_t source_start{};      // in the source's frames
    std::uint64_t target_start{}, frames{};   // on the sequence's timeline, in its frames
};

class Sequence final : public Source {
public:
    Sequence(double rate, std::uint32_t channels, std::vector<Segment> segments);
    [[nodiscard]] double rate() const override { return rate_; }
    [[nodiscard]] std::uint32_t channels() const override { return channels_; }
    [[nodiscard]] std::uint64_t frames() const override { return frames_; }
    std::size_t read(std::uint64_t start, std::size_t count, std::vector<double>& out) override;
    void close() override;

private:
    struct Place {
        Segment segment;
        std::unique_ptr<Resampler> resampler;   // sources at another rate
        std::uint64_t next_target{~0ULL};       // where the resampler's stream continues
        std::uint64_t cursor{};                 // the next source frame it pulls
    };
    void render(Place& place, std::uint64_t from, std::size_t count, double* out);

    double rate_;
    std::uint32_t channels_;
    std::vector<Place> places_;
    std::uint64_t frames_{};
};

} // namespace alchemy::gui::completion_kit::media::audio_source
