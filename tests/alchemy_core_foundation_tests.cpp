#include "alchemy/core_foundation.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

namespace {
void require(bool value, std::string_view reason) { if (!value) throw std::runtime_error(std::string(reason)); }
struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext context{engine, [](auto, auto bytes) { return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{context};
    alchemy::CoreFoundation cf{context};
    Fixture() { context.working_directory = std::filesystem::current_path(); }
    std::uint64_t call(std::string_view name, std::initializer_list<std::uint64_t> args) { return cf.invoke(name, std::span(args.begin(), args.size())); }
    std::uint64_t exported(std::string_view name) { const auto value = context.resolve(alchemy::core_foundation_library, name); require(value.has_value(), "missing CF export"); return value->address; }
    std::uint64_t input(std::span<const std::uint8_t> bytes) { const auto at = context.allocate_guest(bytes.size() + 1); engine.write_bytes(at, bytes); static_cast<transmute::GuestMemory&>(engine).write8(at + bytes.size(), 0); return at; }
    std::uint64_t input(std::string_view text) { return input(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())); }
    void release(std::uint64_t address) { static_cast<void>(call("CFRelease", {address})); }
    template<class F> void rejected(F operation, std::string_view reason) { bool result{}; try { operation(); } catch (const std::runtime_error&) { result = true; } require(result, reason); }
};

void strings_and_constants() {
    Fixture f;
    const auto text = f.cf.string("A\xf0\x9f\x98\x80\xc3\xa9");
    require(f.call("CFStringGetLength", {text}) == 4, "UTF-16 code unit length incorrect");
    require(f.call("CFStringGetCharacterAtIndex", {text, 1}) == 0xd83d && f.call("CFStringGetCharacterAtIndex", {text, 2}) == 0xde00, "supplementary scalar was not a surrogate pair");
    require(f.cf.utf8(text) == "A\xf0\x9f\x98\x80\xc3\xa9", "UTF-8 roundtrip incorrect");
    const auto output = f.context.allocate_guest(32), used = f.context.allocate_guest(8);
    f.engine.fill_bytes(output, 0xa5, 32);
    require(f.call("CFStringGetCString", {text, output, 7, 0x08000100}) == 0 && static_cast<transmute::GuestMemory&>(f.engine).read8(output) == 0xa5, "small C string buffer should fail without a prefix");
    require(f.call("CFStringGetCString", {text, output, 8, 0x08000100}) == 1 && static_cast<transmute::GuestMemory&>(f.engine).read8(output + 7) == 0, "exact C string capacity incorrect");
    require(f.call("CFStringGetBytes", {text, 0, 4, 0x08000100, 0, 0, output, 4, used}) == 1 && f.engine.read64(used) == 1, "partial encoding split a surrogate pair");
    require(f.call("CFStringGetBytes", {text, 0, 4, 0x600, '?', 0, output, 32, used}) == 4 && f.engine.read64(used) == 3, "ASCII lossy encoding did not consume complete scalar");
    const auto chars = f.call("CFStringGetCharactersPtr", {text}); require(chars && f.engine.read32(chars + 2) == 0xde00d83d, "UTF-16 pointer wrong");
    const auto stable = f.call("CFStringGetCStringPtr", {text, 0x08000100}); require(stable && stable == f.call("CFStringGetCStringPtr", {text, 0x08000100}), "C string pointer not stable");
    require(f.call("CFStringGetCStringPtr", {text, 0x600}) == 0, "ASCII pointer accepted non-ASCII text");
    const std::array<std::uint16_t, 2> unpaired{0xd800, 'x'}; const auto bad_surrogate = f.cf.string_utf16(unpaired);
    require(f.call("CFStringGetLength", {bad_surrogate}) == 2 && f.call("CFStringGetCString", {bad_surrogate, output, 32, 0x08000100}) == 0, "unpaired surrogate rules wrong");
    const std::array<std::uint8_t, 3> malformed{0xed, 0xa0, 0x80}; const auto invalid = f.input(malformed);
    require(f.call("CFStringCreateWithBytes", {0, invalid, 3, 0x08000100, 0}) == 0, "UTF-8 encoded surrogate accepted");
    const auto overlong = f.input(std::string_view("\xc0\x80", 2)); require(f.call("CFStringCreateWithBytes", {0, overlong, 2, 0x08000100, 0}) == 0, "overlong UTF-8 accepted");
    f.rejected([&] { static_cast<void>(f.call("CFStringGetCharacterAtIndex", {text, UINT64_MAX})); }, "negative index accepted");
    f.rejected([&] { static_cast<void>(f.call("CFStringCreateCopy", {17, text})); }, "custom allocator accepted");
    const auto constant = f.context.allocate_guest(32), content = f.input("constant");
    f.engine.write64(constant, f.exported("___CFConstantStringClassReference")); f.engine.write32(constant + 8, 0x7c8); f.engine.write64(constant + 16, content); f.engine.write64(constant + 24, 8);
    f.engine.set_memory_access(constant, true, false, false);
    require(f.call("CFStringGetLength", {constant}) == 8, "constant CF string not adopted");
    const auto class_address = f.context.allocate_guest(32); f.cf.set_class(alchemy::ObjectKind::string, class_address);
    require(f.context.objects().require(constant)->class_address.load() == class_address, "read-only constant class metadata not updated");
    f.release(constant); require(f.call("CFGetRetainCount", {constant}) == INT64_MAX, "constant string not immortal");
    f.release(text); f.release(bad_surrogate);
    require(!f.context.objects().find(text) && !f.engine.query_region(chars) && !f.engine.query_region(stable), "string buffers survived destruction");
}

void collections_and_ownership() {
    Fixture f;
    const auto key = f.cf.string("key"), equivalent = f.cf.string("key"), value = f.cf.string("value");
    const auto array = f.call("CFArrayCreateMutable", {0, 0, f.exported("_kCFTypeArrayCallBacks")});
    static_cast<void>(f.call("CFArrayAppendValue", {array, value})); require(f.call("CFGetRetainCount", {value}) == 2, "array did not retain element");
    f.rejected([&] { static_cast<void>(f.call("CFArraySetValueAtIndex", {array, 9, value})); }, "array range failure accepted");
    require(f.call("CFGetRetainCount", {value}) == 2, "failed array mutation leaked ownership");
    const auto dictionary = f.call("CFDictionaryCreateMutable", {0, 0, f.exported("_kCFTypeDictionaryKeyCallBacks"), f.exported("_kCFTypeDictionaryValueCallBacks")});
    const auto overflowing_key = f.cf.string("overflowing key");
    f.context.objects().require(overflowing_key)->references.store(UINT64_MAX);
    f.rejected([&] { static_cast<void>(f.call("CFDictionarySetValue", {dictionary, overflowing_key, value})); }, "dictionary accepted overflowing key ownership");
    f.context.objects().require(overflowing_key)->references.store(1);
    require(f.call("CFGetRetainCount", {value}) == 2 && f.call("CFDictionaryGetCount", {dictionary}) == 0, "failed key retain leaked value ownership or inserted entry");
    f.release(overflowing_key);
    static_cast<void>(f.call("CFDictionarySetValue", {dictionary, key, array})); require(f.call("CFGetRetainCount", {array}) == 2 && f.call("CFGetRetainCount", {key}) == 2, "dictionary did not retain members");
    require(f.call("CFDictionaryGetValue", {dictionary, equivalent}) == array, "dictionary does not use CFEqual for keys");
    static_cast<void>(f.call("CFDictionaryAddValue", {dictionary, equivalent, value})); require(f.call("CFDictionaryGetValue", {dictionary, key}) == array, "add replaced an existing key");
    const auto snapshot = f.call("CFDictionaryCreateCopy", {0, dictionary}); require(f.cf.equal(snapshot, dictionary) && f.cf.hash(snapshot) == f.cf.hash(dictionary), "dictionary copy equality/hash wrong");
    f.rejected([&] { static_cast<void>(f.call("CFDictionarySetValue", {snapshot, key, value})); }, "immutable dictionary mutated");
    static_cast<void>(f.call("CFDictionaryReplaceValue", {dictionary, equivalent, value})); require(f.call("CFDictionaryGetValue", {dictionary, key}) == value, "replace did not update value");
    f.release(array); require(f.context.objects().find(array) != nullptr, "copy did not hold original array");
    f.release(snapshot); require(!f.context.objects().find(array), "nested collection destruction did not release array");
    const auto plain = f.call("CFArrayCreateMutable", {0, 0, 0}); static_cast<void>(f.call("CFArrayAppendValue", {plain, 0x1234})); require(f.call("CFArrayGetValueAtIndex", {plain, 0}) == 0x1234, "null callback array should accept opaque pointers");
    const auto custom = f.context.allocate_guest(40); f.engine.write64(custom + 8, 1);
    f.rejected([&] { static_cast<void>(f.call("CFArrayCreateMutable", {0, 0, custom})); }, "custom callback accepted");
    f.release(plain); f.release(dictionary); f.release(key); f.release(equivalent); f.release(value);
    require(f.context.objects().live_count() == 0, "collections leaked objects");
}

void recursive_keys_and_concurrent_mutations() {
    Fixture f;
    const auto key_callbacks = f.exported("_kCFTypeDictionaryKeyCallBacks"), value_callbacks = f.exported("_kCFTypeDictionaryValueCallBacks");
    const auto value = f.cf.string("value"), recursive = f.call("CFDictionaryCreateMutable", {0, 0, key_callbacks, value_callbacks});
    // A dictionary can appear as its own key. Comparing an equivalent key must
    // never recurse while holding that dictionary's mutation lock.
    static_cast<void>(f.call("CFDictionarySetValue", {recursive, recursive, value}));
    const auto equivalent = f.call("CFDictionaryCreateCopy", {0, recursive});
    static_cast<void>(f.call("CFDictionarySetValue", {recursive, equivalent, value}));
    require(f.call("CFDictionaryGetCount", {recursive}) == 1, "equivalent collection key was duplicated");
    static_cast<void>(f.call("CFDictionaryRemoveValue", {recursive, equivalent}));
    require(f.call("CFDictionaryGetCount", {recursive}) == 0, "equivalent recursive key removal failed");
    f.release(equivalent); f.release(recursive);
    const auto dictionary = f.call("CFDictionaryCreateMutable", {0, 0, key_callbacks, value_callbacks});
    std::array<std::uint64_t, 64> keys{};
    for (std::size_t at = 0; at < keys.size(); ++at) keys[at] = f.cf.string("key" + std::to_string(at));
    std::array<std::exception_ptr, 2> errors{};
    auto worker = [&](unsigned identity) {
        try { for (std::size_t at = identity * 32; at < (identity + 1) * 32; ++at) static_cast<void>(f.call("CFDictionarySetValue", {dictionary, keys[at], value})); }
        catch (...) { errors[identity] = std::current_exception(); }
    };
    std::thread first(worker, 0), second(worker, 1); first.join(); second.join();
    for (auto error : errors) if (error) std::rethrow_exception(error);
    require(f.call("CFDictionaryGetCount", {dictionary}) == 64, "concurrent dictionary commits lost values");
    f.release(dictionary); for (auto key : keys) f.release(key); f.release(value);
    require(f.context.objects().live_count() == 0, "recursive/concurrent dictionary test leaked objects");
}

void data_numbers_and_urls() {
    Fixture f;
    const std::array<std::uint8_t, 4> raw{0, 27, 255, 42}; const auto input = f.input(raw);
    const auto data = f.call("CFDataCreateMutable", {0, 0}); static_cast<void>(f.call("CFDataAppendBytes", {data, input, raw.size()}));
    const auto pointer = f.call("CFDataGetMutableBytePtr", {data}); static_cast<transmute::GuestMemory&>(f.engine).write8(pointer + 1, 99);
    const auto copied = f.call("CFDataCreateCopy", {0, data}); require(f.cf.equal(data, copied), "mutable byte pointer writes ignored by copy");
    static_cast<void>(f.call("CFDataSetLength", {data, 8})); const auto enlarged = f.call("CFDataGetBytePtr", {data}); require(static_cast<transmute::GuestMemory&>(f.engine).read8(enlarged + 1) == 99 && static_cast<transmute::GuestMemory&>(f.engine).read8(enlarged + 7) == 0, "data resize lost bytes or zero fill");
    f.rejected([&] { static_cast<void>(f.call("CFDataAppendBytes", {copied, input, 1})); }, "immutable data mutated");
    const auto cell = f.context.allocate_guest(16), out = f.context.allocate_guest(8);
    f.engine.write64(cell, INT64_MAX); const auto integer = f.call("CFNumberCreate", {0, 4, cell});
    require(f.call("CFNumberGetValue", {integer, 4, out}) == 1 && f.engine.read64(out) == INT64_MAX, "64-bit integer conversion lost precision");
    require(f.call("CFNumberGetValue", {integer, 3, out}) == 0 && f.engine.read32(out) == INT32_MAX, "overflow conversion did not report loss/clamp");
    require(f.call("CFNumberGetValue", {integer, 6, out}) == 0, "rounded integer-to-double claimed exactness");
    f.engine.write64(cell, std::bit_cast<std::uint64_t>(0x1p63)); const auto floating = f.call("CFNumberCreate", {0, 6, cell});
    require(f.call("CFNumberGetValue", {floating, 4, out}) == 0 && f.engine.read64(out) == INT64_MAX, "double at signed boundary overflowed");
    require(!f.cf.equal(integer, floating) && f.call("CFNumberCompare", {integer, floating, 0}) == UINT64_MAX, "integer/double ordering lost precision");
    f.engine.write64(cell, 27); const auto small = f.call("CFNumberCreate", {0, 4, cell}); f.engine.write64(cell, std::bit_cast<std::uint64_t>(27.0)); const auto same = f.call("CFNumberCreate", {0, 6, cell}); require(f.cf.equal(small, same) && f.cf.hash(small) == f.cf.hash(same), "equal numeric values have different hashes");
    require(f.call("CFBooleanGetValue", {f.engine.read64(f.exported("_kCFBooleanTrue"))}) == 1, "boolean constant layout wrong");
    const auto nan = f.engine.read64(f.exported("_kCFNumberNaN")), positive_infinity = f.engine.read64(f.exported("_kCFNumberPositiveInfinity")), negative_infinity = f.engine.read64(f.exported("_kCFNumberNegativeInfinity"));
    require(f.call("CFNumberCompare", {nan, positive_infinity, 0}) == UINT64_MAX && f.call("CFNumberCompare", {nan, negative_infinity, 0}) == 1 && f.call("CFNumberCompare", {nan, nan, 0}) == 0, "Darwin NaN ordering incorrect");
    f.engine.write64(cell, std::bit_cast<std::uint64_t>(-0.0)); const auto negative_zero = f.call("CFNumberCreate", {0, 6, cell});
    f.engine.write64(cell, std::bit_cast<std::uint64_t>(0.0)); const auto positive_zero = f.call("CFNumberCreate", {0, 6, cell});
    require(f.call("CFNumberCompare", {negative_zero, positive_zero, 0}) == UINT64_MAX, "signed zero comparison incorrect");
    const auto pathname = f.cf.string("relative file.txt"), url = f.call("CFURLCreateWithFileSystemPath", {0, pathname, 0, 0});
    const auto result = f.call("CFURLCopyFileSystemPath", {url, 0}); require(f.cf.utf8(result) == alchemy::guest_path_string((f.context.working_directory / "relative file.txt").lexically_normal()), "file URL path mapper disagreement");
    const auto url_string = f.call("CFURLGetString", {url}); require(f.cf.utf8(url_string).find("relative%20file.txt") != std::string::npos, "URL did not encode spaces");
    f.rejected([&] { static_cast<void>(f.call("CFURLCopyFileSystemPath", {url, 2})); }, "unsupported URL style accepted");
    for (auto object : {data, copied, integer, floating, small, same, negative_zero, positive_zero, pathname, result, url}) f.release(object);
    require(!f.context.objects().find(url_string) && f.context.objects().live_count() == 0, "data/number/URL destruction leaked");
}
}
void concurrent_value_creation() {
    Fixture f;
    const auto name = f.cf.string("concurrent file.txt");
    const auto expected = alchemy::guest_path_string((f.context.working_directory / "concurrent file.txt").lexically_normal());
    std::array<std::exception_ptr, 2> errors{};
    const auto worker = [&](unsigned identity) {
        try {
            for (unsigned index = 0; index < 16; ++index) {
                const auto url = f.call("CFURLCreateWithFileSystemPath", {0, name, 0, 0});
                const auto path = f.call("CFURLCopyFileSystemPath", {url, 0});
                require(f.cf.utf8(path) == expected, "concurrent URL creation changed its path");
                f.release(path); f.release(url);
            }
        } catch (...) { errors[identity] = std::current_exception(); }
    };
    std::thread first(worker, 0), second(worker, 1); first.join(); second.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    f.release(name);
    require(f.context.objects().live_count() == 0, "concurrent value creation leaked objects");
}
void locales_dates_and_literals() {
    Fixture f;
    const auto current = f.call("CFLocaleCopyCurrent", {});
    const auto identifier = f.cf.utf8(f.call("CFLocaleGetIdentifier", {current}));
    const auto language = f.call("CFLocaleGetValue", {current, f.engine.read64(f.exported("_kCFLocaleLanguageCode"))});
    require(!identifier.empty() && language && identifier.starts_with(f.cf.utf8(language)), "current locale lacks a language identifier");
    require(f.call("CFGetTypeID", {current}) == f.call("CFLocaleGetTypeID", {}), "CFLocale type identifier mismatch");
    f.release(current); f.release(current); // Copy-rule references of the process locale are balanced.
    const auto name = f.cf.string("zh-Hans-CN"), created = f.call("CFLocaleCreate", {0, name});
    require(f.cf.utf8(f.call("CFLocaleGetIdentifier", {created})) == "zh_Hans_CN", "locale identifier was not canonicalized");
    require(f.cf.utf8(f.call("CFLocaleGetValue", {created, f.engine.read64(f.exported("_kCFLocaleScriptCode"))})) == "Hans" &&
            f.cf.utf8(f.call("CFLocaleGetValue", {created, f.engine.read64(f.exported("_kCFLocaleCountryCode"))})) == "CN", "locale components incorrect");
    const auto us = f.cf.locale("en_US"), metric = f.engine.read64(f.exported("_kCFLocaleUsesMetricSystem"));
    require(f.call("CFLocaleGetValue", {us, metric}) == f.engine.read64(f.exported("_kCFBooleanFalse")) &&
            f.cf.utf8(f.call("CFLocaleGetValue", {us, f.engine.read64(f.exported("_kCFLocaleDecimalSeparator"))})) == ".", "en_US measurement or decimal separator wrong");
    const auto languages = f.call("CFLocaleCopyPreferredLanguages", {});
    require(f.call("CFArrayGetCount", {languages}) == 1 && f.cf.utf8(f.call("CFArrayGetValueAtIndex", {languages, 0})).find('_') == std::string::npos, "preferred languages are not BCP 47");
    const auto portuguese = f.cf.string("pt-br"), canonical = f.call("CFLocaleCreateCanonicalLocaleIdentifierFromString", {0, portuguese});
    require(f.cf.utf8(canonical) == "pt_BR", "canonical locale identifier wrong");
    f.release(f.engine.read64(f.context.resolve(alchemy::core_foundation_library, "_kCFLocaleIdentifier")->address)); // Immortal constant.
    // __kCFBooleanTrue is the boolean object; kCFBooleanTrue points at it.
    require(f.exported("___kCFBooleanTrue") == f.engine.read64(f.exported("_kCFBooleanTrue")), "__kCFBooleanTrue is not the boolean object");
    require(f.cf.utf8(f.engine.read64(f.exported("_kCFBundleVersionKey"))) == "CFBundleVersion" &&
            f.cf.utf8(f.engine.read64(f.exported("_NSDefaultRunLoopMode"))) == "kCFRunLoopDefaultMode" &&
            f.engine.read64(f.exported("_NSDefaultRunLoopMode")) == f.engine.read64(f.exported("_kCFRunLoopDefaultMode")), "CF string constants wrong");
    const auto epoch = f.cf.date(0), later = f.cf.date(86400.5);
    const auto epoch_text = f.call("CFCopyDescription", {epoch});
    require(f.cf.utf8(epoch_text) == "2001-01-01 00:00:00 +0000", "CFDate description is not UTC reference time");
    f.release(epoch_text);
    require(f.call("CFDateCompare", {epoch, later, 0}) == UINT64_MAX && f.cf.date_time(later) == 86400.5, "CFDate ordering wrong");
    const auto now = alchemy::CoreFoundation::absolute_time_now(); require(now > 7e8 && now < 4e9, "CFAbsoluteTime is not relative to 2001");
    // Latin-1 and stable inner pointers per encoding.
    const auto latin = f.input(std::string_view("caf\xe9", 4));
    const auto cafe = f.call("CFStringCreateWithCString", {0, latin, 0x201});
    require(f.cf.utf8(cafe) == "caf\xc3\xa9", "Latin-1 decoding wrong");
    const auto pointer = f.cf.c_string(cafe, 0x201); require(pointer && pointer == f.cf.c_string(cafe, 0x201) && static_cast<transmute::GuestMemory&>(f.engine).read8(pointer + 3) == 0xe9, "Latin-1 inner pointer wrong");
    require(f.cf.c_string(cafe, 0x600) == 0, "ASCII inner pointer accepted non-ASCII text");
    // Clang constant literals are adopted from guest records with the registered classes.
    const auto integer_class = f.context.allocate_guest(64), dictionary_class = f.context.allocate_guest(64), array_class = f.context.allocate_guest(64);
    f.cf.set_literal_class(alchemy::LiteralClass::integer, integer_class); f.cf.set_literal_class(alchemy::LiteralClass::dictionary, dictionary_class);
    f.cf.set_literal_class(alchemy::LiteralClass::array, array_class);
    const auto encoding = f.input("i"), literal = f.context.allocate_guest(24);
    f.engine.write64(literal, integer_class); f.engine.write64(literal + 8, encoding); f.engine.write64(literal + 16, static_cast<std::uint64_t>(-27));
    const auto key_text = f.input("answer"), key = f.context.allocate_guest(32);
    f.engine.write64(key, f.exported("___CFConstantStringClassReference")); f.engine.write32(key + 8, 0x7c8); f.engine.write64(key + 16, key_text); f.engine.write64(key + 24, 6);
    const auto keys = f.context.allocate_guest(8), values = f.context.allocate_guest(8), dictionary = f.context.allocate_guest(40), array = f.context.allocate_guest(24);
    f.engine.write64(keys, key); f.engine.write64(values, literal);
    f.engine.write64(dictionary, dictionary_class); f.engine.write64(dictionary + 8, 1); f.engine.write64(dictionary + 16, 1); f.engine.write64(dictionary + 24, keys); f.engine.write64(dictionary + 32, values);
    f.engine.write64(array, array_class); f.engine.write64(array + 8, 1); f.engine.write64(array + 16, values);
    const auto lookup = f.cf.string("answer"), out = f.context.allocate_guest(8);
    require(f.call("CFDictionaryGetValue", {dictionary, lookup}) == literal, "constant dictionary lookup failed");
    require(f.call("CFNumberGetValue", {literal, 4, out}) == 1 && f.engine.read64(out) == static_cast<std::uint64_t>(-27) && f.call("CFNumberGetType", {literal}) == 9, "constant integer value or type wrong");
    require(f.call("CFArrayGetCount", {array}) == 1 && f.call("CFArrayGetValueAtIndex", {array, 0}) == literal, "constant array contents wrong");
    require(f.context.objects().require(literal)->class_address.load() == integer_class, "constant literal lost its class");
    f.rejected([&] { static_cast<void>(f.call("CFDictionarySetValue", {dictionary, lookup, literal})); }, "constant dictionary was mutable");
    static_cast<transmute::GuestMemory&>(f.engine).write8(encoding, 'x'); const auto bad = f.context.allocate_guest(24); f.engine.write64(bad, integer_class); f.engine.write64(bad + 8, encoding);
    f.rejected([&] { static_cast<void>(f.call("CFNumberGetType", {bad})); }, "unsupported literal encoding accepted");
    for (const auto object : {created, name, us, languages, portuguese, canonical, epoch, later, cafe, lookup}) f.release(object);
    require(f.context.objects().live_count() == 0, "locale/date/literal objects leaked");
}

void url_resources_and_volumes() {
    Fixture f;
    const auto root = std::filesystem::temp_directory_path() / ("alchemy-cf-resources-" + std::to_string(std::random_device{}()));
    std::filesystem::create_directories(root / "folder");
    { std::ofstream(root / "visible.txt", std::ios::binary) << "hello"; std::ofstream(root / ".hidden") << "x"; }
    const auto guest = alchemy::guest_path_string(root);
    const auto key = [&](std::string_view name) { return f.engine.read64(f.exported("_NSURL" + std::string(name))); };
    const auto value = f.context.allocate_guest(8);
    const auto property = [&](std::uint64_t url, std::string_view name) {
        f.engine.write64(value, 0xdead);
        const auto ok = f.call("CFURLCopyResourcePropertyForKey", {url, key(name), value, 0});
        return std::pair{ok, f.engine.read64(value)};
    };
    const auto truth = f.engine.read64(f.exported("_kCFBooleanTrue")), falsity = f.engine.read64(f.exported("_kCFBooleanFalse"));
    const auto file = f.cf.file_url(guest + "/visible.txt", false), hidden = f.cf.file_url(guest + "/.hidden", false), folder = f.cf.file_url(guest + "/folder", true);
    const auto missing = f.cf.file_url(guest + "/missing", false);
    require(property(file, "IsDirectoryKey") == std::pair{std::uint64_t{1}, falsity} && property(folder, "IsDirectoryKey") == std::pair{std::uint64_t{1}, truth}, "directory property wrong");
    require(property(hidden, "IsHiddenKey").second == truth && property(file, "IsHiddenKey").second == falsity, "hidden property wrong");
    require(property(file, "IsReadableKey").second == truth && property(file, "IsWritableKey").second == truth && property(file, "IsAliasFileKey").second == falsity, "access properties wrong");
    const auto [sized, size] = property(file, "FileSizeKey");
    const auto bytes = f.context.allocate_guest(8);
    require(sized == 1 && f.call("CFNumberGetValue", {size, 4, bytes}) == 1 && f.engine.read64(bytes) == 5, "file size property wrong");
    f.release(size);
    const auto [named, file_name] = property(file, "NameKey"); require(named == 1 && f.cf.utf8(file_name) == "visible.txt", "name property wrong"); f.release(file_name);
    const auto [volume_named, volume_name] = property(file, "VolumeLocalizedNameKey");
    require(volume_named == 1 && volume_name && !f.cf.utf8(volume_name).empty(), "volume name missing"); f.release(volume_name);
    require(property(file, "VolumeIsLocalKey").second == truth, "temporary directory volume is not local");
    require(f.engine.read64(f.exported("_kCFURLVolumeIsEjectableKey")) == key("VolumeIsEjectableKey"), "CF and NS URL keys differ");
    require(property(missing, "IsDirectoryKey") == std::pair{std::uint64_t{0}, std::uint64_t{0}}, "unreachable item reported a property");
    const auto unknown = f.cf.string("NSURLNoSuchPropertyKey");
    f.engine.write64(value, 1); require(f.call("CFURLCopyResourcePropertyForKey", {file, unknown, value, 0}) == 1 && f.engine.read64(value) == 0, "unavailable property was not NULL");
    const auto enumerator = f.call("CFURLEnumeratorCreateForMountedVolumes", {0, 2, 0});
    std::vector<std::string> volumes; const auto next = f.context.allocate_guest(8);
    for (std::uint64_t result; (result = f.call("CFURLEnumeratorGetNextURL", {enumerator, next, 0})) != 2;) {
        require(result == 1, "volume enumeration failed");
        const auto url = f.engine.read64(next); volumes.push_back(f.cf.url_path(url));
        require(f.call("CFURLHasDirectoryPath", {url}) == 1, "volume URL is not a directory");
    }
    require(!volumes.empty() && f.call("CFURLEnumeratorGetNextURL", {enumerator, next, 0}) == 2, "volume enumeration produced no volumes or restarted");
#ifdef _WIN32
    require(std::all_of(volumes.begin(), volumes.end(), [](const std::string& path) { return path.size() == 10 && path.starts_with("/Volumes/"); }), "host drives are not /Volumes/<letter>");
#endif
    for (const auto object : {file, hidden, folder, missing, unknown, enumerator}) f.release(object);
    std::filesystem::remove_all(root);
    require(f.context.objects().live_count() == 0, "URL resource objects leaked");
}

int main() {
    try { strings_and_constants(); collections_and_ownership(); recursive_keys_and_concurrent_mutations(); data_numbers_and_urls(); concurrent_value_creation();
        locales_dates_and_literals(); url_resources_and_volumes(); std::cout << "Alchemy CoreFoundation tests passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
