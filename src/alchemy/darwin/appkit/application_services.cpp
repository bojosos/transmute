#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <string>
#include <vector>

// AppKit gaps of the Qt WebEngine images (install_appkit_gaps, called by the
// fwgaps aggregator): NSRunningApplication (the current application only; it
// activates, hides and unhides itself through NSApp), NSSpeechSynthesizer
// (below), the text system's kill ring, and shells
// for NSSpellChecker, NSAccessibilityRemoteUIElement and
// NSTitlebarAccessoryViewController. NSSetFocusRingStyle draws nothing and
// says so once on stderr.
//
// NSSpeechSynthesizer speaks through AVSpeechSynthesizer (AVFAudio, over the
// host's voices: host::speech), so its voices are the host's: their
// identifiers, names, locales and genders (+availableVoices, the host's
// default voice first; +attributesForVoice:). -startSpeakingString: speaks
// with the synthesizer's voice, rate (words per minute, 180 being the voice's
// normal speed, 90 to 540) and volume, stopping what was being spoken; the
// delegate hears -speechSynthesizer:willSpeakWord:ofString: as each word
// starts and -speechSynthesizer:didFinishSpeaking: (NO when stopped, or when
// there is no audio output to speak to). Stopping and pausing happen at once
// or at the next word (a sentence boundary is taken as a word boundary). The
// pitch base property is kept but not applied. Without host voices the
// initializers return nil, as before; writing speech to a file stops
// explicitly. The voice attribute keys other than VoiceName and
// VoiceLocaleIdentifier follow their constants' names (unverified).
namespace alchemy::gui::fill {
namespace {

struct SpeechState final : ObjectPayload {
    Address synthesizer{};             // +1 AVSpeechSynthesizer (made on first speech)
    Address relay{};                   // +1 its delegate, which tells ours
    Address delegate{};                // not retained
    Address voice{};                   // +1 the voice identifier
    Address text{};                    // +1 the string being spoken
    double rate{180}, volume{1}, pitch_base{};
    bool feedback{};
};
struct RelayState final : ObjectPayload { Address owner{}; };   // the NSSpeechSynthesizer (not retained)

// Words per minute as an AVSpeechUtterance rate (0 to 1, 0.5 normal: half to
// three times the voice's speed).
// float arguments and results (s0).
float single(const CpuState& s) { return std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0])); }
void return_single(CpuState& s, float value) { s.v[0] = {std::bit_cast<std::uint32_t>(value), 0}; }

float utterance_rate(double words_per_minute) {
    const double speed = std::clamp(words_per_minute / 180, 0.5, 3.0);
    return static_cast<float>(speed <= 1 ? speed - 0.5 : 0.5 + (speed - 1) / 4);
}

Address send(ObjcRuntime& objc, Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
    CallRegs registers{};
    unsigned index = 2;
    for (const auto argument : arguments) registers.x[index++] = argument;
    return objc.send(receiver, selector, registers).x0;
}

class AppKitGaps final : public Provider {
public:
    using Provider::Provider;
    std::mutex mutex;
    Address current_application{};
    // NSAnimationContext is QLab's (windows/window_extras.cpp), installed first.
    // Kill ring.
    std::vector<std::string> ring;
    bool new_sequence{true};

    Address app() {
        const auto found = process.resolve(appkit, "_NSApp");
        return found ? memory().read64(found->address) : 0;
    }
    void install();
    void install_strings();
    void install_running_application();
    void install_speech_and_spelling();
    void install_speech();
    std::set<Address> synthesizers;    // live NSSpeechSynthesizers (for +isAnyApplicationSpeaking)
    void install_kill_ring();
};

void AppKitGaps::install_strings() {
    const std::array<std::pair<const char*, const char*>, 19> names{{
        {"_NSAccessibilityHelpTagRole", "AXHelpTag"},
        {"_NSControlTintDidChangeNotification", "NSControlTintDidChangeNotification"},
        {"_NSHTMLPboardType", "Apple HTML pasteboard type"},
        {"_NSImageCompressionFactor", "NSImageCompressionFactor"},
        {"_NSPasteboardNameFind", "Apple CFPasteboard find"},
        {"_NSPreferredScrollerStyleDidChangeNotification", "NSPreferredScrollerStyleDidChangeNotification"},
        {"_NSPrintAllPages", "NSPrintAllPages"},
        {"_NSPrintFirstPage", "NSFirstPage"},
        {"_NSPrintLastPage", "NSLastPage"},
        {"_NSRTFPboardType", "NeXT Rich Text Format v1.0 pasteboard type"},
        {"_NSScreenColorSpaceDidChangeNotification", "NSScreenColorSpaceDidChangeNotification"},
        {"_NSSpeechPitchBaseProperty", "pbas"},
        {"_NSSpeechRateProperty", "rate"},
        {"_NSSpeechVolumeProperty", "volm"},
        {"_NSVoiceLocaleIdentifier", "VoiceLocaleIdentifier"},
        {"_NSVoiceName", "VoiceName"},
        {"_NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification", "NSWorkspaceAccessibilityDisplayOptionsDidChangeNotification"},
        {"_NSWorkspaceActiveSpaceDidChangeNotification", "NSWorkspaceActiveSpaceDidChangeNotification"},
        {"_NSWorkspaceLaunchConfigurationArguments", "NSWorkspaceLaunchConfigurationArguments"}}};
    for (const auto& [symbol, value] : names) string(appkit, symbol, value);
    // void NSSetFocusRingStyle(NSFocusRingPlacement): focus rings are not drawn.
    raw(appkit, "_NSSetFocusRingStyle", [this](CpuState&, GuestMemory&) {
        notice_once("NSSetFocusRingStyle: focus rings are not drawn under Alchemy");
    });
}

void AppKitGaps::install_running_application() {
    const auto running = cls(appkit, "NSRunningApplication", "NSObject");
    const auto current = [this, running]() {
        std::lock_guard lock(mutex);
        if (!current_application) {
            current_application = objc.allocate(running);
            if (const auto record = process.objects().find(current_application)) record->immortal = true;
        }
        return current_application;
    };
    const auto bundle_identifier = [this]() -> Address {
        const auto bundle = call(core_foundation, "_CFBundleGetMainBundle", {});
        return bundle ? call(core_foundation, "_CFBundleGetIdentifier", {bundle}) : 0;
    };
    method(running, "currentApplication", [current](CpuState& s, GuestMemory&) { s.x[0] = current(); }, true);
    method(running, "runningApplicationWithProcessIdentifier:", [this, current](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint32_t>(s.x[2]) == static_cast<std::uint32_t>(call(libsystem, "_getpid", {})) ? current() : 0;
    }, true, "@@:i");
    // Only the current application is known to Alchemy.
    method(running, "runningApplicationsWithBundleIdentifier:", [this, current, bundle_identifier](CpuState& s, GuestMemory&) {
        const auto mine = bundle_identifier();
        std::vector<Address> values;
        if (s.x[2] && mine && cf.equal(s.x[2], mine)) values.push_back(current());
        s.x[0] = bridge->autorelease(cf.array(values));
    }, true);
    method(running, "processIdentifier", [this](CpuState& s, GuestMemory&) { s.x[0] = call(libsystem, "_getpid", {}) & 0xffffffffULL; }, false, "i@:");
    method(running, "bundleIdentifier", [bundle_identifier](CpuState& s, GuestMemory&) { s.x[0] = bundle_identifier(); });
    method(running, "localizedName", [this](CpuState& s, GuestMemory&) {
        const auto info = send(objc, objc.find_class("NSProcessInfo"), "processInfo");
        s.x[0] = info ? send(objc, info, "processName") : 0;
    });
    // The application's icon (the running application is this process).
    method(running, "icon", [this](CpuState& s, GuestMemory&) {
        const auto image_class = objc.find_class("NSImage");
        const auto name = bridge->autorelease(cf.string("NSApplicationIcon"));
        s.x[0] = image_class ? send(objc, image_class, "imageNamed:", {name}) : 0;
    });
    method(running, "bundleURL", [this](CpuState& s, GuestMemory&) {
        const auto bundle = call(core_foundation, "_CFBundleGetMainBundle", {});
        s.x[0] = bundle ? bridge->autorelease(call(core_foundation, "_CFBundleCopyBundleURL", {bundle})) : 0;
    });
    method(running, "executableURL", [this](CpuState& s, GuestMemory&) {
        const auto bundle = call(core_foundation, "_CFBundleGetMainBundle", {});
        s.x[0] = bundle ? bridge->autorelease(call(core_foundation, "_CFBundleCopyExecutableURL", {bundle})) : 0;
    });
    method(running, "executableArchitecture", [](CpuState& s, GuestMemory&) { s.x[0] = 0x0100000c; }, false, "q@:");   // ARM64
    method(running, "isActive", [this](CpuState& s, GuestMemory&) { const auto a = app(); s.x[0] = a ? send(objc, a, "isActive") & 0xff : 0; }, false, "B@:");
    method(running, "ownsMenuBar", [this](CpuState& s, GuestMemory&) { const auto a = app(); s.x[0] = a ? send(objc, a, "isActive") & 0xff : 0; }, false, "B@:");
    method(running, "isHidden", [this](CpuState& s, GuestMemory&) { const auto a = app(); s.x[0] = a ? send(objc, a, "isHidden") & 0xff : 0; }, false, "B@:");
    method(running, "activationPolicy", [this](CpuState& s, GuestMemory&) { const auto a = app(); s.x[0] = a ? send(objc, a, "activationPolicy") : 0; }, false, "q@:");
    method(running, "isFinishedLaunching", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, false, "B@:");
    method(running, "isTerminated", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
    method(running, "launchDate", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // The current application activates, hides and unhides itself through
    // NSApp. Only it can exist here, and it is not terminated this way (the
    // calls report failure, as for an application that refuses to quit).
    const struct { const char* selector; const char* app_selector; const char* types; } own[] = {
        {"activateWithOptions:", "activateIgnoringOtherApps:", "B@:Q"}, {"hide", "hide:", "B@:"}, {"unhide", "unhide:", "B@:"}};
    for (const auto& [selector, app_selector, types] : own)
        method(running, selector, [this, current, app_selector](CpuState& s, GuestMemory&) {
            const auto a = app();
            if (!a || s.x[0] != current()) { s.x[0] = 0; return; }
            // activateIgnoringOtherApps: YES; hide:/unhide: with no sender.
            static_cast<void>(send(objc, a, app_selector, {std::string_view(app_selector) == "activateIgnoringOtherApps:" ? Address{1} : Address{0}}));
            s.x[0] = 1;
        }, false, types);
    for (const auto* selector : {"terminate", "forceTerminate"})
        method(running, selector, [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:");
}

void AppKitGaps::install_speech_and_spelling() {
    install_speech();
    // NSSpellChecker's class (Chrome support's spelling.cpp implements it).
    static_cast<void>(cls(appkit, "NSSpellChecker", "NSObject"));
    static_cast<void>(cls(appkit, "NSAccessibilityRemoteUIElement", "NSObject"));
    static_cast<void>(cls(appkit, "NSTitlebarAccessoryViewController", "NSViewController"));
}

void AppKitGaps::install_speech() {
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSVoiceIdentifier", "VoiceIdentifier"}, {"_NSVoiceGender", "VoiceGender"}, {"_NSVoiceAge", "VoiceAge"},
             {"_NSVoiceLanguage", "VoiceLanguage"}, {"_NSVoiceGenderMale", "VoiceGenderMale"},
             {"_NSVoiceGenderFemale", "VoiceGenderFemale"}, {"_NSVoiceGenderNeuter", "VoiceGenderNeuter"}})
        string(appkit, symbol, value);
    const auto speech = cls(appkit, "NSSpeechSynthesizer", "NSObject");
    const auto text_of = [this](Address object) { return object ? cf.utf8(object) : std::string(); };
    const auto auto_string = [this](std::string_view text) { return bridge->autorelease(cf.string(text)); };
    // The host's voices, as AVSpeechSynthesisVoice objects (none without AVFAudio).
    const auto voices = [this]() {
        std::vector<Address> out;
        const auto voice_class = objc.find_class("AVSpeechSynthesisVoice");
        const auto list = voice_class ? send(objc, voice_class, "speechVoices") : 0;
        for (std::uint64_t i = 0, n = list ? cf.length(list) : 0; i < n; ++i) out.push_back(cf.element(list, i));
        return out;
    };
    const auto voice_with = [this, voices, text_of](Address identifier) -> Address {
        const auto wanted = text_of(identifier);
        for (const auto voice : voices())
            if (text_of(send(objc, voice, "identifier")) == wanted) return voice;
        return 0;
    };
    const auto state_of = [this](Address self) {
        const auto state = objc.find_host_state<SpeechState>(self);
        if (!state) throw std::logic_error("Alchemy: an NSSpeechSynthesizer that was not initialized");
        return state;
    };
    method(speech, "availableVoices", [this, voices](CpuState& s, GuestMemory&) {
        std::vector<Address> identifiers;
        for (const auto voice : voices()) identifiers.push_back(send(objc, voice, "identifier"));
        s.x[0] = bridge->autorelease(cf.array(identifiers));
    }, true);
    method(speech, "defaultVoice", [this, voices](CpuState& s, GuestMemory&) {
        const auto all = voices();
        s.x[0] = all.empty() ? 0 : send(objc, all.front(), "identifier");
    }, true);
    method(speech, "attributesForVoice:", [this, voice_with, text_of, auto_string](CpuState& s, GuestMemory&) {
        const auto voice = voice_with(s.x[2]);
        if (!voice) { s.x[0] = 0; return; }
        auto locale = text_of(send(objc, voice, "language"));
        const auto language = locale;
        std::replace(locale.begin(), locale.end(), '-', '_');
        const auto gender = send(objc, voice, "gender");   // AVSpeechSynthesisVoiceGender
        const std::array<std::pair<Address, Address>, 5> entries{{
            {auto_string("VoiceIdentifier"), send(objc, voice, "identifier")},
            {auto_string("VoiceName"), send(objc, voice, "name")},
            {auto_string("VoiceLocaleIdentifier"), auto_string(locale)},
            {auto_string("VoiceLanguage"), auto_string(language)},
            {auto_string("VoiceGender"), auto_string(gender == 1 ? "VoiceGenderMale" : gender == 2 ? "VoiceGenderFemale" : "VoiceGenderNeuter")}}};
        s.x[0] = bridge->autorelease(cf.dictionary(entries));
    }, true);
    method(speech, "isAnyApplicationSpeaking", [this](CpuState& s, GuestMemory&) {
        bool any = false;
        for (const auto self : synthesizers)
            if (const auto state = objc.find_host_state<SpeechState>(self); state && state->synthesizer)
                any = any || (send(objc, state->synthesizer, "isSpeaking") & 0xff) != 0;
        s.x[0] = any ? 1 : 0;
    }, true, "B@:");
    method(speech, "init", [this](CpuState& s, GuestMemory&) { s.x[0] = send(objc, s.x[0], "initWithVoice:", {0}); });
    method(speech, "initWithVoice:", [this, voices, voice_with](CpuState& s, GuestMemory&) {
        const auto all = voices();
        const auto chosen = s.x[2] ? voice_with(s.x[2]) : all.empty() ? 0 : all.front();
        if (!chosen) {   // no host voices, or not one of them
            bridge->release(s.x[0]);
            s.x[0] = 0;
            return;
        }
        const auto state = objc.host_state<SpeechState>(s.x[0], [] { return std::make_shared<SpeechState>(); });
        state->voice = bridge->retain(send(objc, chosen, "identifier"));
        synthesizers.insert(s.x[0]);
    });
    method(speech, "dealloc", [this, speech](CpuState& s, GuestMemory&) {
        if (const auto state = objc.find_host_state<SpeechState>(s.x[0])) {
            if (state->synthesizer) {
                static_cast<void>(send(objc, state->synthesizer, "stopSpeakingAtBoundary:", {0}));
                static_cast<void>(send(objc, state->synthesizer, "setDelegate:", {0}));
            }
            if (state->relay) objc.host_state<RelayState>(state->relay)->owner = 0;
            for (auto* slot : {&state->synthesizer, &state->relay, &state->voice, &state->text}) bridge->release(std::exchange(*slot, 0));
        }
        synthesizers.erase(s.x[0]);
        static_cast<void>(objc.send_super(s.x[0], speech, objc.selector("dealloc")));
    }, false, "v@:");
    // The AV synthesizer's delegate: tells the NSSpeechSynthesizer's.
    const auto relay_class = objc.register_native_class("AlchemyNSSpeechRelay", objc.find_class("NSObject"), 8);
    const auto tell_finished = [this](Address relay, bool finished) {
        const auto owner = objc.host_state<RelayState>(relay)->owner;
        const auto state = owner ? objc.find_host_state<SpeechState>(owner) : nullptr;
        if (!state || !state->delegate) return;
        CallRegs r{};
        r.x[2] = owner;
        r.x[3] = finished ? 1 : 0;
        static_cast<void>(objc.send_if_responds(state->delegate, objc.selector("speechSynthesizer:didFinishSpeaking:"), r));
    };
    static_cast<void>(objc.add_native_method(relay_class, "speechSynthesizer:didFinishSpeechUtterance:",
        [tell_finished](CpuState& s, GuestMemory&) { tell_finished(s.x[0], true); }, false, "v@:@@"));
    static_cast<void>(objc.add_native_method(relay_class, "speechSynthesizer:didCancelSpeechUtterance:",
        [tell_finished](CpuState& s, GuestMemory&) { tell_finished(s.x[0], false); }, false, "v@:@@"));
    static_cast<void>(objc.add_native_method(relay_class, "speechSynthesizer:willSpeakRangeOfSpeechString:utterance:",
        [this](CpuState& s, GuestMemory&) {
            const auto owner = objc.host_state<RelayState>(s.x[0])->owner;
            const auto state = owner ? objc.find_host_state<SpeechState>(owner) : nullptr;
            if (!state || !state->delegate) return;
            CallRegs r{};
            r.x[2] = owner;
            r.x[3] = s.x[3];   // NSRange location, length
            r.x[4] = s.x[4];
            r.x[5] = state->text;
            static_cast<void>(objc.send_if_responds(state->delegate, objc.selector("speechSynthesizer:willSpeakWord:ofString:"), r));
        }, false, "v@:@{_NSRange=QQ}@"));
    method(speech, "startSpeakingString:", [this, state_of, relay_class](CpuState& s, GuestMemory&) {
        const auto self = s.x[0];
        const auto state = state_of(self);
        const auto synth_class = objc.find_class("AVSpeechSynthesizer"), utterance_class = objc.find_class("AVSpeechUtterance");
        const auto voice_class = objc.find_class("AVSpeechSynthesisVoice");
        if (!synth_class || !utterance_class || !voice_class)
            throw std::runtime_error("unsupported: Alchemy: -[NSSpeechSynthesizer startSpeakingString:] without AVFAudio's speech synthesis");
        if (!state->synthesizer) {
            state->synthesizer = send(objc, send(objc, synth_class, "alloc"), "init");
            state->relay = objc.allocate(relay_class);
            objc.host_state<RelayState>(state->relay)->owner = self;
            static_cast<void>(send(objc, state->synthesizer, "setDelegate:", {state->relay}));
        }
        // What was being spoken stops first (its delegate hears NO).
        if (send(objc, state->synthesizer, "isSpeaking") & 0xff) static_cast<void>(send(objc, state->synthesizer, "stopSpeakingAtBoundary:", {0}));
        const auto utterance = send(objc, utterance_class, "speechUtteranceWithString:", {s.x[2]});
        static_cast<void>(send(objc, utterance, "setVoice:", {send(objc, voice_class, "voiceWithIdentifier:", {state->voice})}));
        CallRegs rate{};
        rate.set_s(0, utterance_rate(state->rate));
        static_cast<void>(objc.send(utterance, "setRate:", rate));
        CallRegs volume{};
        volume.set_s(0, static_cast<float>(std::clamp(state->volume, 0.0, 1.0)));
        static_cast<void>(objc.send(utterance, "setVolume:", volume));
        const auto text = bridge->retain(s.x[2]);
        bridge->release(std::exchange(state->text, text));
        static_cast<void>(send(objc, state->synthesizer, "speakUtterance:", {utterance}));
        s.x[0] = 1;
    }, false, "B@:@");
    method(speech, "startSpeakingString:toURL:", [](CpuState&, GuestMemory&) {
        throw std::runtime_error("unsupported: Alchemy: -[NSSpeechSynthesizer startSpeakingString:toURL:] (writing speech to a file)");
    }, false, "B@:@@");
    // NSSpeechBoundary (immediate 0, word 1, sentence 2) as AVSpeechBoundary (immediate 0, word 1).
    const auto forward = [this, state_of](const char* av_selector, std::optional<Address> boundary) {
        return [this, state_of, av_selector, boundary](CpuState& s, GuestMemory&) {
            const auto state = state_of(s.x[0]);
            if (!state->synthesizer) return;
            if (boundary) static_cast<void>(send(objc, state->synthesizer, av_selector, {*boundary == 0 ? 0U : (s.x[2] == 0 ? 0U : 1U)}));
            else static_cast<void>(send(objc, state->synthesizer, av_selector));
        };
    };
    method(speech, "stopSpeaking", forward("stopSpeakingAtBoundary:", Address{0}), false, "v@:");
    method(speech, "stopSpeakingAtBoundary:", forward("stopSpeakingAtBoundary:", Address{1}), false, "v@:q");
    method(speech, "pauseSpeakingAtBoundary:", forward("pauseSpeakingAtBoundary:", Address{1}), false, "v@:q");
    method(speech, "continueSpeaking", forward("continueSpeaking", std::nullopt), false, "v@:");
    method(speech, "isSpeaking", [this, state_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        s.x[0] = state->synthesizer && (send(objc, state->synthesizer, "isSpeaking") & 0xff) ? 1 : 0;
    }, false, "B@:");
    method(speech, "delegate", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->delegate; });
    method(speech, "setDelegate:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->delegate = s.x[2]; }, false, "v@:@");
    method(speech, "voice", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->voice; });
    method(speech, "setVoice:", [this, state_of, voice_with, voices](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        if (state->synthesizer && (send(objc, state->synthesizer, "isSpeaking") & 0xff)) { s.x[0] = 0; return; }
        const auto all = voices();
        const auto chosen = s.x[2] ? voice_with(s.x[2]) : all.empty() ? 0 : all.front();
        if (!chosen) { s.x[0] = 0; return; }
        const auto identifier = bridge->retain(send(objc, chosen, "identifier"));
        bridge->release(std::exchange(state->voice, identifier));
        s.x[0] = 1;
    }, false, "B@:@");
    method(speech, "rate", [state_of](CpuState& s, GuestMemory&) { return_single(s, static_cast<float>(state_of(s.x[0])->rate)); }, false, "f@:");
    method(speech, "setRate:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->rate = std::clamp(static_cast<double>(single(s)), 90.0, 540.0); }, false, "v@:f");
    method(speech, "volume", [state_of](CpuState& s, GuestMemory&) { return_single(s, static_cast<float>(state_of(s.x[0])->volume)); }, false, "f@:");
    method(speech, "setVolume:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->volume = std::clamp(static_cast<double>(single(s)), 0.0, 1.0); }, false, "v@:f");
    method(speech, "usesFeedbackWindow", [state_of](CpuState& s, GuestMemory&) { s.x[0] = state_of(s.x[0])->feedback ? 1 : 0; }, false, "B@:");
    method(speech, "setUsesFeedbackWindow:", [state_of](CpuState& s, GuestMemory&) { state_of(s.x[0])->feedback = (s.x[2] & 0xff) != 0; }, false, "v@:B");
    // The rate ("rate"), volume ("volm") and pitch base ("pbas") properties.
    method(speech, "objectForProperty:error:", [this, state_of, text_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto key = text_of(s.x[2]);
        const double* value = key == "rate" ? &state->rate : key == "volm" ? &state->volume : key == "pbas" ? &state->pitch_base : nullptr;
        if (s.x[3]) memory().write64(s.x[3], 0);
        if (!value) { s.x[0] = 0; return; }
        CallRegs r{};
        r.set_d(0, *value);
        s.x[0] = objc.send(objc.find_class("NSNumber"), "numberWithDouble:", r).x0;
    }, false, "@@:@^@");
    method(speech, "setObject:forProperty:error:", [this, state_of, text_of](CpuState& s, GuestMemory&) {
        const auto state = state_of(s.x[0]);
        const auto key = text_of(s.x[3]);
        double* value = key == "rate" ? &state->rate : key == "volm" ? &state->volume : key == "pbas" ? &state->pitch_base : nullptr;
        if (s.x[4]) memory().write64(s.x[4], 0);
        if (!value || !s.x[2]) { s.x[0] = 0; return; }
        *value = objc.send(s.x[2], "doubleValue", CallRegs{}).d(0);
        if (key == "rate") *value = std::clamp(*value, 90.0, 540.0);
        if (key == "volm") *value = std::clamp(*value, 0.0, 1.0);
        s.x[0] = 1;
    }, false, "B@:@@^@");
}

void AppKitGaps::install_kill_ring() {
    // The text system's kill ring (Emacs-style kill/yank): consecutive kills
    // in one sequence join into one entry.
    raw(appkit, "__NSInitializeKillRing", [this](CpuState&, GuestMemory&) {
        std::lock_guard lock(mutex);
        ring.clear();
        new_sequence = true;
    });
    raw(appkit, "__NSNewKillRingSequence", [this](CpuState&, GuestMemory&) { std::lock_guard lock(mutex); new_sequence = true; });
    raw(appkit, "__NSSetKillRingToYankedState", [this](CpuState&, GuestMemory&) { std::lock_guard lock(mutex); new_sequence = true; });
    const auto add = [this](Address string, bool prepend) {
        const auto value = string ? cf.utf8(string) : std::string();
        std::lock_guard lock(mutex);
        if (new_sequence || ring.empty()) {
            ring.push_back(value);
            if (ring.size() > 16) ring.erase(ring.begin());
            new_sequence = false;
        } else if (prepend) ring.back() = value + ring.back();
        else ring.back() += value;
    };
    raw(appkit, "__NSAppendToKillRing", [add](CpuState& s, GuestMemory&) { add(s.x[0], false); });
    raw(appkit, "__NSPrependToKillRing", [add](CpuState& s, GuestMemory&) { add(s.x[0], true); });
    raw(appkit, "__NSYankFromKillRing", [this](CpuState& s, GuestMemory&) {
        std::string value;
        { std::lock_guard lock(mutex); if (!ring.empty()) value = ring.back(); }
        s.x[0] = bridge->autorelease(cf.string(value));
    });
}

void AppKitGaps::install() {
    install_strings();
    install_running_application();
    install_speech_and_spelling();
    install_kill_ring();
}

} // namespace

void install_appkit_gaps(InstallContext& context) { std::make_shared<AppKitGaps>(context)->install(); }

} // namespace alchemy::gui::fill
