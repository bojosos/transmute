#include "alchemy/darwin/media/media.hpp"

#include <array>

// Audio and MIDI gaps.
//
// - CoreAudio HAL and CoreMIDI are the QLab media group's
//   (src/alchemy/darwin/kit/completion/media: hal.cpp over WASAPI, coremidi.cpp over
//   winmm), installed earlier by install_completion_kit.
// - AudioToolbox: no input device exists for audio queues. AudioConverter,
//   AudioQueue output and AudioFormat properties are audio_converter.cpp's,
//   audio_queue.cpp's and audio_format.cpp's.
// - AudioComponent and the output audio units are audio_units.cpp's.
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t audio_queue_invalid_device = -66680;

class Audio final : public Provider {
public:
    using Provider::Provider;
    void install_audio_toolbox();
};

void Audio::install_audio_toolbox() {
    // AudioQueueNewInput(format, callback, userData, runLoop, runLoopMode, flags, AudioQueueRef *out): no input device.
    failure(audio_toolbox, "_AudioQueueNewInput", audio_queue_invalid_device, {6});
}

} // namespace

void install_audio(InstallContext& context) {
    const auto provider = std::make_shared<Audio>(context);
    provider->install_audio_toolbox();
}

} // namespace alchemy::gui::fill
