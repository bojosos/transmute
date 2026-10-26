#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"

#include <array>
#include <mutex>
#include <string>
#include <vector>

// AppKit gaps of the Qt WebEngine images (install_appkit_gaps, called by the
// fwgaps aggregator): NSRunningApplication (the current application only; it
// activates, hides and unhides itself through NSApp), NSSpeechSynthesizer (no
// voices: initializers return nil), the text system's kill ring, and shells
// for NSSpellChecker, NSAccessibilityRemoteUIElement and
// NSTitlebarAccessoryViewController. NSSetFocusRingStyle draws nothing and
// says so once on stderr.
namespace alchemy::gui::fill {
namespace {

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
    // No speech voices exist under Alchemy.
    const auto speech = cls(appkit, "NSSpeechSynthesizer", "NSObject");
    method(speech, "availableVoices", [this](CpuState& s, GuestMemory&) { s.x[0] = bridge->autorelease(cf.array({})); }, true);
    method(speech, "defaultVoice", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(speech, "attributesForVoice:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true);
    method(speech, "isAnyApplicationSpeaking", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "B@:");
    for (const auto* selector : {"init", "initWithVoice:"}) {
        method(speech, selector, [this](CpuState& s, GuestMemory&) {
            bridge->release(s.x[0]);   // the voice cannot be used: the initializer returns nil
            s.x[0] = 0;
        });
    }
    // NSSpellChecker's class (Chrome support's spelling.cpp implements it).
    static_cast<void>(cls(appkit, "NSSpellChecker", "NSObject"));
    static_cast<void>(cls(appkit, "NSAccessibilityRemoteUIElement", "NSObject"));
    static_cast<void>(cls(appkit, "NSTitlebarAccessoryViewController", "NSViewController"));
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
