#include "alchemy/darwin_services.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/path.hpp"
#include "transmute/jit.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <filesystem>
#include <fstream>
#include <random>
#include <exception>
#include <iostream>
#include <thread>

namespace {
using Address = std::uint64_t;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Operation> void rejected(Operation operation, std::string_view diagnostic) {
    try { operation(); }
    catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).find(diagnostic) != std::string_view::npos, "incorrect Foundation failure diagnostic");
        return;
    }
    throw std::runtime_error("unsupported Foundation behavior was accepted");
}
struct Fixture {
    std::string errors;
    std::size_t chunk{SIZE_MAX};
    std::uint32_t output_error{};
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [this](unsigned descriptor, std::span<const std::uint8_t> bytes) {
        require(descriptor == 2, "NSLog did not use stderr");
        const auto size = output_error ? 0 : std::min(chunk, bytes.size());
        errors.append(reinterpret_cast<const char*>(bytes.data()), size);
        return alchemy::OutputResult{size, output_error};
    }};
    alchemy::DarwinServices services{process};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    std::unique_ptr<alchemy::FoundationBridge> foundation;
    std::vector<Address> inputs;
    Fixture() {
        process.executable = std::filesystem::path("C:/Alchemy/fixture");
        process.arguments = {"/Volumes/C/Alchemy/fixture", "27", "Unicode \xc3\xa9"};
        process.environment = {"ALCHEMY=27", "EMPTY=", "ALCHEMY=second", "malformed", "=hidden=ignored"};
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
    }
    ~Fixture() {
        clear_inputs();
        objc.cleanup_thread(*process.thread(engine));
    }
    void clear_inputs() { for (const auto value : inputs) process.objects().release(value); inputs.clear(); }
    Address string(std::string_view text) { const auto value = cf.string(text); inputs.push_back(value); return value; }
    Address invoke(transmute::ReferenceEngine& worker, std::string_view symbol, std::initializer_list<Address> arguments,
                   std::string_view library = alchemy::objc_library) {
        const auto exported = process.resolve(library, symbol);
        require(exported.has_value(), "missing Foundation test service");
        return worker.invoke_callback(exported->address, std::span(arguments.begin(), arguments.size()));
    }
    Address send(Address object, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        std::vector<Address> values{object, objc.selector(selector)};
        values.insert(values.end(), arguments.begin(), arguments.end());
        return engine.invoke_callback(process.resolve(alchemy::objc_library, "_objc_msgSend")->address, values);
    }
    Address pool() { return send(send(objc.find_class("NSAutoreleasePool"), "alloc"), "init"); }
};

void process_information(bool jit) {
    Fixture f;
    f.engine.set_jit_enabled(jit); f.engine.set_jit_profiling(jit);
    const auto pool = f.pool();
    const auto info = f.send(f.objc.find_class("NSProcessInfo"), "processInfo");
    require(info == f.send(f.objc.find_class("NSProcessInfo"), "processInfo"), "NSProcessInfo singleton changed identity");
    require(f.cf.utf8(f.send(info, "processName")) == "fixture", "incorrect Foundation process name");
    require(f.send(info, "processIdentifier") == f.invoke(f.engine, "_getpid", {}, "/usr/lib/libSystem.B.dylib"),
            "Foundation process identifier disagrees with Darwin");
    const auto arguments = f.send(info, "arguments"), environment = f.send(info, "environment");
    require(f.cf.length(arguments) == 3 && f.cf.utf8(f.cf.element(arguments, 0)) == f.process.arguments[0] &&
            f.cf.utf8(f.cf.element(arguments, 2)) == "Unicode \xc3\xa9", "Foundation arguments lost argv[0] or Unicode");
    const auto key = f.string("ALCHEMY"), empty = f.string("EMPTY");
    require(f.cf.length(environment) == 2 && f.cf.utf8(f.cf.dictionary_value(environment, key)) == "27" &&
            f.cf.utf8(f.cf.dictionary_value(environment, empty)).empty(), "Foundation environment dictionary mismatch");
    f.process.environment[0] = "ALCHEMY=changed";
    require(f.cf.utf8(f.cf.dictionary_value(f.send(info, "environment"), key)) == "27", "Foundation initial environment snapshot changed");
    require(f.send(info, "processorCount") >= 1 && f.send(info, "activeProcessorCount") >= 1, "invalid Foundation processor counts");
    const auto version = f.process.allocate_guest(24);
    f.engine.set_initial_register(8, version);
    static_cast<void>(f.send(info, "operatingSystemVersion"));
    require(f.engine.read64(version) == 14 && !f.engine.read64(version + 8) && !f.engine.read64(version + 16),
            "Foundation OS version indirect return ABI mismatch");
    require(f.send(info, "isOperatingSystemAtLeastVersion:", {version}) == 1, "Foundation same-version comparison failed");
    f.engine.write64(version + 16, 1);
    require(f.send(info, "isOperatingSystemAtLeastVersion:", {version}) == 0, "Foundation newer-version comparison failed");
    f.engine.write64(version, 10); f.engine.write64(version + 8, 99);
    require(f.send(info, "isOperatingSystemAtLeastVersion:", {version}) == 1, "Foundation version comparison is not lexicographic");
    f.engine.write64(version, UINT64_MAX);
    rejected([&] { static_cast<void>(f.send(info, "isOperatingSystemAtLeastVersion:", {version})); }, "nonnegative");
    require(f.cf.utf8(f.send(info, "operatingSystemVersionString")) == "Alchemy macOS 14.0.0 compatibility profile",
            "Foundation OS profile string is misleading");
    f.process.release_guest(version);
    const auto short_output = f.process.allocate_guest(16);
    f.engine.fill_bytes(short_output, 0xa5, 16);
    f.engine.set_initial_register(8, short_output);
    rejected([&] { f.send(info, "operatingSystemVersion"); }, "not writable");
    require(f.engine.read64(short_output) == 0xa5a5a5a5a5a5a5a5ULL &&
            f.engine.read64(short_output + 8) == 0xa5a5a5a5a5a5a5a5ULL,
            "Foundation invalid OS-version output partially overwrote guest memory");
    f.process.release_guest(short_output);
    for (const auto* name : {"NSArray", "NSDictionary"}) {
        const auto alias = f.process.resolve(alchemy::core_foundation_library, "_OBJC_CLASS_$_" + std::string(name));
        require(alias && alias->address == f.objc.find_class(name), "CoreFoundation public class alias differs from the authored class");
    }
    f.clear_inputs(); f.send(pool, "drain");
    require(f.process.objects().live_count() == 0, "Foundation process property snapshots leaked");
    if (jit) require(f.engine.jit_stats().native_guest_instructions > 0, "Foundation messages did not execute translated blocks");
}

void pool_lifecycle() {
    Fixture f;
    std::atomic<unsigned> destroyed{};
    const auto probe = f.objc.register_native_class("FoundationUnitProbe", f.objc.find_class("NSObject"));
    static_cast<void>(f.objc.add_native_method(probe, "dealloc", [&](auto&, auto&) { ++destroyed; }, false, "v@:"));
    f.send(f.send(f.objc.find_class("NSAutoreleasePool"), "alloc"), "release");
    const auto outer = f.pool();
    f.send(f.objc.allocate(probe), "autorelease");
    const auto inner = f.pool();
    f.send(f.objc.allocate(probe), "autorelease");
    rejected([&] { f.send(outer, "drain"); }, "nesting order");
    rejected([&] { f.send(inner, "retain"); }, "cannot be retained");
    rejected([&] { f.send(inner, "autorelease"); }, "cannot be retained");
    rejected([&] { f.send(inner, "init"); }, "initialized twice");
    f.send(inner, "release"); require(destroyed == 1, "Foundation inner pool did not drain independently");
    const auto send = f.process.resolve(alchemy::objc_library, "_objc_msgSend")->address;
    auto first = f.engine.fork_thread(send, 0), second = f.engine.fork_thread(send, 0);
    const auto alloc = f.objc.selector("alloc"), init = f.objc.selector("init"), drain = f.objc.selector("drain");
    const auto autorelease = f.process.resolve(alchemy::objc_library, "_objc_autorelease")->address;
    const auto pool_class = f.objc.find_class("NSAutoreleasePool");
    const auto run = [&](transmute::ReferenceEngine& worker) {
        const std::array<Address, 2> allocate{pool_class, alloc};
        const auto object = worker.invoke_callback(send, allocate);
        const std::array<Address, 2> initialize{object, init};
        static_cast<void>(worker.invoke_callback(send, initialize));
        const auto value = f.objc.allocate(probe);
        const std::array<Address, 1> queue{value};
        static_cast<void>(worker.invoke_callback(autorelease, queue));
        const std::array<Address, 2> release{object, drain};
        static_cast<void>(worker.invoke_callback(send, release));
        require(f.process.thread(worker)->autorelease_pools.empty(), "worker retained its Foundation pool");
    };
    std::array<std::exception_ptr, 2> failures{};
    std::thread a([&] { try { run(*first); } catch (...) { failures[0] = std::current_exception(); } });
    std::thread b([&] { try { run(*second); } catch (...) { failures[1] = std::current_exception(); } });
    a.join(); b.join();
    for (const auto& failure : failures) if (failure) std::rethrow_exception(failure);
    require(destroyed == 3, "Foundation worker pools affected the main-thread pool");
    const std::array<Address, 2> wrong_thread{outer, drain};
    rejected([&] { first->invoke_callback(send, wrong_thread); }, "another thread");
    f.process.release_thread(*first); f.process.release_thread(*second);
    f.send(outer, "drain");
    require(destroyed == 4 && f.process.objects().live_count() == 0, "Foundation pool lifecycle leaked guest objects");
}

void logging() {
    Fixture f;
    const auto pool = f.pool();
    const auto format = f.string("literal %% %@ %s %d %ld %lld %@"), object = f.string("Alchemy \xc3\xa9");
    const auto c_string = f.process.allocate_guest(16);
    f.engine.write_bytes(c_string, std::array<std::uint8_t, 3>{'o', 'k', 0});
    const auto arguments = f.process.allocate_guest(64);
    const std::array<Address, 7> values{object, c_string, static_cast<Address>(-27LL), static_cast<Address>(INT64_MIN), 42, 0, 0};
    for (std::size_t index = 0; index < 6; ++index) f.engine.write64(arguments + index * 8, values[index]);
    f.chunk = 3;
    f.invoke(f.engine, "_NSLogv", {format, arguments}, alchemy::foundation_library);
    require(f.errors == "literal % Alchemy \xc3\xa9 ok -27 -9223372036854775808 42 (null)\n", "checked NSLogv formatting or partial output failed");
    f.errors.clear();
    const auto stack = (f.engine.state().sp - 256) & ~15ULL;
    f.engine.write64(stack, object); f.engine.write64(stack + 8, 27);
    f.invoke(f.engine, "_NSLog", {f.string("%@ %d")}, alchemy::foundation_library);
    require(f.errors == "Alchemy \xc3\xa9 27\n", "NSLog did not decode Apple ARM64 stack variadic arguments");
    f.errors.clear();
    f.invoke(f.engine, "_NSLogv", {f.string("one newline\n"), 0}, alchemy::foundation_library);
    require(f.errors == "one newline\n", "NSLog duplicated a trailing newline");
    f.errors.clear();
    for (const auto* unsupported : {"%n", "%f", "%1$d", "%zu", "%", "%lldx%"}) {
        rejected([&] { f.invoke(f.engine, "_NSLogv", {f.string(unsupported), 0}, alchemy::foundation_library); }, "Foundation NSLog");
        require(f.errors.empty(), "invalid NSLog formatting produced partial output");
    }
    f.engine.write64(arguments, 1);
    rejected([&] { f.invoke(f.engine, "_NSLogv", {f.string("%s"), arguments}, alchemy::foundation_library); }, "memory");
    require(f.errors.empty(), "invalid guest log pointer produced output");
    f.output_error = 5;
    rejected([&] { f.invoke(f.engine, "_NSLogv", {f.string("write fails"), 0}, alchemy::foundation_library); }, "output error 5");
    require(f.engine.read32(f.process.thread(f.engine)->errno_address) == 5, "NSLog output error did not update Darwin errno");
    f.output_error = 0;
    rejected([&] { f.invoke(f.engine, "_NSLogv", {f.string(std::string(65537, 'x')), 0}, alchemy::foundation_library); }, "65536");
    f.process.release_guest(arguments); f.process.release_guest(c_string);
    f.clear_inputs(); f.send(pool, "drain");
    require(f.process.objects().live_count() == 0, "NSLog descriptions or rejected calls leaked objects");
}
void strings_collections_and_sets() {
    Fixture f;
    const auto pool = f.pool();
    const auto cls = [&](const char* name) { const auto value = f.objc.find_class(name); require(value != 0, "missing Foundation class"); return value; };
    // -[NSString mutableCopy] answers a mutable copy (it used to answer an immutable one).
    const auto copied = f.string("copy me");
    const auto mutable_copy = f.send(copied, "mutableCopy");
    require(mutable_copy != copied && f.cf.is_mutable(mutable_copy) && !f.cf.is_mutable(copied) && f.cf.utf8(mutable_copy) == "copy me",
            "-[NSString mutableCopy] did not answer a mutable copy");
    f.process.objects().release(mutable_copy);
    // stringWithFormat: reads Apple ARM64 variadic stack slots, including doubles.
    const auto stack = (f.engine.state().sp - 256) & ~15ULL;
    const auto c_string = f.process.allocate_guest(16);
    f.engine.write_bytes(c_string, std::array<std::uint8_t, 3>{'o', 'k', 0});
    const std::array<Address, 6> values{f.string("obj"), static_cast<Address>(-5), std::bit_cast<Address>(2.5), c_string, c_string, 255};
    for (std::size_t index = 0; index < values.size(); ++index) f.engine.write64(stack + index * 8, values[index]);
    const auto formatted = f.send(cls("NSString"), "stringWithFormat:", {f.string("%@_%d|%.2f|%s|%4.1s|%x%%")});
    require(f.cf.utf8(formatted) == "obj_-5|2.50|ok|   o|ff%", "stringWithFormat: conversion mismatch");
    rejected([&] { f.send(cls("NSString"), "stringWithFormat:", {f.string("%n")}); }, "unsupported format conversion");
    const auto path = f.string("~/a//b/./c/../d.txt/");
    require(f.cf.utf8(f.send(path, "lastPathComponent")) == "d.txt" && f.cf.utf8(f.send(path, "pathExtension")) == "txt", "path component methods wrong");
    const auto standardized = f.cf.utf8(f.send(path, "stringByStandardizingPath"));
    require(!standardized.starts_with('~') && standardized.ends_with("/a/b/d.txt"), "stringByStandardizingPath did not expand and normalize");
    require(f.cf.utf8(f.send(f.string("/x/y"), "stringByAppendingPathComponent:", {f.string("z")})) == "/x/y/z", "path append wrong");
    const auto latin = f.send(f.string("caf\xc3\xa9"), "cStringUsingEncoding:", {5});
    require(latin && static_cast<transmute::GuestMemory&>(f.engine).read8(latin + 3) == 0xe9 && f.send(f.string("caf\xc3\xa9"), "cStringUsingEncoding:", {1}) == 0, "cStringUsingEncoding: wrong");
    const auto buffer = f.process.allocate_guest(8);
    require(f.send(f.string("toolong"), "getCString:maxLength:encoding:", {buffer, 4, 4}) == 0 && f.send(f.string("abc"), "getCString:maxLength:encoding:", {buffer, 4, 4}) == 1, "getCString:maxLength:encoding: capacity rules wrong");
    require(f.send(f.string("hello"), "rangeOfString:", {f.string("ll")}) == 2, "rangeOfString: location wrong");
    require(f.send(f.string("hello"), "rangeOfString:", {f.string("z")}) == static_cast<Address>(INT64_MAX), "rangeOfString: missing result is not NSNotFound");
    // Mutable arrays bridge to NSMutableArray and support fast enumeration.
    const auto array = f.send(cls("NSMutableArray"), "array");
    require(f.objc.object_class(array) == cls("NSMutableArray"), "mutable array has the immutable bridge class");
    for (const auto* text : {"one", "two", "three"}) f.send(array, "addObject:", {f.string(text)});
    f.send(array, "removeObjectAtIndex:", {0});
    require(f.send(array, "count") == 2 && f.send(array, "containsObject:", {f.string("three")}) == 1 && f.send(array, "indexOfObject:", {f.string("three")}) == 1, "NSMutableArray mutation or lookup wrong");
    const auto state = f.process.allocate_guest(64), items = f.process.allocate_guest(64);
    f.engine.fill_bytes(state, 0, 64);
    require(f.send(array, "countByEnumeratingWithState:objects:count:", {state, items, 8}) == 2 && f.engine.read64(state + 8) == items &&
            f.cf.utf8(f.engine.read64(items + 8)) == "three" && f.engine.read64(state + 16) == state + 24, "fast enumeration protocol wrong");
    require(f.send(array, "countByEnumeratingWithState:objects:count:", {state, items, 8}) == 0, "fast enumeration did not finish");
    const auto immutable = f.send(cls("NSArray"), "arrayWithArray:", {array});
    rejected([&] { f.send(immutable, "addObject:", {f.string("x")}); }, "addObject:");
    const auto dictionary = f.send(cls("NSMutableDictionary"), "dictionary");
    f.send(dictionary, "setObject:forKey:", {f.string("value"), f.string("key")});
    require(f.send(dictionary, "count") == 1 && f.cf.utf8(f.send(dictionary, "objectForKey:", {f.string("key")})) == "value", "NSMutableDictionary set/get wrong");
    const auto keys = f.send(dictionary, "allKeys"); require(f.cf.length(keys) == 1, "allKeys wrong");
    f.send(dictionary, "removeObjectForKey:", {f.string("key")}); require(f.send(dictionary, "count") == 0, "removeObjectForKey: failed");
    const auto set = f.send(cls("NSMutableSet"), "set");
    f.send(set, "addObject:", {f.string("member")}); f.send(set, "addObject:", {f.string("member")});
    require(f.send(set, "count") == 1 && f.send(set, "containsObject:", {f.string("member")}) == 1, "NSMutableSet uniqueness wrong");
    f.send(set, "removeObject:", {f.string("member")}); require(f.send(set, "count") == 0, "NSMutableSet removal failed");
    const auto locale = f.send(cls("NSLocale"), "currentLocale");
    require(f.cf.length(f.send(locale, "languageCode")) >= 2, "NSLocale languageCode missing");
    const auto date = f.send(cls("NSDate"), "date"), future = f.send(cls("NSDate"), "distantFuture");
    require(f.send(date, "compare:", {future}) == UINT64_MAX && f.send(future, "earlierDate:", {date}) == date, "NSDate ordering wrong");
    const auto loop = f.send(cls("NSRunLoop"), "currentRunLoop");
    require(loop == f.send(cls("NSRunLoop"), "currentRunLoop") && f.send(loop, "runMode:beforeDate:", {f.string("kCFRunLoopDefaultMode"), future}) == 0, "source-less run loop did not return NO");
    for (const auto* name : {"NSConstantDictionary", "NSConstantIntegerNumber", "NSDate", "NSURL", "NSMutableArray"})
        require(f.process.resolve(alchemy::core_foundation_library, "_OBJC_CLASS_$_" + std::string(name)).has_value(), "Foundation class is not exported from CoreFoundation");
    f.process.release_guest(state); f.process.release_guest(items); f.process.release_guest(buffer); f.process.release_guest(c_string);
    f.clear_inputs(); f.send(pool, "drain");
    require(f.process.objects().live_count() == 0, "Foundation collection objects leaked");
}

void files_bundles_and_notifications() {
    Fixture f;
    const auto pool = f.pool();
    const auto cls = [&](const char* name) { return f.objc.find_class(name); };
    const auto root = std::filesystem::temp_directory_path() / ("alchemy-foundation-" + std::to_string(std::random_device{}()));
    const auto app = root / "Test.app" / "Contents";
    std::filesystem::create_directories(app / "MacOS"); std::filesystem::create_directories(app / "Resources");
    std::ofstream(app / "Info.plist") << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"x\">\n"
        "<plist version=\"1.0\"><dict><key>CFBundleVersion</key><string>4.5 &amp; more</string><key>Flags</key><array><true/><integer>7</integer></array>"
        "<key>CFBundleIdentifier</key><string>org.example.test</string></dict></plist>\n";
    f.process.executable = app / "MacOS" / "test";
    const auto guest = alchemy::guest_path_string(root);
    const auto manager = f.send(cls("NSFileManager"), "defaultManager");
    const auto created = f.string(guest + "/made/deep");
    require(f.send(manager, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", {created, 1, 0, 0}) == 1 &&
            std::filesystem::is_directory(root / "made" / "deep"), "createDirectoryAtPath: did not create intermediates");
    const auto flag = f.process.allocate_guest(8);
    f.engine.fill_bytes(flag, 0, 8);
    require(f.send(manager, "fileExistsAtPath:isDirectory:", {created, flag}) == 1 && static_cast<transmute::GuestMemory&>(f.engine).read8(flag) == 1, "fileExistsAtPath:isDirectory: wrong");
    require(f.send(manager, "fileExistsAtPath:", {f.string(guest + "/absent")}) == 0, "missing file reported present");
    require(f.cf.utf8(f.send(manager, "currentDirectoryPath")) == alchemy::guest_path_string(f.process.working_directory), "currentDirectoryPath disagrees with the process");
    const auto search = f.process.resolve(alchemy::foundation_library, "_NSSearchPathForDirectoriesInDomains");
    require(search.has_value(), "NSSearchPathForDirectoriesInDomains missing");
    const auto paths = f.engine.invoke_callback(search->address, std::array<Address, 3>{14, 1, 1});
    require(f.cf.length(paths) == 1 && f.cf.utf8(f.cf.element(paths, 0)).ends_with("/Library/Application Support"), "user Application Support path wrong");
    const auto tilde = f.engine.invoke_callback(search->address, std::array<Address, 3>{14, 3, 0});
    require(f.cf.length(tilde) == 2 && f.cf.utf8(f.cf.element(tilde, 0)) == "~/Library/Application Support" && f.cf.utf8(f.cf.element(tilde, 1)) == "/Library/Application Support", "domain order or tilde form wrong");
    const auto url = f.send(cls("NSURL"), "fileURLWithPath:", {f.string(guest + "/made")});
    require(f.cf.utf8(f.send(url, "path")) == guest + "/made" && f.send(url, "hasDirectoryPath") == 1, "fileURLWithPath: did not detect the directory");
    const auto directory_key = f.engine.read64(f.process.resolve(alchemy::foundation_library, "_NSURLIsDirectoryKey")->address);
    const auto properties = f.send(url, "resourceValuesForKeys:error:", {f.send(cls("NSArray"), "arrayWithObject:", {directory_key}), 0});
    require(f.send(f.send(properties, "objectForKey:", {directory_key}), "boolValue") == 1, "resourceValuesForKeys: wrong");
    const auto bundle = f.send(cls("NSBundle"), "mainBundle");
    require(bundle == f.send(cls("NSBundle"), "mainBundle") && f.cf.utf8(f.send(bundle, "bundlePath")) == guest + "/Test.app", "main bundle path is not the .app");
    require(f.cf.utf8(f.send(bundle, "resourcePath")) == guest + "/Test.app/Contents/Resources", "bundle resource path wrong");
    const auto version_key = f.engine.read64(f.process.resolve(alchemy::core_foundation_library, "_kCFBundleVersionKey")->address);
    require(f.cf.utf8(f.send(bundle, "objectForInfoDictionaryKey:", {version_key})) == "4.5 & more" &&
            f.cf.utf8(f.send(bundle, "bundleIdentifier")) == "org.example.test", "Info.plist values wrong");
    const auto flags = f.cf.dictionary_value(f.send(bundle, "infoDictionary"), f.string("Flags"));
    require(f.cf.length(flags) == 2 && f.send(f.cf.element(flags, 0), "boolValue") == 1 && f.send(f.cf.element(flags, 1), "intValue") == 7, "Info.plist array decoding wrong");
    // Notifications are delivered synchronously to registered observers.
    unsigned delivered{}; std::string seen;
    const auto observer_class = f.objc.register_native_class("FoundationNotificationProbe", f.objc.find_class("NSObject"));
    static_cast<void>(f.objc.add_native_method(observer_class, "observe:", [&](auto& state, auto&) { ++delivered; seen = f.cf.utf8(f.send(state.x[2], "name")); }, false, "v@:@"));
    const auto observer = f.objc.allocate(observer_class);
    const auto center = f.send(cls("NSNotificationCenter"), "defaultCenter");
    f.send(center, "addObserver:selector:name:object:", {observer, f.objc.selector("observe:"), f.string("Ping"), 0});
    f.send(center, "postNotificationName:object:", {f.string("Pong"), 0});
    f.send(center, "postNotificationName:object:", {f.string("Ping"), observer});
    require(delivered == 1 && seen == "Ping", "notification delivery wrong");
    f.send(center, "removeObserver:", {observer});
    f.send(center, "postNotificationName:object:", {f.string("Ping"), 0});
    require(delivered == 1, "removed observer still notified");
    f.process.objects().release(observer); f.process.release_guest(flag);
    f.clear_inputs(); f.send(pool, "drain");
    std::filesystem::remove_all(root);
}
} // namespace

int main() {
    try {
        process_information(false); process_information(true); pool_lifecycle(); logging();
        strings_collections_and_sets(); files_bundles_and_notifications();
        std::cout << "Alchemy Foundation service tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
