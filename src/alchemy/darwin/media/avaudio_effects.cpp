#include "alchemy/darwin/media/avaudio_effects.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace alchemy::gui::completion_kit::media::avaudio {
namespace {

double decibels(double db) { return std::pow(10.0, db / 20.0); }

// Mixes `wet` into `out` (which holds the dry input): wet/dry in percent.
void mix(double* out, const std::vector<double>& wet, double percent) {
    const double w = std::clamp(percent, 0.0, 100.0) / 100.0;
    for (std::size_t i = 0; i < wet.size(); ++i) out[i] = out[i] * (1 - w) + wet[i] * w;
}

// Reads `frames` input frames into `out` (padding with silence when the input gives fewer).
void take(double* out, std::uint32_t frames, std::uint32_t channels, const Effect::Pull& pull) {
    const auto input = pull(frames);
    const auto count = std::min<std::size_t>(input.size(), static_cast<std::size_t>(frames) * channels);
    std::copy_n(input.begin(), count, out);
    std::fill(out + count, out + static_cast<std::size_t>(frames) * channels, 0.0);
}

} // namespace

// ---- Equalizer ------------------------------------------------------------------------------------

Equalizer::Coefficients Equalizer::design(const Band& band, double rate) {
    const double frequency = std::clamp(band.frequency, 10.0, rate * 0.49);
    const double w0 = 2 * std::numbers::pi * frequency / rate, cs = std::cos(w0), sn = std::sin(w0);
    const double bandwidth = std::clamp(band.bandwidth, 0.05, 5.0);
    const double alpha_bw = sn * std::sinh(std::log(2.0) / 2 * bandwidth * w0 / sn);   // from the bandwidth in octaves
    const double alpha_q = sn / (2 * std::numbers::sqrt2 / 2);                        // Q = 1/sqrt(2)
    const double a = std::pow(10.0, band.gain / 40.0), root = 2 * std::sqrt(a);
    double b0{}, b1{}, b2{}, a0{}, a1{}, a2{};
    switch (band.type) {
    case 1: case 3: {   // low pass (resonant: the band's bandwidth sets Q)
        const double alpha = band.type == 3 ? alpha_bw : alpha_q;
        b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
    }
    case 2: case 4: {   // high pass
        const double alpha = band.type == 4 ? alpha_bw : alpha_q;
        b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha;
        break;
    }
    case 5:   // band pass, 0 dB peak
        b0 = alpha_bw; b1 = 0; b2 = -alpha_bw; a0 = 1 + alpha_bw; a1 = -2 * cs; a2 = 1 - alpha_bw;
        break;
    case 6:   // band stop
        b0 = 1; b1 = -2 * cs; b2 = 1; a0 = 1 + alpha_bw; a1 = -2 * cs; a2 = 1 - alpha_bw;
        break;
    case 7: case 9: {   // low shelf (resonant: the bandwidth sets the slope)
        const double alpha = (band.type == 9 ? alpha_bw : alpha_q) * root / 2;
        b0 = a * ((a + 1) - (a - 1) * cs + 2 * alpha); b1 = 2 * a * ((a - 1) - (a + 1) * cs); b2 = a * ((a + 1) - (a - 1) * cs - 2 * alpha);
        a0 = (a + 1) + (a - 1) * cs + 2 * alpha; a1 = -2 * ((a - 1) + (a + 1) * cs); a2 = (a + 1) + (a - 1) * cs - 2 * alpha;
        break;
    }
    case 8: case 10: {   // high shelf
        const double alpha = (band.type == 10 ? alpha_bw : alpha_q) * root / 2;
        b0 = a * ((a + 1) + (a - 1) * cs + 2 * alpha); b1 = -2 * a * ((a - 1) + (a + 1) * cs); b2 = a * ((a + 1) + (a - 1) * cs - 2 * alpha);
        a0 = (a + 1) - (a - 1) * cs + 2 * alpha; a1 = 2 * ((a - 1) - (a + 1) * cs); a2 = (a + 1) - (a - 1) * cs - 2 * alpha;
        break;
    }
    default:   // parametric (peaking)
        b0 = 1 + alpha_bw * a; b1 = -2 * cs; b2 = 1 - alpha_bw * a; a0 = 1 + alpha_bw / a; a1 = -2 * cs; a2 = 1 - alpha_bw / a;
        break;
    }
    return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

void Equalizer::render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) {
    take(out, frames, channels, pull);
    if (bypass) return;
    if (state_.size() != bands.size() * channels) state_.assign(bands.size() * channels, {});
    for (std::size_t b = 0; b < bands.size(); ++b) {
        if (bands[b].bypass) continue;
        const auto k = design(bands[b], rate);
        for (std::uint32_t c = 0; c < channels; ++c) {
            auto& st = state_[b * channels + c];
            for (std::uint32_t f = 0; f < frames; ++f) {
                double& x = out[static_cast<std::size_t>(f) * channels + c];
                const double y = k.b0 * x + k.b1 * st[0] + k.b2 * st[1] - k.a1 * st[2] - k.a2 * st[3];
                st = {x, st[0], y, st[2]};
                x = y;
            }
        }
    }
    const double gain = decibels(global_gain);
    if (gain != 1.0)
        for (std::size_t i = 0; i < static_cast<std::size_t>(frames) * channels; ++i) out[i] *= gain;
}

// ---- Delay ----------------------------------------------------------------------------------------

void Delay::render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) {
    take(out, frames, channels, pull);
    if (bypass) return;
    if (lines_.size() != channels || rate_ != rate) {
        lines_.assign(channels, Line{std::vector<double>(static_cast<std::size_t>(rate * 2) + 2, 0.0), 0, 0});
        rate_ = rate;
    }
    const auto delay = static_cast<std::size_t>(std::clamp(delay_time, 0.0, 2.0) * rate);
    const double fb = std::clamp(feedback, -100.0, 100.0) / 100.0;
    const double coefficient = std::exp(-2 * std::numbers::pi * std::clamp(low_pass_cutoff, 10.0, rate / 2) / rate);
    std::vector<double> wet(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        auto& line = lines_[c];
        const auto size = line.samples.size();
        for (std::uint32_t f = 0; f < frames; ++f) {
            const auto i = static_cast<std::size_t>(f) * channels + c;
            const double delayed = line.samples[(line.at + size - delay) % size];
            line.low = delayed + coefficient * (line.low - delayed);
            line.samples[line.at] = out[i] + fb * line.low;
            line.at = (line.at + 1) % size;
            wet[i] = delay ? delayed : out[i];
        }
    }
    mix(out, wet, wet_dry_mix);
}

// ---- Reverb -----------------------------------------------------------------------------------------

void Reverb::load_preset(std::int64_t preset) {
    // AVAudioUnitReverbPreset: small room 0, medium room 1, large room 2, medium hall 3, large hall 4,
    // plate 5, medium chamber 6, large chamber 7, cathedral 8, large room 2 9, medium hall 2 10,
    // medium hall 3 11, large hall 2 12: room size (comb feedback) and damping.
    static constexpr std::array<std::array<double, 2>, 13> presets{{
        {0.70, 0.40}, {0.78, 0.35}, {0.84, 0.30}, {0.86, 0.30}, {0.90, 0.25}, {0.88, 0.10}, {0.82, 0.25},
        {0.87, 0.20}, {0.95, 0.15}, {0.85, 0.35}, {0.87, 0.28}, {0.88, 0.22}, {0.92, 0.20}}};
    const auto& p = presets[static_cast<std::size_t>(std::clamp<std::int64_t>(preset, 0, 12))];
    room_ = p[0];
    damping_ = p[1];
}

void Reverb::build(std::uint32_t channels, double rate) {
    static constexpr std::array<double, 8> comb_ms{25.3, 26.9, 28.9, 30.7, 32.2, 33.8, 35.3, 36.7};
    static constexpr std::array<double, 4> pass_ms{5.0, 7.7, 10.0, 12.6};
    channels_.assign(channels, {});
    for (std::uint32_t c = 0; c < channels; ++c) {
        const double spread = 0.52 * c;   // milliseconds between channels: decorrelated tails
        for (const auto ms : comb_ms) channels_[c].combs.push_back({std::vector<double>(static_cast<std::size_t>((ms + spread) * rate / 1000) + 1, 0.0)});
        for (const auto ms : pass_ms) channels_[c].passes.push_back({std::vector<double>(static_cast<std::size_t>((ms + spread) * rate / 1000) + 1, 0.0)});
    }
    rate_ = rate;
}

void Reverb::render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) {
    take(out, frames, channels, pull);
    if (bypass) return;
    if (channels_.size() != channels || rate_ != rate) build(channels, rate);
    std::vector<double> wet(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t c = 0; c < channels; ++c) {
        auto& ch = channels_[c];
        for (std::uint32_t f = 0; f < frames; ++f) {
            const auto i = static_cast<std::size_t>(f) * channels + c;
            const double input = out[i] * 0.015;
            double sum = 0;
            for (auto& comb : ch.combs) {
                const double y = comb.buffer[comb.at];
                comb.low = y * (1 - damping_) + comb.low * damping_;
                comb.buffer[comb.at] = input + comb.low * room_;
                comb.at = (comb.at + 1) % comb.buffer.size();
                sum += y;
            }
            for (auto& pass : ch.passes) {
                const double delayed = pass.buffer[pass.at];
                pass.buffer[pass.at] = sum + delayed * 0.5;
                pass.at = (pass.at + 1) % pass.buffer.size();
                sum = delayed - sum;
            }
            wet[i] = sum * 3.0;
        }
    }
    mix(out, wet, wet_dry_mix);
}

// ---- Distortion -------------------------------------------------------------------------------------

void Distortion::load_preset(std::int64_t preset) {
    // AVAudioUnitDistortionPreset 0 (drums, bit brush) to 21 (speech, waves): the drive grows with
    // the preset's number within each family; every preset is the same soft clipper.
    drive_ = 2.0 + static_cast<double>(std::clamp<std::int64_t>(preset, 0, 21) % 7) * 3.0;
}

void Distortion::render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) {
    static_cast<void>(rate);
    take(out, frames, channels, pull);
    if (bypass) return;
    const double gain = decibels(std::clamp(pre_gain, -80.0, 20.0)) * drive_;
    std::vector<double> wet(static_cast<std::size_t>(frames) * channels);
    for (std::size_t i = 0; i < wet.size(); ++i) wet[i] = std::tanh(out[i] * gain) / std::tanh(std::max(gain, 1e-9));
    mix(out, wet, wet_dry_mix);
}

// ---- Varispeed --------------------------------------------------------------------------------------

void Varispeed::render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) {
    const double speed = std::clamp(this->rate, 0.25, 4.0);
    if (bypass || speed == 1.0) return take(out, frames, channels, pull);
    if (!resampler_ || made_for_ != speed * rate || channels_ != channels) {
        resampler_ = std::make_unique<Resampler>(speed * rate, rate, channels);
        made_for_ = speed * rate;
        channels_ = channels;
    }
    resampler_->render(out, frames, [&](double* into, std::size_t count) { take(into, static_cast<std::uint32_t>(count), channels, pull); });
}

// ---- TimePitch --------------------------------------------------------------------------------------

void TimePitch::reset() {
    resampler_.reset();
    input_.clear();
    input_position_ = 0;
    output_.clear();
    weight_.clear();
    write_ = 0;
}

void TimePitch::render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) {
    const double tempo = std::clamp(this->rate, 1.0 / 32, 32.0);
    const double shift = std::pow(2.0, std::clamp(pitch, -2400.0, 2400.0) / 1200.0);
    if (bypass || (tempo == 1.0 && shift == 1.0)) {
        reset();
        return take(out, frames, channels, pull);
    }
    if (channels_ != channels || !resampler_ || made_for_ != shift * rate) {
        reset();
        resampler_ = std::make_unique<Resampler>(shift * rate, rate, channels);
        made_for_ = shift * rate;
        channels_ = channels;
    }
    // The resampled signal is `shift` times faster and higher; stretching it by shift / tempo leaves
    // the pitch shifted and the tempo at `tempo`.
    const std::size_t window = std::max<std::size_t>(256, static_cast<std::size_t>(rate * 0.046));
    const auto hops = std::clamp(overlap, 3.0, 32.0);
    const std::size_t synthesis = std::max<std::size_t>(1, static_cast<std::size_t>(window / hops));
    const double analysis = static_cast<double>(synthesis) * tempo / shift;
    const auto pull_resampled = [&](std::size_t count) {
        std::vector<double> chunk(count * channels);
        resampler_->render(chunk.data(), count, [&](double* into, std::size_t n) { take(into, static_cast<std::uint32_t>(n), channels, pull); });
        input_.insert(input_.end(), chunk.begin(), chunk.end());
    };
    while (write_ < frames) {
        const auto start = static_cast<std::size_t>(input_position_);
        if (input_.size() / channels < start + window) pull_resampled(start + window - input_.size() / channels);
        if (output_.size() / channels < write_ + window) {
            output_.resize((write_ + window) * channels, 0.0);
            weight_.resize(write_ + window, 0.0);
        }
        for (std::size_t i = 0; i < window; ++i) {
            const double w = 0.5 - 0.5 * std::cos(2 * std::numbers::pi * (static_cast<double>(i) + 0.5) / static_cast<double>(window));
            for (std::uint32_t c = 0; c < channels; ++c) output_[(write_ + i) * channels + c] += w * input_[(start + i) * channels + c];
            weight_[write_ + i] += w;
        }
        write_ += synthesis;
        input_position_ += analysis;
        // Drop consumed input.
        const auto consumed = static_cast<std::size_t>(input_position_);
        if (consumed > 0) {
            input_.erase(input_.begin(), input_.begin() + static_cast<std::ptrdiff_t>(std::min(consumed * channels, input_.size())));
            input_position_ -= static_cast<double>(consumed);
        }
    }
    for (std::uint32_t f = 0; f < frames; ++f)
        for (std::uint32_t c = 0; c < channels; ++c)
            out[static_cast<std::size_t>(f) * channels + c] = weight_[f] > 1e-9 ? output_[static_cast<std::size_t>(f) * channels + c] / weight_[f] : 0.0;
    output_.erase(output_.begin(), output_.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(frames) * channels));
    weight_.erase(weight_.begin(), weight_.begin() + frames);
    write_ -= frames;
}

} // namespace alchemy::gui::completion_kit::media::avaudio
