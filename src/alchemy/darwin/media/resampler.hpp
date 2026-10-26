#pragma once

// Sample-rate conversion for the media group (host-only core): a streaming band-limited
// resampler of interleaved samples, for the output audio units and AudioConverter. Each output
// sample is a windowed-sinc interpolation (Kaiser window, beta 8.6; 32 taps each side) of the
// input around its position, with the cutoff at the lower Nyquist frequency of the two rates.
// Equal rates copy.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace alchemy::gui::completion_kit::media {

class Resampler {
public:
    // `pull(out, frames)` must write `frames` interleaved input frames into `out`.
    using Pull = std::function<void(double* out, std::size_t frames)>;

    Resampler(double input_rate, double output_rate, std::uint32_t channels);

    // Writes `frames` interleaved output frames, pulling input as needed.
    void render(double* out, std::size_t frames, const Pull& pull);
    // Input frames the next render of `frames` output frames will pull, at most.
    [[nodiscard]] std::size_t input_needed(std::size_t frames) const;
    // Forgets the input history (AudioUnitReset, AudioConverterReset).
    void reset();
    [[nodiscard]] double ratio() const { return step_; }

private:
    double kernel(double x) const;

    double step_;                      // input frames per output frame
    double cutoff_;                    // of the input's Nyquist frequency, <= 1
    std::uint32_t channels_;
    std::vector<double> history_;      // interleaved input frames, history_[0] is frame base_
    double position_{};                // the next output's position, in input frames from history_[0]
    std::vector<double> table_;        // the windowed sinc, sampled
};

} // namespace alchemy::gui::completion_kit::media
