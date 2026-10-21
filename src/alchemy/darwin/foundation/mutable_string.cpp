#include "alchemy/darwin/kit/support/support_kit.hpp"

#include <algorithm>
#include <cstdio>

// NSMutableString over CoreFoundation's mutable strings (CoreFoundation::
// mutable_copy / replace_string_units). Every string object is of the
// bridged NSString class, so the mutation methods are added to NSString and
// work on mutable strings only (an immutable receiver fails explicitly with
// "mutation of an immutable string", as an NSInvalidArgumentException would).
// NSMutableString's own constructors (+string, +stringWithCapacity:,
// +stringWithString:, -initWith...) answer mutable CF strings. Each method is
// added only when the class does not answer it yet. Qt uses this for menu and
// window titles (mnemonic stripping) and accessibility text.
namespace alchemy::gui::support_kit {
namespace {

using Units = std::vector<std::uint16_t>;

bool instances_respond(Support& support, Address cls, std::string_view selector) {
    const auto probe = support.objc.allocate(cls);
    const bool answer = support.objc.responds(probe, support.objc.selector(selector));
    support.process.objects().release(probe);
    return answer;
}

Units units_of(Support& support, Address string) { return string ? support.cf.units(string) : Units{}; }

// The caller (module+offset) of a native method: x30 on entry.
std::string caller(Support& support, std::uint64_t lr) {
    for (const auto& image : support.process.images())
        if (lr >= image.begin && lr < image.end) {
            auto name = image.name;
            if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) name = name.substr(slash + 1);
            char text[256];
            std::snprintf(text, sizeof text, "%s+0x%llx", name.c_str(), static_cast<unsigned long long>(lr - image.begin));
            return text;
        }
    return "unknown caller";
}

void require_mutable(Support& support, Address self, std::string_view method, std::uint64_t lr) {
    if (support.cf.is_mutable(self)) return;
    fail(std::string("-[NSString ") + std::string(method) + "] sent to an immutable string (from " + caller(support, lr) + ")");
}

void check_range(const Units& units, std::uint64_t location, std::uint64_t length, std::string_view method) {
    if (location > units.size() || length > units.size() - location)
        fail(std::string("-[NSMutableString ") + std::string(method) + "]: range out of bounds");
}

} // namespace

void install_mutable_string(Support& support) {
    auto* s = &support;
    const auto string_class = support.objc.find_class("NSString");
    if (!string_class) return;
    const auto add = [&](Address cls, const char* name, Handler handler, const char* types, bool class_method = false) {
        if (class_method ? support.objc.responds(cls, support.objc.selector(name)) : instances_respond(support, cls, name)) return;
        support.method(cls, name, std::move(handler), types, class_method);
    };
    const auto replace = [s](Address self, std::uint64_t location, std::uint64_t length, const Units& with, std::string_view method,
                             std::uint64_t lr) {
        require_mutable(*s, self, method, lr);
        auto units = units_of(*s, self);
        check_range(units, location, length, method);
        units.erase(units.begin() + static_cast<std::ptrdiff_t>(location), units.begin() + static_cast<std::ptrdiff_t>(location + length));
        units.insert(units.begin() + static_cast<std::ptrdiff_t>(location), with.begin(), with.end());
        s->cf.replace_string_units(self, std::move(units));
    };
    // replaceCharactersInRange:withString: (NSRange in x2/x3, string in x4)
    add(string_class, "replaceCharactersInRange:withString:", [s, replace](CpuState& c, GuestMemory&) {
        replace(c.x[0], c.x[2], c.x[3], units_of(*s, c.x[4]), "replaceCharactersInRange:withString:", c.x[30]);
    }, "v@:{_NSRange=QQ}@");
    add(string_class, "insertString:atIndex:", [s, replace](CpuState& c, GuestMemory&) {
        replace(c.x[0], c.x[3], 0, units_of(*s, c.x[2]), "insertString:atIndex:", c.x[30]);
    }, "v@:@Q");
    add(string_class, "deleteCharactersInRange:", [replace](CpuState& c, GuestMemory&) {
        replace(c.x[0], c.x[2], c.x[3], {}, "deleteCharactersInRange:", c.x[30]);
    }, "v@:{_NSRange=QQ}");
    add(string_class, "appendString:", [s](CpuState& c, GuestMemory&) {
        require_mutable(*s, c.x[0], "appendString:", c.x[30]);
        auto units = units_of(*s, c.x[0]);
        const auto more = units_of(*s, c.x[2]);
        units.insert(units.end(), more.begin(), more.end());
        s->cf.replace_string_units(c.x[0], std::move(units));
    }, "v@:@");
    add(string_class, "setString:", [s](CpuState& c, GuestMemory&) {
        require_mutable(*s, c.x[0], "setString:", c.x[30]);
        s->cf.replace_string_units(c.x[0], units_of(*s, c.x[2]));
    }, "v@:@");
    // replaceOccurrencesOfString:withString:options:range: (literal search;
    // NSCaseInsensitiveSearch = 1 and NSBackwardsSearch = 4 are not provided).
    add(string_class, "replaceOccurrencesOfString:withString:options:range:", [s](CpuState& c, GuestMemory&) {
        if (c.x[4] & ~std::uint64_t{2 /* NSLiteralSearch */})
            unsupported("-[NSMutableString replaceOccurrencesOfString:withString:options:range:] options other than NSLiteralSearch");
        auto units = units_of(*s, c.x[0]);
        const auto target = units_of(*s, c.x[2]), with = units_of(*s, c.x[3]);
        const auto location = c.x[5], length = c.x[6];
        check_range(units, location, length, "replaceOccurrencesOfString:withString:options:range:");
        if (target.empty()) fail("-[NSMutableString replaceOccurrencesOfString:...]: empty search string");
        Units out(units.begin(), units.begin() + static_cast<std::ptrdiff_t>(location));
        std::uint64_t count = 0;
        std::size_t at = location;
        const auto end = location + length;
        while (at < end) {
            if (at + target.size() <= end && std::equal(target.begin(), target.end(), units.begin() + static_cast<std::ptrdiff_t>(at))) {
                out.insert(out.end(), with.begin(), with.end());
                at += target.size();
                ++count;
            } else {
                out.push_back(units[at++]);
            }
        }
        out.insert(out.end(), units.begin() + static_cast<std::ptrdiff_t>(end), units.end());
        if (count) s->cf.replace_string_units(c.x[0], std::move(out));
        c.x[0] = count;
    }, "Q@:@@Q{_NSRange=QQ}");

    // -[NSString mutableCopy] is Foundation's (foundation_extra.cpp): a mutable copy.

    // NSMutableString constructors: mutable CF strings.
    const auto mutable_class = support.objc.find_class("NSMutableString");
    if (!mutable_class) return;
    const auto make = [s](Address from) {
        const auto empty = from ? 0 : s->cf.string("");
        const auto value = s->cf.mutable_copy(from ? from : empty);
        if (empty) s->release(empty);
        return value;
    };
    // NSMutableString inherits NSString's factories, which answer immutable
    // strings: each is overridden on NSMutableString to call NSString's and
    // answer a mutable copy (variadic arguments are forwarded with the stack).
    support.method(mutable_class, "stringWithCapacity:", [s, make](CpuState& c, GuestMemory&) {
        c.x[0] = s->send(make(0), "autorelease").x0;
    }, "@@:Q", true);
    for (const auto* name : {"string", "stringWithString:", "stringWithFormat:", "stringWithCharacters:length:",
                             "stringWithUTF8String:", "stringWithCString:encoding:", "stringWithContentsOfFile:encoding:error:"}) {
        const auto selector = support.objc.selector(name);
        if (!support.objc.responds(string_class, selector)) continue;
        const auto original = support.objc.lookup_imp(string_class, selector);
        support.method(mutable_class, name, [s, original, string_class](CpuState& c, GuestMemory& m) {
            CallRegs forward;
            for (unsigned index = 0; index < 8; ++index) { forward.x[index] = c.x[index]; forward.v[index] = c.v[index]; }
            forward.x[0] = string_class;
            forward.stack.resize(256);
            m.read_bytes(c.sp, forward.stack);
            const auto immutable = EngineAdapter(s->process.calling_engine()).call_regs(original, forward).x0;
            c.x[0] = immutable ? s->send(s->cf.mutable_copy(immutable), "autorelease").x0 : 0;
        }, "@@:", true);
    }
    // -[NSMutableString init...] on an +alloc'ed placeholder: the placeholder
    // is released and a mutable CF string (+1) is returned instead.
    for (const auto* name : {"init", "initWithCapacity:"})
        support.method(mutable_class, name, [s, make](CpuState& c, GuestMemory&) {
            s->release(c.x[0]);
            c.x[0] = make(0);
        }, "@@:");
    support.method(mutable_class, "initWithString:", [s, make](CpuState& c, GuestMemory&) {
        const auto from = c.x[2];
        s->release(c.x[0]);
        c.x[0] = make(from);
    }, "@@:@");
}

} // namespace alchemy::gui::support_kit
