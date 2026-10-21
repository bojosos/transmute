// Foundation's value classes (value_classes.cpp: NSTimeZone, NSUUID, ...) as
// a renderer plug-in's images call them. Split out of the export-fill tests.
#include "alchemy/darwin/kit/export_fill/tests/export_fill_test_kit.hpp"

namespace {

void test_guest_foundation_classes(Fixture& f) {
    const auto send = [&](std::uint64_t receiver, std::string_view selector, alchemy::CallRegs registers = {}) {
        return f.objc.send(receiver, selector, registers);
    };
    const auto args = [](std::initializer_list<std::uint64_t> values) {
        alchemy::CallRegs registers;
        std::size_t index = 2;
        for (const auto value : values) registers.x[index++] = value;
        return registers;
    };
    // NSTimeZone over the host tzdb.
    const auto zones = f.objc.find_class("NSTimeZone");
    require(zones != 0, "NSTimeZone class");
    const auto sofia = send(zones, "timeZoneWithName:", args({f.cf.string("Europe/Sofia")})).x0;
    require(sofia != 0 && f.cf.utf8(send(sofia, "name").x0) == "Europe/Sofia", "timeZoneWithName:");
    const auto winter = f.cf.date(0.0), summer = f.cf.date(15638400.0);   // 2001-01-01, 2001-07-01
    require(send(sofia, "secondsFromGMTForDate:", args({winter})).x0 == 7200 && send(sofia, "secondsFromGMTForDate:", args({summer})).x0 == 10800,
            "secondsFromGMTForDate:");
    require(send(sofia, "isDaylightSavingTimeForDate:", args({summer})).x0 == 1, "isDaylightSavingTimeForDate:");
    const auto transition = send(sofia, "nextDaylightSavingTimeTransitionAfterDate:", args({winter})).x0;
    // 2001-03-25 01:00 UTC
    require(transition && f.cf.date_time(transition) == 7174800.0, "next transition: " + std::to_string(transition ? f.cf.date_time(transition) : -1));
    require(send(zones, "timeZoneWithName:", args({f.cf.string("Not/AZone")})).x0 == 0, "unknown zone is nil");
    const auto fixed = send(zones, "timeZoneForSecondsFromGMT:", args({3600})).x0;
    require(f.cf.utf8(send(fixed, "name").x0) == "GMT+0100" && send(fixed, "secondsFromGMT").x0 == 3600, "fixed offset zone");
    const auto names = send(zones, "knownTimeZoneNames").x0;
    require(f.cf.length(names) > 300, "knownTimeZoneNames");
    const auto cf_zone = f.call(alchemy::core_foundation_library, "_CFTimeZoneCopyDefault");
    require(!f.cf.utf8(send(cf_zone, "name").x0).empty(), "CFTimeZone is an NSTimeZone");
    // NSLocale's lists (Chrome asks for +ISOLanguageCodes); CFLocale's, from the host's locales.
    const auto locales = f.objc.find_class("NSLocale");
    const auto has = [&f](std::uint64_t array, std::string_view value) {
        for (std::uint64_t index = 0; array && index < f.cf.length(array); ++index) if (f.cf.utf8(f.cf.element(array, index)) == value) return true;
        return false;
    };
    require(has(send(locales, "ISOLanguageCodes").x0, "en") && has(send(locales, "ISOLanguageCodes").x0, "de"), "+[NSLocale ISOLanguageCodes]");
    require(has(send(locales, "ISOCountryCodes").x0, "DE"), "+[NSLocale ISOCountryCodes]");
    require(has(send(locales, "ISOCurrencyCodes").x0, "EUR") && has(send(locales, "commonISOCurrencyCodes").x0, "USD"), "+[NSLocale ISOCurrencyCodes]");
    require(has(send(locales, "availableLocaleIdentifiers").x0, "de_DE"), "+[NSLocale availableLocaleIdentifiers]");
    // NSUUID.
    const auto uuids = f.objc.find_class("NSUUID");
    const auto parsed = send(f.objc.allocate(uuids), "initWithUUIDString:", args({f.cf.string("00010203-0405-0607-0809-0a0b0c0d0e0f")})).x0;
    require(parsed && f.cf.utf8(send(parsed, "UUIDString").x0) == "00010203-0405-0607-0809-0A0B0C0D0E0F", "NSUUID string round trip");
    const auto bytes = f.process.allocate_guest(16);
    send(parsed, "getUUIDBytes:", args({bytes}));
    require(f.engine.read64(bytes) == 0x0706050403020100ULL, "getUUIDBytes:");
    require(send(f.objc.allocate(uuids), "initWithUUIDString:", args({f.cf.string("nope")})).x0 == 0, "invalid UUID string is nil");
    require(f.cf.utf8(send(send(uuids, "UUID").x0, "UUIDString").x0).size() == 36, "+UUID");
    // NSValue.
    const auto values = f.objc.find_class("NSValue");
    const auto range = send(values, "valueWithRange:", args({3, 9})).x0;
    const auto range_back = send(range, "rangeValue");
    require(!range_back.complete || (range_back.x0 == 3 && range_back.x1 == 9), "NSRange value");
    alchemy::CallRegs rect;
    for (unsigned i = 0; i < 4; ++i) rect.set_d(i, 1.5 * (i + 1));
    const auto boxed = send(values, "valueWithRect:", rect).x0;
    const auto rect_back = send(boxed, "rectValue");
    require(!rect_back.complete || (rect_back.d(0) == 1.5 && rect_back.d(3) == 6.0), "NSRect value");
    const auto integer = f.process.allocate_guest(4);
    f.engine.write32(integer, 77);
    const auto boxed_int = send(values, "valueWithBytes:objCType:", args({integer, f.text("i")})).x0;
    require(f.read(send(boxed_int, "objCType").x0) == "i", "objCType");
    const auto out = f.process.allocate_guest(4);
    send(boxed_int, "getValue:", args({out}));
    require(f.engine.read32(out) == 77 && send(boxed_int, "isEqualToValue:", args({boxed_int})).x0 == 1, "getValue:");
}

} // namespace

int main() {
    try {
        const auto stage = [](std::string_view name) { std::cout << "[stage] " << name << std::endl; };
        stage("fixture"); Fixture f;
        stage("guest foundation classes"); test_guest_foundation_classes(f);
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "foundation value classes tests passed\n";
    return 0;
}
