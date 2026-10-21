#include "alchemy/darwin/kit/completion/data/providers.hpp"
#include "alchemy/darwin/coreservices/uttype.hpp"

#include <map>
#include <mutex>

// UniformTypeIdentifiers' UTType class and its type constants (UTTypePNG,
// UTTypeMIDI, ...), over the table of uttype.cpp; CoreServices'
// UTCreateStringForOSType and the kUTType* identifier constants it lacks.
// LSCopyApplicationURLsForURL answers NULL ("no applications found"):
// Alchemy keeps no Launch Services registry of applications, as
// coreservices.cpp's LSCopyDefaultApplicationURLForURL.
// UTType objects are unique per identifier (immortal), as Foundation
// returns the same object for a declared type.
namespace alchemy::gui::completion_kit::data {
namespace {

constexpr std::string_view uti_library = "/System/Library/Frameworks/UniformTypeIdentifiers.framework/Versions/A/UniformTypeIdentifiers";
constexpr std::string_view core_services_library = "/System/Library/Frameworks/CoreServices.framework/Versions/A/CoreServices";

struct TypeState final : ObjectPayload { std::string identifier; };

struct Types {
    explicit Types(Support& support) : s(support) {}
    Support& s;
    Guest g{s};
    Address cls{};
    std::mutex mutex;
    std::map<std::string, Address> unique;   // lower-case identifier -> immortal UTType

    Address type(const std::string& identifier) {
        std::string key = identifier;
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::lock_guard lock(mutex);
        if (const auto found = unique.find(key); found != unique.end()) return found->second;
        const auto* declared = uttype::find(identifier);
        const auto made = s.objc.allocate(cls);
        s.objc.host_state<TypeState>(made)->identifier = declared ? declared->identifier : identifier;
        unique.emplace(key, made);   // never released
        return made;
    }
    std::string id(Address self) { return s.objc.host_state<TypeState>(self)->identifier; }
    std::string id_of_argument(Address type_object) {
        if (!type_object) fail("NSInvalidArgumentException: UTType: nil type");
        if (const auto st = s.objc.find_host_state<TypeState>(type_object)) return st->identifier;
        return g.utf8(s.send_id(type_object, "identifier"));
    }
};

} // namespace

void install_uttypes(Support& support) {
    ProcessContext::add_provider_library(std::string(uti_library));   // QLab links the framework
    auto ty = std::make_shared<Types>(support);
    const auto ut = support.declare_class("UTType", "NSObject", uti_library);
    ty->cls = ut;

    // ---- factories ----
    support.method(ut, "typeWithIdentifier:", [ty](CpuState& s, GuestMemory&) {
        const auto identifier = ty->g.utf8(s.x[2]);
        s.x[0] = uttype::find(identifier) || uttype::is_dynamic(identifier) ? ty->type(identifier) : 0;
    }, "@@:@", true);
    for (const auto* name : {"exportedTypeWithIdentifier:", "importedTypeWithIdentifier:"})
        support.method(ut, name, [ty](CpuState& s, GuestMemory&) { s.x[0] = ty->type(ty->g.utf8(s.x[2])); }, "@@:@", true);
    const auto by_tag = [ty](uttype::Tag tag, Address value, Address conforming) -> Address {
        if (!value) return 0;
        const auto to = conforming ? ty->id_of_argument(conforming) : std::string("public.data");
        const auto found = uttype::for_tag(tag, ty->g.utf8(value), to);
        return found ? ty->type(*found) : 0;
    };
    support.method(ut, "typeWithFilenameExtension:", [by_tag](CpuState& s, GuestMemory&) { s.x[0] = by_tag(uttype::Tag::filename_extension, s.x[2], 0); }, "@@:@", true);
    support.method(ut, "typeWithFilenameExtension:conformingToType:", [by_tag](CpuState& s, GuestMemory&) {
        s.x[0] = by_tag(uttype::Tag::filename_extension, s.x[2], s.x[3]);
    }, "@@:@@", true);
    support.method(ut, "typeWithMIMEType:", [by_tag](CpuState& s, GuestMemory&) { s.x[0] = by_tag(uttype::Tag::mime_type, s.x[2], 0); }, "@@:@", true);
    support.method(ut, "typeWithMIMEType:conformingToType:", [by_tag](CpuState& s, GuestMemory&) { s.x[0] = by_tag(uttype::Tag::mime_type, s.x[2], s.x[3]); }, "@@:@@", true);

    // ---- properties ----
    support.method(ut, "identifier", [ty](CpuState& s, GuestMemory&) { s.x[0] = ty->g.auto_string(std::string_view(ty->id(s.x[0]))); }, "@@:");
    support.method(ut, "preferredFilenameExtension", [ty](CpuState& s, GuestMemory&) {
        const auto tag = uttype::preferred_tag(ty->id(s.x[0]), uttype::Tag::filename_extension);
        s.x[0] = tag ? ty->g.auto_string(std::string_view(*tag)) : 0;
    }, "@@:");
    support.method(ut, "preferredMIMEType", [ty](CpuState& s, GuestMemory&) {
        const auto tag = uttype::preferred_tag(ty->id(s.x[0]), uttype::Tag::mime_type);
        s.x[0] = tag ? ty->g.auto_string(std::string_view(*tag)) : 0;
    }, "@@:");
    support.method(ut, "localizedDescription", [ty](CpuState& s, GuestMemory&) { s.x[0] = ty->g.auto_string(std::string_view(uttype::description(ty->id(s.x[0])))); }, "@@:");
    support.method(ut, "tags", [ty](CpuState& s, GuestMemory&) {
        const auto identifier = ty->id(s.x[0]);
        std::vector<std::pair<Address, Address>> entries;
        for (const auto& [key, tag] : {std::pair{"public.filename-extension", uttype::Tag::filename_extension}, std::pair{"public.mime-type", uttype::Tag::mime_type}}) {
            std::vector<Address> values;
            for (const auto& v : uttype::tags(identifier, tag)) values.push_back(ty->g.auto_string(std::string_view(v)));
            if (!values.empty()) entries.emplace_back(ty->s.cf.constant_string(key), ty->g.auto_array(values));
        }
        s.x[0] = ty->g.autorelease(ty->s.cf.dictionary(entries));
    }, "@@:");
    support.method(ut, "supertypes", [ty](CpuState& s, GuestMemory&) {
        std::vector<Address> types;
        for (const auto& super : uttype::supertypes(ty->id(s.x[0]))) types.push_back(ty->type(super));
        s.x[0] = ty->g.set_of(ty->s.objc.find_class("NSSet"), types);
    }, "@@:");
    support.method(ut, "conformsToType:", [ty](CpuState& s, GuestMemory&) { return_bool(s, uttype::conforms(ty->id(s.x[0]), ty->id_of_argument(s.x[2]))); }, "B@:@");
    support.method(ut, "isSupertypeOfType:", [ty](CpuState& s, GuestMemory&) { return_bool(s, uttype::conforms(ty->id_of_argument(s.x[2]), ty->id(s.x[0]))); }, "B@:@");
    support.method(ut, "isSubtypeOfType:", [ty](CpuState& s, GuestMemory&) {
        const auto self = ty->id(s.x[0]), other = ty->id_of_argument(s.x[2]);
        return_bool(s, uttype::conforms(self, other) && !uttype::conforms(other, self));
    }, "B@:@");
    support.method(ut, "isDeclared", [ty](CpuState& s, GuestMemory&) { return_bool(s, uttype::find(ty->id(s.x[0])) != nullptr); }, "B@:");
    support.method(ut, "isDynamic", [ty](CpuState& s, GuestMemory&) { return_bool(s, uttype::is_dynamic(ty->id(s.x[0]))); }, "B@:");
    support.method(ut, "isPublicType", [ty](CpuState& s, GuestMemory&) { return_bool(s, ty->id(s.x[0]).rfind("public.", 0) == 0); }, "B@:");
    support.method(ut, "isEqual:", [ty](CpuState& s, GuestMemory&) {
        const auto other = s.x[2] ? ty->s.objc.find_host_state<TypeState>(s.x[2]) : nullptr;
        return_bool(s, s.x[0] == s.x[2] || (other && other->identifier == ty->id(s.x[0])));
    }, "B@:@");
    support.method(ut, "hash", [ty](CpuState& s, GuestMemory&) { s.x[0] = std::hash<std::string>{}(ty->id(s.x[0])); }, "Q@:");
    support.method(ut, "description", [ty](CpuState& s, GuestMemory&) { s.x[0] = ty->g.auto_string(std::string_view(ty->id(s.x[0]))); }, "@@:");
    support.method(ut, "copyWithZone:", [](CpuState&, GuestMemory&) {}, "@@:^v");   // immortal and immutable: self
    support.method(ut, "supportsSecureCoding", [](CpuState& s, GuestMemory&) { s.x[0] = 1; }, "B@:", true);

    // ---- UTType constants (UniformTypeIdentifiers) ----
    static const std::pair<const char*, const char*> constants[] = {
        {"UTTypeItem", "public.item"}, {"UTTypeContent", "public.content"}, {"UTTypeCompositeContent", "public.composite-content"},
        {"UTTypeData", "public.data"}, {"UTTypeDirectory", "public.directory"}, {"UTTypeFolder", "public.folder"}, {"UTTypeVolume", "public.volume"},
        {"UTTypeSymbolicLink", "public.symlink"}, {"UTTypePackage", "com.apple.package"}, {"UTTypeBundle", "com.apple.bundle"},
        {"UTTypeApplication", "com.apple.application"}, {"UTTypeApplicationBundle", "com.apple.application-bundle"},
        {"UTTypeFramework", "com.apple.framework"}, {"UTTypePluginBundle", "com.apple.plugin"}, {"UTTypeExecutable", "public.executable"},
        {"UTTypeUnixExecutable", "public.unix-executable"}, {"UTTypeURL", "public.url"}, {"UTTypeFileURL", "public.file-url"},
        {"UTTypeText", "public.text"}, {"UTTypePlainText", "public.plain-text"}, {"UTTypeUTF8PlainText", "public.utf8-plain-text"},
        {"UTTypeUTF16PlainText", "public.utf16-plain-text"}, {"UTTypeCommaSeparatedText", "public.comma-separated-values-text"},
        {"UTTypeTabSeparatedText", "public.tab-separated-values-text"}, {"UTTypeHTML", "public.html"}, {"UTTypeXML", "public.xml"},
        {"UTTypeJSON", "public.json"}, {"UTTypeRTF", "public.rtf"}, {"UTTypeRTFD", "com.apple.rtfd"}, {"UTTypeSourceCode", "public.source-code"},
        {"UTTypeScript", "public.script"}, {"UTTypeShellScript", "public.shell-script"}, {"UTTypePythonScript", "public.python-script"},
        {"UTTypeJavaScript", "com.netscape.javascript-source"}, {"UTTypeVCard", "public.vcard"}, {"UTTypePropertyList", "com.apple.property-list"},
        {"UTTypeXMLPropertyList", "com.apple.xml-property-list"}, {"UTTypeBinaryPropertyList", "com.apple.binary-property-list"},
        {"UTTypeLog", "public.log"}, {"UTTypeImage", "public.image"}, {"UTTypePNG", "public.png"}, {"UTTypeJPEG", "public.jpeg"},
        {"UTTypeTIFF", "public.tiff"}, {"UTTypeGIF", "com.compuserve.gif"}, {"UTTypeBMP", "com.microsoft.bmp"}, {"UTTypeICO", "com.microsoft.ico"},
        {"UTTypeICNS", "com.apple.icns"}, {"UTTypeHEIC", "public.heic"}, {"UTTypeHEIF", "public.heif-standard"}, {"UTTypeWebP", "org.webmproject.webp"},
        {"UTTypeSVG", "public.svg-image"}, {"UTTypeRAWImage", "public.camera-raw-image"}, {"UTTypePDF", "com.adobe.pdf"},
        {"UTTypeArchive", "public.archive"}, {"UTTypeZIP", "public.zip-archive"}, {"UTTypeGZIP", "org.gnu.gnu-zip-archive"},
        {"UTTypeDiskImage", "public.disk-image"}, {"UTTypeAudiovisualContent", "public.audiovisual-content"}, {"UTTypeMovie", "public.movie"},
        {"UTTypeVideo", "public.video"}, {"UTTypeMPEG", "public.mpeg"}, {"UTTypeMPEG4Movie", "public.mpeg-4"}, {"UTTypeQuickTimeMovie", "com.apple.quicktime-movie"},
        {"UTTypeAVI", "public.avi"}, {"UTTypeMPEG2TransportStream", "public.mpeg-2-transport-stream"}, {"UTTypeAudio", "public.audio"},
        {"UTTypeMP3", "public.mp3"}, {"UTTypeMPEG4Audio", "public.mpeg-4-audio"}, {"UTTypeAppleProtectedMPEG4Audio", "com.apple.protected-mpeg-4-audio"},
        {"UTTypeAIFF", "public.aiff-audio"}, {"UTTypeWAV", "com.microsoft.waveform-audio"}, {"UTTypeMIDI", "public.midi-audio"},
        {"UTTypePlaylist", "public.playlist"}, {"UTTypeM3UPlaylist", "public.m3u-playlist"}, {"UTTypeFont", "public.font"},
        {"UTTypeDatabase", "public.database"}, {"UTTypeSpreadsheet", "public.spreadsheet"}, {"UTTypePresentation", "public.presentation"},
        {"UTTypeBookmark", "public.bookmark"}, {"UTTypeInternetLocation", "public.internet-location"}, {"UTTypeAliasFile", "com.apple.alias-file"},
        {"UTTypeResolvable", "com.apple.resolvable"}, {"UTTypeX509Certificate", "public.x509-certificate"},
        {"UTTypeContact", "public.contact"}, {"UTTypeMessage", "public.message"}, {"UTTypeCalendarEvent", "public.calendar-event"},
    };
    for (const auto& [symbol, identifier] : constants) {
        const auto name = std::string("_") + symbol;
        if (support.process.resolve(uti_library, name)) continue;
        const auto slot = support.process.allocate_guest(8);
        support.process.main_engine().write64(slot, ty->type(identifier));
        support.process.install_export(std::string(uti_library), name, {slot, ExportKind::data});
    }

    // ---- CoreServices ----
    for (const auto& [symbol, identifier] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kUTTypeApplication", "com.apple.application"}, {"_kUTTypePNG", "public.png"}, {"_kUTTypeJPEG", "public.jpeg"},
             {"_kUTTypeTIFF", "public.tiff"}, {"_kUTTypeGIF", "com.compuserve.gif"}, {"_kUTTypeImage", "public.image"},
             {"_kUTTypeFolder", "public.folder"}, {"_kUTTypeData", "public.data"}, {"_kUTTypeItem", "public.item"},
             {"_kUTTypeApplicationBundle", "com.apple.application-bundle"}, {"_kUTTypePlainText", "public.plain-text"},
             {"_kUTTypeUTF8PlainText", "public.utf8-plain-text"}, {"_kUTTypeURL", "public.url"}, {"_kUTTypeFileURL", "public.file-url"},
             {"_kUTTypeAudio", "public.audio"}, {"_kUTTypeMovie", "public.movie"}, {"_kUTTypePDF", "com.adobe.pdf"}}) {
        if (!support.process.resolve(core_services_library, symbol)) support.string_constant(core_services_library, symbol, identifier);
    }
    for (const auto& [symbol, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kUTTagClassFilenameExtension", "public.filename-extension"}, {"_kUTTagClassMIMEType", "public.mime-type"},
             {"_kUTTagClassOSType", "com.apple.ostype"}}) {
        if (!support.process.resolve(core_services_library, symbol)) support.string_constant(core_services_library, symbol, value);
    }
    // CFStringRef UTCreateStringForOSType(OSType): the four characters (+1).
    support.fill_function(core_services_library, "_UTCreateStringForOSType", [ty](CpuState& s, GuestMemory&) {
        const auto code = static_cast<std::uint32_t>(s.x[0]);
        std::string text;
        for (int shift = 24; shift >= 0; shift -= 8) text.push_back(static_cast<char>((code >> shift) & 0xff));
        s.x[0] = ty->s.cf.string(text);
    });
    // OSType UTGetOSTypeFromString(CFStringRef)
    support.fill_function(core_services_library, "_UTGetOSTypeFromString", [ty](CpuState& s, GuestMemory&) {
        const auto text = ty->g.utf8(s.x[0]);
        std::uint32_t code = 0;
        if (text.size() == 4) for (const auto c : text) code = (code << 8) | static_cast<unsigned char>(c);
        s.x[0] = code;
    });
    // CFArrayRef LSCopyApplicationURLsForURL(CFURLRef, LSRolesMask): no registered applications.
    support.fill_function(core_services_library, "_LSCopyApplicationURLsForURL", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
}

} // namespace alchemy::gui::completion_kit::data
