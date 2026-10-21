#include "alchemy/host/files.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <aclapi.h>
#include <sddl.h>
#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <filesystem>
#include <limits>

namespace alchemy::host::files {

namespace {
// The protected owner-rights ACL of an owner-only file or directory: exclusive
// creation must never expose a new private file through an inherited public
// directory ACL.
struct OwnerOnly {
    PSECURITY_DESCRIPTOR descriptor{};
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    bool make() {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr)) return false;
        attributes.lpSecurityDescriptor = descriptor;
        return true;
    }
    ~OwnerOnly() { if (descriptor) LocalFree(descriptor); }
};
} // namespace

Result<Handle> open(std::string_view path, Access access, Create create, std::uint32_t options) {
    DWORD desired = (options & open_deletable) ? DELETE : 0;
    if (static_cast<int>(access) & static_cast<int>(Access::read)) desired |= GENERIC_READ;
    if (static_cast<int>(access) & static_cast<int>(Access::write))
        desired |= (options & open_append) ? FILE_APPEND_DATA | SYNCHRONIZE : GENERIC_WRITE;
    DWORD disposition = OPEN_EXISTING;
    switch (create) {
    case Create::open_existing: disposition = OPEN_EXISTING; break;
    case Create::create_new: disposition = CREATE_NEW; break;
    case Create::open_or_create: disposition = OPEN_ALWAYS; break;
    case Create::truncate_existing: disposition = TRUNCATE_EXISTING; break;
    case Create::create_or_truncate: disposition = CREATE_ALWAYS; break;
    }
    OwnerOnly owner;
    if ((options & open_owner_only) && !owner.make()) return win32::last_error();
    const DWORD attributes = (options & open_create_read_only) ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL;
    const auto file = CreateFileW(win32::widen(path).c_str(), desired, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  (options & open_owner_only) ? &owner.attributes : nullptr, disposition,
                                  attributes | FILE_FLAG_BACKUP_SEMANTICS | ((options & open_sequential) ? FILE_FLAG_SEQUENTIAL_SCAN : 0), nullptr);
    if (file == INVALID_HANDLE_VALUE) return win32::last_error();
    return win32::wrap(file);
}

Result<Handle> open_null(Access access) {
    DWORD desired = 0;
    if (static_cast<int>(access) & static_cast<int>(Access::read)) desired |= GENERIC_READ;
    if (static_cast<int>(access) & static_cast<int>(Access::write)) desired |= GENERIC_WRITE;
    const auto file = CreateFileW(L"NUL", desired, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return win32::last_error();
    return win32::wrap(file);
}

Result<std::size_t> read_at(Handle file, Bytes buffer, std::uint64_t offset) {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done = 0;
    const auto size = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), std::numeric_limits<DWORD>::max()));
    if (!ReadFile(win32::native(file), buffer.data(), size, &done, &position)) {
        const auto code = GetLastError();
        if (code == ERROR_HANDLE_EOF) return std::size_t{0};
        return win32::error(code);
    }
    return static_cast<std::size_t>(done);
}

Result<std::size_t> write_at(Handle file, ConstBytes data, std::uint64_t offset) {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD done = 0;
    const auto size = static_cast<DWORD>(std::min<std::size_t>(data.size(), std::numeric_limits<DWORD>::max()));
    if (!WriteFile(win32::native(file), data.data(), size, &done, &position)) return win32::last_error();
    return static_cast<std::size_t>(done);
}

Result<std::size_t> read(Handle file, Bytes buffer) {
    DWORD done = 0;
    const auto size = static_cast<DWORD>(std::min<std::size_t>(buffer.size(), std::numeric_limits<DWORD>::max()));
    if (!ReadFile(win32::native(file), buffer.data(), size, &done, nullptr)) {
        const auto code = GetLastError();
        if (code == ERROR_HANDLE_EOF || code == ERROR_BROKEN_PIPE) return std::size_t{0};
        return win32::error(code);
    }
    return static_cast<std::size_t>(done);
}

Result<std::size_t> write(Handle file, ConstBytes data) {
    DWORD done = 0;
    const auto size = static_cast<DWORD>(std::min<std::size_t>(data.size(), std::numeric_limits<DWORD>::max()));
    if (!WriteFile(win32::native(file), data.data(), size, &done, nullptr)) return win32::last_error();
    return static_cast<std::size_t>(done);
}

Result<std::uint64_t> size(Handle file) {
    LARGE_INTEGER value{};
    if (!GetFileSizeEx(win32::native(file), &value)) return win32::last_error();
    return static_cast<std::uint64_t>(value.QuadPart);
}

Result<Attributes> attributes(std::string_view path) {
    const auto value = GetFileAttributesW(win32::widen(path).c_str());
    if (value == INVALID_FILE_ATTRIBUTES) {
        const auto code = GetLastError();
        return code == ERROR_PATH_NOT_FOUND || code == ERROR_INVALID_NAME ? Error::not_found : win32::error(code);
    }
    return Attributes{(value & FILE_ATTRIBUTE_DIRECTORY) != 0, (value & FILE_ATTRIBUTE_READONLY) != 0,
                      (value & FILE_ATTRIBUTE_HIDDEN) != 0, (value & FILE_ATTRIBUTE_REPARSE_POINT) != 0};
}

Result<Times> times(std::string_view path) {
    // Backup semantics open directories (and volume roots) too.
    const auto file = CreateFileW(win32::widen(path).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return win32::last_error();
    BY_HANDLE_FILE_INFORMATION details{};
    const auto ok = GetFileInformationByHandle(file, &details);
    const auto error = GetLastError();
    CloseHandle(file);
    if (!ok) return win32::error(error);
    // FILETIME counts 100 ns from 1601-01-01.
    constexpr std::int64_t unix_epoch = 116444736000000000LL;
    const auto unix_ns = [](const FILETIME& time) {
        return (static_cast<std::int64_t>(win32::nanoseconds(time) / 100) - unix_epoch) * 100;
    };
    return Times{unix_ns(details.ftCreationTime), unix_ns(details.ftLastWriteTime), unix_ns(details.ftLastAccessTime)};
}

namespace {
// The root of the volume holding `path`, with its trailing separator; empty on failure.
std::wstring volume_root(const std::wstring& path) {
    std::wstring root(MAX_PATH + 1, L'\0');
    if (!GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size()))) return {};
    root.resize(wcslen(root.c_str()));
    if (!root.empty() && root.back() != L'\\') root += L'\\';   // the volume APIs need the trailing separator
    return root;
}
} // namespace

Result<Volume> volume(std::string_view path) {
    const auto root = volume_root(win32::widen(path));
    if (root.empty()) return win32::last_error();
    Volume result;
    switch (GetDriveTypeW(root.c_str())) {
    case DRIVE_NO_ROOT_DIR: return Error::not_found;
    case DRIVE_FIXED: result.kind = VolumeKind::fixed; break;
    case DRIVE_REMOVABLE: result.kind = VolumeKind::removable; break;
    case DRIVE_CDROM: result.kind = VolumeKind::optical; break;
    case DRIVE_REMOTE: result.kind = VolumeKind::network; break;
    case DRIVE_RAMDISK: result.kind = VolumeKind::ram; break;
    default: result.kind = VolumeKind::unknown; break;
    }
    wchar_t label[MAX_PATH + 1]{}, filesystem[MAX_PATH + 1]{};
    DWORD serial = 0, maximum = 0, flags = 0;
    if (!GetVolumeInformationW(root.c_str(), label, MAX_PATH + 1, &serial, &maximum, &flags, filesystem, MAX_PATH + 1))
        return win32::last_error();
    result.root = win32::narrow(root);
    result.name = win32::narrow(label);
    result.filesystem = win32::narrow(filesystem);
    result.serial = serial;
    result.max_name_length = maximum;
    result.read_only = (flags & FILE_READ_ONLY_VOLUME) != 0;
    wchar_t windows[MAX_PATH + 1]{};
    if (const auto length = GetWindowsDirectoryW(windows, MAX_PATH + 1); length && length <= MAX_PATH)
        result.system = CompareStringOrdinal(volume_root(windows).c_str(), -1, root.c_str(), -1, TRUE) == CSTR_EQUAL;
    ULARGE_INTEGER available{}, total{}, free{};
    if (GetDiskFreeSpaceExW(root.c_str(), &available, &total, &free)) {
        result.total = total.QuadPart;
        result.free = free.QuadPart;
        result.available = available.QuadPart;
    }
    DWORD sectors = 0, bytes = 0, free_clusters = 0, clusters = 0;
    if (GetDiskFreeSpaceW(root.c_str(), &sectors, &bytes, &free_clusters, &clusters)) result.block_size = sectors * bytes;
    return result;
}

Result<std::vector<std::string>> volume_roots() {
    const auto drives = GetLogicalDrives();
    if (!drives) return win32::last_error();
    std::vector<std::string> roots;
    for (unsigned letter = 0; letter < 26; ++letter)
        if (drives & (1U << letter)) roots.push_back(std::string{static_cast<char>('A' + letter), ':', '\\'});
    return roots;
}

Result<std::string> create_temporary(std::string_view prefix) {
    wchar_t directory[MAX_PATH + 1]{}, name[MAX_PATH + 1]{};
    const auto length = GetTempPathW(MAX_PATH + 1, directory);
    if (!length || length > MAX_PATH) return length ? Error::name_too_long : win32::last_error();
    // GetTempFileNameW uses up to three characters of the prefix.
    const auto short_prefix = win32::widen(prefix.substr(0, 3));
    if (!GetTempFileNameW(directory, short_prefix.c_str(), 0, name)) return win32::last_error();
    return win32::narrow(name);
}

Status replace(std::string_view from, std::string_view to) {
    if (!MoveFileExW(win32::widen(from).c_str(), win32::widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return win32::last_error();
    return {};
}

Result<Handle> open_metadata(std::string_view path) {
    // Backup semantics open directories too.
    const auto file = CreateFileW(win32::widen(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return win32::last_error();
    return win32::wrap(file);
}

Result<Info> info(Handle file) {
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(win32::native(file), FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(win32::native(file), FileStandardInfo, &standard, sizeof(standard)))
        return win32::last_error();
    // FILETIME-style times count 100 ns from 1601-01-01.
    constexpr std::int64_t unix_epoch = 116444736000000000LL;
    const auto unix_ns = [](const LARGE_INTEGER& time) { return (time.QuadPart - unix_epoch) * 100; };
    Info result;
    result.modified_ns = unix_ns(basic.LastWriteTime);
    result.changed_ns = unix_ns(basic.ChangeTime);
    result.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    result.links = standard.NumberOfLinks;
    result.mode = basic.FileAttributes;
    result.directory = (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.delete_pending = standard.DeletePending || standard.NumberOfLinks == 0;
    return result;
}

Result<std::string> final_path(Handle file) {
    std::wstring buffer(32768, L'\0');
    const auto length = GetFinalPathNameByHandleW(win32::native(file), buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED);
    if (!length) return win32::last_error();
    if (length >= buffer.size()) return Error::name_too_long;
    buffer.resize(length);
    return win32::narrow(buffer);
}

Result<Handle> watch_directory(std::string_view path) {
    const auto watch = FindFirstChangeNotificationW(win32::widen(path).c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
    if (watch == INVALID_HANDLE_VALUE) return win32::last_error();
    return win32::wrap(watch);
}

Result<bool> directory_changed(Handle watch) {
    const auto result = WaitForSingleObject(win32::native(watch), 0);
    if (result == WAIT_TIMEOUT) return false;
    if (result != WAIT_OBJECT_0) return win32::last_error();
    if (!FindNextChangeNotification(win32::native(watch))) return win32::last_error();
    return true;
}

void close_watch(Handle watch) noexcept {
    if (watch) FindCloseChangeNotification(win32::native(watch));
}

namespace {

// ReadDirectoryChangesW over the whole tree, one overlapped read kept
// outstanding between read() calls so no change is lost between them.
class Win32TreeWatch final : public TreeWatch {
public:
    Win32TreeWatch(HANDLE directory, HANDLE completed, HANDLE cancelled)
        : directory_(directory), completed_(completed), cancelled_(cancelled) {
        overlapped_.hEvent = completed_;
    }
    ~Win32TreeWatch() override {
        if (pending_) {
            CancelIoEx(directory_, &overlapped_);
            DWORD bytes{};
            GetOverlappedResult(directory_, &overlapped_, &bytes, TRUE);
        }
        CloseHandle(directory_);
        CloseHandle(completed_);
        CloseHandle(cancelled_);
    }
    Result<std::vector<TreeChange>> read(std::uint32_t timeout_ms, bool& overflow) override {
        overflow = false;
        if (WaitForSingleObject(cancelled_, 0) == WAIT_OBJECT_0) return Error::interrupted;
        if (!pending_) {
            constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
                                     FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION;
            ResetEvent(completed_);
            if (!ReadDirectoryChangesW(directory_, buffer_.data(), static_cast<DWORD>(buffer_.size() * sizeof(DWORD)), TRUE, filter,
                                       nullptr, &overlapped_, nullptr))
                return win32::last_error();
            pending_ = true;
        }
        const std::array<HANDLE, 2> events{completed_, cancelled_};
        const auto waited = WaitForMultipleObjects(2, events.data(), FALSE, timeout_ms);
        if (waited == WAIT_TIMEOUT) return std::vector<TreeChange>{};
        if (waited == WAIT_OBJECT_0 + 1) return Error::interrupted;
        if (waited != WAIT_OBJECT_0) return win32::last_error();
        DWORD bytes{};
        pending_ = false;
        if (!GetOverlappedResult(directory_, &overlapped_, &bytes, FALSE)) {
            const auto code = GetLastError();
            if (code == ERROR_NOTIFY_ENUM_DIR) { overflow = true; return std::vector<TreeChange>{}; }
            return win32::error(code);
        }
        std::vector<TreeChange> changes;
        if (bytes == 0) { overflow = true; return changes; }   // the buffer overflowed: changes were dropped
        const auto* base = reinterpret_cast<const std::uint8_t*>(buffer_.data());
        for (DWORD offset = 0;;) {
            const auto* entry = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            TreeChange change;
            switch (entry->Action) {
            case FILE_ACTION_ADDED: change.kind = TreeChange::Kind::created; break;
            case FILE_ACTION_REMOVED: change.kind = TreeChange::Kind::removed; break;
            case FILE_ACTION_RENAMED_OLD_NAME: change.kind = TreeChange::Kind::renamed_from; break;
            case FILE_ACTION_RENAMED_NEW_NAME: change.kind = TreeChange::Kind::renamed_to; break;
            default: change.kind = TreeChange::Kind::modified; break;
            }
            change.path = win32::narrow(std::wstring_view(entry->FileName, entry->FileNameLength / sizeof(WCHAR)));
            std::replace(change.path.begin(), change.path.end(), '\\', '/');
            changes.push_back(std::move(change));
            if (!entry->NextEntryOffset) break;
            offset += entry->NextEntryOffset;
        }
        return changes;
    }
    void cancel() noexcept override { SetEvent(cancelled_); }

private:
    HANDLE directory_, completed_, cancelled_;
    OVERLAPPED overlapped_{};
    bool pending_{};
    std::array<DWORD, 16384> buffer_{};   // 64 KiB, DWORD-aligned as ReadDirectoryChangesW requires
};

} // namespace

Result<std::unique_ptr<TreeWatch>> watch_tree(std::string_view path) {
    const auto directory = CreateFileW(win32::widen(path).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (directory == INVALID_HANDLE_VALUE) return win32::last_error();
    const auto completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const auto cancelled = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed || !cancelled) {
        const auto failure = win32::last_error();
        if (completed) CloseHandle(completed);
        if (cancelled) CloseHandle(cancelled);
        CloseHandle(directory);
        return failure;
    }
    return std::unique_ptr<TreeWatch>(std::make_unique<Win32TreeWatch>(directory, completed, cancelled));
}

Result<std::uint64_t> seek(Handle file, std::int64_t offset, Whence whence) {
    LARGE_INTEGER distance{}, position{};
    distance.QuadPart = offset;
    const DWORD method = whence == Whence::set ? FILE_BEGIN : whence == Whence::current ? FILE_CURRENT : FILE_END;
    if (!SetFilePointerEx(win32::native(file), distance, &position, method)) {
        const auto code = GetLastError();
        // A negative resulting position: EINVAL, as lseek.
        return code == ERROR_NEGATIVE_SEEK ? Error::invalid_argument : win32::error(code);
    }
    return static_cast<std::uint64_t>(position.QuadPart);
}

Status set_size(Handle file, std::uint64_t size) {
    FILE_END_OF_FILE_INFO end{};
    end.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    if (!SetFileInformationByHandle(win32::native(file), FileEndOfFileInfo, &end, sizeof(end))) {
        const auto code = GetLastError();
        return code == ERROR_USER_MAPPED_FILE ? Error::busy : win32::error(code);
    }
    return {};
}

Status flush(Handle file) {
    if (!FlushFileBuffers(win32::native(file))) return win32::last_error();
    return {};
}

Status sync_directory(std::string_view path) {
    // FlushFileBuffers needs a handle open for writing; NTFS journals
    // directory metadata, so a directory that cannot be opened that way is
    // still durable.
    const auto directory = CreateFileW(win32::widen(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (directory == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return Error::not_found;
        return {};
    }
    const bool flushed = FlushFileBuffers(directory) != 0;
    const auto code = GetLastError();
    CloseHandle(directory);
    if (!flushed && code != ERROR_ACCESS_DENIED && code != ERROR_INVALID_FUNCTION) return Error::io;
    return {};
}

bool is_character_device(Handle file) noexcept { return GetFileType(win32::native(file)) == FILE_TYPE_CHAR; }

Kind kind(Handle file) noexcept {
    switch (GetFileType(win32::native(file))) {
    case FILE_TYPE_DISK: return Kind::file;
    case FILE_TYPE_PIPE: return Kind::pipe;
    case FILE_TYPE_CHAR: return Kind::character;
    default: return Kind::unknown;
    }
}

Status remove_open(Handle file) {
    FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
    if (!SetFileInformationByHandle(win32::native(file), FileDispositionInfoEx, &disposition, sizeof(disposition))) return win32::last_error();
    return {};
}

Status create_directory(std::string_view path, bool owner_only) {
    OwnerOnly owner;
    if (owner_only && !owner.make()) return win32::last_error();
    if (!CreateDirectoryW(win32::widen(path).c_str(), owner_only ? &owner.attributes : nullptr)) return win32::last_error();
    return {};
}

Status set_read_only(std::string_view path, bool read_only) {
    const auto wide = win32::widen(path);
    const auto attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return win32::last_error();
    const DWORD changed = read_only ? (attributes | FILE_ATTRIBUTE_READONLY) : (attributes & ~DWORD{FILE_ATTRIBUTE_READONLY});
    if (changed != attributes && !SetFileAttributesW(wide.c_str(), changed ? changed : FILE_ATTRIBUTE_NORMAL)) return win32::last_error();
    return {};
}

Result<Pipe> create_pipe() {
    HANDLE read_end{}, write_end{};
    if (!CreatePipe(&read_end, &write_end, nullptr, 65536)) return win32::last_error();   // a 64 KiB buffer
    return Pipe{win32::wrap(read_end), win32::wrap(write_end)};
}

Result<std::size_t> pipe_available(Handle pipe) {
    DWORD available = 0;
    if (!PeekNamedPipe(win32::native(pipe), nullptr, 0, nullptr, &available, nullptr)) return win32::last_error();
    return static_cast<std::size_t>(available);
}

namespace {
// FILETIME-style times count 100 ns from 1601-01-01.
std::int64_t unix_ns(const LARGE_INTEGER& time) { return (time.QuadPart - 116444736000000000LL) * 100; }
// Whether the file's DACL is exactly the protected owner-rights ACL of OwnerOnly.
bool owner_only_acl(HANDLE file) {
    bool result = false;
    PACL acl{};
    PSECURITY_DESCRIPTOR security{};
    if (GetSecurityInfo(file, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &security) != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision{};
    void* entry{};
    if (acl && acl->AceCount == 1 && GetSecurityDescriptorControl(security, &control, &revision) && (control & SE_DACL_PROTECTED) &&
        GetAce(acl, 0, &entry)) {
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(entry);
        if (ace->Header.AceType == ACCESS_ALLOWED_ACE_TYPE && ace->Header.AceFlags == 0 && ace->Mask == FILE_ALL_ACCESS) {
            const auto sid = const_cast<DWORD*>(&ace->SidStart);
            constexpr SID_IDENTIFIER_AUTHORITY creator{{0, 0, 0, 0, 0, 3}};   // CREATOR OWNER RIGHTS (S-1-3-4)
            result = IsValidSid(sid) && *GetSidSubAuthorityCount(sid) == 1 &&
                     std::memcmp(GetSidIdentifierAuthority(sid), &creator, sizeof(creator)) == 0 && *GetSidSubAuthority(sid, 0) == 4;
        }
    }
    LocalFree(security);
    return result;
}
Result<Metadata> describe(HANDLE file, bool symlinks) {
    BY_HANDLE_FILE_INFORMATION information{};
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandle(file, &information) || !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic)) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)))
        return win32::last_error();
    Metadata result;
    if (symlinks && (information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &tag, sizeof(tag))) return win32::last_error();
        result.symlink = tag.ReparseTag == IO_REPARSE_TAG_SYMLINK || tag.ReparseTag == IO_REPARSE_TAG_MOUNT_POINT;
    }
    result.device = information.dwVolumeSerialNumber;
    result.inode = (static_cast<std::uint64_t>(information.nFileIndexHigh) << 32) | information.nFileIndexLow;
    result.size = static_cast<std::uint64_t>(standard.EndOfFile.QuadPart);
    result.allocated = static_cast<std::uint64_t>(standard.AllocationSize.QuadPart);
    result.links = information.nNumberOfLinks;
    result.mode = information.dwFileAttributes;
    result.directory = (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    result.read_only = (information.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    result.owner_only = owner_only_acl(file);
    result.accessed_ns = unix_ns(basic.LastAccessTime);
    result.modified_ns = unix_ns(basic.LastWriteTime);
    result.changed_ns = unix_ns(basic.ChangeTime);
    result.created_ns = unix_ns(basic.CreationTime);
    return result;
}
} // namespace

Result<Metadata> metadata(std::string_view path, bool follow) {
    const auto file = CreateFileW(win32::widen(path).c_str(), FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0 : FILE_FLAG_OPEN_REPARSE_POINT), nullptr);
    if (file == INVALID_HANDLE_VALUE) return win32::last_error();
    auto result = describe(file, !follow);
    CloseHandle(file);
    return result;
}

Result<Metadata> metadata(Handle file) { return describe(win32::native(file), false); }

Result<Identity> identity(Handle file) {
    FILE_ID_INFO information{};
    if (!GetFileInformationByHandleEx(win32::native(file), FileIdInfo, &information, sizeof(information))) return win32::last_error();
    Identity result;
    result.volume = information.VolumeSerialNumber;
    std::memcpy(result.id.data(), information.FileId.Identifier, result.id.size());
    return result;
}

Status lock(Handle file, std::uint64_t offset, std::uint64_t length, bool exclusive) {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    if (!LockFileEx(win32::native(file), LOCKFILE_FAIL_IMMEDIATELY | (exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0), 0,
                    static_cast<DWORD>(length), static_cast<DWORD>(length >> 32), &position)) {
        const auto code = GetLastError();
        return code == ERROR_LOCK_VIOLATION ? Error::would_block : win32::error(code);
    }
    return {};
}

Status unlock(Handle file, std::uint64_t offset, std::uint64_t length) {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32);
    if (!UnlockFileEx(win32::native(file), 0, static_cast<DWORD>(length), static_cast<DWORD>(length >> 32), &position)) return win32::last_error();
    return {};
}

Status set_times(std::string_view path, std::optional<std::int64_t> accessed_ns, std::optional<std::int64_t> modified_ns) {
    // FILETIME counts 100 ns from 1601-01-01.
    constexpr std::int64_t epoch = 11644473600LL * 10'000'000LL;
    const auto convert = [](std::int64_t value, FILETIME& out) {
        const auto hundreds = value / 100;
        if (hundreds < -epoch || hundreds > INT64_MAX - epoch) return false;
        const auto ticks = static_cast<std::uint64_t>(hundreds + epoch);
        out = FILETIME{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        return true;
    };
    FILETIME accessed{}, modified{};
    if ((accessed_ns && !convert(*accessed_ns, accessed)) || (modified_ns && !convert(*modified_ns, modified))) return Error::invalid_argument;
    const auto file = CreateFileW(win32::widen(path).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return win32::last_error();
    const bool set = SetFileTime(file, nullptr, accessed_ns ? &accessed : nullptr, modified_ns ? &modified : nullptr) != 0;
    const auto code = GetLastError();
    CloseHandle(file);
    if (!set) return win32::error(code);
    return {};
}

Status rename(std::string_view from, std::string_view to, bool replace) {
    const auto source = win32::widen(from), target = win32::widen(to);
    if (replace) {
        // POSIX semantics: the name is replaced even while the old file is
        // open elsewhere (with FILE_SHARE_DELETE, as every Alchemy open is),
        // as rename(2) does; MoveFileEx refuses that with access denied.
        const auto file = CreateFileW(source.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            // The full target path in its verbatim form (\\?\C:\...), as the call needs.
            std::wstring name(GetFullPathNameW(target.c_str(), 0, nullptr, nullptr), L'\0');
            if (!name.empty()) name.resize(GetFullPathNameW(target.c_str(), static_cast<DWORD>(name.size()), name.data(), nullptr));
            if (name.empty()) name = target;
            if (name.size() > 2 && name[1] == L':') name = LR"(\\?\)" + name;
            std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name.size() * sizeof(wchar_t));
            auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
            info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            info->RootDirectory = nullptr;
            info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
            std::memcpy(info->FileName, name.data(), name.size() * sizeof(wchar_t));
            const bool renamed = SetFileInformationByHandle(file, FileRenameInfoEx, info, static_cast<DWORD>(buffer.size())) != 0;
            const auto code = GetLastError();
            CloseHandle(file);
            if (renamed) return {};
            // A file system or host without POSIX renames (FAT, older Windows) falls back.
            if (code != ERROR_INVALID_PARAMETER && code != ERROR_NOT_SUPPORTED && code != ERROR_INVALID_FUNCTION &&
                code != ERROR_ACCESS_DENIED)
                return win32::error(code);
        }
    }
    if (!MoveFileExW(source.c_str(), target.c_str(), replace ? MOVEFILE_REPLACE_EXISTING : 0)) return win32::last_error();
    return {};
}

Status access(std::string_view path, bool write) {
    const auto attributes = GetFileAttributesW(win32::widen(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return win32::last_error();
    if (write && (attributes & FILE_ATTRIBUTE_READONLY)) return Error::access_denied;
    return {};
}

bool is_console(Handle file) noexcept {
    DWORD mode = 0;
    return GetConsoleMode(win32::native(file), &mode) != 0;
}

namespace {
// An attribute is the alternate data stream "file:name". A ':' in a name
// (com.apple.metadata:...) is stored as U+F03A, the private-use mapping WSL
// and Cygwin use for characters NTFS names cannot hold.
std::wstring attribute_stream(std::string_view path, std::string_view name) {
    auto stream = win32::widen(name);
    std::replace(stream.begin(), stream.end(), L':', L'\xf03a');
    return win32::widen(path) + L":" + stream;
}
Error attribute_error(DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? Error::not_found : win32::error(code); }
} // namespace

Result<std::vector<std::byte>> get_attribute(std::string_view path, std::string_view name) {
    const auto stream = CreateFileW(attribute_stream(path, name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (stream == INVALID_HANDLE_VALUE) return attribute_error(GetLastError());
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{stream};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(stream, &size)) return win32::last_error();
    if (size.QuadPart > 0x7fffffffLL) return Error::out_of_range;
    std::vector<std::byte> value(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (!value.empty() && (!ReadFile(stream, value.data(), static_cast<DWORD>(value.size()), &read, nullptr) || read != value.size())) return Error::io;
    return value;
}

Result<bool> has_attribute(std::string_view path, std::string_view name) {
    if (GetFileAttributesW(attribute_stream(path, name).c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const auto error = attribute_error(GetLastError());
    if (error == Error::not_found) return false;
    return error;
}

Status set_attribute(std::string_view path, std::string_view name, ConstBytes value) {
    if (value.size() > 0x7fffffffU) return Error::out_of_range;
    const auto stream = CreateFileW(attribute_stream(path, name).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, CREATE_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (stream == INVALID_HANDLE_VALUE) return win32::last_error();
    struct Close { HANDLE handle; ~Close() { CloseHandle(handle); } } close{stream};
    DWORD written = 0;
    if (!value.empty() && (!WriteFile(stream, value.data(), static_cast<DWORD>(value.size()), &written, nullptr) || written != value.size()))
        return win32::last_error();
    return {};
}

Status remove_attribute(std::string_view path, std::string_view name) {
    if (!DeleteFileW(attribute_stream(path, name).c_str())) return attribute_error(GetLastError());
    return {};
}

Result<std::vector<std::string>> list_attributes(std::string_view path) {
    std::vector<std::string> names;
    WIN32_FIND_STREAM_DATA data{};
    const auto search = FindFirstStreamW(win32::widen(path).c_str(), FindStreamInfoStandard, &data, 0);
    if (search == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        if (code == ERROR_HANDLE_EOF) return names;   // no streams at all (a directory)
        return attribute_error(code);
    }
    do {
        // ":name:$DATA"; the unnamed data stream is the file itself, not an attribute.
        std::wstring stream = data.cStreamName;
        if (stream.size() < 8 || stream.compare(stream.size() - 6, 6, L":$DATA") != 0) continue;
        auto name = stream.substr(1, stream.size() - 7);
        if (name.empty()) continue;
        std::replace(name.begin(), name.end(), L'\xf03a', L':');
        names.push_back(win32::narrow(name));
    } while (FindNextStreamW(search, &data));
    FindClose(search);
    return names;
}

Status create_symbolic_link(std::string_view link, std::string_view target, bool directory) {
    auto text = win32::widen(target);
    std::replace(text.begin(), text.end(), L'/', L'\\');
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    if (directory) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    if (!CreateSymbolicLinkW(win32::widen(link).c_str(), text.c_str(), flags)) return win32::last_error();
    return {};
}

Status create_junction(std::string_view link, std::string_view absolute_target) {
    // One separator between components (the reparse data names an NT path,
    // which does not collapse doubled separators the way CreateFile does).
    auto absolute = std::filesystem::path(win32::widen(absolute_target)).lexically_normal().make_preferred().wstring();
    if (absolute.starts_with(L"\\\\?\\")) absolute.erase(0, 4);
    if (absolute.size() < 3 || absolute[1] != L':') return Error::invalid_argument;
    const auto substitute = L"\\??\\" + absolute;
    const auto substitute_bytes = substitute.size() * sizeof(wchar_t);
    const auto print_bytes = absolute.size() * sizeof(wchar_t);
    const auto path_bytes = substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t);
    const auto data_length = 8 + path_bytes;   // four USHORT offsets/lengths, then the names
    if (data_length > MAXIMUM_REPARSE_DATA_BUFFER_SIZE - 8) return Error::name_too_long;
    std::vector<unsigned char> buffer(8 + data_length, 0);
    const auto put16 = [&](std::size_t offset, std::size_t value) {
        const auto narrow = static_cast<unsigned short>(value);
        std::memcpy(buffer.data() + offset, &narrow, sizeof(narrow));
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buffer.data(), &tag, sizeof(tag));
    put16(4, data_length);
    put16(8, 0);                                         // SubstituteNameOffset
    put16(10, substitute_bytes);                         // SubstituteNameLength
    put16(12, substitute_bytes + sizeof(wchar_t));       // PrintNameOffset
    put16(14, print_bytes);                              // PrintNameLength
    std::memcpy(buffer.data() + 16, substitute.data(), substitute_bytes);
    std::memcpy(buffer.data() + 16 + substitute_bytes + sizeof(wchar_t), absolute.data(), print_bytes);
    const auto wide_link = win32::widen(link);
    if (!CreateDirectoryW(wide_link.c_str(), nullptr)) return win32::last_error();
    const auto handle = CreateFileW(wide_link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto code = GetLastError();
        RemoveDirectoryW(wide_link.c_str());
        return win32::error(code);
    }
    DWORD returned = 0;
    const auto ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    const auto code = GetLastError();
    CloseHandle(handle);
    if (!ok) {
        RemoveDirectoryW(wide_link.c_str());
        return win32::error(code);
    }
    return {};
}

Status create_hard_link(std::string_view link, std::string_view target) {
    if (!CreateHardLinkW(win32::widen(link).c_str(), win32::widen(target).c_str(), nullptr)) return win32::last_error();
    return {};
}

namespace {
Status remove_tree_wide(const std::wstring& path) {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return {};
    if (attributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), attributes & ~DWORD{FILE_ATTRIBUTE_READONLY});
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        // A link or junction is removed itself, never followed.
        if (!(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::vector<std::wstring> children;
            WIN32_FIND_DATAW entry{};
            const auto search = FindFirstFileW((path + L"\\*").c_str(), &entry);
            if (search != INVALID_HANDLE_VALUE) {
                do {
                    const std::wstring_view name(entry.cFileName);
                    if (name != L"." && name != L"..") children.push_back(path + L"\\" + entry.cFileName);
                } while (FindNextFileW(search, &entry));
                FindClose(search);
            }
            for (const auto& child : children)
                if (const auto removed = remove_tree_wide(child); !removed) return removed;
        }
        if (!RemoveDirectoryW(path.c_str())) return win32::last_error();
    } else if (!DeleteFileW(path.c_str())) {
        return win32::last_error();
    }
    return {};
}
} // namespace

Status remove_tree(std::string_view path) {
    auto wide = win32::widen(path);
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    while (wide.size() > 3 && wide.back() == L'\\') wide.pop_back();
    return remove_tree_wide(wide);
}

Error error(const std::error_code& code) noexcept {
    if (!code) return Error::none;
    if (code.category() == std::system_category()) return win32::error(static_cast<DWORD>(code.value()));
    switch (code.default_error_condition().value()) {
    case EPERM: return Error::permission;
    case ENOENT: return Error::not_found;
    case EACCES: return Error::access_denied;
    case EBADF: return Error::bad_handle;
    case ENOMEM: return Error::no_memory;
    case EEXIST: return Error::exists;
    case ENOTDIR: return Error::not_directory;
    case EISDIR: return Error::is_directory;
    case EINVAL: return Error::invalid_argument;
    case EMFILE: return Error::too_many_open;
    case ENOSPC: return Error::no_space;
    case ERANGE: return Error::out_of_range;
    case ENOTEMPTY: return Error::not_empty;
    case EPIPE: return Error::broken_pipe;
    case ENAMETOOLONG: return Error::name_too_long;
    case EXDEV: return Error::cross_device;
    default: return Error::other;
    }
}

} // namespace alchemy::host::files
