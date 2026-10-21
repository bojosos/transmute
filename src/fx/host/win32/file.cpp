// win32 backend of fx/host/file.hpp.

#include "fx/host/file.hpp"

#include <windows.h>

#include <algorithm>
#include <cwchar>

namespace transmute::fx::host::file {

static_assert(kRead == FILE_SHARE_READ && kWrite == FILE_SHARE_WRITE && kDelete == FILE_SHARE_DELETE);

namespace {

HANDLE native(File file) { return file ? static_cast<HANDLE>(file) : INVALID_HANDLE_VALUE; }

std::uint64_t value(const FILETIME& t) { return (std::uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime; }

} // namespace

File open(const std::wstring& path, unsigned access, unsigned share, Create create, Flags flags) noexcept {
    const DWORD desired = ((access & kRead) ? GENERIC_READ : 0) | ((access & kWrite) ? GENERIC_WRITE : 0) |
                          ((access & kDelete) ? DELETE : 0);
    DWORD disposition = OPEN_EXISTING;
    switch (create) {
    case Create::open_existing: disposition = OPEN_EXISTING; break;
    case Create::open_always: disposition = OPEN_ALWAYS; break;
    case Create::create_new: disposition = CREATE_NEW; break;
    case Create::create_always: disposition = CREATE_ALWAYS; break;
    }
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    switch (flags) {
    case Flags::normal: attributes = FILE_ATTRIBUTE_NORMAL; break;
    case Flags::sequential: attributes = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN; break;
    case Flags::lock_file:
        attributes = FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE;
        break;
    }
    HANDLE handle = CreateFileW(path.c_str(), desired, share, nullptr, disposition, attributes, nullptr);
    return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
}

void close(File file) noexcept {
    if (file) CloseHandle(file);
}

bool write_all(File file, const void* data, std::size_t size) noexcept {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (size > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 64u << 20));
        DWORD written = 0;
        if (!WriteFile(native(file), p, chunk, &written, nullptr) || written == 0) return false;
        p += written;
        size -= written;
    }
    return true;
}

bool read_all(File file, void* data, std::size_t size) noexcept {
    auto* p = static_cast<std::uint8_t*>(data);
    for (std::size_t done = 0; done < size;) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, std::size_t{8} << 20));
        DWORD got = 0;
        if (!ReadFile(native(file), p + done, chunk, &got, nullptr) || got == 0) return false;
        done += got;
    }
    return true;
}

bool seek(File file, std::uint64_t offset) noexcept {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    return SetFilePointerEx(native(file), position, nullptr, FILE_BEGIN) != FALSE;
}

bool seek_end(File file) noexcept {
    const LARGE_INTEGER zero{};
    return SetFilePointerEx(native(file), zero, nullptr, FILE_END) != FALSE;
}

bool size(File file, std::uint64_t& bytes) noexcept {
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(native(file), &length) || length.QuadPart < 0) return false;
    bytes = static_cast<std::uint64_t>(length.QuadPart);
    return true;
}

const std::uint8_t* map_read_only(File file, int* stage) noexcept {
    HANDLE mapping = CreateFileMappingW(native(file), nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        if (stage) *stage = 1;
        return nullptr;
    }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (!view && stage) *stage = 2;
    return static_cast<const std::uint8_t*>(view);
}

void unmap(const void* view) noexcept {
    if (view) UnmapViewOfFile(view);
}

void mark_deleted(File file) noexcept {
    // POSIX semantics: removes the name even while other processes map the
    // file (the classic disposition is denied for mapped files).
    FILE_DISPOSITION_INFO_EX ex{};
    ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
               FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (!SetFileInformationByHandle(native(file), FileDispositionInfoEx, &ex, sizeof(ex))) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        SetFileInformationByHandle(native(file), FileDispositionInfo, &disposition, sizeof(disposition));
    }
}

bool remove(const std::wstring& path) noexcept { return DeleteFileW(path.c_str()) != FALSE; }

bool rename_replace(const std::wstring& from, const std::wstring& to) noexcept {
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool replace_atomically(const std::wstring& temp, const std::wstring& target) noexcept {
    wchar_t full[MAX_PATH * 2];
    const DWORD length = GetFullPathNameW(target.c_str(), MAX_PATH * 2, full, nullptr);
    if (length > 0 && length < MAX_PATH * 2) {
        HANDLE file = CreateFileW(temp.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            alignas(FILE_RENAME_INFO) unsigned char buffer[sizeof(FILE_RENAME_INFO) + MAX_PATH * 2 * sizeof(wchar_t)]{};
            auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer);
            info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
            info->RootDirectory = nullptr;
            info->FileNameLength = length * sizeof(wchar_t);
            std::wmemcpy(info->FileName, full, length);
            const BOOL renamed = SetFileInformationByHandle(file, FileRenameInfoEx, info,
                                                            static_cast<DWORD>(sizeof(FILE_RENAME_INFO) + length * sizeof(wchar_t)));
            CloseHandle(file);
            if (renamed) return true;
        }
    }
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        const DWORD error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION) return false;
        Sleep(10);
    }
    return false;
}

Directory create_directory(const std::wstring& path) noexcept {
    if (CreateDirectoryW(path.c_str(), nullptr)) return Directory::created;
    switch (GetLastError()) {
    case ERROR_ALREADY_EXISTS: return Directory::exists;
    case ERROR_PATH_NOT_FOUND: return Directory::parent_missing;
    default: return Directory::failed;
    }
}

std::vector<Entry> list(const std::wstring& pattern, bool large) {
    std::vector<Entry> entries;
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                   large ? FIND_FIRST_EX_LARGE_FETCH : 0);
    if (find == INVALID_HANDLE_VALUE) return entries;
    do {
        entries.push_back({fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                           (std::uint64_t{fd.nFileSizeHigh} << 32) | fd.nFileSizeLow, value(fd.ftLastWriteTime)});
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return entries;
}

bool attributes(const std::wstring& path, std::uint64_t* bytes, std::uint64_t* write_time) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    if (bytes) *bytes = (std::uint64_t{data.nFileSizeHigh} << 32) | data.nFileSizeLow;
    if (write_time) *write_time = value(data.ftLastWriteTime);
    return true;
}

std::wstring full_path(const std::wstring& path) {
    wchar_t full[MAX_PATH * 4];
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(MAX_PATH * 4), full, nullptr);
    return n > 0 && n < MAX_PATH * 4 ? std::wstring(full, n) : std::wstring();
}

std::wstring temp_directory() {
    wchar_t temp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, temp);
    return std::wstring(temp, n);
}

} // namespace transmute::fx::host::file
