#pragma once

// The sampler behind AVAudioUnitSampler (media group-internal, host-only): instruments of sample
// zones played by note, from SoundFont 2 banks (the SoundFont 2.04 specification) or from audio
// files, rendered as interleaved double samples. avaudio_engine.cpp makes it a node.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::avaudio {

// One sample and how a range of notes plays it.
struct Zone {
    std::shared_ptr<const std::vector<float>> data;   // mono samples
    double sample_rate{44100};
    std::uint8_t key_low{0}, key_high{127}, velocity_low{0}, velocity_high{127};
    int root_key{60};
    double tune{};                     // cents added to the pitch
    double scale_tuning{100};          // cents per key
    std::size_t start{}, end{}, loop_start{}, loop_end{};
    int loop_mode{};                   // 0 none, 1 continuous, 3 until release (SF2 sampleModes)
    double attenuation{};              // dB
    double pan{};                      // -1 (left) to 1 (right)
    // The volume envelope, in seconds (sustain: a level 0..1).
    double delay{}, attack{0.001}, hold{}, decay{}, sustain{1}, release{0.05};
};

struct Instrument { std::vector<Zone> zones; };

// The instrument of one preset in a SoundFont 2 file: bank (128: percussion) and program. Nullopt,
// with `why`, when the file is not a SoundFont 2 bank or has no such preset.
std::optional<Instrument> load_sound_font(std::span<const std::uint8_t> file, int bank, int program, std::string& why);

// An instrument of audio files (mono or mixed down): one file covers every key with its root at
// note 60; several take one key each from note 60 up, each at its own pitch.
Instrument from_samples(std::vector<std::pair<std::vector<float>, double>> files);

class Sampler {
public:
    void set_instrument(Instrument instrument);
    void note_on(int channel, int key, int velocity);
    void note_off(int channel, int key);
    // Channel messages: controllers (7 volume, 10 pan, 11 expression, 64 sustain, 120 and 123 all
    // off), the pitch bend (0..16383, +-2 semitones), program changes (kept for the caller).
    void controller(int channel, int number, int value);
    void pitch_bend(int channel, int value);
    void all_off();
    // Adds `frames` interleaved frames (in `channels`, at `rate`) into `out`.
    void render(double* out, std::uint32_t frames, std::uint32_t channels, double rate);

    double master_gain{};              // dB
    double stereo_pan{};               // -1 .. 1
    double global_tuning{};            // cents

private:
    struct Voice {
        const Zone* zone{};
        int channel{}, key{};
        double velocity_gain{};
        double position{};
        double time{};                 // seconds since the note began
        double release_time{-1};       // seconds since the note was released (-1: held)
        double level_at_release{};
        bool sustained{};              // released while the sustain pedal was down
        bool done{};
    };
    struct Channel { double volume{100.0 / 127}, expression{1}, pan{}, bend{}; bool sustain{}; };
    double envelope(Voice& v) const;
    Instrument instrument_;
    std::vector<Voice> voices_;
    std::array<Channel, 16> channels_{};
};

} // namespace alchemy::gui::completion_kit::media::avaudio
