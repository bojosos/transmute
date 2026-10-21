#include "alchemy/darwin/headless_additions.hpp"
#include "alchemy/darwin/headless_provider.hpp"

#include <cctype>
#include <optional>
#include <string>

// CoreServices (headless-correct): the UTType constants and table
// (conformance, preferred tags and identifiers), Gestalt (Alchemy's macOS
// 14.0.0 profile), and Launch Services with no registered applications.
// GetIconRef fails explicitly.
// Moved from darwin/identities/installer_frameworks.cpp (an installer
// binds them first); registered in both builds by install_darwin_frameworks.
namespace alchemy::darwin_headless {
namespace {

class TypesAndGestalt final : public Provider {
public:
    using Provider::Provider;
    struct UniformType { std::string_view identifier, extension, parent; };
    static constexpr UniformType uniform_types[] = {
        {"public.item", "", ""}, {"public.data", "", "public.item"}, {"public.content", "", ""},
        {"public.text", "", "public.data"}, {"public.plain-text", "txt", "public.text"},
        {"public.utf8-plain-text", "", "public.plain-text"}, {"public.utf16-plain-text", "", "public.plain-text"},
        {"public.utf16-external-plain-text", "", "public.plain-text"}, {"public.html", "html", "public.text"},
        {"public.xml", "xml", "public.text"}, {"public.image", "", "public.data"},
        {"public.png", "png", "public.image"}, {"public.jpeg", "jpg", "public.image"},
        {"public.tiff", "tiff", "public.image"}, {"com.microsoft.bmp", "bmp", "public.image"},
        {"com.compuserve.gif", "gif", "public.image"}, {"com.apple.icns", "icns", "public.image"},
        {"com.adobe.pdf", "pdf", "public.data"}, {"public.url", "", "public.data"},
        {"public.file-url", "", "public.url"}, {"public.zip-archive", "zip", "public.data"},
        {"public.folder", "", "public.item"}, {"com.apple.application-bundle", "app", "public.item"},
        {"public.unix-executable", "", "public.data"}, {"public.python-script", "py", "public.text"}};
    static const UniformType* uniform_type(std::string_view identifier) {
        for (const auto& type : uniform_types) {
            if (type.identifier.size() != identifier.size()) continue;
            bool same = true;
            for (std::size_t index = 0; index < identifier.size() && same; ++index)
                same = std::tolower(static_cast<unsigned char>(type.identifier[index])) == std::tolower(static_cast<unsigned char>(identifier[index]));
            if (same) return &type;
        }
        return nullptr;
    }

    void install() {
        for (const auto& [symbol, value] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
                 {"_kUTTagClassFilenameExtension", "public.filename-extension"}, {"_kUTTypeFileURL", "public.file-url"},
                 {"_kUTTypeHTML", "public.html"}, {"_kUTTypeImage", "public.image"}, {"_kUTTypePDF", "com.adobe.pdf"},
                 {"_kUTTypePlainText", "public.plain-text"}, {"_kUTTypeTIFF", "public.tiff"},
                 {"_kUTTypeUTF16ExternalPlainText", "public.utf16-external-plain-text"},
                 {"_kUTTypeUTF16PlainText", "public.utf16-plain-text"}, {"_kUTTypeUTF8PlainText", "public.utf8-plain-text"}})
            string_symbol(core_services, std::string(symbol), value);
        function(core_services, "_UTTypeConformsTo", [this](CpuState& s, GuestMemory&) {
            if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
            auto current = cf.utf8(s.x[0]);
            const auto wanted = cf.utf8(s.x[1]);
            for (int depth = 0; depth < 16; ++depth) {
                const auto* type = uniform_type(current);
                if (type ? uniform_type(wanted) == type : current == wanted) { s.x[0] = 1; return; }
                if (!type || type->parent.empty()) break;
                current = std::string(type->parent);
            }
            s.x[0] = 0;
        });
        function(core_services, "_UTTypeCopyPreferredTagWithClass", [this](CpuState& s, GuestMemory&) {
            const auto* type = s.x[0] ? uniform_type(cf.utf8(s.x[0])) : nullptr;
            const bool extension_class = s.x[1] && cf.utf8(s.x[1]) == "public.filename-extension";
            s.x[0] = type && extension_class && !type->extension.empty() ? cf.string(type->extension) : 0;
        });
        function(core_services, "_UTTypeCreatePreferredIdentifierForTag", [this](CpuState& s, GuestMemory&) {
            if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
            const auto tag_class = cf.utf8(s.x[0]);
            auto tag = cf.utf8(s.x[1]);
            for (auto& c : tag) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (tag_class == "public.filename-extension") {
                if (tag == "jpeg") tag = "jpg";
                if (tag == "htm") tag = "html";
                if (tag == "tif") tag = "tiff";
                for (const auto& type : uniform_types)
                    if (type.extension == tag) { s.x[0] = cf.string(type.identifier); return; }
            }
            // No declaration: a dynamic identifier, as on macOS (encoding differs).
            s.x[0] = cf.string("dyn.alchemy." + tag_class + "." + tag);
        });
        // Only identifiers are known; no declaration dictionaries.
        function(core_services, "_UTTypeCopyDeclaration", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
        // Gestalt: the macOS version of Alchemy's compatibility profile (14.0.0).
        function(core_services, "_Gestalt", [](CpuState& s, GuestMemory& m) {
            const auto selector = static_cast<std::uint32_t>(s.x[0]);
            std::optional<std::int32_t> value;
            if (selector == 0x73797376U) value = 0x1099;      // 'sysv' saturates at 10.9.9 on modern macOS
            else if (selector == 0x73797331U) value = 14;     // 'sys1'
            else if (selector == 0x73797332U) value = 0;      // 'sys2'
            else if (selector == 0x73797333U) value = 0;      // 'sys3'
            if (!value) { s.x[0] = static_cast<std::uint32_t>(-5551); return; } // gestaltUndefSelectorErr
            if (!s.x[1]) { s.x[0] = static_cast<std::uint32_t>(-50); return; }
            m.write32(s.x[1], static_cast<std::uint32_t>(*value));
            s.x[0] = 0;
        });
        // No applications are registered with Launch Services in Alchemy.
        constexpr std::int32_t application_not_found = -10814;
        function(core_services, "_LSCopyDefaultApplicationURLForContentType", [](CpuState& s, GuestMemory& m) {
            if (s.x[2]) m.write64(s.x[2], 0);
            s.x[0] = 0;
        });
        function(core_services, "_LSOpenCFURLRef", [this](CpuState& s, GuestMemory& m) {
            const auto text = std::string("Alchemy: LSOpenCFURLRef is unsupported (no Launch Services); the URL was not opened\n");
            static_cast<void>(context.output(2, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size())));
            if (s.x[1]) m.write64(s.x[1], 0);
            s.x[0] = static_cast<std::uint32_t>(application_not_found);
        });
        // LSGetExtensionInfo(length, unichars, *start): index after the last '.'.
        function(core_services, "_LSGetExtensionInfo", [](CpuState& s, GuestMemory& m) {
            const auto length = s.x[0];
            if (!s.x[1] || !s.x[2] || length > (1U << 20)) { s.x[0] = static_cast<std::uint32_t>(-50); return; }
            std::uint64_t start = length; // kLSInvalidExtensionIndex is ULONG_MAX; use length when no dot
            for (std::uint64_t index = length; index > 0; --index) {
                const auto unit = static_cast<std::uint16_t>(m.read8(s.x[1] + (index - 1) * 2) | (m.read8(s.x[1] + (index - 1) * 2 + 1) << 8));
                if (unit == '/') break;
                if (unit == '.') { start = index; break; }
            }
            m.write64(s.x[2], start == length ? UINT64_MAX : start);
            s.x[0] = 0;
        });
        unsupported(core_services, "_GetIconRef", "icon services draw icons");
    }
};

} // namespace

void install_coreservices_types_and_gestalt(ProcessContext& context, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<TypesAndGestalt>(context, objc, cf)->install();
}

} // namespace alchemy::darwin_headless
