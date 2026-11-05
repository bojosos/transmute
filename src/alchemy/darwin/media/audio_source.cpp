#include "alchemy/darwin/media/audio_source.hpp"

#include <algorithm>
#include <cmath>

namespace alchemy::gui::completion_kit::media::audio_source {

void add_mapped(const double* in, std::uint32_t from, double* out, std::uint32_t to, std::size_t frames) {
    if (!from || !to) return;
    for (std::size_t f = 0; f < frames; ++f) {
        const double* frame = in + f * from;
        double* target = out + f * to;
        if (from == to) {
            for (std::uint32_t c = 0; c < to; ++c) target[c] += frame[c];
        } else if (from == 1) {
            for (std::uint32_t c = 0; c < to; ++c) target[c] += frame[0];
        } else if (to == 1) {
            double sum = 0;
            for (std::uint32_t c = 0; c < from; ++c) sum += frame[c];
            target[0] += sum / from;
        } else {
            for (std::uint32_t c = 0; c < std::min(from, to); ++c) target[c] += frame[c];
        }
    }
}

Sequence::Sequence(double rate, std::uint32_t channels, std::vector<Segment> segments) : rate_(rate), channels_(channels) {
    for (auto& segment : segments) {
        frames_ = std::max(frames_, segment.target_start + segment.frames);
        Place place;
        place.segment = std::move(segment);
        places_.push_back(std::move(place));
    }
}

void Sequence::close() {
    for (auto& place : places_)
        if (place.segment.source) place.segment.source->close();
}

std::size_t Sequence::read(std::uint64_t start, std::size_t count, std::vector<double>& out) {
    if (start >= frames_) return 0;
    count = static_cast<std::size_t>(std::min<std::uint64_t>(count, frames_ - start));
    const auto base = out.size();
    out.resize(base + count * channels_, 0.0);
    for (auto& place : places_) {
        const auto& s = place.segment;
        if (!s.source || !s.frames) continue;
        const auto from = std::max(start, s.target_start), to = std::min(start + count, s.target_start + s.frames);
        if (from >= to) continue;
        render(place, from, static_cast<std::size_t>(to - from), out.data() + base + (from - start) * channels_);
    }
    return count;
}

// Adds the segment's frames [from, from + count) of the timeline into `out`.
void Sequence::render(Place& place, std::uint64_t from, std::size_t count, double* out) {
    auto& source = *place.segment.source;
    const auto source_channels = source.channels();
    const double source_rate = source.rate();
    const auto offset = from - place.segment.target_start;
    if (source_rate == rate_) {
        std::vector<double> samples;
        const auto got = source.read(place.segment.source_start + offset, count, samples);
        add_mapped(samples.data(), source_channels, out, channels_, got);
        return;
    }
    // Another rate: a resampler stream over the segment's source frames (silence past them).
    const auto source_end = place.segment.source_start +
                            static_cast<std::uint64_t>(std::ceil(static_cast<double>(place.segment.frames) * source_rate / rate_));
    if (!place.resampler || place.next_target != from) {
        place.resampler = std::make_unique<Resampler>(source_rate, rate_, channels_);
        place.cursor = place.segment.source_start + static_cast<std::uint64_t>(std::llround(static_cast<double>(offset) * source_rate / rate_));
    }
    const auto pull = [&](double* in, std::size_t frames) {
        std::fill(in, in + frames * channels_, 0.0);
        const auto available = place.cursor < source_end ? static_cast<std::size_t>(std::min<std::uint64_t>(frames, source_end - place.cursor)) : 0;
        if (available) {
            std::vector<double> samples;
            const auto got = source.read(place.cursor, available, samples);
            add_mapped(samples.data(), source_channels, in, channels_, got);
        }
        place.cursor += frames;
    };
    std::vector<double> converted(count * channels_);
    place.resampler->render(converted.data(), count, pull);
    for (std::size_t i = 0; i < converted.size(); ++i) out[i] += converted[i];
    place.next_target = from + count;
}

} // namespace alchemy::gui::completion_kit::media::audio_source
