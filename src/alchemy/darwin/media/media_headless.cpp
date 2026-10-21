#include "alchemy/darwin/shell_kit.hpp"
#include "alchemy/darwin/shells.hpp"

// alchemy.exe only (darwin/shells.hpp): no CAClock can be created in a
// headless process (from darwin/identities/frameworks.cpp).
namespace alchemy::darwin_shells {
namespace {
constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
// MacErrors.h / CoreAudioBaseTypes.h
constexpr std::int32_t param_err = -50, audio_unimplemented = -4;
} // namespace

void install_media_shell(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    ShellKit kit{context, objc, cf};
    kit.function(audio_toolbox, "_CAClockNew", [](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        s.x[0] = status(audio_unimplemented);
    });
    for (const auto* name : {"_CAClockDispose", "_CAClockGetCurrentTime", "_CAClockGetStartTime",
             "_CAClockSetCurrentTime", "_CAClockSetProperty", "_CAClockStart", "_CAClockStop"})
        kit.function(audio_toolbox, name, [](CpuState& s, GuestMemory&) { s.x[0] = status(param_err); });
}

} // namespace alchemy::darwin_shells
