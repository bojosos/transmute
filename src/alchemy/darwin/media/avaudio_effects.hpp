#pragma once

// The DSP of AVFAudio's effect nodes (media group-internal, host-only): AVAudioUnitEQ,
// AVAudioUnitDelay, AVAudioUnitReverb, AVAudioUnitDistortion, AVAudioUnitVarispeed and
// AVAudioUnitTimePitch, each rendering interleaved double samples pulled from its input.
// avaudio_engine.cpp puts them in the engine's graph and gives them their Objective-C classes.

#include "alchemy/darwin/media/resampler.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace alchemy::gui::completion_kit::media::avaudio {

class Effect {
public:
    // `frames` interleaved input frames in the effect's channels.
    using Pull = std::function<std::vector<double>(std::uint32_t frames)>;
    virtual ~Effect() = default;
    // Writes `frames` interleaved frames (in `channels`, at `rate`) into `out`, pulling its input
    // as it needs it.
    virtual void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) = 0;
    virtual void reset() {}
    bool bypass{};
};

// AVAudioUnitEQ: bands of biquads (the RBJ "Audio EQ Cookbook" responses) and a global gain.
// AVAudioUnitEQFilterType: parametric 0, low pass 1, high pass 2, resonant low pass 3, resonant high
// pass 4, band pass 5, band stop 6, low shelf 7, high shelf 8, resonant low shelf 9, resonant high
// shelf 10.
class Equalizer final : public Effect {
public:
    struct Band { std::int64_t type{}; double frequency{1000}, bandwidth{0.5}, gain{}; bool bypass{}; };
    explicit Equalizer(std::size_t bands) : bands(bands) {}
    void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) override;
    void reset() override { state_.clear(); }
    std::vector<Band> bands;
    double global_gain{};              // dB

private:
    struct Coefficients { double b0{1}, b1{}, b2{}, a1{}, a2{}; };
    static Coefficients design(const Band& band, double rate);
    std::vector<std::array<double, 4>> state_;   // per band and channel: x1 x2 y1 y2
};

// AVAudioUnitDelay: a delay line per channel with feedback through a one-pole low pass.
class Delay final : public Effect {
public:
    void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) override;
    void reset() override { lines_.clear(); }
    double delay_time{1}, feedback{50}, low_pass_cutoff{15000}, wet_dry_mix{100};   // s, %, Hz, %

private:
    struct Line { std::vector<double> samples; std::size_t at{}; double low{}; };
    std::vector<Line> lines_;
    double rate_{};
};

// AVAudioUnitReverb: a Schroeder-Moorer reverberator (parallel damped combs into series all-passes
// per channel) whose room size and damping come from the factory preset.
class Reverb final : public Effect {
public:
    void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) override;
    void reset() override { channels_.clear(); }
    void load_preset(std::int64_t preset);
    double wet_dry_mix{100};           // %

private:
    struct Comb { std::vector<double> buffer; std::size_t at{}; double low{}; };
    struct AllPass { std::vector<double> buffer; std::size_t at{}; };
    struct Channel { std::vector<Comb> combs; std::vector<AllPass> passes; };
    void build(std::uint32_t channels, double rate);
    std::vector<Channel> channels_;
    double rate_{}, room_{0.7}, damping_{0.3};
};

// AVAudioUnitDistortion: pre-gain, then a soft clipper (tanh) whose drive comes from the factory
// preset, mixed with the dry signal.
class Distortion final : public Effect {
public:
    void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) override;
    void load_preset(std::int64_t preset);
    double pre_gain{-6}, wet_dry_mix{50};   // dB, %

private:
    double drive_{4};
};

// AVAudioUnitVarispeed: playback at `rate` times the speed, pitch following (resampled).
class Varispeed final : public Effect {
public:
    void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) override;
    void reset() override { resampler_.reset(); }
    double rate{1};

private:
    std::unique_ptr<Resampler> resampler_;
    double made_for_{};
    std::uint32_t channels_{};
};

// AVAudioUnitTimePitch: tempo `rate` times and pitch shifted by `pitch` cents, independently: an
// overlap-add time stretch (Hann windows, `overlap` per window) of the resampled signal.
class TimePitch final : public Effect {
public:
    void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate, const Pull& pull) override;
    void reset() override;
    double rate{1}, pitch{}, overlap{8};

private:
    std::unique_ptr<Resampler> resampler_;
    double made_for_{};
    std::uint32_t channels_{};
    std::vector<double> input_;        // resampled input not yet consumed, interleaved
    double input_position_{};          // the next window's start in input_, in frames
    std::vector<double> output_;       // overlap-added output not yet delivered
    std::vector<double> weight_;       // the summed windows over output_
    std::size_t write_{};              // where the next window goes in output_
};

} // namespace alchemy::gui::completion_kit::media::avaudio
