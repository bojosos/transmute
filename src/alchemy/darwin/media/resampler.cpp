#include "alchemy/darwin/media/resampler.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace alchemy::gui::completion_kit::media {
namespace {

constexpr int taps = 32;               // zero crossings each side, at the cutoff
constexpr int table_resolution = 512;  // table samples per zero crossing
constexpr double beta = 8.6;

double bessel_i0(double x) {
    double sum = 1, term = 1;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2 * k)) * (x / (2 * k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

} // namespace

Resampler::Resampler(double input_rate, double output_rate, std::uint32_t channels)
    : step_(input_rate / output_rate), cutoff_(std::min(1.0, output_rate / input_rate)), channels_(std::max(channels, 1U)) {
    table_.resize(static_cast<std::size_t>(taps) * table_resolution + 2);
    const double normal = bessel_i0(beta);
    for (std::size_t i = 0; i < table_.size(); ++i) {
        const double u = static_cast<double>(i) / table_resolution;
        if (u >= taps) { table_[i] = 0; continue; }
        const double sinc = u == 0 ? 1.0 : std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
        const double t = u / taps;
        table_[i] = sinc * bessel_i0(beta * std::sqrt(1 - t * t)) / normal;
    }
    reset();
}

void Resampler::reset() {
    const auto width = static_cast<std::size_t>(std::ceil(taps / cutoff_));
    history_.assign(width * channels_, 0.0);
    position_ = static_cast<double>(width);
}

double Resampler::kernel(double x) const {
    const double u = std::abs(x) * cutoff_ * table_resolution;
    const auto index = static_cast<std::size_t>(u);
    if (index + 1 >= table_.size()) return 0;
    const double fraction = u - static_cast<double>(index);
    return cutoff_ * (table_[index] + (table_[index + 1] - table_[index]) * fraction);
}

std::size_t Resampler::input_needed(std::size_t frames) const {
    if (step_ == 1.0) return frames;
    if (frames == 0) return 0;
    const double width = taps / cutoff_;
    const double last = position_ + static_cast<double>(frames - 1) * step_ + width + 1;
    const auto have = history_.size() / channels_;
    return last > static_cast<double>(have) ? static_cast<std::size_t>(std::ceil(last - static_cast<double>(have))) : 0;
}

void Resampler::render(double* out, std::size_t frames, const Pull& pull) {
    if (step_ == 1.0) {
        pull(out, frames);
        return;
    }
    const double width = taps / cutoff_;
    if (const auto needed = input_needed(frames)) {
        const auto old = history_.size();
        history_.resize(old + needed * channels_);
        pull(history_.data() + old, needed);
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double center = position_;
        const auto first = static_cast<std::ptrdiff_t>(std::ceil(center - width));
        const auto last = static_cast<std::ptrdiff_t>(std::floor(center + width));
        for (std::uint32_t c = 0; c < channels_; ++c) out[frame * channels_ + c] = 0;
        for (auto i = std::max<std::ptrdiff_t>(first, 0); i <= last; ++i) {
            if (static_cast<std::size_t>(i) * channels_ >= history_.size()) break;
            const double weight = kernel(center - static_cast<double>(i));
            const double* sample = history_.data() + static_cast<std::size_t>(i) * channels_;
            for (std::uint32_t c = 0; c < channels_; ++c) out[frame * channels_ + c] += weight * sample[c];
        }
        position_ += step_;
    }
    // Keep the input the next outputs still reach back to.
    const auto keep_from = static_cast<std::ptrdiff_t>(std::floor(position_ - width)) - 1;
    if (keep_from > 0) {
        const auto drop = std::min(static_cast<std::size_t>(keep_from), history_.size() / channels_);
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(drop * channels_));
        position_ -= static_cast<double>(drop);
    }
}

} // namespace alchemy::gui::completion_kit::media
