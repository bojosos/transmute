#include "alchemy/gui/fill_provider.hpp"

#include "alchemy/darwin/coreservices/uttype.hpp"

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// CoreServices' File Manager references and the LaunchServices item queries,
// after Apple's public File Manager and Launch Services references, over the
// guest file system (libSystem stat / lstat / realpath / unlink / rmdir /
// mkdir, so paths are the guest's).
//   - FSRef (80 opaque bytes): a magic word and an index into a table of
//     resolved paths. FSPathMakeRef / FSPathMakeRefWithOptions (the object
//     must exist; kFSPathMakeRefDoNotFollowLeafSymlink), FSRefMakePath,
//     FSCompareFSRefs, FSDeleteObject (files and empty folders),
//     FSFindFolder (the user, local and system domains' standard folders
//     under $HOME, /Library and /System/Library, $TMPDIR for temporary
//     items; created when asked and missing), FSGetCatalogInfo (node flags,
//     dates, permissions, data sizes, the name and the parent; volumes,
//     node IDs and Finder info are not available and read as zero).
//   - LSCopyItemAttribute / LSCopyKindStringForURL / LSCopyItemInfoForURL:
//     the content type (from the UTI table by extension; folders,
//     applications, bundles and symbolic links by kind), display name and
//     kind, extension, invisibility (a leading dot or UF_HIDDEN); file
//     types and creators are '????'. LSSetItemAttribute changes nothing:
//     kLSAttributeNotSettableErr.
// Struct layouts follow the Carbon headers' 2-byte packing (MacTypes.h).
namespace alchemy::gui::fill {
namespace {

namespace uti = completion_kit::uttype;

constexpr std::int32_t no_err = 0, param_err = -50, io_err = -36, fnf_err = -43, f_bsy_err = -47, bad_fsref = -1401,
    not_a_folder = -1407, refs_different = -1420, path_too_long = -2110, access_denied = -5000, ls_not_settable = -10821,
    ls_attribute_not_found = -10813;
constexpr std::uint32_t fsref_magic = 0x52465341;   // "ASFR" little-endian
constexpr std::uint64_t mac_epoch_offset = 2082844800;   // seconds from 1904-01-01 to 1970-01-01 (UTC)
constexpr std::uint16_t mode_type = 0170000, mode_directory = 0040000, mode_link = 0120000;
constexpr std::uint32_t uf_hidden = 0x8000;

struct Stat {
    std::uint16_t mode{};
    std::uint32_t uid{}, gid{}, flags{};
    std::int64_t size{}, blocks{};
    std::int64_t access{}, modify{}, change{}, birth{};   // seconds
    std::uint32_t access_ns{}, modify_ns{}, change_ns{}, birth_ns{};
};

std::string leaf(const std::string& path) {
    const auto at = path.find_last_of('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}
std::string parent(const std::string& path) {
    const auto at = path.find_last_of('/');
    if (at == std::string::npos || at == 0) return "/";
    return path.substr(0, at);
}
std::string extension_of(const std::string& name) {
    const auto at = name.find_last_of('.');
    if (at == std::string::npos || at == 0 || at + 1 == name.size()) return {};
    return name.substr(at + 1);
}
std::string lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return text;
}

class FileManager final : public Provider {
public:
    using Provider::Provider;
    std::vector<std::string> paths;   // FSRef index -> resolved guest path

    std::int32_t guest_errno() { return static_cast<std::int32_t>(memory().read32(call(libsystem, "___error", {}))); }
    std::int32_t os_error(std::int32_t error) {
        switch (error) {
        case 2: return fnf_err;               // ENOENT
        case 20: return not_a_folder;         // ENOTDIR
        case 66: case 16: return f_bsy_err;   // ENOTEMPTY, EBUSY
        case 1: case 13: case 30: return access_denied;   // EPERM, EACCES, EROFS
        case 63: return path_too_long;        // ENAMETOOLONG
        default: return io_err;
        }
    }
    std::optional<Stat> stat(const std::string& path, bool follow, std::int32_t* error = nullptr) {
        // struct stat (64-bit inodes): st_mode u16 @4, st_uid @16, st_gid @20, st_atimespec @32, st_mtimespec @48,
        // st_ctimespec @64, st_birthtimespec @80, st_size @96, st_blocks @104, st_flags @116 (144 bytes).
        const auto buffer = malloc(144);
        const auto text = strdup(path);
        const auto result = static_cast<std::int32_t>(call(libsystem, follow ? "_stat" : "_lstat", {text, buffer}));
        std::optional<Stat> value;
        if (result == 0) {
            auto& m = memory();
            Stat info;
            info.mode = read16(m, buffer + 4);
            info.uid = m.read32(buffer + 16);
            info.gid = m.read32(buffer + 20);
            const auto time = [&](Address at, std::int64_t& seconds, std::uint32_t& nanoseconds) {
                seconds = static_cast<std::int64_t>(m.read64(at));
                nanoseconds = static_cast<std::uint32_t>(m.read64(at + 8));
            };
            time(buffer + 32, info.access, info.access_ns);
            time(buffer + 48, info.modify, info.modify_ns);
            time(buffer + 64, info.change, info.change_ns);
            time(buffer + 80, info.birth, info.birth_ns);
            info.size = static_cast<std::int64_t>(m.read64(buffer + 96));
            info.blocks = static_cast<std::int64_t>(m.read64(buffer + 104));
            info.flags = m.read32(buffer + 116);
            value = info;
        } else if (error) {
            *error = os_error(guest_errno());
        }
        free(text);
        free(buffer);
        return value;
    }
    std::optional<std::string> resolve(const std::string& path) {
        const auto text = strdup(path);
        const auto resolved = call(libsystem, "_realpath", {text, 0});
        free(text);
        if (!resolved) return std::nullopt;
        auto result = read_cstring(memory(), resolved);
        free(resolved);
        return result;
    }
    std::int32_t simple(std::string_view name, const std::string& path, std::initializer_list<Address> more = {}) {
        const auto text = strdup(path);
        std::vector<Address> arguments{text};
        arguments.insert(arguments.end(), more.begin(), more.end());
        std::int32_t result{};
        if (arguments.size() == 1) result = static_cast<std::int32_t>(call(libsystem, name, {arguments[0]}));
        else result = static_cast<std::int32_t>(call(libsystem, name, {arguments[0], arguments[1]}));
        const auto error = result == 0 ? 0 : guest_errno();
        free(text);
        return result == 0 ? no_err : os_error(error);
    }

    // ---- FSRef -------------------------------------------------------------------------------------------
    void make_ref(GuestMemory& m, Address ref, const std::string& path) {
        std::uint64_t index{};
        {
            std::lock_guard lock(mutex_);
            const auto found = std::find(paths.begin(), paths.end(), path);
            index = static_cast<std::uint64_t>(found - paths.begin());
            if (found == paths.end()) paths.push_back(path);
        }
        m.fill_bytes(ref, 0, 80);
        m.write32(ref, fsref_magic);
        m.write64(ref + 8, index);
    }
    std::optional<std::string> ref_path(GuestMemory& m, Address ref) {
        if (!ref || m.read32(ref) != fsref_magic) return std::nullopt;
        const auto index = m.read64(ref + 8);
        std::lock_guard lock(mutex_);
        if (index >= paths.size()) return std::nullopt;
        return paths[static_cast<std::size_t>(index)];
    }
    // The canonical path of an existing object (the leaf itself when not following a leaf symbolic link).
    std::pair<std::int32_t, std::string> canonical(const std::string& path, bool follow_leaf) {
        std::int32_t error = fnf_err;
        if (path.empty() || path[0] != '/') return {param_err, {}};
        if (!stat(path, follow_leaf, &error)) return {error, {}};
        if (follow_leaf) {
            auto resolved = resolve(path);
            return resolved ? std::pair{no_err, *resolved} : std::pair{fnf_err, std::string{}};
        }
        auto trimmed = path;
        while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
        const auto directory = resolve(parent(trimmed));
        if (!directory) return {fnf_err, {}};
        return {no_err, (*directory == "/" ? std::string() : *directory) + "/" + leaf(trimmed)};
    }

    // ---- folders --------------------------------------------------------------------------------------
    std::string environment(std::string_view name, std::string fallback) {
        const auto text = strdup(name);
        const auto value = call(libsystem, "_getenv", {text});
        free(text);
        if (!value) return fallback;
        auto result = read_cstring(memory(), value);
        return result.empty() ? fallback : result;
    }
    // The folder of `type` in a domain (vRefNum), or "" when the domain has none.
    std::string folder(std::int16_t domain, std::uint32_t type) {
        enum class Domain { user, local, system } where = Domain::user;
        if (domain == -32765) where = Domain::local;                          // kLocalDomain
        else if (domain == -32766 || domain == -32768) where = Domain::system;   // kSystemDomain, kOnSystemDisk
        else if (domain == -32764 || domain == -32762) return {};             // kNetworkDomain, kClassicDomain
        const auto home = environment("HOME", "/");
        const auto library = where == Domain::user ? home + "/Library" : where == Domain::local ? std::string("/Library") : std::string("/System/Library");
        auto temporary = environment("TMPDIR", "/tmp");
        while (temporary.size() > 1 && temporary.back() == '/') temporary.pop_back();
        switch (type) {
        case fourcc("dlib"): return library;                                        // kDomainLibraryFolderType
        case fourcc("pref"): return library + "/Preferences";
        case fourcc("asup"): return library + "/Application Support";
        case fourcc("cach"): return library + "/Caches";
        case fourcc("logs"): return library + "/Logs";
        case fourcc("font"): return library + "/Fonts";
        case fourcc("fram"): return library + "/Frameworks";
        case fourcc("temp"): case fourcc("chew"): return temporary;                 // kTemporaryFolderType, kChewableItemsFolderType
        case fourcc("apps"): return where == Domain::user ? home + "/Applications" : where == Domain::local ? "/Applications" : "/System/Applications";
        default: break;
        }
        if (where != Domain::user) return {};
        switch (type) {
        case fourcc("cusr"): return home;                       // kCurrentUserFolderType
        case fourcc("desk"): return home + "/Desktop";
        case fourcc("docs"): return home + "/Documents";
        case fourcc("down"): return home + "/Downloads";
        case fourcc("pdoc"): return home + "/Pictures";
        case fourcc("mdoc"): return home + "/Movies";
        case fourcc("\xb5" "doc"): return home + "/Music";
        case fourcc("pubb"): return home + "/Public";
        case fourcc("trsh"): return home + "/.Trash";
        default: return {};
        }
    }

    // ---- item attributes ------------------------------------------------------------------------------
    struct Item {
        std::string path, name, extension, type;
        Stat info;
        bool directory{}, link{}, package{}, application{}, invisible{};
    };
    std::optional<Item> item(const std::string& path) {
        const auto info = stat(path, false);
        if (!info) return std::nullopt;
        Item value;
        value.path = path;
        value.name = leaf(path);
        value.extension = extension_of(value.name);
        value.info = *info;
        value.link = (info->mode & mode_type) == mode_link;
        if (value.link) {
            if (const auto target = stat(path, true)) value.directory = (target->mode & mode_type) == mode_directory;
        } else {
            value.directory = (info->mode & mode_type) == mode_directory;
        }
        const auto ext = lower(value.extension);
        value.application = value.directory && ext == "app";
        value.package = value.directory && (value.application || ext == "bundle" || ext == "framework" || ext == "plugin" || ext == "kext");
        value.invisible = (!value.name.empty() && value.name[0] == '.') || (info->flags & uf_hidden);
        if (value.link) value.type = "public.symlink";
        else if (value.application) value.type = "com.apple.application-bundle";
        else if (ext == "framework") value.type = "com.apple.framework";
        else if (value.package) value.type = "com.apple.bundle";
        else if (value.directory) value.type = "public.folder";
        else if (const auto found = value.extension.empty() ? std::nullopt : uti::for_tag(uti::Tag::filename_extension, ext)) value.type = *found;
        else value.type = "public.data";
        return value;
    }
    std::string kind(const Item& value) {
        if (value.application) return "Application";
        if (value.directory && !value.package) return "Folder";
        const auto text = uti::description(value.type);
        return text.empty() ? "Document" : text;
    }
    std::string display_name(const Item& value) {
        auto name = value.name;
        if (value.application && name.size() > 4) name.resize(name.size() - 4);
        return name;
    }

    void install();

private:
    std::mutex mutex_;
};

void FileManager::install() {
    // OSStatus FSPathMakeRef(const UInt8 *path, FSRef *ref, Boolean *isDirectory)
    // OSStatus FSPathMakeRefWithOptions(const UInt8 *path, OptionBits options, FSRef *ref, Boolean *isDirectory)
    const auto make = [this](CpuState& s, GuestMemory& m, Address path, bool follow, Address ref, Address is_directory) {
        if (!path || !ref) { s.x[0] = status(param_err); return; }
        const auto [code, resolved] = canonical(read_cstring(m, path), follow);
        if (code) { s.x[0] = status(code); return; }
        const auto info = stat(resolved, follow);
        if (!info) { s.x[0] = status(fnf_err); return; }
        make_ref(m, ref, resolved);
        if (is_directory) m.write8(is_directory, (info->mode & mode_type) == mode_directory ? 1 : 0);
        s.x[0] = 0;
    };
    raw(core_services, "_FSPathMakeRef", [make](CpuState& s, GuestMemory& m) { make(s, m, s.x[0], true, s.x[1], s.x[2]); });
    raw(core_services, "_FSPathMakeRefWithOptions", [make](CpuState& s, GuestMemory& m) {
        make(s, m, s.x[0], (s.x[1] & 1) == 0, s.x[2], s.x[3]);   // kFSPathMakeRefDoNotFollowLeafSymlink
    });
    // OSStatus FSRefMakePath(const FSRef *, UInt8 *path, UInt32 maxPathSize)
    raw(core_services, "_FSRefMakePath", [this](CpuState& s, GuestMemory& m) {
        const auto path = ref_path(m, s.x[0]);
        if (!path) { s.x[0] = status(bad_fsref); return; }
        if (!s.x[1]) { s.x[0] = status(param_err); return; }
        if (path->size() + 1 > static_cast<std::uint32_t>(s.x[2])) { s.x[0] = status(path_too_long); return; }
        m.write_bytes(s.x[1], std::span(reinterpret_cast<const std::uint8_t*>(path->data()), path->size()));
        m.write8(s.x[1] + path->size(), 0);
        s.x[0] = 0;
    });
    // OSErr FSCompareFSRefs(const FSRef *, const FSRef *)
    raw(core_services, "_FSCompareFSRefs", [this](CpuState& s, GuestMemory& m) {
        const auto first = ref_path(m, s.x[0]), second = ref_path(m, s.x[1]);
        if (!first || !second) { s.x[0] = status(bad_fsref); return; }
        s.x[0] = *first == *second ? 0 : status(refs_different);
    });
    // OSErr FSDeleteObject(const FSRef *): a file, or an empty folder.
    raw(core_services, "_FSDeleteObject", [this](CpuState& s, GuestMemory& m) {
        const auto path = ref_path(m, s.x[0]);
        if (!path) { s.x[0] = status(bad_fsref); return; }
        std::int32_t error = fnf_err;
        const auto info = stat(*path, false, &error);
        if (!info) { s.x[0] = status(error); return; }
        s.x[0] = status(simple((info->mode & mode_type) == mode_directory ? "_rmdir" : "_unlink", *path));
    });
    // OSErr FSFindFolder(FSVolumeRefNum vRefNum, OSType folderType, Boolean createFolder, FSRef *foundRef)
    raw(core_services, "_FSFindFolder", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = status(param_err); return; }
        const auto path = folder(static_cast<std::int16_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]));
        if (path.empty()) { s.x[0] = status(fnf_err); return; }
        if (!stat(path, true)) {
            if (!(s.x[2] & 0xff)) { s.x[0] = status(fnf_err); return; }
            // kCreateFolder: create it and any missing parents (owner only for the user's folders).
            std::string partial;
            for (std::size_t at = 1; at <= path.size(); ++at) {
                if (at < path.size() && path[at] != '/') continue;
                partial = path.substr(0, at);
                if (!stat(partial, true))
                    if (const auto code = simple("_mkdir", partial, {0700})) { s.x[0] = status(code); return; }
            }
        }
        const auto [code, resolved] = canonical(path, true);
        if (code) { s.x[0] = status(code); return; }
        make_ref(m, s.x[3], resolved);
        s.x[0] = 0;
    });
    // OSErr FSGetCatalogInfo(const FSRef *, FSCatalogInfoBitmap whichInfo, FSCatalogInfo *catalogInfo,
    //     HFSUniStr255 *outName, FSSpecPtr fsSpec, FSRef *parentRef)
    raw(core_services, "_FSGetCatalogInfo", [this](CpuState& s, GuestMemory& m) {
        const auto path = ref_path(m, s.x[0]);
        if (!path) { s.x[0] = status(bad_fsref); return; }
        std::int32_t error = fnf_err;
        const auto info = stat(*path, false, &error);
        if (!info) { s.x[0] = status(error); return; }
        if (s.x[4]) { s.x[0] = status(param_err); return; }   // FSSpecs are not provided (64-bit Carbon has none)
        const auto which = static_cast<std::uint32_t>(s.x[1]);
        const bool directory = (info->mode & mode_type) == mode_directory;
        if (const auto out = s.x[2]) {
            // FSCatalogInfo (2-byte packing): nodeFlags @0, volume @2, parentDirID @4, nodeID @8, sharingFlags @12,
            // userPrivileges @13, createDate @16, contentModDate @24, attributeModDate @32, accessDate @40,
            // backupDate @48, permissions @56 (userID, groupID, reserved1, userAccess, mode, fileSec),
            // finderInfo @76, extFinderInfo @92, dataLogicalSize @108, dataPhysicalSize @116,
            // rsrcLogicalSize @124, rsrcPhysicalSize @132, valence @140, textEncodingHint @144 (148 bytes).
            if (which & 0x0002) m.write_bytes(out, std::vector<std::uint8_t>{static_cast<std::uint8_t>(directory ? 0x10 : 0), 0});   // kFSNodeIsDirectoryMask
            const auto date = [&](std::uint32_t bit, Address at, std::int64_t seconds, std::uint32_t nanoseconds) {
                if (!(which & bit)) return;
                // UTCDateTime {UInt16 highSeconds; UInt32 lowSeconds; UInt16 fraction} since 1904.
                const auto mac = static_cast<std::uint64_t>(seconds + static_cast<std::int64_t>(mac_epoch_offset));
                const auto fraction = static_cast<std::uint16_t>((static_cast<std::uint64_t>(nanoseconds) << 16) / 1000000000ULL);
                std::vector<std::uint8_t> bytes(8);
                bytes[0] = static_cast<std::uint8_t>(mac >> 32); bytes[1] = static_cast<std::uint8_t>(mac >> 40);
                for (int index = 0; index < 4; ++index) bytes[2 + static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(mac >> (8 * index));
                bytes[6] = static_cast<std::uint8_t>(fraction); bytes[7] = static_cast<std::uint8_t>(fraction >> 8);
                m.write_bytes(at, bytes);
            };
            date(0x0020, out + 16, info->birth, info->birth_ns);      // kFSCatInfoCreateDate
            date(0x0040, out + 24, info->modify, info->modify_ns);    // kFSCatInfoContentMod
            date(0x0080, out + 32, info->change, info->change_ns);    // kFSCatInfoAttrMod
            date(0x0100, out + 40, info->access, info->access_ns);    // kFSCatInfoAccessDate
            if (which & 0x0200) m.fill_bytes(out + 48, 0, 8);          // kFSCatInfoBackupDate: never backed up
            if (which & 0x0400) {                                       // kFSCatInfoPermissions
                m.write32(out + 56, info->uid);
                m.write32(out + 60, info->gid);
                m.write_bytes(out + 64, std::vector<std::uint8_t>{0, 0, static_cast<std::uint8_t>(info->mode), static_cast<std::uint8_t>(info->mode >> 8)});
                m.write64(out + 68, 0);
            }
            if (which & 0x0004) m.fill_bytes(out + 2, 0, 2);                    // kFSCatInfoVolume
            if (which & 0x0018) m.fill_bytes(out + 4, 0, 8);                    // kFSCatInfoParentDirID, kFSCatInfoNodeID
            if (which & 0x0800) m.fill_bytes(out + 76, 0, 16);                  // kFSCatInfoFinderInfo
            if (which & 0x1000) m.fill_bytes(out + 92, 0, 16);                  // kFSCatInfoFinderXInfo
            if (which & 0x4000) {                                               // kFSCatInfoDataSizes
                m.write64(out + 108, directory ? 0 : static_cast<std::uint64_t>(info->size));
                m.write64(out + 116, directory ? 0 : static_cast<std::uint64_t>(info->blocks) * 512);
            }
            if (which & 0x8000) m.fill_bytes(out + 124, 0, 16);                 // kFSCatInfoRsrcSizes: no resource forks
            if (which & 0x00010000) m.write32(out + 140, 0);                    // kFSCatInfoValence: not counted
        }
        if (const auto name = s.x[3]) {
            // HFSUniStr255 {UInt16 length; UniChar unicode[255]}
            const auto text = leaf(*path).empty() ? std::string("/") : leaf(*path);
            std::vector<std::uint16_t> units;
            for (std::size_t at = 0; at < text.size() && units.size() < 255;) {
                const auto lead = static_cast<unsigned char>(text[at]);
                std::uint32_t code = lead;
                std::size_t length = 1;
                if (lead >= 0xf0) { code = lead & 0x07; length = 4; }
                else if (lead >= 0xe0) { code = lead & 0x0f; length = 3; }
                else if (lead >= 0xc0) { code = lead & 0x1f; length = 2; }
                for (std::size_t index = 1; index < length && at + index < text.size(); ++index) code = (code << 6) | (static_cast<unsigned char>(text[at + index]) & 0x3f);
                at += length;
                if (code >= 0x10000) { code -= 0x10000; units.push_back(static_cast<std::uint16_t>(0xd800 + (code >> 10))); units.push_back(static_cast<std::uint16_t>(0xdc00 + (code & 0x3ff))); }
                else units.push_back(static_cast<std::uint16_t>(code));
            }
            if (units.size() > 255) units.resize(255);
            std::vector<std::uint8_t> bytes(2 + units.size() * 2);
            bytes[0] = static_cast<std::uint8_t>(units.size()); bytes[1] = static_cast<std::uint8_t>(units.size() >> 8);
            for (std::size_t index = 0; index < units.size(); ++index) { bytes[2 + 2 * index] = static_cast<std::uint8_t>(units[index]); bytes[3 + 2 * index] = static_cast<std::uint8_t>(units[index] >> 8); }
            m.write_bytes(name, bytes);
        }
        if (s.x[5]) make_ref(m, s.x[5], parent(*path));
        s.x[0] = 0;
    });

    // ---- LaunchServices item queries -------------------------------------------------------------------
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"_kLSItemContentType", "LSItemContentType"}, {"_kLSItemFileType", "LSItemFileType"},
             {"_kLSItemFileCreator", "LSItemFileCreator"}, {"_kLSItemExtension", "LSItemExtension"},
             {"_kLSItemDisplayName", "LSItemDisplayName"}, {"_kLSItemDisplayKind", "LSItemDisplayKind"},
             {"_kLSItemRoleHandlerDisplayName", "LSItemRoleHandlerDisplayName"}, {"_kLSItemIsInvisible", "LSItemIsInvisible"},
             {"_kLSItemExtensionIsHidden", "LSItemExtensionIsHidden"}, {"_kLSItemQuarantineProperties", "LSItemQuarantineProperties"}})
        string(core_services, name, value);
    // OSStatus LSCopyItemAttribute(const FSRef *, LSRolesMask, CFStringRef attribute, CFTypeRef *value)
    raw(core_services, "_LSCopyItemAttribute", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) { s.x[0] = status(param_err); return; }
        m.write64(s.x[3], 0);
        const auto path = ref_path(m, s.x[0]);
        if (!path) { s.x[0] = status(bad_fsref); return; }
        const auto value = item(*path);
        if (!value) { s.x[0] = status(fnf_err); return; }
        const auto name = s.x[2] ? cf.utf8(s.x[2]) : std::string();
        Address result{};
        if (name == "LSItemContentType") result = cf.string(value->type);
        else if (name == "LSItemFileType" || name == "LSItemFileCreator") result = cf.string("????");
        else if (name == "LSItemExtension") result = value->extension.empty() ? 0 : cf.string(value->extension);
        else if (name == "LSItemDisplayName") result = cf.string(display_name(*value));
        else if (name == "LSItemDisplayKind") result = cf.string(kind(*value));
        else if (name == "LSItemIsInvisible") result = retain(cf.boolean(value->invisible));
        else if (name == "LSItemExtensionIsHidden") result = retain(cf.boolean(value->application));
        else { s.x[0] = status(ls_attribute_not_found); return; }   // role handlers (none registered), quarantine
        if (!result) { s.x[0] = status(ls_attribute_not_found); return; }
        m.write64(s.x[3], result);
        s.x[0] = 0;
    });
    // OSStatus LSSetItemAttribute(const FSRef *, LSRolesMask, CFStringRef attribute, CFTypeRef value)
    raw(core_services, "_LSSetItemAttribute", [this](CpuState& s, GuestMemory& m) {
        s.x[0] = status(ref_path(m, s.x[0]) ? ls_not_settable : bad_fsref);
    });
    // OSStatus LSCopyKindStringForURL(CFURLRef, CFStringRef *outKindString)
    raw(core_services, "_LSCopyKindStringForURL", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = status(param_err); return; }
        m.write64(s.x[1], 0);
        auto path = cf.url_path(s.x[0]);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const auto value = item(path);
        if (!value) { s.x[0] = status(fnf_err); return; }
        m.write64(s.x[1], cf.string(kind(*value)));
        s.x[0] = 0;
    });
    // OSStatus LSCopyItemInfoForURL(CFURLRef, LSRequestedInfo whichInfo, LSItemInfoRecord *outItemInfo)
    // LSItemInfoRecord (2-byte packing): flags @0, filetype @4, creator @8, extension @12 (CFStringRef, +1 when asked).
    raw(core_services, "_LSCopyItemInfoForURL", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[2]) { s.x[0] = status(param_err); return; }
        auto path = cf.url_path(s.x[0]);
        while (path.size() > 1 && path.back() == '/') path.pop_back();
        const auto value = item(path);
        if (!value) { s.x[0] = status(fnf_err); return; }
        const auto which = static_cast<std::uint32_t>(s.x[1]);
        m.fill_bytes(s.x[2], 0, 20);
        if (which & 0x00000002) m.write64(s.x[2] + 12, value->extension.empty() ? 0 : cf.string(value->extension));   // kLSRequestExtension
        if (which & 0x00000004) { m.write32(s.x[2] + 4, fourcc("????")); m.write32(s.x[2] + 8, fourcc("????")); }   // kLSRequestTypeCreator
        std::uint32_t flags = 0;
        if (which & 0x00000038) {   // kLSRequestBasicFlagsOnly | kLSRequestAppTypeFlags | kLSRequestAllFlags
            if (!value->directory) flags |= 0x00000001;             // kLSItemInfoIsPlainFile
            if (value->package) flags |= 0x00000002;                // kLSItemInfoIsPackage
            if (value->application) flags |= 0x00000004 | 0x00000080;   // kLSItemInfoIsApplication, kLSItemInfoIsNativeApp
            if (value->directory) flags |= 0x00000008;              // kLSItemInfoIsContainer
            if (value->link) flags |= 0x00000020;                   // kLSItemInfoIsSymlink
            if (value->invisible) flags |= 0x00000040;              // kLSItemInfoIsInvisible
            if (value->application) flags |= 0x00100000;            // kLSItemInfoExtensionIsHidden
        }
        m.write32(s.x[2], flags);
        s.x[0] = 0;
    });
}

} // namespace

void install_coreservices_file_manager(InstallContext& context) { std::make_shared<FileManager>(context)->install(); }

} // namespace alchemy::gui::fill
