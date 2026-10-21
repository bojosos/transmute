#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"
#include "alchemy/darwin_frameworks.hpp"

// AudioUnit / AudioToolbox / CoreAudio: no audio component is registered, so
// no unit instance can be created (CAClock: the media headless shell).
// Moved from darwin/identities/frameworks.cpp (the headless framework set);
// registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

constexpr std::string_view audio_unit = darwin_framework_libraries[12];

// MacErrors.h / CoreAudioBaseTypes.h / Components.h
constexpr std::int32_t param_err = -50;
constexpr std::int32_t bad_component_instance = static_cast<std::int32_t>(0x80008001U);

class AudioComponents final : public Provider {
public:
    using Provider::Provider;
    void install() {
        function(audio_unit, "_AudioComponentFindNext", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        function(audio_unit, "_AudioComponentInstanceNew", [](CpuState& s, GuestMemory& m) {
            if (s.x[1]) m.write64(s.x[1], 0);
            s.x[0] = status(param_err);
        });
        for (const auto* name : {"_AudioComponentInstanceDispose", "_AudioOutputUnitStart", "_AudioOutputUnitStop",
                 "_AudioUnitInitialize", "_AudioUnitSetProperty", "_AudioUnitUninitialize"})
            function(audio_unit, name, [](CpuState& s, GuestMemory&) { s.x[0] = status(bad_component_instance); });
    }
};

} // namespace

void install_media_audio_components(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<AudioComponents>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
