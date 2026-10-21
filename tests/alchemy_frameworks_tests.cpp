#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_numeric.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "transmute/jit.hpp"

#include <array>
#include <bit>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Address = std::uint64_t;
constexpr std::string_view appkit = "/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit",
    metal = "/System/Library/Frameworks/Metal.framework/Versions/A/Metal",
    quartz = "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore",
    openal = "/System/Library/Frameworks/OpenAL.framework/Versions/A/OpenAL",
    graphics = "/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics",
    color_sync = "/System/Library/Frameworks/ColorSync.framework/Versions/A/ColorSync",
    video = "/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo",
    iokit = "/System/Library/Frameworks/IOKit.framework/Versions/A/IOKit",
    carbon = "/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon",
    services = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices",
    audio_unit = "/System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit",
    audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox",
    opengl = "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL",
    iconv_library = "/usr/lib/libiconv.2.dylib", libsystem = "/usr/lib/libSystem.B.dylib";
constexpr std::uint64_t failed = UINT64_MAX;
void require(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices darwin{process};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    std::unique_ptr<alchemy::FoundationBridge> foundation;
    std::vector<Address> buffers;
    std::size_t baseline{};
    explicit Fixture(bool jit) {
        engine.set_jit_enabled(jit);
        alchemy::install_darwin_numeric(process);
        foundation = std::make_unique<alchemy::FoundationBridge>(process, objc, cf);
        const auto before = process.objects().live_count();
        alchemy::install_darwin_frameworks(process, objc, cf);
        baseline = process.objects().live_count();
        require(baseline == before, "framework constants are not immortal");
    }
    // Objects created by the tests themselves, independent of other providers.
    std::size_t live() { return process.objects().live_count() - baseline; }
    ~Fixture() {
        for (const auto value : buffers) process.release_guest(value);
        objc.cleanup_thread(*process.thread(engine));
    }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing framework symbol " + std::string(name));
        return found->address;
    }
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return engine.invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    Address buffer(std::uint64_t size) { const auto value = process.allocate_guest(size); buffers.push_back(value); return value; }
    Address text(std::string_view value) {
        const auto address = buffer(value.size() + 1);
        engine.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        static_cast<transmute::GuestMemory&>(engine).write8(address + value.size(), 0);
        return address;
    }
    Address bytes(std::span<const std::uint8_t> value) {
        const auto address = buffer(value.size() + 1);
        engine.write_bytes(address, value);
        return address;
    }
    std::vector<std::uint8_t> read(Address address, std::size_t size) {
        std::vector<std::uint8_t> value(size);
        if (size) engine.read_bytes(address, value);
        return value;
    }
    std::uint32_t error() { return engine.read32(process.thread(engine)->errno_address); }
    std::string constant(std::string_view library, std::string_view name) { return cf.utf8(engine.read64(symbol(library, name))); }
};

void identities() {
    for (const auto library : alchemy::darwin_framework_libraries)
        require(alchemy::ProcessContext::is_provider_library(library), "framework identity is not a provider");
    require(!alchemy::ProcessContext::is_provider_library("/System/Library/Frameworks/Vision.framework/Versions/A/Vision") ||
            alchemy::ProcessContext::probe_mode() != alchemy::ProcessContext::ProbeMode::off,
            "an unprovided framework was accepted as a provider");
}

void classes_and_constants(bool jit) {
    Fixture f(jit);
    const auto object = f.objc.find_class("NSObject");
    const auto super = [&](Address cls) { return f.engine.read64(cls + 8); };
    const auto view = f.symbol(appkit, "_OBJC_CLASS_$_NSView"), gl_view = f.symbol(appkit, "_OBJC_CLASS_$_NSOpenGLView");
    const auto window = f.symbol(appkit, "_OBJC_CLASS_$_NSWindow"), responder = f.objc.find_class("NSResponder");
    require(view == f.objc.find_class("NSView") && super(gl_view) == view && super(view) == responder &&
            super(window) == responder && super(responder) == object, "AppKit class hierarchy is wrong");
    require(f.symbol(appkit, "_OBJC_METACLASS_$_NSView") == f.engine.read64(view), "AppKit metaclass export differs");
    require(f.symbol("/System/Library/Frameworks/Cocoa.framework/Versions/A/Cocoa", "_OBJC_CLASS_$_NSWindow") == window,
            "Cocoa umbrella does not re-export AppKit classes");
    require(super(f.symbol(quartz, "_OBJC_CLASS_$_CAMetalLayer")) == f.objc.find_class("CALayer"), "CAMetalLayer is not a CALayer");
    require(super(f.symbol(metal, "_OBJC_CLASS_$_MTLRenderPassColorAttachmentDescriptor")) ==
            f.objc.find_class("MTLRenderPassAttachmentDescriptor"), "Metal descriptor hierarchy is wrong");
    // Instances of framework classes are ordinary NSObject instances.
    const auto send = f.symbol(alchemy::objc_library, "_objc_msgSend");
    const auto descriptor = f.engine.invoke_callback(send, std::array<Address, 2>{f.symbol(metal, "_OBJC_CLASS_$_MTLTextureDescriptor"), f.objc.selector("new")});
    require(descriptor && f.objc.object_class(descriptor) == f.objc.find_class("MTLTextureDescriptor"), "Metal descriptor did not instantiate");
    static_cast<void>(f.engine.invoke_callback(send, std::array<Address, 2>{descriptor, f.objc.selector("release")}));
    require(f.engine.read64(f.symbol(appkit, "_NSApp")) == 0, "NSApp is not nil in a headless process");
    require(f.engine.read32(f.symbol(iokit, "_kIOMainPortDefault")) == 0, "kIOMainPortDefault is not MACH_PORT_NULL");
    require(super(f.symbol(metal, "_OBJC_CLASS_$_MTLAccelerationStructureMotionTriangleGeometryDescriptor")) ==
            f.objc.find_class("MTLAccelerationStructureGeometryDescriptor") && f.objc.find_class("MTLMotionKeyframeData"),
            "macOS 12 Metal classes are missing");
    require(f.constant(appkit, "_NSPasteboardTypeString") == "public.utf8-plain-text" &&
            f.constant(appkit, "_NSWindowWillCloseNotification") == "NSWindowWillCloseNotification", "AppKit string constants are wrong");
    require(f.constant(graphics, "_kCGColorSpaceExtendedSRGB") == "kCGColorSpaceExtendedSRGB" &&
            f.constant(color_sync, "_kColorSyncDisplayDeviceClass") == "mntr" &&
            f.constant(video, "_kCVPixelBufferMetalCompatibilityKey") == "MetalCompatibility" &&
            f.constant(carbon, "_kTISPropertyUnicodeKeyLayoutData") == "TISPropertyUnicodeKeyLayoutData" &&
            f.constant(services, "_kLSSharedFileListFavoriteItems") == "com.apple.LSSharedFileList.FavoriteItems",
            "framework string constants are wrong");
    require(f.live() == 0, "framework class or constant checks leaked objects");
}

void devices(bool jit) {
    Fixture f(jit);
    require(f.call(metal, "_MTLCreateSystemDefaultDevice") == 0, "headless Metal reported a device");
    const auto all = f.call(metal, "_MTLCopyAllDevices");
    require(all && f.cf.length(all) == 0, "MTLCopyAllDevices is not an empty array");
    f.process.objects().release(all);

    // OpenAL: no device can open, and the NULL device answers queries.
    require(f.call(openal, "_alcGetError", {0}) == 0, "initial ALC error is not ALC_NO_ERROR");
    require(f.call(openal, "_alcOpenDevice", {0}) == 0, "headless OpenAL opened a device");
    require(f.call(openal, "_alcGetError", {0}) == 0xa004 && f.call(openal, "_alcGetError", {0}) == 0,
            "alcGetError did not report and clear ALC_INVALID_VALUE");
    require(f.call(openal, "_alcIsExtensionPresent", {0, f.text("ALC_ENUMERATION_EXT")}) == 0, "an ALC extension was reported");
    const auto list = f.call(openal, "_alcGetString", {0, 0x1005});
    require(list && f.read(list, 2) == std::vector<std::uint8_t>{0, 0}, "device specifier list is not empty");
    const auto message = f.call(openal, "_alcGetString", {0, 0xa001});
    require(f.read(message, 15) == std::vector<std::uint8_t>{'I', 'n', 'v', 'a', 'l', 'i', 'd', ' ', 'D', 'e', 'v', 'i', 'c', 'e', 0},
            "ALC error string is wrong");
    require(f.call(openal, "_alcGetString", {0, 0x7777}) == 0 && f.call(openal, "_alcGetError", {0}) == 0xa003, "bad ALC enum accepted");
    const auto version = f.buffer(8);
    f.engine.write32(version, 77);
    static_cast<void>(f.call(openal, "_alcGetIntegerv", {0, 0x1000, 1, version}));
    require(f.engine.read32(version) == 1 && f.call(openal, "_alcGetError", {0}) == 0, "ALC major version query failed");
    static_cast<void>(f.call(openal, "_alcGetIntegerv", {0, 0x1007, 1, version}));
    require(f.call(openal, "_alcGetError", {0}) == 0xa001, "device attribute query without a device succeeded");
    require(f.call(openal, "_alcCreateContext", {0x1234, 0}) == 0 && f.call(openal, "_alcGetError", {0}) == 0xa001, "context created");
    require(f.call(openal, "_alcMakeContextCurrent", {0}) == 1, "clearing the current ALC context failed");
    require(f.call(openal, "_alcMakeContextCurrent", {0x1234}) == 0 && f.call(openal, "_alcGetError", {0}) == 0xa002, "invalid context made current");
    require(f.call(openal, "_alcGetEnumValue", {0, f.text("ALC_DEVICE_SPECIFIER")}) == 0x1005, "ALC enum lookup failed");
    require(f.call(openal, "_alGetError") == 0xa004, "alGetError without a context is not AL_INVALID_OPERATION");
    require(f.call(openal, "_alGetEnumValue", {f.text("AL_FORMAT_STEREO16")}) == 0x1103 &&
            f.call(openal, "_alGetEnumValue", {f.text("AL_FORMAT_STEREO_FLOAT32")}) == 0, "AL enum lookup is wrong");
    const auto names = f.buffer(8);
    f.engine.write32(names, 0x5a5a5a5a);
    static_cast<void>(f.call(openal, "_alGenSources", {1, names}));
    require(f.engine.read32(names) == 0x5a5a5a5a, "alGenSources wrote names without a context");

    // CoreGraphics and ColorSync: zero displays.
    const auto count = f.buffer(4);
    f.engine.write32(count, 9);
    require(f.call(graphics, "_CGGetActiveDisplayList", {8, f.buffer(32), count}) == 0 && f.engine.read32(count) == 0,
            "CGGetActiveDisplayList reported displays");
    require(f.call(graphics, "_CGGetActiveDisplayList", {0, 0, 0}) == 1001, "CGGetActiveDisplayList accepted a NULL count");
    f.engine.write32(count, 9);
    require(f.call(graphics, "_CGGetDisplaysWithRect", {8, f.buffer(32), count}) == 0 && f.engine.read32(count) == 0,
            "CGGetDisplaysWithRect reported displays");
    require(f.call(graphics, "_CGMainDisplayID") == 0 && f.call(graphics, "_CGDisplayPixelsWide", {1}) == 0 &&
            f.call(graphics, "_CGDisplayCopyDisplayMode", {1}) == 0 && f.call(graphics, "_CGDisplayModeGetWidth", {0}) == 0,
            "a display or display mode exists headless");
    require(f.call(graphics, "_CGDisplayRegisterReconfigurationCallback", {0, 0}) == 1001,
            "CGDisplayRegisterReconfigurationCallback accepted a NULL callback");
    require(f.call(graphics, "_CGDisplayIOServicePort", {1}) == 0 && f.call(color_sync, "_CGDisplayCreateUUIDFromDisplayID", {1}) == 0 &&
            f.call(color_sync, "_ColorSyncDeviceCopyDeviceInfo", {0, 0}) == 0, "display queries found a display");
    const auto extended = f.engine.read64(f.symbol(graphics, "_kCGColorSpaceExtendedSRGB"));
    const auto space = f.call(graphics, "_CGColorSpaceCreateWithName", {extended});
    require(space != 0, "CGColorSpaceCreateWithName rejected a standard name");
    const auto name = f.call(graphics, "_CGColorSpaceCopyName", {space});
    require(f.cf.utf8(name) == "kCGColorSpaceExtendedSRGB", "color space lost its name");
    f.process.objects().release(name);
    static_cast<void>(f.call(graphics, "_CGColorSpaceRelease", {space}));
    static_cast<void>(f.call(graphics, "_CGColorSpaceRelease", {0}));
    const auto unknown = f.cf.string("kCGColorSpaceNotAName");
    require(f.call(graphics, "_CGColorSpaceCreateWithName", {unknown}) == 0, "unknown color space name accepted");
    f.process.objects().release(unknown);

    // IOKit: real matching dictionaries, empty registry.
    const auto matching = f.call(iokit, "_IORegistryEntryIDMatching", {0x1234'5678'9abcULL});
    const auto key = f.cf.string("IORegistryEntryID");
    const auto number = f.cf.dictionary_value(matching, key);
    const auto value = f.buffer(8);
    static_cast<void>(f.cf.invoke("CFNumberGetValue", std::array<Address, 3>{number, 4, value}));
    require(f.engine.read64(value) == 0x1234'5678'9abcULL, "IORegistryEntryIDMatching stored the wrong ID");
    f.process.objects().release(key);
    require(f.call(iokit, "_IOServiceGetMatchingService", {0, matching}) == 0, "IOKit found a service");

    // CoreVideo, OpenGL, audio and input sources fail with documented codes.
    const auto out = f.buffer(8);
    f.engine.write64(out, 0x77);
    require(static_cast<std::int32_t>(f.call(video, "_CVPixelBufferCreate", {0, 16, 16, 0x42475241, 0, out})) == -6663 &&
            f.engine.read64(out) == 0, "CVPixelBufferCreate did not fail as unsupported");
    require(f.call(opengl, "_CGLTexImageIOSurface2D", {0}) == 10004, "CGL accepted a context");
    require(f.call(audio_unit, "_AudioComponentFindNext", {0, f.buffer(20)}) == 0, "an audio component was found");
    f.engine.write64(out, 0x77);
    require(static_cast<std::int32_t>(f.call(audio_unit, "_AudioComponentInstanceNew", {0, out})) == -50 && f.engine.read64(out) == 0,
            "audio unit instance created");
    require(static_cast<std::int32_t>(f.call(audio_unit, "_AudioOutputUnitStart", {0})) == static_cast<std::int32_t>(0x80008001U),
            "audio unit start did not reject the instance");
    f.engine.write64(out, 0x77);
    require(static_cast<std::int32_t>(f.call(audio_toolbox, "_CAClockNew", {0, out})) == -4 && f.engine.read64(out) == 0, "CAClock created");
    require(f.call(carbon, "_TISCopyCurrentKeyboardLayoutInputSource") == 0 && f.call(carbon, "_GetCurrentEventButtonState") == 0,
            "headless input source or button state reported");
    // LaunchServices lists exist for known types and are always empty.
    require(f.call(services, "_LSSharedFileListCreate", {0, 0, 0}) == 0, "LaunchServices list created without a type");
    const auto bogus = f.cf.string("com.example.NotAList");
    require(f.call(services, "_LSSharedFileListCreate", {0, bogus, 0}) == 0, "LaunchServices list created for an unknown type");
    f.process.objects().release(bogus);
    const auto favorites = f.engine.read64(f.symbol(services, "_kLSSharedFileListFavoriteItems"));
    const auto shared = f.call(services, "_LSSharedFileListCreate", {0, favorites, 0});
    require(shared != 0, "LaunchServices favorites list not created");
    const auto seed = f.buffer(4);
    f.engine.write32(seed, 0x77);
    const auto snapshot = f.call(services, "_LSSharedFileListCopySnapshot", {shared, seed});
    require(snapshot && f.cf.length(snapshot) == 0 && f.engine.read32(seed) == 0, "LaunchServices snapshot is not an empty array");
    require(f.call(services, "_LSSharedFileListCopySnapshot", {0, 0}) == 0, "snapshot of a NULL list");
    f.process.objects().release(snapshot);
    // Blender releases the list with CFRelease.
    const std::array<std::uint64_t, 1> release{shared};
    static_cast<void>(f.cf.invoke("CFRelease", release));
    require(f.live() == 0, "framework device queries leaked objects");
}

void math_aliases(bool jit) {
    Fixture f(jit);
    for (const auto* name : {"_exp2", "_log2", "_modff", "_nextafterf", "_feclearexcept", "_fetestexcept"}) {
        const auto system = f.process.resolve(libsystem, name);
        require(!system || system->address == f.symbol(services, name), "CoreServices math symbol differs from libSystem");
    }
    if (!f.process.resolve(libsystem, "_remquo")) {
        const auto quotient = f.buffer(4);
        f.engine.set_initial_vector(0, {std::bit_cast<std::uint64_t>(7.0), 0});
        f.engine.set_initial_vector(1, {std::bit_cast<std::uint64_t>(2.0), 0});
        static_cast<void>(f.call(services, "_remquo", {quotient}));
        require(f.engine.read32(quotient) == 4, "remquo quotient is wrong");
    }
}

struct Iconv {
    Fixture& f;
    Address handle{}, in{}, in_left{}, out{}, out_left{};
    explicit Iconv(Fixture& fixture) : f(fixture), in(f.buffer(8)), in_left(f.buffer(8)), out(f.buffer(8)), out_left(f.buffer(8)) {}
    Address open(std::string_view to, std::string_view from) { return f.call(iconv_library, "_iconv_open", {f.text(to), f.text(from)}); }
    struct Result { std::uint64_t value{}; std::vector<std::uint8_t> bytes; std::uint64_t consumed{}; std::uint32_t error{}; };
    Result run(Address cd, std::span<const std::uint8_t> input, std::uint64_t room = 64) {
        const auto source = f.bytes(input), target = f.buffer(room + 1);
        f.engine.write64(in, source); f.engine.write64(in_left, input.size());
        f.engine.write64(out, target); f.engine.write64(out_left, room);
        f.engine.write32(f.process.thread(f.engine)->errno_address, 0);
        Result result;
        result.value = f.call(iconv_library, "_iconv", {cd, in, in_left, out, out_left});
        result.consumed = f.engine.read64(in) - source;
        require(f.engine.read64(in_left) == input.size() - result.consumed, "iconv input counters disagree");
        const auto produced = f.engine.read64(out) - target;
        require(f.engine.read64(out_left) == room - produced, "iconv output counters disagree");
        result.bytes = f.read(target, static_cast<std::size_t>(produced));
        result.error = f.error();
        return result;
    }
    Result run(Address cd, std::string_view input, std::uint64_t room = 64) {
        return run(cd, std::span(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()), room);
    }
};

void iconv(bool jit) {
    Fixture f(jit);
    Iconv i(f);
    using Bytes = std::vector<std::uint8_t>;
    const auto utf16 = i.open("UTF-16LE", "UTF-8");
    require(utf16 != failed, "iconv_open rejected UTF-8 to UTF-16LE");
    auto r = i.run(utf16, "A\xc3\xa9\xf0\x9f\x98\x80");
    require(r.value == 0 && r.consumed == 7 && r.bytes == Bytes{'A', 0, 0xe9, 0, 0x3d, 0xd8, 0x00, 0xde}, "UTF-8 to UTF-16LE is wrong");
    r = i.run(utf16, "ab\xc3");
    require(r.value == failed && r.error == 22 && r.consumed == 2 && r.bytes == Bytes{'a', 0, 'b', 0}, "truncated UTF-8 is not EINVAL");
    r = i.run(utf16, "a\xff" "b");
    require(r.value == failed && r.error == 92 && r.consumed == 1, "invalid UTF-8 is not EILSEQ");
    r = i.run(utf16, "abc", 5);
    require(r.value == failed && r.error == 7 && r.consumed == 2 && r.bytes.size() == 4, "full output is not E2BIG");
    require(f.call(iconv_library, "_iconv_close", {utf16}) == 0, "iconv_close failed");
    require(f.call(iconv_library, "_iconv_close", {utf16}) == failed && f.error() == 9, "closed iconv handle is not EBADF");

    const auto bom = i.open("UTF-16", "ISO-8859-1");
    r = i.run(bom, "\xe9");
    require(r.bytes == Bytes{0xfe, 0xff, 0x00, 0xe9}, "UTF-16 output lacks a big-endian BOM");
    r = i.run(bom, "z");
    require(r.bytes == Bytes{0x00, 'z'}, "UTF-16 BOM was repeated");
    require(f.call(iconv_library, "_iconv", {bom, 0, 0, 0, 0}) == 0, "iconv reset failed");
    r = i.run(bom, "z");
    require(r.bytes == Bytes{0xfe, 0xff, 0x00, 'z'}, "reset did not restore the initial state");
    static_cast<void>(f.call(iconv_library, "_iconv_close", {bom}));

    const auto from16 = i.open("UTF-8", "UTF-16");
    const std::array<std::uint8_t, 6> little{0xff, 0xfe, 'h', 0, 0xac, 0x20};
    r = i.run(from16, little);
    require(r.value == 0 && r.bytes == Bytes{'h', 0xe2, 0x82, 0xac}, "UTF-16 BOM detection failed");
    static_cast<void>(f.call(iconv_library, "_iconv_close", {from16}));

    const auto ascii = i.open("ASCII", "UTF-8");
    r = i.run(ascii, "x\xc3\xa9y");
    require(r.value == failed && r.error == 92 && r.consumed == 1 && r.bytes == Bytes{'x'}, "unrepresentable character is not EILSEQ");
    static_cast<void>(f.call(iconv_library, "_iconv_close", {ascii}));
    const auto translit = i.open("ascii//TRANSLIT", "utf-8");
    r = i.run(translit, "x\xc3\xa9y");
    require(r.value == 1 && r.bytes == Bytes{'x', '?', 'y'}, "//TRANSLIT did not substitute");
    static_cast<void>(f.call(iconv_library, "_iconv_close", {translit}));
    const auto ignore = i.open("LATIN1//IGNORE", "UTF-32LE");
    const std::array<std::uint8_t, 12> wide{'a', 0, 0, 0, 0x00, 0x01, 0, 0, 0xff, 0, 0, 0};
    r = i.run(ignore, wide);
    require(r.value == 1 && r.bytes == Bytes{'a', 0xff}, "//IGNORE did not skip");
    static_cast<void>(f.call(iconv_library, "_iconv_close", {ignore}));

    require(i.open("EBCDIC-US", "UTF-8") == failed && f.error() == 22, "unknown charset is not EINVAL");
    require(f.call(iconv_library, "_iconv", {0x1234, 0, 0, 0, 0}) == failed && f.error() == 9, "invalid iconv_t is not EBADF");
    require(f.symbol(iconv_library, "_libiconv_open") != 0, "libiconv aliases are missing");
}
} // namespace

int main() {
    try {
        identities();
        for (const bool jit : {false, true}) { classes_and_constants(jit); devices(jit); math_aliases(jit); iconv(jit); }
        std::cout << "Alchemy framework provider tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
