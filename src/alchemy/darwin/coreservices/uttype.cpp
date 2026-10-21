#include "alchemy/darwin/coreservices/uttype.hpp"

#include <algorithm>
#include <cctype>

namespace alchemy::gui::completion_kit::uttype {
namespace {

std::string lower(std::string_view v) {
    std::string out(v);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
bool same(std::string_view a, std::string_view b) { return lower(a) == lower(b); }
constexpr std::string_view dynamic_ext = "dyn.alchemy-ext-", dynamic_mime = "dyn.alchemy-mime-";

} // namespace

const std::vector<Declaration>& declarations() {
    static const std::vector<Declaration> table{
        {"public.item", {}, {}, {}, {}, "item"},
        {"public.content", {}, {}, {}, {}, "content"},
        {"public.composite-content", {"public.content"}, {}, {}, {}, "mixed content"},
        {"public.data", {"public.item"}, {}, {"application/octet-stream"}, {}, "data"},
        {"public.directory", {"public.item"}, {}, {}, {}, "directory"},
        {"public.folder", {"public.directory"}, {}, {}, {"fold"}, "folder"},
        {"public.volume", {"public.folder"}, {}, {}, {"disk"}, "volume"},
        {"public.symlink", {"public.item"}, {}, {}, {}, "symbolic link"},
        {"com.apple.package", {"public.directory"}, {}, {}, {}, "package"},
        {"com.apple.bundle", {"public.directory"}, {"bundle"}, {}, {}, "bundle"},
        {"com.apple.application", {"public.data"}, {}, {}, {}, "application"},
        {"com.apple.application-bundle", {"com.apple.application", "com.apple.bundle", "com.apple.package"}, {"app"}, {}, {"APPL"}, "application"},
        {"com.apple.framework", {"com.apple.bundle"}, {"framework"}, {}, {}, "framework"},
        {"com.apple.plugin", {"com.apple.bundle", "com.apple.package"}, {"plugin"}, {}, {}, "plug-in"},
        {"public.executable", {"public.data"}, {}, {}, {}, "executable"},
        {"public.unix-executable", {"public.data", "public.executable"}, {}, {}, {}, "Unix executable"},
        {"com.apple.mach-o-binary", {"public.data", "public.executable"}, {}, {}, {}, "Mach-O binary"},
        {"public.url", {"public.data"}, {}, {}, {}, "URL"},
        {"public.file-url", {"public.url"}, {}, {}, {}, "file URL"},
        {"public.text", {"public.data", "public.content"}, {}, {"text/*"}, {}, "text"},
        {"public.plain-text", {"public.text"}, {"txt", "text"}, {"text/plain"}, {"TEXT"}, "plain text"},
        {"public.utf8-plain-text", {"public.plain-text"}, {}, {"text/plain;charset=utf-8"}, {}, "Unicode text"},
        {"public.utf16-plain-text", {"public.plain-text"}, {}, {}, {"utxt"}, "Unicode text"},
        {"public.utf16-external-plain-text", {"public.plain-text"}, {"utxt"}, {}, {}, "Unicode text"},
        {"public.delimited-values-text", {"public.text"}, {}, {}, {}, "delimited text"},
        {"public.comma-separated-values-text", {"public.delimited-values-text"}, {"csv"}, {"text/csv"}, {}, "comma-separated values"},
        {"public.tab-separated-values-text", {"public.delimited-values-text"}, {"tsv"}, {"text/tab-separated-values"}, {}, "tab-separated values"},
        {"public.html", {"public.text"}, {"html", "htm", "shtml"}, {"text/html"}, {"HTML"}, "HTML text"},
        {"public.xml", {"public.text"}, {"xml"}, {"application/xml", "text/xml"}, {}, "XML text"},
        {"public.json", {"public.text"}, {"json"}, {"application/json"}, {}, "JSON"},
        {"public.rtf", {"public.text"}, {"rtf"}, {"text/rtf"}, {"RTF "}, "rich text"},
        {"com.apple.rtfd", {"com.apple.package", "public.composite-content"}, {"rtfd"}, {}, {}, "rich text with attachments"},
        {"public.source-code", {"public.plain-text"}, {}, {}, {}, "source code"},
        {"public.script", {"public.source-code"}, {}, {}, {}, "script"},
        {"public.shell-script", {"public.script"}, {"sh", "command"}, {"application/x-sh"}, {}, "shell script"},
        {"public.python-script", {"public.script"}, {"py"}, {"text/x-python-script"}, {}, "Python script"},
        {"com.netscape.javascript-source", {"public.source-code"}, {"js"}, {"text/javascript"}, {}, "JavaScript"},
        {"public.vcard", {"public.text"}, {"vcf", "vcard"}, {"text/vcard"}, {}, "vCard"},
        {"com.apple.property-list", {"public.data"}, {"plist"}, {}, {}, "property list"},
        {"com.apple.xml-property-list", {"public.xml", "com.apple.property-list"}, {}, {}, {}, "XML property list"},
        {"com.apple.binary-property-list", {"com.apple.property-list"}, {}, {}, {}, "binary property list"},
        {"public.log", {"public.plain-text"}, {"log"}, {}, {}, "log file"},
        {"public.image", {"public.data", "public.content"}, {}, {"image/*"}, {}, "image"},
        {"public.png", {"public.image"}, {"png"}, {"image/png"}, {"PNGf"}, "PNG image"},
        {"public.jpeg", {"public.image"}, {"jpeg", "jpg", "jpe"}, {"image/jpeg"}, {"JPEG"}, "JPEG image"},
        {"public.tiff", {"public.image"}, {"tiff", "tif"}, {"image/tiff"}, {"TIFF"}, "TIFF image"},
        {"com.compuserve.gif", {"public.image"}, {"gif"}, {"image/gif"}, {"GIFf"}, "GIF image"},
        {"com.microsoft.bmp", {"public.image"}, {"bmp", "dib"}, {"image/bmp"}, {"BMP "}, "Windows bitmap image"},
        {"com.microsoft.ico", {"public.image"}, {"ico"}, {"image/vnd.microsoft.icon"}, {}, "Windows icon image"},
        {"com.apple.icns", {"public.image"}, {"icns"}, {}, {"icns"}, "Apple icon image"},
        {"public.heic", {"public.heif-standard"}, {"heic"}, {"image/heic"}, {}, "HEIC image"},
        {"public.heif-standard", {"public.image"}, {"heif"}, {"image/heif"}, {}, "HEIF image"},
        {"org.webmproject.webp", {"public.image"}, {"webp"}, {"image/webp"}, {}, "WebP image"},
        {"com.ilm.openexr-image", {"public.image"}, {"exr"}, {"image/x-exr"}, {}, "OpenEXR image"},
        {"public.radiance", {"public.image"}, {"hdr", "pic"}, {}, {}, "Radiance HDR image"},
        {"public.camera-raw-image", {"public.image"}, {}, {}, {}, "raw image"},
        {"public.svg-image", {"public.image", "public.xml"}, {"svg"}, {"image/svg+xml"}, {}, "SVG image"},
        {"com.adobe.photoshop-image", {"public.image"}, {"psd"}, {"image/vnd.adobe.photoshop"}, {"8BPS"}, "Adobe Photoshop image"},
        {"com.adobe.pdf", {"public.data", "public.composite-content"}, {"pdf"}, {"application/pdf"}, {"PDF "}, "PDF document"},
        {"public.archive", {"public.data"}, {}, {}, {}, "archive"},
        {"public.zip-archive", {"com.pkware.zip-archive"}, {}, {}, {}, "zip archive"},
        {"com.pkware.zip-archive", {"public.data", "public.archive"}, {"zip"}, {"application/zip"}, {}, "zip archive"},
        {"org.gnu.gnu-zip-archive", {"public.data", "public.archive"}, {"gz", "gzip"}, {"application/x-gzip"}, {}, "gzip archive"},
        {"public.tar-archive", {"public.data", "public.archive"}, {"tar"}, {"application/x-tar"}, {}, "tar archive"},
        {"com.apple.disk-image-udif", {"public.disk-image"}, {"dmg"}, {"application/x-apple-diskimage"}, {}, "disk image"},
        {"public.disk-image", {"public.data", "public.archive"}, {}, {}, {}, "disk image"},
        {"public.audiovisual-content", {"public.data", "public.content"}, {}, {}, {}, "audiovisual content"},
        {"public.movie", {"public.audiovisual-content"}, {}, {"video/*"}, {}, "movie"},
        {"public.video", {"public.movie"}, {}, {}, {}, "video"},
        {"public.mpeg", {"public.movie"}, {"mpg", "mpeg", "m75", "m15"}, {"video/mpeg"}, {"MPG "}, "MPEG movie"},
        {"public.mpeg-4", {"public.movie"}, {"mp4", "mpg4"}, {"video/mp4"}, {"mpg4"}, "MPEG-4 movie"},
        {"com.apple.m4v-video", {"public.mpeg-4"}, {"m4v"}, {"video/x-m4v"}, {"M4V "}, "MPEG-4 video"},
        {"com.apple.quicktime-movie", {"public.movie"}, {"mov", "qt"}, {"video/quicktime"}, {"MooV"}, "QuickTime movie"},
        {"public.avi", {"public.movie"}, {"avi", "vfw"}, {"video/avi", "video/x-msvideo"}, {"VfW "}, "AVI movie"},
        {"public.mpeg-2-transport-stream", {"public.movie"}, {"ts"}, {"video/mp2t"}, {}, "MPEG-2 transport stream"},
        {"public.audio", {"public.audiovisual-content"}, {}, {"audio/*"}, {}, "audio"},
        {"public.mp3", {"public.audio"}, {"mp3"}, {"audio/mpeg"}, {"MPG3"}, "MP3 audio"},
        {"public.mpeg-4-audio", {"public.mpeg-4", "public.audio"}, {"m4a", "m4b"}, {"audio/mp4", "audio/x-m4a"}, {"M4A "}, "MPEG-4 audio"},
        {"com.apple.m4a-audio", {"public.mpeg-4-audio"}, {}, {}, {}, "Apple MPEG-4 audio"},
        {"com.apple.protected-mpeg-4-audio", {"public.audio"}, {"m4p"}, {"audio/x-m4p"}, {"M4P "}, "protected MPEG-4 audio"},
        {"public.aiff-audio", {"public.aifc-audio"}, {"aiff", "aif"}, {"audio/aiff", "audio/x-aiff"}, {"AIFF"}, "AIFF audio"},
        {"public.aifc-audio", {"public.audio"}, {"aifc", "aiffc"}, {"audio/x-aifc"}, {"AIFC"}, "AIFF-C audio"},
        {"com.microsoft.waveform-audio", {"public.audio"}, {"wav", "wave"}, {"audio/vnd.wave", "audio/wav", "audio/x-wav"}, {"WAVE"}, "Waveform audio"},
        {"com.apple.coreaudio-format", {"public.audio"}, {"caf"}, {"audio/x-caf"}, {"caff"}, "Apple Core Audio format"},
        {"public.midi-audio", {"public.audio"}, {"mid", "midi", "smf", "kar"}, {"audio/midi", "audio/x-midi"}, {"Midi"}, "MIDI audio"},
        {"org.xiph.flac", {"public.audio"}, {"flac"}, {"audio/flac"}, {}, "FLAC audio"},
        {"public.ulaw-audio", {"public.audio"}, {"au", "snd", "ulw"}, {"audio/basic"}, {"ULAW"}, "\xc2\xb5-law audio"},
        {"public.playlist", {"public.data"}, {}, {}, {}, "playlist"},
        {"public.m3u-playlist", {"public.text", "public.playlist"}, {"m3u", "m3u8"}, {"audio/mpegurl", "audio/x-mpegurl"}, {}, "M3U playlist"},
        {"public.font", {"public.data", "public.content"}, {}, {}, {}, "font"},
        {"public.truetype-ttf-font", {"public.font"}, {"ttf"}, {"font/ttf"}, {"tfil"}, "TrueType font"},
        {"public.opentype-font", {"public.font"}, {"otf"}, {"font/otf"}, {"OTTO"}, "OpenType font"},
        {"public.database", {"public.data"}, {}, {}, {}, "database"},
        {"public.calendar-event", {"public.item"}, {}, {}, {}, "calendar event"},
        {"public.contact", {"public.item"}, {}, {}, {}, "contact"},
        {"public.message", {"public.item"}, {}, {}, {}, "message"},
        {"public.presentation", {"public.composite-content"}, {}, {}, {}, "presentation"},
        {"public.spreadsheet", {"public.content"}, {}, {}, {}, "spreadsheet"},
        {"public.bookmark", {"public.item"}, {}, {}, {}, "bookmark"},
        {"public.internet-location", {"public.data"}, {}, {}, {}, "internet location"},
        {"com.apple.alias-file", {"public.data"}, {}, {}, {}, "alias"},
        {"com.apple.resolvable", {}, {}, {}, {}, "resolvable"},
        {"public.x509-certificate", {"public.data"}, {"cer", "crt", "der"}, {"application/x-x509-ca-cert"}, {}, "certificate"},
    };
    return table;
}

const Declaration* find(std::string_view identifier) {
    for (const auto& d : declarations()) if (same(d.identifier, identifier)) return &d;
    return nullptr;
}

bool is_dynamic(std::string_view identifier) { return lower(identifier).rfind("dyn.", 0) == 0; }

std::vector<std::string> supertypes(std::string_view identifier) {
    std::vector<std::string> out;
    std::vector<std::string> pending;
    if (const auto* d = find(identifier)) for (const auto* p : d->parents) pending.emplace_back(p);
    else if (is_dynamic(identifier)) pending = {"public.data"};
    while (!pending.empty()) {
        auto next = std::move(pending.back());
        pending.pop_back();
        if (std::find(out.begin(), out.end(), next) != out.end()) continue;
        if (const auto* d = find(next)) for (const auto* p : d->parents) pending.emplace_back(p);
        out.push_back(std::move(next));
    }
    return out;
}

bool conforms(std::string_view type, std::string_view to) {
    if (same(type, to)) return true;
    const auto all = supertypes(type);
    return std::any_of(all.begin(), all.end(), [&](const std::string& s) { return same(s, to); });
}

namespace {
const std::vector<const char*>& tag_list(const Declaration& d, Tag tag) {
    return tag == Tag::filename_extension ? d.extensions : tag == Tag::mime_type ? d.mime : d.ostypes;
}
} // namespace

std::optional<std::string> for_tag(Tag tag, std::string_view value, std::string_view conforming_to) {
    const auto wanted = tag == Tag::ostype ? std::string(value) : lower(value);
    for (const auto& d : declarations())
        for (const auto* t : tag_list(d, tag))
            if ((tag == Tag::ostype ? std::string(t) : lower(t)) == wanted && (conforming_to.empty() || conforms(d.identifier, conforming_to))) return std::string(d.identifier);
    if (!conforming_to.empty() && !same(conforming_to, "public.data") && !same(conforming_to, "public.item")) return std::nullopt;
    std::string dynamic(tag == Tag::mime_type ? dynamic_mime : dynamic_ext);
    for (const auto c : wanted) dynamic += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    return dynamic;
}

std::vector<std::string> tags(std::string_view identifier, Tag tag) {
    std::vector<std::string> out;
    if (const auto* d = find(identifier)) { for (const auto* t : tag_list(*d, tag)) out.emplace_back(t); return out; }
    const auto id = lower(identifier);
    if (tag == Tag::filename_extension && id.rfind(dynamic_ext, 0) == 0) out.push_back(id.substr(dynamic_ext.size()));
    return out;
}

std::optional<std::string> preferred_tag(std::string_view identifier, Tag tag) {
    auto all = tags(identifier, tag);
    if (all.empty()) return std::nullopt;
    return all.front();
}

std::string description(std::string_view identifier) {
    if (const auto* d = find(identifier)) return d->description;
    if (const auto ext = preferred_tag(identifier, Tag::filename_extension)) {
        auto upper = *ext;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return upper + " file";
    }
    return "data";
}

} // namespace alchemy::gui::completion_kit::uttype
