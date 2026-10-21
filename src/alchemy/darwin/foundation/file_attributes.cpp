#include "alchemy/darwin/kit/completion/completion_kit.hpp"

#include <cmath>
#include <span>
#include <string>
#include <utility>
#include <vector>

// NSFileManager item attributes (QLab's license controller checks its shared
// directory), added when NSFileManager lacks them. They come from the guest's
// own lstat/stat (libSystem's view of the root, Darwin arm64 struct stat):
// NSFileType, NSFileSize, NSFileModificationDate, NSFileCreationDate,
// NSFilePosixPermissions, NSFileReferenceCount, NSFileSystemNumber,
// NSFileSystemFileNumber, NSFileOwnerAccountID, NSFileGroupOwnerAccountID,
// NSFileImmutable, NSFileAppendOnly, NSFileExtensionHidden (NO).
// -setAttributes:ofItemAtPath:error: sets NSFilePosixPermissions (chmod) and
// NSFileModificationDate (utimes), and owner/group names or IDs (getpwnam /
// getgrnam, chown: the emulated libSystem's single unprivileged user makes
// most of these fail, returning NO with an error); other keys fail
// explicitly. Missing items
// give nil / NO with an NSCocoaErrorDomain error (260 not found, 512 other).
// NSDictionary (NSFileAttributes): the accessors read those keys (0 / nil /
// NO when absent, as Foundation).
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";
constexpr double unix_to_reference = 978307200.0;   // seconds from 1970 to 2001

struct Stat {
    std::uint32_t mode{}, nlink{}, uid{}, gid{}, flags{};
    std::int32_t dev{};
    std::uint64_t ino{};
    std::int64_t size{};
    double modified{}, born{};   // reference seconds
};

struct Files {
    Support& support;

    template <class F>
    auto with_path(Address path_object, F&& use) {
        const auto path = support.cf.utf8(path_object);
        const auto block = support.process.allocate_guest(path.size() + 1);
        try {
            support.memory().write_bytes(block, std::span(reinterpret_cast<const std::uint8_t*>(path.c_str()), path.size() + 1));
            auto result = use(block);
            support.process.release_guest(block);
            return result;
        } catch (...) { support.process.release_guest(block); throw; }
    }
    std::uint64_t errno_value() {
        const auto location = support.call(libsystem, "___error", {});
        return location ? support.memory().read32(location) : 0;
    }
    // lstat (or stat when following links); nullopt with errno.
    std::optional<Stat> stat(Address path_object, bool follow, std::uint64_t& error) {
        const auto buffer = support.process.allocate_guest(144);
        try {
            const auto result = with_path(path_object, [&](Address path) {
                return static_cast<std::int32_t>(support.call(libsystem, follow ? "_stat" : "_lstat", {path, buffer}));
            });
            if (result != 0) { error = errno_value(); support.process.release_guest(buffer); return std::nullopt; }
            auto& m = support.memory();
            const auto time = [&](Address at) { return static_cast<double>(static_cast<std::int64_t>(m.read64(buffer + at))) +
                                                       static_cast<double>(static_cast<std::int64_t>(m.read64(buffer + at + 8))) / 1e9 - unix_to_reference; };
            Stat value;
            value.dev = static_cast<std::int32_t>(m.read32(buffer));
            const auto mode_nlink = m.read32(buffer + 4);
            value.mode = mode_nlink & 0xffff;
            value.nlink = mode_nlink >> 16;
            value.ino = m.read64(buffer + 8);
            value.uid = m.read32(buffer + 16);
            value.gid = m.read32(buffer + 20);
            value.modified = time(48);
            value.born = time(80);
            value.size = static_cast<std::int64_t>(m.read64(buffer + 96));
            value.flags = m.read32(buffer + 116);
            support.process.release_guest(buffer);
            return value;
        } catch (...) { support.process.release_guest(buffer); throw; }
    }
    Address key(const char* name) { return support.send_id(support.cf.string(name), "autorelease"); }
    Address error(std::uint64_t code, Address path) {
        const auto error_class = support.objc.find_class("NSError");
        if (!error_class) return 0;
        std::vector<std::pair<Address, Address>> entries;
        if (path) entries.emplace_back(key("NSFilePath"), path);
        const auto info = support.send_id(support.cf.dictionary(entries), "autorelease");
        CallRegs registers;
        registers.x[2] = key("NSCocoaErrorDomain");
        registers.x[3] = code;
        registers.x[4] = info;
        return support.send(error_class, "errorWithDomain:code:userInfo:", registers).x0;
    }
    Address attributes(const Stat& st) {
        const char* type = "NSFileTypeUnknown";
        switch (st.mode & 0170000) {
        case 0040000: type = "NSFileTypeDirectory"; break;
        case 0100000: type = "NSFileTypeRegular"; break;
        case 0120000: type = "NSFileTypeSymbolicLink"; break;
        case 0140000: type = "NSFileTypeSocket"; break;
        case 0020000: type = "NSFileTypeCharacterSpecial"; break;
        case 0060000: type = "NSFileTypeBlockSpecial"; break;
        default: break;
        }
        auto& cf = support.cf;
        std::vector<std::pair<Address, Address>> entries{
            {key("NSFileType"), key(type)},
            {key("NSFileSize"), cf.integer(st.size)},
            {key("NSFileModificationDate"), cf.date(st.modified)},
            {key("NSFileCreationDate"), cf.date(st.born)},
            {key("NSFilePosixPermissions"), cf.integer(st.mode & 07777)},
            {key("NSFileReferenceCount"), cf.integer(st.nlink)},
            {key("NSFileSystemNumber"), cf.integer(st.dev)},
            {key("NSFileSystemFileNumber"), cf.integer(static_cast<std::int64_t>(st.ino))},
            {key("NSFileOwnerAccountID"), cf.integer(st.uid)},
            {key("NSFileGroupOwnerAccountID"), cf.integer(st.gid)},
            {key("NSFileImmutable"), cf.boolean((st.flags & 0x2) != 0)},       // UF_IMMUTABLE
            {key("NSFileAppendOnly"), cf.boolean((st.flags & 0x4) != 0)},      // UF_APPEND
            {key("NSFileExtensionHidden"), cf.boolean(false)}};
        const auto result = cf.dictionary(entries);
        for (const auto& entry : entries) support.release(entry.second);   // the dictionary keeps them
        return support.send_id(result, "autorelease");
    }
};

} // namespace

void install_file_attributes(Support& support) {
    const auto manager = support.objc.find_class("NSFileManager");
    if (!manager) return;
    auto files = std::make_shared<Files>(Files{support});
    const auto responds = [&support, manager](const char* selector) {
        return support.call("/usr/lib/libobjc.A.dylib", "_class_getInstanceMethod", {manager, support.objc.selector(selector)}) != 0;
    };
    const auto attributes_at = [files](Address path, bool follow, Address error_out) -> Address {
        std::uint64_t code{};
        const auto st = path ? files->stat(path, follow, code) : std::nullopt;
        if (!st) {
            if (error_out) files->support.memory().write64(error_out, files->error(code == 2 ? 260 : 512, path));
            return 0;
        }
        return files->attributes(*st);
    };
    // NSDictionary (NSFileAttributes).
    if (const auto dictionary = support.objc.find_class("NSDictionary")) {
        auto* sp = &support;
        const auto lookup = [sp](Address self, const char* key) {
            return sp->send_id(self, "objectForKey:", {sp->send_id(sp->cf.string(key), "autorelease")});
        };
        struct Accessor { const char* selector; const char* key; char kind; };   // o object, q integer, B bool
        for (const auto& [selector, key, kind] : {Accessor{"fileType", "NSFileType", 'o'}, Accessor{"fileModificationDate", "NSFileModificationDate", 'o'},
                 Accessor{"fileCreationDate", "NSFileCreationDate", 'o'}, Accessor{"fileOwnerAccountName", "NSFileOwnerAccountName", 'o'},
                 Accessor{"fileGroupOwnerAccountName", "NSFileGroupOwnerAccountName", 'o'}, Accessor{"fileOwnerAccountID", "NSFileOwnerAccountID", 'o'},
                 Accessor{"fileGroupOwnerAccountID", "NSFileGroupOwnerAccountID", 'o'}, Accessor{"fileSize", "NSFileSize", 'q'},
                 Accessor{"filePosixPermissions", "NSFilePosixPermissions", 'q'}, Accessor{"fileSystemNumber", "NSFileSystemNumber", 'q'},
                 Accessor{"fileSystemFileNumber", "NSFileSystemFileNumber", 'q'}, Accessor{"fileHFSCreatorCode", "NSFileHFSCreatorCode", 'q'},
                 Accessor{"fileHFSTypeCode", "NSFileHFSTypeCode", 'q'}, Accessor{"fileExtensionHidden", "NSFileExtensionHidden", 'B'},
                 Accessor{"fileIsImmutable", "NSFileImmutable", 'B'}, Accessor{"fileIsAppendOnly", "NSFileAppendOnly", 'B'}}) {
            const std::string attribute = key;
            const char type = kind;
            support.method(dictionary, selector, [sp, lookup, attribute, type](CpuState& s, GuestMemory&) {
                const auto value = lookup(s.x[0], attribute.c_str());
                if (type == 'o') { s.x[0] = value; return; }
                if (!value) { s.x[0] = 0; return; }
                s.x[0] = type == 'B' ? (sp->send(value, "boolValue").x0 & 0xff) : sp->send(value, "longLongValue").x0;
            }, type == 'o' ? "@@:" : type == 'B' ? "B@:" : "Q@:");
        }
    }
    // Mounted volumes: the root, then the guest's /Volumes entries (hidden
    // ones, named with a leading dot, are skipped with
    // NSVolumeEnumerationSkipHiddenVolumes = 2). Resource values are not
    // prefetched; the URLs answer them on request.
    if (!responds("mountedVolumeURLsIncludingResourceValuesForKeys:options:"))
        support.method(manager, "mountedVolumeURLsIncludingResourceValuesForKeys:options:", [files](CpuState& s, GuestMemory&) {
            auto& sp = files->support;
            std::vector<Address> urls{sp.cf.file_url("/", true)};
            const auto names = sp.send_id(s.x[0], "contentsOfDirectoryAtPath:error:", {files->key("/Volumes"), 0});
            if (names && sp.cf.kind(names) == ObjectKind::array)
                for (const auto name : sp.cf.array_values(names)) {
                    const auto text = sp.cf.utf8(name);
                    if (text.empty() || ((s.x[3] & 2) && text[0] == '.')) continue;
                    urls.push_back(sp.cf.file_url("/Volumes/" + text, true));
                }
            const auto array = sp.cf.array(urls);
            for (const auto url : urls) sp.release(url);
            s.x[0] = sp.send_id(array, "autorelease");
        }, "@@:@Q");
    // -createFileAtPath:contents:attributes:: the contents (nil: empty) written
    // atomically, then the attributes (their failures do not undo the file).
    if (!responds("createFileAtPath:contents:attributes:"))
        support.method(manager, "createFileAtPath:contents:attributes:", [files](CpuState& s, GuestMemory&) {
            auto& sp = files->support;
            if (!s.x[2]) { s.x[0] = 0; return; }
            auto contents = s.x[3];
            if (!contents) {
                CallRegs empty;
                contents = sp.send(sp.objc.find_class("NSData"), "data", empty).x0;
            }
            CallRegs write;
            write.x[2] = s.x[2];
            write.x[3] = 1;
            const bool written = (sp.send(contents, "writeToFile:atomically:", write).x0 & 0xff) != 0;
            if (written && s.x[4]) {
                CallRegs attributes;
                attributes.x[2] = s.x[4];
                attributes.x[3] = s.x[2];
                attributes.x[4] = 0;
                static_cast<void>(sp.send(s.x[0], "setAttributes:ofItemAtPath:error:", attributes));
            }
            s.x[0] = written;
        }, "B@:@@@");
    // iCloud: no ubiquity container exists (not signed in), so nothing is ubiquitous.
    if (!responds("isUbiquitousItemAtURL:"))
        support.method(manager, "isUbiquitousItemAtURL:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "B@:@");
    if (!responds("URLForUbiquityContainerIdentifier:"))
        support.method(manager, "URLForUbiquityContainerIdentifier:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:@");
    if (!responds("ubiquityIdentityToken"))
        support.method(manager, "ubiquityIdentityToken", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, "@@:");
    if (!responds("attributesOfItemAtPath:error:"))
        support.method(manager, "attributesOfItemAtPath:error:", [attributes_at](CpuState& s, GuestMemory&) { s.x[0] = attributes_at(s.x[2], false, s.x[3]); }, "@@:@^@");
    if (!responds("fileAttributesAtPath:traverseLink:"))
        support.method(manager, "fileAttributesAtPath:traverseLink:", [attributes_at](CpuState& s, GuestMemory&) {
            s.x[0] = attributes_at(s.x[2], (s.x[3] & 0xff) != 0, 0);
        }, "@@:@B");
    if (!responds("setAttributes:ofItemAtPath:error:"))
        support.method(manager, "setAttributes:ofItemAtPath:error:", [files](CpuState& s, GuestMemory& m) {
            auto& sp = files->support;
            bool ok = true;
            for (const auto& [key_object, value] : sp.cf.dictionary_entries(s.x[2])) {
                const auto name = sp.cf.utf8(key_object);
                std::int64_t result = 0;
                if (name == "NSFilePosixPermissions") {
                    const auto mode = static_cast<Address>(sp.send(value, "longLongValue").x0 & 07777);
                    result = files->with_path(s.x[3], [&](Address path) { return static_cast<std::int32_t>(sp.call(libsystem, "_chmod", {path, mode})); });
                } else if (name == "NSFileModificationDate") {
                    // struct timeval[2] {access, modification}, both set to the date.
                    const auto unix_time = sp.cf.date_time(value) + unix_to_reference;
                    const auto seconds = static_cast<std::int64_t>(std::floor(unix_time));
                    const auto micros = static_cast<std::int64_t>((unix_time - static_cast<double>(seconds)) * 1e6);
                    const auto times = sp.process.allocate_guest(32);
                    for (const Address at : {Address{0}, Address{16}}) { m.write64(times + at, static_cast<std::uint64_t>(seconds)); m.write64(times + at + 8, static_cast<std::uint64_t>(micros)); }
                    try { result = files->with_path(s.x[3], [&](Address path) { return static_cast<std::int32_t>(sp.call(libsystem, "_utimes", {path, times})); }); }
                    catch (...) { sp.process.release_guest(times); throw; }
                    sp.process.release_guest(times);
                } else if (name == "NSFileOwnerAccountName" || name == "NSFileGroupOwnerAccountName" ||
                           name == "NSFileOwnerAccountID" || name == "NSFileGroupOwnerAccountID") {
                    // Resolve the name (getpwnam / getgrnam), then chown; the
                    // emulated libSystem has one unprivileged user, so most
                    // changes fail as they would for such a process on macOS.
                    const bool owner = name.find("Owner") == 4;   // "NSFileOwner..." vs "NSFileGroupOwner..."
                    std::int64_t id = -1;
                    if (name.ends_with("Name")) {
                        const auto record = files->with_path(value, [&](Address text) {
                            return sp.call(libsystem, owner ? "_getpwnam" : "_getgrnam", {text});
                        });
                        if (record) id = m.read32(record + 16);   // pw_uid / gr_gid
                    } else {
                        id = static_cast<std::int64_t>(sp.send(value, "longLongValue").x0);
                    }
                    if (id < 0) {
                        if (s.x[4]) m.write64(s.x[4], files->error(512, s.x[3]));
                        ok = false;
                        break;
                    }
                    const auto user = owner ? static_cast<Address>(id) : static_cast<Address>(0xffffffff);
                    const auto group = owner ? static_cast<Address>(0xffffffff) : static_cast<Address>(id);
                    result = files->with_path(s.x[3], [&](Address path) { return static_cast<std::int32_t>(sp.call(libsystem, "_chown", {path, user, group})); });
                    if (result != 0) {
                        const auto code = files->errno_value() == 1 ? 513 : 512;   // EPERM -> NSFileWriteNoPermissionError
                        if (s.x[4]) m.write64(s.x[4], files->error(static_cast<std::uint64_t>(code), s.x[3]));
                        ok = false;
                        break;
                    }
                } else if (name == "NSFileExtensionHidden") {
                    if (sp.send(value, "boolValue").x0 & 0xff) unsupported("NSFileManager NSFileExtensionHidden=YES (no Finder flags)");
                } else {
                    unsupported("-[NSFileManager setAttributes:ofItemAtPath:error:] attribute " + name);
                }
                if (result != 0) {
                    ok = false;
                    if (s.x[4]) m.write64(s.x[4], files->error(512, s.x[3]));
                    break;
                }
            }
            s.x[0] = ok;
        }, "B@:@@^@");
}

} // namespace alchemy::gui::completion_kit
