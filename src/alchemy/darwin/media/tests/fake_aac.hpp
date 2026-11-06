#pragma once

// A deterministic stand-in for the host's AAC encoder (aac_encoder.hpp) for the media tests: 44.1 and 48 kHz, mono
// and stereo, 96, 128 and 192 kb/s; 2048 frames of delay; a whole 1024-frame packet as soon as the delay and the
// input fill one, the rest at the drain. Packet k is 8 bytes: k (little endian), then "AAC!". With `none`, no encoder
// exists.

#include "alchemy/darwin/media/aac_encoder.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace fake_aac {

namespace aac = alchemy::gui::completion_kit::media::aac;

inline constexpr std::uint32_t priming = 2048;

class Encoder final : public aac::Encoder {
public:
    Encoder(std::uint32_t rate, std::uint32_t channels, std::uint32_t bit_rate) : rate_(rate), channels_(channels), bit_rate_(bit_rate) {}
    bool encode(std::span<const std::int16_t> samples, std::vector<std::vector<std::uint8_t>>& out) override {
        frames_ += samples.size() / channels_;
        while ((emitted_ + 1) * aac::frames_per_packet <= fake_aac::priming + frames_) out.push_back(packet(emitted_++));
        return true;
    }
    bool drain(std::vector<std::vector<std::uint8_t>>& out) override {
        while (emitted_ * aac::frames_per_packet < fake_aac::priming + frames_) out.push_back(packet(emitted_++));
        return true;
    }
    std::vector<std::uint8_t> audio_specific_config() const override {
        const std::uint32_t index = rate_ == 48000 ? 3 : 4;   // the sampling frequency index of 48 or 44.1 kHz
        return {static_cast<std::uint8_t>((2 << 3) | (index >> 1)), static_cast<std::uint8_t>(((index & 1) << 7) | (channels_ << 3))};
    }
    std::uint32_t bit_rate() const override { return bit_rate_; }
    std::uint32_t priming() const override { return fake_aac::priming; }

private:
    static std::vector<std::uint8_t> packet(std::uint64_t k) {
        return {static_cast<std::uint8_t>(k), static_cast<std::uint8_t>(k >> 8), static_cast<std::uint8_t>(k >> 16), static_cast<std::uint8_t>(k >> 24),
                'A', 'A', 'C', '!'};
    }
    std::uint32_t rate_, channels_, bit_rate_;
    std::uint64_t frames_{}, emitted_{};
};

inline void install(bool none = false) {
    aac::Backend backend;
    backend.capabilities = [none] {
        aac::Capabilities c;
        if (!none) c = {{44100, 48000}, {1, 2}, {96000, 128000, 192000}};
        return c;
    };
    backend.open = [none](std::uint32_t rate, std::uint32_t channels, std::uint32_t bit_rate, std::string& why) -> std::unique_ptr<aac::Encoder> {
        if (none) { why = "no AAC encoder"; return nullptr; }
        if ((rate != 44100 && rate != 48000) || channels < 1 || channels > 2) { why = "not taken"; return nullptr; }
        std::uint32_t chosen = 128000;
        if (bit_rate)
            for (const std::uint32_t r : {96000u, 128000u, 192000u})
                if ((r > bit_rate ? r - bit_rate : bit_rate - r) < (chosen > bit_rate ? chosen - bit_rate : bit_rate - chosen)) chosen = r;
        return std::make_unique<Encoder>(rate, channels, chosen);
    };
    aac::set_backend(std::move(backend));
}

} // namespace fake_aac
