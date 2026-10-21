#include "alchemy/darwin/media/media.hpp"

#include <array>

// Audio and MIDI gaps.
//
// - CoreAudio HAL and CoreMIDI are the QLab media group's
//   (src/alchemy/darwin/kit/completion/media: hal.cpp over WASAPI, coremidi.cpp over
//   winmm), installed earlier by install_completion_kit.
// - AudioToolbox: no converter supports any format pair ('fmt?'), no format
//   property is available ('prop'), and no input device exists for audio
//   queues. Since converters and queues cannot exist, calls on them answer
//   kAudio_ParamError.
// - AudioUnit: no component or unit instance can exist (badComponentInstance,
//   as the existing AudioUnit provider answers; component queries on NULL
//   answer kAudio_ParamError).
namespace alchemy::gui::fill {
namespace {

constexpr std::int32_t param_err = -50;
constexpr std::int32_t bad_component_instance = static_cast<std::int32_t>(0x80008001U);
constexpr std::int32_t audio_queue_invalid_device = -66680;

class Audio final : public Provider {
public:
    using Provider::Provider;
    void install_audio_toolbox();
    void install_audio_unit();
};

void Audio::install_audio_toolbox() {
    failure(audio_toolbox, "_AudioConverterNew", static_cast<std::int32_t>(fourcc("fmt?")), {2});
    for (const auto* name : {"_AudioConverterDispose", "_AudioConverterReset", "_AudioConverterGetProperty", "_AudioConverterGetPropertyInfo",
                             "_AudioConverterSetProperty", "_AudioConverterFillComplexBuffer"})
        failure(audio_toolbox, name, param_err);
    failure(audio_toolbox, "_AudioFormatGetProperty", static_cast<std::int32_t>(fourcc("prop")));
    failure(audio_toolbox, "_AudioFormatGetPropertyInfo", static_cast<std::int32_t>(fourcc("prop")));
    // AudioQueueNewInput(format, callback, userData, runLoop, runLoopMode, flags, AudioQueueRef *out): no input device.
    failure(audio_toolbox, "_AudioQueueNewInput", audio_queue_invalid_device, {6});
    for (const auto* name : {"_AudioQueueAllocateBuffer", "_AudioQueueDispose", "_AudioQueueEnqueueBuffer", "_AudioQueueStart", "_AudioQueueStop"})
        failure(audio_toolbox, name, param_err);
}

void Audio::install_audio_unit() {
    for (const auto* name : {"_AudioUnitGetProperty", "_AudioUnitGetPropertyInfo", "_AudioUnitRender", "_AudioUnitReset",
                             "_AudioUnitGetParameter", "_AudioUnitSetParameter", "_AudioUnitAddRenderNotify", "_AudioUnitRemoveRenderNotify"})
        failure(audio_unit, name, bad_component_instance);
    // No AudioComponent exists (AudioComponentFindNext finds none): NULL answers kAudio_ParamError,
    // anything else is not a component.
    raw(audio_unit, "_AudioComponentCopyName", [](CpuState& s, GuestMemory& m) {
        if (s.x[0]) throw std::runtime_error("Alchemy: AudioComponentCopyName received a component that cannot exist under Alchemy");
        if (s.x[1]) m.write64(s.x[1], 0);
        s.x[0] = status(param_err);
    });
    raw(audio_unit, "_AudioComponentGetDescription", [](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy: AudioComponentGetDescription received a component that cannot exist under Alchemy");
        s.x[0] = status(param_err);
    });
}

} // namespace

void install_audio(InstallContext& context) {
    const auto provider = std::make_shared<Audio>(context);
    provider->install_audio_toolbox();
    provider->install_audio_unit();
}

} // namespace alchemy::gui::fill
