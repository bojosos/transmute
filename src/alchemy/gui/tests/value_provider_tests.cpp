// The value-class provider's row (src/alchemy/gui/value_provider.hpp): every
// strong import of Chrome 155 outside libSystem that no provider bound before
// resolves (chrome_imports.inc), and so does every strong system import of
// Electron as VS Code ships it (electron_imports.inc); the framework identities exist, and the
// exports behave as documented by the frameworks it installs (constants, value classes, Network
// path monitor and browser, AX values, Apple Events, keychain status,
// sandbox refusal, CoreVideo/CoreMedia/MIDI, CoreText fallback, CF
// additions, imp_implementationWithBlock, bz2 streams, IPP messages).
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/path.hpp"
#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/darwin/compression/bzip2.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
using namespace fill_test;

struct Name { const char* library; const char* symbol; };
constexpr Name chrome_imports[] = {
#include "alchemy/gui/tests/chrome_imports.inc"
};
constexpr Name electron_imports[] = {
#include "alchemy/gui/tests/electron_imports.inc"
#include "alchemy/gui/tests/vscode_native_imports.inc"
};

#define FRAMEWORK(name, version) "/System/Library/Frameworks/" name ".framework/Versions/" version "/" name
constexpr std::string_view appkit = FRAMEWORK("AppKit", "C"), cf_lib = FRAMEWORK("CoreFoundation", "A"),
    foundation = FRAMEWORK("Foundation", "C"), as = FRAMEWORK("ApplicationServices", "A"), cs = FRAMEWORK("CoreServices", "A"),
    sec = FRAMEWORK("Security", "A"), network = FRAMEWORK("Network", "A"), cv = FRAMEWORK("CoreVideo", "A"),
    cm = FRAMEWORK("CoreMedia", "A"), midi = FRAMEWORK("CoreMIDI", "A"), ct = FRAMEWORK("CoreText", "A"),
    sc = FRAMEWORK("SystemConfiguration", "A"), iosurface = FRAMEWORK("IOSurface", "A"),
    vt = FRAMEWORK("VideoToolbox", "A");
#undef FRAMEWORK
constexpr std::string_view service_management = "/System/Library/Frameworks/ServiceManagement.framework/Versions/A/ServiceManagement";
constexpr std::string_view sandbox = "/usr/lib/libsandbox.1.dylib", objc_lib = "/usr/lib/libobjc.A.dylib",
    bz2 = "/usr/lib/libbz2.1.0.dylib", cups = "/usr/lib/libcups.2.dylib", sys = "/usr/lib/libSystem.B.dylib",
    bsm = "/usr/lib/libbsm.0.dylib";

std::shared_ptr<alchemy::gui::security::Bridge> bridge() { return alchemy::gui::security::bridge(); }

void imports(Fixture& f) {
    std::size_t missing = 0;
    std::string first;
    for (const auto& item : chrome_imports) {
#ifndef ALCHEMY_HAVE_VULKAN_HEADERS
        // Metal's exports come from Metal on Vulkan; without the headers only the headless shell exists.
        if (std::string_view(item.library) == "/System/Library/Frameworks/Metal.framework/Versions/A/Metal") continue;
#endif
        if (f.process.resolve(item.library, item.symbol)) continue;
        if (first.empty()) first = std::string(item.library) + " " + item.symbol;
        ++missing;
    }
    require(missing == 0, std::to_string(missing) + " Chrome imports unresolved, first " + first);
    for (const auto* name : {"AVFAudio", "Accessibility", "AuthenticationServices", "CoreHaptics", "CoreML", "CoreMediaIO",
                             "CryptoTokenKit", "LocalAuthenticationEmbeddedUI", "MediaPlayer", "Network", "SafariServices",
                             "UserNotifications", "Vision", "ScreenCaptureKit"})
        require(alchemy::ProcessContext::is_provider_library(std::string("/System/Library/Frameworks/") + name + ".framework/Versions/A/" + name),
                std::string("identity ") + name);
    require(alchemy::ProcessContext::is_provider_library("/System/Library/PrivateFrameworks/SkyLight.framework/Versions/A/SkyLight"), "SkyLight");
    require(alchemy::ProcessContext::is_provider_library(sandbox), "libsandbox");
}

// Electron (VS Code): every strong system import of its image tree resolves.
void electron(Fixture& f) {
    std::size_t missing = 0;
    std::string names;
    for (const auto& item : electron_imports) {
        // dyld's functions are the module loader's (module_loader.cpp), not a provider's.
        const std::string_view symbol = item.symbol;
        if (symbol.starts_with("__dyld_") || symbol.starts_with("_dl") || symbol == "__NSGetExecutablePath" || symbol == "dyld_stub_binder") continue;
#ifndef ALCHEMY_HAVE_VULKAN_HEADERS
        // Metal's exports come from Metal on Vulkan; without the headers only the headless shell exists.
        if (std::string_view(item.library) == "/System/Library/Frameworks/Metal.framework/Versions/A/Metal") continue;
#endif
        if (f.process.resolve(item.library, item.symbol)) continue;
        if (missing++ < 100) names += std::string("\n  ") + item.library + " " + item.symbol;
    }
    require(missing == 0, std::to_string(missing) + " Electron imports unresolved:" + names);
    // The identities it adds have their own cases (darwin/quicklookui, storekit, coredata).
    require(f.string_constant(service_management, "_kSMErrorDomainLaunchd") == "CFErrorDomainLaunchd", "kSMErrorDomainLaunchd");
    bool connection{};
    try { static_cast<void>(f.send(f.cls("NSURLConnection"), "canHandleRequest:", {0})); }
    catch (const std::exception& error) { connection = std::string(error.what()).find("NSURLSession") != std::string::npos; }
    require(connection, "NSURLConnection stops");
    // dlopen("/usr/lib/libSystem.dylib"): the unversioned link to libSystem.B.dylib.
    require(alchemy::ProcessContext::is_provider_library("/usr/lib/libSystem.dylib") &&
            f.process.resolve("/usr/lib/libSystem.dylib", "_malloc").has_value(), "libSystem.dylib is libSystem.B.dylib");
    const auto events = f.send(f.cls("NSAppleEventManager"), "sharedAppleEventManager");
    require(events && f.send(events, "currentAppleEvent") == 0 && f.send(events, "currentReplyAppleEvent") == 0, "no current Apple event");
    // Electron frees its application delegate, then sets nil: the old delegate
    // (unretained) must not be messaged.
    const auto app = f.send(f.cls("NSApplication"), "sharedApplication");
    const auto delegate_class = f.objc.register_native_class("ElectronTestAppDelegate", f.cls("NSObject"), 8);
    static_cast<void>(f.objc.add_native_method(delegate_class, "applicationWillTerminate:", [](auto&, auto&) {}, false, "v@:@"));
    const auto delegate = f.send(f.send(delegate_class, "alloc"), "init");
    static_cast<void>(f.send(app, "setDelegate:", {delegate}));
    static_cast<void>(f.send(delegate, "release"));
    static_cast<void>(f.send(app, "setDelegate:", {0}));
    require(f.send(app, "delegate") == 0, "setDelegate:nil after the delegate was freed");
}

void appkit_constants(Fixture& f) {
    require(f.string_constant(appkit, "_NSAccessibilityEnabledAttribute") == "AXEnabled", "AXEnabled");
    require(f.string_constant(appkit, "_NSAccessibilityRTFForRangeParameterizedAttribute") == "AXRTFForRange", "AXRTFForRange");
    require(f.string_constant(appkit, "_NSAccessibilityFontTextAttribute") == "AXFont", "AXFont");
    require(f.string_constant(appkit, "_NSAccessibilityWindowMovedNotification") == "AXWindowMoved", "AXWindowMoved");
    require(f.string_constant(appkit, "_NSAccessibilityTabButtonSubrole") == "AXTabButton", "AXTabButton");
    require(f.string_constant(appkit, "_NSAccessibilityAscendingSortDirectionValue") == "AXAscendingSortDirection", "sort value");
    require(f.string_constant(appkit, "_NSAccessibilityPlaceholderValueAttribute") == "AXPlaceholderValue", "placeholder");
    require(f.string_constant(appkit, "_NSAccessibilityFontSizeKey") == "AXFontSize", "font size key");
    require(f.string_constant(appkit, "_NSImageNameFolder") == "NSFolder", "NSImageNameFolder");
    require(f.string_constant(appkit, "_NSMarkedClauseSegmentAttributeName") == "NSMarkedClauseSegment", "marked clause");
    require(f.cf.utf8(f.call(appkit, "_NSAccessibilityActionDescription", {f.cf.string("AXPress")})) == "press", "action description");
    require(f.call(appkit, "_NSAccessibilityActionDescription", {f.cf.string("AXCustom")}) == 0, "unknown action");
    require(f.call(appkit, "_NSPerformService", {f.cf.string("x"), 0}) == 0, "no services");
    // Real, subclassable classes with the expected superclasses.
    const auto super_of = [&](std::string_view name) { return f.send(f.cls(name), "superclass"); };
    require(super_of("NSVisualEffectView") == f.cls("NSView"), "NSVisualEffectView : NSView");
    require(super_of("NSThemeFrame") == f.cls("NSTitledFrame") && super_of("NSNextStepFrame") == f.cls("NSFrameView"), "frame views");
    require(super_of("NSStatusBarButton") == f.cls("NSButton"), "NSStatusBarButton : NSButton");
    const auto view = f.send(f.send(f.cls("NSVisualEffectView"), "alloc"), "init");
    static_cast<void>(f.send(view, "setMaterial:", {7}));
    require(f.send(view, "material") == 7, "material");
    require(f.send(f.cls("NSSharingService"), "sharingServiceNamed:", {f.cf.string("com.apple.share.Mail.compose")}) == 0, "no sharing");
    // macOS 14 menu API.
    const auto header = f.send(f.cls("NSMenuItem"), "sectionHeaderWithTitle:", {f.cf.string("Section")});
    require(header && f.cf.utf8(f.send(header, "title")) == "Section" && f.send(header, "isEnabled") == 0 &&
            f.send(header, "isSectionHeader") == 1, "sectionHeaderWithTitle:");
    const auto plain = f.send(f.send(f.cls("NSMenuItem"), "alloc"), "init");
    require(f.send(plain, "isSectionHeader") == 0 && f.send(plain, "allowsAutomaticKeyEquivalentLocalization") == 1, "item defaults");
    static_cast<void>(f.send(plain, "setSubtitle:", {f.cf.string("sub")}));
    require(f.cf.utf8(f.send(plain, "subtitle")) == "sub", "subtitle");
    const auto badge = f.send(f.cls("NSMenuItemBadge"), "alertsWithCount:", {3});
    require(f.send(badge, "itemCount") == 3 && f.send(badge, "type") == 3, "badge");
    const auto menu = f.send(f.send(f.cls("NSMenu"), "alloc"), "init");
    static_cast<void>(f.send(menu, "addItem:", {header}));
    static_cast<void>(f.send(menu, "addItem:", {plain}));
    static_cast<void>(f.send(menu, "setSelectionMode:", {2}));
    static_cast<void>(f.send(menu, "setSelectedItems:", {f.cf.array(std::vector<std::uint64_t>{plain})}));
    require(f.send(menu, "selectionMode") == 2 && f.send(plain, "state") == 1 && f.send(header, "state") == 0, "setSelectedItems:");
    require(f.cf.array_values(f.send(menu, "selectedItems")).size() == 1, "selectedItems");
    const auto palette = f.send(f.cls("NSMenu"), "paletteMenuWithColors:titles:selectionHandler:",
        {f.cf.array(std::vector<std::uint64_t>{f.cf.string("red"), f.cf.string("blue")}), f.cf.array(std::vector<std::uint64_t>{f.cf.string("Red")}), 0});
    require(f.send(palette, "numberOfItems") == 2 && f.send(palette, "presentationStyle") == 1, "palette menu");
    // NSResponder touchBar: nil until set, or made by -makeTouchBar on first access.
    const auto responder = f.send(f.send(f.cls("NSView"), "alloc"), "init");
    require(f.send(responder, "touchBar") == 0, "no touch bar by default");
    const auto bar = f.send(f.send(f.cls("NSTouchBar"), "alloc"), "init");
    static_cast<void>(f.send(responder, "setTouchBar:", {bar}));
    require(f.send(responder, "touchBar") == bar, "setTouchBar:");
    const auto maker = f.objc.register_native_class("AlchemyTouchBarMaker", f.cls("NSView"), 8);
    static_cast<void>(f.objc.add_native_method(maker, "makeTouchBar", [&f](transmute::CpuState& s, transmute::GuestMemory&) {
        s.x[0] = f.send(f.send(f.send(f.cls("NSTouchBar"), "alloc"), "init"), "autorelease");
    }, false, "@@:"));
    const auto made = f.send(f.send(maker, "alloc"), "init");
    const auto first_bar = f.send(made, "touchBar");
    require(first_bar && f.send(made, "touchBar") == first_bar, "touchBar is made once by makeTouchBar");
    static_cast<void>(f.send(made, "release"));
    static_cast<void>(f.send(responder, "release"));
    // Copies keep the macOS 14 fields.
    static_cast<void>(f.send(plain, "setBadge:", {badge}));
    const auto menu_copy = f.send(menu, "copy");
    const auto header_copy = f.send(menu_copy, "itemAtIndex:", {0}), plain_copy = f.send(menu_copy, "itemAtIndex:", {1});
    require(header_copy && header_copy != header && f.send(header_copy, "isSectionHeader") == 1 && f.send(header_copy, "isEnabled") == 0,
            "a section header's copy is a section header");
    require(f.cf.utf8(f.send(plain_copy, "subtitle")) == "sub" && f.send(plain_copy, "badge") == badge && f.send(plain_copy, "state") == 1 &&
            f.send(menu_copy, "selectionMode") == 2, "copied subtitle, badge, state, selection mode");
    static_cast<void>(f.send(menu_copy, "release"));
    // Titlebar accessories.
    const auto accessory = f.send(f.send(f.cls("NSTitlebarAccessoryViewController"), "alloc"), "init");
    require(f.send(accessory, "layoutAttribute") == 4, "default layout attribute");
    static_cast<void>(f.send(accessory, "setLayoutAttribute:", {2}));
    require(f.send(accessory, "layoutAttribute") == 2 && f.send(accessory, "automaticallyAdjustsSize") == 1, "layout attribute");
    // In-process remote accessibility tokens.
    const auto remote = f.cls("NSAccessibilityRemoteUIElement");
    const auto token = f.send(remote, "remoteTokenForLocalUIElement:", {accessory});
    const auto element = f.send(f.send(remote, "alloc"), "initWithRemoteToken:", {token});
    require(token && f.send(element, "localUIElement") == accessory, "remote token round trip");
    const auto manager = f.send(f.cls("ASWebAuthenticationSessionWebBrowserSessionManager"), "sharedManager");
    require(manager && manager == f.send(f.cls("ASWebAuthenticationSessionWebBrowserSessionManager"), "sharedManager") &&
            f.send(manager, "wasLaunchedByAuthenticationServices") == 0, "session manager");
    require(f.send(f.cls("NSApplication"), "instancesRespondToSelector:", {f.objc.selector("registerServicesMenuSendTypes:returnTypes:")}) == 1 &&
            f.send(f.cls("NSWindow"), "instancesRespondToSelector:", {f.objc.selector("windowTitlebarLayoutDirection")}) == 1, "window/app selectors");
    // The private Services classes Chrome patches at start-up.
    require(f.send(f.cls("_NSServiceEntry"), "instancesRespondToSelector:", {f.objc.selector("bundleIdentifier")}) == 1, "_NSServiceEntry");
    const auto updater = f.cls("_NSServicesMenuUpdater");
    require(f.send(updater, "instancesRespondToSelector:", {f.objc.selector("populateMenu:withServiceEntries:forDisplay:")}) == 1, "_NSServicesMenuUpdater");
    static_cast<void>(f.send(f.send(f.send(updater, "alloc"), "init"), "populateMenu:withServiceEntries:forDisplay:", {0, f.cf.array({}), 1}));
}

void services(Fixture& f) {
    // libsandbox refuses.
    const auto params = f.call(sandbox, "_sandbox_create_params");
    require(params && f.status(sandbox, "_sandbox_set_param", {params, f.text("USER_HOMEDIR"), f.text("/Users/x")}) == 0, "params");
    const auto sandbox_error = f.block(8);
    require(f.call(sandbox, "_sandbox_compile_string", {f.text("(version 1)"), params, sandbox_error}) == 0 &&
            f.read_text(f.engine.read64(sandbox_error)).find("sandbox") != std::string::npos, "compile refused");
    require(f.status(sandbox, "_sandbox_apply", {0}) == -1, "apply refused");
    static_cast<void>(f.call(sandbox, "_sandbox_free_params", {params}));
    // audit_token_to_euid reads val[1].
    const auto token = f.block(32);
    f.engine.write32(token + 4, 501);
    require(f.call(bsm, "_audit_token_to_euid", {token}) == 501, "audit_token_to_euid");
}

void media(Fixture& f) {
    require(f.string_constant(vt, "_kVTCompressionPropertyKey_ColorPrimaries") == "ColorPrimaries", "VT key");
    require(f.string_constant(cv, "_kCVImageBufferCleanApertureKey") == "CVCleanAperture", "clean aperture");
    const auto bgra = f.call(cv, "_CVPixelFormatDescriptionCreateWithPixelFormatType", {0, 0x42475241});
    require(bgra && f.cf.number_value(f.cf.dictionary_value(bgra, f.cf.string("BitsPerBlock"))) == 32.0, "BGRA description");
    const auto nv12 = f.call(cv, "_CVPixelFormatDescriptionCreateWithPixelFormatType", {0, 0x34323076});
    require(f.cf.utf8(f.cf.dictionary_value(nv12, f.cf.string("ComponentRange"))) == "VideoRange" &&
            f.cf.array_values(f.cf.dictionary_value(nv12, f.cf.string("Planes"))).size() == 2, "420v description");
    require(f.call(cv, "_CVPixelFormatDescriptionCreateWithPixelFormatType", {0, 0x31323334}) == 0, "unknown format");
    // One second of CMTime is one second of mach time units.
    const auto time = f.block(24);
    f.engine.write64(time, 1000);
    f.engine.write32(time + 8, 1000);
    f.engine.write32(time + 12, 1);
    const auto info = f.block(8);
    static_cast<void>(f.call(sys, "_mach_timebase_info", {info}));
    const auto expected = 1000000000ULL * f.engine.read32(info + 4) / f.engine.read32(info);
    const auto units = f.call(cm, "_CMClockConvertHostTimeToSystemUnits", {time});
    require(units + 1 >= expected && units <= expected + 1, "host time conversion");
    // MIDI 2.0 event lists.
    const auto list = f.block(256);
    const auto words = f.block(8);
    f.engine.write32(words, 0x20903c64);
    const auto first = f.call(midi, "_MIDIEventListInit", {list, 2});
    require(first == list + 8 && f.engine.read32(list) == 2 && f.engine.read32(list + 4) == 0, "MIDIEventListInit");
    const auto packet = f.call(midi, "_MIDIEventListAdd", {list, 256, first, 100, 1, words});
    require(packet == first && f.engine.read32(list + 4) == 1 && f.engine.read32(packet + 8) == 1, "first packet");
    const auto same = f.call(midi, "_MIDIEventListAdd", {list, 256, packet, 100, 1, words});
    require(same == packet && f.engine.read32(packet + 8) == 2, "same timestamp appends");
    const auto next = f.call(midi, "_MIDIEventListAdd", {list, 256, packet, 200, 1, words});
    require(next == packet + 12 + 8 && f.engine.read32(list + 4) == 2, "new packet");
    require(f.call(midi, "_MIDIEventListAdd", {list, 40, next, 300, 4, words}) == 0, "no room");
    // (IOSurfaceSetPurgeable is QuartzCore's: compositing_tests.cpp.)
    // CoreText: the fallback font for a covered string is the font itself.
    const auto font = f.engine.invoke_callback(f.symbol(ct, "_CTFontCreateWithName"), std::array<Address, 2>{f.cf.string("Arial"), 0});
    if (font) {
        const auto text = f.cf.string("A");
        const auto result = f.call(ct, "_CTFontCreateForStringWithLanguage", {font, text, 0, 1, f.cf.string("en")});
        require(result != 0, "CTFontCreateForStringWithLanguage");
    }
    require(f.call(ct, "_CTFontCopyVariationAxesInternal", {font}) == 0, "no variation axes");
}

void objc_runtime(Fixture& f) {
    // imp_implementationWithBlock: self arrives as the block's first argument.
    auto b = bridge();
    std::array<Address, 3> seen{};
    const auto block = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        seen = {s.x[0], s.x[1], s.x[2]};
        s.x[0] = 0x1234;
    });
    const auto imp = f.call(objc_lib, "_imp_implementationWithBlock", {block});
    require(imp != 0, "trampoline");
    const auto copied = f.call(objc_lib, "_imp_getBlock", {imp});
    const std::array<Address, 3> arguments{0x5000, 0x6000, 0x7000};
    require(f.engine.invoke_callback(imp, arguments) == 0x1234, "trampoline result");
    require(seen[0] == copied && seen[1] == 0x5000 && seen[2] == 0x7000, "self moved, _cmd dropped");
    require(f.call(objc_lib, "_objc_setExceptionPreprocessor", {0x10}) == 0 && f.call(objc_lib, "_objc_setExceptionPreprocessor", {0}) == 0x10,
            "exception preprocessor");
}

void bzip2_streams(Fixture& f) {
    std::vector<std::uint8_t> original(5000);
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<std::uint8_t>((i * 7) % 61);
    const auto compressed = alchemy::gui::completion_kit::bzip2::encode(original);
    const auto stream = f.block(80);
    require(f.status(bz2, "_BZ2_bzDecompressInit", {stream, 0, 0}) == 0, "init");
    const auto input = f.block(compressed.size());
    f.engine.write_bytes(input, compressed);
    const auto output = f.block(original.size());
    // Feed the input in two halves, then read.
    const auto half = compressed.size() / 2;
    f.engine.write64(stream, input);
    f.engine.write32(stream + 8, static_cast<std::uint32_t>(half));
    f.engine.write64(stream + 24, output);
    f.engine.write32(stream + 32, static_cast<std::uint32_t>(original.size()));
    require(f.status(bz2, "_BZ2_bzDecompress", {stream}) == 0, "needs more input");
    f.engine.write32(stream + 8, static_cast<std::uint32_t>(compressed.size() - half));
    require(f.status(bz2, "_BZ2_bzDecompress", {stream}) == 4, "stream end");
    std::vector<std::uint8_t> result(original.size());
    f.engine.read_bytes(output, result);
    require(result == original && f.engine.read32(stream + 36) == original.size(), "decompressed");
    require(f.status(bz2, "_BZ2_bzDecompressEnd", {stream}) == 0, "end");
}

void ipp(Fixture& f) {
    const auto message = f.call(cups, "_ippNew");
    static_cast<void>(f.call(cups, "_ippAddBoolean", {message, 2, f.text("color"), 1}));
    static_cast<void>(f.call(cups, "_ippAddRange", {message, 2, f.text("copies"), 1, 99}));
    const auto inner = f.call(cups, "_ippNew");
    static_cast<void>(f.call(cups, "_ippAddInteger", {inner, 2, 0x21, f.text("x-dimension"), 21000}));
    static_cast<void>(f.call(cups, "_ippAddCollection", {message, 2, f.text("media-size"), inner}));
    static_cast<void>(f.call(cups, "_ippDelete", {inner}));
    const auto copies = f.call(cups, "_ippFindAttribute", {message, f.text("copies"), 0});
    const auto upper = f.block(4);
    require(copies && f.call(cups, "_ippGetRange", {copies, 0, upper}) == 1 && f.engine.read32(upper) == 99, "range");
    const auto media = f.call(cups, "_ippFindAttribute", {message, f.text("media-size"), 0x34});
    const auto collection = f.call(cups, "_ippGetCollection", {media, 0});
    const auto x = f.call(cups, "_ippFindAttribute", {collection, f.text("x-dimension"), 0});
    require(x && f.call(cups, "_ippGetInteger", {x, 0}) == 21000, "collection copy");
    static_cast<void>(f.call(cups, "_ippDeleteAttribute", {message, copies}));
    require(f.call(cups, "_ippFindAttribute", {message, f.text("copies"), 0}) == 0 &&
            f.call(cups, "_ippFindAttribute", {message, f.text("media-size"), 0}) == media, "delete attribute");
    static_cast<void>(f.call(cups, "_ippDelete", {message}));
    const auto a4 = f.call(cups, "_pwgMediaForSize", {21000, 29700});
    require(a4 && f.read_text(f.engine.read64(a4)) == "iso_a4_210x297mm", "pwgMediaForSize A4");
    const auto custom = f.call(cups, "_pwgMediaForSize", {10000, 15050});
    require(f.read_text(f.engine.read64(custom)) == "custom_100x150.5mm_100x150.5mm", "custom size");
    require(!f.read_text(f.call(cups, "_cupsUser")).empty(), "cupsUser");
}

// Chrome's frontier (stage 1 (f)): framework bundle resources, file-URL
// components, CFStringCompare options.
void frontier(Fixture& f) {
    // A versioned framework: X.framework/Versions/1/{Resources/{Info.plist,
    // icudtl.dat,en.lproj/l.strings}}, opened at the version directory.
    const auto root = std::filesystem::temp_directory_path() / "alchemy_chrome_test.framework";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto version = root / "Versions" / "1";
    std::filesystem::create_directories(version / "Resources" / "en.lproj");
    {
        std::ofstream info(version / "Resources" / "Info.plist", std::ios::binary);
        info << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\"><dict>"
                "<key>CFBundleIdentifier</key><string>com.example.alchemy.framework</string></dict></plist>\n";
        std::ofstream(version / "Resources" / "icudtl.dat", std::ios::binary) << "icu";
        std::ofstream(version / "Resources" / "en.lproj" / "l.strings", std::ios::binary) << "x";
    }
    const auto bundle = f.send(f.send(f.cls("NSBundle"), "alloc"), "initWithPath:", {f.cf.string(alchemy::guest_path_string(version))});
    const auto icu = f.send(bundle, "URLForResource:withExtension:", {f.cf.string("icudtl.dat"), 0});
    require(icu && f.cf.url_path(icu).ends_with("/Versions/1/Resources/icudtl.dat"), "framework resource");
    require(f.cf.utf8(f.send(bundle, "pathForResource:ofType:", {f.cf.string("l"), f.cf.string("strings")})).ends_with("/en.lproj/l.strings"), "lproj resource");
    require(f.cf.utf8(f.send(bundle, "resourcePath")).ends_with("/Versions/1/Resources"), "resourcePath");
    require(f.cf.utf8(f.send(bundle, "bundleIdentifier")) == "com.example.alchemy.framework", "framework Info.plist");
    // File URL components.
    const auto file = f.cf.file_url("/tmp/a b.txt", false);
    require(f.cf.utf8(f.call(cf_lib, "_CFURLCopyScheme", {file})) == "file", "file URL scheme");
    const auto file_path = f.cf.utf8(f.call(cf_lib, "_CFURLCopyPath", {file}));
    require(file_path.ends_with("/a%20b.txt"), "file URL path " + file_path);
    // SCDynamicStore run-loop source (Chrome's network config watcher).
    const auto sc_store = f.call(sc, "_SCDynamicStoreCreate", {0, f.cf.string("test"), 0x1000, 0});
    require(sc_store != 0, "SCDynamicStoreCreate");
    const auto watched = f.cf.array(std::vector<std::uint64_t>{f.cf.string("State:/Network/Global/IPv4"), f.cf.string("State:/Network/Global/DNS")});
    require((f.call(sc, "_SCDynamicStoreSetNotificationKeys", {sc_store, watched, 0}) & 0xff) == 1, "notification keys");
    const auto sc_source = f.call(sc, "_SCDynamicStoreCreateRunLoopSource", {0, sc_store, 0});
    require(sc_source != 0, "SCDynamicStoreCreateRunLoopSource");
    const auto loop = f.call(cf_lib, "_CFRunLoopGetCurrent");
    static_cast<void>(f.call(cf_lib, "_CFRunLoopAddSource", {loop, sc_source, f.engine.read64(f.symbol(cf_lib, "_kCFRunLoopDefaultMode"))}));
    static_cast<void>(f.call(cf_lib, "_CFRunLoopSourceInvalidate", {sc_source}));
    // CFStringCompare options.
    const auto compare = [&](std::string_view a, std::string_view b, Address options) {
        return static_cast<std::int64_t>(f.call(cf_lib, "_CFStringCompare", {f.cf.string(a), f.cf.string(b), options}));
    };
    require(compare("FILE", "file", 1) == 0 && compare("FILE", "file", 0) == -1, "case insensitive");
    require(compare("_", "a", 1) == -1, "case folds to lower case");
    require(compare("file10", "file9", 64) == 1 && compare("file10", "file9", 0) == -1, "numerically");
    require(compare("\xC3\xA9", "e", 128) == 0, "diacritic insensitive");
    require(compare("e\xCC\x81", "\xC3\xA9", 16) == 0, "nonliteral");
    require(compare("\xEF\xBC\xA1", "A", 256) == 0, "width insensitive");
    require(compare("a", "A", 1 | 512) != 0, "forced ordering");
    require(compare("abc", "ABD", 1 | 4 | 8) == -1, "backwards/anchored ignored");
}

// Blocks as CF collection values, NSURL backup exclusion, NSUserNotificationCenter.
void blocks_and_urls(Fixture& f) {
    // A heap block: _Block_copy of a stack block literal {isa, flags, reserved, invoke, descriptor}.
    const auto descriptor = f.block(16);
    f.engine.write64(descriptor, 0);
    f.engine.write64(descriptor + 8, 32);
    const auto literal = f.block(32);
    f.engine.write64(literal, f.symbol(sys, "__NSConcreteStackBlock"));
    f.engine.write64(literal + 16, f.symbol(sys, "_getpid"));
    f.engine.write64(literal + 24, descriptor);
    const auto heap = f.call(sys, "__Block_copy", {literal});
    const auto refs = [&] { return (f.engine.read32(heap + 8) & 0xfffe) >> 1; };
    require(heap && heap != literal && refs() == 1, "heap block");
    const auto dictionary = f.call(cf_lib, "_CFDictionaryCreateMutable", {0, 0, f.symbol(cf_lib, "_kCFTypeDictionaryKeyCallBacks"),
                                                                        f.symbol(cf_lib, "_kCFTypeDictionaryValueCallBacks")});
    const auto key = f.cf.string("block");
    static_cast<void>(f.call(cf_lib, "_CFDictionarySetValue", {dictionary, key, heap}));
    require(refs() == 2 && f.call(cf_lib, "_CFDictionaryGetValue", {dictionary, key}) == heap, "dictionary retains the block");
    require(f.call(cf_lib, "_CFHash", {heap}) == f.call(cf_lib, "_CFHash", {heap}) && f.call(cf_lib, "_CFEqual", {heap, heap}) == 1, "identity");
    static_cast<void>(f.call(cf_lib, "_CFDictionaryRemoveValue", {dictionary, key}));
    require(refs() == 1, "removal releases the block");
    static_cast<void>(f.call(sys, "__Block_release", {heap}));
    // NSURLIsExcludedFromBackupKey: the Time Machine exclusion xattr.
    const auto file = std::filesystem::temp_directory_path() / "alchemy_chrome_backup_test.txt";
    std::ofstream(file, std::ios::binary) << "x";
    const auto url = f.cf.file_url(alchemy::guest_path_string(file), false);
    const auto error = f.block(8);
    const auto yes = f.send(f.cls("NSNumber"), "numberWithBool:", {1});
    f.engine.write64(error, 0);
    const auto excluded = f.send(url, "setResourceValue:forKey:error:", {yes, f.cf.string("NSURLIsExcludedFromBackupKey"), error}) & 0xff;
    require(excluded == 1, "exclude from backup: " + (f.engine.read64(error) ? std::to_string(f.send(f.engine.read64(error), "code")) + " " +
                                                      f.cf.utf8(f.send(f.engine.read64(error), "localizedDescription")) : std::string("no error")));
    const auto size = static_cast<std::int64_t>(f.call(sys, "_getxattr", {f.text(alchemy::guest_path_string(file)),
                                                    f.text("com.apple.metadata:com_apple_backup_excludeItem"), 0, 0, 0, 0}));
    require(size == 61, "exclusion attribute " + std::to_string(size));
    const auto names = f.block(256);
    const auto listed = static_cast<std::int64_t>(f.call(sys, "_listxattr", {f.text(alchemy::guest_path_string(file)), names, 256, 0}));
    require(listed > 0 && f.read_text(names) == "com.apple.metadata:com_apple_backup_excludeItem", "listxattr keeps the ':'");
    require((f.send(url, "setResourceValue:forKey:error:", {yes, f.cf.string("NSURLIsHiddenKey"), error}) & 0xff) == 0 && f.engine.read64(error), "unsupported key");
    std::error_code ignored;
    std::filesystem::remove(file, ignored);
    // NSUserNotificationCenter: a real default center; nothing is delivered.
    const auto center = f.send(f.cls("NSUserNotificationCenter"), "defaultUserNotificationCenter");
    require(center && center == f.send(f.cls("NSUserNotificationCenter"), "defaultUserNotificationCenter"), "default center");
    const auto note = f.send(f.send(f.cls("NSUserNotification"), "alloc"), "init");
    static_cast<void>(f.send(note, "setTitle:", {f.cf.string("t")}));
    static_cast<void>(f.send(center, "deliverNotification:", {note}));
    require(f.cf.array_values(f.send(center, "deliveredNotifications")).empty(), "nothing delivered");
}

} // namespace

// NSSpellChecker over the host's Windows spell checker (en-US must exist).
void spelling(Fixture& f) {
    const auto checker = f.send(f.cls("NSSpellChecker"), "sharedSpellChecker");
    require(checker && f.send(f.cls("NSSpellChecker"), "sharedSpellCheckerExists") == 1, "sharedSpellChecker");
    const auto languages = f.cf.array_values(f.send(checker, "availableLanguages"));
    bool has_en_us = false;
    for (const auto language : languages) has_en_us = has_en_us || f.cf.utf8(language) == "en_US";
    require(has_en_us, "availableLanguages has en_US (" + std::to_string(languages.size()) + " languages)");
    require((f.send(checker, "setLanguage:", {f.cf.string("en")}) & 0xff) == 1 && f.cf.utf8(f.send(checker, "language")).rfind("en_", 0) == 0,
            "setLanguage: en picks an English dictionary");
    require((f.send(checker, "setLanguage:", {f.cf.string("xx_NOPE")}) & 0xff) == 0, "an unknown language is refused");
    static_cast<void>(f.send(checker, "setLanguage:", {f.cf.string("en_US")}));
    const auto tag = f.send(checker, "uniqueSpellDocumentTag");
    const auto text = f.cf.string("This sentnce is fine.");
    const auto count = f.block(8);
    alchemy::CallRegs check;
    check.x[2] = text; check.x[3] = 0; check.x[4] = 0; check.x[5] = 0; check.x[6] = tag; check.x[7] = count;
    auto found = f.objc.send(checker, "checkSpellingOfString:startingAt:language:wrap:inSpellDocumentWithTag:wordCount:", check);
    require(found.x0 == 5 && found.x1 == 7 && f.engine.read64(count) == 4, "checkSpellingOfString finds sentnce");
    alchemy::CallRegs guess;
    guess.x[2] = 5; guess.x[3] = 7; guess.x[4] = text; guess.x[5] = 0; guess.x[6] = tag;
    const auto guesses = f.cf.array_values(f.objc.send(checker, "guessesForWordRange:inString:language:inSpellDocumentWithTag:", guess).x0);
    bool sentence = false;
    for (const auto word : guesses) sentence = sentence || f.cf.utf8(word) == "sentence";
    require(sentence, "guesses include sentence");
    alchemy::CallRegs results;
    results.x[2] = text; results.x[3] = 0; results.x[4] = 21; results.x[5] = 2; results.x[6] = 0; results.x[7] = tag;
    results.push_stack(std::uint64_t{0});
    results.push_stack(std::uint64_t{0});
    const auto list = f.cf.array_values(f.objc.send(checker, "checkString:range:types:options:inSpellDocumentWithTag:orthography:wordCount:", results).x0);
    require(list.size() == 1 && f.send(list[0], "resultType") == 2, "checkString: one spelling result");
    const auto range = f.objc.send(list[0], "range");
    require(range.x0 == 5 && range.x1 == 7, "spelling result range");
    static_cast<void>(f.send(checker, "ignoreWord:inSpellDocumentWithTag:", {f.cf.string("sentnce"), tag}));
    found = f.objc.send(checker, "checkSpellingOfString:startingAt:language:wrap:inSpellDocumentWithTag:wordCount:", check);
    require(found.x0 == 0x7fffffffffffffffULL, "an ignored word is not reported");
    static_cast<void>(f.send(checker, "closeSpellDocumentWithTag:", {tag}));
    static_cast<void>(f.send(checker, "learnWord:", {f.cf.string("Alchemyzz")}));
    require(f.send(checker, "hasLearnedWord:", {f.cf.string("Alchemyzz")}) == 1, "learnWord:");
    static_cast<void>(f.send(checker, "unlearnWord:", {f.cf.string("Alchemyzz")}));
    require(f.send(checker, "hasLearnedWord:", {f.cf.string("Alchemyzz")}) == 0, "unlearnWord:");
    require(f.send(checker, "spellingPanel") == 0, "no spelling panel");
    // A correction indicator is never shown; dismissing it completes with nil.
    int completions = 0;
    Address accepted = 1;
    const auto completion = bridge()->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) { ++completions; accepted = s.x[1]; });
    alchemy::CallRegs show;
    show.x[2] = 0; show.x[3] = f.cf.string("sentence"); show.x[4] = f.cf.array({}); show.x[5] = 0x1234; show.x[6] = completion;
    static_cast<void>(f.objc.send(checker, "showCorrectionIndicatorOfType:primaryString:alternativeStrings:forStringInRect:view:completionHandler:", show));
    require(completions == 0, "the indicator waits for its dismissal");
    static_cast<void>(f.send(checker, "dismissCorrectionIndicatorForView:", {0x1234}));
    require(completions == 1 && accepted == 0, "dismissal completes without an accepted string");
    require(f.send(checker, "preventsAutocorrectionBeforeString:language:", {f.cf.string("x"), 0}) == 0, "no autocorrection rules");
}

int main() {
    return run("value-provider", {{"imports", imports}, {"electron", electron}, {"appkit", appkit_constants}, {"services", services}, {"media", media},
                                     {"objc", objc_runtime}, {"bzip2", bzip2_streams},
                                     {"ipp", ipp}, {"frontier", frontier}, {"blocks and urls", blocks_and_urls}, {"spelling", spelling}});
}
