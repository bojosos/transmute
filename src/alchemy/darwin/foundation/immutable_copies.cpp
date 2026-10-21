#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <vector>

// NSCopying for Foundation's immutable value classes whose providers lack it
// (NSURL, NSDate, NSLocale, NSNumber, NSTimeZone): as in Foundation, a copy
// of an immutable value is the same object, retained. Each class gets the
// methods only when its instances do not already answer -copyWithZone:.
namespace alchemy::gui::completion_kit {

void install_immutable_copies(Support& support) {
    auto* sp = &support;
    std::vector<Address> samples;
    samples.push_back(support.cf.file_url("/", true));
    samples.push_back(support.cf.date(0));
    samples.push_back(support.cf.integer(0));
    if (const auto locale_class = support.objc.find_class("NSLocale")) samples.push_back(support.send_id(support.send_id(locale_class, "systemLocale"), "retain"));
    if (const auto zone_class = support.objc.find_class("NSTimeZone")) {
        const auto zone = support.send_id(zone_class, "localTimeZone");
        if (zone) samples.push_back(support.send_id(zone, "retain"));
    }
    const auto copy_selector = support.objc.selector("copyWithZone:");
    for (const auto sample : samples) {
        if (!sample) continue;
        const auto cls = support.objc.object_class(sample);
        if (!support.objc.responds(sample, copy_selector)) {
            support.method(cls, "copyWithZone:", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "retain"); }, "@@:^v");
            if (!support.objc.responds(sample, support.objc.selector("copy")))
                support.method(cls, "copy", [sp](CpuState& s, GuestMemory&) { s.x[0] = sp->send_id(s.x[0], "retain"); }, "@@:");
        }
        sp->send_id(sample, "release");
    }
}

} // namespace alchemy::gui::completion_kit
