// AVFAudio (src/alchemy/darwin/avfaudio/): the identity, the utterance value object, and
// an idle synthesizer (media/avaudio_speech.cpp speaks; media_guest_tests checks it).
#include "alchemy/gui/tests/framework_case.hpp"

namespace {
using namespace fill_test;

#define LIBRARY "/System/Library/Frameworks/AVFAudio.framework/Versions/A/AVFAudio"
[[maybe_unused]] constexpr std::string_view cf_lib = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

const Case avfaudio_case("avfaudio", [](Fixture& f) {
    const auto library = std::string_view(LIBRARY);
    require(alchemy::ProcessContext::is_provider_library(library), "AVFAudio identity");
    require(std::bit_cast<float>(f.engine.read32(f.symbol(library, "_AVSpeechUtteranceDefaultSpeechRate"))) == 0.5f, "default speech rate");
    const auto utterance = f.send(f.cls("AVSpeechUtterance"), "speechUtteranceWithString:", {f.cf.string("hello")});
    require(f.cf.utf8(f.send(utterance, "speechString")) == "hello", "an utterance");
    const auto synthesizer = f.send(f.send(f.cls("AVSpeechSynthesizer"), "alloc"), "init");
    require(f.send(synthesizer, "isSpeaking") == 0 && f.send(synthesizer, "stopSpeakingAtBoundary:", {0}) == 0, "nothing is spoken or stopped");
});

#undef LIBRARY
} // namespace
