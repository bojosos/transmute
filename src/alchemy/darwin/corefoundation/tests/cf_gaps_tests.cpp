// CoreFoundation gap tests (cf_sets_notifications.cpp, charset.cpp, and the
// CFUUID functions), split out of the fwgaps tests: CFSet,
// CFCharacterSet/NSCharacterSet, CFNotificationCenter, CFString search and
// file-system representation, CFArray and CFUUID, CFUserNotification.
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <array>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view as = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";

Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }

void sets(Fixture& f) {
    const auto callbacks = f.symbol(cf, "_kCFTypeSetCallBacks");
    const auto values = f.block(24);
    const auto a = string(f, "a"), b = string(f, "b"), a2 = string(f, "a");
    f.engine.write64(values, a); f.engine.write64(values + 8, b); f.engine.write64(values + 16, a2);
    const auto set = f.call(cf, "_CFSetCreate", {0, values, 3, callbacks});
    require(f.call(cf, "_CFSetGetCount", {set}) == 2, "CFSet membership uses CFEqual");
    require(f.call(cf, "_CFGetTypeID", {set}) == f.call(cf, "_CFSetGetTypeID"), "CFSet type id");
    const auto out = f.block(16);
    static_cast<void>(f.call(cf, "_CFSetGetValues", {set, out}));
    require(f.engine.read64(out) && f.engine.read64(out + 8), "CFSetGetValues");
    const auto mutable_set = f.call(cf, "_CFSetCreateMutable", {0, 0, callbacks});
    static_cast<void>(f.call(cf, "_CFSetAddValue", {mutable_set, a}));
    static_cast<void>(f.call(cf, "_CFSetAddValue", {mutable_set, a2}));
    require(f.call(cf, "_CFSetGetCount", {mutable_set}) == 1, "CFSetAddValue");
    bool refused = false;
    try { static_cast<void>(f.call(cf, "_CFSetCreate", {0, values, 3, 0})); } catch (const std::exception&) { refused = true; }
    require(refused, "custom set callbacks stop explicitly");
    require(f.call(cf, "_CFArrayGetCount", {f.symbol(cf, "___NSArray0__struct")}) == 0, "__NSArray0__struct");
}

void character_sets(Fixture& f) {
    const auto whitespace = f.call(cf, "_CFCharacterSetGetPredefined", {2});
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {whitespace, ' '}) == 1, "space is whitespace");
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {whitespace, '\t'}) == 1, "tab is whitespace");
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {whitespace, '\n'}) == 0, "newline is not whitespace");
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {f.call(cf, "_CFCharacterSetGetPredefined", {5}), 0x00e9}) == 1, "letter");
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {f.call(cf, "_CFCharacterSetGetPredefined", {4}), '7'}) == 1, "digit");
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {f.call(cf, "_CFCharacterSetGetPredefined", {12}), 0x1fffe}) == 1, "illegal noncharacter");
    const auto set = f.call(cf, "_CFCharacterSetCreateMutable", {0});
    static_cast<void>(f.call(cf, "_CFCharacterSetAddCharactersInRange", {set, 'a', 3}));
    static_cast<void>(f.call(cf, "_CFCharacterSetAddCharactersInString", {set, string(f, "z\xF0\x9F\x98\x80")}));
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {set, 'c'}) == 1 && f.call(cf, "_CFCharacterSetIsLongCharacterMember", {set, 'd'}) == 0, "ranges");
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {set, 0x1f600}) == 1, "surrogate pair added as one code point");
    static_cast<void>(f.call(cf, "_CFCharacterSetUnion", {set, whitespace}));
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {set, ' '}) == 1, "union");
    const auto bitmap = f.call(cf, "_CFCharacterSetCreateBitmapRepresentation", {0, set});
    require(f.call(cf, "_CFDataGetLength", {bitmap}) == 8192 + 8193, "BMP bitmap plus one plane");
    const auto bytes = f.call(cf, "_CFDataGetBytePtr", {bitmap});
    require((f.memory().read8(bytes + ('a' >> 3)) >> ('a' & 7)) & 1, "bitmap bit for 'a'");
    require(f.memory().read8(bytes + 8192) == 1, "plane byte");
    // NSCharacterSet bridge.
    const auto ns = f.send(f.cls("NSCharacterSet"), "whitespaceAndNewlineCharacterSet");
    require(f.send(ns, "characterIsMember:", {'\n'}) == 1, "NSCharacterSet characterIsMember:");
    require(f.send(f.send(ns, "invertedSet"), "characterIsMember:", {'x'}) == 1, "invertedSet");
}

std::vector<std::pair<Address, Address>> observed;
void notification_centers(Fixture& f) {
    const auto callback = f.process.install_function("__fill_test_observer", [](transmute::CpuState& s, transmute::GuestMemory&) {
        observed.emplace_back(s.x[1], s.x[2]);
    }, "fill.test");
    const auto center = f.call(cf, "_CFNotificationCenterGetLocalCenter");
    require(center == f.send(f.cls("NSNotificationCenter"), "defaultCenter"), "local center is NSNotificationCenter's");
    const auto name = string(f, "AlchemyTestNotification");
    static_cast<void>(f.call(cf, "_CFNotificationCenterAddObserver", {center, 0x1234, callback, name, 0, 4}));
    static_cast<void>(f.send(center, "postNotificationName:object:", {name, 0}));
    require(observed.size() == 1 && observed[0].first == 0x1234 && f.cf.utf8(observed[0].second) == "AlchemyTestNotification", "observer called");
    static_cast<void>(f.call(cf, "_CFNotificationCenterRemoveEveryObserver", {center, 0x1234}));
    static_cast<void>(f.send(center, "postNotificationName:object:", {name, 0}));
    require(observed.size() == 1, "observer removed");
    const auto distributed = f.call(cf, "_CFNotificationCenterGetDistributedCenter");
    require(distributed && distributed != center && distributed == f.call(cf, "_CFNotificationCenterGetDistributedCenter"), "distributed center");
}

void strings(Fixture& f) {
    const auto haystack = string(f, "Hello World, hello world");
    const auto result = f.block(16);
    const auto find = [&](std::string_view needle, Address options) {
        return f.call(cf, "_CFStringFindWithOptions", {haystack, string(f, needle), 0, 24, options, result}) & 0xff;
    };
    require(find("World", 0) && f.engine.read64(result) == 6 && f.engine.read64(result + 8) == 5, "exact search");
    require(find("world", 1) && f.engine.read64(result) == 6, "case-insensitive search");
    require(find("world", 4) && f.engine.read64(result) == 19, "backwards search");
    require(!find("World", 8), "anchored search");
    require(find("hello", 1 | 8) && f.engine.read64(result) == 0, "anchored case-insensitive search");
    require(!find("planet", 0), "not found");
    // File-system representation: decomposed UTF-8.
    const auto e_acute = f.cf.string("\xC3\xA9");
    const auto buffer = f.block(16);
    require(f.call(cf, "_CFStringGetFileSystemRepresentation", {e_acute, buffer, 16}) & 0xff, "file-system representation");
    require(f.read_text(buffer) == "e\xCC\x81", "NFD bytes");
    require(f.call(cf, "_CFStringGetMaximumSizeOfFileSystemRepresentation", {e_acute}) == 4, "maximum size");
    require(!(f.call(cf, "_CFStringGetFileSystemRepresentation", {e_acute, buffer, 3}) & 0xff), "too small buffer");
    // NoCopy with kCFAllocatorNull keeps the caller's bytes.
    const auto bytes = f.text("bytes");
    const auto copy = f.call(cf, "_CFStringCreateWithBytesNoCopy", {0, bytes, 5, 0x08000100, 0, f.engine.read64(f.symbol(cf, "_kCFAllocatorNull"))});
    require(f.cf.utf8(copy) == "bytes", "CFStringCreateWithBytesNoCopy");
    require(f.call(cf, "_CFStringIsHyphenationAvailableForLocale", {0}) == 0, "no hyphenation");
    require(f.string_constant(cf, "_NSCalendarIdentifierGregorian") == "gregorian", "NSCalendarIdentifierGregorian");
}

void arrays_and_uuids(Fixture& f) {
    const auto array = f.call(cf, "_CFArrayCreateMutable", {0, 0, f.symbol(cf, "_kCFTypeArrayCallBacks")});
    for (const auto* text : {"a", "b", "c", "d"}) static_cast<void>(f.call(cf, "_CFArrayAppendValue", {array, string(f, text)}));
    const auto replacement = f.block(8);
    f.engine.write64(replacement, string(f, "X"));
    static_cast<void>(f.call(cf, "_CFArrayReplaceValues", {array, 1, 2, replacement, 1}));
    require(f.call(cf, "_CFArrayGetCount", {array}) == 3 && f.cf.utf8(f.call(cf, "_CFArrayGetValueAtIndex", {array, 1})) == "X", "CFArrayReplaceValues");
    // CFUUIDGetConstantUUIDWithBytes: bytes 7-15 are packed on the stack.
    alchemy::CallRegs registers{};
    for (unsigned i = 1; i < 8; ++i) registers.x[i] = i - 1;
    for (std::uint8_t i = 7; i < 16; ++i) registers.push_stack(i);
    const auto uuid = f.regs(cf, "_CFUUIDGetConstantUUIDWithBytes", registers).x0;
    require(uuid && uuid == f.regs(cf, "_CFUUIDGetConstantUUIDWithBytes", registers).x0, "constant UUID is shared");
    require(f.call(cf, "_CFGetTypeID", {uuid}) == f.call(cf, "_CFUUIDGetTypeID"), "CFUUID type id");
    const auto text = f.cf.utf8(f.call(cf, "_CFUUIDCreateString", {0, uuid}));
    require(text == "00010203-0405-0607-0809-0A0B0C0D0E0F", "UUID bytes: " + text);
}

void user_notifications(Fixture& f) {
    // No user session: Create fails with *error set, a response is Cancel.
    require(f.string_constant(cf, "_kCFUserNotificationAlertHeaderKey") == "AlertHeader", "alert header key");
    require(f.string_constant(cf, "_kCFUserNotificationCheckBoxTitlesKey") == "CheckBoxTitles", "check box titles key");
    const auto dictionary = f.call(cf, "_CFDictionaryCreateMutable",
        {0, 0, f.symbol(cf, "_kCFTypeDictionaryKeyCallBacks"), f.symbol(cf, "_kCFTypeDictionaryValueCallBacks")});
    static_cast<void>(f.call(cf, "_CFDictionarySetValue", {dictionary, f.engine.read64(f.symbol(cf, "_kCFUserNotificationAlertHeaderKey")), string(f, "Scatter")}));
    const auto error = f.block(4);
    alchemy::CallRegs registers{};
    registers.x[2] = error;
    registers.x[3] = dictionary;
    registers.set_d(0, 0);
    require(f.regs(cf, "_CFUserNotificationCreate", registers).x0 == 0 && f.engine.read32(error) != 0, "CFUserNotificationCreate fails");
    const auto response = f.block(8, 0xff);
    require(f.status(cf, "_CFUserNotificationReceiveResponse", {0, response}) == -1 && f.engine.read64(response) == 3, "response is cancel");
}

} // namespace

int main() {
    return run("corefoundation gaps", {{"sets", sets}, {"character sets", character_sets}, {"notification centers", notification_centers}, {"strings", strings}, {"arrays and UUIDs", arrays_and_uuids}, {"user notifications", user_notifications}});
}
