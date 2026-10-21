#include "alchemy/darwin/avfaudio/install.hpp"
#include "alchemy/gui/value_provider.hpp"

// AVFAudio speech synthesis: no speech voices are installed (+speechVoices is
// empty), so the synthesizer never speaks and stop/pause report that nothing
// was stopped (NO). AVSpeechUtterance is a real value object.
// String constants carry the values of Apple's public headers; values
// marked "(unverified)" follow the framework's naming convention and could
// not be checked against a header. Chrome 155 first linked the framework (moved from
// darwin/identities/identities.cpp).
namespace alchemy::avfaudio {
namespace {

namespace fill = gui::fill;
using fill::Address;
using transmute::CpuState;
using transmute::GuestMemory;

constexpr std::string_view avf_audio = "/System/Library/Frameworks/AVFAudio.framework/Versions/A/AVFAudio";

class Provider final : public gui::values::Provider {
public:
    using gui::values::Provider::Provider;
    void install();
};

void Provider::install() {
    ProcessContext::add_provider_library(std::string(avf_audio));
    const auto empty_array = [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); };
    const auto nil = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };
    const auto no = [](CpuState& s, GuestMemory&) { s.x[0] = 0; };
    float_constant(avf_audio, "_AVSpeechUtteranceMinimumSpeechRate", 0.0f);
    float_constant(avf_audio, "_AVSpeechUtteranceDefaultSpeechRate", 0.5f);
    float_constant(avf_audio, "_AVSpeechUtteranceMaximumSpeechRate", 1.0f);
    const auto voice = cls(avf_audio, "AVSpeechSynthesisVoice", "NSObject");
    method(voice, "speechVoices", empty_array, true);
    method(voice, "voiceWithLanguage:", nil, true);
    method(voice, "voiceWithIdentifier:", nil, true);
    const auto utterance = cls(avf_audio, "AVSpeechUtterance", "NSObject");
    object_properties(utterance, {"speechString", "voice"});
    real_properties(utterance, {"rate", "pitchMultiplier", "volume"}, true);
    real_properties(utterance, {"preUtteranceDelay", "postUtteranceDelay"});
    method(utterance, "initWithString:", [this](CpuState& s, GuestMemory&) {
        store(s.x[0], "speechString", s.x[2]);
        const auto state = properties(s.x[0]);
        std::lock_guard lock(state->mutex);
        state->reals["rate"] = 0.5;   // AVSpeechUtteranceDefaultSpeechRate
        state->reals["pitchMultiplier"] = 1.0;
        state->reals["volume"] = 1.0;
    });
    method(utterance, "speechUtteranceWithString:", [this, utterance](CpuState& s, GuestMemory&) {
        CallRegs registers{};
        registers.x[2] = s.x[2];
        s.x[0] = bridge->autorelease(objc.send(objc.allocate(utterance), "initWithString:", registers).x0);
    }, true);
    const auto synthesizer = cls(avf_audio, "AVSpeechSynthesizer", "NSObject");
    weak_properties(synthesizer, {"delegate"});
    method(synthesizer, "isSpeaking", no, false, "B@:");
    method(synthesizer, "isPaused", no, false, "B@:");
    // Nothing is ever speaking, so nothing is stopped or paused (NO).
    method(synthesizer, "stopSpeakingAtBoundary:", no, false, "B@:q");
    method(synthesizer, "pauseSpeakingAtBoundary:", no, false, "B@:q");
    method(synthesizer, "continueSpeaking", no, false, "B@:");
}

} // namespace

void install_avfaudio(gui::InstallContext& context) { std::make_shared<Provider>(context)->install(); }

} // namespace alchemy::avfaudio
