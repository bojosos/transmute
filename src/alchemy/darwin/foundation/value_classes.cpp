#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/host/locale.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/object_store.hpp"

#include <array>
#include <charconv>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <random>
#include <string>
#include <vector>

// Foundation value classes a renderer plug-in's Qt and tools import:
//   NSTimeZone   over the host IANA time-zone database (std::chrono tzdb):
//                system/default/named/fixed-offset zones, offsets, daylight
//                saving, abbreviations, next transition, known names, the
//                abbreviation dictionary (CF-Lite's defaults).
//                CFTimeZone objects are instances of the private subclass
//                __NSTimeZone, as on Darwin, and the CFTimeZone calls
//                (create by name, abbreviation or offset, offsets, daylight
//                saving, transitions, names, defaults) send NSTimeZone's
//                methods. CFTimeZoneGetData and TZif data stop explicitly.
//   NSUUID       random and parsed UUIDs, bytes and strings.
//   NSValue      boxed bytes with an Objective-C type encoding, pointers,
//                non-retained objects, NSRange/NSPoint/NSSize/NSRect.
namespace alchemy::export_fill {

std::string default_time_zone() { return host::locale::time_zone().value_or("GMT"); }

std::uint64_t time_zone_class(ObjcRuntime& objc) {
    if (const auto found = objc.find_class("__NSTimeZone")) return found;
    auto base = objc.find_class("NSTimeZone");
    if (!base) base = objc.register_native_class("NSTimeZone", objc.find_class("NSObject"), 16);
    return objc.register_native_class("__NSTimeZone", base, 16);
}

std::uint64_t make_time_zone(ProcessContext& process, CoreFoundation& cf, ObjcRuntime& objc, const std::string& name) {
    auto payload = std::make_shared<CfTimeZone>();
    payload->name = cf.string(name);
    payload->identifier = name;
    const auto string = payload->name;
    return process.objects().create(16, time_zone_class(objc), ObjectKind::instance, std::move(payload),
                                    [&process, string](std::uint64_t) { process.objects().release(string); });
}

namespace {

using namespace std::chrono;

constexpr double unix_to_absolute = 978307200.0;

// A zone: an IANA database entry, or a fixed offset ("GMT+0200").
struct Zone {
    const time_zone* zone{};
    std::int64_t fixed{};
};

std::optional<Zone> resolve_zone(const std::string& name) {
    if (name == "GMT" || name == "UTC") return Zone{nullptr, 0};
    if ((name.starts_with("GMT") || name.starts_with("UTC")) && name.size() >= 5) {
        int hours{}, minutes{};
        const char sign = name[3];
        if ((sign == '+' || sign == '-') && std::sscanf(name.c_str() + 4, "%2d%2d", &hours, &minutes) >= 1) {
            const auto offset = hours * 3600 + minutes * 60;
            return Zone{nullptr, sign == '-' ? -offset : offset};
        }
    }
    try { return Zone{locate_zone(name), 0}; }
    catch (const std::exception&) { return std::nullopt; }
}

sys_seconds instant(double absolute) { return sys_seconds{seconds{static_cast<std::int64_t>(std::floor(absolute + unix_to_absolute))}}; }

sys_info zone_info(const Zone& zone, double absolute) {
    if (zone.zone) return zone.zone->get_info(instant(absolute));
    sys_info value{};
    value.begin = sys_seconds::min();
    value.end = sys_seconds::max();
    value.offset = seconds{zone.fixed};
    value.save = minutes{0};
    char text[16];
    std::snprintf(text, sizeof(text), "GMT%c%02lld%02lld", zone.fixed < 0 ? '-' : '+', static_cast<long long>(std::llabs(zone.fixed) / 3600),
                  static_cast<long long>(std::llabs(zone.fixed) / 60 % 60));
    value.abbrev = zone.fixed ? text : "GMT";
    return value;
}

} // namespace

void install_foundation_classes(Provider& p, ObjcRuntime& objc, CoreFoundation& cf) {
    auto& process = p.process();
    const auto autorelease = [p](std::uint64_t object) { return object ? p.call(objc_library, "_objc_autorelease", {object}) : 0; };
    const auto alias = [&process](std::uint64_t cls, const std::string& name) {
        for (const auto* library : {core_foundation_library, foundation_library}) {
            if (!process.resolve(library, "_OBJC_CLASS_$_" + name))
                process.install_export(library, "_OBJC_CLASS_$_" + name, {cls, ExportKind::data});
            if (!process.resolve(library, "_OBJC_METACLASS_$_" + name))
                process.install_export(library, "_OBJC_METACLASS_$_" + name, {process.main_engine().read64(cls), ExportKind::data});
        }
    };
    const auto add = [&objc](std::uint64_t cls, const char* name, transmute::GuestCallHandler handler, bool class_method = false,
                             const char* types = "@@:") {
        static_cast<void>(objc.add_native_method(cls, name, std::move(handler), class_method, types));
    };
    const auto set_double = [](CpuState& s, double value) { s.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; };

    // ---- NSTimeZone ----
    static_cast<void>(time_zone_class(objc));
    const auto zone_class = objc.find_class("NSTimeZone");
    alias(zone_class, "NSTimeZone");
    const auto zone_of = [&process](std::uint64_t object) {
        const auto record = object ? process.objects().find(object) : nullptr;
        const auto payload = record ? std::dynamic_pointer_cast<CfTimeZone>(record->payload) : nullptr;
        if (!payload) throw std::runtime_error("Alchemy NSTimeZone: object is not a time zone");
        const auto zone = resolve_zone(payload->identifier);
        if (!zone) throw std::runtime_error("Alchemy NSTimeZone: unknown zone " + payload->identifier);
        return std::pair{payload, *zone};
    };
    const auto date_time = [&cf](std::uint64_t date) { return date ? cf.date_time(date) : CoreFoundation::absolute_time_now(); };
    // The default zone may be replaced with +setDefaultTimeZone:.
    auto default_zone = std::make_shared<std::pair<std::mutex, std::uint64_t>>();
    const auto system_zone = [&process, &cf, &objc]() { return make_time_zone(process, cf, objc, default_time_zone()); };
    for (const auto* name : {"systemTimeZone", "localTimeZone"})
        add(zone_class, name, [system_zone, autorelease](CpuState& s, GuestMemory&) { s.x[0] = autorelease(system_zone()); }, true);
    add(zone_class, "defaultTimeZone", [&process, default_zone, system_zone, autorelease](CpuState& s, GuestMemory&) {
        std::lock_guard lock(default_zone->first);
        s.x[0] = default_zone->second ? autorelease(process.objects().retain(default_zone->second)) : autorelease(system_zone());
    }, true);
    add(zone_class, "setDefaultTimeZone:", [&process, default_zone](CpuState& s, GuestMemory&) {
        std::lock_guard lock(default_zone->first);
        if (default_zone->second) process.objects().release(default_zone->second);
        default_zone->second = s.x[2] ? process.objects().retain(s.x[2]) : 0;
        s.x[0] = 0;
    }, true, "v@:@");
    add(zone_class, "resetSystemTimeZone", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "v@:");
    add(zone_class, "timeZoneWithName:", [&process, &cf, &objc, autorelease](CpuState& s, GuestMemory&) {
        const auto name = s.x[2] ? cf.utf8(s.x[2]) : std::string{};
        s.x[0] = !name.empty() && resolve_zone(name) ? autorelease(make_time_zone(process, cf, objc, name)) : 0;
    }, true, "@@:@");
    add(zone_class, "timeZoneForSecondsFromGMT:", [&process, &cf, &objc, autorelease](CpuState& s, GuestMemory&) {
        const auto offset = static_cast<std::int64_t>(s.x[2]);
        if (offset < -18 * 3600 || offset > 18 * 3600) { s.x[0] = 0; return; }
        char name[16];
        std::snprintf(name, sizeof(name), "GMT%c%02lld%02lld", offset < 0 ? '-' : '+', static_cast<long long>(std::llabs(offset) / 3600),
                      static_cast<long long>(std::llabs(offset) / 60 % 60));
        s.x[0] = autorelease(make_time_zone(process, cf, objc, offset ? name : "GMT"));
    }, true, "@@:q");
    add(zone_class, "knownTimeZoneNames", [&process, &cf, autorelease](CpuState& s, GuestMemory&) {
        std::vector<std::uint64_t> names;
        for (const auto& zone : get_tzdb().zones) names.push_back(cf.string(zone.name()));
        const auto array = cf.array(names);
        for (const auto name : names) process.objects().release(name);
        s.x[0] = autorelease(array);
    }, true);
    add(zone_class, "name", [zone_of](CpuState& s, GuestMemory&) { s.x[0] = zone_of(s.x[0]).first->name; });
    add(zone_class, "description", [zone_of](CpuState& s, GuestMemory&) { s.x[0] = zone_of(s.x[0]).first->name; });
    add(zone_class, "secondsFromGMT", [zone_of](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(zone_info(zone_of(s.x[0]).second, CoreFoundation::absolute_time_now()).offset.count());
    }, false, "q@:");
    add(zone_class, "secondsFromGMTForDate:", [zone_of, date_time](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(zone_info(zone_of(s.x[0]).second, date_time(s.x[2])).offset.count());
    }, false, "q@:@");
    add(zone_class, "abbreviation", [&cf, zone_of, autorelease](CpuState& s, GuestMemory&) {
        s.x[0] = autorelease(cf.string(zone_info(zone_of(s.x[0]).second, CoreFoundation::absolute_time_now()).abbrev));
    });
    add(zone_class, "abbreviationForDate:", [&cf, zone_of, date_time, autorelease](CpuState& s, GuestMemory&) {
        s.x[0] = autorelease(cf.string(zone_info(zone_of(s.x[0]).second, date_time(s.x[2])).abbrev));
    }, false, "@@:@");
    add(zone_class, "isDaylightSavingTime", [zone_of](CpuState& s, GuestMemory&) {
        s.x[0] = zone_info(zone_of(s.x[0]).second, CoreFoundation::absolute_time_now()).save.count() != 0 ? 1 : 0;
    }, false, "B@:");
    add(zone_class, "isDaylightSavingTimeForDate:", [zone_of, date_time](CpuState& s, GuestMemory&) {
        s.x[0] = zone_info(zone_of(s.x[0]).second, date_time(s.x[2])).save.count() != 0 ? 1 : 0;
    }, false, "B@:@");
    add(zone_class, "daylightSavingTimeOffset", [zone_of, set_double](CpuState& s, GuestMemory&) {
        set_double(s, static_cast<double>(duration_cast<seconds>(zone_info(zone_of(s.x[0]).second, CoreFoundation::absolute_time_now()).save).count()));
    }, false, "d@:");
    add(zone_class, "daylightSavingTimeOffsetForDate:", [zone_of, date_time, set_double](CpuState& s, GuestMemory&) {
        set_double(s, static_cast<double>(duration_cast<seconds>(zone_info(zone_of(s.x[0]).second, date_time(s.x[2])).save).count()));
    }, false, "d@:@");
    // The next offset or abbreviation change after the date (nil when none).
    add(zone_class, "nextDaylightSavingTimeTransitionAfterDate:", [&cf, zone_of, date_time, autorelease](CpuState& s, GuestMemory&) {
        const auto zone = zone_of(s.x[0]).second;
        if (!zone.zone) { s.x[0] = 0; return; }
        auto at = date_time(s.x[2]);
        for (int step = 0; step < 64; ++step) {
            const auto current = zone.zone->get_info(instant(at));
            if (current.end == sys_seconds::max()) { s.x[0] = 0; return; }
            const auto next = static_cast<double>(current.end.time_since_epoch().count()) - unix_to_absolute;
            const auto following = zone.zone->get_info(current.end);
            if (following.save != current.save || following.offset != current.offset) { s.x[0] = autorelease(cf.date(next)); return; }
            at = next;
        }
        s.x[0] = 0;
    }, false, "@@:@");
    add(zone_class, "nextDaylightSavingTimeTransition", [&objc](CpuState& s, GuestMemory&) {
        CallRegs registers;
        registers.x[2] = 0;
        s.x[0] = objc.send(s.x[0], "nextDaylightSavingTimeTransitionAfterDate:", registers).x0;
    });
    // -localizedName:locale: (NSTimeZoneNameStyle): the abbreviation for the
    // short styles, otherwise the identifier (no localized zone names).
    add(zone_class, "localizedName:locale:", [&cf, zone_of, autorelease](CpuState& s, GuestMemory&) {
        const auto [payload, zone] = zone_of(s.x[0]);
        const auto style = s.x[2];
        const auto now = zone_info(zone, CoreFoundation::absolute_time_now());
        s.x[0] = style == 1 || style == 3 || style == 5 ? autorelease(cf.string(now.abbrev)) : autorelease(cf.string(payload->identifier));
    }, false, "@@:q@");
    add(zone_class, "isEqualToTimeZone:", [zone_of](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[2] && zone_of(s.x[0]).first->identifier == zone_of(s.x[2]).first->identifier ? 1 : 0;
    }, false, "B@:@");
    add(zone_class, "isEqual:", [&process, zone_of](CpuState& s, GuestMemory&) {
        const auto record = s.x[2] ? process.objects().find(s.x[2]) : nullptr;
        const bool zone = record && std::dynamic_pointer_cast<CfTimeZone>(record->payload);
        s.x[0] = zone && zone_of(s.x[0]).first->identifier == zone_of(s.x[2]).first->identifier ? 1 : 0;
    }, false, "B@:@");
    add(zone_class, "hash", [zone_of](CpuState& s, GuestMemory&) {
        s.x[0] = std::hash<std::string>{}(zone_of(s.x[0]).first->identifier);
    }, false, "Q@:");
    add(zone_class, "copyWithZone:", [&process](CpuState& s, GuestMemory&) { s.x[0] = process.objects().retain(s.x[0]); });

    // ---- abbreviations ----
    // The default abbreviation dictionary is CF-Lite's (swift-corelibs-foundation CFTimeZone.c,
    // Apache-2.0, third_party/swift-corelibs-foundation); +setAbbreviationDictionary: and
    // CFTimeZoneSetAbbreviationDictionary replace it for the process.
    static constexpr std::pair<const char*, const char*> default_abbreviations[] = {
        {"ADT", "America/Halifax"}, {"AKDT", "America/Juneau"}, {"AKST", "America/Juneau"}, {"ART", "America/Argentina/Buenos_Aires"},
        {"AST", "America/Halifax"}, {"BDT", "Asia/Dhaka"}, {"BRST", "America/Sao_Paulo"}, {"BRT", "America/Sao_Paulo"},
        {"BST", "Europe/London"}, {"CAT", "Africa/Harare"}, {"CDT", "America/Chicago"}, {"CEST", "Europe/Paris"},
        {"CET", "Europe/Paris"}, {"CLST", "America/Santiago"}, {"CLT", "America/Santiago"}, {"COT", "America/Bogota"},
        {"CST", "America/Chicago"}, {"EAT", "Africa/Addis_Ababa"}, {"EDT", "America/New_York"}, {"EEST", "Europe/Istanbul"},
        {"EET", "Europe/Istanbul"}, {"EST", "America/New_York"}, {"GMT", "GMT"}, {"GST", "Asia/Dubai"},
        {"HKT", "Asia/Hong_Kong"}, {"HST", "Pacific/Honolulu"}, {"ICT", "Asia/Bangkok"}, {"IRST", "Asia/Tehran"},
        {"IST", "Asia/Calcutta"}, {"JST", "Asia/Tokyo"}, {"KST", "Asia/Seoul"}, {"MDT", "America/Denver"},
        {"MSD", "Europe/Moscow"}, {"MSK", "Europe/Moscow"}, {"MST", "America/Denver"}, {"NZDT", "Pacific/Auckland"},
        {"NZST", "Pacific/Auckland"}, {"PDT", "America/Los_Angeles"}, {"PET", "America/Lima"}, {"PHT", "Asia/Manila"},
        {"PKT", "Asia/Karachi"}, {"PST", "America/Los_Angeles"}, {"SGT", "Asia/Singapore"}, {"UTC", "UTC"},
        {"WAT", "Africa/Lagos"}, {"WEST", "Europe/Lisbon"}, {"WET", "Europe/Lisbon"}, {"WIT", "Asia/Jakarta"},
    };
    auto abbreviations = std::make_shared<std::pair<std::mutex, std::uint64_t>>();   // the dictionary (+1), made on first use
    const auto abbreviation_dictionary = [&process, &cf, abbreviations]() {   // +1
        std::lock_guard lock(abbreviations->first);
        if (!abbreviations->second) {
            std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
            for (const auto& [key, value] : default_abbreviations) entries.emplace_back(cf.string(key), cf.string(value));
            abbreviations->second = cf.dictionary(entries);
            for (const auto& [key, value] : entries) { process.objects().release(key); process.objects().release(value); }
        }
        return process.objects().retain(abbreviations->second);
    };
    const auto set_abbreviation_dictionary = [&process, abbreviations](std::uint64_t dictionary) {
        if (!dictionary) return;
        std::lock_guard lock(abbreviations->first);
        if (abbreviations->second) process.objects().release(abbreviations->second);
        abbreviations->second = process.objects().retain(dictionary);
    };
    add(zone_class, "abbreviationDictionary", [abbreviation_dictionary, autorelease, zone_class](CpuState& s, GuestMemory&) {
        s.x[0] = autorelease(abbreviation_dictionary());
    }, true);
    add(zone_class, "setAbbreviationDictionary:", [set_abbreviation_dictionary](CpuState& s, GuestMemory&) { set_abbreviation_dictionary(s.x[2]); },
        true, "v@:@");
    // The zone an abbreviation names (nil when the dictionary has none).
    const auto zone_for_abbreviation = [&process, &cf, &objc, abbreviation_dictionary](std::uint64_t abbreviation) -> std::uint64_t {   // +1
        if (!abbreviation) return 0;
        const auto dictionary = abbreviation_dictionary();
        std::uint64_t name{};
        for (const auto& [key, value] : cf.dictionary_entries(dictionary))
            if (cf.utf8(key) == cf.utf8(abbreviation)) name = value;
        const auto identifier = name ? cf.utf8(name) : std::string{};
        process.objects().release(dictionary);
        return !identifier.empty() && resolve_zone(identifier) ? make_time_zone(process, cf, objc, identifier) : 0;
    };

    // ---- CFTimeZone: a CFTimeZoneRef is an NSTimeZone (toll-free bridged) ----
    const auto time_real = [](const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); };
    const auto send = [&objc](std::uint64_t receiver, const char* selector, std::initializer_list<std::uint64_t> arguments = {}) {
        CallRegs registers;
        std::size_t index = 2;
        for (const auto value : arguments) registers.x[index++] = value;
        return objc.send(receiver, selector, registers);
    };
    const auto with_date = [&process, &cf](double at, auto&& use) {
        const auto date = cf.date(at);
        const auto result = use(date);
        process.objects().release(date);
        return result;
    };
    const auto named = [&process, &cf, &objc, zone_for_abbreviation](std::uint64_t name, bool try_abbreviation) -> std::uint64_t {   // +1
        // With tryAbbrev the abbreviation dictionary comes first, as in CF (CFTimeZone.c).
        if (try_abbreviation)
            if (const auto zone = zone_for_abbreviation(name)) return zone;
        const auto text = name ? cf.utf8(name) : std::string{};
        return !text.empty() && resolve_zone(text) ? make_time_zone(process, cf, objc, text) : 0;
    };
    const auto no_allocator = [](std::uint64_t allocator) {
        if (allocator) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
    };
    // CFTimeZoneRef CFTimeZoneCreateWithName(CFAllocatorRef, CFStringRef name, Boolean tryAbbrev)
    p.function(core_foundation_library, "_CFTimeZoneCreateWithName", [named, no_allocator](CpuState& s, GuestMemory&) -> std::uint64_t {
        no_allocator(s.x[0]);
        return named(s.x[1], (s.x[2] & 0xff) != 0);
    });
    // CFTimeZoneRef CFTimeZoneCreate(CFAllocatorRef, CFStringRef name, CFDataRef data): TZif data is not read.
    p.function(core_foundation_library, "_CFTimeZoneCreate", [named, no_allocator](CpuState& s, GuestMemory&) -> std::uint64_t {
        no_allocator(s.x[0]);
        if (s.x[2]) throw std::runtime_error("unsupported: CFTimeZoneCreate with TZif data (zones come from the host time-zone database by name)");
        return named(s.x[1], false);
    });
    // CFTimeZoneRef CFTimeZoneCreateWithTimeIntervalFromGMT(CFAllocatorRef, CFTimeInterval ti): NULL beyond Â±18 hours.
    p.function(core_foundation_library, "_CFTimeZoneCreateWithTimeIntervalFromGMT", [&process, send, time_real, no_allocator, zone_class](CpuState& s, GuestMemory&) -> std::uint64_t {
        no_allocator(s.x[0]);
        const auto zone = send(zone_class, "timeZoneForSecondsFromGMT:", {static_cast<std::uint64_t>(static_cast<std::int64_t>(time_real(s, 0)))}).x0;
        return zone ? process.objects().retain(zone) : 0;
    });
    p.function(core_foundation_library, "_CFTimeZoneGetSecondsFromGMT", [send, time_real, with_date](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto seconds = with_date(time_real(s, 0), [&](std::uint64_t date) { return static_cast<std::int64_t>(send(s.x[0], "secondsFromGMTForDate:", {date}).x0); });
        s.v[0] = {std::bit_cast<std::uint64_t>(static_cast<double>(seconds)), 0};
        return 0;
    });
    p.function(core_foundation_library, "_CFTimeZoneCopyAbbreviation", [&process, send, time_real, with_date](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto text = with_date(time_real(s, 0), [&](std::uint64_t date) { return send(s.x[0], "abbreviationForDate:", {date}).x0; });
        return text ? process.objects().retain(text) : 0;
    });
    p.function(core_foundation_library, "_CFTimeZoneIsDaylightSavingTime", [send, time_real, with_date](CpuState& s, GuestMemory&) -> std::uint64_t {
        return with_date(time_real(s, 0), [&](std::uint64_t date) { return send(s.x[0], "isDaylightSavingTimeForDate:", {date}).x0 & 0xff; });
    });
    p.function(core_foundation_library, "_CFTimeZoneGetDaylightSavingTimeOffset", [send, time_real, with_date](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto offset = with_date(time_real(s, 0), [&](std::uint64_t date) { return send(s.x[0], "daylightSavingTimeOffsetForDate:", {date}).v[0][0]; });
        s.v[0] = {offset, 0};
        return 0;
    });
    // CFAbsoluteTime CFTimeZoneGetNextDaylightSavingTimeTransition(CFTimeZoneRef, CFAbsoluteTime at): 0.0 when none.
    p.function(core_foundation_library, "_CFTimeZoneGetNextDaylightSavingTimeTransition", [&cf, send, time_real, with_date](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto next = with_date(time_real(s, 0), [&](std::uint64_t date) { return send(s.x[0], "nextDaylightSavingTimeTransitionAfterDate:", {date}).x0; });
        s.v[0] = {std::bit_cast<std::uint64_t>(next ? cf.date_time(next) : 0.0), 0};
        return 0;
    });
    // CFStringRef CFTimeZoneCopyLocalizedName(CFTimeZoneRef, CFTimeZoneNameStyle, CFLocaleRef)
    p.function(core_foundation_library, "_CFTimeZoneCopyLocalizedName", [&process, send](CpuState& s, GuestMemory&) -> std::uint64_t {
        const auto text = send(s.x[0], "localizedName:locale:", {s.x[1], s.x[2]}).x0;
        return text ? process.objects().retain(text) : 0;
    });
    p.function(core_foundation_library, "_CFTimeZoneCopyKnownNames", [&process, send, zone_class](CpuState&, GuestMemory&) -> std::uint64_t {
        return process.objects().retain(send(zone_class, "knownTimeZoneNames").x0);
    });
    p.function(core_foundation_library, "_CFTimeZoneCopyAbbreviationDictionary", [abbreviation_dictionary](CpuState&, GuestMemory&) -> std::uint64_t {
        return abbreviation_dictionary();
    });
    p.function(core_foundation_library, "_CFTimeZoneSetAbbreviationDictionary", [set_abbreviation_dictionary](CpuState& s, GuestMemory&) -> std::uint64_t {
        set_abbreviation_dictionary(s.x[0]);
        return 0;
    });
    p.function(core_foundation_library, "_CFTimeZoneSetDefault", [send, zone_class](CpuState& s, GuestMemory&) -> std::uint64_t {
        static_cast<void>(send(zone_class, "setDefaultTimeZone:", {s.x[0]}));
        return 0;
    });
    p.function(core_foundation_library, "_CFTimeZoneResetSystem", [send, zone_class](CpuState&, GuestMemory&) -> std::uint64_t {
        static_cast<void>(send(zone_class, "resetSystemTimeZone"));
        return 0;
    });
    // CFDataRef CFTimeZoneGetData(CFTimeZoneRef): the zone's TZif bytes, which the host database does not expose.
    p.function(core_foundation_library, "_CFTimeZoneGetData", [](CpuState&, GuestMemory&) -> std::uint64_t {
        throw std::runtime_error("unsupported: CFTimeZoneGetData (the host time-zone database has no TZif data)");
    });
    add(zone_class, "timeZoneWithAbbreviation:", [zone_for_abbreviation, autorelease](CpuState& s, GuestMemory&) {
        s.x[0] = autorelease(zone_for_abbreviation(s.x[2]));
    }, true, "@@:@");

    // ---- NSUUID ----
    auto uuid_class = objc.find_class("NSUUID");
    {   // A methodless class shell from another provider gets these methods.
        if (!uuid_class) uuid_class = objc.register_native_class("NSUUID", objc.find_class("NSObject"), 16);
        alias(uuid_class, "NSUUID");
        const auto bytes_of = [&objc](std::uint64_t object) { return objc.host_state<CfUuid>(object); };
        add(uuid_class, "UUID", [&objc, uuid_class, bytes_of, autorelease](CpuState& s, GuestMemory&) {
            const auto object = objc.allocate(uuid_class);
            auto value = bytes_of(object);
            std::random_device device;
            for (auto& byte : value->bytes) byte = static_cast<std::uint8_t>(device());
            value->bytes[6] = static_cast<std::uint8_t>((value->bytes[6] & 0x0f) | 0x40);
            value->bytes[8] = static_cast<std::uint8_t>((value->bytes[8] & 0x3f) | 0x80);
            s.x[0] = autorelease(object);
        }, true);
        add(uuid_class, "init", [bytes_of](CpuState& s, GuestMemory&) {
            auto value = bytes_of(s.x[0]);
            std::random_device device;
            for (auto& byte : value->bytes) byte = static_cast<std::uint8_t>(device());
            value->bytes[6] = static_cast<std::uint8_t>((value->bytes[6] & 0x0f) | 0x40);
            value->bytes[8] = static_cast<std::uint8_t>((value->bytes[8] & 0x3f) | 0x80);
        });
        add(uuid_class, "initWithUUIDBytes:", [bytes_of](CpuState& s, GuestMemory& m) {
            auto value = bytes_of(s.x[0]);
            if (s.x[2]) m.read_bytes(s.x[2], value->bytes);
        }, false, "@@:r*");
        // -initWithUUIDString: accepts 8-4-4-4-12 hexadecimal digits; nil otherwise.
        add(uuid_class, "initWithUUIDString:", [&process, &cf, bytes_of](CpuState& s, GuestMemory&) {
            const auto text = s.x[2] ? cf.utf8(s.x[2]) : std::string{};
            std::array<std::uint8_t, 16> parsed{};
            bool valid = text.size() == 36;
            std::size_t out = 0;
            for (std::size_t i = 0; valid && i < text.size();) {
                if (i == 8 || i == 13 || i == 18 || i == 23) { valid = text[i] == '-'; ++i; continue; }
                const auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; };
                const auto high = digit(text[i]), low = i + 1 < text.size() ? digit(text[i + 1]) : -1;
                valid = high >= 0 && low >= 0 && out < 16;
                if (valid) parsed[out++] = static_cast<std::uint8_t>(high * 16 + low);
                i += 2;
            }
            if (!valid || out != 16) { process.objects().release(s.x[0]); s.x[0] = 0; return; }
            bytes_of(s.x[0])->bytes = parsed;
        }, false, "@@:@");
        add(uuid_class, "getUUIDBytes:", [bytes_of](CpuState& s, GuestMemory& m) {
            m.write_bytes(s.x[2], bytes_of(s.x[0])->bytes);
            s.x[0] = 0;
        }, false, "v@:^C");
        add(uuid_class, "UUIDString", [&cf, bytes_of, autorelease](CpuState& s, GuestMemory&) {
            static constexpr char digits[] = "0123456789ABCDEF";
            std::string text;
            const auto value = bytes_of(s.x[0]);
            for (std::size_t index = 0; index < 16; ++index) {
                if (index == 4 || index == 6 || index == 8 || index == 10) text.push_back('-');
                text.push_back(digits[value->bytes[index] >> 4]);
                text.push_back(digits[value->bytes[index] & 15]);
            }
            s.x[0] = autorelease(cf.string(text));
        });
        add(uuid_class, "isEqual:", [&objc, uuid_class, bytes_of](CpuState& s, GuestMemory&) {
            const auto other = s.x[2] ? objc.find_host_state<CfUuid>(s.x[2]) : nullptr;
            s.x[0] = other && objc.object_class(s.x[2]) == uuid_class && other->bytes == bytes_of(s.x[0])->bytes ? 1 : 0;
        }, false, "B@:@");
        add(uuid_class, "hash", [bytes_of](CpuState& s, GuestMemory&) {
            std::uint64_t value{};
            std::memcpy(&value, bytes_of(s.x[0])->bytes.data(), 8);
            s.x[0] = value;
        }, false, "Q@:");
        add(uuid_class, "copyWithZone:", [&process](CpuState& s, GuestMemory&) { s.x[0] = process.objects().retain(s.x[0]); });
    }

    // ---- NSValue ----
    struct Value : ObjectPayload { std::vector<std::uint8_t> bytes; std::string type; };
    auto value_class = objc.find_class("NSValue");
    {   // A methodless class shell from another provider gets these methods.
        if (!value_class) value_class = objc.register_native_class("NSValue", objc.find_class("NSObject"), 16);
        alias(value_class, "NSValue");
        const auto make = [&objc, value_class](std::span<const std::uint8_t> bytes, std::string type) {
            const auto object = objc.allocate(value_class);
            auto value = objc.host_state<Value>(object);
            value->bytes.assign(bytes.begin(), bytes.end());
            value->type = std::move(type);
            return object;
        };
        const auto state = [&objc](std::uint64_t object) {
            auto value = objc.find_host_state<Value>(object);
            if (!value) throw std::runtime_error("Alchemy NSValue: object holds no value");
            return value;
        };
        // Sizes of the encodings NSValue boxes here: scalars, pointers and the geometry structs.
        const auto encoded_size = [](std::string_view type) -> std::optional<std::size_t> {
            if (type.size() == 1) {
                switch (type[0]) {
                case 'c': case 'C': case 'B': return 1;
                case 's': case 'S': return 2;
                case 'i': case 'I': case 'f': return 4;
                case 'l': case 'L': case 'q': case 'Q': case 'd': case '@': case '#': case ':': case '*': return 8;
                default: return std::nullopt;
                }
            }
            if (type.starts_with('^')) return 8;
            if (type.starts_with("{_NSRange=") || type.starts_with("{CGPoint=") || type.starts_with("{CGSize=") || type.starts_with("{_NSPoint=") ||
                type.starts_with("{_NSSize=")) return 16;
            if (type.starts_with("{CGRect=") || type.starts_with("{_NSRect=")) return 32;
            return std::nullopt;
        };
        // +valueWithBytes:objCType: / -initWithBytes:objCType:
        const auto from_bytes = [make, encoded_size](GuestMemory& m, std::uint64_t bytes, std::uint64_t encoding) {
            const auto type = guest_string(m, encoding, 1024);
            const auto size = encoded_size(type);
            if (!size) throw std::runtime_error("Alchemy NSValue: unsupported type encoding " + type);
            std::vector<std::uint8_t> data(*size);
            m.read_bytes(bytes, data);
            return make(data, type);
        };
        add(value_class, "valueWithBytes:objCType:", [from_bytes, autorelease](CpuState& s, GuestMemory& m) {
            s.x[0] = autorelease(from_bytes(m, s.x[2], s.x[3]));
        }, true, "@@:r^vr*");
        add(value_class, "value:withObjCType:", [from_bytes, autorelease](CpuState& s, GuestMemory& m) {
            s.x[0] = autorelease(from_bytes(m, s.x[2], s.x[3]));
        }, true, "@@:r^vr*");
        const auto scalar = [make, autorelease](std::uint64_t value, const char* type) {
            std::array<std::uint8_t, 8> bytes{};
            std::memcpy(bytes.data(), &value, 8);
            return autorelease(make(bytes, type));
        };
        add(value_class, "valueWithPointer:", [scalar](CpuState& s, GuestMemory&) { s.x[0] = scalar(s.x[2], "^v"); }, true, "@@:^v");
        add(value_class, "valueWithNonretainedObject:", [scalar](CpuState& s, GuestMemory&) { s.x[0] = scalar(s.x[2], "@"); }, true);
        add(value_class, "valueWithRange:", [make, autorelease](CpuState& s, GuestMemory&) {
            std::array<std::uint8_t, 16> bytes{};
            std::memcpy(bytes.data(), &s.x[2], 8);
            std::memcpy(bytes.data() + 8, &s.x[3], 8);
            s.x[0] = autorelease(make(bytes, "{_NSRange=QQ}"));
        }, true, "@@:{_NSRange=QQ}");
        // NSPoint/NSSize/NSRect are homogeneous double aggregates (d0..d3).
        const auto hfa = [make, autorelease](CpuState& s, unsigned count, const char* type) {
            std::vector<std::uint8_t> bytes(count * 8);
            for (unsigned index = 0; index < count; ++index) std::memcpy(bytes.data() + index * 8, &s.v[index][0], 8);
            return autorelease(make(bytes, type));
        };
        add(value_class, "valueWithPoint:", [hfa](CpuState& s, GuestMemory&) { s.x[0] = hfa(s, 2, "{CGPoint=dd}"); }, true, "@@:{CGPoint=dd}");
        add(value_class, "valueWithSize:", [hfa](CpuState& s, GuestMemory&) { s.x[0] = hfa(s, 2, "{CGSize=dd}"); }, true, "@@:{CGSize=dd}");
        add(value_class, "valueWithRect:", [hfa](CpuState& s, GuestMemory&) { s.x[0] = hfa(s, 4, "{CGRect={CGPoint=dd}{CGSize=dd}}"); }, true,
            "@@:{CGRect={CGPoint=dd}{CGSize=dd}}");
        const auto load_hfa = [state](CpuState& s, unsigned count) {
            const auto value = state(s.x[0]);
            if (value->bytes.size() < count * 8) throw std::runtime_error("Alchemy NSValue: the value is not a " + std::to_string(count * 8) + "-byte struct");
            for (unsigned index = 0; index < count; ++index) {
                std::uint64_t bits{};
                std::memcpy(&bits, value->bytes.data() + index * 8, 8);
                s.v[index] = {bits, 0};
            }
        };
        add(value_class, "pointValue", [load_hfa](CpuState& s, GuestMemory&) { load_hfa(s, 2); }, false, "{CGPoint=dd}@:");
        add(value_class, "sizeValue", [load_hfa](CpuState& s, GuestMemory&) { load_hfa(s, 2); }, false, "{CGSize=dd}@:");
        add(value_class, "rectValue", [load_hfa](CpuState& s, GuestMemory&) { load_hfa(s, 4); }, false, "{CGRect={CGPoint=dd}{CGSize=dd}}@:");
        add(value_class, "rangeValue", [state](CpuState& s, GuestMemory&) {
            const auto value = state(s.x[0]);
            if (value->bytes.size() < 16) throw std::runtime_error("Alchemy NSValue: the value is not an NSRange");
            std::memcpy(&s.x[1], value->bytes.data() + 8, 8);
            std::memcpy(&s.x[0], value->bytes.data(), 8);
        }, false, "{_NSRange=QQ}@:");
        for (const auto* name : {"pointerValue", "nonretainedObjectValue"}) {
            add(value_class, name, [state](CpuState& s, GuestMemory&) {
                const auto value = state(s.x[0]);
                std::uint64_t bits{};
                std::memcpy(&bits, value->bytes.data(), std::min<std::size_t>(8, value->bytes.size()));
                s.x[0] = bits;
            }, false, "^v@:");
        }
        // -getValue:size: / -getValue: copy the stored bytes out.
        add(value_class, "getValue:size:", [state](CpuState& s, GuestMemory& m) {
            const auto value = state(s.x[0]);
            if (s.x[3] != value->bytes.size()) throw std::runtime_error("Alchemy NSValue: getValue:size: with the wrong size");
            m.write_bytes(s.x[2], value->bytes);
            s.x[0] = 0;
        }, false, "v@:^vQ");
        add(value_class, "getValue:", [state](CpuState& s, GuestMemory& m) {
            m.write_bytes(s.x[2], state(s.x[0])->bytes);
            s.x[0] = 0;
        }, false, "v@:^v");
        // -objCType: the encoding string lives as long as the value.
        auto encodings = std::make_shared<std::pair<std::mutex, std::map<std::string, std::uint64_t>>>();
        add(value_class, "objCType", [&process, state, encodings](CpuState& s, GuestMemory& m) {
            const auto type = state(s.x[0])->type;
            std::lock_guard lock(encodings->first);
            auto& cell = encodings->second[type];
            if (!cell) { cell = process.allocate_guest(type.size() + 1); put_string(m, cell, type); }
            s.x[0] = cell;
        }, false, "r*@:");
        add(value_class, "isEqualToValue:", [&objc, state](CpuState& s, GuestMemory&) {
            const auto other = s.x[2] ? objc.find_host_state<Value>(s.x[2]) : nullptr;
            const auto own = state(s.x[0]);
            s.x[0] = other && other->type == own->type && other->bytes == own->bytes ? 1 : 0;
        }, false, "B@:@");
        add(value_class, "isEqual:", [&objc, state](CpuState& s, GuestMemory&) {
            const auto other = s.x[2] ? objc.find_host_state<Value>(s.x[2]) : nullptr;
            const auto own = state(s.x[0]);
            s.x[0] = other && other->type == own->type && other->bytes == own->bytes ? 1 : 0;
        }, false, "B@:@");
        add(value_class, "hash", [state](CpuState& s, GuestMemory&) {
            const auto value = state(s.x[0]);
            s.x[0] = std::hash<std::string_view>{}(std::string_view(reinterpret_cast<const char*>(value->bytes.data()), value->bytes.size()));
        }, false, "Q@:");
        add(value_class, "copyWithZone:", [&process](CpuState& s, GuestMemory&) { s.x[0] = process.objects().retain(s.x[0]); });
    }

    // ---- Foundation geometry and range functions (NSGeometry.h, NSRange.h) ----
    // NSPoint/NSSize/NSRect are HFAs of doubles (d0..), NSRange is two words (x0, x1).
    const auto real = [](const CpuState& s, std::size_t index) { return std::bit_cast<double>(s.v[index][0]); };
    const auto put_real = [](CpuState& s, std::size_t index, double value) { s.v[index] = {std::bit_cast<std::uint64_t>(value), 0}; };
    const auto number = [](double value) {
        char buffer[64]{};
        const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        return std::string(buffer, result.ptr);
    };
    const auto string_result = [&cf, autorelease](CpuState& s, const std::string& text) { s.x[0] = autorelease(cf.string(text)); };
    const auto raw = [&process](const char* name, transmute::GuestCallHandler handler) {
        if (!provided(process, foundation_library, name)) static_cast<void>(process.install_function(name, std::move(handler), std::string(foundation_library)));
    };
    raw("_NSIntersectionRange", [](CpuState& s, GuestMemory&) {
        const auto begin = std::max(s.x[0], s.x[2]), end = std::min(s.x[0] + s.x[1], s.x[2] + s.x[3]);
        if (end <= begin) { s.x[0] = 0; s.x[1] = 0; } else { s.x[0] = begin; s.x[1] = end - begin; }
    });
    raw("_NSUnionRange", [](CpuState& s, GuestMemory&) {
        const auto begin = std::min(s.x[0], s.x[2]), end = std::max(s.x[0] + s.x[1], s.x[2] + s.x[3]);
        s.x[0] = begin; s.x[1] = end - begin;
    });
    raw("_NSStringFromRange", [string_result](CpuState& s, GuestMemory&) {
        string_result(s, "{" + std::to_string(s.x[0]) + ", " + std::to_string(s.x[1]) + "}");
    });
    raw("_NSStringFromPoint", [real, number, string_result](CpuState& s, GuestMemory&) {
        string_result(s, "{" + number(real(s, 0)) + ", " + number(real(s, 1)) + "}");
    });
    raw("_NSStringFromSize", [real, number, string_result](CpuState& s, GuestMemory&) {
        string_result(s, "{" + number(real(s, 0)) + ", " + number(real(s, 1)) + "}");
    });
    raw("_NSStringFromRect", [real, number, string_result](CpuState& s, GuestMemory&) {
        string_result(s, "{{" + number(real(s, 0)) + ", " + number(real(s, 1)) + "}, {" + number(real(s, 2)) + ", " + number(real(s, 3)) + "}}");
    });
    // NSRect NSOffsetRect(NSRect, CGFloat dx, CGFloat dy)
    raw("_NSOffsetRect", [real, put_real](CpuState& s, GuestMemory&) {
        put_real(s, 0, real(s, 0) + real(s, 4));
        put_real(s, 1, real(s, 1) + real(s, 5));
    });
    // BOOL NSPointInRect(NSPoint, NSRect): the half-open rectangle (NSMouseInRect, flipped).
    raw("_NSPointInRect", [real](CpuState& s, GuestMemory&) {
        const auto x = real(s, 0), y = real(s, 1), left = real(s, 2), top = real(s, 3), width = real(s, 4), height = real(s, 5);
        s.x[0] = x >= left && x < left + width && y >= top && y < top + height ? 1 : 0;
    });
    // NSString* NSUserName(void): the login name of the virtual user.
    raw("_NSUserName", [p, &cf, autorelease](CpuState& s, GuestMemory& m) {
        const auto name = p.call(libsystem_library, "_getlogin", {});
        s.x[0] = name ? autorelease(cf.string(guest_string(m, name, 256))) : 0;
    });

    // Whether instances of `cls` already respond to `selector` (another provider's method).
    const auto has_method = [p, &objc](std::uint64_t cls, const char* selector) {
        return p.call(objc_library, "_class_getInstanceMethod", {cls, objc.selector(selector)}) != 0;
    };

    // ---- NSBundle.executableURL: the executablePath as a file URL ----
    if (const auto bundle_class = objc.find_class("NSBundle"); bundle_class && !has_method(bundle_class, "executableURL")) {
        add(bundle_class, "executableURL", [&objc, &cf, autorelease](CpuState& s, GuestMemory&) {
            const auto path = objc.send(s.x[0], "executablePath").x0;
            s.x[0] = path ? autorelease(cf.file_url(cf.utf8(path), false)) : 0;
        });
    }

    // ---- NSProcessInfo activities ----
    // -beginActivityWithOptions:reason: returns a token; the idle-sleep
    // options become host power requests held until -endActivity:
    // (NSActivityIdleSystemSleepDisabled keeps the system awake,
    // NSActivityIdleDisplaySleepDisabled the display).
    // Sudden/automatic termination and App Nap do not exist for an Alchemy
    // process, so the other options need no action.
    if (const auto info_class = objc.find_class("NSProcessInfo"); info_class && !has_method(info_class, "beginActivityWithOptions:reason:")) {
        struct Activity : ObjectPayload {
            host::system::PowerRequest request;
            void end() { request.release(); }
        };
        auto activity_class = objc.find_class("_NSActivityAssertion");
        if (!activity_class) activity_class = objc.register_native_class("_NSActivityAssertion", objc.find_class("NSObject"), 16);
        const auto begin = [&process, activity_class](std::uint64_t options) {
            auto activity = std::make_shared<Activity>();
            const bool system = (options & (1ULL << 20)) != 0, display = (options & (1ULL << 40)) != 0;
            if (system || display)
                if (auto request = host::system::PowerRequest::create("Alchemy guest activity (NSProcessInfo)", system, display))
                    activity->request = std::move(*request);
            return process.objects().create(16, activity_class, ObjectKind::instance, std::move(activity));
        };
        add(info_class, "beginActivityWithOptions:reason:", [begin, autorelease](CpuState& s, GuestMemory&) {
            s.x[0] = autorelease(begin(s.x[2]));
        }, false, "@@:Q@");
        add(info_class, "endActivity:", [&process](CpuState& s, GuestMemory&) {
            const auto record = s.x[2] ? process.objects().find(s.x[2]) : nullptr;
            if (const auto activity = record ? std::dynamic_pointer_cast<Activity>(record->payload) : nullptr) activity->end();
        }, false, "v@:@");
        // -performActivityWithOptions:reason:usingBlock: runs the block synchronously inside the activity.
        add(info_class, "performActivityWithOptions:reason:usingBlock:", [&process, begin](CpuState& s, GuestMemory& m) {
            const auto token = begin(s.x[2]);
            try {
                if (s.x[4]) {
                    const std::array<std::uint64_t, 1> arguments{s.x[4]};
                    static_cast<void>(EngineAdapter(process.calling_engine()).call(m.read64(s.x[4] + 16), arguments));
                }
            } catch (...) { process.objects().release(token); throw; }
            process.objects().release(token);
        }, false, "v@:Q@@?");
    }
}

} // namespace alchemy::export_fill
