// fwgaps provider tests: the Qt WebEngine imports of frameworks Alchemy
// already provides resolve, and the gap exports behave as documented in
// src/alchemy/darwin/kit/fwgaps (CFSet, CFCharacterSet/NSCharacterSet,
// CFNotificationCenter, CFString search and file-system representation,
// Foundation/AppKit helpers, CGEvent, CATransform3D, CoreText collections and
// coverage, CGL failures). The CoreServices, Security, SecurityInterface,
// SystemConfiguration, OpenDirectory and libobjc pieces and the vDSP kernels
// are tested in their frameworks' tests/ (alchemy_gui_framework_cases).
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <array>
#include <cmath>

namespace {
using namespace fill_test;

constexpr std::string_view cf = "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation";
constexpr std::string_view foundation = "/System/Library/Frameworks/Foundation.framework/Versions/C/Foundation";
constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit";
constexpr std::string_view cg = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics";
constexpr std::string_view ct = "/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText";
constexpr std::string_view ca = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore";
constexpr std::string_view gl = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL";
constexpr std::string_view cs = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";
constexpr std::string_view as = "/System/Library/Frameworks/ApplicationServices.framework/Versions/A/ApplicationServices";
constexpr std::string_view carbon = "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon";
constexpr std::string_view sec = "/System/Library/Frameworks/Security.framework/Versions/A/Security";
constexpr std::string_view sc = "/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration";
constexpr std::string_view iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit";
constexpr std::string_view vdsp = "/System/Library/Frameworks/Accelerate.framework/Versions/A/Accelerate";
constexpr std::string_view objc_lib = "/usr/lib/libobjc.A.dylib";
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";

Address string(Fixture& f, std::string_view text) { return f.cf.string(text); }
Address floats(Fixture& f, std::initializer_list<float> values) {
    const auto address = f.block(values.size() * 4);
    std::size_t i = 0;
    for (const auto value : values) f.engine.write32(address + 4 * i++, std::bit_cast<std::uint32_t>(value));
    return address;
}
float read_float(Fixture& f, Address address, std::size_t index = 0) { return std::bit_cast<float>(f.engine.read32(address + index * 4)); }

void imports(Fixture& f) {
    imports_resolve(f, {cf, foundation, appkit, cg, ct, ca, gl, cs, as, carbon, sec, sc, iokit, vdsp, objc_lib,
                        "/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface",
                        "/System/Library/Frameworks/OpenDirectory.framework/Versions/A/OpenDirectory",
                        "/System/Library/Frameworks/SecurityInterface.framework/Versions/A/SecurityInterface",
                        "/System/Library/Frameworks/CoreImage.framework/Versions/A/CoreImage"});
}

void appkit_gaps(Fixture& f) {
    require(f.string_constant(appkit, "_NSPrintFirstPage") == "NSFirstPage", "NSPrintFirstPage");
    // Kill ring.
    static_cast<void>(f.call(appkit, "__NSInitializeKillRing"));
    static_cast<void>(f.call(appkit, "__NSAppendToKillRing", {string(f, "Hello")}));
    static_cast<void>(f.call(appkit, "__NSAppendToKillRing", {string(f, " world")}));
    require(f.cf.utf8(f.call(appkit, "__NSYankFromKillRing")) == "Hello world", "kill sequence joins");
    static_cast<void>(f.call(appkit, "__NSNewKillRingSequence"));
    static_cast<void>(f.call(appkit, "__NSPrependToKillRing", {string(f, "second")}));
    require(f.cf.utf8(f.call(appkit, "__NSYankFromKillRing")) == "second", "new sequence");
    // NSRunningApplication: the current application.
    const auto current = f.send(f.cls("NSRunningApplication"), "currentApplication");
    require(static_cast<std::uint32_t>(f.send(current, "processIdentifier")) == static_cast<std::uint32_t>(f.call(sys, "_getpid")), "processIdentifier");
    require(f.send(f.cls("NSRunningApplication"), "runningApplicationWithProcessIdentifier:", {99999}) == 0, "other pid");
    // Speech voices are the host's (through AVFAudio, when installed): a
    // synthesizer exists exactly when there is a voice (speaking is tested in
    // alchemy_gui_appkit_api).
    const bool voiced = f.cf.length(f.send(f.cls("NSSpeechSynthesizer"), "availableVoices")) != 0;
    const auto synthesizer = f.send(f.send(f.cls("NSSpeechSynthesizer"), "alloc"), "init");
    require((synthesizer != 0) == voiced, "a synthesizer exactly when there is a voice");
    if (synthesizer) static_cast<void>(f.send(synthesizer, "release"));
    // NSAnimationContext: changes run at once, then the completion handler.
    const auto bridge = alchemy::gui::security::bridge();
    int order = 0, changes = 0, completion = 0;
    const auto changes_block = bridge->make_block([&](transmute::CpuState&, transmute::GuestMemory&) { changes = ++order; });
    const auto completion_block = bridge->make_block([&](transmute::CpuState&, transmute::GuestMemory&) { completion = ++order; });
    static_cast<void>(f.send(f.cls("NSAnimationContext"), "runAnimationGroup:completionHandler:", {changes_block, completion_block}));
    require(changes == 1 && completion == 2, "runAnimationGroup:completionHandler:");
}

void graphics(Fixture& f) {
    const auto event = f.call(cg, "_CGEventCreate", {0});
    require(event != 0, "CGEventCreate");
    const auto location = f.regs(cg, "_CGEventGetLocation", [&] { alchemy::CallRegs r{}; r.x[0] = event; return r; }());
    require(std::isfinite(location.d(0)) && std::isfinite(location.d(1)), "CGEventGetLocation");
    const auto data = f.call(cg, "_CGEventCreateData", {0, event});
    const auto copy = f.call(cg, "_CGEventCreateFromData", {0, data});
    require(copy && copy != event, "CGEventCreateFromData");
    const auto again = f.regs(cg, "_CGEventGetLocation", [&] { alchemy::CallRegs r{}; r.x[0] = copy; return r; }());
    require(again.d(0) == location.d(0) && again.d(1) == location.d(1), "event data round trip");
    require(f.call(cg, "_CGEventGetIntegerValueField", {event, 1}) == 0, "unset field is 0");
    require(f.call(cg, "_CGEventCreateFromData", {0, f.call(cf, "_CFDataCreate", {0, f.text("junk"), 4})}) == 0, "invalid data");
    // Granted for own content only; with no windows the list is empty (screencapturekit_tests.cpp lists windows).
    require(f.call(cg, "_CGPreflightScreenCaptureAccess") == 1, "screen capture of own content");
    const auto windows = f.call(cg, "_CGWindowListCopyWindowInfo", {0, 0});
    require(windows && f.cf.length(windows) == 0, "no own window, an empty window list");
    require(f.call(cg, "_CGPDFDocumentCreateWithProvider", {0}) == 0, "no provider, no PDF document");   // reading: coregraphics/tests/pdf_tests.cpp
    require(f.status(cg, "_CGSSetWindowAlpha", {0, 1, 0}) == 1002, "CGS calls: invalid connection");
    require(f.string_constant(cg, "_kCGWindowNumber") == "kCGWindowNumber", "kCGWindowNumber");
    // CATransform3D.
    const auto identity = f.symbol(ca, "_CATransform3DIdentity");
    require(std::bit_cast<double>(f.engine.read64(identity)) == 1.0 && std::bit_cast<double>(f.engine.read64(identity + 120)) == 1.0, "identity");
    alchemy::CallRegs registers{};
    registers.set_d(0, 1.5); registers.set_d(1, -2); registers.set_d(2, 3);
    const auto out = f.block(128);
    registers.indirect_result = out;
    static_cast<void>(f.regs(ca, "_CATransform3DMakeTranslation", registers));
    require(std::bit_cast<double>(f.engine.read64(out + 96)) == 1.5 && std::bit_cast<double>(f.engine.read64(out + 104)) == -2 &&
            std::bit_cast<double>(f.engine.read64(out + 112)) == 3 && std::bit_cast<double>(f.engine.read64(out)) == 1, "MakeTranslation");
    // CGL (darwin/opengl/): an off-screen renderer is never found.
    const auto pixel = f.block(8, 0xff), count = f.block(4, 0xff), off_screen = f.block(8);
    f.engine.write32(off_screen, 53);   // kCGLPFAOffScreen
    require(f.status(gl, "_CGLChoosePixelFormat", {off_screen, pixel, count}) == 0 && f.engine.read64(pixel) == 0 && f.engine.read32(count) == 0,
            "CGLChoosePixelFormat");
    require(f.call(gl, "_CGLGetCurrentContext") == 0 && f.status(gl, "_CGLSetCurrentContext", {0}) == 0, "no current context");
    require(f.read_text(f.call(gl, "_CGLErrorString", {10004})) == "invalid context", "CGLErrorString");
    // Carbon / ApplicationServices.
    const auto layout = static_cast<std::uint32_t>(f.call(carbon, "_KBGetLayoutType", {0}));
    require(layout == 0x414e5349 || layout == 0x49534f20 || layout == 0x4a495320, "KBGetLayoutType");
    require(f.status(carbon, "_EnableSecureEventInput") == 0 && f.status(carbon, "_DisableSecureEventInput") == 0, "secure input");
    require(f.status(as, "_GetProcessInformation", {f.block(8), f.block(64)}) == -600, "GetProcessInformation");
}

void core_text(Fixture& f) {
    const auto collection = f.call(ct, "_CTFontCollectionCreateFromAvailableFonts", {0});
    const auto descriptors = f.call(ct, "_CTFontCollectionCreateMatchingFontDescriptors", {collection});
    require(descriptors && f.cf.length(descriptors) > 0, "available fonts");
    alchemy::CallRegs registers{};
    registers.x[0] = string(f, "Arial");
    registers.set_d(0, 12);
    const auto font = f.regs(ct, "_CTFontCreateWithName", registers).x0;
    require(font != 0, "CTFontCreateWithName");
    const auto coverage = f.call(ct, "_CTFontCopyCharacterSet", {font});
    require(f.call(cf, "_CFCharacterSetIsLongCharacterMember", {coverage, 'A'}) == 1, "font covers 'A'");
    const auto tables = f.call(ct, "_CTFontCopyAvailableTables", {font, 0});
    bool cmap = false;
    for (std::uint64_t i = 0; i < f.call(cf, "_CFArrayGetCount", {tables}); ++i) cmap = cmap || f.call(cf, "_CFArrayGetValueAtIndex", {tables, i}) == 0x636d6170;
    require(cmap, "cmap table listed");
    require(f.call(ct, "_CTFontCopyVariationAxes", {font}) == 0, "variations not modelled");
}

void core_text_keys(Fixture& f) {
    require(f.string_constant(ct, "_kCTFontFeatureTypeIdentifierKey") == "CTFeatureTypeIdentifier", "feature type key");
    require(f.string_constant(ct, "_kCTFontFeatureSelectorIdentifierKey") == "CTFeatureSelectorIdentifier", "feature selector key");
}

} // namespace

// LibreOffice's VCL plug-in (libvclplug_osxlo.dylib): the CoreUI identity
// and the names it binds; the HITheme menu / text-box drawing stops explicitly.
void libreoffice(Fixture& f) {
    require(alchemy::ProcessContext::is_provider_library("/System/Library/PrivateFrameworks/CoreUI.framework/Versions/A/CoreUI"), "CoreUI identity");
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_NSAccessibilityIdentifierAttribute", "AXIdentifier"}, {"_NSAccessibilityPickAction", "AXPick"},
             {"_NSAccessibilityStandardWindowSubrole", "AXStandardWindow"}, {"_NSAccessibilityTextLinkSubrole", "AXTextLink"},
             {"_NSAccessibilitySuperscriptTextAttribute", "AXSuperscript"}})
        require(f.string_constant(appkit, symbol) == value, symbol);
    require(f.string_constant(ct, "_kCTFontEnabledAttribute") == "NSCTFontEnabledAttribute", "kCTFontEnabledAttribute");
    require(f.string_constant(ct, "_kCTFontCharacterSetAttribute") == "NSCTFontCharacterSetAttribute" &&
                f.string_constant(ct, "_kCTFontOpenTypeFeatureTag") == "CTFeatureOpenTypeTag" &&
                f.string_constant(ct, "_kCTFontOpenTypeFeatureValue") == "CTFeatureOpenTypeValue",
            "the character set and OpenType feature keys");
    require(f.string_constant(ct, "_kCTFontFeaturesAttribute") == "NSCTFontFeaturesAttribute", "kCTFontFeaturesAttribute");
    for (const auto* name : {"_HIThemeDrawMenuBackground", "_HIThemeDrawMenuItem", "_HIThemeDrawTextBox"}) {
        bool stopped = false;
        try { static_cast<void>(f.call(carbon, name, {0, 0, 0, 0, 0})); } catch (const std::exception&) { stopped = true; }
        require(stopped, std::string(name) + " stops explicitly");
    }
    const auto eps = f.cls("NSEPSImageRep");
    const auto data = f.send(f.cls("NSData"), "dataWithBytes:length:", {f.block(16), 16});
    require((f.send(eps, "canInitWithData:", {data}) & 0xff) == 0 && f.send(eps, "imageRepWithData:", {data}) == 0 &&
                f.cf.length(f.send(eps, "imageUnfilteredTypes")) == 0,
            "NSEPSImageRep interprets no EPS");
}

// The media apps' names (VLC, mpv, OBS, Firefox, HandBrake; Krita): what each answers.
void media(Fixture& f) {
    require(f.string_constant(appkit, "_NSWorkspaceApplicationKey") == "NSWorkspaceApplicationKey" &&
                f.string_constant(appkit, "_NSAccessibilityZoomButtonAttribute") == "AXZoomButton" &&
                f.string_constant(appkit, "_NSImageNameTouchBarPlayTemplate") == "NSTouchBarPlayTemplate" &&
                f.string_constant(appkit, "_NSAboutPanelOptionApplicationName") == "ApplicationName",
            "AppKit names");
    require(std::bit_cast<double>(f.engine.read64(f.symbol(appkit, "_NSAppKitVersionNumber"))) == 2487.0, "NSAppKitVersionNumber");
    require(f.call(appkit, "_NSApplicationLoad") == 1, "NSApplicationLoad");
    // Process Manager: the current process, then none.
    const auto psn = f.block(8);
    require(f.call(as, "_GetNextProcess", {psn}) == 0 && f.engine.read32(psn + 4) == 2, "GetNextProcess: the current process");
    require(static_cast<std::int16_t>(f.call(as, "_GetNextProcess", {psn})) == -600, "GetNextProcess: then procNotFound");
    // CoreGraphics: no fade reservation, an identity gamma table, no input monitoring, no display stream.
    const auto token = f.block(4);
    require(static_cast<std::int32_t>(f.call(cg, "_CGAcquireDisplayFadeReservation", {token})) == 1011, "no fade reservation");
    const auto red = f.block(256 * 4), green = f.block(256 * 4), blue = f.block(256 * 4), samples = f.block(4);
    require(f.call(cg, "_CGGetDisplayTransferByTable", {1, 256, red, green, blue, samples}) == 0 && f.engine.read32(samples) == 256 &&
                read_float(f, red, 0) == 0.0f && read_float(f, blue, 255) == 1.0f,
            "an identity transfer table");
    require(f.call(cg, "_CGPreflightListenEventAccess") == 0 && f.call(cg, "_CGDisplayStreamCreateWithDispatchQueue") == 0,
            "no input monitoring, no display stream");
    // CGFontCopyTableTags lists the font's tables (cmap among them).
    const auto font = f.call(cg, "_CGFontCreateWithFontName", {string(f, "Arial")});
    const auto tags = f.call(cg, "_CGFontCopyTableTags", {font});
    bool cmap = false;
    for (std::uint64_t i = 0; i < f.call(cf, "_CFArrayGetCount", {tags}); ++i) cmap = cmap || f.call(cf, "_CFArrayGetValueAtIndex", {tags, i}) == 0x636d6170;
    require(cmap, "CGFontCopyTableTags lists cmap");
    require(f.call(cg, "_CGFontCopyVariationAxes", {font}) == 0, "font variations are not modelled");
    // Pika: NSColorSpace archives securely and decodes back to the same named space.
    const auto srgb = f.send(f.cls("NSColorSpace"), "sRGBColorSpace");
    const auto archived = f.send(f.cls("NSKeyedArchiver"), "archivedDataWithRootObject:requiringSecureCoding:error:", {srgb, 1, 0});
    require(archived != 0, "an NSColorSpace archives with secure coding");
    const auto unarchived = f.send(f.cls("NSKeyedUnarchiver"), "unarchivedObjectOfClass:fromData:error:", {f.cls("NSColorSpace"), archived, 0});
    require(unarchived == srgb, "and unarchives to the sRGB color space");
    // Godot: Help-menu search handlers are kept (retained) until unregistered.
    const auto app = f.send(f.cls("NSApplication"), "sharedApplication");
    const auto handler = f.send(f.send(f.cls("NSObject"), "alloc"), "init");
    const auto count_before = f.send(handler, "retainCount");
    static_cast<void>(f.send(app, "registerUserInterfaceItemSearchHandler:", {handler}));
    require(f.send(handler, "retainCount") == count_before + 1, "the search handler is kept");
    static_cast<void>(f.send(app, "unregisterUserInterfaceItemSearchHandler:", {handler}));
    require(f.send(handler, "retainCount") == count_before, "and released when unregistered");
    // OBS: TextKit 2 content storage (no layout managers) and WebKit's rule list store (no rule lists).
    const auto content = f.send(f.send(f.cls("NSTextContentStorage"), "alloc"), "init");
    const auto storage = f.send(content, "textStorage");
    require(storage && f.send(content, "attributedString") == storage && f.cf.length(f.send(content, "textLayoutManagers")) == 0,
            "NSTextContentStorage");
    bool stopped = false;
    try { static_cast<void>(f.send(content, "addTextLayoutManager:", {0})); } catch (const std::exception&) { stopped = true; }
    require(stopped, "TextKit 2 layout stops explicitly");
    const auto bridge = alchemy::gui::security::bridge();
    std::int64_t lookup_code = 0;
    const auto lookup = bridge->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        lookup_code = s.x[1] == 0 && s.x[2] ? static_cast<std::int64_t>(f.send(s.x[2], "code")) : -1;
    });
    static_cast<void>(f.send(f.send(f.cls("WKContentRuleListStore"), "defaultStore"), "lookUpContentRuleListForIdentifier:completionHandler:",
                             {f.cf.string("rules"), lookup}));
    require(lookup_code == 7, "no content rule list is found (WKErrorContentRuleListStoreLookUpFailed)");
    // Inkscape and LibreOffice: no ATS platform font maps to a CGFont; LibreOffice's dispatcher target.
    require(f.call(cg, "_CGFontCreateWithPlatformFont", {f.block(8)}) == 0, "CGFontCreateWithPlatformFont");
    const auto dispatcher = f.call(carbon, "_GetEventDispatcherTarget");
    require(dispatcher && dispatcher != f.call(carbon, "_GetApplicationEventTarget"), "GetEventDispatcherTarget");
}

int main() {
    return run("fwgaps", {{"imports", imports}, {"appkit", appkit_gaps}, {"graphics", graphics}, {"core text", core_text},
                          {"core text keys", core_text_keys}, {"libreoffice", libreoffice}, {"media", media}});
}
