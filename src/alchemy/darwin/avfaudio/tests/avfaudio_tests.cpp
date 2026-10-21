// AVFAudio (src/alchemy/darwin/avfaudio/): the identity, and the documented
// "unavailable" answers.
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/AVFAudio.framework/Versions/A/AVFAudio"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case avfaudio_case("avfaudio", [](Fixture& f) {
    const auto library = std::string_view(LIBRARY);
    require(alchemy::ProcessContext::is_provider_library(library), "AVFAudio identity");
    require(std::bit_cast<float>(f.engine.read32(f.symbol(library, "_AVSpeechUtteranceDefaultSpeechRate"))) == 0.5f, "default speech rate");
    require(f.call(cf_lib, "_CFArrayGetCount", {f.send(f.cls("AVSpeechSynthesisVoice"), "speechVoices")}) == 0, "no voices");
    const auto synthesizer = f.send(f.send(f.cls("AVSpeechSynthesizer"), "alloc"), "init");
    require(f.send(synthesizer, "isSpeaking") == 0 && f.send(synthesizer, "stopSpeakingAtBoundary:", {0}) == 0, "nothing is spoken or stopped");
});

#undef LIBRARY
} // namespace
