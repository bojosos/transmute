// NSUbiquitousKeyValueStore (a local store, no iCloud), -[NSURL pathComponents] for file URLs and
// NSItemProvider (kvs_and_item_providers.cpp, foundation_extra.cpp).
#include "alchemy/gui/tests/framework_case.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/path.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

namespace {
using namespace fill_test;

constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";

std::atomic<Address> loaded_data{0}, loaded_error{0};
std::atomic<int> answers{0};

void wait_for(int count) {
    for (int i = 0; i < 500 && answers.load() < count; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(answers.load() >= count, "the completion handler ran");
}

const Case foundation_kvs_and_item_providers_case("foundation_kvs_and_item_providers", [](Fixture& f) {
    const auto home = std::filesystem::temp_directory_path() /
        ("alchemy_kvs_case_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(home);
    f.process.environment.push_back("HOME=" + alchemy::guest_path_string(home));

    // NSUbiquitousKeyValueStore: typed values in a local store, written by -synchronize.
    const auto store_class = f.objc.find_class("NSUbiquitousKeyValueStore");
    require(store_class != 0, "NSUbiquitousKeyValueStore");
    const auto store = f.send(store_class, "defaultStore");
    require(store && f.send(store_class, "defaultStore") == store, "+defaultStore");
    const auto name = f.cf.string("name"), count = f.cf.string("count"), flag = f.cf.string("flag");
    f.send(store, "setString:forKey:", {f.cf.string("value"), name});
    f.send(store, "setLongLong:forKey:", {42, count});
    f.send(store, "setBool:forKey:", {1, flag});
    require(f.cf.utf8(f.send(store, "stringForKey:", {name})) == "value" && f.send(store, "longLongForKey:", {count}) == 42 &&
            (f.send(store, "boolForKey:", {flag}) & 0xff) == 1 && f.send(store, "arrayForKey:", {name}) == 0, "typed values");
    require((f.send(store, "synchronize") & 0xff) == 1, "-synchronize");
    bool written = false;
    for (const auto& entry : std::filesystem::directory_iterator(home / "Library" / "Preferences"))
        written = written || entry.path().filename().string().ends_with(".AlchemyUbiquitousKeyValueStore.plist");
    require(written, "the store is a local preferences file");
    require(f.call(cf, "_CFDictionaryGetCount", {f.send(store, "dictionaryRepresentation")}) == 3, "-dictionaryRepresentation");
    f.send(store, "removeObjectForKey:", {count});
    require(f.send(store, "objectForKey:", {count}) == 0, "-removeObjectForKey:");
    require(f.string_constant(foundation, "_NSUbiquitousKeyValueStoreDidChangeExternallyNotification") ==
                "NSUbiquitousKeyValueStoreDidChangeExternallyNotification" &&
            f.string_constant(foundation, "_NSUbiquitousKeyValueStoreChangedKeysKey") == "NSUbiquitousKeyValueStoreChangedKeysKey", "constants");

    // -[NSURL pathComponents] for a file URL.
    const auto components = f.send(f.cf.file_url("/Users/test/file.txt", false), "pathComponents");
    std::string listed;
    if (components) for (const auto part : f.cf.array_values(components)) listed += "[" + f.cf.utf8(part) + "]";
    // (the fixture's guest paths may sit under /Volumes/<drive>; the components follow the URL's path)
    const auto parts = f.cf.length(components);
    require(parts >= 4 && f.cf.utf8(f.call(cf, "_CFArrayGetValueAtIndex", {components, 0})) == "/" &&
            f.cf.utf8(f.call(cf, "_CFArrayGetValueAtIndex", {components, parts - 1})) == "file.txt" &&
            f.cf.utf8(f.call(cf, "_CFArrayGetValueAtIndex", {components, parts - 2})) == "test", "-[NSURL pathComponents]: " + listed);

    // NSItemProvider: an item, a registered loader, and a type it does not have.
    auto bridge = alchemy::gui::security::bridge();
    const auto done = bridge->make_block([](transmute::CpuState& c, transmute::GuestMemory&) {
        loaded_data = c.x[1];
        loaded_error = c.x[2];
        ++answers;
    });
    const auto provider_class = f.objc.find_class("NSItemProvider");
    require(provider_class != 0, "NSItemProvider");
    const auto text = f.send(f.send(provider_class, "alloc"), "initWithObject:", {f.cf.string("hello")});
    const auto types = f.send(text, "registeredTypeIdentifiers");
    require(f.cf.length(types) == 1 && f.cf.utf8(f.call(cf, "_CFArrayGetValueAtIndex", {types, 0})) == "public.utf8-plain-text", "an NSString item");
    require((f.send(text, "hasItemConformingToTypeIdentifier:", {f.cf.string("public.plain-text")}) & 0xff) == 1 &&
            (f.send(text, "hasItemConformingToTypeIdentifier:", {f.cf.string("public.png")}) & 0xff) == 0, "conformance");
    answers = 0;
    require(f.send(text, "loadDataRepresentationForTypeIdentifier:completionHandler:", {f.cf.string("public.utf8-plain-text"), done}) != 0,
            "the load returns its NSProgress");
    wait_for(1);
    require(loaded_data.load() && loaded_error.load() == 0 && f.cf.length(loaded_data.load()) == 5, "the item's data");

    static Address loader_reply;
    const std::vector<std::uint8_t> reply_bytes{'l', 'o', 'a', 'd'};
    loader_reply = bridge->data(reply_bytes);
    const auto loader = bridge->make_block([](transmute::CpuState& c, transmute::GuestMemory&) {
        static_cast<void>(alchemy::gui::security::bridge()->invoke_block(c.x[1], {loader_reply, 0}));
        c.x[0] = 0;   // no NSProgress
    });
    const auto lazy = f.send(f.send(provider_class, "alloc"), "init");
    f.send(lazy, "registerDataRepresentationForTypeIdentifier:visibility:loadHandler:", {f.cf.string("public.data"), 0, loader});
    answers = 0;
    f.send(lazy, "loadDataRepresentationForTypeIdentifier:completionHandler:", {f.cf.string("public.data"), done});
    wait_for(1);
    require(loaded_data.load() == loader_reply && loaded_error.load() == 0, "a registered loader's data");
    answers = 0;
    f.send(lazy, "loadDataRepresentationForTypeIdentifier:completionHandler:", {f.cf.string("public.png"), done});
    wait_for(1);
    require(loaded_data.load() == 0 && loaded_error.load() && f.call(cf, "_CFErrorGetCode", {loaded_error.load()}) == static_cast<Address>(-1000),
            "NSItemProviderItemUnavailableError");
    std::error_code ignored;
    std::filesystem::remove_all(home, ignored);
});

} // namespace
