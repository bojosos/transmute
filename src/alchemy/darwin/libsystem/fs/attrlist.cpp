#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/libsystem/install.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/path.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The getattrlist(2) family (fill-only): getattrlist, fgetattrlist, getattrlistat, setattrlist, fsetattrlist,
// setattrlistat, getattrlistbulk, from what stat, statfs, the extended attributes and the host volume report.
//
// getattrlist packs, after its u_int32 length (and the attribute_set_t of ATTR_CMN_RETURNED_ATTRS), the requested
// attributes in bit order: common, volume, directory (for a directory), file (for anything else), then the
// extended common ones (FSOPT_ATTR_CMN_EXTENDED, in forkattr). Variable-length values (names, paths) are
// attrreference_t {offset from the reference, length with the NUL} into a 4-byte-aligned area after the fixed
// ones. An attribute this file system cannot answer (ATTR_CMN_GEN_COUNT, DOCUMENT_ID, the security and UUID
// attributes, ADDEDTIME, the volume object counts and UUID...) is left out, or packed as zeros with
// FSOPT_PACK_INVAL_ATTRS; ATTR_CMN_RETURNED_ATTRS tells which were returned. Volume attributes need ATTR_VOL_INFO,
// the root of the volume and no directory or file attributes (EINVAL otherwise).
//
// setattrlist sets, in the same order and packing (without the length): ATTR_CMN_CRTIME (the host creation time),
// MODTIME and ACCTIME (utimensat / futimens), FNDRINFO (the com.apple.FinderInfo extended attribute), OWNERID /
// GRPID (chown), ACCESSMASK (chmod) and FLAGS (chflags: UF_HIDDEN). Any other attribute cannot be set: EINVAL.
//
// getattrlistbulk packs one such record per directory entry (without "." and ".."; symbolic links not followed),
// from the directory descriptor's position on, as many as fit; each record's length covers its padding to 8 bytes.
// ATTR_CMN_RETURNED_ATTRS is required and volume attributes are refused (EINVAL); ATTR_CMN_ERROR is 0 for every
// entry returned (an entry that vanished since the listing is skipped). The position moves past the entries
// returned (lseek on the directory reads and sets it); 0 entries means the end. A first entry larger than the
// buffer: ERANGE.
namespace alchemy::export_fill {
namespace {

constexpr std::uint32_t enoattr = 93, enotdir = 20;
constexpr std::uint32_t fsopt_nofollow = 0x1, fsopt_report_fullsize = 0x4, fsopt_pack_inval_attrs = 0x8, fsopt_attr_cmn_extended = 0x20;
constexpr std::uint32_t attr_cmn_returned_attrs = 0x80000000U, attr_vol_info = 0x80000000U;
constexpr std::uint32_t vol_valid = 0xF007FFFFU, dir_valid = 0x3FU, file_valid = 0x7FFFU, cmnext_valid = 0x1FFCU;
constexpr std::uint32_t fork_valid = 0x3U;   // ATTR_FORK_TOTALSIZE / ALLOCSIZE (deprecated)
constexpr std::int64_t at_fdcwd = -2;
constexpr std::uint64_t stat_size = 144, statfs_size = 2168;

std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t value{};
    std::memcpy(&value, b.data() + at, 4);
    return value;
}
std::uint64_t le64(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint64_t value{};
    std::memcpy(&value, b.data() + at, 8);
    return value;
}

// The packed attribute buffer: fixed values, then the variable data the attrreference_t values point at.
class Packer {
public:
    void u32(std::uint32_t value) { put(&value, 4); }
    void u64(std::uint64_t value) { put(&value, 8); }
    void bytes(std::span<const std::uint8_t> value) { fixed_.insert(fixed_.end(), value.begin(), value.end()); }
    void zeros(std::size_t count) { fixed_.insert(fixed_.end(), count, 0); }
    void timespec(std::uint64_t seconds, std::uint64_t nanoseconds) { u64(seconds); u64(nanoseconds); }
    void reference(const std::string& text) {
        references_.push_back({fixed_.size(), text});
        zeros(8);
    }
    // The whole buffer, without the leading length and attribute set (`prefix` bytes come before it).
    std::vector<std::uint8_t> finish(std::size_t prefix) {
        std::vector<std::uint8_t> out = fixed_;
        for (const auto& [at, text] : references_) {
            while (out.size() % 4) out.push_back(0);
            const auto offset = static_cast<std::int32_t>(out.size() - at);
            const auto length = static_cast<std::uint32_t>(text.size() + 1);
            std::memcpy(out.data() + at, &offset, 4);
            std::memcpy(out.data() + at + 4, &length, 4);
            out.insert(out.end(), text.begin(), text.end());
            out.push_back(0);
        }
        while ((out.size() + prefix) % 4) out.push_back(0);
        return out;
    }
private:
    void put(const void* value, std::size_t size) {
        const auto* bytes = static_cast<const std::uint8_t*>(value);
        fixed_.insert(fixed_.end(), bytes, bytes + size);
    }
    std::vector<std::uint8_t> fixed_;
    std::vector<std::pair<std::size_t, std::string>> references_;
};

struct Target { std::string path; std::optional<std::uint64_t> fd; bool follow{true}; };

class AttributeLists {
public:
    AttributeLists(const Provider& p, GuestMemory& m) : p_(p), m_(m) {}

    // The target of fgetattrlist / getattrlistat as a path (F_GETPATH for a descriptor).
    std::string descriptor_path(std::uint64_t fd) {
        const auto buffer = p_.malloc(1024);
        CallRegs registers;
        registers.x[0] = fd; registers.x[1] = 50;   // F_GETPATH
        registers.push_variadic(buffer);
        const auto result = static_cast<std::int32_t>(p_.call_regs(libsystem_library, "_fcntl", registers));
        const auto text = result == 0 ? read(buffer) : std::string();
        free(buffer);
        if (result != 0) throw Errno{p_.guest_errno()};
        return text;
    }
    std::string at_path(std::uint64_t dirfd, std::uint64_t path_address) {
        auto path = read(path_address);
        if (path.empty()) throw Errno{enoent};
        if (path[0] == '/' || static_cast<std::int32_t>(dirfd) == at_fdcwd) return path;
        return descriptor_path(dirfd) + "/" + path;
    }

    std::uint64_t get(const Target& target, std::uint64_t list, std::uint64_t buffer, std::uint64_t size, std::uint32_t options) {
        auto out = pack(target, list, options, false);
        if (size < 4) throw Errno{einval};
        const auto total = out.size();
        const auto written = static_cast<std::size_t>(std::min<std::uint64_t>(size, total));
        if (written < total && !(options & fsopt_report_fullsize)) {
            const auto shown = static_cast<std::uint32_t>(written);
            std::memcpy(out.data(), &shown, 4);
        }
        m_.write_bytes(buffer, std::span<const std::uint8_t>(out.data(), written));
        return 0;
    }

    // getattrlistbulk(dirfd, attrList, attrBuf, size, options): the number of entries packed, 0 at the end.
    std::uint64_t bulk(std::uint64_t fd, std::uint64_t list, std::uint64_t buffer, std::uint64_t size, std::uint64_t options) {
        const auto [common, volume, directory, file, fork] = read_list(list);
        static_cast<void>(directory); static_cast<void>(file); static_cast<void>(fork);
        if (!(common & attr_cmn_returned_attrs) || volume) throw Errno{einval};
        if (options & ~std::uint64_t{fsopt_pack_inval_attrs | fsopt_attr_cmn_extended}) throw Errno{einval};
        const auto directory_status = stat({"", fd, true});
        if ((le32(directory_status, 4) & 0170000) != 0040000) throw Errno{enotdir};
        auto path = descriptor_path(fd);
        if (!path.ends_with('/')) path += '/';
        const auto position = [&](std::optional<std::uint64_t> moved) {
            const auto result = p_.call(libsystem_library, "_lseek", {fd, moved.value_or(0), moved ? 0U : 1U});   // SEEK_SET / SEEK_CUR
            if (result == UINT64_MAX) throw Errno{p_.guest_errno()};
            return result;
        };
        const auto names = entry_names(path);
        auto at = position(std::nullopt);
        std::vector<std::uint8_t> out;
        std::uint64_t count{};
        for (; at < names.size(); ++at) {
            std::vector<std::uint8_t> entry;
            try { entry = pack({path + names[static_cast<std::size_t>(at)], std::nullopt, false}, list, static_cast<std::uint32_t>(options), true); }
            catch (const Errno&) { continue; }   // gone since the listing
            while (entry.size() % 8) entry.push_back(0);
            const auto length = static_cast<std::uint32_t>(entry.size());
            std::memcpy(entry.data(), &length, 4);
            if (out.size() + entry.size() > size) {
                if (!count) throw Errno{erange};
                break;
            }
            out.insert(out.end(), entry.begin(), entry.end());
            ++count;
        }
        static_cast<void>(position(at));
        if (!out.empty()) m_.write_bytes(buffer, out);
        return count;
    }

    // The attribute record of getattrlist (bulk: getattrlistbulk's, which answers ATTR_CMN_ERROR).
    std::vector<std::uint8_t> pack(const Target& target, std::uint64_t list, std::uint32_t options, bool bulk) {
        const auto [common, volume, directory, file, fork] = read_list(list);
        const bool extended = (options & fsopt_attr_cmn_extended) != 0;
        if (volume & ~vol_valid || directory & ~dir_valid || file & ~file_valid || fork & ~(extended ? cmnext_valid : fork_valid))
            throw Errno{einval};
        if (!extended && fork) throw Errno{einval};   // the fork attributes: not supported
        if (volume && (!(volume & attr_vol_info) || directory || file)) throw Errno{einval};
        const bool pack_invalid = (options & fsopt_pack_inval_attrs) != 0;
        const auto status = stat(target);
        const auto mode = le32(status, 4) & 0xffff;
        const bool is_directory = (mode & 0170000) == 0040000;
        // The absolute path (realpath, which walks the whole path): only for the attributes that need it.
        std::optional<std::string> full_value;
        const auto full = [&]() -> const std::string& {
            if (!full_value) full_value = full_path(target);
            return *full_value;
        };
        // The path as given (or the descriptor's), for what any name of the object answers.
        const auto given = target.fd ? std::string() : target.path;
        std::optional<std::vector<std::uint8_t>> volume_stats;
        const auto statfs_info = [&]() -> const std::vector<std::uint8_t>& {
            if (!volume_stats) volume_stats = statfs(target);
            return *volume_stats;
        };
        // The volume's root: the object is the mount point itself (same device and inode).
        std::optional<bool> root;
        const auto is_root = [&]() {
            if (!root) {
                const auto mount = stat({text(statfs_info(), 88, 1024), std::nullopt, true});
                root = le64(mount, 8) == le64(status, 8) && le32(mount, 0) == le32(status, 0);
            }
            return *root;
        };
        if (volume && !is_root()) throw Errno{einval};

        Packer packer;
        std::array<std::uint32_t, 5> returned{};
        // One attribute: pack it when it can be answered, else zeros of `size` with FSOPT_PACK_INVAL_ATTRS.
        const auto attribute = [&](std::size_t group, std::uint32_t bit, std::size_t invalid_size, auto&& pack) {
            if (pack(packer)) returned[group] |= bit;
            else if (pack_invalid) packer.zeros(invalid_size);
        };
        const auto ts = [&](std::size_t offset) { return [&, offset](Packer& out) { out.timespec(le64(status, offset), le64(status, offset + 8)); return true; }; };
        const auto never = [](Packer&) { return false; };
        const auto parent_inode = [&]() -> std::uint64_t {
            if (is_root()) return le64(status, 8);
            const auto& path = full();
            const auto slash = path.find_last_of('/');
            Target parent{slash == 0 ? std::string("/") : path.substr(0, slash), std::nullopt, true};
            return le64(stat(parent), 8);
        };
        const auto name = [&]() -> std::string {
            if (is_root()) return volume_name(target, statfs_info());
            // The given path's last component, unless it is ".", ".." or ends with "/".
            const auto slash = given.find_last_of('/');
            const auto last = slash == std::string::npos ? given : given.substr(slash + 1);
            if (!last.empty() && last != "." && last != "..") return last;
            return full().substr(full().find_last_of('/') + 1);
        };

        for (std::uint32_t bit = 1; bit && bit < attr_cmn_returned_attrs; bit <<= 1) {
            if (!(common & bit)) continue;
            switch (bit) {
            case 0x1: attribute(0, bit, 8, [&](Packer& out) { out.reference(name()); return true; }); break;   // NAME
            case 0x2: attribute(0, bit, 4, [&](Packer& out) { out.u32(le32(status, 0)); return true; }); break;   // DEVID
            case 0x4: attribute(0, bit, 8, [&](Packer& out) { out.u64(le64(statfs_info(), 48)); return true; }); break;   // FSID
            case 0x8: attribute(0, bit, 4, [&](Packer& out) { out.u32(object_type(mode)); return true; }); break;   // OBJTYPE
            case 0x10: attribute(0, bit, 4, [](Packer& out) { out.u32(24); return true; }); break;   // OBJTAG: VT_OTHER
            case 0x20: case 0x40:   // OBJID, OBJPERMANENTID: {fid_objno, fid_generation}
                attribute(0, bit, 8, [&](Packer& out) { out.u32(static_cast<std::uint32_t>(le64(status, 8))); out.u32(0); return true; }); break;
            case 0x80:   // PAROBJID
                attribute(0, bit, 8, [&](Packer& out) { out.u32(static_cast<std::uint32_t>(parent_inode())); out.u32(0); return true; }); break;
            case 0x100: attribute(0, bit, 4, [](Packer& out) { out.u32(0); return true; }); break;   // SCRIPT
            case 0x200: attribute(0, bit, 16, ts(80)); break;   // CRTIME
            case 0x400: attribute(0, bit, 16, ts(48)); break;   // MODTIME
            case 0x800: attribute(0, bit, 16, ts(64)); break;   // CHGTIME
            case 0x1000: attribute(0, bit, 16, ts(32)); break;   // ACCTIME
            case 0x2000: attribute(0, bit, 16, [](Packer& out) { out.timespec(0, 0); return true; }); break;   // BKUPTIME: never backed up
            case 0x4000: attribute(0, bit, 32, [&](Packer& out) { out.bytes(finder_info(target)); return true; }); break;   // FNDRINFO
            case 0x8000: attribute(0, bit, 4, [&](Packer& out) { out.u32(le32(status, 16)); return true; }); break;   // OWNERID
            case 0x10000: attribute(0, bit, 4, [&](Packer& out) { out.u32(le32(status, 20)); return true; }); break;   // GRPID
            case 0x20000: attribute(0, bit, 4, [&](Packer& out) { out.u32(mode); return true; }); break;   // ACCESSMASK
            case 0x40000: attribute(0, bit, 4, [&](Packer& out) { out.u32(le32(status, 116)); return true; }); break;   // FLAGS
            case 0x200000: attribute(0, bit, 4, [&](Packer& out) { out.u32(user_access(given.empty() ? full() : given)); return true; }); break;   // USERACCESS
            case 0x2000000: attribute(0, bit, 8, [&](Packer& out) { out.u64(le64(status, 8)); return true; }); break;   // FILEID
            case 0x4000000: attribute(0, bit, 8, [&](Packer& out) { out.u64(parent_inode()); return true; }); break;   // PARENTID
            case 0x8000000: attribute(0, bit, 8, [&](Packer& out) { out.reference(full()); return true; }); break;   // FULLPATH
            // GEN_COUNT, DOCUMENT_ID, ERROR, DATA_PROTECT_FLAGS (u_int32); EXTENDED_SECURITY (a reference);
            // UUID, GRPUUID (guid_t); ADDEDTIME (timespec): not kept by this file system.
            case 0x400000: attribute(0, bit, 8, never); break;
            case 0x800000: case 0x1000000: attribute(0, bit, 16, never); break;
            case 0x10000000: attribute(0, bit, 16, never); break;
            case 0x20000000: attribute(0, bit, 4, [bulk](Packer& out) { if (!bulk) return false; out.u32(0); return true; }); break;   // ERROR
            default: attribute(0, bit, 4, never); break;
            }
        }
        if (volume) {
            const auto& fs = statfs_info();
            const auto block = le32(fs, 0);
            for (std::uint32_t bit = 1; bit && bit < attr_vol_info; bit <<= 1) {
                if (!(volume & bit)) continue;
                switch (bit) {
                case 0x1: attribute(1, bit, 4, [&](Packer& out) { out.u32(le32(fs, 60)); return true; }); break;   // FSTYPE
                case 0x4: attribute(1, bit, 8, [&](Packer& out) { out.u64(le64(fs, 8) * block); return true; }); break;   // SIZE
                case 0x8: attribute(1, bit, 8, [&](Packer& out) { out.u64(le64(fs, 16) * block); return true; }); break;   // SPACEFREE
                case 0x10: attribute(1, bit, 8, [&](Packer& out) { out.u64(le64(fs, 24) * block); return true; }); break;   // SPACEAVAIL
                case 0x20: case 0x40: attribute(1, bit, 8, [&](Packer& out) { out.u64(block); return true; }); break;   // MINALLOCATION, ALLOCATIONCLUMP
                case 0x80: attribute(1, bit, 4, [&](Packer& out) { out.u32(le32(fs, 4)); return true; }); break;   // IOBLOCKSIZE
                case 0x1000: attribute(1, bit, 8, [&](Packer& out) { out.reference(text(fs, 88, 1024)); return true; }); break;   // MOUNTPOINT
                case 0x2000: attribute(1, bit, 8, [&](Packer& out) { out.reference(volume_name(target, fs)); return true; }); break;   // NAME
                case 0x4000: attribute(1, bit, 4, [&](Packer& out) { out.u32(le32(fs, 64)); return true; }); break;   // MOUNTFLAGS
                case 0x8000: attribute(1, bit, 8, [&](Packer& out) { out.reference(text(fs, 1112, 1024)); return true; }); break;   // MOUNTEDDEVICE
                case 0x20000: attribute(1, bit, 32, [&](Packer& out) { capabilities(out, text(fs, 72, 16), (le32(fs, 64) & 0x1000) != 0); return true; }); break;
                case 0x40000000: attribute(1, bit, 40, [&](Packer& out) { supported_attributes(out); return true; }); break;   // ATTRIBUTES
                // SIGNATURE, OBJCOUNT, FILECOUNT, DIRCOUNT, MAXOBJCOUNT (u_int32), ENCODINGSUSED, QUOTA_SIZE,
                // RESERVED_SIZE (64 bits), UUID (16 bytes): not reported by the host volume.
                case 0x10000: case 0x10000000: case 0x20000000: attribute(1, bit, 8, never); break;
                case 0x40000: attribute(1, bit, 16, never); break;
                default: attribute(1, bit, 4, never); break;
                }
            }
        }
        const auto allocated = le64(status, 104) * 512;
        if (directory && is_directory) {
            for (std::uint32_t bit = 1; bit <= 0x20; bit <<= 1) {
                if (!(directory & bit)) continue;
                switch (bit) {
                case 0x1: attribute(2, bit, 4, [&](Packer& out) { out.u32(le32(status, 6) & 0xffff); return true; }); break;   // LINKCOUNT
                case 0x2: attribute(2, bit, 4, [&](Packer& out) { out.u32(entry_count(given.empty() ? full() : given)); return true; }); break;   // ENTRYCOUNT
                case 0x4: attribute(2, bit, 4, [&](Packer& out) { out.u32(is_root() ? 1U : 0U); return true; }); break;   // MOUNTSTATUS
                case 0x8: attribute(2, bit, 8, [&](Packer& out) { out.u64(allocated); return true; }); break;   // ALLOCSIZE
                case 0x10: attribute(2, bit, 4, [&](Packer& out) { out.u32(le32(status, 112)); return true; }); break;   // IOBLOCKSIZE
                default: attribute(2, bit, 8, [&](Packer& out) { out.u64(le64(status, 96)); return true; }); break;   // DATALENGTH
                }
            }
        }
        if (file && !is_directory) {
            for (std::uint32_t bit = 1; bit <= 0x4000; bit <<= 1) {
                if (!(file & bit)) continue;
                switch (bit) {
                case 0x1: attribute(3, bit, 4, [&](Packer& out) { out.u32(le32(status, 6) & 0xffff); return true; }); break;   // LINKCOUNT
                case 0x2: case 0x200: attribute(3, bit, 8, [&](Packer& out) { out.u64(le64(status, 96)); return true; }); break;   // TOTALSIZE, DATALENGTH
                case 0x4: case 0x400: attribute(3, bit, 8, [&](Packer& out) { out.u64(allocated); return true; }); break;   // ALLOCSIZE, DATAALLOCSIZE
                case 0x8: attribute(3, bit, 4, [&](Packer& out) { out.u32(le32(status, 112)); return true; }); break;   // IOBLOCKSIZE
                case 0x20: attribute(3, bit, 4, [&](Packer& out) { out.u32(le32(status, 24)); return true; }); break;   // DEVTYPE
                case 0x1000: case 0x2000: attribute(3, bit, 8, [](Packer& out) { out.u64(0); return true; }); break;   // RSRCLENGTH, RSRCALLOCSIZE
                // CLUMPSIZE, FILETYPE, FORKCOUNT (u_int32), FORKLIST (a reference), DATAEXTENTS and RSRCEXTENTS
                // (extentrecord): obsolete, not reported.
                case 0x100: attribute(3, bit, 8, never); break;
                case 0x800: case 0x4000: attribute(3, bit, 64, never); break;
                default: attribute(3, bit, 4, never); break;
                }
            }
        }
        if (extended && fork) {
            for (std::uint32_t bit = 0x4; bit <= 0x1000; bit <<= 1) {
                if (!(fork & bit)) continue;
                switch (bit) {
                case 0x10: attribute(4, bit, 8, [&](Packer& out) { out.u64(le64(status, 8)); return true; }); break;   // LINKID
                case 0x20: attribute(4, bit, 8, [&](Packer& out) { out.reference(full()); return true; }); break;   // NOFIRMLINKPATH
                case 0x40: attribute(4, bit, 4, [&](Packer& out) { out.u32(le32(status, 0)); return true; }); break;   // REALDEVID
                case 0x80: attribute(4, bit, 8, [&](Packer& out) { out.u64(le64(statfs_info(), 48)); return true; }); break;   // REALFSID
                // RELPATH (a reference), PRIVATESIZE, CLONEID, EXT_FLAGS, RECURSIVE_GENCOUNT, ATTRIBUTION_TAG (64
                // bits), CLONE_REFCNT (u_int32): not kept here.
                case 0x4: attribute(4, bit, 8, never); break;
                case 0x1000: attribute(4, bit, 4, never); break;
                default: attribute(4, bit, 8, never); break;
                }
            }
        }

        const bool with_returned = (common & attr_cmn_returned_attrs) != 0;
        if (with_returned) returned[0] |= attr_cmn_returned_attrs;
        const std::size_t prefix = 4 + (with_returned ? 20 : 0);
        const auto body = packer.finish(prefix);
        const auto total = prefix + body.size();
        std::vector<std::uint8_t> out(total);
        const auto length = static_cast<std::uint32_t>(total);
        std::memcpy(out.data(), &length, 4);
        if (with_returned) std::memcpy(out.data() + 4, returned.data(), 20);
        std::memcpy(out.data() + prefix, body.data(), body.size());
        return out;
    }

    std::uint64_t set(const Target& target, std::uint64_t list, std::uint64_t buffer, std::uint64_t size, std::uint32_t options) {
        static_cast<void>(options);
        const auto [common, volume, directory, file, fork] = read_list(list);
        constexpr std::uint32_t settable = 0x200 | 0x400 | 0x1000 | 0x4000 | 0x8000 | 0x10000 | 0x20000 | 0x40000;
        if (volume || directory || file || fork || (common & ~settable)) throw Errno{einval};
        std::vector<std::uint8_t> values(static_cast<std::size_t>(size));
        if (size) m_.read_bytes(buffer, values);
        std::size_t at = 0;
        const auto take = [&](std::size_t count) {
            if (at + count > values.size()) throw Errno{einval};
            const auto start = at;
            at += count;
            return start;
        };
        std::optional<std::pair<std::uint64_t, std::uint64_t>> modified, accessed;
        std::optional<std::uint32_t> owner, group, mode, flags;
        std::optional<std::vector<std::uint8_t>> finder;
        std::optional<std::int64_t> created;
        for (std::uint32_t bit = 0x200; bit <= 0x40000; bit <<= 1) {
            if (!(common & bit)) continue;
            switch (bit) {
            case 0x200: { const auto o = take(16); created = static_cast<std::int64_t>(le64(values, o)) * 1'000'000'000 + static_cast<std::int64_t>(le64(values, o + 8)); break; }
            case 0x400: { const auto o = take(16); modified = {le64(values, o), le64(values, o + 8)}; break; }
            case 0x1000: { const auto o = take(16); accessed = {le64(values, o), le64(values, o + 8)}; break; }
            case 0x4000: { const auto o = take(32); finder = std::vector<std::uint8_t>(values.begin() + static_cast<std::ptrdiff_t>(o), values.begin() + static_cast<std::ptrdiff_t>(o + 32)); break; }
            case 0x8000: owner = le32(values, take(4)); break;
            case 0x10000: group = le32(values, take(4)); break;
            case 0x20000: mode = le32(values, take(4)); break;
            case 0x40000: flags = le32(values, take(4)); break;
            default: break;
            }
        }
        const auto path = target.fd ? descriptor_path(*target.fd) : target.path;
        const auto follow = target.fd ? true : target.follow;
        const auto guest_path = string(path);
        const auto check = [&](std::uint64_t result) { if (static_cast<std::int32_t>(result) != 0) { const auto error = p_.guest_errno(); free(guest_path); throw Errno{error}; } };
        if (created) {
            const auto host = path_utf8(p_.process().map_guest_path(path));
            if (const auto set_created = host::files::set_creation_time(host, *created, follow); !set_created) {
                free(guest_path);
                throw Errno{set_created.error() == host::Error::not_found ? enoent : set_created.error() == host::Error::access_denied ? eacces : einval};
            }
        }
        // The Finder information first: the times set below are the ones that stay.
        if (finder) {
            const auto name = string("com.apple.FinderInfo");
            const auto data = p_.malloc(32);
            m_.write_bytes(data, *finder);
            const auto result = p_.call(libsystem_library, "_setxattr", {guest_path, name, data, 32, 0, follow ? 0U : 1U});
            free(data);
            free(name);
            check(result);
        }
        if (modified || accessed) {
            // struct timespec[2] {access, modification}; UTIME_OMIT (-2) for the one not set.
            const auto times = p_.malloc(32);
            const std::array<std::uint64_t, 4> words{accessed ? accessed->first : 0, accessed ? accessed->second : static_cast<std::uint64_t>(-2),
                                                     modified ? modified->first : 0, modified ? modified->second : static_cast<std::uint64_t>(-2)};
            for (std::size_t k = 0; k < 4; ++k) m_.write64(times + k * 8, words[k]);
            const auto result = p_.call(libsystem_library, "_utimensat", {static_cast<std::uint64_t>(at_fdcwd), guest_path, times, follow ? 0U : 0x20U});
            free(times);
            check(result);
        }
        if (owner || group) {
            // chown: the owner and group stay as they are (any other change is refused, as chown refuses it).
            const auto status = stat({path, std::nullopt, follow});
            if ((owner && *owner != le32(status, 16)) || (group && *group != le32(status, 20))) { free(guest_path); throw Errno{eperm}; }
        }
        if (mode) check(p_.call(libsystem_library, follow ? "_chmod" : "_lchmod", {guest_path, *mode & 07777}));
        if (flags) check(p_.call(libsystem_library, follow ? "_chflags" : "_lchflags", {guest_path, *flags}));
        free(guest_path);
        return 0;
    }

private:
    std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t> read_list(std::uint64_t list) {
        if (!list) throw Errno{efault};
        std::vector<std::uint8_t> bytes(24);
        m_.read_bytes(list, bytes);
        if ((le32(bytes, 0) & 0xffff) != 5) throw Errno{einval};   // ATTR_BIT_MAP_COUNT
        return {le32(bytes, 4), le32(bytes, 8), le32(bytes, 12), le32(bytes, 16), le32(bytes, 20)};
    }
    std::vector<std::uint8_t> stat(const Target& target) {
        const auto buffer = p_.malloc(stat_size);
        std::uint64_t result;
        if (target.fd) result = p_.call(libsystem_library, "_fstat", {*target.fd, buffer});
        else {
            const auto path = string(target.path);
            result = p_.call(libsystem_library, target.follow ? "_stat" : "_lstat", {path, buffer});
            free(path);
        }
        std::vector<std::uint8_t> bytes(stat_size);
        if (static_cast<std::int32_t>(result) == 0) m_.read_bytes(buffer, bytes);
        free(buffer);
        if (static_cast<std::int32_t>(result) != 0) throw Errno{p_.guest_errno()};
        return bytes;
    }
    std::vector<std::uint8_t> statfs(const Target& target) {
        const auto buffer = p_.malloc(statfs_size);
        std::uint64_t result;
        if (target.fd) result = p_.call(libsystem_library, "_fstatfs", {*target.fd, buffer});
        else {
            const auto path = string(target.path);
            result = p_.call(libsystem_library, "_statfs", {path, buffer});
            free(path);
        }
        std::vector<std::uint8_t> bytes(statfs_size);
        if (static_cast<std::int32_t>(result) == 0) m_.read_bytes(buffer, bytes);
        free(buffer);
        if (static_cast<std::int32_t>(result) != 0) throw Errno{p_.guest_errno()};
        return bytes;
    }
    // The object's absolute path, symbolic links in it resolved (the last one too unless FSOPT_NOFOLLOW).
    std::string full_path(const Target& target) {
        if (target.fd) return descriptor_path(*target.fd);
        auto path = target.path;
        std::string last;
        if (!target.follow) {
            const auto slash = path.find_last_of('/');
            if (slash != std::string::npos && slash + 1 < path.size()) { last = path.substr(slash + 1); path = slash == 0 ? "/" : path.substr(0, slash); }
            else if (slash == std::string::npos) { last = path; path = "."; }
        }
        const auto guest = string(path);
        const auto resolved = p_.call(libsystem_library, "_realpath", {guest, 0});
        free(guest);
        if (!resolved) throw Errno{p_.guest_errno()};
        auto result = read(resolved);
        free(resolved);
        while (result.size() > 1 && result.back() == '/') result.pop_back();   // a volume root's path (/Volumes/C/)
        if (!last.empty() && last != "." && last != "..") result = (result == "/" ? "" : result) + "/" + last;
        return result;
    }
    std::vector<std::uint8_t> finder_info(const Target& target) {
        std::vector<std::uint8_t> info(32, 0);
        const auto name = string("com.apple.FinderInfo");
        const auto data = p_.malloc(32);
        std::uint64_t result;
        if (target.fd) result = p_.call(libsystem_library, "_fgetxattr", {*target.fd, name, data, 32, 0, 0});
        else {
            const auto path = string(target.path);
            result = p_.call(libsystem_library, "_getxattr", {path, name, data, 32, 0, target.follow ? 0U : 1U});
            free(path);
        }
        if (static_cast<std::int64_t>(result) == 32) m_.read_bytes(data, info);
        const auto error = static_cast<std::int64_t>(result) < 0 ? p_.guest_errno() : 0;
        free(data);
        free(name);
        // No Finder information (ENOATTR, or a volume without extended attributes) is all zeros.
        if (static_cast<std::int64_t>(result) < 0 && error != enoattr && error != 45 && error != 102) throw Errno{error};
        return info;
    }
    std::uint32_t user_access(const std::string& path) {
        const auto guest = string(path);
        std::uint32_t result{};
        for (const std::uint32_t bit : {4U, 2U, 1U})   // R_OK, W_OK, X_OK
            if (p_.call(libsystem_library, "_access", {guest, bit}) == 0) result |= bit;
        free(guest);
        return result;
    }
    // A directory's entry names in readdir order, without "." and "..".
    std::vector<std::string> entry_names(const std::string& path) {
        const auto guest = string(path);
        const auto directory = p_.call(libsystem_library, "_opendir", {guest});
        free(guest);
        if (!directory) throw Errno{p_.guest_errno()};
        std::vector<std::string> names;
        while (const auto entry = p_.call(libsystem_library, "_readdir", {directory})) {
            auto name = read(entry + 21);   // struct dirent: d_name at 21
            if (name != "." && name != "..") names.push_back(std::move(name));
        }
        p_.call(libsystem_library, "_closedir", {directory});
        return names;
    }
    std::uint32_t entry_count(const std::string& path) {
        const auto guest = string(path);
        const auto directory = p_.call(libsystem_library, "_opendir", {guest});
        free(guest);
        if (!directory) throw Errno{p_.guest_errno()};
        std::uint32_t count{};
        while (const auto entry = p_.call(libsystem_library, "_readdir", {directory})) {
            const auto name = read(entry + 21);   // struct dirent: d_name at 21
            if (name != "." && name != "..") ++count;
        }
        p_.call(libsystem_library, "_closedir", {directory});
        return count;
    }
    // The volume's name: its host label, else the mount point's last component.
    std::string volume_name(const Target& target, const std::vector<std::uint8_t>& fs) {
        const auto mount = text(fs, 88, 1024);
        const auto host_path = path_utf8(p_.process().map_guest_path(target.fd ? descriptor_path(*target.fd) : target.path));
        if (const auto volume = host::files::volume(host_path); volume && !volume->name.empty()) return volume->name;
        const auto slash = mount.find_last_of('/');
        return mount == "/" || slash == std::string::npos ? std::string("/") : mount.substr(slash + 1);
    }
    // vol_capabilities_attr_t: capabilities[4] then valid[4] (FORMAT, INTERFACES, two reserved).
    static void capabilities(Packer& out, const std::string& type, bool local) {
        const bool ntfs = type == "ntfs" || type == "refs";
        std::uint32_t format = 0x200 | 0x400 | 0x800 | 0x2000;   // CASE_PRESERVING, FAST_STATFS, 2TB_FILESIZE, HIDDEN_FILES
        if (ntfs) format |= 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20000;   // PERSISTENTOBJECTIDS, SYMBOLICLINKS, HARDLINKS, JOURNAL(_ACTIVE), 64BIT_OBJECT_IDS
        if (!local) format &= ~0x400U;
        std::uint32_t interfaces = 0x2 | 0x100 | 0x200 | 0x800 | 0x80000;   // ATTRLIST, ADVLOCK, FLOCK, USERACCESS, RENAME_EXCL
        if (ntfs) interfaces |= 0x4000;   // EXTENDED_ATTR
        out.u32(format); out.u32(interfaces); out.u32(0); out.u32(0);
        out.u32(0x03FFFFFF); out.u32(0x001F7FFF); out.u32(0); out.u32(0);   // every capability bit defined is known
    }
    // vol_attributes_attr_t: validattr then nativeattr (what getattrlist answers here).
    static void supported_attributes(Packer& out) {
        constexpr std::array<std::uint32_t, 5> answered{0x8E27FFFFU, 0xC002F0FDU, 0x3FU, 0x362FU, 0};
        for (int round = 0; round < 2; ++round) for (const auto value : answered) out.u32(value);
    }
    static std::uint32_t object_type(std::uint32_t mode) {
        switch (mode & 0170000) {
        case 0100000: return 1;   // VREG
        case 0040000: return 2;   // VDIR
        case 0060000: return 3;   // VBLK
        case 0020000: return 4;   // VCHR
        case 0120000: return 5;   // VLNK
        case 0140000: return 6;   // VSOCK
        case 0010000: return 7;   // VFIFO
        default: return 0;        // VNON
        }
    }
    static std::string text(const std::vector<std::uint8_t>& bytes, std::size_t at, std::size_t limit) {
        std::string value;
        for (std::size_t k = at; k < at + limit && k < bytes.size() && bytes[k]; ++k) value += static_cast<char>(bytes[k]);
        return value;
    }
    std::string read(std::uint64_t address) const {
        const auto bytes = m_.read_string(address, 1, 4096);
        return {bytes.begin(), bytes.end()};
    }
    std::uint64_t string(const std::string& value) const {
        const auto address = p_.malloc(value.size() + 1);
        m_.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
        m_.write8(address + value.size(), 0);
        return address;
    }
    void free(std::uint64_t address) const { if (address) p_.call(libsystem_library, "_free", {address}); }

    const Provider& p_;
    GuestMemory& m_;
};

} // namespace

void install_libsystem_attrlist(Provider& p) {
    // getattrlist(path, attrList, attrBuf, size, options); fgetattrlist(fd, ...); getattrlistat(dirfd, path, ..., options).
    p.libsystem("_getattrlist", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        AttributeLists lists(p, m);
        const auto options = static_cast<std::uint32_t>(s.x[4]);
        return lists.get({lists.at_path(static_cast<std::uint64_t>(at_fdcwd), s.x[0]), std::nullopt, !(options & fsopt_nofollow)}, s.x[1], s.x[2], s.x[3], options);
    });
    p.libsystem("_fgetattrlist", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        AttributeLists lists(p, m);
        return lists.get({"", s.x[0], true}, s.x[1], s.x[2], s.x[3], static_cast<std::uint32_t>(s.x[4]));
    });
    p.libsystem("_getattrlistat", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        AttributeLists lists(p, m);
        const auto options = static_cast<std::uint32_t>(s.x[5]);
        return lists.get({lists.at_path(s.x[0], s.x[1]), std::nullopt, !(options & fsopt_nofollow)}, s.x[2], s.x[3], s.x[4], options);
    });
    p.libsystem("_getattrlistbulk", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        AttributeLists lists(p, m);
        return lists.bulk(s.x[0], s.x[1], s.x[2], s.x[3], s.x[4]);
    });
    p.libsystem("_setattrlist", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        AttributeLists lists(p, m);
        const auto options = static_cast<std::uint32_t>(s.x[4]);
        return lists.set({lists.at_path(static_cast<std::uint64_t>(at_fdcwd), s.x[0]), std::nullopt, !(options & fsopt_nofollow)}, s.x[1], s.x[2], s.x[3], options);
    });
    p.libsystem("_fsetattrlist", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        AttributeLists lists(p, m);
        return lists.set({"", s.x[0], true}, s.x[1], s.x[2], s.x[3], static_cast<std::uint32_t>(s.x[4]));
    });
    p.libsystem("_setattrlistat", [p](CpuState& s, GuestMemory& m) -> std::uint64_t {
        AttributeLists lists(p, m);
        const auto options = static_cast<std::uint32_t>(s.x[5]);
        return lists.set({lists.at_path(s.x[0], s.x[1]), std::nullopt, !(options & fsopt_nofollow)}, s.x[2], s.x[3], s.x[4], options);
    });
}

} // namespace alchemy::export_fill
