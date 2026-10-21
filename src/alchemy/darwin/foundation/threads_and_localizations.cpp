#include "alchemy/darwin/kit/support/support_kit.hpp"

#include "alchemy/host/process.hpp"

#include <array>
#include <map>
#include <mutex>

// Foundation: NSFileTypeForHFSTypeCode, NSThread main-thread queries (one
// NSThread object per host thread), NSBundle localizations (the development
// localization only), and the NSMutableString and NSUserNotification(Center)
// class shells. -executableURL is value_classes.cpp's.
// Qt 6.5's bind-time surface first needed them (moved from
// darwin/identities/qt_imports.cpp); installed by the support kit
// (kit/support/install.cpp) at the old position, only when missing.
namespace alchemy::gui::support_kit {

namespace {

constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";

} // namespace

void install_foundation_threads_and_localizations(Support& support) {
    auto* s = &support;
    // ---- Foundation functions (real) ----
    support.fill_function(foundation, "_NSFileTypeForHFSTypeCode", [s](CpuState& c, GuestMemory&) {
        const auto code = static_cast<std::uint32_t>(c.x[0]);
        std::string text = "'";
        for (int shift = 24; shift >= 0; shift -= 8) text.push_back(static_cast<char>((code >> shift) & 0xff));
        text += "'";
        c.x[0] = s->send(s->cf.string(text), "autorelease").x0;
    });

    // ---- NSThread (real main-thread queries) ----
    if (!support.process.resolve(foundation, "_OBJC_CLASS_$_NSThread")) {
        const auto thread = support.declare_class("NSThread", "NSObject", foundation);
        const auto is_main = [s](CpuState& c, GuestMemory&) { c.x[0] = s->on_main_thread() ? 1 : 0; };
        support.method(thread, "isMainThread", is_main, "B@:", true);
        support.method(thread, "isMultiThreaded", [](CpuState& c, GuestMemory&) { c.x[0] = 1; }, "B@:", true);
        // One NSThread object per host thread (immortal: threads outlive autorelease pools).
        struct ThreadObject final : ObjectPayload { bool main{}; Address dictionary{}, name{}; };
        auto objects = std::make_shared<std::pair<std::mutex, std::map<unsigned long, Address>>>();
        const auto current = [s, thread, objects](bool main_only) -> Address {
            const auto id = main_only ? s->main_thread_id : static_cast<unsigned long>(host::process::current_thread_id());
            std::lock_guard lock(objects->first);
            auto& slot = objects->second[id];
            if (!slot) {
                auto payload = std::make_shared<ThreadObject>();
                payload->main = id == s->main_thread_id;
                slot = s->process.objects().create(16, thread, ObjectKind::instance, payload, {}, true);
            }
            return slot;
        };
        support.method(thread, "currentThread", [current](CpuState& c, GuestMemory&) { c.x[0] = current(false); }, "@@:", true);
        support.method(thread, "mainThread", [current](CpuState& c, GuestMemory&) { c.x[0] = current(true); }, "@@:", true);
        support.method(thread, "isMainThread", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            c.x[0] = state && state->main ? 1 : 0;
        }, "B@:");
        support.method(thread, "threadDictionary", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            if (!state) fail("-[NSThread threadDictionary] on an NSThread that is not a running thread");
            if (!state->dictionary) state->dictionary = s->cf.dictionary(std::span<const std::pair<std::uint64_t, std::uint64_t>>{}, true);
            c.x[0] = state->dictionary;
        }, "@@:");
        support.method(thread, "name", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            c.x[0] = state ? state->name : 0;
        }, "@@:");
        support.method(thread, "setName:", [s](CpuState& c, GuestMemory&) {
            const auto state = s->objc.find_host_state<ThreadObject>(c.x[0]);
            if (!state) return;
            const auto old = std::exchange(state->name, c.x[2] ? s->send(c.x[2], "copy").x0 : 0);
            s->release(old);
        }, "v@:@");
    }

    // ---- NSBundle localizations (-executableURL is darwin/foundation/value_classes.cpp's) ----
    if (const auto bundle = support.objc.find_class("NSBundle")) {
        // Localizations: bundles carry the development localization only ("en").
        const auto english = [s] {
            const auto text = s->cf.string("en");
            const std::array<Address, 1> values{text};
            const auto array = s->cf.array(values);
            s->release(text);
            return s->send(array, "autorelease").x0;
        };
        for (const auto* name : {"localizations", "preferredLocalizations"})
            support.method(bundle, name, [english](CpuState& c, GuestMemory&) { c.x[0] = english(); }, "@@:");
        support.method(bundle, "developmentLocalization", [s](CpuState& c, GuestMemory&) {
            c.x[0] = s->send(s->cf.string("en"), "autorelease").x0;
        }, "@@:");
        support.method(bundle, "preferredLocalizationsFromArray:", [s, english](CpuState& c, GuestMemory&) {
            c.x[0] = c.x[2] && s->cf.length(c.x[2]) ? s->send(s->cf.array(std::array<Address, 1>{s->cf.element(c.x[2], 0)}), "autorelease").x0 : english();
        }, "@@:@", true);
        // +bundleForClass: answers the main bundle (images are not bundles of
        // their own here; Qt only reads identifiers and paths from it).
        if (!support.objc.responds(bundle, support.objc.selector("bundleForClass:")))
            support.method(bundle, "bundleForClass:", [s, bundle](CpuState& c, GuestMemory&) {
                c.x[0] = s->send(bundle, "mainBundle").x0;
            }, "@@:#", true);
    }

    const auto shell = [&](std::string_view library, const char* name, const char* superclass) {
        if (support.process.resolve(library, "_OBJC_CLASS_$_" + std::string(name))) return;
        if (!support.objc.find_class(superclass)) return;
        static_cast<void>(support.declare_class(name, superclass, library));
    };
    shell(foundation, "NSMutableString", "NSString");
    shell(foundation, "NSUserNotification", "NSObject");
    shell(foundation, "NSUserNotificationCenter", "NSObject");
}

} // namespace alchemy::gui::support_kit
