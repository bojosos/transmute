#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

#include "alchemy/host/locale.hpp"

#include <array>
#include <string>
#include <vector>

// CoreFoundation (headless-correct): the system encoding and the encodings
// the provider converts, CFStringCreateMutableCopy, CFStringNormalize over the
// host normalizer, and the toll-free bridged calls answered by the Foundation
// provider's classes: kCFAllocatorNull, CFAutorelease, the NoCopy strings,
// encoding names, CFBundle over NSBundle, CFURL over NSURL, and
// CFUserNotificationDisplayAlert (no user session: the text goes to stderr).
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

// CFString.h
constexpr std::uint32_t encoding_mac_roman = 0, encoding_latin1 = 0x201, encoding_ascii = 0x600,
    encoding_utf8 = 0x08000100, encoding_utf16 = 0x100, encoding_utf16be = 0x10000100,
    encoding_utf16le = 0x14000100, encoding_utf32 = 0x0c000100, encoding_utf32be = 0x18000100,
    encoding_utf32le = 0x1c000100;


class StringsAndBundles final : public Provider {
public:
    using Provider::Provider;
    static void default_allocator(Address allocator) {
        if (allocator) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
    }
    void install() {
        // A US-English macOS reports Mac Roman as the legacy system encoding.
        function(core_foundation_library, "_CFStringGetSystemEncoding", [](CpuState& s, GuestMemory&) {
            s.x[0] = encoding_mac_roman;
        });
        // The encodings the CoreFoundation provider converts.
        function(core_foundation_library, "_CFStringIsEncodingAvailable", [](CpuState& s, GuestMemory&) {
            const auto encoding = static_cast<std::uint32_t>(s.x[0]);
            s.x[0] = encoding == encoding_ascii || encoding == encoding_utf8 || encoding == encoding_latin1 ||
                encoding == encoding_utf16 || encoding == encoding_utf16be || encoding == encoding_utf16le ||
                encoding == encoding_utf32 || encoding == encoding_utf32be || encoding == encoding_utf32le;
        });
        function(core_foundation_library, "_CFStringCreateMutableCopy", [this](CpuState& s, GuestMemory&) {
            if (!s.x[2]) throw std::runtime_error("Alchemy CoreFoundation: CFStringCreateMutableCopy of NULL");
            if (s.x[1] && cf.length(s.x[2]) > s.x[1])
                throw std::runtime_error("Alchemy CoreFoundation: CFStringCreateMutableCopy beyond maxLength");
            s.x[0] = cf.mutable_copy(s.x[2]);
        });
        // CFStringNormalize over the host Unicode normalizer (host::locale::normalize).
        function(core_foundation_library, "_CFStringNormalize", [this](CpuState& s, GuestMemory&) {
            // CFStringNormalizationForm: D=0, KD=1, C=2, KC=3.
            if (s.x[1] > 3) throw std::runtime_error("Alchemy CoreFoundation: invalid CFStringNormalizationForm");
            auto units = cf.units(s.x[0]);
            if (units.empty()) return;
            using host::locale::Form;
            constexpr Form forms[] = {Form::d, Form::kd, Form::c, Form::kc};
            const std::u16string input(units.begin(), units.end());
            const auto output = host::locale::normalize(input, forms[s.x[1]]);
            if (!output) throw std::runtime_error("Alchemy CoreFoundation: CFStringNormalize failed (invalid UTF-16)");
            cf.replace_string_units(s.x[0], std::vector<std::uint16_t>(output->begin(), output->end()));
        });

        const auto null_allocator = context.allocate_guest(16);
        memory().fill_bytes(null_allocator, 0, 16);
        const auto null_slot = context.allocate_guest(8);
        memory().write64(null_slot, null_allocator);
        context.install_export(std::string(core_foundation_library), "_kCFAllocatorNull", {null_slot, ExportKind::data});
        function(core_foundation_library, "_CFAutorelease", [this](CpuState& s, GuestMemory&) {
            if (!s.x[0]) throw std::runtime_error("Alchemy CoreFoundation: CFAutorelease of NULL");
            s.x[0] = guest_call(objc_library, "_objc_autorelease", {s.x[0]});
        });
        // NoCopy: the characters are copied, then the buffer goes to its
        // deallocator at once (kCFAllocatorNull: the caller keeps it).
        function(core_foundation_library, "_CFStringCreateWithCStringNoCopy", [this, null_allocator](CpuState& s, GuestMemory&) {
            default_allocator(s.x[0]);
            const auto deallocator = s.x[3];
            const auto result = cf.invoke("CFStringCreateWithCString", std::array<Address, 3>{0, s.x[1], s.x[2]});
            if (deallocator != null_allocator) {
                default_allocator(deallocator);
                static_cast<void>(guest_call(libsystem, "_free", {s.x[1]}));
            }
            s.x[0] = result;
        });
        function(core_foundation_library, "_CFStringGetNameOfEncoding", [this](CpuState& s, GuestMemory&) {
            static constexpr std::pair<std::uint32_t, std::string_view> names[] = {
                {encoding_mac_roman, "Western (Mac OS Roman)"}, {encoding_latin1, "Western (ISO Latin 1)"},
                {encoding_ascii, "Western (ASCII)"}, {encoding_utf8, "Unicode (UTF-8)"},
                {encoding_utf16, "Unicode (UTF-16)"}, {encoding_utf16be, "Unicode (UTF-16BE)"},
                {encoding_utf16le, "Unicode (UTF-16LE)"}, {encoding_utf32, "Unicode (UTF-32)"},
                {encoding_utf32be, "Unicode (UTF-32BE)"}, {encoding_utf32le, "Unicode (UTF-32LE)"}};
            const auto wanted = static_cast<std::uint32_t>(s.x[0]);
            s.x[0] = 0;
            for (const auto& [encoding, name] : names)
                if (encoding == wanted) { s.x[0] = constant_string(name); return; }
        });
        // CFBundle is toll-free bridged with the Foundation provider's NSBundle.
        function(core_foundation_library, "_CFBundleGetMainBundle", [this](CpuState& s, GuestMemory&) {
            s.x[0] = send(class_named("NSBundle"), "mainBundle");
        });
        function(core_foundation_library, "_CFBundleCreate", [this](CpuState& s, GuestMemory&) {
            default_allocator(s.x[0]);
            if (!s.x[1]) { s.x[0] = 0; return; }
            const auto path = cf.string(cf.url_path(s.x[1]));
            const auto bundle = send(class_named("NSBundle"), "bundleWithPath:", {path});
            context.objects().release(path);
            s.x[0] = retained(bundle);
        });
        function(core_foundation_library, "_CFBundleCopyBundleURL", [this](CpuState& s, GuestMemory&) {
            s.x[0] = s.x[0] ? retained(send(s.x[0], "bundleURL")) : 0;
        });
        function(core_foundation_library, "_CFBundleCopyResourceURL", [this](CpuState& s, GuestMemory&) {
            const auto path = s.x[0] ? send(s.x[0], "pathForResource:ofType:inDirectory:", {s.x[1], s.x[2], s.x[3]}) : 0;
            s.x[0] = path ? cf.file_url(cf.utf8(path), false) : 0;
        });
        function(core_foundation_library, "_CFBundleGetValueForInfoDictionaryKey", [this](CpuState& s, GuestMemory&) {
            s.x[0] = s.x[0] && s.x[1] ? send(s.x[0], "objectForInfoDictionaryKey:", {s.x[1]}) : 0;
        });
        // CFURL over the provider's NSURL (file URLs; other schemes are NULL,
        // as from -[NSURL URLWithString:] in that provider).
        function(core_foundation_library, "_CFURLCreateWithString", [this](CpuState& s, GuestMemory&) {
            default_allocator(s.x[0]);
            if (s.x[2]) throw std::runtime_error("Alchemy CoreFoundation: CFURLCreateWithString with a base URL is unsupported");
            s.x[0] = s.x[1] ? retained(send(class_named("NSURL"), "URLWithString:", {s.x[1]})) : 0;
        });
        function(core_foundation_library, "_CFURLCreateWithBytes", [this](CpuState& s, GuestMemory&) {
            default_allocator(s.x[0]);
            if (s.x[4]) throw std::runtime_error("Alchemy CoreFoundation: CFURLCreateWithBytes with a base URL is unsupported");
            const auto text = cf.invoke("CFStringCreateWithBytes", std::array<Address, 5>{0, s.x[1], s.x[2], s.x[3], 0});
            if (!text) { s.x[0] = 0; return; }
            const auto url = send(class_named("NSURL"), "URLWithString:", {text});
            context.objects().release(text);
            s.x[0] = retained(url);
        });
        function(core_foundation_library, "_CFURLCreateData", [this](CpuState& s, GuestMemory& m) {
            default_allocator(s.x[0]);
            const auto text = cf.invoke("CFURLGetString", std::array<Address, 1>{s.x[1]});
            const auto length = cf.length(text);
            const auto size = cf.invoke("CFStringGetMaximumSizeForEncoding", std::array<Address, 2>{length, s.x[2]});
            const auto buffer = context.allocate_guest(std::max<Address>(size, 1));
            const auto used = context.allocate_guest(8);
            m.write64(used, 0);
            static_cast<void>(cf.invoke("CFStringGetBytes", std::array<Address, 9>{text, 0, length, s.x[2], '?', 0, buffer, size, used}));
            s.x[0] = cf.invoke("CFDataCreate", std::array<Address, 3>{0, buffer, m.read64(used)});
            context.release_guest(used);
            context.release_guest(buffer);
        });
        function(core_foundation_library, "_CFURLCreateCopyDeletingLastPathComponent", [this](CpuState& s, GuestMemory&) {
            default_allocator(s.x[0]);
            s.x[0] = s.x[1] ? retained(send(s.x[1], "URLByDeletingLastPathComponent")) : 0;
        });
        // CFAttributedStringCreate: CoreText's headless shell (darwin/shells.hpp).
        // No user session can show an alert: the text goes to stderr and the
        // call reports failure with the cancel response.
        function(core_foundation_library, "_CFUserNotificationDisplayAlert", [this](CpuState& s, GuestMemory& m) {
            std::string text = "Alchemy: CFUserNotificationDisplayAlert (no user session):";
            for (const auto object : {s.x[4], s.x[5]})
                if (object && cf.kind(object) == ObjectKind::string) text += " " + cf.utf8(object);
            text += "\n";
            static_cast<void>(context.output(2, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
            const auto flags = m.read64(s.sp + 8); // after otherButtonTitle; timeout is in d0
            if (flags) m.write64(flags, 3); // kCFUserNotificationCancelResponse
            s.x[0] = static_cast<std::uint32_t>(-1);
        });
    }
};

} // namespace

void install_corefoundation_strings_and_bundles(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<StringsAndBundles>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
