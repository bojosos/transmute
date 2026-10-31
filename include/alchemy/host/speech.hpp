#pragma once

// Host area "speech" (owner: fw-media). The rules are in include/alchemy/host/host.hpp. Text to
// speech: the host's voices, and speech synthesized into PCM samples with the times of its words.
// AVSpeechSynthesizer (src/alchemy/darwin/media/avaudio_speech.cpp) plays it through the audio
// output. GUI-only: the backends are src/alchemy/host/<backend>/gui/speech.cpp, compiled into
// alchemy-fx.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::speech {

struct Voice {
    std::string identifier;            // the host's stable name for it
    std::string name;                  // for people ("Microsoft Zira Desktop")
    std::string language;              // BCP 47 ("en-US"); empty when the host does not say
    enum class Gender { unspecified, male, female } gender{};
};

// The installed voices, in the host's order (its default first when it has one).
[[nodiscard]] Result<std::vector<Voice>> voices();

struct Options {
    double rate{1};                    // speed, a multiple of the voice's normal rate (1/3 to 3)
    double pitch{1};                   // pitch, a multiple of the voice's (1/2 to 2)
    double volume{1};                  // 0 to 1
};

// One word of the text: its UTF-16 code units, and the sample frame where it starts.
struct Word {
    std::size_t offset{}, length{};
    std::uint64_t frame{};
};

// Speech as mono 16-bit samples at `rate` frames per second.
struct Speech {
    std::uint32_t rate{};
    std::vector<std::int16_t> samples;
    std::vector<Word> words;
};

// Speaks `text` (plain text) with the voice named `voice` (empty: the host's default) into
// samples. Error::not_found for an unknown voice; Error::unsupported without speech synthesis.
[[nodiscard]] Result<Speech> synthesize(std::string_view voice, std::u16string_view text, const Options& options);

} // namespace alchemy::host::speech
