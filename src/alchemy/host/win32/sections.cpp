#include "alchemy/host/sections.hpp"

#include "alchemy/host/win32/win32.hpp"

namespace alchemy::host::sections {

Result<Handle> create_section(std::uint64_t size, bool inheritable) {
    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, inheritable ? &inherit : nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                            static_cast<DWORD>(size), nullptr);
    if (!section) return win32::last_error();
    return win32::wrap(section);
}

bool same(Handle first, Handle second) noexcept { return CompareObjectHandles(win32::native(first), win32::native(second)) != 0; }

Result<std::uint64_t> duplicate_into(Handle section, std::uint32_t target_pid, bool writable) {
    const auto target = OpenProcess(PROCESS_DUP_HANDLE, FALSE, target_pid);
    if (!target) return GetLastError() == ERROR_INVALID_PARAMETER ? Error::not_found : win32::last_error();
    HANDLE remote{};
    const DWORD access = FILE_MAP_READ | (writable ? FILE_MAP_WRITE : 0);
    const bool duplicated = DuplicateHandle(GetCurrentProcess(), win32::native(section), target, &remote, access, FALSE, 0) != 0;
    const auto code = GetLastError();
    CloseHandle(target);
    if (!duplicated) return win32::error(code);
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(remote));
}

Status flush(std::uintptr_t address, std::size_t size) {
    if (!FlushViewOfFile(reinterpret_cast<const void*>(address), size)) return win32::last_error();
    return {};
}

Result<Handle> create_file_section(Handle file, bool writable) {
    const auto section = CreateFileMappingW(win32::native(file), nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    if (!section) return win32::last_error();
    return win32::wrap(section);
}

Result<std::uintptr_t> map(Handle section, std::uint64_t offset, std::size_t size, bool writable) {
    const auto view = MapViewOfFile(win32::native(section), writable ? FILE_MAP_WRITE : FILE_MAP_READ,
                                    static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset), size);
    if (!view) return win32::last_error();
    return reinterpret_cast<std::uintptr_t>(view);
}

Status unmap(std::uintptr_t address) {
    if (!UnmapViewOfFile(reinterpret_cast<const void*>(address))) return win32::last_error();
    return {};
}

Result<Named> open_named(std::string_view name, std::uint64_t size) {
    const auto wide = win32::local_name(name);
    const auto section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(size >> 32),
                                            static_cast<DWORD>(size), wide.c_str());
    if (!section) return win32::last_error();
    return Named{win32::wrap(section), GetLastError() != ERROR_ALREADY_EXISTS};
}

Result<Handle> open_existing(std::string_view name, bool writable) {
    const auto section = OpenFileMappingW(writable ? FILE_MAP_READ | FILE_MAP_WRITE : FILE_MAP_READ, FALSE, win32::local_name(name).c_str());
    if (!section) return win32::last_error();
    return win32::wrap(section);
}

} // namespace alchemy::host::sections
