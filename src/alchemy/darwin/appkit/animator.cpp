#include "alchemy/darwin/kit/completion/completion_kit.hpp"

// NSAnimatablePropertyContainer for NSView and NSWindow (QLab fades and
// moves views through -animator), added when missing. There is no animation
// engine (as fwgaps' NSAnimationContext states, changes apply at once), so
// the animator proxy is the receiver itself: a property set through it takes
// its final value immediately, which is where an animation would end.
// -animations is an empty dictionary until set; +defaultAnimationForKey: is nil.
namespace alchemy::gui::completion_kit {
void install_animator(Support& support) {
    auto* sp = &support;
    for (const auto* name : {"NSView", "NSWindow"}) {
        const auto cls = support.objc.find_class(name);
        if (!cls) continue;
        const auto has = [sp, cls](const char* selector) {
            return sp->call("/usr/lib/libobjc.A.dylib", "_class_getInstanceMethod", {cls, sp->objc.selector(selector)}) != 0;
        };
        if (!has("animator")) support.method(cls, "animator", [](CpuState&, GuestMemory&) {}, "@@:");   // returns self (x0)
        if (!has("animations")) {
            // Stored as an associated object (copied, released with the object).
            static const Address key = sp->process.allocate_guest(8);
            support.method(cls, "animations", [sp](CpuState& s, GuestMemory&) {
                const auto value = sp->call("/usr/lib/libobjc.A.dylib", "_objc_getAssociatedObject", {s.x[0], key});
                s.x[0] = value ? value : sp->send_id(sp->cf.dictionary({}), "autorelease");
            }, "@@:");
            support.method(cls, "setAnimations:", [sp](CpuState& s, GuestMemory&) {
                static_cast<void>(sp->call("/usr/lib/libobjc.A.dylib", "_objc_setAssociatedObject", {s.x[0], key, s.x[2], 3}));   // COPY_NONATOMIC
            }, "v@:@");
        }
        if (!support.objc.responds(cls, support.objc.selector("defaultAnimationForKey:")))
            support.method(cls, "defaultAnimationForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@", true);
    }
}

} // namespace alchemy::gui::completion_kit
