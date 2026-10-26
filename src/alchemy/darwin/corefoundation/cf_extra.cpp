#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include "alchemy/host/files.hpp"
#include "alchemy/object_store.hpp"
#include "alchemy/path.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>


// CoreFoundation pieces Qt Core (and the scatter/VFB libraries) import that
// sit on existing Alchemy objects:
//   - constants: kCFAbsoluteTimeIntervalSince1970, NSCurrentLocaleDidChange-
//     Notification, the kCFDateFormatter/kCFNumberFormatter/kCFLocale
//     quotation/kCFPreferences/kCFURLVolume*Capacity keys, UTType identifiers;
//   - CFBundle over NSBundle (foundation_extra.cpp): main bundle, create,
//     identifier, info dictionary values, bundle/executable/resource/plug-in
//     URLs, package info;
//   - NoCopy constructors (the contents are copied; ownership of the bytes
//     moves to Alchemy, which releases default-allocator memory at once),
//     CFStringCreateWithFileSystemRepresentation, CFStringCreateMutableCopy,
//     CFArrayAppendArray, CFAutorelease, CFStringGetSystemEncoding;
//   - CFUUID bytes, CFURLCopyResourcePropertiesForKeys (with volume
//     capacities), and the bookmark functions (hosts have no alias files).
namespace alchemy::export_fill {
namespace {

constexpr std::string_view core_services_library = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
constexpr std::uint32_t utf8_encoding = 0x08000100;

std::filesystem::path host_path(ProcessContext& process, const std::string& guest) {
    return process.map_guest_path ? process.map_guest_path(guest) : utf8_path(guest);
}

// A data export holding a pointer to an immortal CFString.
void constant_string(ProcessContext& process, CoreFoundation& cf, std::string_view library, const std::string& name, std::string_view value) {
    if (provided(process, library, name)) return;
    const auto string = cf.string(value);
    if (const auto record = process.objects().find(string)) record->immortal = true;
    const auto cell = process.allocate_guest(8);
    process.main_engine().write64(cell, string);
    process.install_export(std::string(library), name, {cell, ExportKind::data});
}

} // namespace

void install_cf_extra(Provider& p, ObjcRuntime& objc, CoreFoundation& cf) {
    auto& process = p.process();
    const std::string cf_library(core_foundation_library);
    const auto function = [&](std::string_view library, const char* name, transmute::GuestCallHandler handler) {
        if (provided(process, library, name)) return;
        static_cast<void>(process.install_function(name, std::move(handler), std::string(library)));
    };

    // ---- constants ----
    if (!process.resolve(cf_library, "_kCFAbsoluteTimeIntervalSince1970")) {
        // const CFTimeInterval: seconds from 1970-01-01 to 2001-01-01.
        std::array<std::uint8_t, 8> bytes{};
        const double value = 978307200.0;
        std::memcpy(bytes.data(), &value, 8);
        static_cast<void>(p.data(cf_library, "_kCFAbsoluteTimeIntervalSince1970", 8, bytes));
    }
    constant_string(process, cf, cf_library, "_NSCurrentLocaleDidChangeNotification", "kCFLocaleCurrentLocaleDidChangeNotification");
    constant_string(process, cf, foundation_library, "_NSCurrentLocaleDidChangeNotification", "kCFLocaleCurrentLocaleDidChangeNotification");
    for (const auto* name : {"AnyApplication", "CurrentApplication", "AnyUser", "CurrentUser", "AnyHost", "CurrentHost"})
        constant_string(process, cf, cf_library, std::string("_kCFPreferences") + name, std::string("kCFPreferences") + name);
    for (const auto* name : {"MonthSymbols", "ShortMonthSymbols", "VeryShortMonthSymbols", "StandaloneMonthSymbols",
                             "ShortStandaloneMonthSymbols", "VeryShortStandaloneMonthSymbols", "WeekdaySymbols", "ShortWeekdaySymbols",
                             "VeryShortWeekdaySymbols", "StandaloneWeekdaySymbols", "ShortStandaloneWeekdaySymbols",
                             "VeryShortStandaloneWeekdaySymbols", "AMSymbol", "PMSymbol", "EraSymbols", "LongEraSymbols",
                             "QuarterSymbols", "ShortQuarterSymbols", "TimeZone", "Calendar", "IsLenient", "DefaultFormat"})
        constant_string(process, cf, cf_library, std::string("_kCFDateFormatter") + name, std::string("kCFDateFormatter") + name + "Key");
    for (const auto* name : {"CurrencySymbol", "CurrencyCode", "DecimalSeparator", "GroupingSeparator", "MinusSign", "PlusSign",
                             "PercentSymbol", "ZeroSymbol", "NaNSymbol", "InfinitySymbol", "InternationalCurrencySymbol",
                             "MinFractionDigits", "MaxFractionDigits", "UseGroupingSeparator", "PositivePrefix", "NegativePrefix"})
        constant_string(process, cf, cf_library, std::string("_kCFNumberFormatter") + name, std::string("kCFNumberFormatter") + name + "Key");
    for (const auto* name : {"QuotationBeginDelimiterKey", "QuotationEndDelimiterKey", "AlternateQuotationBeginDelimiterKey",
                             "AlternateQuotationEndDelimiterKey"})
        constant_string(process, cf, cf_library, std::string("_kCFLocale") + name, std::string("kCFLocale") + name);
    for (const auto* name : {"VolumeAvailableCapacityKey", "VolumeTotalCapacityKey"}) {
        constant_string(process, cf, cf_library, std::string("_kCFURL") + name, std::string("NSURL") + name);
        constant_string(process, cf, foundation_library, std::string("_NSURL") + name, std::string("NSURL") + name);
    }
    // Uniform Type Identifiers (CoreServices LaunchServices UTCoreTypes.h).
    for (const auto& [name, value] : std::array<std::pair<const char*, const char*>, 15>{{
             {"_kUTTypeBundle", "com.apple.bundle"}, {"_kUTTypeTIFF", "public.tiff"}, {"_kUTTypeFileURL", "public.file-url"},
             {"_kUTTypeHTML", "public.html"}, {"_kUTTypeImage", "public.image"}, {"_kUTTypePDF", "com.adobe.pdf"},
             {"_kUTTypePlainText", "public.plain-text"}, {"_kUTTypeUTF16ExternalPlainText", "public.utf16-external-plain-text"},
             {"_kUTTypeUTF16PlainText", "public.utf16-plain-text"}, {"_kUTTypeUTF8PlainText", "public.utf8-plain-text"},
             {"_kUTTypeInkText", "com.apple.ink.inktext"}, {"_kUTTypeVCard", "public.vcard"}, {"_kUTTypeURL", "public.url"},
             {"_kUTTagClassFilenameExtension", "public.filename-extension"}, {"_kUTTagClassMIMEType", "public.mime-type"}}})
        constant_string(process, cf, core_services_library, name, value);

    // ---- ownership-transferring constructors ----
    // The bytes are copied. A deallocator other than kCFAllocatorNull hands
    // the buffer to CF: with the default allocator (NULL or
    // kCFAllocatorDefault/SystemDefault/Malloc, all NULL here) it is freed now;
    // custom allocators are unsupported (as everywhere in Alchemy CF).
    const auto null_allocator = [&process]() -> std::uint64_t {
        const auto found = process.resolve(core_foundation_library, "_kCFAllocatorNull");
        return found ? process.main_engine().read64(found->address) : 0;
    };
    const auto release_contents = [p, null_allocator](std::uint64_t deallocator, std::uint64_t bytes) {
        if (!bytes || (deallocator && deallocator == null_allocator())) return;
        if (deallocator) throw std::runtime_error("Alchemy CoreFoundation: custom contents deallocators are unsupported");
        static_cast<void>(p.call(libsystem_library, "_free", {bytes}));
    };
    // CFDataRef CFDataCreateWithBytesNoCopy(alloc, const UInt8*, CFIndex, CFAllocatorRef deallocator)
    function(cf_library, "_CFDataCreateWithBytesNoCopy", [&cf, release_contents](CpuState& s, GuestMemory&) {
        const auto data = cf.invoke("CFDataCreate", std::array<std::uint64_t, 3>{s.x[0], s.x[1], s.x[2]});
        release_contents(s.x[3], s.x[1]);
        s.x[0] = data;
    });
    // CFStringRef CFStringCreateWithCharactersNoCopy(alloc, const UniChar*, CFIndex, CFAllocatorRef)
    function(cf_library, "_CFStringCreateWithCharactersNoCopy", [&cf, release_contents](CpuState& s, GuestMemory&) {
        const auto string = cf.invoke("CFStringCreateWithCharacters", std::array<std::uint64_t, 3>{s.x[0], s.x[1], s.x[2]});
        release_contents(s.x[3], s.x[1]);
        s.x[0] = string;
    });
    // CFStringRef CFStringCreateWithCStringNoCopy(alloc, const char*, CFStringEncoding, CFAllocatorRef)
    function(cf_library, "_CFStringCreateWithCStringNoCopy", [&cf, release_contents](CpuState& s, GuestMemory&) {
        const auto string = cf.invoke("CFStringCreateWithCString", std::array<std::uint64_t, 3>{s.x[0], s.x[1], s.x[2]});
        release_contents(s.x[3], s.x[1]);
        s.x[0] = string;
    });
    // CFStringRef CFStringCreateWithFileSystemRepresentation(alloc, const char*): UTF-8 on Darwin.
    function(cf_library, "_CFStringCreateWithFileSystemRepresentation", [&cf](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[1] ? cf.invoke("CFStringCreateWithCString", std::array<std::uint64_t, 3>{s.x[0], s.x[1], utf8_encoding}) : 0;
    });
    // CFMutableStringRef CFStringCreateMutableCopy(alloc, CFIndex maxLength, CFStringRef)
    function(cf_library, "_CFStringCreateMutableCopy", [&cf](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
        s.x[0] = cf.mutable_copy(s.x[2]);
    });
    // CFStringEncoding CFStringGetSystemEncoding(void): kCFStringEncodingMacRoman, as on macOS.
    function(cf_library, "_CFStringGetSystemEncoding", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    // void CFArrayAppendArray(CFMutableArrayRef, CFArrayRef, CFRange)
    function(cf_library, "_CFArrayAppendArray", [&cf](CpuState& s, GuestMemory&) {
        const auto target = s.x[0], source = s.x[1];
        const auto location = static_cast<std::int64_t>(s.x[2]), length = static_cast<std::int64_t>(s.x[3]);
        const auto count = static_cast<std::int64_t>(cf.length(source));
        if (location < 0 || length < 0 || location + length > count)
            throw std::runtime_error("Alchemy CFArrayAppendArray: range outside the source array");
        for (auto index = location; index < location + length; ++index) {
            const auto value = cf.element(source, static_cast<std::uint64_t>(index));
            static_cast<void>(cf.invoke("CFArrayAppendValue", std::array<std::uint64_t, 2>{target, value}));
        }
        s.x[0] = 0;
    });
    // CFTypeRef CFAutorelease(CFTypeRef)
    function(cf_library, "_CFAutorelease", [p](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? p.call(objc_library, "_objc_autorelease", {s.x[0]}) : 0;
    });

    // ---- CFUUID bytes ----
    // CFUUIDBytes CFUUIDGetUUIDBytes(CFUUIDRef): 16 bytes returned in x0/x1.
    function(cf_library, "_CFUUIDGetUUIDBytes", [&process](CpuState& s, GuestMemory&) {
        const auto record = s.x[0] ? process.objects().find(s.x[0]) : nullptr;
        const auto payload = record ? std::dynamic_pointer_cast<CfUuid>(record->payload) : nullptr;
        if (!payload) throw std::runtime_error("Alchemy CoreFoundation: object is not a CFUUID");
        std::memcpy(&s.x[0], payload->bytes.data(), 8);
        std::memcpy(&s.x[1], payload->bytes.data() + 8, 8);
    });
    // CFUUIDRef CFUUIDCreateFromUUIDBytes(alloc, CFUUIDBytes): the bytes arrive in x1/x2.
    function(cf_library, "_CFUUIDCreateFromUUIDBytes", [&process, &objc](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
        auto payload = std::make_shared<CfUuid>();
        std::memcpy(payload->bytes.data(), &s.x[1], 8);
        std::memcpy(payload->bytes.data() + 8, &s.x[2], 8);
        s.x[0] = process.objects().create(16, cf_uuid_class(objc), ObjectKind::instance, std::move(payload));
    });

    // ---- CFURL ----
    // CFDictionaryRef CFURLCopyResourcePropertiesForKeys(CFURLRef, CFArrayRef keys, CFErrorRef*)
    function(cf_library, "_CFURLCopyResourcePropertiesForKeys", [&process, &cf](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
        std::vector<std::uint64_t> owned;
        const auto keys = cf.array_values(s.x[1]);
        try {
            for (const auto key : keys) {
                const auto name = cf.utf8(key);
                std::optional<std::uint64_t> value;
                if (name == "NSURLVolumeAvailableCapacityKey" || name == "NSURLVolumeTotalCapacityKey") {
                    const auto root = host_path(process, cf.url_path(s.x[0])).root_path().u8string();
                    if (const auto volume = host::files::volume(std::string(reinterpret_cast<const char*>(root.data()), root.size())))
                        value = cf.integer(static_cast<std::int64_t>(name == "NSURLVolumeTotalCapacityKey" ? volume->total : volume->available), 4);
                } else value = cf.resource_value(s.x[0], key);
                if (!value) continue;
                owned.push_back(*value);
                entries.emplace_back(key, *value);
            }
            s.x[0] = cf.dictionary(entries);
        } catch (...) {
            for (const auto value : owned) process.objects().release(value);
            throw;
        }
        for (const auto value : owned) process.objects().release(value);
    });
    // Bookmark data comes from Finder alias files, which host file systems do
    // not have, so no file holds bookmark data and none can be resolved.
    // CFDataRef CFURLCreateBookmarkDataFromFile(alloc, CFURLRef, CFErrorRef*)
    function(cf_library, "_CFURLCreateBookmarkDataFromFile", [](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write64(s.x[2], 0);
        s.x[0] = 0;
    });
    // CFURLRef CFURLCreateByResolvingBookmarkData(alloc, CFDataRef, options, CFURLRef relative,
    //                                             CFArrayRef keys, Boolean* isStale, CFErrorRef*)
    function(cf_library, "_CFURLCreateByResolvingBookmarkData", [](CpuState& s, GuestMemory& m) {
        if (s.x[5]) m.write8(s.x[5], 0);
        if (s.x[6]) m.write64(s.x[6], 0);
        s.x[0] = 0;
    });

    // ---- CFBundle over NSBundle ----
    const auto bundle_class = [&objc]() {
        const auto found = objc.find_class("NSBundle");
        if (!found) throw std::runtime_error("Alchemy CFBundle: NSBundle is not available");
        return found;
    };
    const auto send = [&objc](std::uint64_t receiver, std::string_view selector, std::initializer_list<std::uint64_t> arguments = {}) {
        CallRegs registers;
        std::size_t index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return objc.send(receiver, selector, registers).x0;
    };
    const auto retained = [&process](std::uint64_t object) { return object ? process.objects().retain(object) : 0; };
    // CFBundleRef CFBundleGetMainBundle(void)
    function(cf_library, "_CFBundleGetMainBundle", [bundle_class, send](CpuState& s, GuestMemory&) { s.x[0] = send(bundle_class(), "mainBundle"); });
    // CFBundleRef CFBundleCreate(alloc, CFURLRef bundleURL)
    function(cf_library, "_CFBundleCreate", [&process, &objc, &cf, bundle_class, send](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
        if (!s.x[1]) { s.x[0] = 0; return; }
        const auto path = cf.string(cf.url_path(s.x[1]));
        const auto object = objc.allocate(bundle_class());
        std::uint64_t result{};
        try { result = send(object, "initWithPath:", {path}); }
        catch (...) { process.objects().release(path); throw; }
        process.objects().release(path);
        s.x[0] = result;
    });
    // CFBundleRef CFBundleGetBundleWithIdentifier(CFStringRef): bundles are
    // not registered by identifier; the main bundle is the one known bundle.
    function(cf_library, "_CFBundleGetBundleWithIdentifier", [&cf, bundle_class, send](CpuState& s, GuestMemory&) {
        const auto main = send(bundle_class(), "mainBundle");
        const auto identifier = main ? send(main, "bundleIdentifier") : 0;
        s.x[0] = identifier && s.x[0] && cf.equal(identifier, s.x[0]) ? main : 0;
    });
    function(cf_library, "_CFBundleGetIdentifier", [send](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? send(s.x[0], "bundleIdentifier") : 0; });
    function(cf_library, "_CFBundleGetInfoDictionary", [send](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? send(s.x[0], "infoDictionary") : 0; });
    function(cf_library, "_CFBundleGetValueForInfoDictionaryKey", [send](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] && s.x[1] ? send(s.x[0], "objectForInfoDictionaryKey:", {s.x[1]}) : 0;
    });
    function(cf_library, "_CFBundleCopyBundleURL", [send, retained](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "bundleURL")); });
    function(cf_library, "_CFBundleCopyResourcesDirectoryURL", [send, retained](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "resourceURL")); });
    function(cf_library, "_CFBundleCopyBuiltInPlugInsURL", [send, retained](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "builtInPlugInsURL")); });
    function(cf_library, "_CFBundleCopyPrivateFrameworksURL", [send, retained](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "privateFrameworksURL")); });
    function(cf_library, "_CFBundleCopyExecutableURL", [&cf, send](CpuState& s, GuestMemory&) {
        const auto path = send(s.x[0], "executablePath");
        s.x[0] = path ? cf.file_url(cf.utf8(path), false) : 0;
    });
    // CFURLRef CFBundleCopyResourceURL(CFBundleRef, CFStringRef name, CFStringRef type, CFStringRef subDir)
    function(cf_library, "_CFBundleCopyResourceURL", [&process, &cf, send](CpuState& s, GuestMemory&) {
        const auto path = send(s.x[0], "pathForResource:ofType:inDirectory:", {s.x[1], s.x[2], s.x[3]});
        if (!path) { s.x[0] = 0; return; }
        const auto guest = cf.utf8(path);
        std::error_code error;
        s.x[0] = cf.file_url(guest, std::filesystem::is_directory(host_path(process, guest), error));
    });
    // ---- the rest of CFBundle over NSBundle (localizations, resources, the executable) ----
    const auto bundle_for_url = [&process, &cf, bundle_class, send](std::uint64_t url) -> std::uint64_t {   // +0
        if (!url) return 0;
        const auto path = cf.string(cf.url_path(url));
        const auto bundle = send(bundle_class(), "bundleWithPath:", {path});
        process.objects().release(path);
        return bundle;
    };
    // CFArrayRef CFBundleGetAllBundles(void): the bundles the process knows (kept until the next call).
    auto all_bundles = std::make_shared<std::pair<std::mutex, std::uint64_t>>();
    function(cf_library, "_CFBundleGetAllBundles", [&process, &cf, bundle_class, send, all_bundles](CpuState& s, GuestMemory&) {
        std::vector<std::uint64_t> values;
        for (const auto* selector : {"allBundles", "allFrameworks"})
            if (const auto list = send(bundle_class(), selector))
                for (const auto bundle : cf.array_values(list)) values.push_back(bundle);
        const auto array = cf.array(values);
        std::lock_guard lock(all_bundles->first);
        if (all_bundles->second) process.objects().release(all_bundles->second);
        all_bundles->second = array;
        s.x[0] = array;
    });
    // CFArrayRef CFBundleCreateBundlesFromDirectory(alloc, CFURLRef directoryURL, CFStringRef bundleType):
    // the bundles directly inside the directory whose extension is bundleType (any extension when NULL).
    function(cf_library, "_CFBundleCreateBundlesFromDirectory", [&process, &cf, bundle_class, send](CpuState& s, GuestMemory&) {
        if (s.x[0]) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
        std::vector<std::uint64_t> values;
        const auto directory = s.x[1] ? cf.url_path(s.x[1]) : std::string{};
        const auto type = s.x[2] ? cf.utf8(s.x[2]) : std::string{};
        std::error_code error;
        if (!directory.empty())
            for (std::filesystem::directory_iterator entry(host_path(process, directory), error), end; !error && entry != end; entry.increment(error)) {
                if (!entry->is_directory(error)) continue;
                const auto name8 = entry->path().filename().u8string();
                const std::string name(reinterpret_cast<const char*>(name8.data()), name8.size());
                const auto dot = name.rfind('.');
                if (dot == std::string::npos || dot == 0 || (!type.empty() && name.substr(dot + 1) != type)) continue;
                const auto path = cf.string(directory + "/" + name);
                if (const auto bundle = send(bundle_class(), "bundleWithPath:", {path})) values.push_back(bundle);
                process.objects().release(path);
            }
        s.x[0] = cf.array(values);
    });
    function(cf_library, "_CFBundleCopySharedFrameworksURL", [send, retained](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "sharedFrameworksURL")); });
    function(cf_library, "_CFBundleCopySharedSupportURL", [send, retained](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "sharedSupportURL")); });
    // CFURLRef CFBundleCopyAuxiliaryExecutableURL(CFBundleRef, CFStringRef executableName)
    function(cf_library, "_CFBundleCopyAuxiliaryExecutableURL", [send, retained](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[1] ? retained(send(s.x[0], "URLForAuxiliaryExecutable:", {s.x[1]})) : 0;
    });
    // CFArrayRef CFBundleCopyResourceURLsOfType(CFBundleRef, CFStringRef resourceType, CFStringRef subDirName)
    function(cf_library, "_CFBundleCopyResourceURLsOfType", [send, retained](CpuState& s, GuestMemory&) {
        s.x[0] = retained(send(s.x[0], "URLsForResourcesWithExtension:subdirectory:", {s.x[1], s.x[2]}));
    });
    // CFURLRef CFBundleCopyResourceURLForLocalization(CFBundleRef, name, type, subDir, CFStringRef localizationName)
    function(cf_library, "_CFBundleCopyResourceURLForLocalization", [send, retained](CpuState& s, GuestMemory&) {
        s.x[0] = retained(send(s.x[0], "URLForResource:withExtension:subdirectory:localization:", {s.x[1], s.x[2], s.x[3], s.x[4]}));
    });
    // CFURLRef CFBundleCopyResourceURLInDirectory(CFURLRef bundleURL, name, type, subDir)
    function(cf_library, "_CFBundleCopyResourceURLInDirectory", [send, retained, bundle_for_url](CpuState& s, GuestMemory&) {
        const auto bundle = bundle_for_url(s.x[0]);
        s.x[0] = bundle ? retained(send(bundle, "URLForResource:withExtension:subdirectory:", {s.x[1], s.x[2], s.x[3]})) : 0;
    });
    // CFArrayRef CFBundleCopyResourceURLsOfTypeInDirectory(CFURLRef bundleURL, type, subDir)
    function(cf_library, "_CFBundleCopyResourceURLsOfTypeInDirectory", [send, retained, bundle_for_url](CpuState& s, GuestMemory&) {
        const auto bundle = bundle_for_url(s.x[0]);
        s.x[0] = bundle ? retained(send(bundle, "URLsForResourcesWithExtension:subdirectory:", {s.x[1], s.x[2]})) : 0;
    });
    // CFStringRef CFBundleCopyLocalizedString(CFBundleRef, CFStringRef key, CFStringRef value, CFStringRef tableName)
    function(cf_library, "_CFBundleCopyLocalizedString", [send, retained](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? retained(send(s.x[0], "localizedStringForKey:value:table:", {s.x[1], s.x[2], s.x[3]})) : 0;
    });
    function(cf_library, "_CFBundleCopyBundleLocalizations", [send, retained](CpuState& s, GuestMemory&) { s.x[0] = retained(send(s.x[0], "localizations")); });
    // CFArrayRef CFBundleCopyPreferredLocalizationsFromArray(CFArrayRef locArray)
    function(cf_library, "_CFBundleCopyPreferredLocalizationsFromArray", [bundle_class, send, retained](CpuState& s, GuestMemory&) {
        s.x[0] = retained(send(bundle_class(), "preferredLocalizationsFromArray:", {s.x[0]}));
    });
    // CFArrayRef CFBundleCopyLocalizationsForPreferences(CFArrayRef locArray, CFArrayRef prefArray)
    function(cf_library, "_CFBundleCopyLocalizationsForPreferences", [bundle_class, send, retained](CpuState& s, GuestMemory&) {
        s.x[0] = retained(send(bundle_class(), "preferredLocalizationsFromArray:forPreferences:", {s.x[0], s.x[1]}));
    });
    // CFStringRef CFBundleGetDevelopmentRegion(CFBundleRef): the Info.plist's value (not retained).
    const auto english = cf.constant_string("en");
    function(cf_library, "_CFBundleGetDevelopmentRegion", [&cf, send, english](CpuState& s, GuestMemory&) {
        const auto region = s.x[0] ? send(s.x[0], "objectForInfoDictionaryKey:", {cf.constant_string("CFBundleDevelopmentRegion")}) : 0;
        s.x[0] = region ? region : english;
    });
    // CFDictionaryRef CFBundleGetLocalInfoDictionary(CFBundleRef): the preferred localization's InfoPlist.strings.
    function(cf_library, "_CFBundleGetLocalInfoDictionary", [send](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? send(s.x[0], "_alchemyLocalInfoDictionary") : 0; });
    function(cf_library, "_CFBundleIsExecutableLoaded", [send](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? send(s.x[0], "isLoaded") & 0xff : 0; });
    function(cf_library, "_CFBundleLoadExecutable", [send](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? send(s.x[0], "load") & 0xff : 0; });
    // Boolean CFBundleLoadExecutableAndReturnError(CFBundleRef, CFErrorRef*): the NSError is the CFError (+1 to the caller).
    function(cf_library, "_CFBundleLoadExecutableAndReturnError", [&process, send](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        const auto slot = process.allocate_guest(8);
        m.write64(slot, 0);
        const auto loaded = s.x[0] ? send(s.x[0], "loadAndReturnError:", {slot}) & 0xff : 0;
        const auto error = m.read64(slot);
        process.release_guest(slot);
        if (!loaded && error && s.x[1]) m.write64(s.x[1], process.objects().retain(error));
        s.x[0] = loaded;
    });
    function(cf_library, "_CFBundlePreflightExecutable", [&process, send](CpuState& s, GuestMemory& m) {
        if (s.x[1]) m.write64(s.x[1], 0);
        const auto slot = process.allocate_guest(8);
        m.write64(slot, 0);
        const auto ready = s.x[0] ? send(s.x[0], "preflightAndReturnError:", {slot}) & 0xff : 0;
        const auto error = m.read64(slot);
        process.release_guest(slot);
        if (!ready && error && s.x[1]) m.write64(s.x[1], process.objects().retain(error));
        s.x[0] = ready;
    });
    function(cf_library, "_CFBundleUnloadExecutable", [send](CpuState& s, GuestMemory&) { if (s.x[0]) static_cast<void>(send(s.x[0], "unload")); s.x[0] = 0; });
    function(cf_library, "_CFBundleCopyExecutableArchitectures", [send, retained](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? retained(send(s.x[0], "executableArchitectures")) : 0;
    });
    // Symbols of the bundle's executable (loaded on demand): dlsym(3) through a RTLD_NOLOAD handle; the main
    // bundle's are the process's (dlopen(NULL)).
    const auto symbol_of = [&process, &cf, bundle_class, send](std::uint64_t bundle, std::uint64_t name) -> std::uint64_t {
        if (!bundle || !name) return 0;
        constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
        const auto call = [&process](std::string_view library, std::string_view symbol, std::initializer_list<std::uint64_t> arguments) -> std::uint64_t {
            const auto found = process.resolve(library, symbol);
            if (!found) throw std::runtime_error("Alchemy CFBundle: missing " + std::string(symbol));
            return process.calling_engine().invoke_callback(found->address, std::span(arguments.begin(), arguments.size()));
        };
        const auto c_string = [&process](std::string_view text) {
            const auto address = process.allocate_guest(text.size() + 1, 1);
            transmute::GuestMemory& memory = process.calling_engine();
            memory.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
            memory.write8(address + text.size(), 0);
            return address;
        };
        std::uint64_t handle{};
        if (bundle == send(bundle_class(), "mainBundle")) handle = call(libsystem, "_dlopen", {0, 1});
        else {
            if (!(send(bundle, "load") & 0xff)) return 0;
            const auto path = send(bundle, "executablePath");
            if (!path) return 0;
            const auto text = c_string(cf.utf8(path));
            handle = call(libsystem, "_dlopen", {text, 1 | 0x10});   // RTLD_LAZY | RTLD_NOLOAD: the loaded image
            process.release_guest(text);
        }
        if (!handle) return 0;
        const auto symbol = c_string(cf.utf8(name));
        const auto address = call(libsystem, "_dlsym", {handle, symbol});
        process.release_guest(symbol);
        static_cast<void>(call(libsystem, "_dlclose", {handle}));
        return address;
    };
    // void* CFBundleGetDataPointerForName(CFBundleRef, CFStringRef symbolName)
    function(cf_library, "_CFBundleGetDataPointerForName", [symbol_of](CpuState& s, GuestMemory&) { s.x[0] = symbol_of(s.x[0], s.x[1]); });
    // void CFBundleGetFunctionPointersForNames / GetDataPointersForNames(CFBundleRef, CFArrayRef names, void* pointers[])
    for (const auto* name : {"_CFBundleGetFunctionPointersForNames", "_CFBundleGetDataPointersForNames"})
        function(cf_library, name, [&cf, symbol_of](CpuState& s, GuestMemory& m) {
            if (s.x[1] && s.x[2]) {
                const auto names = cf.array_values(s.x[1]);
                for (std::size_t index = 0; index < names.size(); ++index) m.write64(s.x[2] + index * 8, symbol_of(s.x[0], names[index]));
            }
            s.x[0] = 0;
        });
    // CFDictionaryRef CFBundleCopyInfoDictionaryForURL(CFURLRef): the Info.plist of the bundle at the URL.
    function(cf_library, "_CFBundleCopyInfoDictionaryForURL", [&process, &objc, &cf, bundle_class, send, retained](CpuState& s, GuestMemory&) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        const auto path = cf.string(cf.url_path(s.x[0]));
        const auto object = objc.allocate(bundle_class());
        std::uint64_t bundle{};
        try { bundle = send(object, "initWithPath:", {path}); }
        catch (...) { process.objects().release(path); throw; }
        process.objects().release(path);
        if (!bundle) { s.x[0] = 0; return; }
        const auto info = retained(send(bundle, "infoDictionary"));
        process.objects().release(bundle);
        s.x[0] = info;
    });
    // Boolean CFBundleGetPackageInfoInDirectory(CFURLRef, UInt32* packageType, UInt32* packageCreator)
    // From Contents/PkgInfo ("APPLabcd"), else the Info.plist's
    // CFBundlePackageType and CFBundleSignature.
    function(cf_library, "_CFBundleGetPackageInfoInDirectory", [&process, &objc, &cf, bundle_class, send](CpuState& s, GuestMemory& m) {
        const auto guest = cf.url_path(s.x[0]);
        const auto put = [&](std::uint64_t address, std::string_view code) {
            if (!address) return;
            std::uint32_t value{};
            for (std::size_t index = 0; index < 4; ++index)
                value = (value << 8) | static_cast<std::uint8_t>(index < code.size() ? code[index] : ' ');
            m.write32(address, value);
        };
        for (const auto* location : {"/Contents/PkgInfo", "/PkgInfo"}) {
            std::ifstream file(host_path(process, guest + location), std::ios::binary);
            char bytes[8]{};
            if (file && file.read(bytes, 8)) {
                put(s.x[1], std::string_view(bytes, 4));
                put(s.x[2], std::string_view(bytes + 4, 4));
                s.x[0] = 1;
                return;
            }
        }
        const auto path = cf.string(guest);
        const auto object = objc.allocate(bundle_class());
        std::uint64_t bundle{};
        try { bundle = send(object, "initWithPath:", {path}); }
        catch (...) { process.objects().release(path); throw; }
        process.objects().release(path);
        std::string type, creator;
        if (bundle) {
            const auto info = send(bundle, "infoDictionary");
            const auto read = [&](std::string_view key) {
                const auto name = cf.string(key);
                const auto value = info ? cf.dictionary_value(info, name) : 0;
                process.objects().release(name);
                return value ? cf.utf8(value) : std::string{};
            };
            type = read("CFBundlePackageType");
            creator = read("CFBundleSignature");
            process.objects().release(bundle);
        }
        if (type.empty()) { s.x[0] = 0; return; }
        put(s.x[1], type);
        put(s.x[2], creator.empty() ? "????" : creator);
        s.x[0] = 1;
    });

    // CFDataRef CFStringCreateExternalRepresentation(CFAllocatorRef, CFStringRef,
    //     CFStringEncoding, UInt8 lossByte): CFStringGetBytes with the external
    //     representation (a BOM for the UTF-16/UTF-32 host-order encodings);
    //     NULL when a character cannot be converted and lossByte is 0.
    function(cf_library, "_CFStringCreateExternalRepresentation", [&process, &cf](CpuState& s, GuestMemory& m) {
        const auto string = s.x[1], encoding = s.x[2] & 0xffffffffULL, loss = s.x[3] & 0xff;
        if (!string) { s.x[0] = 0; return; }
        const auto length = cf.length(string);
        const auto used = process.allocate_guest(8);
        const std::array<std::uint64_t, 9> measure{string, 0, length, encoding, loss, 1, 0, 0, used};
        const auto converted = cf.invoke("CFStringGetBytes", measure);
        const auto size = m.read64(used);
        if (converted != length) { process.release_guest(used); s.x[0] = 0; return; }
        const auto buffer = process.allocate_guest(size ? size : 1);
        const std::array<std::uint64_t, 9> fill{string, 0, length, encoding, loss, 1, buffer, size, used};
        static_cast<void>(cf.invoke("CFStringGetBytes", fill));
        const std::array<std::uint64_t, 3> create{0, buffer, m.read64(used)};
        s.x[0] = cf.invoke("CFDataCreate", create);
        process.release_guest(buffer);
        process.release_guest(used);
    });

    // Boolean CFURLCreateDataAndPropertiesFromResource(CFAllocatorRef, CFURLRef,
    //     CFDataRef* data, CFDictionaryRef* properties, CFArrayRef desired, SInt32* errorCode):
    // the contents of a file URL, and the kCFURLFileExists / kCFURLFileLength
    // properties (the others are not reported).
    constant_string(process, cf, cf_library, "_kCFURLFileExists", "kCFURLFileExists");
    constant_string(process, cf, cf_library, "_kCFURLFileLength", "kCFURLFileLength");
    function(cf_library, "_CFURLCreateDataAndPropertiesFromResource", [&process, &cf](CpuState& s, GuestMemory& m) {
        const auto url = s.x[1], data_out = s.x[2], properties_out = s.x[3], desired = s.x[4], error_out = s.x[5];
        if (data_out) m.write64(data_out, 0);
        if (properties_out) m.write64(properties_out, 0);
        const auto fail = [&](std::int32_t code) { if (error_out) m.write32(error_out, static_cast<std::uint32_t>(code)); s.x[0] = 0; };
        if (!url) { fail(-15); return; }                               // kCFURLImproperArgumentsError
        const auto host = host_path(process, cf.url_path(url));
        std::error_code error;
        const bool exists = std::filesystem::exists(host, error);
        const bool directory = exists && std::filesystem::is_directory(host, error);
        const auto size = exists && !directory ? std::filesystem::file_size(host, error) : 0;
        if (data_out) {
            if (!exists) { fail(-12); return; }                        // kCFURLResourceNotFoundError
            if (directory) { fail(-10); return; }                      // kCFURLUnknownError
            std::ifstream file(host, std::ios::binary);
            if (!file) { fail(-13); return; }                          // kCFURLResourceAccessViolationError
            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
            if (size && !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) { fail(-13); return; }
            const auto buffer = process.allocate_guest(bytes.empty() ? 1 : bytes.size());
            if (!bytes.empty()) m.write_bytes(buffer, bytes);
            const std::array<std::uint64_t, 3> create{0, buffer, bytes.size()};
            m.write64(data_out, cf.invoke("CFDataCreate", create));
            process.release_guest(buffer);
        }
        if (properties_out) {
            const auto wanted = [&](std::string_view key) {
                if (!desired) return true;
                for (const auto value : cf.array_values(desired)) if (cf.utf8(value) == key) return true;
                return false;
            };
            std::vector<std::pair<std::uint64_t, std::uint64_t>> entries;
            std::vector<std::uint64_t> owned;
            if (wanted("kCFURLFileExists")) {
                const auto key = cf.string("kCFURLFileExists");
                owned.push_back(key);
                entries.emplace_back(key, cf.boolean(exists));
            }
            if (wanted("kCFURLFileLength") && exists && !directory) {
                const auto key = cf.string("kCFURLFileLength"), value = cf.integer(static_cast<std::int64_t>(size), 4);
                owned.push_back(key); owned.push_back(value);
                entries.emplace_back(key, value);
            }
            m.write64(properties_out, cf.dictionary(entries));
            for (const auto value : owned) process.objects().release(value);
        }
        if (error_out) m.write32(error_out, 0);
        s.x[0] = 1;
    });

    // __NSDictionary0__struct: the immortal empty dictionary that clang emits
    // for @{} literals.
    if (!process.resolve(cf_library, "___NSDictionary0__struct")) {
        const auto keys = process.resolve(cf_library, "_kCFTypeDictionaryKeyCallBacks");
        const auto values = process.resolve(cf_library, "_kCFTypeDictionaryValueCallBacks");
        const std::array<std::uint64_t, 6> arguments{0, 0, 0, 0, keys ? keys->address : 0, values ? values->address : 0};
        const auto empty = cf.invoke("CFDictionaryCreate", arguments);
        if (const auto record = process.objects().find(empty)) record->immortal = true;
        process.install_export(cf_library, "___NSDictionary0__struct", {empty, ExportKind::data});
    }
}

} // namespace alchemy::export_fill
