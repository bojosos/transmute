#define _CRT_RAND_S
#include "transmute/win32_bridge.hpp"
#include "transmute/guest_process.hpp"
#include "crt_scan.hpp"

#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <winver.h>
#include <bcrypt.h>
#include <Shlwapi.h>
#include <pathcch.h>
#include <shellscalingapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <ShlDisp.h>
#include <ole2.h>
#include <corecrt_wctype.h>
#include <corecrt_math.h>
#include <CommCtrl.h>
#include <gl/GL.h>
#include <mmdeviceapi.h>
#include <dwmapi.h>
#include <imm.h>
#include <directmanipulation.h>
#include "win32_gl_scalar.hpp"
#include "win32_gl_memory.hpp"
#include "win32_gl_buffers.hpp"
#include "win32_gl_reflection.hpp"
#include "win32_gl_sync.hpp"
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/timeb.h>
#include <time.h>

#include <algorithm>
#include <condition_variable>
#include <system_error>
#include <array>
#include <bit>
#include <cerrno>
#include <cctype>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <locale.h>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <exception>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fenv.h>
#include <fstream>
#include <float.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace transmute {
namespace {

void __cdecl return_from_invalid_host_crt_parameter(
    const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {}

struct ScopedHostCrtParameterGuard {
    _invalid_parameter_handler previous{
        _set_thread_local_invalid_parameter_handler(
            return_from_invalid_host_crt_parameter)};
    ~ScopedHostCrtParameterGuard() {
        _set_thread_local_invalid_parameter_handler(previous);
    }
};

constexpr std::uint64_t kernel32_handle = 0x0000600100000000ULL;
constexpr std::uint64_t kernelbase_handle = 0x0000600100010000ULL;
constexpr std::uint64_t opengl32_handle = 0x0000600100020000ULL;
constexpr std::uint64_t stdio_base = 0x0000600300000000ULL;
constexpr std::uint64_t stdio_files_base = 0x0000600300010000ULL;
constexpr std::uint64_t heap_base = 0x0000600400000000ULL;
constexpr std::uint64_t locale_base = 0x0000600500000000ULL;
constexpr std::uint64_t ctype_base = 0x0000600600000000ULL;
constexpr std::uint64_t locale_names_base = 0x0000600700000000ULL;
constexpr std::uint64_t local_heap_base = 0x0000600800000000ULL;
constexpr std::uint64_t locale_object_base = 0x0000600900000000ULL;
constexpr std::uint64_t host_handle_base = 0x0000600a00000000ULL;
constexpr std::uint32_t child_bootstrap_magic = 0x544d4348U;
constexpr std::uint32_t child_bootstrap_version = 1;
constexpr std::uint32_t maximum_child_bootstrap_bytes = 1024U * 1024U;
struct ChildBootstrapHeader {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t total_bytes{};
    std::uint32_t command_units{};
    std::uint32_t environment_units{};
    std::uint32_t handle_count{};
    std::uint32_t reserved0{};
    std::uint32_t reserved1{};
    std::array<std::uint64_t, 3> standard_handle_tokens{};
};
static_assert(sizeof(ChildBootstrapHeader) == 56);
struct ChildBootstrapHandle {
    std::uint64_t guest{};
    std::uint64_t native{};
};
static_assert(sizeof(ChildBootstrapHandle) == 16);
constexpr std::uint64_t process_heap_handle = 0x0000600b00000000ULL;
constexpr std::uint64_t startup_strings_base = 0x0000600c00000000ULL;
constexpr std::uint64_t command_line_base = 0x0000600d00000000ULL;
constexpr std::uint64_t environment_base = 0x0000600e00000000ULL;
constexpr std::uint64_t tls_array_base = 0x0000600f00000000ULL;
constexpr std::uint64_t tls_data_base = 0x0000601000000000ULL;
constexpr std::uint64_t crt_time_base = 0x0000601100000000ULL;
constexpr std::uint64_t lconv_base = 0x0000601200000000ULL;
constexpr std::uint64_t argv_base = 0x0000601300000000ULL;
constexpr std::uint64_t crt_errors_base = 0x0000601400000000ULL;
constexpr std::uint64_t com_task_base = 0x0000602100000000ULL;
constexpr std::uint64_t image_handle_base = 0x0000602200000000ULL;
constexpr std::uint64_t gdi_handle_base = 0x0000602300000000ULL;
constexpr std::uint64_t window_handle_base = 0x0000602400000000ULL;
constexpr std::uint64_t window_scratch_base = 0x0000602500000000ULL;
constexpr std::uint64_t shell_folder_base = 0x0000602600000000ULL;
constexpr std::uint64_t shell_vtable_base = 0x0000602700000000ULL;
constexpr std::uint64_t guest_bstr_base = 0x0000602800000000ULL;
constexpr std::uint64_t shell_automation_base = 0x0000602900000000ULL;
constexpr std::uint64_t shell_automation_vtable_base = 0x0000602a00000000ULL;
constexpr std::uint64_t cng_handle_base = 0x0000602b00000000ULL;
constexpr std::uint64_t dll_cookie_base = 0x0000602c00000000ULL;
constexpr std::uint64_t socket_handle_base = 0x0000602d00000000ULL;
constexpr std::uint64_t registry_handle_base = 0x0000602e00000000ULL;
constexpr std::uint64_t resolver_base = 0x0000602e00000000ULL;
constexpr unsigned shell_dispatch_kind = 0;
constexpr unsigned shell_folder_kind = 1;
constexpr unsigned shell_items_kind = 2;
constexpr unsigned shell_item_kind = 3;
constexpr std::uint64_t argv_region_size = 0x000a0000ULL;
constexpr std::uint64_t argv_table_region_size = 0x00040000ULL;
constexpr std::uint64_t virtual_memory_base = 0x0000500000000000ULL;
constexpr std::uint64_t file_view_base = 0x0000520000000000ULL;
constexpr std::uint64_t guest_page_size = 4096;
constexpr std::uint64_t guest_allocation_granularity = 65536;
static_assert(sizeof(WSADATA) == 408);
static_assert(offsetof(WSADATA, lpVendorInfo) == 8);
static_assert(sizeof(fd_set) == 8 + FD_SETSIZE * sizeof(SOCKET));
static_assert(sizeof(ADDRINFOA) == 48);
static_assert(offsetof(ADDRINFOA, ai_addrlen) == 16);
static_assert(offsetof(ADDRINFOA, ai_next) == 40);

// Match the packaged guests' legacy Winsock entry points, including their
// accepted address spellings and thread-local string behavior.
#pragma warning(push)
#pragma warning(disable: 4996)
u_long host_inet_addr(const char* text) { return ::inet_addr(text); }
const char* host_inet_ntoa(IN_ADDR address) { return ::inet_ntoa(address); }
#pragma warning(pop)

// Keep the non-secure CRT entry points' sharing and reopen behavior for their
// matching guest imports. The secure alternatives are separate guest imports.
#pragma warning(push)
#pragma warning(disable: 4996)
FILE* open_plain(const char* path, const char* mode) {
    return std::fopen(path, mode);
}
FILE* open_plain_wide(const wchar_t* path, const wchar_t* mode) {
    return _wfopen(path, mode);
}
FILE* adopt_descriptor(int descriptor, const char* mode) {
    return _fdopen(descriptor, mode);
}
FILE* adopt_descriptor_wide(int descriptor, const wchar_t* mode) {
    return _wfdopen(descriptor, mode);
}
FILE* reopen_plain(const char* path, const char* mode, FILE* stream) {
    return std::freopen(path, mode, stream);
}
FILE* reopen_plain_wide(const wchar_t* path, const wchar_t* mode,
                        FILE* stream) {
    return _wfreopen(path, mode, stream);
}
int host_system_error_count() {
    return *__sys_nerr();
}
char** host_system_error_list() {
    return __sys_errlist();
}
#pragma warning(pop)

struct GuestThreadRecord {
    HANDLE retained{};
    std::uint64_t guest_handle{};
    std::mutex mutex;
    std::string failure;
    ~GuestThreadRecord() {
        if (retained != nullptr) CloseHandle(retained);
    }
};

struct GuestThreadStart {
    std::function<unsigned()> run;
    std::shared_ptr<GuestThreadRecord> record;
};

struct GuestWindowContext : std::enable_shared_from_this<GuestWindowContext> {
    Win32Bridge* bridge{};
    GuestMemory* memory{};
    std::uint64_t guest_handle{};
    std::uint64_t guest_proc{};
    std::uint64_t guest_instance{};
    std::uint64_t guest_parent{};
    std::uint64_t guest_menu{};
    std::uint64_t guest_parameter{};
    std::uint64_t guest_class_name{};
    std::uint64_t guest_window_name{};
    std::wstring registered_class_name;
    std::uint64_t scratch{};
    HWND host{};
    WNDPROC native_proc{};
    std::uint32_t extra_bytes{};
    LPARAM active_host_lparam{};
    bool ansi{};
    bool scratch_mapped{};
    bool shutting_down{};
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t,
                                std::uint64_t)> invoke;
};

std::mutex native_window_mutex;
std::unordered_map<HWND, std::shared_ptr<GuestWindowContext>> native_windows;

LRESULT CALLBACK guest_window_procedure(HWND window, UINT message,
                                         WPARAM wparam, LPARAM lparam) {
    std::shared_ptr<GuestWindowContext> context;
    {
        std::lock_guard lock(native_window_mutex);
        if (message == WM_NCCREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            context = static_cast<GuestWindowContext*>(creation->lpCreateParams)->shared_from_this();
            native_windows[window] = context;
        } else if (const auto found = native_windows.find(window);
                   found != native_windows.end()) {
            context = found->second;
        }
    }
    const auto result = context == nullptr
        ? ::DefWindowProcW(window, message, wparam, lparam)
        : static_cast<LRESULT>(context->bridge->host_window_message(
            window, message, wparam, lparam, context.get()));
    if (message == WM_NCDESTROY) {
        std::lock_guard lock(native_window_mutex);
        native_windows.erase(window);
    }
    return result;
}

unsigned __stdcall run_guest_thread(void* opaque) noexcept {
    std::unique_ptr<GuestThreadStart> start(static_cast<GuestThreadStart*>(opaque));
    try {
        return start->run();
    } catch (const std::exception& error) {
        std::lock_guard lock(start->record->mutex);
        if (start->record->failure.empty()) start->record->failure = error.what();
    } catch (...) {
        std::lock_guard lock(start->record->mutex);
        if (start->record->failure.empty()) {
            start->record->failure = "unknown guest thread failure";
        }
    }
    return 0xe0000001U;
}

std::optional<std::uint64_t> align_up_page(std::uint64_t value) {
    if (value > UINT64_MAX - (guest_page_size - 1)) return std::nullopt;
    return (value + guest_page_size - 1) & ~(guest_page_size - 1);
}

bool supported_page_protection(DWORD protection) {
    switch (protection) {
    case PAGE_NOACCESS:
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY: return true;
    default: return false;
    }
}

void apply_guest_page_protection(GuestMemory& memory, std::uint64_t page,
                                 DWORD protection) {
    const bool readable = protection == PAGE_READONLY ||
                          protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY ||
                          protection == PAGE_EXECUTE_READ ||
                          protection == PAGE_EXECUTE_READWRITE ||
                          protection == PAGE_EXECUTE_WRITECOPY;
    const bool writable = protection == PAGE_READWRITE ||
                          protection == PAGE_WRITECOPY ||
                          protection == PAGE_EXECUTE_READWRITE ||
                          protection == PAGE_EXECUTE_WRITECOPY;
    const bool executable = protection == PAGE_EXECUTE ||
                            protection == PAGE_EXECUTE_READ ||
                            protection == PAGE_EXECUTE_READWRITE ||
                            protection == PAGE_EXECUTE_WRITECOPY;
    memory.set_memory_access(page, readable, writable, executable);
}

std::wstring lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

struct WindowsEnvironmentLess {
    bool operator()(const std::wstring& left,
                    const std::wstring& right) const {
        const auto order = CompareStringOrdinal(
            left.data(), static_cast<int>(left.size()),
            right.data(), static_cast<int>(right.size()), TRUE);
        return order == CSTR_LESS_THAN;
    }
};

std::wstring environment_wide(std::string_view value) {
    if (value.empty()) return {};
    const auto count = MultiByteToWideChar(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::runtime_error("cannot decode guest environment text");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), result.data(), count) != count) {
        throw std::runtime_error("cannot decode guest environment text");
    }
    return result;
}

std::string environment_ansi(std::wstring_view value) {
    if (value.empty()) return {};
    const auto count = WideCharToMultiByte(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) throw std::runtime_error("cannot encode guest environment text");
    std::string result(static_cast<std::size_t>(count), '\0');
    if (WideCharToMultiByte(CP_ACP, 0, value.data(),
        static_cast<int>(value.size()), result.data(), count,
        nullptr, nullptr) != count) {
        throw std::runtime_error("cannot encode guest environment text");
    }
    return result;
}

std::wstring guest_string(GuestMemory& memory, std::uint64_t address,
                          bool wide) {
    bool terminated = false;
    const auto bytes = memory.read_string(address, wide ? 2 : 1, 32768, &terminated);
    if (!terminated) throw std::runtime_error("guest module name is not terminated");
    std::wstring value;
    value.reserve(bytes.size() / (wide ? 2 : 1));
    for (std::size_t index = 0; index < bytes.size(); index += wide ? 2 : 1) {
        const auto character = static_cast<std::uint16_t>(bytes[index]) |
            (wide ? static_cast<std::uint16_t>(bytes[index + 1]) << 8 : 0);
        value.push_back(static_cast<wchar_t>(character));
    }
    return value;
}

bool guest_range(GuestMemory& memory, std::uint64_t address,
                 std::uint64_t size, bool writable) {
    if (address == 0 || size > UINT64_MAX - address) return false;
    const auto region = memory.query_region(address);
    return region && (writable ? region->writable : region->readable) &&
           address + size <= region->base + region->size;
}

std::optional<std::uint16_t> pe_machine(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    IMAGE_DOS_HEADER dos{};
    file.read(reinterpret_cast<char*>(&dos), sizeof(dos));
    if (!file || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
        dos.e_lfanew > 1024 * 1024) return std::nullopt;
    file.seekg(dos.e_lfanew);
    DWORD signature = 0;
    WORD machine = 0;
    file.read(reinterpret_cast<char*>(&signature), sizeof(signature));
    file.read(reinterpret_cast<char*>(&machine), sizeof(machine));
    if (!file || signature != IMAGE_NT_SIGNATURE) return std::nullopt;
    return machine;
}

// The guest sees the real host Windows version regardless of Transmute's own
// application manifest. Keep all guest version APIs on this single policy.
OSVERSIONINFOEXW guest_os_version() {
    using GetVersionFunction = LONG (WINAPI*)(OSVERSIONINFOW*);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto get_version = module ? reinterpret_cast<GetVersionFunction>(
        GetProcAddress(module, "RtlGetVersion")) : nullptr;
    if (!get_version) throw std::runtime_error("RtlGetVersion is unavailable");
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (get_version(reinterpret_cast<OSVERSIONINFOW*>(&version)) < 0) {
        throw std::runtime_error("RtlGetVersion failed");
    }
    return version;
}

DWORD verify_guest_os_version(OSVERSIONINFOEXW& requested, DWORD type_mask,
                              DWORDLONG condition_mask) {
    using VerifyVersionFunction = LONG (WINAPI*)(OSVERSIONINFOEXW*, ULONG,
                                                 ULONGLONG);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto verify = module ? reinterpret_cast<VerifyVersionFunction>(
        GetProcAddress(module, "RtlVerifyVersionInfo")) : nullptr;
    if (!verify) throw std::runtime_error("RtlVerifyVersionInfo is unavailable");
    const auto status = verify(&requested, type_mask, condition_mask);
    if (status >= 0) return ERROR_SUCCESS;
    if (static_cast<std::uint32_t>(status) == 0xc0000059U) {
        return ERROR_OLD_WIN_VERSION;
    }
    using StatusToErrorFunction = ULONG (WINAPI*)(LONG);
    const auto to_error = reinterpret_cast<StatusToErrorFunction>(
        GetProcAddress(module, "RtlNtStatusToDosError"));
    return to_error ? to_error(status) : ERROR_INVALID_PARAMETER;
}

std::optional<std::vector<std::uint8_t>> guest_pidl(
    GuestMemory& memory, std::uint64_t address) {
    constexpr std::size_t maximum_pidl = 65536;
    std::vector<std::uint8_t> bytes;
    while (bytes.size() + 2 <= maximum_pidl) {
        if (address > UINT64_MAX - bytes.size() ||
            !guest_range(memory, address + bytes.size(), 2, false)) {
            return std::nullopt;
        }
        const auto size = static_cast<std::uint16_t>(
            memory.read8(address + bytes.size()) |
            (memory.read8(address + bytes.size() + 1) << 8));
        if (size == 0) {
            bytes.push_back(0);
            bytes.push_back(0);
            return bytes;
        }
        if (size < 2 || size > maximum_pidl - bytes.size() ||
            !guest_range(memory, address + bytes.size(), size, false)) {
            return std::nullopt;
        }
        for (std::size_t index = 0; index < size; ++index) {
            bytes.push_back(memory.read8(address + bytes.size()));
        }
    }
    return std::nullopt;
}

std::string guest_ansi_string(GuestMemory& memory, std::uint64_t address) {
    bool terminated = false;
    const auto bytes = memory.read_string(address, 1, 32768, &terminated);
    if (!terminated) throw std::runtime_error("guest ANSI string is not terminated");
    return {bytes.begin(), bytes.end()};
}

template <typename Stat, std::size_t Total, std::size_t SizeOffset,
          std::size_t AccessOffset, std::size_t ModifyOffset,
          std::size_t ChangeOffset>
constexpr bool guest_stat_layout_matches() {
    return sizeof(Stat) == Total &&
           offsetof(Stat, st_dev) == 0 &&
           offsetof(Stat, st_ino) == 4 &&
           offsetof(Stat, st_mode) == 6 &&
           offsetof(Stat, st_nlink) == 8 &&
           offsetof(Stat, st_uid) == 10 &&
           offsetof(Stat, st_gid) == 12 &&
           offsetof(Stat, st_rdev) == 16 &&
           offsetof(Stat, st_size) == SizeOffset &&
           offsetof(Stat, st_atime) == AccessOffset &&
           offsetof(Stat, st_mtime) == ModifyOffset &&
           offsetof(Stat, st_ctime) == ChangeOffset;
}

// These offsets were also checked against the Windows SDK's ARM64 UCRT headers.
static_assert(guest_stat_layout_matches<struct _stat64, 56, 24, 32, 40, 48>());
static_assert(guest_stat_layout_matches<struct _stat64i32, 48, 20, 24, 32, 40>());
static_assert(guest_stat_layout_matches<struct _stat32i64, 48, 24, 32, 36, 40>());
static_assert(guest_stat_layout_matches<struct _stat32, 36, 20, 24, 28, 32>());

template <typename Stat>
void write_guest_stat(GuestMemory& memory, std::uint64_t address,
                      const Stat& value) {
    for (std::size_t offset = 0; offset < sizeof(Stat); ++offset) {
        memory.write8(address + offset, 0);
    }
    const auto write16 = [&](std::size_t offset, std::uint16_t number) {
        memory.write8(address + offset, static_cast<std::uint8_t>(number));
        memory.write8(address + offset + 1,
                      static_cast<std::uint8_t>(number >> 8));
    };
    const auto write_number = [&](std::size_t offset, std::uint64_t number,
                                  std::size_t bytes) {
        for (std::size_t index = 0; index < bytes; ++index) {
            memory.write8(address + offset + index,
                          static_cast<std::uint8_t>(number >> (index * 8)));
        }
    };
    memory.write32(address + offsetof(Stat, st_dev),
                   static_cast<std::uint32_t>(value.st_dev));
    write16(offsetof(Stat, st_ino), static_cast<std::uint16_t>(value.st_ino));
    write16(offsetof(Stat, st_mode), value.st_mode);
    write16(offsetof(Stat, st_nlink), static_cast<std::uint16_t>(value.st_nlink));
    write16(offsetof(Stat, st_uid), static_cast<std::uint16_t>(value.st_uid));
    write16(offsetof(Stat, st_gid), static_cast<std::uint16_t>(value.st_gid));
    memory.write32(address + offsetof(Stat, st_rdev),
                   static_cast<std::uint32_t>(value.st_rdev));
    write_number(offsetof(Stat, st_size),
                 static_cast<std::uint64_t>(value.st_size), sizeof(value.st_size));
    write_number(offsetof(Stat, st_atime),
                 static_cast<std::uint64_t>(value.st_atime), sizeof(value.st_atime));
    write_number(offsetof(Stat, st_mtime),
                 static_cast<std::uint64_t>(value.st_mtime), sizeof(value.st_mtime));
    write_number(offsetof(Stat, st_ctime),
                 static_cast<std::uint64_t>(value.st_ctime), sizeof(value.st_ctime));
}

struct MarshaledPrintfArguments {
    std::vector<std::uint64_t> slots;
    std::vector<std::unique_ptr<std::string>> narrow_strings;
    std::vector<std::unique_ptr<std::wstring>> wide_strings;
};

std::string printf_format_syntax(std::wstring_view format) {
    std::string syntax;
    syntax.reserve(format.size());
    for (const auto character : format) {
        syntax.push_back(character <= 0x7f ? static_cast<char>(character) : '?');
    }
    return syntax;
}

MarshaledPrintfArguments marshal_printf_arguments(
    GuestMemory& memory, std::string_view format, std::uint64_t argument_address,
    bool wide_format = false, bool legacy_wide_specifiers = true) {
    static_assert(sizeof(wchar_t) == 2);
    MarshaledPrintfArguments result;
    const auto next_argument = [&] {
        if (argument_address == 0 || result.slots.size() >= 128) {
            throw std::runtime_error("invalid guest printf argument list");
        }
        const auto value = memory.read64(argument_address + result.slots.size() * 8);
        result.slots.push_back(value);
        return value;
    };
    for (std::size_t index = 0; index < format.size(); ++index) {
        if (format[index] != '%') continue;
        ++index;
        if (index == format.size()) {
            throw std::runtime_error("incomplete guest printf format");
        }
        if (format[index] == '%') continue;
        while (index < format.size() &&
               std::string_view("-+ #0'").find(format[index]) !=
                   std::string_view::npos) {
            ++index;
        }
        if (index < format.size() && format[index] == '*') {
            static_cast<void>(next_argument());
            ++index;
        } else {
            while (index < format.size() &&
                   std::isdigit(static_cast<unsigned char>(format[index]))) {
                ++index;
            }
        }
        if (index < format.size() && format[index] == '.') {
            ++index;
            if (index < format.size() && format[index] == '*') {
                static_cast<void>(next_argument());
                ++index;
            } else {
                while (index < format.size() &&
                       std::isdigit(static_cast<unsigned char>(format[index]))) {
                    ++index;
                }
            }
        }
        std::string_view length;
        if (index < format.size() &&
            std::string_view("hljztLwI").find(format[index]) !=
                std::string_view::npos) {
            const auto start = index++;
            if (index < format.size() &&
                ((format[start] == 'h' && format[index] == 'h') ||
                 (format[start] == 'l' && format[index] == 'l'))) {
                ++index;
            } else if (format[start] == 'I' && index + 1 < format.size() &&
                       ((format[index] == '3' && format[index + 1] == '2') ||
                        (format[index] == '6' && format[index + 1] == '4'))) {
                index += 2;
            }
            length = format.substr(start, index - start);
        }
        if (index == format.size()) {
            throw std::runtime_error("incomplete guest printf conversion");
        }
        const auto conversion = format[index];
        if (conversion == 's' || conversion == 'S') {
            const auto address = next_argument();
            if (address == 0) continue;
            const bool wide = length == "l" || length == "w" ||
                (length != "h" && (wide_format
                    ? (legacy_wide_specifiers ? conversion == 's'
                                              : conversion == 'S')
                    : conversion == 'S'));
            if (wide) {
                auto value = std::make_unique<std::wstring>(
                    guest_string(memory, address, true));
                result.slots.back() = reinterpret_cast<std::uintptr_t>(
                    value->c_str());
                result.wide_strings.push_back(std::move(value));
            } else {
                auto value = std::make_unique<std::string>(
                    guest_ansi_string(memory, address));
                result.slots.back() = reinterpret_cast<std::uintptr_t>(
                    value->c_str());
                result.narrow_strings.push_back(std::move(value));
            }
        } else if (std::string_view("diuoxXfFeEgGaAcCp").find(conversion) !=
                   std::string_view::npos) {
            static_cast<void>(next_argument());
        } else {
            throw std::runtime_error("unsupported guest printf conversion");
        }
    }
    if (result.slots.empty()) result.slots.push_back(0);
    return result;
}

std::wstring guest_wide_span(GuestMemory& memory, std::uint64_t address,
                             int count) {
    constexpr int maximum_count = 1024 * 1024;
    if (count > maximum_count || count < -1 || address == 0) {
        throw std::runtime_error("invalid guest Unicode string span");
    }
    const auto limit = count < 0 ? maximum_count : count;
    bool terminated = false;
    auto bytes = count < 0 ? memory.read_string(address, 2, limit, &terminated)
                          : std::vector<std::uint8_t>(static_cast<std::size_t>(limit) * 2);
    if (count >= 0) {
        if (typeid(memory) == typeid(ReferenceEngine)) memory.read_bytes(address, bytes);
        else for (std::size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = memory.read8(address + index);
    }
    if (count < 0 && !terminated) throw std::runtime_error("guest Unicode string is not terminated");
    std::wstring result;
    result.reserve(bytes.size() / 2);
    for (std::size_t index = 0; index < bytes.size(); index += 2)
        result.push_back(static_cast<wchar_t>(bytes[index] | (bytes[index + 1] << 8)));
    return result;
}

std::string guest_ansi_span(GuestMemory& memory, std::uint64_t address,
                            int count) {
    constexpr int maximum_count = 1024 * 1024;
    if (count > maximum_count || count < -1 || address == 0) {
        throw std::runtime_error("invalid guest ANSI string span");
    }
    const auto limit = count < 0 ? maximum_count : count;
    bool terminated = false;
    auto bytes = count < 0 ? memory.read_string(address, 1, limit, &terminated)
                          : std::vector<std::uint8_t>(static_cast<std::size_t>(limit));
    if (count >= 0) {
        if (typeid(memory) == typeid(ReferenceEngine)) memory.read_bytes(address, bytes);
        else for (std::size_t index = 0; index < bytes.size(); ++index)
            bytes[index] = memory.read8(address + index);
    }
    if (count < 0 && !terminated) throw std::runtime_error("guest ANSI string is not terminated");
    return {bytes.begin(), bytes.end()};
}

void copy_from_guest(GuestMemory& memory, std::uint64_t address,
                     std::span<std::uint8_t> bytes) {
    memory.read_bytes(address, bytes);
}

void copy_to_guest(GuestMemory& memory, std::uint64_t address,
                   std::span<const std::uint8_t> bytes) {
    memory.write_bytes(address, bytes);
}

void fill_guest(GuestMemory& memory, std::uint64_t address,
                std::uint8_t value, std::uint64_t count) {
    memory.fill_bytes(address, value, count);
}

void copy_guest(GuestMemory& memory, std::uint64_t destination,
                std::uint64_t source, std::uint64_t count, bool move = false) {
    memory.copy_bytes(destination, source, count, move);
}

template <typename T>
T guest_structure(GuestMemory& memory, std::uint64_t address) {
    T result{};
    auto* bytes = reinterpret_cast<std::uint8_t*>(&result);
    copy_from_guest(memory, address, {bytes, sizeof(T)});
    return result;
}

template <typename T>
void write_guest_structure(GuestMemory& memory, std::uint64_t address,
                           const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    copy_to_guest(memory, address, {bytes, sizeof(T)});
}

std::wstring module_basename(const std::wstring& path) {
    const auto separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? path : path.substr(separator + 1);
}

void mirror_ctype_table(GuestMemory& memory, std::uint64_t base,
                        const unsigned short* table) {
    for (int index = -127; index <= 255; ++index) {
        const auto value = table[index];
        const auto address = base + static_cast<std::uint64_t>(index + 127) * 2;
        memory.write8(address, static_cast<std::uint8_t>(value));
        memory.write8(address + 1, static_cast<std::uint8_t>(value >> 8));
    }
}

void mirror_ctype_table(GuestMemory& memory) {
    mirror_ctype_table(memory, ctype_base, __pctype_func());
}

void mirror_locale_names(GuestMemory& memory) {
    auto* names = ___lc_locale_name_func();
    std::uint64_t next_string = locale_names_base + 64;
    for (int category = LC_MIN; category <= LC_MAX; ++category) {
        const auto* name = names[category];
        if (name == nullptr) {
            memory.write64(locale_names_base + category * 8, 0);
            continue;
        }
        memory.write64(locale_names_base + category * 8, next_string);
        std::size_t length = 0;
        while (name[length] != 0) {
            if (length >= 1024 || next_string + 2 * (length + 1) >
                                      locale_names_base + 4096) {
                throw std::runtime_error("guest locale name array is too long");
            }
            const auto character = static_cast<std::uint16_t>(name[length]);
            memory.write8(next_string + 2 * length,
                          static_cast<std::uint8_t>(character));
            memory.write8(next_string + 2 * length + 1,
                          static_cast<std::uint8_t>(character >> 8));
            ++length;
        }
        memory.write8(next_string + 2 * length, 0);
        memory.write8(next_string + 2 * length + 1, 0);
        next_string += 2 * (length + 1);
    }
}

void mirror_lconv(GuestMemory& memory, std::uint64_t address,
                  const lconv& host, bool unsigned_char_defaults) {
    auto next_string = address + 0x200;
    const auto limit = address + 4096;
    const auto copy_narrow = [&](std::size_t offset, const char* source) {
        if (source == nullptr) {
            memory.write64(address + offset, 0);
            return;
        }
        const auto length = std::strlen(source);
        if (length >= 4096 || next_string > limit - length - 1) {
            throw std::runtime_error("guest lconv strings are too long");
        }
        memory.write64(address + offset, next_string);
        for (std::size_t index = 0; index <= length; ++index) {
            memory.write8(next_string + index,
                          static_cast<std::uint8_t>(source[index]));
        }
        next_string += length + 1;
    };
    const auto copy_wide = [&](std::size_t offset, const wchar_t* source) {
        if (source == nullptr) {
            memory.write64(address + offset, 0);
            return;
        }
        const auto length = std::wcslen(source);
        if (length >= 2048 || next_string > limit - (length + 1) * 2) {
            throw std::runtime_error("guest wide lconv strings are too long");
        }
        memory.write64(address + offset, next_string);
        for (std::size_t index = 0; index <= length; ++index) {
            const auto character = static_cast<std::uint16_t>(source[index]);
            memory.write8(next_string + index * 2,
                          static_cast<std::uint8_t>(character));
            memory.write8(next_string + index * 2 + 1,
                          static_cast<std::uint8_t>(character >> 8));
        }
        next_string += (length + 1) * 2;
    };
    const auto copy_char = [&](std::size_t offset, char source) {
        auto value = static_cast<std::uint8_t>(source);
        if (unsigned_char_defaults && value == CHAR_MAX) value = UCHAR_MAX;
        memory.write8(address + offset, value);
    };
    copy_narrow(offsetof(lconv, decimal_point), host.decimal_point);
    copy_narrow(offsetof(lconv, thousands_sep), host.thousands_sep);
    copy_narrow(offsetof(lconv, grouping), host.grouping);
    copy_narrow(offsetof(lconv, int_curr_symbol), host.int_curr_symbol);
    copy_narrow(offsetof(lconv, currency_symbol), host.currency_symbol);
    copy_narrow(offsetof(lconv, mon_decimal_point), host.mon_decimal_point);
    copy_narrow(offsetof(lconv, mon_thousands_sep), host.mon_thousands_sep);
    copy_narrow(offsetof(lconv, mon_grouping), host.mon_grouping);
    copy_narrow(offsetof(lconv, positive_sign), host.positive_sign);
    copy_narrow(offsetof(lconv, negative_sign), host.negative_sign);
    copy_char(offsetof(lconv, int_frac_digits), host.int_frac_digits);
    copy_char(offsetof(lconv, frac_digits), host.frac_digits);
    copy_char(offsetof(lconv, p_cs_precedes), host.p_cs_precedes);
    copy_char(offsetof(lconv, p_sep_by_space), host.p_sep_by_space);
    copy_char(offsetof(lconv, n_cs_precedes), host.n_cs_precedes);
    copy_char(offsetof(lconv, n_sep_by_space), host.n_sep_by_space);
    copy_char(offsetof(lconv, p_sign_posn), host.p_sign_posn);
    copy_char(offsetof(lconv, n_sign_posn), host.n_sign_posn);
    copy_wide(offsetof(lconv, _W_decimal_point), host._W_decimal_point);
    copy_wide(offsetof(lconv, _W_thousands_sep), host._W_thousands_sep);
    copy_wide(offsetof(lconv, _W_int_curr_symbol), host._W_int_curr_symbol);
    copy_wide(offsetof(lconv, _W_currency_symbol), host._W_currency_symbol);
    copy_wide(offsetof(lconv, _W_mon_decimal_point), host._W_mon_decimal_point);
    copy_wide(offsetof(lconv, _W_mon_thousands_sep), host._W_mon_thousands_sep);
    copy_wide(offsetof(lconv, _W_positive_sign), host._W_positive_sign);
    copy_wide(offsetof(lconv, _W_negative_sign), host._W_negative_sign);
}

bool dispatch_crt_string(const std::string& function, CpuState& state,
                         GuestMemory& memory) {
    constexpr std::uint64_t maximum_count = 1024ULL * 1024ULL * 1024ULL;
    const auto destination = state.x[0];
    const auto source = state.x[1];
    const auto count = state.x[2];
    const auto read_crt_unit = [&memory](std::uint64_t address,
                                         std::uint64_t index, bool wide)
        -> std::uint16_t {
        const auto width = wide ? 2ULL : 1ULL;
        if (index > (UINT64_MAX - address) / width ||
            address + index * width > UINT64_MAX - (width - 1)) {
            throw std::runtime_error("guest string address overflow");
        }
        const auto location = address + index * width;
        return static_cast<std::uint16_t>(
            memory.read8(location) |
            (wide ? static_cast<std::uint16_t>(memory.read8(location + 1)) << 8 : 0));
    };
    const auto write_crt_unit = [&memory](std::uint64_t address,
                                          std::uint64_t index, bool wide,
                                          std::uint16_t value) {
        const auto width = wide ? 2ULL : 1ULL;
        if (index > (UINT64_MAX - address) / width ||
            address + index * width > UINT64_MAX - (width - 1)) {
            throw std::runtime_error("guest string address overflow");
        }
        const auto location = address + index * width;
        memory.write8(location, static_cast<std::uint8_t>(value));
        if (wide) memory.write8(location + 1,
                                static_cast<std::uint8_t>(value >> 8));
    };
    if (function == "strcpy_s" || function == "strncpy_s" ||
        function == "strcat_s" || function == "strncat_s" ||
        function == "wcscpy_s" || function == "wcsncpy_s" ||
        function == "wcscat_s" || function == "wcsncat_s") {
        const bool wide = function.starts_with("wcs");
        const bool append = function.find("cat") != std::string::npos;
        const bool counted = function.find("n") != std::string::npos;
        const auto unit_bytes = wide ? 2ULL : 1ULL;
        const auto capacity = source;
        const auto source_address = count;
        const auto requested = state.x[3];
        const bool truncate = counted && requested == UINT64_MAX;
        if (destination == 0 || capacity == 0 || capacity > maximum_count) {
            state.x[0] = EINVAL;
            return true;
        }
        const auto read_unit = [&memory, unit_bytes](std::uint64_t address,
                                                      std::uint64_t index)
            -> std::uint16_t {
            const auto byte = address + index * unit_bytes;
            return static_cast<std::uint16_t>(memory.read8(byte)) |
                   (unit_bytes == 2
                        ? static_cast<std::uint16_t>(memory.read8(byte + 1)) << 8
                        : 0);
        };
        const auto write_unit = [&memory, unit_bytes](std::uint64_t address,
                                                       std::uint64_t index,
                                                       std::uint16_t value) {
            const auto byte = address + index * unit_bytes;
            memory.write8(byte, static_cast<std::uint8_t>(value));
            if (unit_bytes == 2) {
                memory.write8(byte + 1, static_cast<std::uint8_t>(value >> 8));
            }
        };
        if (source_address == 0) {
            write_unit(destination, 0, 0);
            state.x[0] = EINVAL;
            return true;
        }
        std::uint64_t prefix = 0;
        if (append) {
            while (prefix < capacity && read_unit(destination, prefix) != 0) {
                ++prefix;
            }
            if (prefix == capacity) {
                write_unit(destination, 0, 0);
                state.x[0] = ERANGE;
                return true;
            }
        }
        const auto available = capacity - prefix - 1;
        const auto source_limit = counted && !truncate
            ? std::min(requested, maximum_count) : maximum_count;
        std::uint64_t copied = 0;
        bool terminated = false;
        while (copied < available && copied < source_limit) {
            const auto character = read_unit(source_address, copied);
            if (character == 0) {
                terminated = true;
                break;
            }
            write_unit(destination, prefix + copied, character);
            ++copied;
        }
        if (!terminated && copied == available && copied < source_limit) {
            terminated = read_unit(source_address, copied) == 0;
        }
        if (!terminated && copied < source_limit) {
            if (!truncate) {
                write_unit(destination, 0, 0);
                state.x[0] = ERANGE;
                return true;
            }
            state.x[0] = STRUNCATE;
        } else {
            state.x[0] = 0;
        }
        write_unit(destination, prefix + copied, 0);
        return true;
    }
    if (function == "wcslen" || function == "wcsnlen") {
        const auto limit = function == "wcslen" ? maximum_count :
                           std::min(source, maximum_count);
        bool terminated = false;
        const auto length = memory.string_length(destination, 2,
            static_cast<std::size_t>(limit), &terminated);
        if (function == "wcslen" && !terminated) {
            throw std::runtime_error("guest wide string is not terminated");
        }
        state.x[0] = length;
        return true;
    }
    if (function == "strlen" || function == "strnlen" ||
        function == "_strncnt" || function == "__strncnt") {
        const auto limit = function == "strlen" ? maximum_count :
                           std::min(source, maximum_count);
        bool terminated = false;
        const auto length = memory.string_length(destination, 1,
            static_cast<std::size_t>(limit), &terminated);
        if (function == "strlen" && !terminated) {
            throw std::runtime_error("guest string is not terminated");
        }
        state.x[0] = length;
        return true;
    }
    if (function == "strcpy" || function == "strncpy" ||
        function == "wcscpy" || function == "wcsncpy") {
        const bool wide = function.starts_with("wcs");
        const bool counted = function.ends_with("ncpy");
        const auto limit = counted ? count : maximum_count;
        if (limit > maximum_count) throw std::runtime_error("guest string copy is too large");
        bool terminated = false;
        for (std::uint64_t index = 0; index < limit; ++index) {
            const auto character = terminated ? std::uint16_t{0}
                                              : read_crt_unit(source, index, wide);
            write_crt_unit(destination, index, wide, character);
            if (character == 0) {
                terminated = true;
                if (!counted) break;
            }
        }
        if (!counted && !terminated) {
            throw std::runtime_error("guest string is not terminated");
        }
        return true;
    }
    if (function == "strcat" || function == "strncat" ||
        function == "wcscat" || function == "wcsncat") {
        const bool wide = function.starts_with("wcs");
        const bool counted = function.ends_with("ncat");
        if (counted && count > maximum_count) {
            throw std::runtime_error("guest string append is too large");
        }
        std::uint64_t length = 0;
        while (length < maximum_count &&
               read_crt_unit(destination, length, wide) != 0) {
            ++length;
        }
        if (length == maximum_count) throw std::runtime_error("guest string is not terminated");
        std::uint64_t copied = 0;
        const auto limit = counted ? count : maximum_count;
        while (copied < limit) {
            const auto character = read_crt_unit(source, copied, wide);
            if (character == 0) break;
            write_crt_unit(destination, length + copied, wide, character);
            ++copied;
        }
        if (!counted && copied == limit) {
            throw std::runtime_error("guest string is not terminated");
        }
        write_crt_unit(destination, length + copied, wide, 0);
        return true;
    }
    if (function == "memcmp" || function == "strcmp" ||
        function == "strncmp" || function == "wcscmp" ||
        function == "wcsncmp") {
        const bool wide = function.starts_with("wcs");
        const bool unbounded = function == "strcmp" || function == "wcscmp";
        const auto limit = unbounded ? maximum_count : count;
        if (limit > maximum_count) throw std::runtime_error("guest comparison is too large");
        for (std::uint64_t index = 0; index < limit; ++index) {
            const auto lhs = read_crt_unit(destination, index, wide);
            const auto rhs = read_crt_unit(source, index, wide);
            if (lhs != rhs) {
                state.x[0] = static_cast<std::uint32_t>(
                    static_cast<std::int32_t>(lhs) - rhs);
                return true;
            }
            if (function != "memcmp" && lhs == 0) {
                state.x[0] = 0;
                return true;
            }
        }
        if (unbounded) throw std::runtime_error("guest string is not terminated");
        state.x[0] = 0;
        return true;
    }
    if (function == "memcpy" || function == "memmove" ||
        function == "memset") {
        if (count > maximum_count) throw std::runtime_error("guest memory operation is too large");
        if (function == "memset") {
            fill_guest(memory, destination, static_cast<std::uint8_t>(source), count);
        } else {
            copy_guest(memory, destination, source, count, function == "memmove");
        }
        return true;
    }
    if (function == "strchr" || function == "strrchr") {
        std::uint64_t found = 0;
        const auto target = static_cast<std::uint8_t>(source);
        for (std::uint64_t index = 0; index < maximum_count; ++index) {
            const auto character = memory.read8(destination + index);
            if (character == target) {
                found = destination + index;
                if (function == "strchr") break;
            }
            if (character == 0) break;
        }
        state.x[0] = found;
        return true;
    }
    if (function == "strspn" || function == "strcspn" ||
        function == "strpbrk") {
        const auto characters = guest_ansi_string(memory, source);
        std::uint64_t index = 0;
        for (; index < maximum_count; ++index) {
            const auto character = memory.read8(destination + index);
            if (character == 0) break;
            const bool member = characters.find(static_cast<char>(character)) !=
                                std::string::npos;
            if ((function == "strspn" && !member) ||
                (function != "strspn" && member)) break;
        }
        if (index == maximum_count) {
            throw std::runtime_error("guest string is not terminated");
        }
        state.x[0] = function == "strpbrk"
            ? (memory.read8(destination + index) == 0 ? 0 : destination + index)
            : index;
        return true;
    }
    if (function == "tolower" || function == "toupper" ||
        function == "isdigit" || function == "isalnum" ||
        function == "isalpha" || function == "isxdigit" ||
        function == "isspace" || function == "isupper" ||
        function == "islower" || function == "isprint" ||
        function == "ispunct") {
        const auto value = static_cast<std::uint32_t>(destination);
        if (value == static_cast<std::uint32_t>(EOF)) {
            state.x[0] = function == "tolower" || function == "toupper"
                ? value : 0;
            return true;
        }
        if (value > 255) {
            throw std::runtime_error("invalid guest CRT character");
        }
        const auto character = static_cast<unsigned char>(value);
        const auto result = function == "tolower" ? std::tolower(character) :
                            function == "toupper" ? std::toupper(character) :
                            function == "isdigit" ? std::isdigit(character) :
                            function == "isalnum" ? std::isalnum(character) :
                            function == "isalpha" ? std::isalpha(character) :
                            function == "isxdigit" ? std::isxdigit(character) :
                            function == "isspace" ? std::isspace(character) :
                            function == "isupper" ? std::isupper(character) :
                            function == "islower" ? std::islower(character) :
                            function == "isprint" ? std::isprint(character) :
                                                    std::ispunct(character);
        state.x[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    if (function == "towlower" || function == "towupper" ||
        function == "iswalnum" || function == "iswalpha" ||
        function == "iswdigit" || function == "iswspace" ||
        function == "iswupper" || function == "iswlower" ||
        function == "iswprint" || function == "iswpunct" ||
        function == "iswxdigit") {
        const auto value = static_cast<std::uint32_t>(destination);
        const bool transform = function == "towlower" || function == "towupper";
        if (value == static_cast<std::uint32_t>(WEOF)) {
            state.x[0] = transform ? value : 0;
            return true;
        }
        if (value > UINT16_MAX) {
            throw std::runtime_error("invalid guest CRT wide character");
        }
        const auto character = static_cast<wint_t>(value);
        const auto result = function == "towlower" ? std::towlower(character) :
                            function == "towupper" ? std::towupper(character) :
                            function == "iswalnum" ? std::iswalnum(character) :
                            function == "iswalpha" ? std::iswalpha(character) :
                            function == "iswdigit" ? std::iswdigit(character) :
                            function == "iswspace" ? std::iswspace(character) :
                            function == "iswupper" ? std::iswupper(character) :
                            function == "iswlower" ? std::iswlower(character) :
                            function == "iswprint" ? std::iswprint(character) :
                            function == "iswxdigit" ? std::iswxdigit(character) :
                                                       std::iswpunct(character);
        state.x[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    return false;
}

bool dispatch_crt_math(const std::string& function, CpuState& state,
                       GuestMemory& memory) {
    static_assert(sizeof(long double) == sizeof(double));
    const auto d0 = std::bit_cast<double>(state.v[0][0]);
    const auto d1 = std::bit_cast<double>(state.v[1][0]);
    const auto f0 = std::bit_cast<float>(
        static_cast<std::uint32_t>(state.v[0][0]));
    const auto f1 = std::bit_cast<float>(
        static_cast<std::uint32_t>(state.v[1][0]));
    const auto return_double = [&state](double value) {
        state.v[0] = {std::bit_cast<std::uint64_t>(value), 0};
    };
    const auto return_float = [&state](float value) {
        state.v[0] = {std::bit_cast<std::uint32_t>(value), 0};
    };
    if (function == "_dclass" || function == "_ldclass" ||
        function == "_fdclass" || function == "_dsign" ||
        function == "_ldsign" || function == "_fdsign" ||
        function == "_fpclass" || function == "_fpclassf") {
        if (function == "_dclass") {
            state.x[0] = static_cast<std::uint16_t>(::_dclass(d0));
        } else if (function == "_ldclass") {
            state.x[0] = static_cast<std::uint16_t>(::_ldclass(d0));
        } else if (function == "_fdclass") {
            state.x[0] = static_cast<std::uint16_t>(::_fdclass(f0));
        } else if (function == "_dsign") {
            state.x[0] = static_cast<std::uint32_t>(::_dsign(d0));
        } else if (function == "_ldsign") {
            state.x[0] = static_cast<std::uint32_t>(::_ldsign(d0));
        } else if (function == "_fdsign") {
            state.x[0] = static_cast<std::uint32_t>(::_fdsign(f0));
        } else if (function == "_fpclass") {
            state.x[0] = static_cast<std::uint32_t>(::_fpclass(d0));
        } else {
            state.x[0] = static_cast<std::uint32_t>(::_fpclassf(f0));
        }
        return true;
    }
    if (function == "_dtest" || function == "_ldtest" ||
        function == "_fdtest") {
        if (function == "_fdtest") {
            const auto value = std::bit_cast<float>(memory.read32(state.x[0]));
            float host_value = value;
            state.x[0] = static_cast<std::uint16_t>(::_fdtest(&host_value));
        } else {
            const auto value = std::bit_cast<double>(memory.read64(state.x[0]));
            double host_value = value;
            long double host_long_value = value;
            state.x[0] = static_cast<std::uint16_t>(function == "_dtest"
                ? ::_dtest(&host_value) : ::_ldtest(&host_long_value));
        }
        return true;
    }
    if (function == "acos") return_double(std::acos(d0));
    else if (function == "acosh") return_double(std::acosh(d0));
    else if (function == "asin") return_double(std::asin(d0));
    else if (function == "asinh") return_double(std::asinh(d0));
    else if (function == "atan") return_double(std::atan(d0));
    else if (function == "atanh") return_double(std::atanh(d0));
    else if (function == "cbrt") return_double(std::cbrt(d0));
    else if (function == "ceil") return_double(std::ceil(d0));
    else if (function == "cos") return_double(std::cos(d0));
    else if (function == "cosh") return_double(std::cosh(d0));
    else if (function == "erf") return_double(std::erf(d0));
    else if (function == "erfc") return_double(std::erfc(d0));
    else if (function == "exp") return_double(std::exp(d0));
    else if (function == "exp2") return_double(std::exp2(d0));
    else if (function == "expm1") return_double(std::expm1(d0));
    else if (function == "fabs") return_double(std::fabs(d0));
    else if (function == "floor") return_double(std::floor(d0));
    else if (function == "log") return_double(std::log(d0));
    else if (function == "log10") return_double(std::log10(d0));
    else if (function == "log1p") return_double(std::log1p(d0));
    else if (function == "log2") return_double(std::log2(d0));
    else if (function == "round") return_double(std::round(d0));
    else if (function == "sin") return_double(std::sin(d0));
    else if (function == "sinh") return_double(std::sinh(d0));
    else if (function == "sqrt") return_double(std::sqrt(d0));
    else if (function == "tan") return_double(std::tan(d0));
    else if (function == "tanh") return_double(std::tanh(d0));
    else if (function == "trunc") return_double(std::trunc(d0));
    else if (function == "acosf") return_float(std::acos(f0));
    else if (function == "asinf") return_float(std::asin(f0));
    else if (function == "atanf") return_float(std::atan(f0));
    else if (function == "cbrtf") return_float(std::cbrt(f0));
    else if (function == "cosf") return_float(std::cos(f0));
    else if (function == "coshf") return_float(std::cosh(f0));
    else if (function == "expf") return_float(std::exp(f0));
    else if (function == "exp2f") return_float(std::exp2(f0));
    else if (function == "logf") return_float(std::log(f0));
    else if (function == "log10f") return_float(std::log10(f0));
    else if (function == "log2f") return_float(std::log2(f0));
    else if (function == "roundf") return_float(std::round(f0));
    else if (function == "sinf") return_float(std::sin(f0));
    else if (function == "sinhf") return_float(std::sinh(f0));
    else if (function == "sqrtf") return_float(std::sqrt(f0));
    else if (function == "tanf") return_float(std::tan(f0));
    else if (function == "tanhf") return_float(std::tanh(f0));
    else if (function == "atan2") return_double(std::atan2(d0, d1));
    else if (function == "copysign") return_double(std::copysign(d0, d1));
    else if (function == "fmax") return_double(std::fmax(d0, d1));
    else if (function == "fmod") return_double(std::fmod(d0, d1));
    else if (function == "hypot") return_double(std::hypot(d0, d1));
    else if (function == "nextafter") return_double(std::nextafter(d0, d1));
    else if (function == "pow") return_double(std::pow(d0, d1));
    else if (function == "atan2f") return_float(std::atan2(f0, f1));
    else if (function == "copysignf") return_float(std::copysign(f0, f1));
    else if (function == "fmodf") return_float(std::fmod(f0, f1));
    else if (function == "_hypotf") return_float(std::hypot(f0, f1));
    else if (function == "nextafterf") return_float(std::nextafter(f0, f1));
    else if (function == "nexttowardf") return_float(std::nexttoward(f0, d1));
    else if (function == "powf") return_float(std::pow(f0, f1));
    else if (function == "frexp") {
        int exponent = 0;
        return_double(std::frexp(d0, &exponent));
        memory.write32(state.x[0], static_cast<std::uint32_t>(exponent));
    } else if (function == "modf") {
        double integer = 0;
        return_double(std::modf(d0, &integer));
        memory.write64(state.x[0], std::bit_cast<std::uint64_t>(integer));
    } else if (function == "modff") {
        float integer = 0;
        return_float(std::modf(f0, &integer));
        memory.write32(state.x[0], std::bit_cast<std::uint32_t>(integer));
    } else if (function == "ldexp") {
        return_double(std::ldexp(d0, static_cast<int>(state.x[0])));
    } else if (function == "scalbn") {
        return_double(std::scalbn(d0, static_cast<int>(state.x[0])));
    } else if (function == "remquo") {
        int quotient = 0;
        return_double(std::remquo(d0, d1, &quotient));
        memory.write32(state.x[0], static_cast<std::uint32_t>(quotient));
    } else if (function == "ilogb") {
        state.x[0] = static_cast<std::uint32_t>(std::ilogb(d0));
    } else if (function == "lrint") {
        state.x[0] = static_cast<std::uint32_t>(std::lrint(d0));
    } else if (function == "lround") {
        state.x[0] = static_cast<std::uint32_t>(std::lround(d0));
    } else if (function == "lroundf") {
        state.x[0] = static_cast<std::uint32_t>(std::lround(f0));
    } else if (function == "_finite") {
        state.x[0] = std::isfinite(d0);
    } else if (function == "_isnan") {
        state.x[0] = std::isnan(d0);
    } else {
        return false;
    }
    return true;
}

bool dispatch_text_conversion(const std::string& function, CpuState& state,
                              GuestMemory& memory) {
    if (function != "MultiByteToWideChar" &&
        function != "WideCharToMultiByte") return false;
    constexpr int maximum_units = 16 * 1024 * 1024;
    const auto code_page = static_cast<UINT>(state.x[0]);
    const auto flags = static_cast<DWORD>(state.x[1]);
    const auto source_address = state.x[2];
    const auto source_count = static_cast<int>(state.x[3]);
    const auto destination_address = state.x[4];
    const auto destination_count = static_cast<int>(state.x[5]);
    if (source_count < -1 || source_count == 0 ||
        destination_count < 0 || source_count > maximum_units ||
        destination_count > maximum_units) {
        SetLastError(ERROR_INVALID_PARAMETER);
        state.x[0] = 0;
        return true;
    }
    const bool wide_source = function == "WideCharToMultiByte";
    int length = source_count;
    if (length == -1) {
        length = 0;
        while (length < maximum_units) {
            const auto address = source_address +
                                 static_cast<std::uint64_t>(length) *
                                     (wide_source ? 2U : 1U);
            const auto character = wide_source
                                       ? static_cast<std::uint16_t>(memory.read8(address)) |
                                             (static_cast<std::uint16_t>(
                                                  memory.read8(address + 1)) << 8)
                                       : memory.read8(address);
            ++length;
            if (character == 0) break;
        }
        if (length == maximum_units) {
            throw std::runtime_error("guest text conversion input is not terminated");
        }
    }
    if (wide_source) {
        std::wstring input(static_cast<std::size_t>(length), L'\0');
        for (int index = 0; index < length; ++index) {
            input[index] = static_cast<wchar_t>(
                static_cast<std::uint16_t>(memory.read8(source_address + index * 2ULL)) |
                (static_cast<std::uint16_t>(
                     memory.read8(source_address + index * 2ULL + 1)) << 8));
        }
        std::vector<char> output(static_cast<std::size_t>(destination_count));
        char default_character = 0;
        const char* default_pointer = nullptr;
        if (state.x[6] != 0) {
            default_character = static_cast<char>(memory.read8(state.x[6]));
            default_pointer = &default_character;
        }
        BOOL used_default = FALSE;
        const auto result = WideCharToMultiByte(
            code_page, flags, input.data(), source_count,
            destination_count == 0 ? nullptr : output.data(),
            destination_count, default_pointer,
            state.x[7] == 0 ? nullptr : &used_default);
        const auto error = GetLastError();
        if (result > 0 && destination_count != 0) {
            for (int index = 0; index < result; ++index) {
                memory.write8(destination_address + index,
                              static_cast<std::uint8_t>(output[index]));
            }
        }
        if (state.x[7] != 0) {
            memory.write32(state.x[7], used_default);
        }
        state.x[0] = static_cast<std::uint32_t>(result);
        SetLastError(error);
    } else {
        std::string input(static_cast<std::size_t>(length), '\0');
        for (int index = 0; index < length; ++index) {
            input[index] = static_cast<char>(memory.read8(source_address + index));
        }
        std::vector<wchar_t> output(static_cast<std::size_t>(destination_count));
        const auto result = MultiByteToWideChar(
            code_page, flags, input.data(), source_count,
            destination_count == 0 ? nullptr : output.data(), destination_count);
        const auto error = GetLastError();
        if (result > 0 && destination_count != 0) {
            for (int index = 0; index < result; ++index) {
                memory.write8(destination_address + index * 2ULL,
                              static_cast<std::uint8_t>(output[index]));
                memory.write8(destination_address + index * 2ULL + 1,
                              static_cast<std::uint8_t>(output[index] >> 8));
            }
        }
        state.x[0] = static_cast<std::uint32_t>(result);
        SetLastError(error);
    }
    return true;
}

void mirror_critical_section(GuestMemory& memory, std::uint64_t address,
                             const CRITICAL_SECTION& section) {
    static_assert(sizeof(CRITICAL_SECTION) == 40);
    const std::array fields{
        GuestScalarWrite{address, 0, 8}, // Host debug pointer stays private.
        GuestScalarWrite{address + 8, static_cast<std::uint32_t>(section.LockCount), 4},
        GuestScalarWrite{address + 12, static_cast<std::uint32_t>(section.RecursionCount), 4},
        GuestScalarWrite{address + 16,
            reinterpret_cast<std::uintptr_t>(section.OwningThread), 8},
        GuestScalarWrite{address + 24, 0, 8}, // Host semaphore stays private.
        GuestScalarWrite{address + 32, section.SpinCount, 8},
    };
    memory.write_scalar_fields(fields);
}

std::vector<std::wstring> parse_crt_command_line(std::wstring_view command_line) {
    std::vector<std::wstring> arguments;
    std::size_t offset = 0;
    std::wstring executable;
    bool quoted = false;
    while (offset < command_line.size()) {
        const auto character = command_line[offset];
        if (character == L'"') {
            quoted = !quoted;
            ++offset;
        } else if (!quoted && (character == L' ' || character == L'\t')) {
            break;
        } else {
            executable.push_back(character);
            ++offset;
        }
    }
    arguments.push_back(std::move(executable));
    while (offset < command_line.size()) {
        while (offset < command_line.size() &&
               (command_line[offset] == L' ' || command_line[offset] == L'\t')) {
            ++offset;
        }
        if (offset == command_line.size()) break;
        std::wstring argument;
        quoted = false;
        while (offset < command_line.size()) {
            std::size_t slashes = 0;
            while (offset < command_line.size() &&
                   command_line[offset] == L'\\') {
                ++slashes;
                ++offset;
            }
            if (offset < command_line.size() && command_line[offset] == L'"') {
                argument.append(slashes / 2, L'\\');
                if ((slashes & 1U) != 0) {
                    argument.push_back(L'"');
                    ++offset;
                } else if (quoted && offset + 1 < command_line.size() &&
                           command_line[offset + 1] == L'"') {
                    argument.push_back(L'"');
                    offset += 2;
                } else {
                    quoted = !quoted;
                    ++offset;
                }
                continue;
            }
            argument.append(slashes, L'\\');
            if (offset == command_line.size() ||
                (!quoted && (command_line[offset] == L' ' ||
                             command_line[offset] == L'\t'))) {
                break;
            }
            argument.push_back(command_line[offset++]);
        }
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

} // namespace

std::optional<std::pair<HANDLE, std::uint32_t>> make_child_bootstrap_mapping(
    const ChildBootstrap& bootstrap) {
    if (bootstrap.guest_command_line.empty() ||
        bootstrap.guest_command_line.size() >= 32768 ||
        bootstrap.guest_environment.size() < 2 ||
        bootstrap.guest_environment.size() > 524288 ||
        bootstrap.inherited_handles.size() > 64) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return std::nullopt;
    }
    ChildBootstrapHeader header{};
    header.magic = child_bootstrap_magic;
    header.version = child_bootstrap_version;
    header.command_units = static_cast<std::uint32_t>(
        bootstrap.guest_command_line.size());
    header.environment_units = static_cast<std::uint32_t>(
        bootstrap.guest_environment.size());
    header.handle_count = static_cast<std::uint32_t>(
        bootstrap.inherited_handles.size());
    header.standard_handle_tokens = bootstrap.standard_handle_tokens;
    const auto total = static_cast<std::uint64_t>(sizeof(header)) +
        static_cast<std::uint64_t>(header.command_units) * sizeof(wchar_t) +
        static_cast<std::uint64_t>(header.environment_units) * sizeof(wchar_t) +
        static_cast<std::uint64_t>(header.handle_count) *
            sizeof(ChildBootstrapHandle);
    if (total > maximum_child_bootstrap_bytes) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return std::nullopt;
    }
    header.total_bytes = static_cast<std::uint32_t>(total);
    std::vector<std::uint8_t> bytes(header.total_bytes);
    std::memcpy(bytes.data(), &header, sizeof(header));
    std::size_t cursor = sizeof(header);
    std::memcpy(bytes.data() + cursor, bootstrap.guest_command_line.data(),
                header.command_units * sizeof(wchar_t));
    cursor += header.command_units * sizeof(wchar_t);
    std::memcpy(bytes.data() + cursor, bootstrap.guest_environment.data(),
                header.environment_units * sizeof(wchar_t));
    cursor += header.environment_units * sizeof(wchar_t);
    for (const auto& [guest, native] : bootstrap.inherited_handles) {
        const ChildBootstrapHandle entry{guest, native};
        std::memcpy(bytes.data() + cursor, &entry, sizeof(entry));
        cursor += sizeof(entry);
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const auto mapping = ::CreateFileMappingW(
        INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
        header.total_bytes, nullptr);
    if (!mapping) return std::nullopt;
    auto* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0,
                              header.total_bytes);
    if (!view) {
        const auto error = GetLastError();
        CloseHandle(mapping);
        SetLastError(error);
        return std::nullopt;
    }
    std::memcpy(view, bytes.data(), bytes.size());
    UnmapViewOfFile(view);
    return std::pair{mapping, header.total_bytes};
}

std::wstring quote_child_argument(std::wstring_view argument) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'"');
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            result.push_back(character);
            slashes = 0;
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

ChildBootstrap read_child_bootstrap(std::uint64_t inherited_mapping,
                                    std::uint32_t mapped_bytes) {
    if (mapped_bytes < sizeof(ChildBootstrapHeader) ||
        mapped_bytes > maximum_child_bootstrap_bytes ||
        inherited_mapping == 0 || inherited_mapping == UINT64_MAX) {
        throw std::runtime_error("invalid child bootstrap mapping arguments");
    }
    const auto mapping = reinterpret_cast<HANDLE>(inherited_mapping);
    DWORD flags = 0;
    if (!GetHandleInformation(mapping, &flags) ||
        (flags & HANDLE_FLAG_INHERIT) == 0) {
        throw std::runtime_error("child bootstrap mapping was not inherited");
    }
    const auto* view = static_cast<const std::uint8_t*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, mapped_bytes));
    if (!view) throw std::runtime_error("cannot map child bootstrap");
    try {
        ChildBootstrapHeader header{};
        std::memcpy(&header, view, sizeof(header));
        const auto payload = static_cast<std::uint64_t>(header.command_units) *
                                 sizeof(wchar_t) +
                             static_cast<std::uint64_t>(header.environment_units) *
                                 sizeof(wchar_t) +
                             static_cast<std::uint64_t>(header.handle_count) *
                                 sizeof(ChildBootstrapHandle);
        if (header.magic != child_bootstrap_magic ||
            header.version != child_bootstrap_version ||
            header.total_bytes != mapped_bytes ||
            header.reserved0 != 0 || header.reserved1 != 0 ||
            header.command_units == 0 || header.command_units >= 32768 ||
            header.environment_units < 2 ||
            header.handle_count > 64 ||
            payload + sizeof(header) != mapped_bytes) {
            throw std::runtime_error("invalid child bootstrap header");
        }
        ChildBootstrap result;
        result.standard_handle_tokens = header.standard_handle_tokens;
        std::size_t cursor = sizeof(header);
        result.guest_command_line.resize(header.command_units);
        std::memcpy(result.guest_command_line.data(), view + cursor,
                    header.command_units * sizeof(wchar_t));
        cursor += header.command_units * sizeof(wchar_t);
        if (result.guest_command_line.find(L'\0') != std::wstring::npos) {
            throw std::runtime_error("child bootstrap command line contains NUL");
        }
        result.guest_environment.resize(header.environment_units);
        std::memcpy(result.guest_environment.data(), view + cursor,
                    header.environment_units * sizeof(wchar_t));
        cursor += header.environment_units * sizeof(wchar_t);
        if (result.guest_environment.back() != 0 ||
            result.guest_environment[result.guest_environment.size() - 2] != 0) {
            throw std::runtime_error("child bootstrap environment is unterminated");
        }
        for (std::uint32_t index = 0; index < header.handle_count; ++index) {
            ChildBootstrapHandle entry{};
            std::memcpy(&entry, view + cursor, sizeof(entry));
            cursor += sizeof(entry);
            if (entry.guest < host_handle_base ||
                entry.guest >= host_handle_base + (1ULL << 32) ||
                (entry.guest - host_handle_base) % 8 != 0 ||
                entry.native == 0 || entry.native == UINT64_MAX) {
                throw std::runtime_error("invalid child bootstrap handle entry");
            }
            DWORD native_flags = 0;
            if (!GetHandleInformation(reinterpret_cast<HANDLE>(entry.native),
                                      &native_flags) ||
                (native_flags & HANDLE_FLAG_INHERIT) == 0) {
                throw std::runtime_error("child bootstrap handle was not inherited");
            }
            for (const auto& existing : result.inherited_handles) {
                if (existing.first == entry.guest) {
                    throw std::runtime_error("duplicate child bootstrap guest handle");
                }
            }
            result.inherited_handles.emplace_back(entry.guest, entry.native);
        }
        for (const auto token : result.standard_handle_tokens) {
            if (token == 0) continue;
            const auto found = std::find_if(result.inherited_handles.begin(),
                result.inherited_handles.end(), [token](const auto& entry) {
                    return entry.first == token;
                });
            if (found == result.inherited_handles.end()) {
                throw std::runtime_error("child bootstrap standard handle is absent");
            }
        }
        UnmapViewOfFile(view);
        return result;
    } catch (...) {
        UnmapViewOfFile(view);
        throw;
    }
}

struct Win32Bridge::State {
    struct CriticalSection {
        CRITICAL_SECTION value{};
        bool initialized{};
        ~CriticalSection() {
            if (initialized) DeleteCriticalSection(&value);
        }
    };
    struct FlsSlot {
        std::uint64_t callback{};
        std::unordered_map<DWORD, std::uint64_t> values;
    };
    struct SList {
        std::uint64_t first{};
        std::uint16_t depth{};
    };
    struct Once {
        INIT_ONCE value{};
    };
    struct SlimLock {
        SRWLOCK value{};
        SlimLock() { InitializeSRWLock(&value); }
    };
    struct Condition {
        CONDITION_VARIABLE value{};
        Condition() { InitializeConditionVariable(&value); }
    };
    struct HeapAllocation {
        std::size_t size{};
        bool aligned{};
        bool process_heap{};
    };
    struct LocaleObject {
        _locale_t host{};
    };
    struct HostHandle {
        HANDLE value{};
        bool owned{};
    };
    struct ProcessAttributeList {
        std::vector<std::byte> native;
        std::vector<std::uint64_t> guest_handle_tokens;
        std::vector<HANDLE> inherited_handles;
        std::size_t guest_size{};
    };
    struct Stream {
        FILE* host{};
        std::uint64_t guest_buffer{};
        std::uint64_t buffer_capacity{};
        char* native_base{};
        char* native_cursor{};
        int native_count{};
        std::size_t readable_extent{};
        std::optional<std::uint64_t> guest_buffer_revision;
        bool mirrored{};
        bool closing{};
    };
    struct StreamLock {
        DWORD owner{};
        unsigned depth{};
        unsigned waiters{};
    };
    struct VirtualAllocation {
        std::uint64_t size{};
        DWORD allocation_protect{};
        std::map<std::uint64_t, DWORD> committed_pages;
    };
    struct MappedView {
        void* host{};
        std::size_t size{};
    };
    struct ImageHandle {
        HICON host{};
        bool cursor{};
        bool shared{};
        bool owned{};
        std::uint64_t module{};
        std::optional<std::uint16_t> id;
        std::u16string name;
        int width{};
        int height{};
        UINT flags{};
    };
    struct GdiObject {
        HGDIOBJ host{};
        UINT type{};
        bool owned{};
        std::uint64_t borrowed_dc{};
    };
    struct GdiDc {
        HDC host{};
        HWND window{};
        bool window_dc{};
        unsigned acquisitions{1};
    };
    struct WindowClass {
        ATOM host_atom{};
        std::wstring host_name;
        std::wstring guest_name;
        std::uint64_t guest_instance{};
        std::uint64_t guest_proc{};
        bool ansi{};
        std::uint32_t extra_bytes{};
    };
    struct ShellFolder {
        IShellFolder* host{};
        DWORD owner{};
        std::uint32_t references{1};
    };
    struct ShellAutomation {
        IDispatch* host{};
        DWORD owner{};
        std::uint32_t references{1};
        unsigned kind{};
    };
    std::recursive_mutex mutex;
    std::mutex dynamic_dll_mutex;
    std::recursive_mutex locale_mutex;
    std::unordered_map<DWORD, int> thread_locale_modes;
    std::unordered_map<DWORD, int> original_host_locale_modes;
    std::unordered_map<DWORD, std::uint64_t> lconv_pages;
    std::uint64_t next_lconv_address{lconv_base};
    bool unsigned_char_lconv{};
    std::unordered_map<std::uint64_t, std::shared_ptr<CriticalSection>> sections;
    std::unordered_map<std::uint64_t, SList> slists;
    std::unordered_map<std::uint64_t, std::shared_ptr<Once>> once_objects;
    std::unordered_map<std::uint64_t, std::shared_ptr<SlimLock>> slim_locks;
    std::unordered_map<std::uint64_t, std::shared_ptr<Condition>> conditions;
    std::unordered_map<DWORD, FlsSlot> fls_slots;
    std::unordered_set<DWORD> tls_slots;
    std::unordered_map<DWORD, std::unordered_map<DWORD, std::uint64_t>> tls_values;
    DWORD next_tls_index{};
    std::uint64_t next_tls_data_address{tls_data_base};
    std::vector<const PeImage*> registration_order;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> onexit_tables;
    std::vector<std::uint64_t> crt_exit_callbacks;
    std::vector<std::uint64_t> crt_quick_exit_callbacks;
    std::uint64_t thread_local_exe_atexit_callback{};
    bool process_exit_callbacks_suppressed{};
    std::uint32_t crt_app_type{};
    std::uint32_t narrow_argv_mode{};
    std::uint32_t wide_argv_mode{};
    bool argv_mapped{};
    std::uint32_t crt_abort_behavior{};
    std::unordered_map<int, std::uint64_t> crt_signal_handlers;
    std::int32_t crt_error_mode{};
    struct CrtErrno {
        std::uint64_t address{};
        std::int32_t value{};
    };
    std::unordered_map<DWORD, CrtErrno> crt_errno_by_thread;
    std::uint64_t crt_invalid_parameter_handler{};
    std::unordered_map<DWORD, std::uint64_t> crt_thread_invalid_parameter_handlers;
    bool crt_error_region_mapped{};
    bool crt_error_table_mapped{};
    std::uint64_t next_crt_error_buffer{crt_errors_base + 0x40000};
    std::unordered_map<DWORD, std::uint64_t> crt_error_buffers;
    std::vector<std::uint64_t> free_crt_error_buffers;
    std::unordered_map<DWORD, std::uint32_t> crt_rand_states;
    std::unordered_map<DWORD, std::uint64_t> strtok_cursors;
    CrtErrno& thread_errno() {
        std::lock_guard lock(mutex);
        return crt_errno_by_thread[GetCurrentThreadId()];
    }
    std::int32_t crt_new_mode{};
    std::uint64_t crt_new_handler{};
    std::uint64_t crt_fmode_address{};
    std::int32_t crt_fmode{_O_TEXT};
    std::uint64_t crt_commode_address{};
    std::int32_t crt_commode{};
    std::uint64_t initial_narrow_environment{};
    std::uint64_t initial_wide_environment{};
    std::uint64_t narrow_environment_slot{};
    std::uint64_t wide_environment_slot{};
    std::unordered_map<std::wstring, std::uint64_t> modules;
    std::unordered_map<std::uint64_t, const PeImage*> guest_modules;
    std::function<std::uint64_t(const std::filesystem::path&,
        const std::vector<std::filesystem::path>&, bool, ReferenceEngine&)>
        guest_dll_loader;
    DWORD default_dll_search_flags{};
    std::unordered_map<std::string, std::uint64_t> dynamic_kernel32_exports;
    struct GuestDllDirectory {
        DLL_DIRECTORY_COOKIE host{};
        std::wstring path;
    };
    std::uint64_t next_dll_cookie{dll_cookie_base};
    std::unordered_map<std::uint64_t, GuestDllDirectory> dll_directories;
    std::unordered_set<std::uint64_t> thread_notifications_disabled;
    std::uint64_t executable_base{};
    std::uint64_t next_heap_address{heap_base};
    std::uint64_t next_virtual_address{virtual_memory_base};
    std::map<std::uint64_t, VirtualAllocation> virtual_allocations;
    std::uint64_t next_file_view_address{file_view_base};
    std::map<std::uint64_t, MappedView> mapped_views;
    std::unordered_map<std::uint64_t, HeapAllocation> heap_allocations;
    std::uint64_t next_local_address{local_heap_base};
    std::unordered_map<std::uint64_t, std::size_t> local_allocations;
    std::uint64_t next_com_task_address{com_task_base};
    std::unordered_map<std::uint64_t, std::size_t> com_task_allocations;
    std::uint64_t next_locale_address{locale_object_base};
    std::unordered_map<std::uint64_t, LocaleObject> locale_objects;
    std::uint64_t next_host_handle{host_handle_base};
    std::unordered_map<std::uint64_t, HostHandle> host_handles;
    std::uint32_t winsock_startups{};
    std::uint64_t next_socket_handle{socket_handle_base};
    std::unordered_map<std::uint64_t, SOCKET> sockets;
    std::uint64_t next_registry_handle{registry_handle_base};
    std::unordered_map<std::uint64_t, HKEY> registry_handles;
    std::unordered_map<DWORD, int> winsock_errors;
    std::uint64_t next_resolver_address{resolver_base};
    std::unordered_map<std::uint64_t, std::size_t> resolver_allocations;
    std::unordered_map<DWORD, std::uint64_t> inet_ntoa_strings;
    std::unordered_map<std::uint64_t, ProcessAttributeList> process_attributes;
    std::unordered_map<std::uint64_t, HANDLE> find_handles;
    std::unordered_map<std::uint64_t, std::vector<std::uint8_t>>
        version_info_buffers;
    std::uint64_t next_image_handle{image_handle_base};
    std::unordered_map<std::uint64_t, ImageHandle> image_handles;
    std::unordered_map<DWORD, std::uint64_t> current_cursors;
    std::uint64_t next_gdi_handle{gdi_handle_base};
    std::unordered_map<std::uint64_t, GdiObject> gdi_objects;
    std::unordered_map<std::uint64_t, GdiDc> gdi_dcs;
    std::uint64_t next_window_handle{window_handle_base};
    std::uint64_t next_window_scratch{window_scratch_base};
    std::unordered_map<ATOM, WindowClass> window_classes;
    std::unordered_map<std::uint64_t, std::shared_ptr<GuestWindowContext>> windows;
    std::unordered_map<HWND, std::uint64_t> host_windows;
    std::unordered_map<std::uint64_t, HGLRC> gl_contexts;
    std::uint64_t next_gl_context{0x0000603000000000ULL};
    std::uint64_t next_gl_string{0x0000603100000000ULL};
    std::unordered_map<std::string, std::uint64_t> gl_strings;
    std::unordered_map<std::string, std::uint64_t> gl_exports;
    std::shared_ptr<gl_detail::BufferMappings> gl_buffer_mappings{std::make_shared<gl_detail::BufferMappings>()};
    std::shared_ptr<gl_detail::SyncObjects> gl_sync_objects{std::make_shared<gl_detail::SyncObjects>()};
    std::unordered_map<std::uint64_t, HMENU> gui_menus;
    std::uint64_t next_gui_menu{0x0000603200000000ULL};
    std::uint64_t shell_vtable{};
    std::uint64_t next_shell_folder{shell_folder_base};
    std::unordered_map<std::uint64_t, ShellFolder> shell_folders;
    std::uint64_t next_guest_bstr{guest_bstr_base};
    std::unordered_map<std::uint64_t, std::pair<std::uint64_t, std::size_t>>
        guest_bstr_allocations;
    std::uint64_t next_shell_automation{shell_automation_base};
    std::array<std::uint64_t, 4> shell_automation_vtables{};
    std::unordered_map<std::uint64_t, ShellAutomation> shell_automation;
    std::uint64_t next_cng_handle{cng_handle_base};
    std::unordered_map<std::uint64_t, BCRYPT_ALG_HANDLE> cng_algorithms;
    std::unordered_map<DWORD, std::string> window_failures;
    std::unordered_set<std::uint64_t> raw_device_handles;
    std::unordered_set<std::uint64_t> raw_input_handles;
    bool guest_dpi_awareness_set{};
    PROCESS_DPI_AWARENESS guest_dpi_awareness{PROCESS_DPI_UNAWARE};
    bool explicit_app_id_set{};
    std::wstring explicit_app_id;
    struct ComApartment {
        bool apartment_threaded{};
        std::uint32_t co_depth{};
        std::uint32_t ole_depth{};
    };
    std::unordered_map<DWORD, ComApartment> com_apartments;
    std::vector<std::shared_ptr<GuestThreadRecord>> guest_threads;
    bool shutting_down_guest_threads{};
    bool dynamic_module_initializing{};
    DWORD dynamic_module_initializing_thread{};
    DWORD dll_notification_thread{};
    std::condition_variable_any dynamic_module_ready;
    std::uint64_t next_guest_thread_slot{1};
    std::unordered_map<DWORD, std::uint64_t> guest_thread_slots;
    std::vector<std::uint64_t> free_guest_thread_slots;
    std::unordered_map<std::uint64_t, std::vector<std::uint64_t>>
        guest_thread_mappings;
    std::uint64_t next_stream_address{stdio_files_base};
    std::unordered_map<std::uint64_t, Stream> streams;
    std::array<Stream, 3> standard_streams{
        Stream{stdin}, Stream{stdout}, Stream{stderr}};
    std::unordered_map<std::uint64_t, StreamLock> stream_locks;
    Stream& stream_record(std::uint64_t address) {
        if (address >= stdio_base && address <= stdio_base + 64 &&
            (address - stdio_base) % 32 == 0) {
            return standard_streams[(address - stdio_base) / 32];
        }
        const auto found = streams.find(address);
        if (found == streams.end() || found->second.closing) {
            throw std::runtime_error("invalid guest CRT stream pointer");
        }
        return found->second;
    }
    // Callers hold the native FILE lock and State mutex. Only this public CRT
    // accessor is used to inspect native buffering; no native pointers escape.
    void sync_stream_buffer(GuestMemory& memory, std::uint64_t address,
                            Stream& stream) {
        if (!stream.mirrored) return;
        const auto base = memory.read64(address);
        const auto cursor = memory.read64(address + 8);
        const auto count = static_cast<std::int32_t>(memory.read32(address + 16));
        if (base != stream.guest_buffer || cursor < base || count < 0 ||
            cursor - base > stream.readable_extent ||
            static_cast<std::size_t>(count) !=
                stream.readable_extent - static_cast<std::size_t>(cursor - base)) {
            throw std::runtime_error("invalid guest CRT read buffer cursor/count");
        }
        char** native_base = nullptr;
        char** native_cursor = nullptr;
        int* native_count = nullptr;
        _get_stream_buffer_pointers(stream.host, &native_base,
                                     &native_cursor, &native_count);
        if (*native_base != stream.native_base ||
            *native_cursor != stream.native_cursor ||
            *native_count != stream.native_count) {
            throw std::runtime_error("native CRT read buffer changed outside the bridge");
        }
        const auto offset = static_cast<std::size_t>(cursor - base);
        const auto revision = memory.write_revision(base, stream.readable_extent);
        if (!revision || !stream.guest_buffer_revision ||
            revision != stream.guest_buffer_revision) {
            // Putback can edit a consumed byte and return to the same cursor.
            // Reconcile the whole exposed buffer, including consumed bytes
            // that native operations or later putback can expose again. Only
            // an unchanged owned-region revision proves its content unchanged.
            copy_from_guest(memory, base,
                {reinterpret_cast<std::uint8_t*>(stream.native_base), stream.readable_extent});
        }
        *native_cursor = stream.native_base + offset;
        *native_count = count;
        stream.native_cursor = *native_cursor;
        stream.native_count = count;
        stream.guest_buffer_revision = revision;
    }
    void reset_stream_buffer(GuestMemory& memory, std::uint64_t address,
                             Stream& stream, bool release = false) {
        memory.write64(address, 0);
        memory.write64(address + 8, 0);
        memory.write32(address + 16, 0);
        stream.mirrored = false;
        stream.native_base = nullptr;
        stream.native_cursor = nullptr;
        stream.native_count = 0;
        stream.readable_extent = 0;
        stream.guest_buffer_revision.reset();
        if (release && stream.guest_buffer != 0) {
            memory.unmap_memory(stream.guest_buffer);
            stream.guest_buffer = 0;
            stream.buffer_capacity = 0;
        }
    }
    void publish_stream_buffer(GuestMemory& memory, std::uint64_t address,
                               Stream& stream, bool force_copy = false) {
        char** native_base = nullptr;
        char** native_cursor = nullptr;
        int* native_count = nullptr;
        _get_stream_buffer_pointers(stream.host, &native_base,
                                     &native_cursor, &native_count);
        const auto base = reinterpret_cast<std::uintptr_t>(*native_base);
        const auto cursor = reinterpret_cast<std::uintptr_t>(*native_cursor);
        if (base == 0 || cursor < base || *native_count < 0 ||
            cursor - base > 1024 * 1024 ||
            static_cast<std::size_t>(*native_count) >
                1024 * 1024 - static_cast<std::size_t>(cursor - base)) {
            reset_stream_buffer(memory, address, stream);
            return;
        }
        const auto offset = static_cast<std::size_t>(cursor - base);
        const auto extent = offset + static_cast<std::size_t>(*native_count);
        const bool unchanged_bytes = stream.mirrored &&
            *native_base == stream.native_base && extent == stream.readable_extent &&
            cursor >= reinterpret_cast<std::uintptr_t>(stream.native_cursor) &&
            *native_count <= stream.native_count &&
            cursor - reinterpret_cast<std::uintptr_t>(stream.native_cursor) ==
                static_cast<std::size_t>(stream.native_count - *native_count);
        if (stream.buffer_capacity < std::max<std::size_t>(extent, 1)) {
            const auto capacity = (std::max<std::size_t>(extent, 1) + 4095) & ~std::size_t{4095};
            const auto guest = next_stream_address;
            if (guest >= heap_base || capacity > heap_base - guest) {
                throw std::bad_alloc();
            }
            memory.map_memory(guest, capacity);
            next_stream_address += capacity;
            if (stream.guest_buffer != 0) memory.unmap_memory(stream.guest_buffer);
            stream.guest_buffer = guest;
            stream.buffer_capacity = capacity;
            force_copy = true;
        }
        if (force_copy || !unchanged_bytes) {
            if (typeid(memory) == typeid(ReferenceEngine)) {
                memory.write_bytes(stream.guest_buffer,
                    {reinterpret_cast<const std::uint8_t*>(*native_base), extent});
            } else for (std::size_t index = 0; index < extent;) {
                if (extent - index >= sizeof(std::uint64_t)) {
                    std::uint64_t value{};
                    std::memcpy(&value, *native_base + index, sizeof(value));
                    memory.write64(stream.guest_buffer + index, value);
                    index += sizeof(value);
                } else {
                    memory.write8(stream.guest_buffer + index,
                                  static_cast<std::uint8_t>((*native_base)[index]));
                    ++index;
                }
            }
        }
        memory.write64(address, stream.guest_buffer);
        memory.write64(address + 8, stream.guest_buffer + offset);
        memory.write32(address + 16, static_cast<std::uint32_t>(*native_count));
        stream.native_base = *native_base;
        stream.native_cursor = *native_cursor;
        stream.native_count = *native_count;
        stream.readable_extent = extent;
        stream.guest_buffer_revision = memory.write_revision(stream.guest_buffer, extent);
        stream.mirrored = true;
    }
    struct StreamAccess {
        State& owner;
        GuestMemory& memory;
        std::uint64_t address;
        FILE* host{};
        StreamAccess(State& state, GuestMemory& guest_memory, std::uint64_t guest)
            : owner(state), memory(guest_memory), address(guest) {
            {
                std::lock_guard lock(owner.mutex);
                host = owner.stream_record(address).host;
                if (address >= stdio_base && address <= stdio_base + 64 &&
                    !memory.query_region(address)) {
                    memory.map_memory(stdio_base, 4096);
                }
                ++owner.stream_locks[address].waiters;
            }
            // Never wait for a FILE lock while holding State: its guest owner
            // needs State to unlock it. Native CRT FILE locks are recursive.
            _lock_file(host);
            try {
                std::lock_guard lock(owner.mutex);
                owner.sync_stream_buffer(memory, address, owner.stream_record(address));
            } catch (...) {
                release();
                throw;
            }
        }
        StreamAccess(const StreamAccess&) = delete;
        StreamAccess& operator=(const StreamAccess&) = delete;
        ~StreamAccess() { release(); }
        void release() {
            _unlock_file(host);
            std::lock_guard lock(owner.mutex);
            const auto found = owner.stream_locks.find(address);
            if (found != owner.stream_locks.end() &&
                --found->second.waiters == 0 && found->second.depth == 0) {
                owner.stream_locks.erase(found);
            }
        }
        void publish(bool force_copy = false) {
            std::lock_guard lock(owner.mutex);
            owner.publish_stream_buffer(memory, address, owner.stream_record(address), force_copy);
        }
        void reset() {
            std::lock_guard lock(owner.mutex);
            owner.reset_stream_buffer(memory, address, owner.stream_record(address));
        }
    };
    struct StreamLifecycle {
        State& owner;
        std::uint64_t address;
        FILE* host{};
        StreamLifecycle(State& state, GuestMemory& memory, std::uint64_t guest)
            : owner(state), address(guest) {
            {
                std::lock_guard lock(owner.mutex);
                const auto busy = owner.stream_locks.find(address);
                if (busy != owner.stream_locks.end() &&
                    (busy->second.depth != 0 || busy->second.waiters != 0)) {
                    throw std::runtime_error("cannot close/reopen a locked guest CRT stream");
                }
                const auto found = owner.streams.find(address);
                if (found == owner.streams.end() || found->second.closing) {
                    throw std::runtime_error("invalid guest CRT stream pointer");
                }
                auto& stream = found->second;
                stream.closing = true;
                host = stream.host;
            }
            _lock_file(host);
            try {
                std::lock_guard lock(owner.mutex);
                owner.sync_stream_buffer(memory, address, owner.streams.at(address));
            } catch (...) {
                _unlock_file(host);
                std::lock_guard lock(owner.mutex);
                owner.streams.at(address).closing = false;
                throw;
            }
            _unlock_file(host);
        }
        StreamLifecycle(const StreamLifecycle&) = delete;
        StreamLifecycle& operator=(const StreamLifecycle&) = delete;
        ~StreamLifecycle() {
            std::lock_guard lock(owner.mutex);
            if (const auto found = owner.streams.find(address); found != owner.streams.end()) {
                found->second.closing = false;
            }
        }
    };
    std::unordered_set<int> file_descriptors;
    std::unordered_map<int, std::uint64_t> descriptor_handles;
    std::unordered_map<DWORD, std::uint64_t> standard_handles;
    std::uint64_t next_environment_address{environment_base};
    std::unordered_map<std::uint64_t, std::size_t> environment_allocations;
    bool environment_ready{};
    std::map<std::wstring, std::wstring, WindowsEnvironmentLess> guest_environment;
    std::map<std::wstring, std::wstring, WindowsEnvironmentLess> crt_environment;
    std::filesystem::path child_host_executable;
    std::vector<std::filesystem::path> child_dll_directories;
    std::optional<std::uint64_t> child_instruction_limit;
    bool child_jit{};
    std::unordered_map<std::string, std::pair<std::uint64_t, std::string>> getenv_ansi;
    std::unordered_map<std::wstring, std::pair<std::uint64_t, std::wstring>> getenv_wide;
    bool crt_time_mapped{};
    bool startup_strings_mapped{};
    std::wstring command_line_w;
    std::string command_line_a;
    std::function<std::uint64_t(std::uint64_t, std::uint64_t)> guest_callback;
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t)> guest_tls_callback;
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t,
                                std::uint64_t)> guest_window_callback;
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t)> guest_binary_callback;
    std::uint64_t unhandled_exception_filter{};
    ~State() {
        std::vector<std::shared_ptr<GuestThreadRecord>> threads_to_join;
        {
            std::lock_guard lock(mutex);
            shutting_down_guest_threads = true;
            threads_to_join.swap(guest_threads);
        }
        for (const auto& thread : threads_to_join) {
            if (thread->retained != nullptr) {
                if (WaitForSingleObject(thread->retained, 1000) == WAIT_TIMEOUT) {
                    // ExitProcess also tears down surviving threads. A suspended
                    // guest worker must not strand the translator on teardown.
                    TerminateThread(thread->retained, 0xe0000002U);
                    WaitForSingleObject(thread->retained, INFINITE);
                }
            }
        }
        for (const auto& [guest_cookie, directory] : dll_directories) {
            static_cast<void>(guest_cookie);
            RemoveDllDirectory(directory.host);
        }
        for (const auto& [guest, context] : gl_contexts) {
            static_cast<void>(guest);
            if (::wglGetCurrentContext() == context) ::wglMakeCurrent(nullptr, nullptr);
            ::wglDeleteContext(context);
        }
        for (const auto& [guest, dc] : gdi_dcs) {
            static_cast<void>(guest);
            if (dc.window_dc) {
                for (unsigned i = 0; i < dc.acquisitions; ++i) ::ReleaseDC(dc.window, dc.host);
            } else {
                ::DeleteDC(dc.host);
            }
        }
        std::vector<HWND> windows_to_destroy;
        for (const auto& [native, guest] : host_windows) {
            static_cast<void>(guest);
            if (GetWindowThreadProcessId(native, nullptr) == GetCurrentThreadId()) {
                windows_to_destroy.push_back(native);
            }
        }
        for (const auto native : windows_to_destroy) {
            DestroyWindow(native);
        }
        for (const auto& [atom, registration] : window_classes) {
            static_cast<void>(atom);
            UnregisterClassW(registration.host_name.c_str(),
                             GetModuleHandleW(nullptr));
        }
        if (const auto original = original_host_locale_modes.find(GetCurrentThreadId());
            original != original_host_locale_modes.end()) {
            static_cast<void>(_configthreadlocale(original->second));
        }
        for (const auto& [address, view] : mapped_views) {
            static_cast<void>(address);
            UnmapViewOfFile(view.host);
        }
        for (const auto& [address, locale] : locale_objects) {
            static_cast<void>(address);
            _free_locale(locale.host);
        }
        for (auto& [address, attributes] : process_attributes) {
            static_cast<void>(address);
            DeleteProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                    attributes.native.data()));
        }
        for (const auto& [address, handle] : host_handles) {
            static_cast<void>(address);
            if (handle.owned) {
                // Process exit closes protected handles too; drop the guest's
                // protection before releasing our retained host handle.
                SetHandleInformation(handle.value,
                                     HANDLE_FLAG_PROTECT_FROM_CLOSE, 0);
                CloseHandle(handle.value);
            }
        }
        for (const auto& [address, key] : registry_handles) {
            static_cast<void>(address);
            RegCloseKey(key);
        }
        for (const auto& [address, native] : sockets) {
            static_cast<void>(address);
            static_cast<void>(::closesocket(native));
        }
        for (std::uint32_t index = 0; index < winsock_startups; ++index) {
            static_cast<void>(::WSACleanup());
        }
        for (const auto& [address, handle] : find_handles) {
            static_cast<void>(address);
            FindClose(handle);
        }
        for (const auto& [address, image] : image_handles) {
            static_cast<void>(address);
            if (image.owned) {
                if (image.cursor) {
                    DestroyCursor(reinterpret_cast<HCURSOR>(image.host));
                } else {
                    DestroyIcon(image.host);
                }
            }
        }
        for (const auto& [address, folder] : shell_folders) {
            static_cast<void>(address);
            folder.host->Release();
        }
        for (const auto& [address, automation] : shell_automation) {
            static_cast<void>(address);
            automation.host->Release();
        }
        for (const auto& [address, algorithm] : cng_algorithms) {
            static_cast<void>(address);
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
        for (const auto& [address, object] : gdi_objects) {
            static_cast<void>(address);
            if (object.owned) DeleteObject(object.host);
        }
        for (const auto& [address, stream] : streams) {
            static_cast<void>(address);
            std::fclose(stream.host);
        }
        if (const auto apartment = com_apartments.find(GetCurrentThreadId());
            apartment != com_apartments.end()) {
            for (std::uint32_t index = 0;
                 index < apartment->second.ole_depth; ++index) {
                OleUninitialize();
            }
            for (std::uint32_t index = 0;
                 index < apartment->second.co_depth; ++index) {
                CoUninitialize();
            }
        }
    }
};

Win32Bridge::Win32Bridge() : state_(std::make_unique<State>()) {
    state_->modules.emplace(L"kernel32.dll", kernel32_handle);
    state_->modules.emplace(L"kernelbase.dll", kernelbase_handle);
    state_->modules.emplace(L"opengl32.dll", opengl32_handle);
}
Win32Bridge::~Win32Bridge() {
    // The caller may have already destroyed its engine while unwinding a
    // failed guest launch. Native window teardown must never reenter it.
    std::lock_guard lock(state_->mutex);
    for (auto& [token, context] : state_->windows) {
        static_cast<void>(token);
        context->shutting_down = true;
    }
}

std::uint64_t Win32Bridge::allocate_guest_bstr(
    GuestMemory& memory, std::wstring_view value) {
    if (value.size() > 65536) return 0;
    const auto bytes = value.size() * 2;
    const auto size = bytes + 10;
    std::lock_guard lock(state_->mutex);
    const auto base = state_->next_guest_bstr;
    const auto stride = (size + guest_page_size - 1) & ~(guest_page_size - 1);
    if (base > UINT64_MAX - stride ||
        !memory.is_range_available(base, size)) return 0;
    memory.map_memory(base, size);
    const auto pointer = base + 8;
    memory.write32(pointer - 4, static_cast<std::uint32_t>(bytes));
    for (std::size_t index = 0; index < value.size(); ++index) {
        memory.write8(pointer + index * 2,
            static_cast<std::uint8_t>(value[index]));
        memory.write8(pointer + index * 2 + 1,
            static_cast<std::uint8_t>(value[index] >> 8));
    }
    memory.write8(pointer + bytes, 0);
    memory.write8(pointer + bytes + 1, 0);
    state_->guest_bstr_allocations.emplace(pointer,
        std::make_pair(base, size));
    state_->next_guest_bstr += stride;
    return pointer;
}

std::optional<std::wstring> Win32Bridge::read_guest_bstr(
    GuestMemory& memory, std::uint64_t pointer) {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->guest_bstr_allocations.find(pointer);
    if (found == state_->guest_bstr_allocations.end() ||
        !guest_range(memory, pointer - 4, 4, false)) return std::nullopt;
    const auto bytes = memory.read32(pointer - 4);
    if ((bytes & 1U) != 0 || bytes > found->second.second - 10 ||
        !guest_range(memory, pointer, bytes + 2, false)) return std::nullopt;
    std::wstring value;
    value.reserve(bytes / 2);
    for (std::size_t index = 0; index < bytes; index += 2) {
        value.push_back(static_cast<wchar_t>(memory.read8(pointer + index) |
            (memory.read8(pointer + index + 1) << 8)));
    }
    return value;
}

bool Win32Bridge::free_guest_bstr(GuestMemory& memory, std::uint64_t pointer) {
    if (pointer == 0) return true;
    std::lock_guard lock(state_->mutex);
    const auto found = state_->guest_bstr_allocations.find(pointer);
    if (found == state_->guest_bstr_allocations.end()) return false;
    memory.unmap_memory(found->second.first);
    state_->guest_bstr_allocations.erase(found);
    return true;
}

std::uint64_t Win32Bridge::wrap_shell_automation(
    GuestMemory& memory, void* host, unsigned kind) {
    auto* engine = dynamic_cast<ReferenceEngine*>(&memory);
    auto* host_dispatch = static_cast<IDispatch*>(host);
    if (engine == nullptr || host_dispatch == nullptr || kind >= 4) {
        if (host_dispatch != nullptr) host_dispatch->Release();
        return 0;
    }
    std::lock_guard lock(state_->mutex);
    if (state_->shell_automation_vtables[kind] == 0) {
        const auto vtable = shell_automation_vtable_base +
            static_cast<std::uint64_t>(kind) * guest_page_size;
        if (!memory.is_range_available(vtable, guest_page_size)) {
            host_dispatch->Release();
            return 0;
        }
        memory.map_memory(vtable, guest_page_size);
        state_->shell_automation_vtables[kind] = vtable;
        for (unsigned slot = 0; slot <= 16; ++slot) {
            const auto callback = engine->register_guest_call(
                [this, kind, slot](CpuState& call, GuestMemory& guest) {
                    shell_automation_call(kind, slot, call, guest);
                });
            memory.write64(vtable + slot * 8, callback);
        }
    }
    const auto object = state_->next_shell_automation;
    if (object > UINT64_MAX - guest_page_size ||
        !memory.is_range_available(object, 16)) {
        host_dispatch->Release();
        return 0;
    }
    memory.map_memory(object, 16);
    memory.write64(object, state_->shell_automation_vtables[kind]);
    state_->shell_automation.emplace(object, State::ShellAutomation{
        host_dispatch, GetCurrentThreadId(), 1, kind});
    state_->next_shell_automation += guest_page_size;
    return object;
}

void Win32Bridge::shell_automation_call(
    unsigned kind, unsigned slot, CpuState& call, GuestMemory& memory) {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->shell_automation.find(call.x[0]);
    if (found == state_->shell_automation.end() ||
        found->second.kind != kind) {
        call.x[0] = static_cast<std::uint32_t>(E_POINTER);
        return;
    }
    auto& object = found->second;
    if (object.owner != GetCurrentThreadId()) {
        call.x[0] = static_cast<std::uint32_t>(RPC_E_WRONG_THREAD);
        return;
    }
    if (slot == 1) {
        call.x[0] = ++object.references;
        return;
    }
    if (slot == 2) {
        const auto remaining = --object.references;
        if (remaining == 0) {
            object.host->Release();
            memory.unmap_memory(found->first);
            state_->shell_automation.erase(found);
        }
        call.x[0] = remaining;
        return;
    }
    if (slot == 0) {
        if (!guest_range(memory, call.x[1], sizeof(GUID), false) ||
            !guest_range(memory, call.x[2], 8, true)) {
            call.x[0] = static_cast<std::uint32_t>(E_POINTER);
            return;
        }
        memory.write64(call.x[2], 0);
        GUID requested{};
        auto* bytes = reinterpret_cast<std::uint8_t*>(&requested);
        for (std::size_t index = 0; index < sizeof(GUID); ++index) {
            bytes[index] = memory.read8(call.x[1] + index);
        }
        const auto supported = IsEqualGUID(requested, IID_IUnknown) ||
            IsEqualGUID(requested, IID_IDispatch) ||
            (kind == shell_dispatch_kind &&
             IsEqualGUID(requested, __uuidof(IShellDispatch))) ||
            (kind == shell_folder_kind &&
             IsEqualGUID(requested, __uuidof(Folder))) ||
            (kind == shell_items_kind &&
             IsEqualGUID(requested, __uuidof(FolderItems))) ||
            (kind == shell_item_kind &&
             IsEqualGUID(requested, __uuidof(FolderItem)));
        if (!supported) {
            call.x[0] = static_cast<std::uint32_t>(E_NOINTERFACE);
            return;
        }
        ++object.references;
        memory.write64(call.x[2], found->first);
        call.x[0] = S_OK;
        return;
    }
    if (slot == 3) {
        if (!guest_range(memory, call.x[1], 4, true)) {
            call.x[0] = static_cast<std::uint32_t>(E_POINTER);
            return;
        }
        UINT count = 0;
        const auto hr = object.host->GetTypeInfoCount(&count);
        if (SUCCEEDED(hr)) memory.write32(call.x[1], count);
        call.x[0] = static_cast<std::uint32_t>(hr);
        return;
    }
    const auto output_pointer = [&](std::uint64_t address) {
        if (!guest_range(memory, address, 8, true)) {
            call.x[0] = static_cast<std::uint32_t>(E_POINTER);
            return false;
        }
        memory.write64(address, 0);
        return true;
    };
    if (kind == shell_dispatch_kind && slot == 9) {
        if (!guest_range(memory, call.x[1], sizeof(VARIANT), false) ||
            !output_pointer(call.x[2])) return;
        VARIANT directory{};
        directory.vt = static_cast<VARTYPE>(memory.read32(call.x[1]) & 0xffffU);
        if (directory.vt == VT_BSTR) {
            const auto value = read_guest_bstr(memory, memory.read64(call.x[1] + 8));
            if (!value) {
                call.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                return;
            }
            directory.bstrVal = SysAllocStringLen(value->data(),
                static_cast<UINT>(value->size()));
            if (directory.bstrVal == nullptr) {
                call.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                return;
            }
        } else if (directory.vt == VT_I4) {
            directory.lVal = static_cast<LONG>(memory.read32(call.x[1] + 8));
        } else {
            call.x[0] = static_cast<std::uint32_t>(DISP_E_TYPEMISMATCH);
            return;
        }
        Folder* folder = nullptr;
        const auto hr = static_cast<IShellDispatch*>(object.host)->NameSpace(
            directory, &folder);
        VariantClear(&directory);
        if (SUCCEEDED(hr) && folder != nullptr) {
            const auto wrapped = wrap_shell_automation(
                memory, static_cast<IDispatch*>(folder), shell_folder_kind);
            if (wrapped == 0) {
                call.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                return;
            }
            memory.write64(call.x[2], wrapped);
        }
        call.x[0] = static_cast<std::uint32_t>(hr);
        return;
    }
    if (kind == shell_folder_kind && slot == 11) {
        if (!output_pointer(call.x[1])) return;
        FolderItems* items = nullptr;
        const auto hr = static_cast<Folder*>(object.host)->Items(&items);
        if (SUCCEEDED(hr) && items != nullptr) {
            const auto wrapped = wrap_shell_automation(
                memory, static_cast<IDispatch*>(items), shell_items_kind);
            if (wrapped == 0) {
                call.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                return;
            }
            memory.write64(call.x[1], wrapped);
        }
        call.x[0] = static_cast<std::uint32_t>(hr);
        return;
    }
    if (kind == shell_items_kind && slot == 7) {
        if (!guest_range(memory, call.x[1], 4, true)) {
            call.x[0] = static_cast<std::uint32_t>(E_POINTER);
            return;
        }
        long count = 0;
        const auto hr = static_cast<FolderItems*>(object.host)->get_Count(&count);
        if (SUCCEEDED(hr)) memory.write32(call.x[1],
            static_cast<std::uint32_t>(count));
        call.x[0] = static_cast<std::uint32_t>(hr);
        return;
    }
    if (kind == shell_items_kind && slot == 10) {
        if (!guest_range(memory, call.x[1], sizeof(VARIANT), false) ||
            !output_pointer(call.x[2])) return;
        if (memory.read32(call.x[1]) != VT_I4) {
            call.x[0] = static_cast<std::uint32_t>(DISP_E_TYPEMISMATCH);
            return;
        }
        VARIANT index{};
        index.vt = VT_I4;
        index.lVal = static_cast<LONG>(memory.read32(call.x[1] + 8));
        FolderItem* item = nullptr;
        const auto hr = static_cast<FolderItems*>(object.host)->Item(index, &item);
        if (SUCCEEDED(hr) && item != nullptr) {
            const auto wrapped = wrap_shell_automation(
                memory, static_cast<IDispatch*>(item), shell_item_kind);
            if (wrapped == 0) {
                call.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                return;
            }
            memory.write64(call.x[2], wrapped);
        }
        call.x[0] = static_cast<std::uint32_t>(hr);
        return;
    }
    if (kind == shell_item_kind && slot == 15) {
        if (!guest_range(memory, call.x[1], 2, true)) {
            call.x[0] = static_cast<std::uint32_t>(E_POINTER);
            return;
        }
        VARIANT_BOOL is_folder = VARIANT_FALSE;
        const auto hr = static_cast<FolderItem*>(object.host)->get_IsFolder(
            &is_folder);
        if (SUCCEEDED(hr)) {
            const auto result = static_cast<std::uint16_t>(is_folder);
            memory.write8(call.x[1], static_cast<std::uint8_t>(result));
            memory.write8(call.x[1] + 1,
                static_cast<std::uint8_t>(result >> 8));
        }
        call.x[0] = static_cast<std::uint32_t>(hr);
        return;
    }
    if (kind == shell_item_kind && slot == 11) {
        if (!output_pointer(call.x[1])) return;
        BSTR path = nullptr;
        const auto hr = static_cast<FolderItem*>(object.host)->get_Path(&path);
        if (SUCCEEDED(hr) && path != nullptr) {
            const auto wrapped = allocate_guest_bstr(memory,
                std::wstring_view(path, SysStringLen(path)));
            SysFreeString(path);
            if (wrapped == 0) {
                call.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                return;
            }
            memory.write64(call.x[1], wrapped);
        }
        call.x[0] = static_cast<std::uint32_t>(hr);
        return;
    }
    call.x[0] = static_cast<std::uint32_t>(E_NOTIMPL);
}

void Win32Bridge::ensure_guest_environment() {
    std::lock_guard lock(state_->mutex);
    if (state_->environment_ready) return;
    auto* block = ::GetEnvironmentStringsW();
    if (block == nullptr) {
        throw std::runtime_error("cannot snapshot the host environment");
    }
    constexpr std::size_t maximum_characters = 1024 * 1024;
    std::size_t cursor = 0;
    while (cursor < maximum_characters && block[cursor] != 0) {
        const auto start = cursor;
        while (cursor < maximum_characters && block[cursor] != 0) ++cursor;
        if (cursor == maximum_characters) {
            ::FreeEnvironmentStringsW(block);
            throw std::runtime_error("host environment block is too large");
        }
        const std::wstring_view entry(block + start, cursor - start);
        const auto separator = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
        if (separator != std::wstring_view::npos) {
            state_->guest_environment.emplace(
                std::wstring(entry.substr(0, separator)),
                std::wstring(entry.substr(separator + 1)));
        }
        ++cursor;
    }
    ::FreeEnvironmentStringsW(block);
    state_->crt_environment = state_->guest_environment;
    state_->environment_ready = true;
}

std::optional<std::wstring> Win32Bridge::guest_environment_value(
    std::wstring_view name, bool crt) {
    ensure_guest_environment();
    std::lock_guard lock(state_->mutex);
    const auto& environment = crt ? state_->crt_environment
                                  : state_->guest_environment;
    const auto found = environment.find(std::wstring(name));
    if (found == environment.end()) return std::nullopt;
    return found->second;
}

void Win32Bridge::rebuild_crt_environment(GuestMemory& memory, bool wide) {
    ensure_guest_environment();
    std::lock_guard lock(state_->mutex);
    auto& initial = wide ? state_->initial_wide_environment
                         : state_->initial_narrow_environment;
    auto& slot = wide ? state_->wide_environment_slot
                      : state_->narrow_environment_slot;
    if (slot == 0) {
        slot = state_->next_environment_address;
        memory.map_memory(slot, 16);
        state_->next_environment_address += 16;
    }
    std::vector<std::uint64_t> entries;
    entries.reserve(state_->crt_environment.size());
    for (const auto& [name, value] : state_->crt_environment) {
        const auto text = name + L"=" + value;
        const auto narrow = wide ? std::string{} : environment_ansi(text);
        const auto byte_count = wide ? (text.size() + 1) * 2 : narrow.size() + 1;
        const auto size = (byte_count + 15) & ~std::size_t{15};
        const auto address = state_->next_environment_address;
        memory.map_memory(address, size);
        state_->next_environment_address += size;
        if (wide) {
            for (std::size_t index = 0; index < text.size(); ++index) {
                memory.write8(address + index * 2,
                    static_cast<std::uint8_t>(text[index]));
                memory.write8(address + index * 2 + 1,
                    static_cast<std::uint8_t>(text[index] >> 8));
            }
        } else {
            for (std::size_t index = 0; index < narrow.size(); ++index) {
                memory.write8(address + index,
                    static_cast<std::uint8_t>(narrow[index]));
            }
        }
        entries.push_back(address);
    }
    const auto table_bytes = (entries.size() + 1) * 8;
    const auto table_size = (table_bytes + 15) & ~std::size_t{15};
    const auto table = state_->next_environment_address;
    memory.map_memory(table, table_size);
    state_->next_environment_address += table_size;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        memory.write64(table + index * 8, entries[index]);
    }
    memory.write64(table + entries.size() * 8, 0);
    initial = table;
    memory.write64(slot, table);
}

void Win32Bridge::update_guest_environment(
    GuestMemory& memory, std::wstring name,
    std::optional<std::wstring> value, bool crt) {
    ensure_guest_environment();
    std::lock_guard lock(state_->mutex);
    auto& environment = crt ? state_->crt_environment
                            : state_->guest_environment;
    if (value) environment.insert_or_assign(std::move(name),
                                             std::move(*value));
    else environment.erase(name);
    if (!crt) return;
    for (const auto& [key, cached] : state_->getenv_ansi) {
        static_cast<void>(key);
        memory.unmap_memory(cached.first);
    }
    for (const auto& [key, cached] : state_->getenv_wide) {
        static_cast<void>(key);
        memory.unmap_memory(cached.first);
    }
    state_->getenv_ansi.clear();
    state_->getenv_wide.clear();
    if (state_->narrow_environment_slot != 0) {
        rebuild_crt_environment(memory, false);
    }
    if (state_->wide_environment_slot != 0) {
        rebuild_crt_environment(memory, true);
    }
}

void Win32Bridge::set_guest_command_line(std::wstring command_line) {
    if (command_line.size() >= 32768) {
        throw std::runtime_error("guest command line exceeds the Windows limit");
    }
    state_->command_line_w = std::move(command_line);
    const auto required = WideCharToMultiByte(
        CP_ACP, 0, state_->command_line_w.c_str(), -1,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0 || required > 65536) {
        throw std::runtime_error("cannot encode guest command line");
    }
    state_->command_line_a.resize(static_cast<std::size_t>(required));
    if (WideCharToMultiByte(CP_ACP, 0, state_->command_line_w.c_str(), -1,
                            state_->command_line_a.data(), required,
                            nullptr, nullptr) != required) {
        throw std::runtime_error("cannot write encoded guest command line");
    }
}

void Win32Bridge::configure_child_launch(
    std::filesystem::path host_executable,
    std::vector<std::filesystem::path> dll_directories,
    std::optional<std::uint64_t> instruction_limit, bool jit) {
    std::lock_guard lock(state_->mutex);
    state_->child_host_executable = std::move(host_executable);
    state_->child_dll_directories = std::move(dll_directories);
    state_->child_instruction_limit = instruction_limit;
    state_->child_jit = jit;
}

void Win32Bridge::apply_child_bootstrap(ChildBootstrap bootstrap) {
    set_guest_command_line(std::move(bootstrap.guest_command_line));
    std::lock_guard lock(state_->mutex);
    state_->guest_environment.clear();
    for (std::size_t index = 0;
         index + 1 < bootstrap.guest_environment.size();) {
        const auto end = std::find(bootstrap.guest_environment.begin() + index,
                                   bootstrap.guest_environment.end(), L'\0');
        if (end == bootstrap.guest_environment.end()) {
            throw std::runtime_error("child environment entry is unterminated");
        }
        if (end == bootstrap.guest_environment.begin() + index) break;
        const std::wstring_view entry(
            bootstrap.guest_environment.data() + index,
            static_cast<std::size_t>(end -
                                     (bootstrap.guest_environment.begin() + index)));
        const auto separator = entry.find(L'=', entry.front() == L'=' ? 1 : 0);
        if (separator == std::wstring_view::npos) {
            throw std::runtime_error("invalid child environment entry");
        }
        state_->guest_environment.insert_or_assign(
            std::wstring(entry.substr(0, separator)),
            std::wstring(entry.substr(separator + 1)));
        index += entry.size() + 1;
    }
    state_->crt_environment = state_->guest_environment;
    state_->environment_ready = true;
    for (const auto& [guest, native] : bootstrap.inherited_handles) {
        if (state_->host_handles.contains(guest)) {
            throw std::runtime_error("child inherited guest handle collision");
        }
        state_->host_handles.emplace(guest,
            State::HostHandle{reinterpret_cast<HANDLE>(native), true});
        state_->next_host_handle = std::max(state_->next_host_handle, guest + 8);
    }
    for (std::size_t index = 0;
         index < bootstrap.standard_handle_tokens.size(); ++index) {
        const auto token = bootstrap.standard_handle_tokens[index];
        if (token != 0) {
            state_->standard_handles.emplace(
                index == 0 ? STD_INPUT_HANDLE :
                index == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE, token);
        }
    }
}

void Win32Bridge::register_guest_module(const PeImage& module) {
    if (module.source().empty()) {
        throw std::runtime_error("guest module has no source path");
    }
    const auto path = lowercase(std::filesystem::path(module.source()).wstring());
    const auto base_name = module_basename(path);
    std::lock_guard lock(state_->mutex);
    if (!module.is_dll()) state_->executable_base = module.base();
    state_->modules.emplace(path, module.base());
    state_->modules.emplace(base_name, module.base());
    state_->guest_modules.emplace(module.base(), &module);
    state_->registration_order.push_back(&module);
}

void Win32Bridge::unregister_guest_module(const PeImage& module) {
    std::lock_guard lock(state_->mutex);
    const auto found = state_->guest_modules.find(module.base());
    if (found == state_->guest_modules.end() || found->second != &module) return;
    state_->guest_modules.erase(found);
    for (auto entry = state_->modules.begin(); entry != state_->modules.end();) {
        if (entry->second == module.base()) entry = state_->modules.erase(entry);
        else ++entry;
    }
    state_->registration_order.erase(std::remove(
        state_->registration_order.begin(), state_->registration_order.end(),
        &module), state_->registration_order.end());
    state_->thread_notifications_disabled.erase(module.base());
}

std::optional<std::string> Win32Bridge::describe_guest_address(
    std::uint64_t address) const {
    std::lock_guard lock(state_->mutex);
    for (const auto& [base, module] : state_->guest_modules) {
        if (address < base || address - base >= module->size()) continue;
        std::ostringstream description;
        const auto& source = module->source();
        const auto separator = source.find_last_of("/\\");
        description << source.substr(separator == std::string::npos
                                         ? 0 : separator + 1)
                    << "+0x" << std::hex << address - base;
        return description.str();
    }
    return std::nullopt;
}

void Win32Bridge::begin_dynamic_module_initialization() {
    std::lock_guard lock(state_->mutex);
    if (state_->dynamic_module_initializing) {
        throw std::runtime_error("nested guest DLL initialization is unsupported");
    }
    state_->dynamic_module_initializing = true;
    state_->dynamic_module_initializing_thread = GetCurrentThreadId();
}

void Win32Bridge::end_dynamic_module_initialization() {
    {
        std::lock_guard lock(state_->mutex);
        state_->dynamic_module_initializing = false;
        state_->dynamic_module_initializing_thread = 0;
    }
    state_->dynamic_module_ready.notify_all();
}

void Win32Bridge::initialize_guest_dlls(
    ReferenceEngine& engine, std::span<PeImage* const> dependency_order) {
    std::lock_guard loader_lock(state_->dynamic_dll_mutex);
    begin_dynamic_module_initialization();
    try {
        engine.initialize_dlls(dependency_order);
    } catch (...) {
        end_dynamic_module_initialization();
        throw;
    }
    end_dynamic_module_initialization();
}

std::uint64_t Win32Bridge::load_guest_dll(
    GuestProcess& process, const std::filesystem::path& requested,
    const std::vector<std::filesystem::path>& directories,
    bool include_requested_directory, ReferenceEngine& caller) {
    std::lock_guard loader_lock(state_->dynamic_dll_mutex);
    auto plan = process.prepare_dynamic_load(
        requested, directories, include_requested_directory);
    const auto base = plan.requested->base();
    if (plan.modules.empty()) return base;
    process.reserve_dynamic_modules(plan.modules.size());
    std::vector<DynamicTlsRegistration> tls(plan.attachment_order.size());
    std::vector<PeImage*> attached;
    std::vector<PeImage*> registered;
    attached.reserve(plan.attachment_order.size());
    registered.reserve(plan.attachment_order.size());
    begin_dynamic_module_initialization();
    try {
        for (const auto& module : plan.modules) caller.stage_module(*module);
        for (std::size_t index = 0; index < plan.attachment_order.size(); ++index) {
            auto& module = *plan.attachment_order[index];
            tls[index] = initialize_dynamic_tls(caller, module);
            const std::array arguments{
                module.base(), std::uint64_t{DLL_PROCESS_ATTACH},
                std::uint64_t{0}};
            if (module.tls()) {
                for (const auto callback : module.tls()->callbacks) {
                    static_cast<void>(caller.invoke_callback(
                        callback, std::span<const std::uint64_t>(arguments)));
                }
            }
            if (module.entry_rva() != 0 &&
                caller.invoke_callback(module.entry(),
                    std::span<const std::uint64_t>(arguments)) == 0) {
                // Windows sends PROCESS_DETACH when DllMain returns FALSE.
                const std::array detach{
                    module.base(), std::uint64_t{DLL_PROCESS_DETACH},
                    std::uint64_t{0}};
                try {
                    static_cast<void>(caller.invoke_callback(module.entry(),
                        std::span<const std::uint64_t>(detach)));
                } catch (...) {
                    // Preserve the attach failure.
                }
                throw std::system_error(
                    std::error_code(ERROR_DLL_INIT_FAILED,
                                    std::system_category()),
                    "guest DLL rejected process attach");
            }
            attached.push_back(&module);
        }
        for (auto* module : plan.attachment_order) {
            registered.push_back(module);
            register_guest_module(*module);
        }
        caller.commit_staged_modules();
        process.adopt_dynamic_load(std::move(plan));
        end_dynamic_module_initialization();
        return base;
    } catch (...) {
        const auto original = std::current_exception();
        for (auto* module : registered) unregister_guest_module(*module);
        for (auto entry = attached.rbegin(); entry != attached.rend(); ++entry) {
            const std::array detach{
                (*entry)->base(), std::uint64_t{DLL_PROCESS_DETACH},
                std::uint64_t{0}};
            try {
                if ((*entry)->tls()) {
                    for (const auto callback : (*entry)->tls()->callbacks) {
                        static_cast<void>(caller.invoke_callback(
                            callback, std::span<const std::uint64_t>(detach)));
                    }
                }
                if ((*entry)->entry_rva() != 0) {
                    static_cast<void>(caller.invoke_callback((*entry)->entry(),
                        std::span<const std::uint64_t>(detach)));
                }
            } catch (...) {
                // Continue rolling back all previously attached modules.
            }
        }
        for (std::size_t index = tls.size(); index != 0; --index) {
            try {
                rollback_dynamic_tls(caller, *plan.attachment_order[index - 1],
                                     tls[index - 1]);
            } catch (...) {
                // Preserve the primary initialization failure.
            }
        }
        caller.discard_staged_modules();
        end_dynamic_module_initialization();
        std::rethrow_exception(original);
    }
}

void Win32Bridge::initialize_thread_environment(ReferenceEngine& engine,
                                       std::uint64_t image_base) {
    constexpr std::uint64_t shared_base = 0x000000007ffe0000ULL;
    constexpr std::uint64_t peb_base = 0x00007ffe00000000ULL;
    constexpr std::uint64_t teb_base = 0x00007fff00000000ULL;
    engine.map_memory(shared_base, 4096);
    engine.map_memory(peb_base, 4096);
    engine.map_memory(teb_base, 8192);
    engine.map_memory(stdio_base, 4096);
    engine.map_memory(locale_base, 4096);
    engine.map_memory(ctype_base, 4096);
    engine.map_memory(locale_names_base, 4096);
    engine.map_memory(command_line_base, 131072);
    auto& memory = static_cast<GuestMemory&>(engine);
    for (std::size_t index = 0; index < state_->command_line_w.size(); ++index) {
        const auto character = static_cast<std::uint16_t>(state_->command_line_w[index]);
        memory.write8(command_line_base + index * 2,
                      static_cast<std::uint8_t>(character));
        memory.write8(command_line_base + index * 2 + 1,
                      static_cast<std::uint8_t>(character >> 8));
    }
    for (std::size_t index = 0; index < state_->command_line_a.size(); ++index) {
        memory.write8(command_line_base + 65536 + index,
                      static_cast<std::uint8_t>(state_->command_line_a[index]));
    }
    const auto [stack_limit, stack_base] = engine.stack_bounds();
    memory.write64(teb_base, UINT64_MAX); // NT_TIB.ExceptionList
    memory.write64(teb_base + 0x08, stack_base);
    memory.write64(teb_base + 0x10, stack_limit);
    memory.write64(teb_base + 0x30, teb_base); // NT_TIB.Self
    memory.write64(teb_base + 0x40, GetCurrentProcessId());
    memory.write64(teb_base + 0x48, GetCurrentThreadId());
    memory.write64(teb_base + 0x60, peb_base);
    memory.write64(peb_base + 0x10, image_base);
    memory.write8(shared_base + 0x26a, PROCESSOR_ARCHITECTURE_ARM64);
    memory.write32(shared_base + 0x26c, 10);
    memory.write32(shared_base + 0x270, 0);
    memory.write8(shared_base + 0x274 + PF_ARM_V8_INSTRUCTIONS_AVAILABLE, 1);
    memory.write8(shared_base + 0x274 + PF_ARM_NEON_INSTRUCTIONS_AVAILABLE, 1);
    engine.set_memory_writable(shared_base, false);
    engine.set_initial_register(18, teb_base);
    {
        std::lock_guard lock(state_->mutex);
        state_->guest_thread_slots[GetCurrentThreadId()] = 0;
    }
}

void Win32Bridge::initialize_worker_thread_environment(ReferenceEngine& engine,
                                                       std::uint64_t image_base) {
    constexpr std::uint64_t peb_base = 0x00007ffe00000000ULL;
    constexpr std::uint64_t first_teb = 0x00007fff00000000ULL;
    constexpr std::size_t maximum_slots = 1024;
    std::lock_guard loader_lock(state_->dynamic_dll_mutex);
    std::unique_lock lock(state_->mutex);
    if (state_->guest_thread_slots.contains(GetCurrentThreadId())) {
        throw std::runtime_error("host thread already has a guest environment");
    }
    const auto slot = state_->free_guest_thread_slots.empty()
        ? state_->next_guest_thread_slot : state_->free_guest_thread_slots.back();
    if (slot >= maximum_slots) {
        throw std::runtime_error("too many concurrent guest threads");
    }
    auto& mappings = state_->guest_thread_mappings[slot];
    mappings.reserve(state_->registration_order.size() + 2);
    state_->guest_thread_slots.emplace(GetCurrentThreadId(), slot);
    if (state_->free_guest_thread_slots.empty()) ++state_->next_guest_thread_slot;
    else state_->free_guest_thread_slots.pop_back();
    try {
        const auto teb = first_teb + slot * 0x10000;
        const auto tls_array = tls_array_base + slot * 0x10000;
        engine.map_memory(teb, 8192);
        mappings.push_back(teb);
        engine.map_memory(tls_array, maximum_slots * 8);
        mappings.push_back(tls_array);
        auto& memory = static_cast<GuestMemory&>(engine);
        const auto [stack_limit, stack_base] = engine.stack_bounds();
        memory.write64(teb, UINT64_MAX);
        memory.write64(teb + 0x08, stack_base);
        memory.write64(teb + 0x10, stack_limit);
        memory.write64(teb + 0x30, teb);
        memory.write64(teb + 0x40, GetCurrentProcessId());
        memory.write64(teb + 0x48, GetCurrentThreadId());
        memory.write64(teb + 0x58, tls_array);
        memory.write64(teb + 0x60, peb_base);
        memory.write64(peb_base + 0x10, image_base);
        for (const auto* module : state_->registration_order) {
            if (!module->tls()) continue;
            const auto& tls = *module->tls();
            const auto index = memory.read32(tls.index_address);
            if (index >= maximum_slots) {
                throw std::runtime_error("invalid guest static TLS module index");
            }
            const auto requested = tls.initial_data.size() + tls.zero_fill;
            const auto size = (std::max<std::size_t>(requested, 1) + 15) &
                              ~std::size_t{15};
            const auto address = state_->next_tls_data_address;
            if (address > UINT64_MAX - size) {
                throw std::runtime_error("guest static TLS address space exhausted");
            }
            memory.map_memory(address, size);
            mappings.push_back(address);
            state_->next_tls_data_address = address + size;
            for (std::size_t byte = 0; byte < tls.initial_data.size(); ++byte) {
                memory.write8(address + byte, tls.initial_data[byte]);
            }
            memory.write64(tls_array + static_cast<std::uint64_t>(index) * 8,
                           address);
        }
        engine.set_initial_register(18, teb);
    } catch (...) {
        release_worker_thread_environment(engine);
        throw;
    }
    lock.unlock();
    try {
        notify_thread_libraries(engine, DLL_THREAD_ATTACH);
    } catch (...) {
        release_worker_thread_values(engine);
        throw;
    }
}

void Win32Bridge::release_worker_thread_environment(ReferenceEngine& engine) {
    const auto found = state_->guest_thread_slots.find(GetCurrentThreadId());
    if (found == state_->guest_thread_slots.end() || found->second == 0) return;
    const auto slot = found->second;
    if (const auto mappings = state_->guest_thread_mappings.find(slot);
        mappings != state_->guest_thread_mappings.end()) {
        for (auto address = mappings->second.rbegin();
             address != mappings->second.rend(); ++address) {
            engine.unmap_memory(*address);
        }
        state_->guest_thread_mappings.erase(mappings);
    }
    state_->guest_thread_slots.erase(found);
    state_->free_guest_thread_slots.push_back(slot);
}

void Win32Bridge::notify_thread_libraries(
    ReferenceEngine& engine, std::uint32_t reason,
    std::uint64_t completed_tls_callback) {
    // The caller owns dynamic_dll_mutex for the whole notification batch.
    std::vector<const PeImage*> modules;
    std::unordered_set<std::uint64_t> disabled;
    {
        std::lock_guard lock(state_->mutex);
        modules = state_->registration_order;
        disabled = state_->thread_notifications_disabled;
        state_->dll_notification_thread = GetCurrentThreadId();
    }
    struct NotificationScope {
        State& state;
        ~NotificationScope() {
            std::lock_guard lock(state.mutex);
            state.dll_notification_thread = 0;
        }
    } notification{*state_};
    const auto call_tls = [&](const PeImage& module) {
        if (!module.tls()) return;
        const std::array arguments{
            module.base(), std::uint64_t{reason}, std::uint64_t{0}};
        for (const auto callback : module.tls()->callbacks) {
            if (!module.is_dll() && callback == completed_tls_callback) continue;
            static_cast<void>(engine.invoke_callback(
                callback, std::span<const std::uint64_t>(arguments)));
        }
    };
    const auto call_dll = [&](const PeImage& module) {
        if (!module.is_dll() || disabled.contains(module.base())) return;
        call_tls(module);
        if (module.entry_rva() != 0) {
            const std::array arguments{
                module.base(), std::uint64_t{reason}, std::uint64_t{0}};
            // Only PROCESS_ATTACH uses DllMain's Boolean result.
            static_cast<void>(engine.invoke_callback(
                module.entry(), std::span<const std::uint64_t>(arguments)));
        }
    };
    if (reason == DLL_THREAD_ATTACH) {
        for (const auto* module : modules) call_dll(*module);
    } else {
        for (auto module = modules.rbegin(); module != modules.rend(); ++module) {
            call_dll(**module);
        }
    }
    for (const auto* module : modules) {
        if (!module->is_dll()) call_tls(*module);
    }
}

void Win32Bridge::initialize_static_tls(ReferenceEngine& engine) {
    constexpr std::size_t maximum_slots = 1024;
    constexpr std::uint64_t teb_base = 0x00007fff00000000ULL;
    auto& memory = static_cast<GuestMemory&>(engine);
    memory.map_memory(tls_array_base, maximum_slots * 8);
    memory.write64(teb_base + 0x58, tls_array_base);
    for (const auto* module : state_->registration_order) {
        if (!module->tls()) continue;
        if (state_->next_tls_index >= maximum_slots) {
            throw std::runtime_error("too many guest static TLS modules");
        }
        const auto& tls = *module->tls();
        const auto requested = tls.initial_data.size() + tls.zero_fill;
        const auto size = (std::max<std::size_t>(requested, 1) + 15) & ~std::size_t{15};
        const auto address = state_->next_tls_data_address;
        if (address > UINT64_MAX - size) {
            throw std::runtime_error("guest static TLS address space exhausted");
        }
        memory.map_memory(address, size);
        state_->guest_thread_mappings[0].push_back(address);
        for (std::size_t index = 0; index < tls.initial_data.size(); ++index) {
            memory.write8(address + index, tls.initial_data[index]);
        }
        memory.write32(tls.index_address, state_->next_tls_index);
        memory.write64(tls_array_base + state_->next_tls_index * 8ULL, address);
        state_->next_tls_data_address = address + size;
        ++state_->next_tls_index;
    }
}

Win32Bridge::DynamicTlsRegistration Win32Bridge::initialize_dynamic_tls(
    ReferenceEngine& engine, const PeImage& module) {
    DynamicTlsRegistration registration;
    if (!module.tls()) return registration;
    constexpr std::size_t maximum_slots = 1024;
    std::lock_guard lock(state_->mutex);
    if (state_->next_tls_index >= maximum_slots) {
        throw std::runtime_error("too many guest static TLS modules");
    }
    const auto& tls = *module.tls();
    auto& memory = static_cast<GuestMemory&>(engine);
    registration.index = state_->next_tls_index;
    registration.previous_index = memory.read32(tls.index_address);
    registration.active = true;
    const auto requested = tls.initial_data.size() + tls.zero_fill;
    const auto size = (std::max<std::size_t>(requested, 1) + 15) &
                      ~std::size_t{15};
    registration.slots.reserve(state_->guest_thread_slots.size());
    try {
        for (const auto& [thread_id, slot] : state_->guest_thread_slots) {
            static_cast<void>(thread_id);
            const auto tls_array = tls_array_base + slot * 0x10000;
            const auto region = memory.query_region(tls_array);
            if (!region) continue; // A finished thread's TEB may be unmapped.
            if (!region->writable ||
                tls_array + maximum_slots * 8 > region->base + region->size) {
                throw std::runtime_error("invalid guest static TLS array");
            }
            const auto address = state_->next_tls_data_address;
            if (address > UINT64_MAX - size) {
                throw std::runtime_error("guest static TLS address space exhausted");
            }
            auto& mappings = state_->guest_thread_mappings[slot];
            mappings.reserve(mappings.size() + 1);
            memory.map_memory(address, size);
            state_->next_tls_data_address = address + size;
            registration.slots.emplace_back(tls_array, address);
            mappings.push_back(address);
            for (std::size_t byte = 0; byte < tls.initial_data.size(); ++byte) {
                memory.write8(address + byte, tls.initial_data[byte]);
            }
            memory.write64(tls_array + registration.index * 8ULL, address);
        }
        if (registration.slots.empty()) {
            throw std::runtime_error("guest static TLS arrays are not initialized");
        }
        memory.write32(tls.index_address, registration.index);
        ++state_->next_tls_index;
        return registration;
    } catch (...) {
        rollback_dynamic_tls(engine, module, registration);
        throw;
    }
}

void Win32Bridge::rollback_dynamic_tls(
    ReferenceEngine& engine, const PeImage& module,
    DynamicTlsRegistration& registration) {
    if (!registration.active || !module.tls()) return;
    std::lock_guard lock(state_->mutex);
    auto& memory = static_cast<GuestMemory&>(engine);
    for (const auto& [tls_array, address] : registration.slots) {
        const auto slot = (tls_array - tls_array_base) / 0x10000;
        const auto mappings = state_->guest_thread_mappings.find(slot);
        if (mappings == state_->guest_thread_mappings.end()) continue;
        const auto owned = std::find(mappings->second.begin(),
                                     mappings->second.end(), address);
        if (owned == mappings->second.end()) continue;
        // A completed thread may have released this registration, then its
        // slot may have been reused. Never clear the replacement's pointer.
        if (memory.read64(tls_array + registration.index * 8ULL) == address) {
            memory.write64(tls_array + registration.index * 8ULL, 0);
        }
        engine.unmap_memory(address);
        mappings->second.erase(owned);
    }
    memory.write32(module.tls()->index_address, registration.previous_index);
    if (state_->next_tls_index == registration.index + 1) --state_->next_tls_index;
    registration.active = false;
}

void Win32Bridge::set_guest_callback_invoker(
    std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoker) {
    std::lock_guard lock(state_->mutex);
    state_->guest_callback = std::move(invoker);
}

void Win32Bridge::set_guest_dll_loader(
    std::function<std::uint64_t(const std::filesystem::path&,
        const std::vector<std::filesystem::path>&, bool, ReferenceEngine&)>
        loader) {
    std::lock_guard lock(state_->mutex);
    state_->guest_dll_loader = std::move(loader);
}

void Win32Bridge::set_guest_tls_callback_invoker(
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t)> invoker) {
    std::lock_guard lock(state_->mutex);
    state_->guest_tls_callback = std::move(invoker);
}

void Win32Bridge::set_guest_window_callback_invoker(
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t,
                                std::uint64_t)> invoker) {
    std::lock_guard lock(state_->mutex);
    state_->guest_window_callback = std::move(invoker);
}

void Win32Bridge::set_guest_binary_callback_invoker(
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t)> invoker) {
    std::lock_guard lock(state_->mutex);
    state_->guest_binary_callback = std::move(invoker);
}

std::intptr_t Win32Bridge::host_window_message(
    void* native_window, unsigned message, std::uintptr_t wparam,
    std::intptr_t lparam, void* opaque) noexcept {
    auto* context = static_cast<GuestWindowContext*>(opaque);
    const auto window = static_cast<HWND>(native_window);
    if (context->native_proc != nullptr) {
        const auto result = ::CallWindowProcW(context->native_proc, window, message, wparam, lparam);
        if (message == WM_NCDESTROY) {
            std::lock_guard lock(state_->mutex);
            state_->host_windows.erase(window);
            state_->windows.erase(context->guest_handle);
        }
        return result;
    }
    if (context->shutting_down) {
        const auto result = ::DefWindowProcW(window, message, wparam, lparam);
        if (message == WM_NCDESTROY) {
            std::lock_guard lock(state_->mutex);
            state_->host_windows.erase(window);
            state_->windows.erase(context->guest_handle);
        }
        return result;
    }
    try {
        if (message == WM_NCCREATE) {
            std::lock_guard lock(state_->mutex);
            context->host = window;
            state_->host_windows[window] = context->guest_handle;
            state_->windows.emplace(context->guest_handle, context->shared_from_this());
        }
        if (!context->invoke) {
            throw std::runtime_error("guest window callback invoker is unavailable");
        }
        std::uint64_t guest_lparam = static_cast<std::uint64_t>(lparam);
        if (message == WM_NCCREATE || message == WM_CREATE) {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto& memory = *context->memory;
            const auto at = context->scratch;
            memory.write64(at, context->guest_parameter);
            memory.write64(at + 8, context->guest_instance);
            memory.write64(at + 16, context->guest_menu);
            memory.write64(at + 24, context->guest_parent);
            memory.write32(at + 32, static_cast<std::uint32_t>(creation->cy));
            memory.write32(at + 36, static_cast<std::uint32_t>(creation->cx));
            memory.write32(at + 40, static_cast<std::uint32_t>(creation->y));
            memory.write32(at + 44, static_cast<std::uint32_t>(creation->x));
            memory.write32(at + 48, static_cast<std::uint32_t>(creation->style));
            memory.write32(at + 52, 0);
            memory.write64(at + 56, context->guest_window_name);
            memory.write64(at + 64, context->guest_class_name);
            memory.write32(at + 72, creation->dwExStyle);
            memory.write32(at + 76, 0);
            guest_lparam = at;
        } else if (message == WM_GETMINMAXINFO) {
            auto& memory = *context->memory;
            constexpr std::uint64_t offset = 128;
            for (std::uint64_t index = 0; index < sizeof(MINMAXINFO); ++index) {
                memory.write8(context->scratch + offset + index,
                    reinterpret_cast<const std::uint8_t*>(lparam)[index]);
            }
            guest_lparam = context->scratch + offset;
        } else {
            switch (message) {
            case WM_CLOSE: case WM_DESTROY: case WM_NCDESTROY:
            case WM_PAINT: case WM_SIZE: case WM_MOVE: case WM_SHOWWINDOW:
            case WM_ACTIVATE: case WM_SETFOCUS: case WM_KILLFOCUS:
            case WM_ENABLE: case WM_QUERYENDSESSION: case WM_ENDSESSION:
            case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
            case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
            case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSELEAVE:
            case WM_TIMER: case WM_ENTERSIZEMOVE: case WM_EXITSIZEMOVE:
                break;
            default:
                if (message < WM_USER) return ::DefWindowProcW(window, message, wparam, lparam);
                break;
            }
        }
        const auto previous = context->active_host_lparam;
        context->active_host_lparam = lparam;
        std::uint64_t result{};
        try {
            result = context->invoke(context->guest_proc, context->guest_handle,
                            message, wparam, guest_lparam);
        } catch (...) {
            context->active_host_lparam = previous;
            throw;
        }
        context->active_host_lparam = previous;
        if (message == WM_GETMINMAXINFO) {
            auto& memory = *context->memory;
            constexpr std::uint64_t offset = 128;
            for (std::uint64_t index = 0; index < sizeof(MINMAXINFO); ++index) {
                reinterpret_cast<std::uint8_t*>(lparam)[index] =
                    memory.read8(context->scratch + offset + index);
            }
        }
        if (message == WM_NCDESTROY) {
            if (context->scratch_mapped) {
                context->memory->unmap_memory(context->scratch);
                context->scratch_mapped = false;
            }
            std::lock_guard lock(state_->mutex);
            state_->host_windows.erase(window);
            state_->windows.erase(context->guest_handle);
        }
        return static_cast<std::intptr_t>(result);
    } catch (const std::exception& error) {
        std::lock_guard lock(state_->mutex);
        state_->window_failures[GetCurrentThreadId()] = error.what();
        return message == WM_NCCREATE ? FALSE : 0;
    } catch (...) {
        std::lock_guard lock(state_->mutex);
        state_->window_failures[GetCurrentThreadId()] =
            "guest window callback failed";
        return message == WM_NCCREATE ? FALSE : 0;
    }
}

void Win32Bridge::cleanup_process_exit() {
    std::uint64_t callback{};
    std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                std::uint64_t, std::uint64_t)> invoke;
    {
        std::lock_guard lock(state_->mutex);
        if (state_->process_exit_callbacks_suppressed) return;
        callback = state_->thread_local_exe_atexit_callback;
        invoke = state_->guest_tls_callback;
        if (callback != 0 && !invoke) {
            throw std::runtime_error(
                "guest CRT thread-local exit callback invoker is unavailable");
        }
        state_->thread_local_exe_atexit_callback = 0;
    }
    if (callback != 0) invoke(callback, 0, DLL_PROCESS_DETACH, 0);
}

void Win32Bridge::release_worker_thread_values(ReferenceEngine& engine) {
    // Locale imports take locale_mutex before State::mutex when setting errno.
    std::lock_guard locale_lock(state_->locale_mutex);
    std::lock_guard lock(state_->mutex);
    const auto thread = GetCurrentThreadId();
    state_->crt_rand_states.erase(thread);
    state_->strtok_cursors.erase(thread);
    if (const auto found = state_->crt_error_buffers.find(thread);
        found != state_->crt_error_buffers.end()) {
        state_->free_crt_error_buffers.push_back(found->second);
        state_->crt_error_buffers.erase(found);
    }
    state_->crt_thread_invalid_parameter_handlers.erase(thread);
    if (const auto found = state_->crt_errno_by_thread.find(thread);
        found != state_->crt_errno_by_thread.end()) {
        if (found->second.address != 0) engine.unmap_memory(found->second.address);
        state_->crt_errno_by_thread.erase(found);
    }
    for (auto& [index, values] : state_->tls_values) {
        static_cast<void>(index);
        values.erase(thread);
    }
    for (auto& [index, slot] : state_->fls_slots) {
        static_cast<void>(index);
        slot.values.erase(thread); // A callback may have set a fresh value.
    }
    if (const auto found = state_->lconv_pages.find(thread);
        found != state_->lconv_pages.end()) {
        engine.unmap_memory(found->second);
        state_->lconv_pages.erase(found);
    }
    if (const auto found = state_->original_host_locale_modes.find(thread);
        found != state_->original_host_locale_modes.end()) {
        static_cast<void>(_configthreadlocale(found->second));
        state_->original_host_locale_modes.erase(found);
    }
    state_->thread_locale_modes.erase(thread);
    state_->winsock_errors.erase(thread);
    if (const auto found = state_->inet_ntoa_strings.find(thread);
        found != state_->inet_ntoa_strings.end()) {
        engine.unmap_memory(found->second);
        state_->inet_ntoa_strings.erase(found);
    }
    state_->current_cursors.erase(thread);
    state_->window_failures.erase(thread);
    release_worker_thread_environment(engine);
}

void Win32Bridge::cleanup_worker_thread(ReferenceEngine& engine) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> callbacks;
    std::uint64_t crt_tls_callback{};
    {
        std::lock_guard lock(state_->mutex);
        const auto thread = GetCurrentThreadId();
        for (auto& [index, slot] : state_->fls_slots) {
            static_cast<void>(index);
            const auto found = slot.values.find(thread);
            if (found == slot.values.end()) continue;
            if (slot.callback != 0 && found->second != 0) {
                callbacks.emplace_back(slot.callback, found->second);
            }
            slot.values.erase(found);
        }
        crt_tls_callback = state_->thread_local_exe_atexit_callback;
    }
    const auto release_thread_values = [this, &engine] {
        std::lock_guard loader_lock(state_->dynamic_dll_mutex);
        release_worker_thread_values(engine);
    };
    const auto release_com = [this] {
        State::ComApartment apartment{};
        {
            std::lock_guard lock(state_->mutex);
            if (const auto found = state_->com_apartments.find(
                    GetCurrentThreadId());
                found != state_->com_apartments.end()) {
                apartment = found->second;
                state_->com_apartments.erase(found);
            }
        }
        for (std::uint32_t index = 0; index < apartment.ole_depth; ++index) {
            OleUninitialize();
        }
        for (std::uint32_t index = 0; index < apartment.co_depth; ++index) {
            CoUninitialize();
        }
    };
    try {
        for (const auto& [callback, value] : callbacks) {
            engine.invoke_callback(callback, value);
        }
        if (crt_tls_callback != 0) {
            const std::array arguments{
                std::uint64_t{0}, std::uint64_t{DLL_THREAD_DETACH},
                std::uint64_t{0}};
            engine.invoke_callback(crt_tls_callback,
                                   std::span<const std::uint64_t>(arguments));
        }
        {
            std::lock_guard loader_lock(state_->dynamic_dll_mutex);
            notify_thread_libraries(engine, DLL_THREAD_DETACH, crt_tls_callback);
        }
    } catch (...) {
        release_com();
        release_thread_values();
        throw;
    }
    release_com();
    release_thread_values();
}

void Win32Bridge::check_worker_failures() {
    std::vector<std::shared_ptr<GuestThreadRecord>> workers;
    {
        std::lock_guard lock(state_->mutex);
        workers = state_->guest_threads;
    }
    for (const auto& worker : workers) {
        std::lock_guard lock(worker->mutex);
        if (!worker->failure.empty()) {
            throw std::runtime_error("guest worker thread failed: " +
                                     worker->failure);
        }
    }
}

std::optional<std::uint32_t> Win32Bridge::dispatch_registry(
    const Import& imported, CpuState& state, GuestMemory& memory) {
    const auto& function = imported.function;
    std::unique_lock lock(state_->mutex);
    const auto finish = [&state](LSTATUS status) -> std::optional<std::uint32_t> {
        state.x[0] = static_cast<std::uint32_t>(status);
        return std::nullopt;
    };
    const auto key_for = [this](std::uint64_t guest) -> HKEY {
        const auto upper = guest >> 32;
        if (upper == 0 || upper == UINT32_MAX) {
            switch (static_cast<std::uint32_t>(guest)) {
            case 0x80000000U: return HKEY_CLASSES_ROOT;
            case 0x80000001U: return HKEY_CURRENT_USER;
            case 0x80000002U: return HKEY_LOCAL_MACHINE;
            case 0x80000003U: return HKEY_USERS;
            case 0x80000004U: return HKEY_PERFORMANCE_DATA;
            case 0x80000005U: return HKEY_CURRENT_CONFIG;
            default: break;
            }
        }
        const auto found = state_->registry_handles.find(guest);
        return found == state_->registry_handles.end() ? nullptr : found->second;
    };
    const auto key = key_for((function == "RegConnectRegistryW" ||
                              function == "RegConnectRegistryA")
                                 ? state.x[1] : state.x[0]);
    if (!key) return finish(ERROR_INVALID_HANDLE);
    const auto store_key = [this, &memory](std::uint64_t output, HKEY native,
                                           std::uint64_t original,
                                           HKEY original_native) {
        if (native == original_native &&
            (original >> 32 == 0 || original >> 32 == UINT32_MAX) &&
            (static_cast<std::uint32_t>(original) & 0xffffff00U) ==
                0x80000000U) {
            memory.write64(output, original);
            return;
        }
        const auto token = state_->next_registry_handle;
        state_->next_registry_handle += 8;
        state_->registry_handles.emplace(token, native);
        memory.write64(output, token);
    };
    const auto optional_name = [&memory](std::uint64_t pointer)
        -> std::optional<std::wstring> {
        if (!pointer) return std::nullopt;
        return guest_string(memory, pointer, true);
    };
    const auto optional_ansi = [&memory](std::uint64_t pointer)
        -> std::optional<std::string> {
        if (!pointer) return std::nullopt;
        return guest_ansi_string(memory, pointer);
    };
    const auto read_bytes = [&memory](std::uint64_t address, DWORD length) {
        std::vector<BYTE> bytes(length);
        copy_from_guest(memory, address, bytes);
        return bytes;
    };
    const auto write_bytes = [&memory](std::uint64_t address,
                                        const BYTE* bytes, DWORD length) {
        copy_to_guest(memory, address, {bytes, length});
    };
    const auto write_wide_char = [&memory](std::uint64_t address,
                                            wchar_t character) {
        memory.write8(address, static_cast<std::uint8_t>(character));
        memory.write8(address + 1,
                      static_cast<std::uint8_t>(character >> 8));
    };
    try {
        if (function == "RegCloseKey") {
            const auto found = state_->registry_handles.find(state.x[0]);
            if (found == state_->registry_handles.end()) {
                // Predefined keys are process-owned and do not require closing.
                return finish(ERROR_SUCCESS);
            }
            const auto status = RegCloseKey(found->second);
            if (status == ERROR_SUCCESS) state_->registry_handles.erase(found);
            return finish(status);
        }
        if (function == "RegFlushKey") return finish(RegFlushKey(key));
        if (function == "RegGetValueW" || function == "RegGetValueA") {
            const bool wide = function == "RegGetValueW";
            const auto size_pointer = state.x[6];
            if ((state.x[5] != 0 && size_pointer == 0) ||
                (size_pointer != 0 &&
                 !guest_range(memory, size_pointer, 4, true)) ||
                (state.x[4] != 0 &&
                 !guest_range(memory, state.x[4], 4, true)))
                return finish(ERROR_NOACCESS);
            DWORD size = size_pointer ? memory.read32(size_pointer) : 0;
            if (size > 1024 * 1024) return finish(ERROR_INVALID_PARAMETER);
            if (state.x[5] != 0 && size != 0 &&
                !guest_range(memory, state.x[5], size, true))
                return finish(ERROR_NOACCESS);
            std::vector<BYTE> bytes(state.x[5] == 0 ? 0 :
                                    std::max<DWORD>(1, size));
            DWORD type = 0;
            LSTATUS status = ERROR_SUCCESS;
            if (wide) {
                const auto subkey = optional_name(state.x[1]);
                const auto value = optional_name(state.x[2]);
                status = RegGetValueW(key,
                    subkey ? subkey->c_str() : nullptr,
                    value ? value->c_str() : nullptr,
                    static_cast<DWORD>(state.x[3]),
                    state.x[4] ? &type : nullptr,
                    state.x[5] ? bytes.data() : nullptr,
                    size_pointer ? &size : nullptr);
            } else {
                const auto subkey = optional_ansi(state.x[1]);
                const auto value = optional_ansi(state.x[2]);
                status = RegGetValueA(key,
                    subkey ? subkey->c_str() : nullptr,
                    value ? value->c_str() : nullptr,
                    static_cast<DWORD>(state.x[3]),
                    state.x[4] ? &type : nullptr,
                    state.x[5] ? bytes.data() : nullptr,
                    size_pointer ? &size : nullptr);
            }
            if (state.x[4] != 0) memory.write32(state.x[4], type);
            if (size_pointer != 0) memory.write32(size_pointer, size);
            if (state.x[5] != 0 && !bytes.empty() &&
                (status == ERROR_SUCCESS ||
                 (static_cast<DWORD>(state.x[3]) & RRF_ZEROONFAILURE) != 0)) {
                write_bytes(state.x[5], bytes.data(), status == ERROR_SUCCESS
                    ? std::min<DWORD>(size,
                        static_cast<DWORD>(bytes.size()))
                    : static_cast<DWORD>(bytes.size()));
            }
            return finish(status);
        }
        if (function == "RegDeleteTreeW" || function == "RegDeleteTreeA") {
            if (function == "RegDeleteTreeW") {
                const auto subkey = optional_name(state.x[1]);
                return finish(RegDeleteTreeW(key,
                    subkey ? subkey->c_str() : nullptr));
            }
            const auto subkey = optional_ansi(state.x[1]);
            return finish(RegDeleteTreeA(key,
                subkey ? subkey->c_str() : nullptr));
        }
        if (function == "RegNotifyChangeKeyValue") {
            HANDLE event = nullptr;
            const bool asynchronous = state.x[4] != 0;
            if (asynchronous) {
                const auto found = state_->host_handles.find(state.x[3]);
                if (found == state_->host_handles.end())
                    return finish(ERROR_INVALID_HANDLE);
                event = found->second.value;
            }
            const auto subtree = state.x[1] != 0;
            const auto filter = static_cast<DWORD>(state.x[2]);
            if (asynchronous) {
                return finish(RegNotifyChangeKeyValue(key, subtree,
                    filter, event, TRUE));
            }
            // A synchronous wait must not hold the registry/handle map lock:
            // another guest worker may be the writer that wakes this call.
            HKEY watch_key = nullptr;
            const auto opened = RegOpenKeyExW(key, nullptr, 0,
                                              KEY_NOTIFY, &watch_key);
            if (opened != ERROR_SUCCESS) return finish(opened);
            lock.unlock();
            const auto status = RegNotifyChangeKeyValue(
                watch_key, subtree, filter, nullptr, FALSE);
            RegCloseKey(watch_key);
            return finish(status);
        }
        if (function == "RegOpenKeyExW" || function == "RegCreateKeyExW" ||
            function == "RegCreateKeyW") {
            const bool extended_create = function == "RegCreateKeyExW";
            const auto output = extended_create ? state.x[7]
                : function == "RegCreateKeyW" ? state.x[2] : state.x[4];
            if (!guest_range(memory, output, 8, true))
                return finish(ERROR_NOACCESS);
            const auto subkey = optional_name(state.x[1]);
            if (function != "RegOpenKeyExW" && !subkey)
                return finish(ERROR_INVALID_PARAMETER);
            HKEY created = nullptr;
            DWORD disposition = 0;
            LSTATUS status = ERROR_SUCCESS;
            if (function == "RegOpenKeyExW") {
                status = RegOpenKeyExW(key, subkey ? subkey->c_str() : nullptr,
                    static_cast<DWORD>(state.x[2]),
                    static_cast<REGSAM>(state.x[3]), &created);
            } else if (!extended_create) {
                status = RegCreateKeyW(key, subkey->c_str(), &created);
            } else {
                if (state.x[2] != 0) return finish(ERROR_INVALID_PARAMETER);
                const auto class_name = optional_name(state.x[3]);
                SECURITY_ATTRIBUTES security{};
                SECURITY_ATTRIBUTES* attributes = nullptr;
                if (state.x[6] != 0) {
                    if (!guest_range(memory, state.x[6], sizeof(security), false))
                        return finish(ERROR_NOACCESS);
                    if (memory.read32(state.x[6]) != sizeof(security))
                        return finish(ERROR_INVALID_PARAMETER);
                    if (memory.read64(state.x[6] + 8) != 0)
                        return finish(ERROR_NOT_SUPPORTED);
                    security.nLength = sizeof(security);
                    security.bInheritHandle = memory.read32(state.x[6] + 16);
                    attributes = &security;
                }
                if (state.sp == 0 || !guest_range(memory, state.sp, 8, false))
                    return finish(ERROR_NOACCESS);
                const auto disposition_output = memory.read64(state.sp);
                if (disposition_output != 0 &&
                    !guest_range(memory, disposition_output, 4, true))
                    return finish(ERROR_NOACCESS);
                status = RegCreateKeyExW(key, subkey->c_str(), 0,
                    class_name ? const_cast<wchar_t*>(class_name->c_str()) : nullptr,
                    static_cast<DWORD>(state.x[4]),
                    static_cast<REGSAM>(state.x[5]), attributes, &created,
                    disposition_output ? &disposition : nullptr);
                if (status == ERROR_SUCCESS && disposition_output)
                    memory.write32(disposition_output, disposition);
            }
            if (status == ERROR_SUCCESS)
                store_key(output, created, state.x[0], key);
            return finish(status);
        }
        if (function == "RegOpenKeyExA" || function == "RegCreateKeyExA" ||
            function == "RegCreateKeyA") {
            const bool extended_create = function == "RegCreateKeyExA";
            const auto output = extended_create ? state.x[7]
                : function == "RegCreateKeyA" ? state.x[2] : state.x[4];
            if (!guest_range(memory, output, 8, true))
                return finish(ERROR_NOACCESS);
            const auto subkey = optional_ansi(state.x[1]);
            if (function != "RegOpenKeyExA" && !subkey)
                return finish(ERROR_INVALID_PARAMETER);
            HKEY created = nullptr;
            DWORD disposition = 0;
            LSTATUS status = ERROR_SUCCESS;
            if (function == "RegOpenKeyExA") {
                status = RegOpenKeyExA(key, subkey ? subkey->c_str() : nullptr,
                    static_cast<DWORD>(state.x[2]),
                    static_cast<REGSAM>(state.x[3]), &created);
            } else if (!extended_create) {
                status = RegCreateKeyA(key, subkey->c_str(), &created);
            } else {
                if (state.x[2] != 0) return finish(ERROR_INVALID_PARAMETER);
                const auto class_name = optional_ansi(state.x[3]);
                SECURITY_ATTRIBUTES security{};
                SECURITY_ATTRIBUTES* attributes = nullptr;
                if (state.x[6] != 0) {
                    if (!guest_range(memory, state.x[6], sizeof(security), false))
                        return finish(ERROR_NOACCESS);
                    if (memory.read32(state.x[6]) != sizeof(security))
                        return finish(ERROR_INVALID_PARAMETER);
                    if (memory.read64(state.x[6] + 8) != 0)
                        return finish(ERROR_NOT_SUPPORTED);
                    security.nLength = sizeof(security);
                    security.bInheritHandle = memory.read32(state.x[6] + 16);
                    attributes = &security;
                }
                if (state.sp == 0 || !guest_range(memory, state.sp, 8, false))
                    return finish(ERROR_NOACCESS);
                const auto disposition_output = memory.read64(state.sp);
                if (disposition_output != 0 &&
                    !guest_range(memory, disposition_output, 4, true))
                    return finish(ERROR_NOACCESS);
                status = RegCreateKeyExA(key, subkey->c_str(), 0,
                    class_name ? const_cast<char*>(class_name->c_str()) : nullptr,
                    static_cast<DWORD>(state.x[4]),
                    static_cast<REGSAM>(state.x[5]), attributes, &created,
                    disposition_output ? &disposition : nullptr);
                if (status == ERROR_SUCCESS && disposition_output)
                    memory.write32(disposition_output, disposition);
            }
            if (status == ERROR_SUCCESS)
                store_key(output, created, state.x[0], key);
            return finish(status);
        }
        if (function == "RegSetValueExW") {
            if (state.x[2] != 0) return finish(ERROR_INVALID_PARAMETER);
            const auto name = optional_name(state.x[1]);
            const auto size = static_cast<DWORD>(state.x[5]);
            if (size > 1024 * 1024 ||
                (size != 0 && !guest_range(memory, state.x[4], size, false)))
                return finish(size > 1024 * 1024 ? ERROR_INVALID_PARAMETER
                                                : ERROR_NOACCESS);
            const auto bytes = read_bytes(state.x[4], size);
            return finish(RegSetValueExW(key,
                name ? name->c_str() : nullptr, 0,
                static_cast<DWORD>(state.x[3]),
                bytes.empty() ? nullptr : bytes.data(), size));
        }
        if (function == "RegSetValueExA") {
            if (state.x[2] != 0) return finish(ERROR_INVALID_PARAMETER);
            const auto name = optional_ansi(state.x[1]);
            const auto size = static_cast<DWORD>(state.x[5]);
            if (size > 1024 * 1024 ||
                (size != 0 && !guest_range(memory, state.x[4], size, false)))
                return finish(size > 1024 * 1024 ? ERROR_INVALID_PARAMETER
                                                : ERROR_NOACCESS);
            const auto bytes = read_bytes(state.x[4], size);
            return finish(RegSetValueExA(key,
                name ? name->c_str() : nullptr, 0,
                static_cast<DWORD>(state.x[3]),
                bytes.empty() ? nullptr : bytes.data(), size));
        }
        if (function == "RegQueryValueExW") {
            if (state.x[2] != 0) return finish(ERROR_INVALID_PARAMETER);
            const auto name = optional_name(state.x[1]);
            const auto size_pointer = state.x[5];
            if ((size_pointer == 0 && state.x[4] != 0) ||
                (size_pointer != 0 &&
                 !guest_range(memory, size_pointer, 4, true)) ||
                (state.x[3] != 0 && !guest_range(memory, state.x[3], 4, true)))
                return finish(ERROR_NOACCESS);
            DWORD size = size_pointer ? memory.read32(size_pointer) : 0;
            if (size > 1024 * 1024) return finish(ERROR_INVALID_PARAMETER);
            if (state.x[4] != 0 && size != 0 &&
                !guest_range(memory, state.x[4], size, true))
                return finish(ERROR_NOACCESS);
            std::vector<BYTE> bytes(state.x[4] == 0 ? 0
                                                    : std::max<DWORD>(1, size));
            DWORD type = 0;
            const auto status = RegQueryValueExW(key,
                name ? name->c_str() : nullptr, nullptr,
                state.x[3] ? &type : nullptr,
                state.x[4] ? bytes.data() : nullptr,
                size_pointer ? &size : nullptr);
            if (state.x[3]) memory.write32(state.x[3], type);
            if (size_pointer) memory.write32(size_pointer, size);
            if (status == ERROR_SUCCESS && state.x[4] != 0 && size != 0)
                write_bytes(state.x[4], bytes.data(), size);
            return finish(status);
        }
        if (function == "RegQueryValueExA") {
            if (state.x[2] != 0) return finish(ERROR_INVALID_PARAMETER);
            const auto name = optional_ansi(state.x[1]);
            const auto size_pointer = state.x[5];
            if ((size_pointer == 0 && state.x[4] != 0) ||
                (size_pointer != 0 &&
                 !guest_range(memory, size_pointer, 4, true)) ||
                (state.x[3] != 0 && !guest_range(memory, state.x[3], 4, true)))
                return finish(ERROR_NOACCESS);
            DWORD size = size_pointer ? memory.read32(size_pointer) : 0;
            if (size > 1024 * 1024) return finish(ERROR_INVALID_PARAMETER);
            if (state.x[4] != 0 && size != 0 &&
                !guest_range(memory, state.x[4], size, true))
                return finish(ERROR_NOACCESS);
            std::vector<BYTE> bytes(state.x[4] == 0 ? 0
                                                    : std::max<DWORD>(1, size));
            DWORD type = 0;
            const auto status = RegQueryValueExA(key,
                name ? name->c_str() : nullptr, nullptr,
                state.x[3] ? &type : nullptr,
                state.x[4] ? bytes.data() : nullptr,
                size_pointer ? &size : nullptr);
            if (state.x[3]) memory.write32(state.x[3], type);
            if (size_pointer) memory.write32(size_pointer, size);
            if (status == ERROR_SUCCESS && state.x[4] != 0 && size != 0)
                write_bytes(state.x[4], bytes.data(), size);
            return finish(status);
        }
        if (function == "RegEnumKeyExW") {
            if (state.x[4] != 0 || state.x[2] == 0 || state.x[3] == 0 ||
                (state.x[5] != 0 && state.x[6] == 0) ||
                !guest_range(memory, state.x[3], 4, true) ||
                (state.x[6] != 0 &&
                 !guest_range(memory, state.x[6], 4, true)) ||
                (state.x[7] != 0 &&
                 !guest_range(memory, state.x[7], sizeof(FILETIME), true)))
                return finish(ERROR_NOACCESS);
            DWORD name_length = memory.read32(state.x[3]);
            DWORD class_length = state.x[6] ? memory.read32(state.x[6]) : 0;
            if (name_length > 32768 || class_length > 32768)
                return finish(ERROR_INVALID_PARAMETER);
            if (!guest_range(memory, state.x[2],
                             static_cast<std::uint64_t>(name_length) * 2,
                             true) ||
                (state.x[5] != 0 &&
                 !guest_range(memory, state.x[5],
                              static_cast<std::uint64_t>(class_length) * 2,
                              true)))
                return finish(ERROR_NOACCESS);
            std::vector<wchar_t> name(std::max<DWORD>(1, name_length));
            std::vector<wchar_t> class_name(state.x[5]
                ? std::max<DWORD>(1, class_length) : 0);
            FILETIME modified{};
            const auto status = RegEnumKeyExW(key,
                static_cast<DWORD>(state.x[1]), name.data(), &name_length,
                nullptr, state.x[5] ? class_name.data() : nullptr,
                state.x[6] ? &class_length : nullptr,
                state.x[7] ? &modified : nullptr);
            memory.write32(state.x[3], name_length);
            if (state.x[6]) memory.write32(state.x[6], class_length);
            if (status == ERROR_SUCCESS) {
                for (DWORD index = 0; index <= name_length; ++index)
                    write_wide_char(state.x[2] + 2ULL * index, name[index]);
                if (state.x[5]) {
                    for (DWORD index = 0; index <= class_length; ++index)
                        write_wide_char(state.x[5] + 2ULL * index,
                                        class_name[index]);
                }
                if (state.x[7]) write_guest_structure(memory, state.x[7], modified);
            }
            return finish(status);
        }
        if (function == "RegEnumKeyExA") {
            if (state.x[4] != 0 || state.x[2] == 0 || state.x[3] == 0 ||
                (state.x[5] != 0 && state.x[6] == 0) ||
                !guest_range(memory, state.x[3], 4, true) ||
                (state.x[6] != 0 &&
                 !guest_range(memory, state.x[6], 4, true)) ||
                (state.x[7] != 0 &&
                 !guest_range(memory, state.x[7], sizeof(FILETIME), true)))
                return finish(ERROR_NOACCESS);
            DWORD name_length = memory.read32(state.x[3]);
            DWORD class_length = state.x[6] ? memory.read32(state.x[6]) : 0;
            if (name_length > 32768 || class_length > 32768)
                return finish(ERROR_INVALID_PARAMETER);
            if (!guest_range(memory, state.x[2], name_length, true) ||
                (state.x[5] != 0 &&
                 !guest_range(memory, state.x[5], class_length, true)))
                return finish(ERROR_NOACCESS);
            std::vector<char> name(std::max<DWORD>(1, name_length));
            std::vector<char> class_name(state.x[5]
                ? std::max<DWORD>(1, class_length) : 0);
            FILETIME modified{};
            const auto status = RegEnumKeyExA(key,
                static_cast<DWORD>(state.x[1]), name.data(), &name_length,
                nullptr, state.x[5] ? class_name.data() : nullptr,
                state.x[6] ? &class_length : nullptr,
                state.x[7] ? &modified : nullptr);
            memory.write32(state.x[3], name_length);
            if (state.x[6]) memory.write32(state.x[6], class_length);
            if (status == ERROR_SUCCESS) {
                write_bytes(state.x[2],
                    reinterpret_cast<const BYTE*>(name.data()),
                    name_length + 1);
                if (state.x[5]) write_bytes(state.x[5],
                    reinterpret_cast<const BYTE*>(class_name.data()),
                    class_length + 1);
                if (state.x[7]) write_guest_structure(memory, state.x[7], modified);
            }
            return finish(status);
        }
        if (function == "RegEnumValueW") {
            if (state.x[4] != 0 || state.x[2] == 0 || state.x[3] == 0 ||
                !guest_range(memory, state.x[3], 4, true) ||
                (state.x[5] != 0 &&
                 !guest_range(memory, state.x[5], 4, true)) ||
                (state.x[6] != 0 && (state.x[7] == 0 ||
                 !guest_range(memory, state.x[7], 4, true))))
                return finish(ERROR_NOACCESS);
            DWORD name_length = memory.read32(state.x[3]);
            DWORD data_length = state.x[7] ? memory.read32(state.x[7]) : 0;
            if (name_length > 32768 || data_length > 1024 * 1024)
                return finish(ERROR_INVALID_PARAMETER);
            if (!guest_range(memory, state.x[2],
                             static_cast<std::uint64_t>(name_length) * 2,
                             true) ||
                (state.x[6] != 0 && data_length != 0 &&
                 !guest_range(memory, state.x[6], data_length, true)))
                return finish(ERROR_NOACCESS);
            std::vector<wchar_t> name(std::max<DWORD>(1, name_length));
            std::vector<BYTE> data(state.x[6] == 0 ? 0
                : std::max<DWORD>(1, data_length));
            DWORD type = 0;
            const auto status = RegEnumValueW(key,
                static_cast<DWORD>(state.x[1]), name.data(), &name_length,
                nullptr, state.x[5] ? &type : nullptr,
                state.x[6] ? data.data() : nullptr,
                state.x[7] ? &data_length : nullptr);
            memory.write32(state.x[3], name_length);
            if (state.x[5]) memory.write32(state.x[5], type);
            if (state.x[7]) memory.write32(state.x[7], data_length);
            if (status == ERROR_SUCCESS) {
                for (DWORD index = 0; index <= name_length; ++index)
                    write_wide_char(state.x[2] + 2ULL * index, name[index]);
                if (state.x[6] != 0 && data_length != 0)
                    write_bytes(state.x[6], data.data(), data_length);
            }
            return finish(status);
        }
        if (function == "RegEnumValueA") {
            if (state.x[4] != 0 || state.x[2] == 0 || state.x[3] == 0 ||
                !guest_range(memory, state.x[3], 4, true) ||
                (state.x[5] != 0 &&
                 !guest_range(memory, state.x[5], 4, true)) ||
                (state.x[6] != 0 && (state.x[7] == 0 ||
                 !guest_range(memory, state.x[7], 4, true))))
                return finish(ERROR_NOACCESS);
            DWORD name_length = memory.read32(state.x[3]);
            DWORD data_length = state.x[7] ? memory.read32(state.x[7]) : 0;
            if (name_length > 32768 || data_length > 1024 * 1024)
                return finish(ERROR_INVALID_PARAMETER);
            if (!guest_range(memory, state.x[2], name_length, true) ||
                (state.x[6] != 0 && data_length != 0 &&
                 !guest_range(memory, state.x[6], data_length, true)))
                return finish(ERROR_NOACCESS);
            std::vector<char> name(std::max<DWORD>(1, name_length));
            std::vector<BYTE> data(state.x[6] == 0 ? 0
                : std::max<DWORD>(1, data_length));
            DWORD type = 0;
            const auto status = RegEnumValueA(key,
                static_cast<DWORD>(state.x[1]), name.data(), &name_length,
                nullptr, state.x[5] ? &type : nullptr,
                state.x[6] ? data.data() : nullptr,
                state.x[7] ? &data_length : nullptr);
            memory.write32(state.x[3], name_length);
            if (state.x[5]) memory.write32(state.x[5], type);
            if (state.x[7]) memory.write32(state.x[7], data_length);
            if (status == ERROR_SUCCESS) {
                write_bytes(state.x[2],
                    reinterpret_cast<const BYTE*>(name.data()),
                    name_length + 1);
                if (state.x[6] != 0 && data_length != 0)
                    write_bytes(state.x[6], data.data(), data_length);
            }
            return finish(status);
        }
        if (function == "RegQueryInfoKeyW") {
            if (state.x[3] != 0 ||
                (state.x[1] != 0 && state.x[2] == 0) ||
                (state.x[2] != 0 &&
                 !guest_range(memory, state.x[2], 4, true)) ||
                state.sp == 0 || !guest_range(memory, state.sp, 32, false))
                return finish(ERROR_NOACCESS);
            const std::array<std::uint64_t, 7> outputs{
                state.x[4], state.x[5], state.x[6], state.x[7],
                memory.read64(state.sp), memory.read64(state.sp + 8),
                memory.read64(state.sp + 16)};
            const auto time_output = memory.read64(state.sp + 24);
            for (const auto output : outputs) {
                if (output != 0 && !guest_range(memory, output, 4, true))
                    return finish(ERROR_NOACCESS);
            }
            if (time_output != 0 &&
                !guest_range(memory, time_output, sizeof(FILETIME), true))
                return finish(ERROR_NOACCESS);
            DWORD class_length = state.x[2] ? memory.read32(state.x[2]) : 0;
            if (class_length > 32768) return finish(ERROR_INVALID_PARAMETER);
            if (state.x[1] != 0 &&
                !guest_range(memory, state.x[1],
                             static_cast<std::uint64_t>(class_length) * 2,
                             true)) return finish(ERROR_NOACCESS);
            std::vector<wchar_t> class_name(state.x[1]
                ? std::max<DWORD>(1, class_length) : 0);
            std::array<DWORD, 7> values{};
            FILETIME modified{};
            const auto status = RegQueryInfoKeyW(key,
                state.x[1] ? class_name.data() : nullptr,
                state.x[2] ? &class_length : nullptr, nullptr,
                outputs[0] ? &values[0] : nullptr,
                outputs[1] ? &values[1] : nullptr,
                outputs[2] ? &values[2] : nullptr,
                outputs[3] ? &values[3] : nullptr,
                outputs[4] ? &values[4] : nullptr,
                outputs[5] ? &values[5] : nullptr,
                outputs[6] ? &values[6] : nullptr,
                time_output ? &modified : nullptr);
            if (state.x[2]) memory.write32(state.x[2], class_length);
            if (status == ERROR_SUCCESS) {
                if (state.x[1]) {
                    for (DWORD index = 0; index <= class_length; ++index)
                        write_wide_char(state.x[1] + 2ULL * index,
                                        class_name[index]);
                }
                for (std::size_t index = 0; index < outputs.size(); ++index) {
                    if (outputs[index]) memory.write32(outputs[index], values[index]);
                }
                if (time_output)
                    write_guest_structure(memory, time_output, modified);
            }
            return finish(status);
        }
        if (function == "RegQueryInfoKeyA") {
            if (state.x[3] != 0 ||
                (state.x[1] != 0 && state.x[2] == 0) ||
                (state.x[2] != 0 &&
                 !guest_range(memory, state.x[2], 4, true)) ||
                state.sp == 0 || !guest_range(memory, state.sp, 32, false))
                return finish(ERROR_NOACCESS);
            const std::array<std::uint64_t, 7> outputs{
                state.x[4], state.x[5], state.x[6], state.x[7],
                memory.read64(state.sp), memory.read64(state.sp + 8),
                memory.read64(state.sp + 16)};
            const auto time_output = memory.read64(state.sp + 24);
            for (const auto output : outputs) {
                if (output != 0 && !guest_range(memory, output, 4, true))
                    return finish(ERROR_NOACCESS);
            }
            if (time_output != 0 &&
                !guest_range(memory, time_output, sizeof(FILETIME), true))
                return finish(ERROR_NOACCESS);
            DWORD class_length = state.x[2] ? memory.read32(state.x[2]) : 0;
            if (class_length > 32768) return finish(ERROR_INVALID_PARAMETER);
            if (state.x[1] != 0 &&
                !guest_range(memory, state.x[1], class_length, true))
                return finish(ERROR_NOACCESS);
            std::vector<char> class_name(state.x[1]
                ? std::max<DWORD>(1, class_length) : 0);
            std::array<DWORD, 7> values{};
            FILETIME modified{};
            const auto status = RegQueryInfoKeyA(key,
                state.x[1] ? class_name.data() : nullptr,
                state.x[2] ? &class_length : nullptr, nullptr,
                outputs[0] ? &values[0] : nullptr,
                outputs[1] ? &values[1] : nullptr,
                outputs[2] ? &values[2] : nullptr,
                outputs[3] ? &values[3] : nullptr,
                outputs[4] ? &values[4] : nullptr,
                outputs[5] ? &values[5] : nullptr,
                outputs[6] ? &values[6] : nullptr,
                time_output ? &modified : nullptr);
            if (state.x[2]) memory.write32(state.x[2], class_length);
            if (status == ERROR_SUCCESS) {
                if (state.x[1]) write_bytes(state.x[1],
                    reinterpret_cast<const BYTE*>(class_name.data()),
                    class_length + 1);
                for (std::size_t index = 0; index < outputs.size(); ++index) {
                    if (outputs[index]) memory.write32(outputs[index], values[index]);
                }
                if (time_output)
                    write_guest_structure(memory, time_output, modified);
            }
            return finish(status);
        }
        if (function == "RegSetValueW") {
            if (state.x[3] == 0) return finish(ERROR_INVALID_PARAMETER);
            const auto subkey = optional_name(state.x[1]);
            const auto value = guest_string(memory, state.x[3], true);
            return finish(RegSetValueW(key,
                subkey ? subkey->c_str() : nullptr,
                static_cast<DWORD>(state.x[2]), value.c_str(),
                static_cast<DWORD>(state.x[4])));
        }
        if (function == "RegSetValueA") {
            if (state.x[3] == 0) return finish(ERROR_INVALID_PARAMETER);
            const auto subkey = optional_ansi(state.x[1]);
            const auto value = guest_ansi_string(memory, state.x[3]);
            return finish(RegSetValueA(key,
                subkey ? subkey->c_str() : nullptr,
                static_cast<DWORD>(state.x[2]), value.c_str(),
                static_cast<DWORD>(state.x[4])));
        }
        if (function == "RegQueryValueW") {
            const auto subkey = optional_name(state.x[1]);
            if (state.x[2] != 0 && state.x[3] == 0)
                return finish(ERROR_INVALID_PARAMETER);
            if (state.x[3] != 0 &&
                !guest_range(memory, state.x[3], 4, true))
                return finish(ERROR_NOACCESS);
            LONG size = state.x[3] ?
                static_cast<LONG>(memory.read32(state.x[3])) : 0;
            if (size < 0 || size > 1024 * 1024)
                return finish(ERROR_INVALID_PARAMETER);
            if (state.x[2] != 0 && size != 0 &&
                !guest_range(memory, state.x[2], size, true))
                return finish(ERROR_NOACCESS);
            std::vector<wchar_t> data(state.x[2] == 0 ? 0
                : std::max<LONG>(1, (size + 1) / 2));
            const auto status = RegQueryValueW(key,
                subkey ? subkey->c_str() : nullptr,
                state.x[2] ? data.data() : nullptr,
                state.x[3] ? &size : nullptr);
            if (state.x[3])
                memory.write32(state.x[3], static_cast<std::uint32_t>(size));
            if (status == ERROR_SUCCESS && state.x[2] && size > 0)
                write_bytes(state.x[2],
                    reinterpret_cast<const BYTE*>(data.data()),
                    static_cast<DWORD>(size));
            return finish(status);
        }
        if (function == "RegQueryValueA") {
            const auto subkey = optional_ansi(state.x[1]);
            if (state.x[2] != 0 && state.x[3] == 0)
                return finish(ERROR_INVALID_PARAMETER);
            if (state.x[3] != 0 &&
                !guest_range(memory, state.x[3], 4, true))
                return finish(ERROR_NOACCESS);
            LONG size = state.x[3] ?
                static_cast<LONG>(memory.read32(state.x[3])) : 0;
            if (size < 0 || size > 1024 * 1024)
                return finish(ERROR_INVALID_PARAMETER);
            if (state.x[2] != 0 && size != 0 &&
                !guest_range(memory, state.x[2], size, true))
                return finish(ERROR_NOACCESS);
            std::vector<char> data(state.x[2] == 0 ? 0
                : std::max<LONG>(1, size));
            const auto status = RegQueryValueA(key,
                subkey ? subkey->c_str() : nullptr,
                state.x[2] ? data.data() : nullptr,
                state.x[3] ? &size : nullptr);
            if (state.x[3])
                memory.write32(state.x[3], static_cast<std::uint32_t>(size));
            if (status == ERROR_SUCCESS && state.x[2] && size > 0)
                write_bytes(state.x[2],
                    reinterpret_cast<const BYTE*>(data.data()),
                    static_cast<DWORD>(size));
            return finish(status);
        }
        if (function == "RegConnectRegistryW") {
            if (!guest_range(memory, state.x[2], 8, true))
                return finish(ERROR_NOACCESS);
            const auto machine = optional_name(state.x[0]);
            const auto root = key_for(state.x[1]);
            if (!root) return finish(ERROR_INVALID_HANDLE);
            HKEY connected = nullptr;
            const auto status = RegConnectRegistryW(
                machine ? machine->c_str() : nullptr, root, &connected);
            if (status == ERROR_SUCCESS)
                store_key(state.x[2], connected, state.x[1], root);
            return finish(status);
        }
        if (function == "RegConnectRegistryA") {
            if (!guest_range(memory, state.x[2], 8, true))
                return finish(ERROR_NOACCESS);
            const auto machine = optional_ansi(state.x[0]);
            const auto root = key_for(state.x[1]);
            if (!root) return finish(ERROR_INVALID_HANDLE);
            HKEY connected = nullptr;
            const auto status = RegConnectRegistryA(
                machine ? machine->c_str() : nullptr, root, &connected);
            if (status == ERROR_SUCCESS)
                store_key(state.x[2], connected, state.x[1], root);
            return finish(status);
        }
        if (function == "RegLoadKeyW") {
            if (state.x[1] == 0 || state.x[2] == 0)
                return finish(ERROR_INVALID_PARAMETER);
            const auto subkey = guest_string(memory, state.x[1], true);
            const auto file = guest_string(memory, state.x[2], true);
            return finish(RegLoadKeyW(key, subkey.c_str(), file.c_str()));
        }
        if (function == "RegLoadKeyA") {
            if (state.x[1] == 0 || state.x[2] == 0)
                return finish(ERROR_INVALID_PARAMETER);
            const auto subkey = guest_ansi_string(memory, state.x[1]);
            const auto file = guest_ansi_string(memory, state.x[2]);
            return finish(RegLoadKeyA(key, subkey.c_str(), file.c_str()));
        }
        if (function == "RegSaveKeyW") {
            if (state.x[1] == 0) return finish(ERROR_INVALID_PARAMETER);
            const auto file = guest_string(memory, state.x[1], true);
            SECURITY_ATTRIBUTES security{};
            SECURITY_ATTRIBUTES* attributes = nullptr;
            if (state.x[2] != 0) {
                if (!guest_range(memory, state.x[2], sizeof(security), false))
                    return finish(ERROR_NOACCESS);
                if (memory.read32(state.x[2]) != sizeof(security))
                    return finish(ERROR_INVALID_PARAMETER);
                if (memory.read64(state.x[2] + 8) != 0)
                    return finish(ERROR_NOT_SUPPORTED);
                security.nLength = sizeof(security);
                security.bInheritHandle = memory.read32(state.x[2] + 16);
                attributes = &security;
            }
            return finish(RegSaveKeyW(key, file.c_str(), attributes));
        }
        if (function == "RegSaveKeyA") {
            if (state.x[1] == 0) return finish(ERROR_INVALID_PARAMETER);
            const auto file = guest_ansi_string(memory, state.x[1]);
            SECURITY_ATTRIBUTES security{};
            SECURITY_ATTRIBUTES* attributes = nullptr;
            if (state.x[2] != 0) {
                if (!guest_range(memory, state.x[2], sizeof(security), false))
                    return finish(ERROR_NOACCESS);
                if (memory.read32(state.x[2]) != sizeof(security))
                    return finish(ERROR_INVALID_PARAMETER);
                if (memory.read64(state.x[2] + 8) != 0)
                    return finish(ERROR_NOT_SUPPORTED);
                security.nLength = sizeof(security);
                security.bInheritHandle = memory.read32(state.x[2] + 16);
                attributes = &security;
            }
            return finish(RegSaveKeyA(key, file.c_str(), attributes));
        }
        if (function == "RegDeleteValueW" || function == "RegDeleteKeyW" ||
            function == "RegDeleteKeyExW") {
            const auto name = optional_name(state.x[1]);
            if (function != "RegDeleteValueW" && !name)
                return finish(ERROR_INVALID_PARAMETER);
            if (function == "RegDeleteValueW")
                return finish(RegDeleteValueW(key,
                    name ? name->c_str() : nullptr));
            if (function == "RegDeleteKeyW")
                return finish(RegDeleteKeyW(key, name->c_str()));
            if (state.x[3] != 0) return finish(ERROR_INVALID_PARAMETER);
            return finish(RegDeleteKeyExW(key, name->c_str(),
                static_cast<REGSAM>(state.x[2]), 0));
        }
        if (function == "RegDeleteValueA" || function == "RegDeleteKeyA" ||
            function == "RegDeleteKeyExA") {
            const auto name = optional_ansi(state.x[1]);
            if (function != "RegDeleteValueA" && !name)
                return finish(ERROR_INVALID_PARAMETER);
            if (function == "RegDeleteValueA")
                return finish(RegDeleteValueA(key,
                    name ? name->c_str() : nullptr));
            if (function == "RegDeleteKeyA")
                return finish(RegDeleteKeyA(key, name->c_str()));
            if (state.x[3] != 0) return finish(ERROR_INVALID_PARAMETER);
            return finish(RegDeleteKeyExA(key, name->c_str(),
                static_cast<REGSAM>(state.x[2]), 0));
        }
    } catch (const std::runtime_error&) {
        return finish(ERROR_NOACCESS);
    }
    throw std::runtime_error("unsupported Windows import " + imported.dll +
                             "!" + function);
}

void Win32Bridge::cleanup_fls() {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> callbacks;
    std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
    {
        std::lock_guard lock(state_->mutex);
        invoke = state_->guest_callback;
        for (auto& [index, slot] : state_->fls_slots) {
            const auto value = slot.values.find(GetCurrentThreadId());
            if (value != slot.values.end()) {
                if (slot.callback != 0 && value->second != 0) {
                    callbacks.emplace_back(slot.callback, value->second);
                }
                slot.values.erase(value);
            }
        }
    }
    if (!callbacks.empty() && !invoke) {
        throw std::runtime_error("guest FLS callback invoker is unavailable");
    }
    for (const auto& [callback, value] : callbacks) invoke(callback, value);
}

#include "win32_gui.inc"

std::optional<std::uint32_t> Win32Bridge::dispatch(const Import& imported,
                                                   CpuState& state,
                                                   GuestMemory& memory) {
    {
        std::unique_lock lock(state_->mutex);
        state_->dynamic_module_ready.wait(lock, [this] {
            return !state_->dynamic_module_initializing ||
                   state_->dynamic_module_initializing_thread == GetCurrentThreadId();
        });
    }
    std::string dll = imported.dll;
    std::transform(dll.begin(), dll.end(), dll.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const auto& function = imported.function;
    if ((dll == "user32.dll" || dll == "gdi32.dll" || dll == "opengl32.dll" ||
         dll == "dwmapi.dll" || dll == "imm32.dll" || dll == "ole32.dll" || dll == "shell32.dll") &&
        dispatch_gui({dll, function, imported.entry}, state, memory)) {
        return std::nullopt;
    }
    if (dll == "version.dll" &&
        (function == "GetFileVersionInfoSizeW" ||
         function == "GetFileVersionInfoSizeA" ||
         function == "GetFileVersionInfoW" ||
         function == "GetFileVersionInfoA" ||
         function == "VerQueryValueW" ||
         function == "VerQueryValueA")) {
        constexpr std::uint32_t maximum_version_info = 16U * 1024U * 1024U;
        const bool wide = function.ends_with('W');
        const auto fail = [&state](DWORD error) -> std::optional<std::uint32_t> {
            SetLastError(error);
            state.x[0] = 0;
            return std::nullopt;
        };
        if (function.starts_with("GetFileVersionInfoSize") ||
            function.starts_with("GetFileVersionInfo")) {
            if (!guest_range(memory, state.x[0], wide ? 2 : 1, false)) {
                return fail(ERROR_NOACCESS);
            }
            const bool size_query = function.starts_with("GetFileVersionInfoSize");
            if (size_query && state.x[1] != 0 &&
                !guest_range(memory, state.x[1], 4, true)) {
                return fail(ERROR_NOACCESS);
            }
            std::wstring wide_path;
            std::string ansi_path;
            try {
                if (wide) wide_path = guest_string(memory, state.x[0], true);
                else ansi_path = guest_ansi_string(memory, state.x[0]);
            } catch (const std::runtime_error&) {
                return fail(ERROR_NOACCESS);
            }
            if (size_query) {
                DWORD ignored_handle = 0;
                const DWORD result = wide
                    ? ::GetFileVersionInfoSizeW(wide_path.c_str(), &ignored_handle)
                    : ::GetFileVersionInfoSizeA(ansi_path.c_str(), &ignored_handle);
                const DWORD error = GetLastError();
                if (state.x[1] != 0) memory.write32(state.x[1], 0);
                state.x[0] = result;
                SetLastError(error);
                return std::nullopt;
            }
            if (state.x[2] > maximum_version_info ||
                state.x[2] != static_cast<std::uint32_t>(state.x[2])) {
                return fail(ERROR_INVALID_PARAMETER);
            }
            const auto length = static_cast<std::uint32_t>(state.x[2]);
            if (!guest_range(memory, state.x[3], length, true)) {
                return fail(ERROR_NOACCESS);
            }
            {
                std::lock_guard lock(state_->mutex);
                state_->version_info_buffers.erase(state.x[3]);
            }
            std::vector<std::uint8_t> bytes(length);
            const BOOL success = wide
                ? ::GetFileVersionInfoW(wide_path.c_str(),
                    static_cast<DWORD>(state.x[1]), length, bytes.data())
                : ::GetFileVersionInfoA(ansi_path.c_str(),
                    static_cast<DWORD>(state.x[1]), length, bytes.data());
            const DWORD error = GetLastError();
            if (success) {
                for (std::uint32_t index = 0; index < length; ++index) {
                    memory.write8(state.x[3] + index, bytes[index]);
                }
                const DWORD required = wide
                    ? ::GetFileVersionInfoSizeW(wide_path.c_str(), nullptr)
                    : ::GetFileVersionInfoSizeA(ansi_path.c_str(), nullptr);
                std::lock_guard lock(state_->mutex);
                if (required != 0 && required <= length) {
                    state_->version_info_buffers[state.x[3]] = std::move(bytes);
                } else {
                    // Windows may report success after truncating the blob.
                    // Such a blob is not safe input for native VerQueryValue.
                    state_->version_info_buffers.erase(state.x[3]);
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        const auto guest_block = state.x[0];
        const auto guest_subblock = state.x[1];
        const auto guest_value_output = state.x[2];
        const auto guest_length_output = state.x[3];
        if (!guest_range(memory, guest_subblock, wide ? 2 : 1, false) ||
            !guest_range(memory, guest_value_output, 8, true) ||
            !guest_range(memory, guest_length_output, 4, true)) {
            return fail(ERROR_NOACCESS);
        }
        std::vector<std::uint8_t> bytes;
        {
            std::lock_guard lock(state_->mutex);
            if (const auto found = state_->version_info_buffers.find(guest_block);
                found != state_->version_info_buffers.end()) {
                bytes = found->second;
            }
        }
        const auto capacity = bytes.size();
        if (capacity == 0 || !guest_range(memory, guest_block, capacity, false) ||
            !guest_range(memory, guest_block, capacity, true)) {
            return fail(ERROR_INVALID_DATA);
        }
        std::wstring wide_subblock;
        std::string ansi_subblock;
        try {
            if (wide) wide_subblock = guest_string(memory, guest_subblock, true);
            else ansi_subblock = guest_ansi_string(memory, guest_subblock);
        } catch (const std::runtime_error&) {
            return fail(ERROR_NOACCESS);
        }
        void* host_value = nullptr;
        UINT value_length = 0;
        const BOOL success = wide
            ? ::VerQueryValueW(bytes.data(), wide_subblock.c_str(),
                               &host_value, &value_length)
            : ::VerQueryValueA(bytes.data(), ansi_subblock.c_str(),
                               &host_value, &value_length);
        const DWORD error = GetLastError();
        if (!success) {
            memory.write64(guest_value_output, 0);
            memory.write32(guest_length_output, 0);
            state.x[0] = 0;
            SetLastError(error);
            return std::nullopt;
        }
        const auto host_base = reinterpret_cast<std::uintptr_t>(bytes.data());
        const auto host_address = reinterpret_cast<std::uintptr_t>(host_value);
        if (host_address < host_base || host_address >= host_base + capacity) {
            return fail(ERROR_INVALID_DATA);
        }
        const auto offset = host_address - host_base;
        auto normalized_subblock = wide ? wide_subblock :
            std::wstring(ansi_subblock.begin(), ansi_subblock.end());
        std::transform(normalized_subblock.begin(), normalized_subblock.end(),
            normalized_subblock.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
        const bool string_value = normalized_subblock.starts_with(
            L"\\stringfileinfo\\");
        const auto byte_length = static_cast<std::uint64_t>(value_length) *
            (string_value && wide ? 2ULL : 1ULL);
        if (byte_length > capacity - offset) return fail(ERROR_INVALID_DATA);
        // VerQueryValueA can convert a string within its caller-owned block.
        // Publish those changes before returning a pointer into the guest copy.
        for (std::size_t index = 0; index < capacity; ++index) {
            memory.write8(guest_block + index, bytes[index]);
        }
        memory.write64(guest_value_output, guest_block + offset);
        memory.write32(guest_length_output, value_length);
        state.x[0] = 1;
        SetLastError(error);
        return std::nullopt;
    }
    if (dll == "advapi32.dll" && function.starts_with("Reg")) {
        return dispatch_registry(imported, state, memory);
    }
    if (dll == "ws2_32.dll") {
        const auto is = [&](std::string_view name, std::string_view ordinal) {
            return function == name || function == ordinal;
        };
        const auto error = [&](int value) {
            state_->winsock_errors[GetCurrentThreadId()] = value;
            state.x[0] = static_cast<std::uint32_t>(SOCKET_ERROR);
            return std::nullopt;
        };
        const auto arg_i32 = [&](unsigned index) {
            return static_cast<int>(static_cast<std::int32_t>(state.x[index]));
        };
        if (is("WSAStartup", "#115")) {
            if (!guest_range(memory, state.x[1], sizeof(WSADATA), true)) {
                state.x[0] = WSAEFAULT;
                return std::nullopt;
            }
            std::lock_guard lock(state_->mutex);
            WSADATA data{};
            const auto status = ::WSAStartup(
                static_cast<WORD>(state.x[0]), &data);
            if (status == 0) {
                // Winsock 2 does not use this vendor pointer. Never expose a
                // host address in the guest's ARM64 WSADATA structure.
                data.lpVendorInfo = nullptr;
                try {
                    write_guest_structure(memory, state.x[1], data);
                } catch (const std::exception&) {
                    static_cast<void>(::WSACleanup());
                    state.x[0] = WSAEFAULT;
                    return std::nullopt;
                }
                ++state_->winsock_startups;
            }
            state.x[0] = static_cast<std::uint32_t>(status);
            return std::nullopt;
        }
        if (is("WSACleanup", "#116")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            const auto status = ::WSACleanup();
            if (status == 0) {
                --state_->winsock_startups;
                if (state_->winsock_startups == 0) state_->sockets.clear();
                state.x[0] = 0;
            } else {
                return error(::WSAGetLastError());
            }
            return std::nullopt;
        }
        if (is("WSAGetLastError", "#111")) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->winsock_errors.find(GetCurrentThreadId());
            state.x[0] = found == state_->winsock_errors.end()
                ? 0 : static_cast<std::uint32_t>(found->second);
            return std::nullopt;
        }
        if (is("WSASetLastError", "#112")) {
            std::lock_guard lock(state_->mutex);
            state_->winsock_errors[GetCurrentThreadId()] =
                static_cast<int>(state.x[0]);
            return std::nullopt;
        }
        if (is("socket", "#23")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) {
                error(WSANOTINITIALISED);
                state.x[0] = UINT64_MAX;
                return std::nullopt;
            }
            const auto native = ::socket(static_cast<int>(state.x[0]),
                static_cast<int>(state.x[1]), static_cast<int>(state.x[2]));
            if (native == INVALID_SOCKET) {
                error(::WSAGetLastError());
                state.x[0] = UINT64_MAX;
                return std::nullopt;
            }
            const auto token = state_->next_socket_handle;
            state_->next_socket_handle += 8;
            state_->sockets.emplace(token, native);
            state.x[0] = token;
            return std::nullopt;
        }
        if (is("closesocket", "#3") || is("shutdown", "#22") ||
            is("ioctlsocket", "#10")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            const auto found = state_->sockets.find(state.x[0]);
            if (found == state_->sockets.end()) return error(WSAENOTSOCK);
            int status = SOCKET_ERROR;
            if (is("closesocket", "#3")) {
                status = ::closesocket(found->second);
                if (status == 0) state_->sockets.erase(found);
            } else if (is("shutdown", "#22")) {
                status = ::shutdown(found->second,
                                    static_cast<int>(state.x[1]));
            } else {
                if (!guest_range(memory, state.x[2], 4, false) ||
                    !guest_range(memory, state.x[2], 4, true)) {
                    return error(WSAEFAULT);
                }
                u_long argument = memory.read32(state.x[2]);
                status = ::ioctlsocket(found->second,
                    static_cast<long>(state.x[1]), &argument);
                if (status == 0) memory.write32(state.x[2], argument);
            }
            if (status == SOCKET_ERROR) return error(::WSAGetLastError());
            state.x[0] = 0;
            return std::nullopt;
        }
        if (is("bind", "#2") || is("connect", "#4") ||
            is("listen", "#13")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            const auto found = state_->sockets.find(state.x[0]);
            if (found == state_->sockets.end()) return error(WSAENOTSOCK);
            int status = SOCKET_ERROR;
            if (is("listen", "#13")) {
                status = ::listen(found->second, arg_i32(1));
            } else {
                const auto length = arg_i32(2);
                if (length < static_cast<int>(sizeof(ADDRESS_FAMILY)) ||
                    length > static_cast<int>(sizeof(SOCKADDR_STORAGE)) ||
                    !guest_range(memory, state.x[1], length, false)) {
                    return error(WSAEFAULT);
                }
                SOCKADDR_STORAGE address{};
                auto* bytes = reinterpret_cast<std::uint8_t*>(&address);
                for (int index = 0; index < length; ++index) {
                    bytes[index] = memory.read8(state.x[1] + index);
                }
                status = is("bind", "#2")
                    ? ::bind(found->second,
                             reinterpret_cast<const SOCKADDR*>(&address), length)
                    : ::connect(found->second,
                                reinterpret_cast<const SOCKADDR*>(&address), length);
            }
            if (status == SOCKET_ERROR) return error(::WSAGetLastError());
            state.x[0] = 0;
            return std::nullopt;
        }
        if (is("accept", "#1") || is("getpeername", "#5") ||
            is("getsockname", "#6")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            const auto found = state_->sockets.find(state.x[0]);
            if (found == state_->sockets.end()) return error(WSAENOTSOCK);
            const bool address_requested = state.x[1] != 0 || state.x[2] != 0;
            int address_length = sizeof(SOCKADDR_STORAGE);
            if (address_requested) {
                if (state.x[1] == 0 ||
                    !guest_range(memory, state.x[2], 4, true) ||
                    !guest_range(memory, state.x[2], 4, false)) {
                    return error(WSAEFAULT);
                }
                const auto requested = static_cast<int>(memory.read32(state.x[2]));
                if (requested < 0 || requested > 65536 ||
                    !guest_range(memory, state.x[1], requested, true)) {
                    return error(WSAEFAULT);
                }
                address_length = std::min(requested,
                    static_cast<int>(sizeof(SOCKADDR_STORAGE)));
            }
            SOCKADDR_STORAGE address{};
            auto* native_address = reinterpret_cast<SOCKADDR*>(&address);
            if (is("accept", "#1")) {
                const auto accepted = ::accept(found->second,
                    address_requested ? native_address : nullptr,
                    address_requested ? &address_length : nullptr);
                if (accepted == INVALID_SOCKET) return error(::WSAGetLastError());
                if (address_requested) {
                    for (int index = 0; index < address_length; ++index) {
                        memory.write8(state.x[1] + index,
                            reinterpret_cast<const std::uint8_t*>(&address)[index]);
                    }
                    memory.write32(state.x[2], address_length);
                }
                const auto token = state_->next_socket_handle;
                state_->next_socket_handle += 8;
                state_->sockets.emplace(token, accepted);
                state.x[0] = token;
                return std::nullopt;
            }
            if (!address_requested) return error(WSAEFAULT);
            const auto status = is("getpeername", "#5")
                ? ::getpeername(found->second, native_address, &address_length)
                : ::getsockname(found->second, native_address, &address_length);
            if (status == SOCKET_ERROR) return error(::WSAGetLastError());
            for (int index = 0; index < address_length; ++index) {
                memory.write8(state.x[1] + index,
                    reinterpret_cast<const std::uint8_t*>(&address)[index]);
            }
            memory.write32(state.x[2], address_length);
            state.x[0] = 0;
            return std::nullopt;
        }
        if (is("send", "#19") || is("recv", "#16") ||
            is("sendto", "#20") || is("recvfrom", "#17")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            const auto found = state_->sockets.find(state.x[0]);
            if (found == state_->sockets.end()) return error(WSAENOTSOCK);
            const auto length = arg_i32(2);
            if (length < 0) return error(WSAEINVAL);
            if (length > 16 * 1024 * 1024) return error(WSAENOBUFS);
            const bool sending = is("send", "#19") || is("sendto", "#20");
            if (length != 0 &&
                !guest_range(memory, state.x[1], length, !sending)) {
                return error(WSAEFAULT);
            }
            std::vector<char> bytes(static_cast<std::size_t>(
                std::max(length, 1)));
            if (sending) {
                for (int index = 0; index < length; ++index) {
                    bytes[index] = static_cast<char>(memory.read8(
                        state.x[1] + index));
                }
            }
            SOCKADDR_STORAGE address{};
            int address_length = sizeof(address);
            const bool with_address = is("sendto", "#20") ||
                                      is("recvfrom", "#17");
            const bool address_requested = with_address &&
                (state.x[4] != 0 || state.x[5] != 0);
            if (address_requested && sending) {
                const auto requested = arg_i32(5);
                if (state.x[4] == 0 ||
                    requested < static_cast<int>(sizeof(ADDRESS_FAMILY)) ||
                    requested > static_cast<int>(sizeof(address)) ||
                    !guest_range(memory, state.x[4], requested, false)) {
                    return error(WSAEFAULT);
                }
                address_length = requested;
                for (int index = 0; index < requested; ++index) {
                    reinterpret_cast<std::uint8_t*>(&address)[index] =
                        memory.read8(state.x[4] + index);
                }
            }
            if (address_requested && !sending) {
                if (state.x[4] == 0 ||
                    !guest_range(memory, state.x[5], 4, false) ||
                    !guest_range(memory, state.x[5], 4, true)) {
                    return error(WSAEFAULT);
                }
                const auto requested = static_cast<int>(
                    memory.read32(state.x[5]));
                if (requested < 0 || requested > 65536 ||
                    !guest_range(memory, state.x[4], requested, true)) {
                    return error(WSAEFAULT);
                }
                address_length = std::min(requested,
                    static_cast<int>(sizeof(address)));
            }
            int status = SOCKET_ERROR;
            if (is("send", "#19")) {
                status = ::send(found->second, bytes.data(), length,
                                arg_i32(3));
            } else if (is("recv", "#16")) {
                status = ::recv(found->second, bytes.data(), length,
                                arg_i32(3));
            } else if (is("sendto", "#20")) {
                status = ::sendto(found->second, bytes.data(), length,
                    arg_i32(3), address_requested
                        ? reinterpret_cast<const SOCKADDR*>(&address) : nullptr,
                    address_requested ? address_length : 0);
            } else {
                status = ::recvfrom(found->second, bytes.data(), length,
                    arg_i32(3), address_requested
                        ? reinterpret_cast<SOCKADDR*>(&address) : nullptr,
                    address_requested ? &address_length : nullptr);
            }
            if (status == SOCKET_ERROR) return error(::WSAGetLastError());
            if (!sending) {
                for (int index = 0; index < status; ++index) {
                    memory.write8(state.x[1] + index,
                                  static_cast<std::uint8_t>(bytes[index]));
                }
            }
            if (address_requested && !sending) {
                for (int index = 0; index < address_length; ++index) {
                    memory.write8(state.x[4] + index,
                        reinterpret_cast<const std::uint8_t*>(&address)[index]);
                }
                memory.write32(state.x[5], address_length);
            }
            state.x[0] = static_cast<std::uint32_t>(status);
            return std::nullopt;
        }
        if (is("setsockopt", "#21") || is("getsockopt", "#7")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            const auto found = state_->sockets.find(state.x[0]);
            if (found == state_->sockets.end()) return error(WSAENOTSOCK);
            const bool setting = is("setsockopt", "#21");
            if (!setting && (!guest_range(memory, state.x[4], 4, false) ||
                             !guest_range(memory, state.x[4], 4, true))) {
                return error(WSAEFAULT);
            }
            const auto length = setting ? arg_i32(4)
                : static_cast<int>(memory.read32(state.x[4]));
            if (length < 0) return error(WSAEINVAL);
            if (length > 65536) return error(WSAENOBUFS);
            if (length != 0 &&
                !guest_range(memory, state.x[3], length, !setting)) {
                return error(WSAEFAULT);
            }
            std::vector<char> value(static_cast<std::size_t>(
                std::max(length, 1)));
            if (setting) {
                for (int index = 0; index < length; ++index) {
                    value[index] = static_cast<char>(memory.read8(
                        state.x[3] + index));
                }
            }
            int native_length = length;
            const auto status = setting
                ? ::setsockopt(found->second, arg_i32(1), arg_i32(2),
                               value.data(), length)
                : ::getsockopt(found->second, arg_i32(1), arg_i32(2),
                               value.data(), &native_length);
            if (status == SOCKET_ERROR) return error(::WSAGetLastError());
            if (!setting) {
                for (int index = 0; index < native_length; ++index) {
                    memory.write8(state.x[3] + index,
                        static_cast<std::uint8_t>(value[index]));
                }
                memory.write32(state.x[4], native_length);
            }
            state.x[0] = 0;
            return std::nullopt;
        }
        if (is("select", "#18")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            struct SocketSet {
                fd_set native{};
                std::vector<std::pair<SOCKET, std::uint64_t>> tokens;
                std::uint64_t address{};
            };
            std::array<SocketSet, 3> sets{};
            for (unsigned set_index = 0; set_index < sets.size(); ++set_index) {
                auto& set = sets[set_index];
                set.address = state.x[set_index + 1];
                if (set.address == 0) continue;
                if (!guest_range(memory, set.address, 8, false) ||
                    !guest_range(memory, set.address, 8, true)) {
                    return error(WSAEFAULT);
                }
                const auto count = memory.read32(set.address);
                if (count > FD_SETSIZE ||
                    !guest_range(memory, set.address, 8ULL + count * 8ULL,
                                 false) ||
                    !guest_range(memory, set.address, 8ULL + count * 8ULL,
                                 true)) {
                    return error(WSAEFAULT);
                }
                set.native.fd_count = count;
                for (unsigned index = 0; index < count; ++index) {
                    const auto guest = memory.read64(set.address + 8 + index * 8ULL);
                    const auto found = state_->sockets.find(guest);
                    if (found == state_->sockets.end()) return error(WSAENOTSOCK);
                    set.native.fd_array[index] = found->second;
                    set.tokens.emplace_back(found->second, guest);
                }
            }
            timeval timeout{};
            timeval* native_timeout = nullptr;
            if (state.x[4] != 0) {
                if (!guest_range(memory, state.x[4], 8, false)) {
                    return error(WSAEFAULT);
                }
                timeout.tv_sec = static_cast<long>(static_cast<std::int32_t>(
                    memory.read32(state.x[4])));
                timeout.tv_usec = static_cast<long>(static_cast<std::int32_t>(
                    memory.read32(state.x[4] + 4)));
                native_timeout = &timeout;
            }
            const auto status = ::select(arg_i32(0),
                sets[0].address ? &sets[0].native : nullptr,
                sets[1].address ? &sets[1].native : nullptr,
                sets[2].address ? &sets[2].native : nullptr,
                native_timeout);
            if (status == SOCKET_ERROR) return error(::WSAGetLastError());
            for (auto& set : sets) {
                if (set.address == 0) continue;
                memory.write32(set.address, set.native.fd_count);
                for (unsigned index = 0; index < set.native.fd_count; ++index) {
                    const auto native = set.native.fd_array[index];
                    const auto found = std::find_if(set.tokens.begin(),
                        set.tokens.end(), [native](const auto& token) {
                            return token.first == native;
                        });
                    if (found == set.tokens.end()) {
                        throw std::runtime_error(
                            "Winsock select returned an unknown socket");
                    }
                    memory.write64(set.address + 8 + index * 8ULL,
                                   found->second);
                }
            }
            state.x[0] = static_cast<std::uint32_t>(status);
            return std::nullopt;
        }
        if (is("__WSAFDIsSet", "#151")) {
            const auto set = state.x[1];
            if (!guest_range(memory, set, 8, false)) {
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto count = memory.read32(set);
            if (count > FD_SETSIZE ||
                !guest_range(memory, set, 8ULL + count * 8ULL, false)) {
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto socket = state.x[0];
            state.x[0] = 0;
            for (unsigned index = 0; index < count; ++index) {
                if (memory.read64(set + 8 + index * 8ULL) == socket) {
                    state.x[0] = 1;
                    break;
                }
            }
            return std::nullopt;
        }
        if (is("htonl", "#8") || is("ntohl", "#14")) {
            state.x[0] = ::htonl(static_cast<u_long>(state.x[0]));
            return std::nullopt;
        }
        if (is("htons", "#9") || is("ntohs", "#15")) {
            state.x[0] = ::htons(static_cast<u_short>(state.x[0]));
            return std::nullopt;
        }
        if (is("gethostname", "#57")) {
            std::lock_guard lock(state_->mutex);
            if (state_->winsock_startups == 0) return error(WSANOTINITIALISED);
            if (state.x[1] == 0 || state.x[1] > 65536) {
                return error(WSAEFAULT);
            }
            const auto length = static_cast<int>(state.x[1]);
            if (!guest_range(memory, state.x[0], length, true)) {
                return error(WSAEFAULT);
            }
            std::vector<char> name(static_cast<std::size_t>(length));
            const auto status = ::gethostname(name.data(), length);
            if (status == SOCKET_ERROR) return error(::WSAGetLastError());
            for (int index = 0; index < length; ++index) {
                memory.write8(state.x[0] + index,
                              static_cast<std::uint8_t>(name[index]));
                if (name[index] == '\0') break;
            }
            state.x[0] = 0;
            return std::nullopt;
        }
        if (is("inet_addr", "#11")) {
            try {
                const auto text = guest_ansi_span(memory, state.x[0], -1);
                state.x[0] = host_inet_addr(text.c_str());
            } catch (const std::exception&) {
                std::lock_guard lock(state_->mutex);
                state_->winsock_errors[GetCurrentThreadId()] = WSAEFAULT;
                state.x[0] = INADDR_NONE;
            }
            return std::nullopt;
        }
        if (is("inet_ntoa", "#12")) {
            IN_ADDR address{};
            address.S_un.S_addr = static_cast<u_long>(state.x[0]);
            const auto* text = host_inet_ntoa(address);
            if (text == nullptr) {
                state.x[0] = 0;
                return std::nullopt;
            }
            std::lock_guard lock(state_->mutex);
            auto& guest = state_->inet_ntoa_strings[GetCurrentThreadId()];
            if (guest == 0) {
                guest = state_->next_resolver_address;
                state_->next_resolver_address += 2 * guest_page_size;
                memory.map_memory(guest, guest_page_size);
            }
            for (std::size_t index = 0; index < 64; ++index) {
                memory.write8(guest + index,
                              static_cast<std::uint8_t>(text[index]));
                if (text[index] == '\0') break;
            }
            state.x[0] = guest;
            return std::nullopt;
        }
        if (function == "getnameinfo") {
            int result = 0;
            if (state.x[1] < sizeof(ADDRESS_FAMILY) ||
                state.x[1] > sizeof(SOCKADDR_STORAGE) ||
                !guest_range(memory, state.x[0], state.x[1], false) ||
                state.x[3] > 65536 || state.x[5] > 65536 ||
                (state.x[2] != 0 &&
                 !guest_range(memory, state.x[2], state.x[3], true)) ||
                (state.x[4] != 0 &&
                 !guest_range(memory, state.x[4], state.x[5], true))) {
                result = WSAEFAULT;
            } else {
                SOCKADDR_STORAGE address{};
                for (std::uint64_t index = 0; index < state.x[1]; ++index) {
                    reinterpret_cast<std::uint8_t*>(&address)[index] =
                        memory.read8(state.x[0] + index);
                }
                std::vector<char> node(static_cast<std::size_t>(
                    std::max<std::uint64_t>(state.x[3], 1)));
                std::vector<char> service(static_cast<std::size_t>(
                    std::max<std::uint64_t>(state.x[5], 1)));
                result = ::getnameinfo(
                    reinterpret_cast<const SOCKADDR*>(&address),
                    static_cast<socklen_t>(state.x[1]),
                    state.x[2] ? node.data() : nullptr,
                    static_cast<DWORD>(state.x[3]),
                    state.x[4] ? service.data() : nullptr,
                    static_cast<DWORD>(state.x[5]), arg_i32(6));
                if (result == 0) {
                    if (state.x[2] != 0) {
                        const auto size = strnlen_s(node.data(), node.size());
                        for (std::size_t index = 0;
                             index < std::min(size + 1, node.size()); ++index) {
                            memory.write8(state.x[2] + index,
                                static_cast<std::uint8_t>(node[index]));
                        }
                    }
                    if (state.x[4] != 0) {
                        const auto size = strnlen_s(service.data(), service.size());
                        for (std::size_t index = 0;
                             index < std::min(size + 1, service.size()); ++index) {
                            memory.write8(state.x[4] + index,
                                static_cast<std::uint8_t>(service[index]));
                        }
                    }
                }
            }
            if (result != 0) {
                std::lock_guard lock(state_->mutex);
                state_->winsock_errors[GetCurrentThreadId()] = result;
            }
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        if (function == "getaddrinfo") {
            const auto output = state.x[3];
            if (!guest_range(memory, output, 8, true)) {
                state.x[0] = WSAEFAULT;
                return std::nullopt;
            }
            memory.write64(output, 0);
            {
                std::lock_guard lock(state_->mutex);
                if (state_->winsock_startups == 0) {
                    state.x[0] = WSANOTINITIALISED;
                    return std::nullopt;
                }
            }
            std::string node;
            std::string service;
            try {
                if (state.x[0] != 0) {
                    node = guest_ansi_span(memory, state.x[0], -1);
                }
                if (state.x[1] != 0) {
                    service = guest_ansi_span(memory, state.x[1], -1);
                }
            } catch (const std::exception&) {
                state.x[0] = WSAEFAULT;
                return std::nullopt;
            }
            ADDRINFOA hints{};
            ADDRINFOA* hint_pointer = nullptr;
            if (state.x[2] != 0) {
                if (!guest_range(memory, state.x[2], sizeof(ADDRINFOA), false)) {
                    state.x[0] = WSAEFAULT;
                    return std::nullopt;
                }
                hints.ai_flags = static_cast<int>(memory.read32(state.x[2]));
                hints.ai_family = static_cast<int>(memory.read32(state.x[2] + 4));
                hints.ai_socktype = static_cast<int>(memory.read32(state.x[2] + 8));
                hints.ai_protocol = static_cast<int>(memory.read32(state.x[2] + 12));
                if (memory.read64(state.x[2] + 16) != 0 ||
                    memory.read64(state.x[2] + 24) != 0 ||
                    memory.read64(state.x[2] + 32) != 0 ||
                    memory.read64(state.x[2] + 40) != 0) {
                    state.x[0] = WSAEINVAL;
                    return std::nullopt;
                }
                hint_pointer = &hints;
            }
            ADDRINFOA* resolved = nullptr;
            const auto status = ::getaddrinfo(
                state.x[0] ? node.c_str() : nullptr,
                state.x[1] ? service.c_str() : nullptr,
                hint_pointer, &resolved);
            if (status != 0) {
                std::lock_guard lock(state_->mutex);
                state_->winsock_errors[GetCurrentThreadId()] = status;
                state.x[0] = static_cast<std::uint32_t>(status);
                return std::nullopt;
            }
            const std::unique_ptr<ADDRINFOA, void(*)(ADDRINFOA*)> guard(
                resolved, [](ADDRINFOA* value) { ::freeaddrinfo(value); });
            struct ResolverNode {
                const ADDRINFOA* native{};
                std::size_t node{};
                std::size_t address{};
                std::size_t name{};
                std::size_t name_size{};
            };
            std::vector<ResolverNode> nodes;
            std::size_t used = 0;
            const auto align8 = [](std::size_t value) {
                return (value + 7U) & ~std::size_t{7};
            };
            for (auto* item = resolved; item != nullptr; item = item->ai_next) {
                if (nodes.size() == 256 || item->ai_addrlen >
                        sizeof(SOCKADDR_STORAGE) ||
                    (item->ai_addrlen != 0 && item->ai_addr == nullptr)) {
                    state.x[0] = WSAENOBUFS;
                    return std::nullopt;
                }
                ResolverNode entry{};
                entry.native = item;
                entry.node = align8(used);
                used = entry.node + sizeof(ADDRINFOA);
                entry.address = align8(used);
                used = entry.address + item->ai_addrlen;
                if (item->ai_canonname != nullptr) {
                    entry.name_size = strnlen_s(item->ai_canonname, 1025);
                    if (entry.name_size == 1025) {
                        state.x[0] = WSAENOBUFS;
                        return std::nullopt;
                    }
                    entry.name = used;
                    used += entry.name_size + 1;
                }
                if (used > 1024 * 1024) {
                    state.x[0] = WSAENOBUFS;
                    return std::nullopt;
                }
                nodes.push_back(entry);
            }
            if (nodes.empty()) {
                state.x[0] = WSAHOST_NOT_FOUND;
                return std::nullopt;
            }
            const auto allocation_size =
                (used + guest_page_size - 1) & ~(guest_page_size - 1);
            std::lock_guard lock(state_->mutex);
            const auto base = state_->next_resolver_address;
            state_->next_resolver_address += allocation_size + guest_page_size;
            try {
                memory.map_memory(base, allocation_size);
                state_->resolver_allocations.emplace(base, allocation_size);
                for (std::size_t index = 0; index < nodes.size(); ++index) {
                    const auto& entry = nodes[index];
                    const auto* item = entry.native;
                    const auto at = base + entry.node;
                    memory.write32(at, item->ai_flags);
                    memory.write32(at + 4, item->ai_family);
                    memory.write32(at + 8, item->ai_socktype);
                    memory.write32(at + 12, item->ai_protocol);
                    memory.write64(at + 16, item->ai_addrlen);
                    memory.write64(at + 24,
                        item->ai_canonname != nullptr ? base + entry.name : 0);
                    memory.write64(at + 32,
                        item->ai_addrlen != 0 ? base + entry.address : 0);
                    memory.write64(at + 40, index + 1 < nodes.size()
                        ? base + nodes[index + 1].node : 0);
                    for (std::size_t byte = 0; byte < item->ai_addrlen; ++byte) {
                        memory.write8(base + entry.address + byte,
                            reinterpret_cast<const std::uint8_t*>(
                                item->ai_addr)[byte]);
                    }
                    if (item->ai_canonname != nullptr) {
                        for (std::size_t byte = 0;
                             byte <= entry.name_size; ++byte) {
                            memory.write8(base + entry.name + byte,
                                static_cast<std::uint8_t>(
                                    item->ai_canonname[byte]));
                        }
                    }
                }
                memory.write64(output, base + nodes.front().node);
            } catch (const std::exception&) {
                if (state_->resolver_allocations.erase(base) != 0) {
                    memory.unmap_memory(base);
                }
                state.x[0] = WSAENOBUFS;
                return std::nullopt;
            }
            state.x[0] = 0;
            return std::nullopt;
        }
        if (function == "freeaddrinfo") {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->resolver_allocations.find(state.x[0]);
            if (found == state_->resolver_allocations.end()) {
                state_->winsock_errors[GetCurrentThreadId()] = WSAEINVAL;
                return std::nullopt;
            }
            memory.unmap_memory(found->first);
            state_->resolver_allocations.erase(found);
            return std::nullopt;
        }
    }
    if (dll == "bcrypt.dll" &&
        (function == "BCryptOpenAlgorithmProvider" ||
         function == "BCryptCloseAlgorithmProvider" ||
         function == "BCryptGenRandom")) {
        constexpr std::uint32_t invalid_parameter = 0xc000000dU;
        constexpr std::uint32_t invalid_handle = 0xc0000008U;
        if (function == "BCryptOpenAlgorithmProvider") {
            if (!guest_range(memory, state.x[0], 8, true) ||
                state.x[1] == 0 || state.x[3] > UINT32_MAX) {
                state.x[0] = invalid_parameter;
                return std::nullopt;
            }
            const auto output = state.x[0];
            memory.write64(output, 0);
            const auto algorithm = guest_string(memory, state.x[1], true);
            const auto provider = state.x[2] == 0 ? std::wstring{} :
                guest_string(memory, state.x[2], true);
            BCRYPT_ALG_HANDLE native = nullptr;
            const auto status = ::BCryptOpenAlgorithmProvider(&native,
                algorithm.c_str(), state.x[2] == 0 ? nullptr : provider.c_str(),
                static_cast<ULONG>(state.x[3]));
            if (status >= 0) {
                std::lock_guard lock(state_->mutex);
                const auto guest_handle = state_->next_cng_handle;
                state_->next_cng_handle += 16;
                state_->cng_algorithms.emplace(guest_handle, native);
                memory.write64(output, guest_handle);
            }
            state.x[0] = static_cast<std::uint32_t>(status);
            return std::nullopt;
        }
        if (function == "BCryptCloseAlgorithmProvider") {
            if (state.x[1] > UINT32_MAX) {
                state.x[0] = invalid_parameter;
                return std::nullopt;
            }
            std::lock_guard lock(state_->mutex);
            const auto found = state_->cng_algorithms.find(state.x[0]);
            if (found == state_->cng_algorithms.end()) {
                state.x[0] = invalid_handle;
                return std::nullopt;
            }
            const auto status = ::BCryptCloseAlgorithmProvider(found->second,
                static_cast<ULONG>(state.x[1]));
            if (status >= 0) state_->cng_algorithms.erase(found);
            state.x[0] = static_cast<std::uint32_t>(status);
            return std::nullopt;
        }
        if (state.x[2] > 16ULL * 1024 * 1024 ||
            state.x[3] > UINT32_MAX || state.x[2] > UINT32_MAX ||
            (state.x[2] != 0 &&
             !guest_range(memory, state.x[1], state.x[2], true))) {
            state.x[0] = invalid_parameter;
            return std::nullopt;
        }
        BCRYPT_ALG_HANDLE native = nullptr;
        if (state.x[0] != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->cng_algorithms.find(state.x[0]);
            if (found == state_->cng_algorithms.end()) {
                state.x[0] = invalid_handle;
                return std::nullopt;
            }
            native = found->second;
        }
        const auto output = state.x[1];
        const auto size = static_cast<ULONG>(state.x[2]);
        const auto flags = static_cast<ULONG>(state.x[3]);
        std::vector<std::uint8_t> bytes(size);
        if ((flags & BCRYPT_RNG_USE_ENTROPY_IN_BUFFER) != 0) {
            if (!guest_range(memory, output, size, false)) {
                state.x[0] = invalid_parameter;
                return std::nullopt;
            }
            for (ULONG index = 0; index < size; ++index) {
                bytes[index] = memory.read8(output + index);
            }
        }
        const auto status = ::BCryptGenRandom(native,
            size == 0 ? nullptr : bytes.data(), size, flags);
        if (status >= 0) {
            for (ULONG index = 0; index < size; ++index) {
                memory.write8(output + index, bytes[index]);
            }
        }
        state.x[0] = static_cast<std::uint32_t>(status);
        return std::nullopt;
    }
    if (dll == "advapi32.dll" &&
        (function == "OpenProcessToken" ||
         function == "LookupPrivilegeValueW" ||
         function == "AdjustTokenPrivileges")) {
        static_assert(sizeof(LUID) == 8);
        static_assert(offsetof(TOKEN_PRIVILEGES, Privileges) == 4);
        static_assert(sizeof(LUID_AND_ATTRIBUTES) == 12);
        const auto writable = [&memory](std::uint64_t address,
                                        std::uint64_t size) {
            if (address == 0 || size > UINT64_MAX - address) return false;
            const auto region = memory.query_region(address);
            return region && region->writable &&
                   address + size <= region->base + region->size;
        };
        const auto readable = [&memory](std::uint64_t address,
                                        std::uint64_t size) {
            if (address == 0 || size > UINT64_MAX - address) return false;
            const auto region = memory.query_region(address);
            return region && region->readable &&
                   address + size <= region->base + region->size;
        };
        const auto host_handle = [this](std::uint64_t guest) {
            if (guest == UINT64_MAX) return GetCurrentProcess();
            std::lock_guard lock(state_->mutex);
            const auto found = state_->host_handles.find(guest);
            if (found == state_->host_handles.end()) {
                SetLastError(ERROR_INVALID_HANDLE);
                return static_cast<HANDLE>(nullptr);
            }
            return found->second.value;
        };
        if (function == "LookupPrivilegeValueW") {
            if (state.x[1] == 0 || !writable(state.x[2], sizeof(LUID))) {
                SetLastError(state.x[1] == 0 ? ERROR_INVALID_PARAMETER
                                             : ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto system = state.x[0] == 0 ? std::wstring{} :
                guest_string(memory, state.x[0], true);
            const auto name = guest_string(memory, state.x[1], true);
            LUID value{};
            const auto success = LookupPrivilegeValueW(
                state.x[0] == 0 ? nullptr : system.c_str(),
                name.c_str(), &value);
            const auto error = GetLastError();
            if (success) {
                memory.write32(state.x[2], value.LowPart);
                memory.write32(state.x[2] + 4,
                               static_cast<std::uint32_t>(value.HighPart));
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "OpenProcessToken") {
            if (!writable(state.x[2], sizeof(std::uint64_t))) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto process = host_handle(state.x[0]);
            if (process == nullptr) {
                state.x[0] = 0;
                return std::nullopt;
            }
            HANDLE token = nullptr;
            const auto success = OpenProcessToken(
                process, static_cast<DWORD>(state.x[1]), &token);
            const auto error = GetLastError();
            std::uint64_t guest_token = 0;
            if (success) {
                std::lock_guard lock(state_->mutex);
                guest_token = state_->next_host_handle;
                state_->next_host_handle += 8;
                state_->host_handles.emplace(guest_token,
                                             State::HostHandle{token, true});
            }
            memory.write64(state.x[2], guest_token);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        const auto token = host_handle(state.x[0]);
        if (token == nullptr) {
            state.x[0] = 0;
            return std::nullopt;
        }
        const bool disable_all = static_cast<BOOL>(state.x[1]) != FALSE;
        const auto new_state = state.x[2];
        const auto buffer_length = static_cast<DWORD>(state.x[3]);
        const auto previous_state = state.x[4];
        const auto return_length = state.x[5];
        if (state.x[3] != buffer_length || buffer_length > 1024U * 1024U ||
            (previous_state != 0 && return_length == 0) ||
            (return_length != 0 && !writable(return_length, 4)) ||
            (previous_state != 0 && buffer_length != 0 &&
             !writable(previous_state, buffer_length)) ||
            (!disable_all && !readable(new_state, 4))) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        std::vector<std::uint64_t> input_words;
        TOKEN_PRIVILEGES* input = nullptr;
        if (!disable_all) {
            const auto count = memory.read32(new_state);
            if (count > 1024 || !readable(new_state, 4ULL + count * 12ULL)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            input_words.resize((std::max<std::uint64_t>(
                sizeof(TOKEN_PRIVILEGES), 4ULL + count * 12ULL) + 7) / 8);
            input = reinterpret_cast<TOKEN_PRIVILEGES*>(input_words.data());
            input->PrivilegeCount = count;
            for (DWORD index = 0; index < count; ++index) {
                const auto address = new_state + 4ULL + index * 12ULL;
                input->Privileges[index].Luid.LowPart = memory.read32(address);
                input->Privileges[index].Luid.HighPart = static_cast<LONG>(
                    memory.read32(address + 4));
                input->Privileges[index].Attributes = memory.read32(address + 8);
            }
        }
        std::vector<std::uint64_t> previous_words;
        TOKEN_PRIVILEGES* previous = nullptr;
        if (previous_state != 0) {
            previous_words.resize((std::max<std::uint64_t>(
                sizeof(TOKEN_PRIVILEGES), buffer_length) + 7) / 8);
            previous = reinterpret_cast<TOKEN_PRIVILEGES*>(previous_words.data());
        }
        DWORD required = 0;
        const auto success = AdjustTokenPrivileges(
            token, disable_all, input, buffer_length, previous,
            return_length == 0 ? nullptr : &required);
        const auto error = GetLastError();
        if (return_length != 0) memory.write32(return_length, required);
        if (success && previous != nullptr) {
            const auto count = previous->PrivilegeCount;
            if (count > 1024 || 4ULL + count * 12ULL > buffer_length) {
                throw std::runtime_error(
                    "host token previous-state length is inconsistent");
            }
            memory.write32(previous_state, count);
            for (DWORD index = 0; index < count; ++index) {
                const auto address = previous_state + 4ULL + index * 12ULL;
                memory.write32(address,
                               previous->Privileges[index].Luid.LowPart);
                memory.write32(address + 4, static_cast<std::uint32_t>(
                    previous->Privileges[index].Luid.HighPart));
                memory.write32(address + 8,
                               previous->Privileges[index].Attributes);
            }
        }
        state.x[0] = static_cast<std::uint32_t>(success);
        SetLastError(error);
        return std::nullopt;
    }
    if ((dll == "api-ms-win-shcore-scaling-l1-1-1.dll" ||
         dll == "shcore.dll") &&
        (function == "SetProcessDpiAwareness" ||
         function == "GetDpiForMonitor")) {
        if (function == "SetProcessDpiAwareness") {
            HRESULT result = E_INVALIDARG;
            if (state.x[0] <= static_cast<std::uint64_t>(
                                  PROCESS_PER_MONITOR_DPI_AWARE)) {
                std::lock_guard lock(state_->mutex);
                if (state_->guest_dpi_awareness_set) {
                    result = E_ACCESSDENIED;
                } else {
                    state_->guest_dpi_awareness_set = true;
                    state_->guest_dpi_awareness =
                        static_cast<PROCESS_DPI_AWARENESS>(state.x[0]);
                    // The guest has its own once-per-process state. Try to
                    // align native window scaling without inheriting a DPI
                    // setting from the translator executable or test host.
                    static_cast<void>(::SetProcessDpiAwareness(
                        state_->guest_dpi_awareness));
                    result = S_OK;
                }
            }
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        const auto writable = [&memory](std::uint64_t address) {
            if (address == 0 || address > UINT64_MAX - 4) return false;
            const auto region = memory.query_region(address);
            return region && region->writable &&
                   address + 4 <= region->base + region->size;
        };
        if (state.x[0] == 0 ||
            state.x[1] > static_cast<std::uint64_t>(MDT_RAW_DPI) ||
            !writable(state.x[2]) || !writable(state.x[3])) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        UINT dpi_x = 0;
        UINT dpi_y = 0;
        const auto result = ::GetDpiForMonitor(
            reinterpret_cast<HMONITOR>(state.x[0]),
            static_cast<MONITOR_DPI_TYPE>(state.x[1]), &dpi_x, &dpi_y);
        if (SUCCEEDED(result)) {
            memory.write32(state.x[2], dpi_x);
            memory.write32(state.x[3], dpi_y);
        }
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "user32.dll" && function == "SetProcessDPIAware") {
        BOOL success = FALSE;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->guest_dpi_awareness_set) {
                state_->guest_dpi_awareness_set = true;
                state_->guest_dpi_awareness = PROCESS_SYSTEM_DPI_AWARE;
                static_cast<void>(::SetProcessDpiAwareness(
                    PROCESS_SYSTEM_DPI_AWARE));
                success = TRUE;
            }
        }
        state.x[0] = static_cast<std::uint32_t>(success);
        if (!success) SetLastError(ERROR_ACCESS_DENIED);
        return std::nullopt;
    }
    if (dll == "user32.dll" &&
        (function == "GetKeyboardLayout" ||
         function == "GetKeyboardState" ||
         function == "GetKeyState" ||
         function == "GetAsyncKeyState" ||
         function == "MapVirtualKeyA" ||
         function == "MapVirtualKeyW" ||
         function == "ToUnicode" ||
         function == "ToUnicodeEx" ||
         function == "VkKeyScanExA")) {
        const auto guest_range = [&memory](std::uint64_t address,
                                           std::uint64_t size, bool writable) {
            if (address == 0 || size > UINT64_MAX - address) return false;
            const auto region = memory.query_region(address);
            return region && (writable ? region->writable : region->readable) &&
                   address + size <= region->base + region->size;
        };
        if (function == "GetKeyboardLayout") {
            state.x[0] = reinterpret_cast<std::uintptr_t>(
                ::GetKeyboardLayout(static_cast<DWORD>(state.x[0])));
            return std::nullopt;
        }
        if (function == "GetKeyboardState") {
            if (!guest_range(state.x[0], 256, true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::array<BYTE, 256> keys{};
            const auto success = ::GetKeyboardState(keys.data());
            const auto error = GetLastError();
            if (success) {
                for (std::size_t index = 0; index < keys.size(); ++index) {
                    memory.write8(state.x[0] + index, keys[index]);
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetKeyState" || function == "GetAsyncKeyState") {
            const auto key = static_cast<int>(static_cast<std::uint32_t>(
                state.x[0]));
            const auto result = function == "GetKeyState"
                ? ::GetKeyState(key) : ::GetAsyncKeyState(key);
            state.x[0] = static_cast<std::uint16_t>(result);
            return std::nullopt;
        }
        if (function == "MapVirtualKeyA" || function == "MapVirtualKeyW") {
            state.x[0] = function == "MapVirtualKeyA"
                ? ::MapVirtualKeyA(static_cast<UINT>(state.x[0]),
                                   static_cast<UINT>(state.x[1]))
                : ::MapVirtualKeyW(static_cast<UINT>(state.x[0]),
                                   static_cast<UINT>(state.x[1]));
            return std::nullopt;
        }
        if (function == "VkKeyScanExA") {
            const auto result = ::VkKeyScanExA(
                static_cast<CHAR>(state.x[0]),
                reinterpret_cast<HKL>(state.x[1]));
            state.x[0] = static_cast<std::uint16_t>(result);
            return std::nullopt;
        }
        const auto capacity = static_cast<std::int32_t>(
            static_cast<std::uint32_t>(state.x[4]));
        if (capacity < 0 || capacity > 4096) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        if (!guest_range(state.x[2], 256, false) ||
            (capacity != 0 && !guest_range(state.x[3],
                                           static_cast<std::uint64_t>(capacity) * 2,
                                           true))) {
            SetLastError(ERROR_NOACCESS);
            state.x[0] = 0;
            return std::nullopt;
        }
        std::array<BYTE, 256> keys{};
        for (std::size_t index = 0; index < keys.size(); ++index) {
            keys[index] = memory.read8(state.x[2] + index);
        }
        std::vector<wchar_t> output(static_cast<std::size_t>(
            std::max(capacity, 1)));
        const auto result = function == "ToUnicodeEx"
            ? ::ToUnicodeEx(static_cast<UINT>(state.x[0]),
                            static_cast<UINT>(state.x[1]), keys.data(),
                            output.data(), capacity,
                            static_cast<UINT>(state.x[5]),
                            reinterpret_cast<HKL>(state.x[6]))
            : ::ToUnicode(static_cast<UINT>(state.x[0]),
                          static_cast<UINT>(state.x[1]), keys.data(),
                          output.data(), capacity,
                          static_cast<UINT>(state.x[5]));
        const auto written = std::min<std::size_t>(
            static_cast<std::size_t>(std::abs(result)),
            static_cast<std::size_t>(capacity));
        for (std::size_t index = 0; index < written; ++index) {
            const auto character = static_cast<std::uint16_t>(output[index]);
            memory.write8(state.x[3] + index * 2,
                          static_cast<std::uint8_t>(character));
            memory.write8(state.x[3] + index * 2 + 1,
                          static_cast<std::uint8_t>(character >> 8));
        }
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "user32.dll" &&
        (function == "LoadIconA" || function == "LoadIconW" ||
         function == "LoadCursorA" || function == "LoadCursorW" ||
         function == "LoadImageA" || function == "LoadImageW" ||
         function == "DestroyIcon" || function == "DestroyCursor" ||
         function == "SetCursor")) {
        if (function == "SetCursor") {
            std::lock_guard lock(state_->mutex);
            HCURSOR host_cursor = nullptr;
            if (state.x[0] != 0) {
                const auto found = state_->image_handles.find(state.x[0]);
                if (found == state_->image_handles.end() ||
                    !found->second.cursor) {
                    SetLastError(ERROR_INVALID_HANDLE);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                host_cursor = reinterpret_cast<HCURSOR>(found->second.host);
            }
            const auto thread = GetCurrentThreadId();
            const auto previous = state_->current_cursors[thread];
            ::SetCursor(host_cursor);
            state_->current_cursors[thread] = state.x[0];
            state.x[0] = previous;
            return std::nullopt;
        }
        if (function == "DestroyIcon" || function == "DestroyCursor") {
            const bool cursor = function == "DestroyCursor";
            std::lock_guard lock(state_->mutex);
            const auto found = state_->image_handles.find(state.x[0]);
            if (found == state_->image_handles.end() || found->second.shared ||
                found->second.cursor != cursor) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto success = cursor
                ? ::DestroyCursor(reinterpret_cast<HCURSOR>(found->second.host))
                : ::DestroyIcon(found->second.host);
            const auto error = GetLastError();
            if (success) {
                for (auto& [thread, current] : state_->current_cursors) {
                    static_cast<void>(thread);
                    if (current == state.x[0]) current = 0;
                }
                state_->image_handles.erase(found);
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        const bool load_icon = function.starts_with("LoadIcon");
        const bool load_cursor = function.starts_with("LoadCursor");
        const bool fixed_size = load_icon || load_cursor;
        const bool cursor = load_cursor ||
            (!load_icon && state.x[2] == IMAGE_CURSOR);
        const bool wide = function.ends_with('W');
        if (!fixed_size && state.x[2] != IMAGE_ICON &&
            state.x[2] != IMAGE_CURSOR) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto flags = fixed_size
            ? static_cast<UINT>(LR_SHARED | LR_DEFAULTSIZE)
            : static_cast<UINT>(state.x[5]);
        if ((!fixed_size && state.x[5] != flags) ||
            (state.x[0] != 0 && (flags & LR_LOADFROMFILE) != 0)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto x_metric = cursor ? SM_CXCURSOR : SM_CXICON;
        const auto y_metric = cursor ? SM_CYCURSOR : SM_CYICON;
        int width = fixed_size ? ::GetSystemMetrics(x_metric)
                               : static_cast<int>(static_cast<std::uint32_t>(state.x[3]));
        int height = fixed_size ? ::GetSystemMetrics(y_metric)
                                : static_cast<int>(static_cast<std::uint32_t>(state.x[4]));
        if ((flags & LR_DEFAULTSIZE) != 0) {
            if (width == 0) width = ::GetSystemMetrics(x_metric);
            if (height == 0) height = ::GetSystemMetrics(y_metric);
        }
        const auto module_handle = state.x[0];
        const bool shared = (flags & LR_SHARED) != 0;
        std::optional<std::uint16_t> resource_id;
        std::u16string resource_name;
        std::wstring wide_name;
        if (state.x[1] <= 0xffff) {
            resource_id = static_cast<std::uint16_t>(state.x[1]);
        } else {
            try {
                if (wide) {
                    wide_name = guest_string(memory, state.x[1], true);
                } else {
                    const auto ansi_name = guest_ansi_string(memory, state.x[1]);
                    const auto length = ::MultiByteToWideChar(
                        CP_ACP, 0, ansi_name.c_str(), -1, nullptr, 0);
                    if (length <= 0 || length > 32768) {
                        state.x[0] = 0;
                        return std::nullopt;
                    }
                    wide_name.resize(static_cast<std::size_t>(length));
                    if (::MultiByteToWideChar(CP_ACP, 0, ansi_name.c_str(),
                                              -1, wide_name.data(), length) != length) {
                        state.x[0] = 0;
                        return std::nullopt;
                    }
                    wide_name.pop_back();
                }
            } catch (const std::runtime_error&) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            resource_name.reserve(wide_name.size());
            for (const auto character : wide_name) {
                resource_name.push_back(static_cast<char16_t>(character));
            }
        }
        const PeImage* guest_module = nullptr;
        if (module_handle != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->guest_modules.find(module_handle);
            if (found == state_->guest_modules.end()) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            guest_module = found->second;
        }
        std::lock_guard lock(state_->mutex);
        if (shared) {
            for (const auto& [handle, image] : state_->image_handles) {
                if (image.shared && image.cursor == cursor &&
                    image.module == module_handle &&
                    image.id == resource_id && image.name == resource_name &&
                    image.width == width && image.height == height &&
                    image.flags == flags) {
                    state.x[0] = handle;
                    return std::nullopt;
                }
            }
        }
        HICON host_icon = nullptr;
        bool owned = false;
        if (guest_module != nullptr) {
            const auto group_type = static_cast<std::uint16_t>(
                cursor ? 12 : 14);
            const auto group = resource_id
                ? guest_module->find_resource(group_type, *resource_id)
                : guest_module->find_resource(group_type, resource_name);
            if (!group) {
                SetLastError(ERROR_RESOURCE_NAME_NOT_FOUND);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto read16 = [&group](std::size_t offset) {
                return static_cast<std::uint16_t>((*group)[offset] |
                    (static_cast<std::uint16_t>((*group)[offset + 1]) << 8));
            };
            if (group->size() < 6 || read16(0) != 0 ||
                read16(2) != (cursor ? 2 : 1) ||
                read16(4) == 0 || read16(4) > 4096 ||
                group->size() < 6ULL + static_cast<std::size_t>(read16(4)) * 14) {
                SetLastError(ERROR_INVALID_DATA);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto image_id = ::LookupIconIdFromDirectoryEx(
                const_cast<BYTE*>(group->data()), cursor ? FALSE : TRUE,
                width, height,
                flags & ~LR_SHARED);
            if (image_id == 0) {
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto image = guest_module->find_resource(
                static_cast<std::uint16_t>(cursor ? 1 : 3),
                static_cast<std::uint16_t>(image_id));
            if (!image || image->empty()) {
                SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
                state.x[0] = 0;
                return std::nullopt;
            }
            host_icon = ::CreateIconFromResourceEx(
                const_cast<BYTE*>(image->data()),
                static_cast<DWORD>(image->size()), cursor ? FALSE : TRUE,
                0x00030000,
                width, height, flags & ~LR_SHARED);
            owned = host_icon != nullptr;
        } else {
            const auto* name = resource_id
                ? MAKEINTRESOURCEW(*resource_id) : wide_name.c_str();
            host_icon = load_icon ? ::LoadIconW(nullptr, name)
                : load_cursor
                    ? reinterpret_cast<HICON>(::LoadCursorW(nullptr, name))
                    : static_cast<HICON>(::LoadImageW(nullptr, name,
                        cursor ? IMAGE_CURSOR : IMAGE_ICON,
                        width, height, flags));
            owned = host_icon != nullptr && !shared;
        }
        const auto error = GetLastError();
        if (host_icon == nullptr) {
            state.x[0] = 0;
            SetLastError(error);
            return std::nullopt;
        }
        const auto handle = state_->next_image_handle;
        if (handle > UINT64_MAX - 16) {
            if (owned) {
                if (cursor) ::DestroyCursor(reinterpret_cast<HCURSOR>(host_icon));
                else ::DestroyIcon(host_icon);
            }
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            state.x[0] = 0;
            return std::nullopt;
        }
        state_->next_image_handle += 16;
        state_->image_handles.emplace(handle,
            State::ImageHandle{host_icon, cursor, shared, owned,
                               module_handle, resource_id,
                               std::move(resource_name), width, height, flags});
        state.x[0] = handle;
        SetLastError(error);
        return std::nullopt;
    }
    if (dll == "user32.dll" &&
        (function == "RegisterClassA" || function == "RegisterClassW" ||
         function == "RegisterClassExA" || function == "RegisterClassExW" ||
         function == "UnregisterClassA" || function == "UnregisterClassW")) {
        static_assert(sizeof(WNDCLASSW) == 72 &&
                      sizeof(WNDCLASSEXW) == 80);
        const bool wide = function.ends_with('W');
        const bool unregister = function.starts_with("Unregister");
        const bool extended = function.starts_with("RegisterClassEx");
        if (unregister) {
            std::wstring name;
            if (state.x[0] > 0xffff) {
                name = guest_string(memory, state.x[0], wide);
            }
            std::lock_guard lock(state_->mutex);
            auto found = state_->window_classes.end();
            for (auto entry = state_->window_classes.begin();
                 entry != state_->window_classes.end(); ++entry) {
                if ((state.x[0] <= 0xffff && entry->first == state.x[0]) ||
                    (state.x[0] > 0xffff && entry->second.guest_name == name &&
                     entry->second.guest_instance == state.x[1])) {
                    found = entry;
                    break;
                }
            }
            if (found == state_->window_classes.end()) {
                SetLastError(ERROR_CLASS_DOES_NOT_EXIST);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto success = ::UnregisterClassW(
                found->second.host_name.c_str(), GetModuleHandleW(nullptr));
            const auto error = GetLastError();
            if (success) state_->window_classes.erase(found);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        const auto base = state.x[0];
        const auto size = extended ? 80ULL : 72ULL;
        const auto region = base == 0 || base > UINT64_MAX - size
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(base);
        if (!region || !region->readable ||
            base + size > region->base + region->size ||
            (extended && memory.read32(base) != size)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto guest_proc = memory.read64(base + 8);
        const auto guest_instance = memory.read64(base + 24);
        const auto guest_icon = memory.read64(base + 32);
        const auto guest_cursor = memory.read64(base + 40);
        const auto guest_background = memory.read64(base + 48);
        const auto guest_menu = memory.read64(base + 56);
        const auto guest_class_name = memory.read64(base + 64);
        const auto guest_small_icon = extended ? memory.read64(base + 72) : 0;
        if (guest_class_name <= 0xffff || guest_proc == 0 || guest_menu != 0 ||
            memory.read32(base + 16) > 4096 ||
            memory.read32(base + 20) > 4096) {
            SetLastError(guest_menu != 0 ? ERROR_CALL_NOT_IMPLEMENTED
                                          : ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto class_name = guest_string(memory, guest_class_name, wide);
        std::lock_guard lock(state_->mutex);
        if (guest_instance != 0 &&
            !state_->guest_modules.contains(guest_instance)) {
            SetLastError(ERROR_INVALID_HANDLE);
            state.x[0] = 0;
            return std::nullopt;
        }
        bool executable_proc = false;
        for (const auto& [module_base, module] : state_->guest_modules) {
            if (guest_proc < module_base ||
                guest_proc - module_base >= module->size()) continue;
            for (const auto& section : module->sections()) {
                if (section.executable &&
                    guest_proc - module_base >= section.rva &&
                    guest_proc - module_base <
                        static_cast<std::uint64_t>(section.rva) + section.size) {
                    executable_proc = true;
                }
            }
        }
        if (!executable_proc || !state_->guest_window_callback) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto host_image = [this](std::uint64_t handle, bool cursor)
            -> HICON {
            if (handle == 0) return nullptr;
            const auto found = state_->image_handles.find(handle);
            if (found == state_->image_handles.end() ||
                found->second.cursor != cursor) return nullptr;
            return found->second.host;
        };
        const auto icon = host_image(guest_icon, false);
        const auto cursor = host_image(guest_cursor, true);
        const auto small_icon = host_image(guest_small_icon, false);
        if ((guest_icon != 0 && icon == nullptr) ||
            (guest_cursor != 0 && cursor == nullptr) ||
            (guest_small_icon != 0 && small_icon == nullptr)) {
            SetLastError(ERROR_INVALID_HANDLE);
            state.x[0] = 0;
            return std::nullopt;
        }
        HBRUSH background = nullptr;
        if (guest_background != 0) {
            if (guest_background <= COLOR_MENUBAR + 1) {
                background = reinterpret_cast<HBRUSH>(guest_background);
            } else if (const auto found = state_->gdi_objects.find(
                           guest_background);
                       found != state_->gdi_objects.end() &&
                       found->second.type == OBJ_BRUSH) {
                background = reinterpret_cast<HBRUSH>(found->second.host);
            } else {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
        }
        WNDCLASSEXW native{};
        native.cbSize = static_cast<UINT>(sizeof(native));
        native.style = memory.read32(base + (extended ? 4 : 0));
        native.lpfnWndProc = guest_window_procedure;
        native.cbClsExtra = static_cast<int>(memory.read32(base + 16));
        native.cbWndExtra = static_cast<int>(memory.read32(base + 20));
        native.hInstance = GetModuleHandleW(nullptr);
        native.hIcon = icon;
        native.hCursor = reinterpret_cast<HCURSOR>(cursor);
        native.hbrBackground = background;
        const auto host_name = L"TransmuteWindowClass_" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(this)) + L"_" +
            std::to_wstring(state_->window_classes.size() + 1);
        native.lpszClassName = host_name.c_str();
        native.hIconSm = small_icon;
        const auto atom = ::RegisterClassExW(&native);
        const auto error = GetLastError();
        if (atom != 0) {
            state_->window_classes.emplace(atom, State::WindowClass{
                atom, host_name, class_name, guest_instance, guest_proc,
                !wide, memory.read32(base + 20)});
        }
        state.x[0] = atom;
        SetLastError(error);
        return std::nullopt;
    }
    if (dll == "user32.dll" &&
        (function == "CreateWindowExA" || function == "CreateWindowExW" ||
         function == "DestroyWindow" || function == "DefWindowProcA" ||
         function == "DefWindowProcW")) {
        const auto native_window = [this](std::uint64_t guest) -> HWND {
            const auto found = state_->windows.find(guest);
            return found == state_->windows.end() ? nullptr : found->second->host;
        };
        const auto callback_failure = [this]() {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->window_failures.find(GetCurrentThreadId());
            if (found == state_->window_failures.end()) return std::string{};
            auto message = std::move(found->second);
            state_->window_failures.erase(found);
            return message;
        };
        if (function == "DestroyWindow") {
            std::shared_ptr<GuestWindowContext> context;
            HWND host = nullptr;
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->windows.find(state.x[0]);
                if (found != state_->windows.end()) context = found->second;
                host = native_window(state.x[0]);
            }
            if (!context || host == nullptr) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto success = ::DestroyWindow(host);
            const auto error = GetLastError();
            const auto failure = callback_failure();
            if (!failure.empty()) {
                throw std::runtime_error("guest window procedure failed: " + failure);
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function.starts_with("DefWindowProc")) {
            std::shared_ptr<GuestWindowContext> context;
            GuestWindowContext* active = nullptr;
            HWND host = nullptr;
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->windows.find(state.x[0]);
                if (found != state_->windows.end()) context = found->second;
                host = native_window(state.x[0]);
            }
            if (host == nullptr) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            active = context.get();
            if (active == nullptr) {
                std::lock_guard lock(native_window_mutex);
                const auto found = native_windows.find(host);
                if (found != native_windows.end()) active = found->second.get();
            }
            auto parameter = static_cast<LPARAM>(state.x[3]);
            if (active != nullptr &&
                (state.x[1] == WM_NCCREATE || state.x[1] == WM_CREATE ||
                 state.x[1] == WM_GETMINMAXINFO) &&
                (state.x[3] == active->scratch ||
                 state.x[3] == active->scratch + 128)) {
                parameter = active->active_host_lparam;
            }
            state.x[0] = static_cast<std::uint64_t>(::DefWindowProcW(
                host, static_cast<UINT>(state.x[1]),
                static_cast<WPARAM>(state.x[2]), parameter));
            return std::nullopt;
        }
        const bool wide = function.ends_with('W');
        const auto stack = state.sp;
        const auto stack_region = stack == 0 || stack > UINT64_MAX - 32
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(stack);
        if (!stack_region || !stack_region->readable ||
            stack + 32 > stack_region->base + stack_region->size ||
            state.x[2] <= 0xffff && state.x[2] != 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto guest_parent = memory.read64(stack);
        const auto guest_menu = memory.read64(stack + 8);
        const auto guest_instance = memory.read64(stack + 16);
        const auto guest_parameter = memory.read64(stack + 24);
        const auto requested_class = state.x[1] > 0xffff
            ? guest_string(memory, state.x[1], wide) : std::wstring{};
        const auto window_name = state.x[2] == 0
            ? std::wstring{} : guest_string(memory, state.x[2], wide);
        State::WindowClass registration{};
        const auto native_static = state.x[1] > 0xffff && _wcsicmp(requested_class.c_str(), L"STATIC") == 0;
        bool system_class = false;
        HWND parent = nullptr;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = std::find_if(
                state_->window_classes.begin(), state_->window_classes.end(),
                [&](const auto& entry) {
                    return state.x[1] <= 0xffff
                        ? entry.first == state.x[1]
                        : entry.second.guest_name == requested_class;
                });
            if (found == state_->window_classes.end() && !native_static) {
                SetLastError(ERROR_CLASS_DOES_NOT_EXIST);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (found != state_->window_classes.end()) registration = found->second;
            else {
                system_class = true;
                registration.host_name = L"STATIC";
                registration.guest_name = requested_class;
                registration.guest_instance = guest_instance;
            }
            if (guest_instance != 0 &&
                guest_instance != registration.guest_instance) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (guest_parent != 0) parent = native_window(guest_parent);
            if (guest_parent != 0 && parent == nullptr) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
        }
        if (guest_menu != 0 &&
            (guest_parent == 0 || guest_menu > 0xffff)) {
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            state.x[0] = 0;
            return std::nullopt;
        }
        auto context = std::make_shared<GuestWindowContext>();
        context->bridge = this;
        context->memory = &memory;
        context->guest_proc = registration.guest_proc;
        context->guest_instance = guest_instance;
        context->guest_parent = guest_parent;
        context->guest_menu = guest_menu;
        context->guest_parameter = guest_parameter;
        context->guest_class_name = state.x[1];
        context->guest_window_name = state.x[2];
        context->registered_class_name = registration.guest_name;
        context->extra_bytes = registration.extra_bytes;
        context->ansi = !wide;
        {
            std::lock_guard lock(state_->mutex);
            context->invoke = state_->guest_window_callback;
            context->guest_handle = state_->next_window_handle;
            state_->next_window_handle += 16;
            context->scratch = state_->next_window_scratch;
            state_->next_window_scratch += 4096;
        }
        if (!system_class) {
            memory.map_memory(context->scratch, 4096);
            context->scratch_mapped = true;
        }
        const auto host = ::CreateWindowExW(
            static_cast<DWORD>(state.x[0]), registration.host_name.c_str(),
            window_name.c_str(), static_cast<DWORD>(state.x[3]),
            static_cast<int>(static_cast<std::uint32_t>(state.x[4])),
            static_cast<int>(static_cast<std::uint32_t>(state.x[5])),
            static_cast<int>(static_cast<std::uint32_t>(state.x[6])),
            static_cast<int>(static_cast<std::uint32_t>(state.x[7])),
            parent, reinterpret_cast<HMENU>(guest_menu),
            GetModuleHandleW(nullptr), system_class ? nullptr : context.get());
        const auto error = GetLastError();
        const auto failure = callback_failure();
        if (host == nullptr) {
            if (context->scratch_mapped) {
                memory.unmap_memory(context->scratch);
                context->scratch_mapped = false;
            }
            {
                std::lock_guard lock(state_->mutex);
                state_->host_windows.erase(host);
            }
            if (!failure.empty()) {
                throw std::runtime_error("guest window procedure failed: " +
                                         failure);
            }
            state.x[0] = 0;
            SetLastError(error);
            return std::nullopt;
        }
        {
            std::lock_guard lock(state_->mutex);
            context->host = host;
            state_->host_windows[host] = context->guest_handle;
            state_->windows.emplace(context->guest_handle, context);
        }
        if (system_class) {
            context->native_proc = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(host, GWLP_WNDPROC));
            context->extra_bytes = static_cast<std::uint32_t>(::GetClassLongW(host, GCL_CBWNDEXTRA));
            {
                std::lock_guard lock(native_window_mutex);
                native_windows.emplace(host, context);
            }
            if (::SetWindowLongPtrW(host, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(guest_window_procedure)) == 0) {
                ::DestroyWindow(host);
                std::lock_guard lock(state_->mutex);
                state_->host_windows.erase(host);
                state_->windows.erase(context->guest_handle);
                std::lock_guard native_lock(native_window_mutex);
                native_windows.erase(host);
                throw std::runtime_error("could not track the native STATIC window lifetime");
            }
        }
        if (!failure.empty()) {
            ::DestroyWindow(host);
            throw std::runtime_error("guest window procedure failed: " +
                                     failure);
        }
        state.x[0] = context->guest_handle;
        SetLastError(error);
        return std::nullopt;
    }
    if (dll == "gdi32.dll" &&
        (function == "GetStockObject" || function == "DeleteObject" ||
         function == "CreateSolidBrush" || function == "CreateCompatibleDC" ||
         function == "DeleteDC" || function == "SelectObject" ||
         function == "GetObjectA")) {
        static_assert(sizeof(LOGBRUSH) == 16 &&
                      offsetof(LOGBRUSH, lbHatch) == 8);
        static_assert(sizeof(LOGPEN) == 16);
        static_assert(sizeof(LOGFONTA) == 60);
        static_assert(sizeof(BITMAP) == 32 && offsetof(BITMAP, bmBits) == 24);
        std::lock_guard lock(state_->mutex);
        const auto new_handle = [this]() {
            const auto handle = state_->next_gdi_handle;
            if (handle > UINT64_MAX - 16) {
                throw std::runtime_error("guest GDI handle space exhausted");
            }
            state_->next_gdi_handle += 16;
            return handle;
        };
        if (function == "GetStockObject") {
            const auto host = ::GetStockObject(static_cast<int>(
                static_cast<std::uint32_t>(state.x[0])));
            if (host == nullptr) {
                state.x[0] = 0;
                return std::nullopt;
            }
            for (const auto& [guest, object] : state_->gdi_objects) {
                if (object.host == host) {
                    state.x[0] = guest;
                    return std::nullopt;
                }
            }
            const auto guest = new_handle();
            state_->gdi_objects.emplace(guest, State::GdiObject{
                host, ::GetObjectType(host), false, 0});
            state.x[0] = guest;
            return std::nullopt;
        }
        if (function == "CreateSolidBrush") {
            const auto host = ::CreateSolidBrush(static_cast<COLORREF>(state.x[0]));
            if (host == nullptr) {
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto guest = new_handle();
            state_->gdi_objects.emplace(guest, State::GdiObject{
                host, OBJ_BRUSH, true, 0});
            state.x[0] = guest;
            return std::nullopt;
        }
        if (function == "DeleteObject") {
            const auto found = state_->gdi_objects.find(state.x[0]);
            if (found == state_->gdi_objects.end() || !found->second.owned) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto success = ::DeleteObject(found->second.host);
            const auto error = GetLastError();
            if (success) state_->gdi_objects.erase(found);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "CreateCompatibleDC") {
            HDC source = nullptr;
            if (state.x[0] != 0) {
                const auto found = state_->gdi_dcs.find(state.x[0]);
                if (found == state_->gdi_dcs.end()) {
                    SetLastError(ERROR_INVALID_HANDLE);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                source = found->second.host;
            }
            const auto host = ::CreateCompatibleDC(source);
            if (host == nullptr) {
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto guest = new_handle();
            state_->gdi_dcs.emplace(guest, State::GdiDc{host});
            state.x[0] = guest;
            return std::nullopt;
        }
        if (function == "DeleteDC") {
            const auto found = state_->gdi_dcs.find(state.x[0]);
            if (found == state_->gdi_dcs.end() || found->second.window_dc) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto dc_handle = state.x[0];
            const auto success = ::DeleteDC(found->second.host);
            const auto error = GetLastError();
            if (success) {
                state_->gdi_dcs.erase(found);
                for (auto object = state_->gdi_objects.begin();
                     object != state_->gdi_objects.end();) {
                    object = object->second.borrowed_dc == dc_handle
                        ? state_->gdi_objects.erase(object) : ++object;
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "SelectObject") {
            const auto dc = state_->gdi_dcs.find(state.x[0]);
            const auto object = state_->gdi_objects.find(state.x[1]);
            if (dc == state_->gdi_dcs.end() ||
                object == state_->gdi_objects.end()) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto kind = object->second.type;
            if (kind != OBJ_BRUSH && kind != OBJ_PEN &&
                kind != OBJ_FONT && kind != OBJ_BITMAP) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto previous = ::SelectObject(dc->second.host,
                                                  object->second.host);
            if (previous == nullptr || previous == HGDI_ERROR) {
                state.x[0] = 0;
                return std::nullopt;
            }
            for (const auto& [guest, known] : state_->gdi_objects) {
                if (known.host == previous) {
                    state.x[0] = guest;
                    return std::nullopt;
                }
            }
            std::uint64_t borrowed_dc = state.x[0];
            for (int index = 0; index <= DC_PEN; ++index) {
                if (::GetStockObject(index) == previous) {
                    borrowed_dc = 0;
                    break;
                }
            }
            const auto guest = new_handle();
            state_->gdi_objects.emplace(guest, State::GdiObject{
                previous, ::GetObjectType(previous), false, borrowed_dc});
            state.x[0] = guest;
            return std::nullopt;
        }
        const auto object = state_->gdi_objects.find(state.x[0]);
        if (object == state_->gdi_objects.end()) {
            SetLastError(ERROR_INVALID_HANDLE);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto count = state.x[1];
        if (count > 4096 || (state.x[2] != 0 && (state.x[2] & 3) != 0)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        std::array<std::uint8_t, sizeof(LOGFONTA)> bytes{};
        std::size_t size = 0;
        switch (object->second.type) {
        case OBJ_BRUSH: {
            LOGBRUSH brush{};
            if (::GetObjectA(object->second.host,
                             static_cast<int>(sizeof(brush)), &brush) == 0 ||
                (brush.lbStyle != BS_SOLID && brush.lbStyle != BS_NULL &&
                 brush.lbStyle != BS_HATCHED)) {
                SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::memcpy(bytes.data(), &brush, sizeof(brush));
            size = sizeof(brush);
            break;
        }
        case OBJ_PEN: {
            LOGPEN pen{};
            if (::GetObjectA(object->second.host,
                             static_cast<int>(sizeof(pen)), &pen) == 0) {
                state.x[0] = 0;
                return std::nullopt;
            }
            std::memcpy(bytes.data(), &pen, sizeof(pen));
            size = sizeof(pen);
            break;
        }
        case OBJ_FONT: {
            LOGFONTA font{};
            if (::GetObjectA(object->second.host,
                             static_cast<int>(sizeof(font)), &font) == 0) {
                state.x[0] = 0;
                return std::nullopt;
            }
            std::memcpy(bytes.data(), &font, sizeof(font));
            size = sizeof(font);
            break;
        }
        case OBJ_BITMAP: {
            BITMAP bitmap{};
            if (::GetObjectA(object->second.host,
                             static_cast<int>(sizeof(bitmap)), &bitmap) == 0) {
                state.x[0] = 0;
                return std::nullopt;
            }
            if (bitmap.bmBits != nullptr) {
                SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::memcpy(bytes.data(), &bitmap, sizeof(bitmap));
            size = sizeof(bitmap);
            break;
        }
        default:
            SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
            state.x[0] = 0;
            return std::nullopt;
        }
        if (state.x[2] == 0) {
            state.x[0] = size;
            return std::nullopt;
        }
        if (count < size) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto region = memory.query_region(state.x[2]);
        if (!region || !region->writable ||
            size > UINT64_MAX - state.x[2] ||
            state.x[2] + size > region->base + region->size) {
            SetLastError(ERROR_NOACCESS);
            state.x[0] = 0;
            return std::nullopt;
        }
        for (std::size_t index = 0; index < size; ++index) {
            memory.write8(state.x[2] + index, bytes[index]);
        }
        state.x[0] = size;
        return std::nullopt;
    }
    if (dll == "user32.dll" &&
        (function == "RegisterRawInputDevices" ||
         function == "GetRawInputData" ||
         function == "GetRawInputDeviceInfoA" ||
         function == "GetRawInputDeviceList")) {
        static_assert(sizeof(RAWINPUTDEVICE) == 16);
        static_assert(sizeof(RAWINPUTDEVICELIST) == 16);
        static_assert(sizeof(RAWINPUTHEADER) == 24);
        static_assert(offsetof(RAWINPUT, data) == 24);
        constexpr std::uint64_t maximum_raw_bytes = 16ULL * 1024 * 1024;
        const auto guest_range = [&memory](std::uint64_t address,
                                           std::uint64_t size, bool write) {
            if (address == 0 || size > UINT64_MAX - address) return false;
            const auto region = memory.query_region(address);
            return region && region->readable &&
                   (!write || region->writable) &&
                   address + size <= region->base + region->size;
        };
        if (function == "RegisterRawInputDevices") {
            const auto count = static_cast<std::uint32_t>(state.x[1]);
            if (state.x[1] != count || state.x[2] != sizeof(RAWINPUTDEVICE) ||
                count > 1024 ||
                (count != 0 && !guest_range(state.x[0],
                                             static_cast<std::uint64_t>(count) * 16,
                                             false))) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::vector<RAWINPUTDEVICE> devices(count);
            for (std::uint32_t index = 0; index < count; ++index) {
                const auto address = state.x[0] +
                                     static_cast<std::uint64_t>(index) * 16;
                const auto usage = memory.read32(address);
                devices[index].usUsagePage =
                    static_cast<USHORT>(usage);
                devices[index].usUsage =
                    static_cast<USHORT>(usage >> 16);
                devices[index].dwFlags = memory.read32(address + 4);
                if (memory.read64(address + 8) != 0) {
                    SetLastError(ERROR_INVALID_HANDLE);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                devices[index].hwndTarget = nullptr;
            }
            const auto success = ::RegisterRawInputDevices(
                count == 0 ? nullptr : devices.data(), count,
                sizeof(RAWINPUTDEVICE));
            const auto error = GetLastError();
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetRawInputDeviceList") {
            const auto count_address = state.x[1];
            if (state.x[2] != sizeof(RAWINPUTDEVICELIST) ||
                !guest_range(count_address, 4, true)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
            UINT count = memory.read32(count_address);
            const auto capacity = count;
            if (state.x[0] != 0 &&
                (count > maximum_raw_bytes / sizeof(RAWINPUTDEVICELIST) ||
                 (count != 0 && !guest_range(state.x[0],
                    static_cast<std::uint64_t>(count) *
                        sizeof(RAWINPUTDEVICELIST), true)))) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
            std::vector<RAWINPUTDEVICELIST> devices(
                state.x[0] == 0 ? 0 : std::max<UINT>(capacity, 1));
            const auto result = ::GetRawInputDeviceList(
                state.x[0] == 0 ? nullptr : devices.data(), &count,
                sizeof(RAWINPUTDEVICELIST));
            const auto error = GetLastError();
            if (result != UINT32_MAX && state.x[0] != 0) {
                if (result > capacity) {
                    throw std::runtime_error(
                        "host raw-input device count exceeded guest capacity");
                }
                for (UINT index = 0; index < result; ++index) {
                    {
                        std::lock_guard lock(state_->mutex);
                        state_->raw_device_handles.insert(
                            reinterpret_cast<std::uintptr_t>(
                                devices[index].hDevice));
                    }
                    write_guest_structure(memory,
                        state.x[0] + static_cast<std::uint64_t>(index) * 16,
                        devices[index]);
                }
            }
            memory.write32(count_address, count);
            state.x[0] = result;
            SetLastError(error);
            return std::nullopt;
        }
        const bool input_data = function == "GetRawInputData";
        const auto output = state.x[2];
        const auto count_address = state.x[3];
        if (!guest_range(count_address, 4, true)) {
            SetLastError(ERROR_NOACCESS);
            state.x[0] = UINT32_MAX;
            return std::nullopt;
        }
        if (input_data && state.x[4] != sizeof(RAWINPUTHEADER)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = UINT32_MAX;
            return std::nullopt;
        }
        {
            std::lock_guard lock(state_->mutex);
            const auto& known = input_data ? state_->raw_input_handles
                                            : state_->raw_device_handles;
            if (!known.contains(state.x[0])) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
        }
        UINT count = memory.read32(count_address);
        if (output != 0 &&
            (count > maximum_raw_bytes ||
             (count != 0 && !guest_range(output, count, true)))) {
            SetLastError(ERROR_NOACCESS);
            state.x[0] = UINT32_MAX;
            return std::nullopt;
        }
        std::vector<std::uint64_t> buffer(
            output == 0 ? 0 : (std::max<UINT>(count, 1) + 7) / 8);
        auto* bytes = reinterpret_cast<std::uint8_t*>(buffer.data());
        if (!input_data && output != 0 &&
            static_cast<UINT>(state.x[1]) == RIDI_DEVICEINFO && count >= 4) {
            const auto size = memory.read32(output);
            std::memcpy(bytes, &size, sizeof(size));
        }
        const auto result = input_data
            ? ::GetRawInputData(reinterpret_cast<HRAWINPUT>(state.x[0]),
                static_cast<UINT>(state.x[1]),
                output == 0 ? nullptr : bytes, &count,
                sizeof(RAWINPUTHEADER))
            : ::GetRawInputDeviceInfoA(
                reinterpret_cast<HANDLE>(state.x[0]),
                static_cast<UINT>(state.x[1]),
                output == 0 ? nullptr : bytes, &count);
        const auto error = GetLastError();
        if (result != UINT32_MAX && output != 0) {
            if (result > buffer.size() * sizeof(std::uint64_t) ||
                result > memory.read32(count_address)) {
                throw std::runtime_error(
                    "host raw-input result exceeded guest capacity");
            }
            for (UINT index = 0; index < result; ++index) {
                memory.write8(output + index, bytes[index]);
            }
        }
        memory.write32(count_address, count);
        state.x[0] = result;
        SetLastError(error);
        return std::nullopt;
    }
    if (dll == "comctl32.dll" &&
        (function == "#17" || function == "InitCommonControls" ||
         function == "#83" || function == "InitCommonControlsEx")) {
        static_assert(sizeof(INITCOMMONCONTROLSEX) == 8);
        if (function == "#17" || function == "InitCommonControls") {
            ::InitCommonControls();
            return std::nullopt;
        }
        const auto address = state.x[0];
        const auto region = address == 0 || address > UINT64_MAX - 8
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(address);
        if (!region || !region->readable ||
            address + 8 > region->base + region->size) {
            SetLastError(ERROR_NOACCESS);
            state.x[0] = 0;
            return std::nullopt;
        }
        INITCOMMONCONTROLSEX requested{};
        requested.dwSize = memory.read32(address);
        requested.dwICC = memory.read32(address + 4);
        const auto success = ::InitCommonControlsEx(&requested);
        const auto error = GetLastError();
        state.x[0] = static_cast<std::uint32_t>(success);
        SetLastError(error);
        return std::nullopt;
    }
    if (dll == "oleaut32.dll" &&
        (function == "#2" || function == "SysAllocString" ||
         function == "#6" || function == "SysFreeString" ||
         function == "#9" || function == "VariantClear")) {
        if (function == "#2" || function == "SysAllocString") {
            if (state.x[0] == 0) return std::nullopt;
            std::wstring value;
            try {
                value = guest_string(memory, state.x[0], true);
            } catch (const std::runtime_error&) {
                state.x[0] = 0;
                return std::nullopt;
            }
            state.x[0] = allocate_guest_bstr(memory, value);
            return std::nullopt;
        }
        if (function == "#6" || function == "SysFreeString") {
            if (!free_guest_bstr(memory, state.x[0])) {
                throw std::runtime_error("invalid guest BSTR pointer");
            }
            return std::nullopt;
        }
        if (!guest_range(memory, state.x[0], sizeof(VARIANT), true)) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        const auto address = state.x[0];
        const auto type = static_cast<VARTYPE>(memory.read32(address) & 0xffffU);
        if (type == VT_BSTR &&
            !free_guest_bstr(memory, memory.read64(address + 8))) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        if (type != VT_BSTR && type != VT_EMPTY && type != VT_NULL &&
            type != VT_I2 && type != VT_I4 && type != VT_UI2 &&
            type != VT_UI4 && type != VT_BOOL && type != VT_R4 &&
            type != VT_R8) {
            state.x[0] = static_cast<std::uint32_t>(DISP_E_BADVARTYPE);
            return std::nullopt;
        }
        for (std::size_t index = 0; index < sizeof(VARIANT); ++index) {
            memory.write8(address + index, 0);
        }
        state.x[0] = S_OK;
        return std::nullopt;
    }
    if ((dll == "ole32.dll" || dll == "combase.dll") &&
        function == "CoCreateInstance") {
        if (!guest_range(memory, state.x[0], sizeof(GUID), false) ||
            !guest_range(memory, state.x[3], sizeof(GUID), false) ||
            !guest_range(memory, state.x[4], 8, true) ||
            state.x[2] > UINT32_MAX) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        const auto read_guid = [&](std::uint64_t address) {
            GUID value{};
            auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
            for (std::size_t index = 0; index < sizeof(value); ++index) {
                bytes[index] = memory.read8(address + index);
            }
            return value;
        };
        const auto class_id = read_guid(state.x[0]);
        const auto interface_id = read_guid(state.x[3]);
        const auto output = state.x[4];
        memory.write64(output, 0);
        if (IsEqualGUID(class_id, __uuidof(TaskbarList)) ||
            IsEqualGUID(class_id, __uuidof(MMDeviceEnumerator)) ||
            IsEqualGUID(class_id, __uuidof(DirectManipulationManager))) {
            // Taskbar integration and WASAPI audio discovery are optional.
            // Return failure until these interfaces have translated vtables.
            state.x[0] = static_cast<std::uint32_t>(E_NOINTERFACE);
            return std::nullopt;
        }
        if (!IsEqualGUID(class_id, __uuidof(Shell)) ||
            (!IsEqualGUID(interface_id, __uuidof(IShellDispatch)) &&
             !IsEqualGUID(interface_id, IID_IDispatch))) {
            throw std::runtime_error("unsupported guest COM class or interface");
        }
        if (state.x[1] != 0) {
            state.x[0] = static_cast<std::uint32_t>(CLASS_E_NOAGGREGATION);
            return std::nullopt;
        }
        IShellDispatch* native = nullptr;
        const auto hr = ::CoCreateInstance(class_id, nullptr,
            static_cast<DWORD>(state.x[2]), __uuidof(IShellDispatch),
            reinterpret_cast<void**>(&native));
        if (FAILED(hr)) {
            state.x[0] = static_cast<std::uint32_t>(hr);
            return std::nullopt;
        }
        const auto wrapped = wrap_shell_automation(
            memory, static_cast<IDispatch*>(native), shell_dispatch_kind);
        if (wrapped == 0) {
            state.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
            return std::nullopt;
        }
        memory.write64(output, wrapped);
        state.x[0] = static_cast<std::uint32_t>(hr);
        return std::nullopt;
    }
    if ((dll == "ole32.dll" || dll == "combase.dll") &&
        (function == "OleInitialize" || function == "OleUninitialize" ||
         function == "CoInitialize" || function == "CoInitializeEx" ||
         function == "CoUninitialize")) {
        const bool ole = function.starts_with("Ole");
        const bool initialize = function.ends_with("Initialize") ||
                                function == "CoInitializeEx";
        const auto thread = GetCurrentThreadId();
        if (!initialize) {
            bool release = false;
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->com_apartments.find(thread);
                if (found != state_->com_apartments.end()) {
                    auto& depth = ole ? found->second.ole_depth
                                      : found->second.co_depth;
                    if (depth != 0) {
                        --depth;
                        release = true;
                    }
                    if (found->second.ole_depth == 0 &&
                        found->second.co_depth == 0) {
                        state_->com_apartments.erase(found);
                    }
                }
            }
            if (release) {
                if (ole) ::OleUninitialize();
                else ::CoUninitialize();
            }
            return std::nullopt;
        }
        const auto flags = function == "CoInitializeEx"
            ? static_cast<DWORD>(state.x[1])
            : static_cast<DWORD>(COINIT_APARTMENTTHREADED);
        constexpr DWORD valid_flags = COINIT_APARTMENTTHREADED |
                                      COINIT_DISABLE_OLE1DDE |
                                      COINIT_SPEED_OVER_MEMORY;
        if (state.x[0] != 0 || (flags & ~valid_flags) != 0) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        const bool apartment_threaded = ole ||
            function == "CoInitialize" ||
            (flags & COINIT_APARTMENTTHREADED) != 0;
        bool already_initialized = false;
        {
            std::lock_guard lock(state_->mutex);
            if (const auto found = state_->com_apartments.find(thread);
                found != state_->com_apartments.end()) {
                already_initialized = true;
                if (found->second.apartment_threaded != apartment_threaded) {
                    state.x[0] = static_cast<std::uint32_t>(
                        RPC_E_CHANGED_MODE);
                    return std::nullopt;
                }
            }
        }
        const auto result = ole ? ::OleInitialize(nullptr)
            : function == "CoInitialize" ? ::CoInitialize(nullptr)
            : ::CoInitializeEx(nullptr, static_cast<COINIT>(flags));
        if (SUCCEEDED(result)) {
            std::lock_guard lock(state_->mutex);
            auto& apartment = state_->com_apartments[thread];
            apartment.apartment_threaded = apartment_threaded;
            if (ole) ++apartment.ole_depth;
            else ++apartment.co_depth;
            state.x[0] = already_initialized ? S_FALSE : S_OK;
        } else {
            state.x[0] = static_cast<std::uint32_t>(result);
        }
        return std::nullopt;
    }
    if ((dll == "ole32.dll" || dll == "combase.dll") &&
        (function == "CoTaskMemAlloc" || function == "CoTaskMemFree")) {
        if (function == "CoTaskMemFree") {
            if (state.x[0] == 0) return std::nullopt;
            std::lock_guard lock(state_->mutex);
            if (state_->com_task_allocations.erase(state.x[0]) == 0) {
                throw std::runtime_error("invalid guest COM task-memory pointer");
            }
            memory.unmap_memory(state.x[0]);
            return std::nullopt;
        }
        constexpr std::uint64_t maximum_com_allocation = 128ULL * 1024 * 1024;
        const auto requested = state.x[0];
        if (requested > maximum_com_allocation) {
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto size = static_cast<std::size_t>(std::max<std::uint64_t>(
            requested, 1));
        std::lock_guard lock(state_->mutex);
        auto address = state_->next_com_task_address;
        const auto stride = (size + 15) & ~std::size_t{15};
        if (address > UINT64_MAX - stride - 16 ||
            !memory.is_range_available(address, size)) {
            state.x[0] = 0;
            return std::nullopt;
        }
        memory.map_memory(address, size);
        state_->com_task_allocations.emplace(address, size);
        state_->next_com_task_address = address + stride + 16;
        state.x[0] = address;
        return std::nullopt;
    }
    if (dll == "shell32.dll" &&
        (function == "SetCurrentProcessExplicitAppUserModelID" ||
         function == "GetCurrentProcessExplicitAppUserModelID")) {
        const auto address = state.x[0];
        const auto width = function.starts_with("Get") ? 8ULL : 2ULL;
        const auto region = address == 0 || address > UINT64_MAX - width
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(address);
        if (!region || address + width > region->base + region->size ||
            (function.starts_with("Get") ? !region->writable
                                        : !region->readable)) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        if (function.starts_with("Set")) {
            const auto id = guest_string(memory, address, true);
            const auto result = ::SetCurrentProcessExplicitAppUserModelID(
                id.c_str());
            if (SUCCEEDED(result)) {
                std::lock_guard lock(state_->mutex);
                state_->explicit_app_id = id;
                state_->explicit_app_id_set = true;
            }
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        memory.write64(address, 0);
        std::wstring id;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->explicit_app_id_set) {
                state.x[0] = static_cast<std::uint32_t>(E_FAIL);
                return std::nullopt;
            }
            id = state_->explicit_app_id;
        }
        CpuState allocation{};
        allocation.x[0] = (id.size() + 1) * sizeof(wchar_t);
        static_cast<void>(dispatch({"ole32.dll", "CoTaskMemAlloc", 0},
                                   allocation, memory));
        if (allocation.x[0] == 0) {
            state.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
            return std::nullopt;
        }
        auto cursor = allocation.x[0];
        for (const auto character : id) {
            memory.write8(cursor++, static_cast<std::uint8_t>(character));
            memory.write8(cursor++, static_cast<std::uint8_t>(character >> 8));
        }
        memory.write8(cursor++, 0);
        memory.write8(cursor, 0);
        memory.write64(address, allocation.x[0]);
        state.x[0] = S_OK;
        return std::nullopt;
    }
    if (dll == "shell32.dll" &&
        (function == "SHGetSpecialFolderPathA" ||
         function == "SHGetSpecialFolderPathW" ||
         function == "SHGetFolderPathA" || function == "SHGetFolderPathW")) {
        const bool wide = function.ends_with('W');
        const bool special = function.starts_with("SHGetSpecial");
        const auto output = state.x[1 + (special ? 0 : 3)];
        const auto output_bytes = static_cast<std::uint64_t>(MAX_PATH) *
                                  (wide ? 2 : 1);
        const auto region = output == 0 || output > UINT64_MAX - output_bytes
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(output);
        const auto fail = [&](DWORD error) {
            SetLastError(error);
            state.x[0] = special ? 0 :
                static_cast<std::uint32_t>(HRESULT_FROM_WIN32(error));
        };
        if (!region || !region->writable ||
            output + output_bytes > region->base + region->size) {
            fail(ERROR_INVALID_PARAMETER);
            return std::nullopt;
        }
        HWND window = nullptr;
        if (state.x[0] != 0) {
            std::lock_guard lock(state_->mutex);
            for (const auto& [host, guest] : state_->host_windows) {
                if (guest == state.x[0]) {
                    window = host;
                    break;
                }
            }
            if (window == nullptr) {
                fail(ERROR_INVALID_HANDLE);
                return std::nullopt;
            }
        }
        HANDLE token = nullptr;
        if (!special && state.x[2] != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->host_handles.find(state.x[2]);
            if (found == state_->host_handles.end()) {
                fail(ERROR_INVALID_HANDLE);
                return std::nullopt;
            }
            token = found->second.value;
        }
        const auto csidl = static_cast<int>(
            static_cast<std::uint32_t>(state.x[special ? 2 : 1]));
        if (wide) {
            wchar_t path[MAX_PATH]{};
            const auto result = special
                ? (::SHGetSpecialFolderPathW(window, path, csidl,
                      state.x[3] != 0) ? S_OK : E_FAIL)
                : ::SHGetFolderPathW(window, csidl, token,
                      static_cast<DWORD>(state.x[3]), path);
            if (SUCCEEDED(result)) {
                const auto length = wcsnlen_s(path, MAX_PATH);
                if (length < MAX_PATH) {
                    for (std::size_t index = 0; index <= length; ++index) {
                        const auto character = static_cast<std::uint16_t>(path[index]);
                        memory.write8(output + index * 2,
                                      static_cast<std::uint8_t>(character));
                        memory.write8(output + index * 2 + 1,
                                      static_cast<std::uint8_t>(character >> 8));
                    }
                    state.x[0] = special ? 1 :
                        static_cast<std::uint32_t>(result);
                    return std::nullopt;
                }
            }
            memory.write8(output, 0);
            memory.write8(output + 1, 0);
            state.x[0] = special ? 0 : static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        char path[MAX_PATH]{};
        const auto result = special
            ? (::SHGetSpecialFolderPathA(window, path, csidl,
                  state.x[3] != 0) ? S_OK : E_FAIL)
            : ::SHGetFolderPathA(window, csidl, token,
                  static_cast<DWORD>(state.x[3]), path);
        if (SUCCEEDED(result)) {
            const auto length = strnlen_s(path, MAX_PATH);
            if (length < MAX_PATH) {
                for (std::size_t index = 0; index <= length; ++index) {
                    memory.write8(output + index,
                                  static_cast<std::uint8_t>(path[index]));
                }
                state.x[0] = special ? 1 : static_cast<std::uint32_t>(result);
                return std::nullopt;
            }
        }
        memory.write8(output, 0);
        state.x[0] = special ? 0 : static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "shell32.dll" && function == "SHGetDesktopFolder") {
        if (!guest_range(memory, state.x[0], 8, true)) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        memory.write64(state.x[0], 0);
        auto* engine = dynamic_cast<ReferenceEngine*>(&memory);
        if (engine == nullptr) {
            state.x[0] = static_cast<std::uint32_t>(E_NOTIMPL);
            return std::nullopt;
        }
        IShellFolder* host = nullptr;
        const auto result = ::SHGetDesktopFolder(&host);
        if (FAILED(result)) {
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        std::lock_guard lock(state_->mutex);
        if (state_->shell_vtable == 0) {
            if (!memory.is_range_available(shell_vtable_base, guest_page_size)) {
                host->Release();
                state.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                return std::nullopt;
            }
            memory.map_memory(shell_vtable_base, guest_page_size);
            state_->shell_vtable = shell_vtable_base;
            for (unsigned slot = 0; slot < 13; ++slot) {
                const auto callback = engine->register_guest_call(
                    [this, slot](CpuState& call, GuestMemory& guest) {
                        std::lock_guard folder_lock(state_->mutex);
                        const auto found = state_->shell_folders.find(call.x[0]);
                        if (found == state_->shell_folders.end()) {
                            call.x[0] = static_cast<std::uint32_t>(E_POINTER);
                            return;
                        }
                        auto& folder = found->second;
                        if (folder.owner != GetCurrentThreadId()) {
                            call.x[0] = static_cast<std::uint32_t>(RPC_E_WRONG_THREAD);
                            return;
                        }
                        if (slot == 1) {
                            call.x[0] = ++folder.references;
                            return;
                        }
                        if (slot == 2) {
                            const auto remaining = --folder.references;
                            if (remaining == 0) {
                                folder.host->Release();
                                guest.unmap_memory(found->first);
                                state_->shell_folders.erase(found);
                            }
                            call.x[0] = remaining;
                            return;
                        }
                        if (slot == 0) {
                            const auto iid_address = call.x[1];
                            const auto output = call.x[2];
                            if (!guest_range(guest, iid_address, sizeof(GUID), false) ||
                                !guest_range(guest, output, 8, true)) {
                                call.x[0] = static_cast<std::uint32_t>(E_POINTER);
                                return;
                            }
                            guest.write64(output, 0);
                            GUID requested{};
                            auto* bytes = reinterpret_cast<std::uint8_t*>(&requested);
                            for (std::size_t index = 0; index < sizeof(GUID); ++index) {
                                bytes[index] = guest.read8(iid_address + index);
                            }
                            if (!IsEqualGUID(requested, IID_IUnknown) &&
                                !IsEqualGUID(requested, __uuidof(IShellFolder))) {
                                call.x[0] = static_cast<std::uint32_t>(E_NOINTERFACE);
                                return;
                            }
                            ++folder.references;
                            guest.write64(output, found->first);
                            call.x[0] = S_OK;
                            return;
                        }
                        const auto allocate_wide = [&](const std::wstring& value) {
                            CpuState allocation{};
                            allocation.x[0] = (value.size() + 1) * 2;
                            static_cast<void>(dispatch(
                                {"ole32.dll", "CoTaskMemAlloc", 0}, allocation, guest));
                            if (allocation.x[0] != 0) {
                                for (std::size_t index = 0; index <= value.size(); ++index) {
                                    guest.write8(allocation.x[0] + index * 2,
                                        static_cast<std::uint8_t>(value[index]));
                                    guest.write8(allocation.x[0] + index * 2 + 1,
                                        static_cast<std::uint8_t>(value[index] >> 8));
                                }
                            }
                            return allocation.x[0];
                        };
                        if (slot == 3) {
                            if (call.x[1] != 0 || call.x[2] != 0 ||
                                call.x[3] == 0 ||
                                !guest_range(guest, call.x[5], 8, true) ||
                                (call.x[4] != 0 &&
                                 !guest_range(guest, call.x[4], 4, true)) ||
                                (call.x[6] != 0 &&
                                 !guest_range(guest, call.x[6], 4, true))) {
                                call.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                                return;
                            }
                            guest.write64(call.x[5], 0);
                            std::wstring path;
                            try {
                                path = guest_string(guest, call.x[3], true);
                            } catch (const std::runtime_error&) {
                                call.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                                return;
                            }
                            ULONG eaten = 0;
                            ULONG attributes = call.x[6] == 0 ? 0 :
                                guest.read32(call.x[6]);
                            PIDLIST_RELATIVE native_pidl = nullptr;
                            const auto hr = folder.host->ParseDisplayName(
                                nullptr, nullptr, path.data(), &eaten,
                                &native_pidl, call.x[6] == 0 ? nullptr : &attributes);
                            if (call.x[4] != 0) guest.write32(call.x[4], eaten);
                            if (call.x[6] != 0) guest.write32(call.x[6], attributes);
                            if (FAILED(hr)) {
                                CoTaskMemFree(native_pidl);
                                call.x[0] = static_cast<std::uint32_t>(hr);
                                return;
                            }
                            const auto size = native_pidl == nullptr ? 0U :
                                ILGetSize(native_pidl);
                            if (size < 2 || size > 65536) {
                                CoTaskMemFree(native_pidl);
                                call.x[0] = static_cast<std::uint32_t>(E_FAIL);
                                return;
                            }
                            CpuState allocation{};
                            allocation.x[0] = size;
                            static_cast<void>(dispatch(
                                {"ole32.dll", "CoTaskMemAlloc", 0}, allocation, guest));
                            if (allocation.x[0] == 0) {
                                CoTaskMemFree(native_pidl);
                                call.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                                return;
                            }
                            const auto* source = reinterpret_cast<const std::uint8_t*>(
                                native_pidl);
                            for (std::size_t index = 0; index < size; ++index) {
                                guest.write8(allocation.x[0] + index, source[index]);
                            }
                            CoTaskMemFree(native_pidl);
                            guest.write64(call.x[5], allocation.x[0]);
                            call.x[0] = static_cast<std::uint32_t>(hr);
                            return;
                        }
                        if (slot == 11) {
                            if (!guest_range(guest, call.x[3], sizeof(STRRET), true)) {
                                call.x[0] = static_cast<std::uint32_t>(E_POINTER);
                                return;
                            }
                            const auto pidl = guest_pidl(guest, call.x[1]);
                            if (!pidl || call.x[2] > UINT32_MAX) {
                                call.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                                return;
                            }
                            const auto* native_pidl = reinterpret_cast<PCUITEMID_CHILD>(
                                pidl->data());
                            STRRET native_result{};
                            const auto hr = folder.host->GetDisplayNameOf(
                                native_pidl, static_cast<SHGDNF>(call.x[2]),
                                &native_result);
                            if (FAILED(hr)) {
                                call.x[0] = static_cast<std::uint32_t>(hr);
                                return;
                            }
                            PWSTR native_name = nullptr;
                            const auto conversion = ::StrRetToStrW(
                                &native_result, native_pidl, &native_name);
                            if (FAILED(conversion)) {
                                call.x[0] = static_cast<std::uint32_t>(conversion);
                                return;
                            }
                            const std::wstring name(native_name);
                            CoTaskMemFree(native_name);
                            const auto guest_name = allocate_wide(name);
                            if (guest_name == 0) {
                                call.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
                                return;
                            }
                            for (std::size_t index = 0; index < sizeof(STRRET); ++index) {
                                guest.write8(call.x[3] + index, 0);
                            }
                            guest.write32(call.x[3], STRRET_WSTR);
                            guest.write64(call.x[3] + 8, guest_name);
                            call.x[0] = static_cast<std::uint32_t>(hr);
                            return;
                        }
                        call.x[0] = static_cast<std::uint32_t>(E_NOTIMPL);
                    });
                memory.write64(shell_vtable_base + slot * 8, callback);
            }
        }
        const auto object = state_->next_shell_folder;
        if (object > UINT64_MAX - guest_page_size ||
            !memory.is_range_available(object, 16)) {
            host->Release();
            state.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
            return std::nullopt;
        }
        memory.map_memory(object, 16);
        memory.write64(object, state_->shell_vtable);
        state_->shell_folders.emplace(object,
            State::ShellFolder{host, GetCurrentThreadId(), 1});
        state_->next_shell_folder += guest_page_size;
        memory.write64(state.x[0], object);
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "shlwapi.dll" && function == "StrRetToStrW") {
        const auto input = state.x[0];
        const auto output = state.x[2];
        if (!guest_range(memory, input, sizeof(STRRET), true) ||
            !guest_range(memory, output, 8, true)) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        memory.write64(output, 0);
        const auto type = memory.read32(input);
        std::wstring value;
        if (type == STRRET_WSTR) {
            const auto source = memory.read64(input + 8);
            if (source == 0) {
                state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                return std::nullopt;
            }
            {
                std::lock_guard lock(state_->mutex);
                if (!state_->com_task_allocations.contains(source)) {
                    state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                    return std::nullopt;
                }
            }
            try {
                value = guest_string(memory, source, true);
            } catch (const std::runtime_error&) {
                state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                return std::nullopt;
            }
        } else if (type == STRRET_CSTR || type == STRRET_OFFSET) {
            const auto pidl = guest_pidl(memory, state.x[1]);
            if (type == STRRET_OFFSET && !pidl) {
                state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                return std::nullopt;
            }
            STRRET native{};
            native.uType = type;
            if (type == STRRET_CSTR) {
                for (std::size_t index = 0; index < sizeof(native.cStr); ++index) {
                    native.cStr[index] = static_cast<char>(memory.read8(input + 8 + index));
                }
            } else {
                native.uOffset = memory.read32(input + 8);
            }
            PWSTR native_value = nullptr;
            const auto hr = ::StrRetToStrW(&native,
                pidl ? reinterpret_cast<PCUITEMID_CHILD>(pidl->data()) : nullptr,
                &native_value);
            if (FAILED(hr)) {
                state.x[0] = static_cast<std::uint32_t>(hr);
                return std::nullopt;
            }
            value = native_value;
            CoTaskMemFree(native_value);
        } else {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        CpuState allocation{};
        allocation.x[0] = (value.size() + 1) * 2;
        static_cast<void>(dispatch({"ole32.dll", "CoTaskMemAlloc", 0},
                                   allocation, memory));
        if (allocation.x[0] == 0) {
            state.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
            return std::nullopt;
        }
        for (std::size_t index = 0; index <= value.size(); ++index) {
            memory.write8(allocation.x[0] + index * 2,
                          static_cast<std::uint8_t>(value[index]));
            memory.write8(allocation.x[0] + index * 2 + 1,
                          static_cast<std::uint8_t>(value[index] >> 8));
        }
        if (type == STRRET_WSTR) {
            CpuState release{};
            release.x[0] = memory.read64(input + 8);
            static_cast<void>(dispatch({"ole32.dll", "CoTaskMemFree", 0},
                                       release, memory));
            memory.write64(input + 8, 0);
        }
        memory.write64(output, allocation.x[0]);
        state.x[0] = S_OK;
        return std::nullopt;
    }
    if (dll == "shell32.dll" && function == "SHGetKnownFolderPath") {
        if (state.x[0] == 0 || state.x[3] == 0 ||
            state.x[1] > UINT32_MAX || state.x[0] > UINT64_MAX - sizeof(GUID) ||
            state.x[3] > UINT64_MAX - 8) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        const auto input = memory.query_region(state.x[0]);
        const auto output = memory.query_region(state.x[3]);
        if (!input || !input->readable ||
            state.x[0] + sizeof(GUID) > input->base + input->size ||
            !output || !output->writable ||
            state.x[3] + 8 > output->base + output->size) {
            state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return std::nullopt;
        }
        memory.write64(state.x[3], 0);
        GUID folder{};
        auto* bytes = reinterpret_cast<std::uint8_t*>(&folder);
        for (std::size_t index = 0; index < sizeof(folder); ++index) {
            bytes[index] = memory.read8(state.x[0] + index);
        }
        HANDLE token = nullptr;
        if (state.x[2] != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->host_handles.find(state.x[2]);
            if (found == state_->host_handles.end()) {
                state.x[0] = static_cast<std::uint32_t>(
                    HRESULT_FROM_WIN32(ERROR_INVALID_HANDLE));
                return std::nullopt;
            }
            token = found->second.value;
        }
        PWSTR native_path = nullptr;
        const auto result = SHGetKnownFolderPath(
            folder, static_cast<DWORD>(state.x[1]), token, &native_path);
        const std::wstring path = native_path == nullptr
            ? std::wstring{} : std::wstring(native_path);
        CoTaskMemFree(native_path);
        if (FAILED(result)) {
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        CpuState allocation{};
        allocation.x[0] = (path.size() + 1) * sizeof(wchar_t);
        static_cast<void>(dispatch({"ole32.dll", "CoTaskMemAlloc", 0},
                                   allocation, memory));
        if (allocation.x[0] == 0) {
            state.x[0] = static_cast<std::uint32_t>(E_OUTOFMEMORY);
            return std::nullopt;
        }
        auto cursor = allocation.x[0];
        for (const auto character : path) {
            memory.write8(cursor++, static_cast<std::uint8_t>(character));
            memory.write8(cursor++, static_cast<std::uint8_t>(character >> 8));
        }
        memory.write8(cursor++, 0);
        memory.write8(cursor, 0);
        memory.write64(state.x[3], allocation.x[0]);
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "shell32.dll" && function == "CommandLineToArgvW") {
        if (state.x[0] == 0 || state.x[1] == 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto command_line = guest_string(memory, state.x[0], true);
        std::vector<std::wstring> arguments;
        if (command_line.empty()) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->guest_modules.find(state_->executable_base);
            arguments.push_back(found == state_->guest_modules.end()
                ? std::wstring{} :
                  std::filesystem::path(found->second->source()).wstring());
        } else {
            int count = 0;
            auto** host_arguments = CommandLineToArgvW(command_line.c_str(),
                                                        &count);
            if (host_arguments == nullptr) {
                state.x[0] = 0;
                return std::nullopt;
            }
            arguments.reserve(static_cast<std::size_t>(count));
            for (int index = 0; index < count; ++index) {
                arguments.emplace_back(host_arguments[index]);
            }
            LocalFree(host_arguments);
        }
        std::uint64_t size = (arguments.size() + 1) * 8;
        for (const auto& argument : arguments) {
            size += (argument.size() + 1) * 2;
        }
        CpuState allocation{};
        allocation.x[0] = LMEM_ZEROINIT;
        allocation.x[1] = size;
        static_cast<void>(dispatch({"kernel32.dll", "LocalAlloc", 0},
                                   allocation, memory));
        if (allocation.x[0] == 0) {
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto table = allocation.x[0];
        auto cursor = table + (arguments.size() + 1) * 8;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            memory.write64(table + index * 8, cursor);
            for (const auto character : arguments[index]) {
                memory.write8(cursor++, static_cast<std::uint8_t>(character));
                memory.write8(cursor++, static_cast<std::uint8_t>(character >> 8));
            }
            memory.write8(cursor++, 0);
            memory.write8(cursor++, 0);
        }
        memory.write64(table + arguments.size() * 8, 0);
        memory.write32(state.x[1], static_cast<std::uint32_t>(arguments.size()));
        state.x[0] = table;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-convert-l1-1-0.dll" &&
        (function == "mbstowcs" || function == "wcstombs")) {
        constexpr std::uint64_t maximum_conversion_units = 1024U * 1024U;
        const bool to_wide = function == "mbstowcs";
        const auto destination = state.x[0];
        const auto source = state.x[1];
        const auto count = state.x[2];
        const auto set_guest_errno = [this, &memory](int value) {
            std::lock_guard lock(state_->mutex);
            auto& slot = state_->thread_errno();
            slot.value = value;
            if (slot.address != 0) {
                memory.write32(slot.address, static_cast<std::uint32_t>(value));
            }
        };
        if (source == 0 || count > INT_MAX ||
            (destination != 0 &&
             (count > maximum_conversion_units ||
              !guest_range(memory, destination,
                           count * (to_wide ? 2ULL : 1ULL), true)))) {
            set_guest_errno(EINVAL);
            state.x[0] = UINT64_MAX;
            return std::nullopt;
        }
        std::string narrow_source;
        std::wstring wide_source;
        try {
            if (to_wide) narrow_source = guest_ansi_string(memory, source);
            else wide_source = guest_string(memory, source, true);
        } catch (const std::runtime_error&) {
            set_guest_errno(EINVAL);
            state.x[0] = UINT64_MAX;
            return std::nullopt;
        }
        std::vector<wchar_t> wide_output(to_wide && destination != 0
            ? static_cast<std::size_t>(count) : 0);
        std::vector<char> narrow_output(!to_wide && destination != 0
            ? static_cast<std::size_t>(count) : 0);
        wchar_t empty_wide_output{};
        char empty_narrow_output{};
        int guest_errno = 0;
        {
            std::lock_guard lock(state_->mutex);
            const auto& slot = state_->thread_errno();
            guest_errno = slot.address == 0 ? slot.value :
                static_cast<int>(memory.read32(slot.address));
        }
        const auto saved_host_errno = errno;
        std::size_t result = SIZE_MAX;
        int result_errno = 0;
        {
            std::lock_guard lock(state_->locale_mutex);
            errno = guest_errno;
#pragma warning(push)
#pragma warning(disable: 4996)
            result = to_wide
                ? ::mbstowcs(destination == 0 ? nullptr :
                                 count == 0 ? &empty_wide_output : wide_output.data(),
                             narrow_source.c_str(), static_cast<std::size_t>(count))
                : ::wcstombs(destination == 0 ? nullptr :
                                 count == 0 ? &empty_narrow_output :
                                              narrow_output.data(),
                             wide_source.c_str(), static_cast<std::size_t>(count));
#pragma warning(pop)
            result_errno = errno;
            errno = saved_host_errno;
        }
        set_guest_errno(result_errno);
        if (destination != 0 && result != SIZE_MAX) {
            const auto units = std::min<std::size_t>(
                static_cast<std::size_t>(count), result + 1);
            for (std::size_t index = 0; index < units; ++index) {
                if (to_wide) {
                    const auto character = static_cast<std::uint16_t>(
                        wide_output[index]);
                    memory.write8(destination + index * 2,
                        static_cast<std::uint8_t>(character));
                    memory.write8(destination + index * 2 + 1,
                        static_cast<std::uint8_t>(character >> 8));
                } else {
                    memory.write8(destination + index,
                        static_cast<std::uint8_t>(narrow_output[index]));
                }
            }
        }
        state.x[0] = result;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-convert-l1-1-0.dll" &&
        (function == "_strtoi64" || function == "_strtoui64" ||
         function == "strtol" || function == "strtoul" ||
         function == "strtoll" || function == "strtoull" ||
         function == "strtoimax" || function == "strtoumax" ||
         function == "wcstol" || function == "wcstoul" ||
         function == "wcstoll" || function == "wcstoull" ||
         function == "_wcstoi64" || function == "_wcstoui64" ||
         function == "atoi" || function == "atol" || function == "atoll" ||
         function == "_atoi64" || function == "_wtoi" ||
         function == "_wtol" || function == "_wtoi64")) {
        const auto source = state.x[0];
        const auto end_pointer = state.x[1];
        const bool wide = function.starts_with("wcs") ||
                          function.starts_with("_w");
        const bool simple = function == "atoi" || function == "atol" ||
                            function == "atoll" || function == "_atoi64" ||
                            function == "_wtoi" || function == "_wtol" ||
                            function == "_wtoi64";
        const auto base = simple ? 10 :
            static_cast<std::int32_t>(static_cast<std::uint32_t>(state.x[2]));
        const auto set_guest_errno = [this, &memory](int value) {
            std::lock_guard lock(state_->mutex);
            auto& slot = state_->thread_errno();
            slot.value = value;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(value));
            }
        };
        if (source == 0 || (!simple && base != 0 &&
                            (base < 2 || base > 36))) {
            set_guest_errno(EINVAL);
            if (!simple && end_pointer != 0) memory.write64(end_pointer, source);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto wide_input = wide ? guest_string(memory, source, true)
                                     : std::wstring{};
        const auto narrow_input = wide ? std::string{}
                                       : guest_ansi_string(memory, source);
        int guest_errno{};
        {
            std::lock_guard lock(state_->mutex);
            const auto& slot = state_->thread_errno();
            guest_errno = slot.address == 0 ? slot.value :
                static_cast<std::int32_t>(
                    memory.read32(slot.address));
        }
        const auto saved_host_errno = errno;
        errno = guest_errno;
        std::uint64_t value{};
        std::size_t end_offset{};
        if (wide) {
            const auto& input = wide_input;
            wchar_t* end = nullptr;
            if (function == "wcstol" || function == "_wtol") {
                value = static_cast<std::uint32_t>(
                    simple ? _wtol(input.c_str()) :
                             wcstol(input.c_str(), &end, base));
            } else if (function == "wcstoul") {
                value = static_cast<std::uint32_t>(
                    wcstoul(input.c_str(), &end, base));
            } else if (function == "_wtoi") {
                value = static_cast<std::uint32_t>(_wtoi(input.c_str()));
            } else if (function == "wcstoull" ||
                       function == "_wcstoui64") {
                value = _wcstoui64(input.c_str(), &end, base);
            } else {
                value = static_cast<std::uint64_t>(simple
                    ? _wtoi64(input.c_str())
                    : _wcstoi64(input.c_str(), &end, base));
            }
            if (!simple) end_offset = static_cast<std::size_t>(
                end - input.c_str()) * 2;
        } else {
            const auto& input = narrow_input;
            char* end = nullptr;
            if (function == "strtol" || function == "atol") {
                value = static_cast<std::uint32_t>(simple
                    ? atol(input.c_str())
                    : strtol(input.c_str(), &end, base));
            } else if (function == "strtoul") {
                value = static_cast<std::uint32_t>(
                    strtoul(input.c_str(), &end, base));
            } else if (function == "atoi") {
                value = static_cast<std::uint32_t>(atoi(input.c_str()));
            } else if (function == "strtoull" || function == "strtoumax" ||
                       function == "_strtoui64") {
                value = _strtoui64(input.c_str(), &end, base);
            } else {
                value = static_cast<std::uint64_t>(simple
                    ? _atoi64(input.c_str())
                    : _strtoi64(input.c_str(), &end, base));
            }
            if (!simple) end_offset = static_cast<std::size_t>(
                end - input.c_str());
        }
        const auto result_errno = errno;
        errno = saved_host_errno;
        set_guest_errno(result_errno);
        if (!simple && end_pointer != 0) {
            memory.write64(end_pointer, source + end_offset);
        }
        state.x[0] = value;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-convert-l1-1-0.dll" &&
        (function == "strtod" || function == "strtof" ||
         function == "strtold" || function == "wcstod" ||
         function == "wcstof" || function == "wcstold" ||
         function == "atof" || function == "_wtof")) {
        static_assert(sizeof(long double) == sizeof(double));
        const auto source = state.x[0];
        const auto end_pointer = state.x[1];
        const bool wide = function.starts_with("wcs") || function == "_wtof";
        const bool simple = function == "atof" || function == "_wtof";
        const bool single = function == "strtof" || function == "wcstof";
        const auto set_guest_errno = [this, &memory](int value) {
            std::lock_guard lock(state_->mutex);
            auto& slot = state_->thread_errno();
            slot.value = value;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(value));
            }
        };
        if (source == 0) {
            set_guest_errno(EINVAL);
            if (!simple && end_pointer != 0) memory.write64(end_pointer, 0);
            state.v[0] = {};
            return std::nullopt;
        }
        int guest_errno{};
        {
            std::lock_guard lock(state_->mutex);
            const auto& slot = state_->thread_errno();
            guest_errno = slot.address == 0 ? slot.value :
                static_cast<std::int32_t>(memory.read32(slot.address));
        }
        const auto narrow_input = wide ? std::string{} :
            guest_ansi_string(memory, source);
        const auto wide_input = wide ? guest_string(memory, source, true) :
            std::wstring{};
        const auto saved_host_errno = errno;
        errno = guest_errno;
        double result{};
        float single_result{};
        std::size_t end_offset{};
        if (wide) {
            wchar_t* end = nullptr;
            if (function == "_wtof") result = _wtof(wide_input.c_str());
            else if (single) {
                single_result = wcstof(wide_input.c_str(), &end);
            } else if (function == "wcstold") {
                result = static_cast<double>(wcstold(wide_input.c_str(), &end));
            } else result = wcstod(wide_input.c_str(), &end);
            if (!simple) end_offset = static_cast<std::size_t>(
                end - wide_input.c_str()) * 2;
        } else {
            char* end = nullptr;
            if (function == "atof") result = ::atof(narrow_input.c_str());
            else if (single) {
                single_result = ::strtof(narrow_input.c_str(), &end);
            } else if (function == "strtold") {
                result = static_cast<double>(::strtold(
                    narrow_input.c_str(), &end));
            } else result = ::strtod(narrow_input.c_str(), &end);
            if (!simple) end_offset = static_cast<std::size_t>(
                end - narrow_input.c_str());
        }
        const auto result_errno = errno;
        errno = saved_host_errno;
        set_guest_errno(result_errno);
        if (!simple && end_pointer != 0) {
            if (source > UINT64_MAX - end_offset) {
                throw std::runtime_error("guest floating conversion endptr overflow");
            }
            memory.write64(end_pointer, source + end_offset);
        }
        state.v[0] = {single
            ? static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(single_result))
            : std::bit_cast<std::uint64_t>(result), 0};
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-utility-l1-1-0.dll" &&
        (function == "qsort" || function == "bsearch")) {
        const bool sorting = function == "qsort";
        const auto key = sorting ? 0 : state.x[0];
        const auto base = sorting ? state.x[0] : state.x[1];
        const auto count = sorting ? state.x[1] : state.x[2];
        const auto width = sorting ? state.x[2] : state.x[3];
        const auto comparator = sorting ? state.x[3] : state.x[4];
        if (count == 0) {
            if (!sorting) state.x[0] = 0;
            return std::nullopt;
        }
        if (width == 0 || count > UINT64_MAX / width) {
            throw std::runtime_error("invalid guest CRT sort element size");
        }
        const auto total = count * width;
        const auto guest_range = [&memory](std::uint64_t address,
                                           std::uint64_t length,
                                           bool writable) {
            if (address == 0 || length > UINT64_MAX - address) return false;
            const auto end = address + length;
            for (auto current = address; current < end;) {
                const auto region = memory.query_region(current);
                if (!region || !region->readable ||
                    (writable && !region->writable) ||
                    region->size > UINT64_MAX - region->base) return false;
                const auto region_end = region->base + region->size;
                if (current >= region_end) return false;
                const auto next_page = (current | 4095ULL) + 1;
                const auto next = region->image
                    ? std::min(end, std::min(region_end, next_page))
                    : std::min(end, region_end);
                if (next <= current) return false;
                current = next;
            }
            return true;
        };
        if (!guest_range(base, total, sorting) ||
            (!sorting && !guest_range(key, width, false))) {
            throw std::runtime_error("invalid guest CRT sort/search memory range");
        }
        std::function<std::uint64_t(std::uint64_t, std::uint64_t,
                                    std::uint64_t)> invoke;
        {
            std::lock_guard lock(state_->mutex);
            invoke = state_->guest_binary_callback;
        }
        const auto code = memory.query_region(comparator);
        if (comparator == 0 || !code || !code->executable || !invoke) {
            throw std::runtime_error("invalid guest CRT comparator callback");
        }
        const auto element = [base, width](std::uint64_t index) {
            return base + index * width;
        };
        const auto compare = [&](std::uint64_t left, std::uint64_t right) {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(
                invoke(comparator, left, right)));
        };
        if (!sorting) {
            std::uint64_t low = 0;
            std::uint64_t high = count;
            while (low < high) {
                const auto middle = low + (high - low) / 2;
                const auto relation = compare(key, element(middle));
                if (relation < 0) high = middle;
                else if (relation > 0) low = middle + 1;
                else {
                    state.x[0] = element(middle);
                    return std::nullopt;
                }
            }
            state.x[0] = 0;
            return std::nullopt;
        }
        if (count < 2) return std::nullopt;
        const auto swap_elements = [&](std::uint64_t left,
                                       std::uint64_t right) {
            if (left == right) return;
            const auto first = element(left);
            const auto second = element(right);
            for (std::uint64_t byte = 0; byte < width; ++byte) {
                const auto saved = memory.read8(first + byte);
                memory.write8(first + byte, memory.read8(second + byte));
                memory.write8(second + byte, saved);
            }
        };
        const auto sift_down = [&](std::uint64_t root,
                                   std::uint64_t end) {
            while (root < end / 2) {
                auto child = root * 2 + 1;
                if (child + 1 < end &&
                    compare(element(child), element(child + 1)) < 0) {
                    ++child;
                }
                if (compare(element(root), element(child)) >= 0) break;
                swap_elements(root, child);
                root = child;
            }
        };
        for (auto start = count / 2; start > 0;) {
            sift_down(--start, count);
        }
        for (auto end = count; end > 1;) {
            swap_elements(0, --end);
            sift_down(0, end);
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-utility-l1-1-0.dll" &&
        (function == "rand_s" || function == "rand" ||
         function == "srand")) {
        if (function == "srand") {
            std::lock_guard lock(state_->mutex);
            state_->crt_rand_states[GetCurrentThreadId()] =
                static_cast<std::uint32_t>(state.x[0]);
            return std::nullopt;
        }
        if (function == "rand") {
            std::lock_guard lock(state_->mutex);
            auto& seed = state_->crt_rand_states.try_emplace(
                GetCurrentThreadId(), 1U).first->second;
            seed = seed * 214013U + 2531011U;
            state.x[0] = (seed >> 16) & RAND_MAX;
            return std::nullopt;
        }
        const auto output = state.x[0];
        const auto region = output == 0
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(output);
        if (!region || !region->writable || output > UINT64_MAX - 4 ||
            output + 4 > region->base + region->size) {
            auto& slot = state_->thread_errno();
            slot.value = EINVAL;
            if (slot.address != 0) memory.write32(slot.address, EINVAL);
            state.x[0] = EINVAL;
            return std::nullopt;
        }
        unsigned int random_value = 0;
        const auto result = ::rand_s(&random_value);
        memory.write32(output, result == 0 ? random_value : 0);
        if (result != 0) {
            auto& slot = state_->thread_errno();
            slot.value = result;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(result));
            }
        }
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-time-l1-1-0.dll") {
        static_assert(sizeof(tm) == 36);
        static_assert(sizeof(__timeb64) == 16);
        static_assert(sizeof(_timespec64) == 16);
        const auto ensure_time_storage = [this, &memory] {
            std::lock_guard lock(state_->mutex);
            if (!state_->crt_time_mapped) {
                memory.map_memory(crt_time_base, 4096);
                state_->crt_time_mapped = true;
            }
        };
        if (function == "_time64") {
            const auto now = _time64(nullptr);
            if (state.x[0] != 0) memory.write64(state.x[0], now);
            state.x[0] = static_cast<std::uint64_t>(now);
            return std::nullopt;
        }
        if (function == "_timespec64_get") {
            _timespec64 value{};
            const auto result = _timespec64_get(&value,
                                                 static_cast<int>(state.x[1]));
            if (result != 0) write_guest_structure(memory, state.x[0], value);
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        if (function == "clock") {
            state.x[0] = static_cast<std::uint32_t>(clock());
            return std::nullopt;
        }
        if (function == "_ftime64") {
            __timeb64 value{};
            static_cast<void>(_ftime64_s(&value));
            write_guest_structure(memory, state.x[0], value);
            return std::nullopt;
        }
        if (function == "_gmtime64_s" || function == "_localtime64_s" ||
            function == "_gmtime64" || function == "_localtime64") {
            const bool secure = function.ends_with("_s");
            const bool utc = function.starts_with("_gmtime");
            const auto source = secure ? state.x[1] : state.x[0];
            if (source == 0) {
                state.x[0] = secure ? EINVAL : 0;
                return std::nullopt;
            }
            const auto timestamp = static_cast<__time64_t>(memory.read64(source));
            tm value{};
            const auto error = utc ? _gmtime64_s(&value, &timestamp)
                                   : _localtime64_s(&value, &timestamp);
            if (secure) {
                if (state.x[0] != 0) write_guest_structure(memory, state.x[0], value);
                state.x[0] = static_cast<std::uint32_t>(error);
            } else if (error == 0) {
                ensure_time_storage();
                write_guest_structure(memory, crt_time_base, value);
                state.x[0] = crt_time_base;
            } else {
                state.x[0] = 0;
            }
            return std::nullopt;
        }
        if (function == "_mktime64") {
            auto value = guest_structure<tm>(memory, state.x[0]);
            const auto timestamp = _mktime64(&value);
            write_guest_structure(memory, state.x[0], value);
            state.x[0] = static_cast<std::uint64_t>(timestamp);
            return std::nullopt;
        }
        if (function == "strftime") {
            constexpr std::size_t maximum_chars = 1024 * 1024;
            const auto capacity = static_cast<std::size_t>(state.x[1]);
            if (capacity > maximum_chars) {
                throw std::runtime_error("guest time format buffer is too large");
            }
            const auto format = guest_ansi_string(memory, state.x[2]);
            const auto calendar = guest_structure<tm>(memory, state.x[3]);
            std::vector<char> output(capacity);
            const auto written = capacity == 0 ? 0 :
                ::strftime(output.data(), capacity, format.c_str(), &calendar);
            if (written != 0) {
                for (std::size_t index = 0; index <= written; ++index) {
                    memory.write8(state.x[0] + index,
                                  static_cast<std::uint8_t>(output[index]));
                }
            }
            state.x[0] = written;
            return std::nullopt;
        }
        if (function == "asctime" || function == "_ctime64" ||
            function == "_ctime64_s") {
            const bool secure = function == "_ctime64_s";
            char output[64]{};
            errno_t error = 0;
            if (function == "asctime") {
                const auto calendar = guest_structure<tm>(memory, state.x[0]);
                error = asctime_s(output, sizeof(output), &calendar);
            } else {
                const auto source = secure ? state.x[2] : state.x[0];
                if (source == 0) {
                    state.x[0] = secure ? EINVAL : 0;
                    return std::nullopt;
                }
                const auto timestamp = static_cast<__time64_t>(memory.read64(source));
                error = _ctime64_s(output, sizeof(output), &timestamp);
            }
            if (secure) {
                if (error == 0 && state.x[1] <= std::strlen(output)) {
                    state.x[0] = ERANGE;
                    return std::nullopt;
                }
                if (error == 0) {
                    for (std::size_t index = 0; index <= std::strlen(output); ++index) {
                        memory.write8(state.x[0] + index,
                                      static_cast<std::uint8_t>(output[index]));
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(error);
            } else if (error == 0) {
                ensure_time_storage();
                for (std::size_t index = 0; index <= std::strlen(output); ++index) {
                    memory.write8(crt_time_base + 256 + index,
                                  static_cast<std::uint8_t>(output[index]));
                }
                state.x[0] = crt_time_base + 256;
            } else {
                state.x[0] = 0;
            }
            return std::nullopt;
        }
        if (function == "_tzset" || function == "__timezone" ||
            function == "__daylight") {
            _tzset();
            if (function == "_tzset") return std::nullopt;
            ensure_time_storage();
            if (function == "__timezone") {
                long seconds = 0;
                static_cast<void>(_get_timezone(&seconds));
                memory.write32(crt_time_base + 512,
                               static_cast<std::uint32_t>(seconds));
                state.x[0] = crt_time_base + 512;
            } else {
                int daylight = 0;
                static_cast<void>(_get_daylight(&daylight));
                memory.write32(crt_time_base + 520,
                               static_cast<std::uint32_t>(daylight));
                state.x[0] = crt_time_base + 520;
            }
            return std::nullopt;
        }
    }
    if (dll == "api-ms-win-core-path-l1-1-0.dll" || dll == "pathcch.dll") {
        const auto read_path = [&](std::uint64_t address,
                                   std::size_t limit) -> std::optional<std::wstring> {
            if (address == 0) return std::nullopt;
            std::wstring path;
            for (std::size_t index = 0; index < limit; ++index) {
                if (index > (UINT64_MAX - address) / 2 ||
                    !guest_range(memory, address + index * 2, 2, false)) {
                    return std::nullopt;
                }
                const auto character = static_cast<wchar_t>(
                    memory.read8(address + index * 2) |
                    (memory.read8(address + index * 2 + 1) << 8));
                if (character == 0) return path;
                path.push_back(character);
            }
            return std::nullopt;
        };
        const auto write_path = [&](std::uint64_t address,
                                    const wchar_t* path) {
            for (std::size_t index = 0;; ++index) {
                const auto character = static_cast<std::uint16_t>(path[index]);
                memory.write8(address + index * 2,
                              static_cast<std::uint8_t>(character));
                memory.write8(address + index * 2 + 1,
                              static_cast<std::uint8_t>(character >> 8));
                if (character == 0) break;
            }
        };
        if (function == "PathCchSkipRoot" ||
            function == "PathCchFindExtension") {
            const auto path_address = state.x[0];
            const auto output_address = state.x[function == "PathCchSkipRoot" ? 1 : 2];
            const auto capacity = function == "PathCchSkipRoot"
                ? static_cast<std::uint64_t>(PATHCCH_MAX_CCH) : state.x[1];
            if (capacity == 0 || capacity > PATHCCH_MAX_CCH ||
                !guest_range(memory, output_address, 8, true)) {
                state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                return std::nullopt;
            }
            const auto path = read_path(path_address,
                                        static_cast<std::size_t>(capacity));
            if (!path) {
                state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                return std::nullopt;
            }
            std::vector<wchar_t> buffer(static_cast<std::size_t>(capacity));
            std::copy(path->begin(), path->end(), buffer.begin());
            const wchar_t* host_result = nullptr;
            const auto result = function == "PathCchSkipRoot"
                ? ::PathCchSkipRoot(buffer.data(), &host_result)
                : ::PathCchFindExtension(buffer.data(),
                      static_cast<std::size_t>(capacity), &host_result);
            if (SUCCEEDED(result) && host_result != nullptr) {
                memory.write64(output_address, path_address +
                    static_cast<std::uint64_t>(host_result - buffer.data()) * 2);
            }
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        if (function == "PathCchRemoveFileSpec" ||
            function == "PathCchCombine" || function == "PathCchCombineEx") {
            const auto output_address = state.x[0];
            const auto capacity = state.x[1];
            if (capacity == 0 || capacity > PATHCCH_MAX_CCH ||
                !guest_range(memory, output_address, capacity * 2, true)) {
                state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                return std::nullopt;
            }
            std::vector<wchar_t> buffer(static_cast<std::size_t>(capacity));
            HRESULT result = E_INVALIDARG;
            if (function == "PathCchRemoveFileSpec") {
                const auto path = read_path(output_address,
                                            static_cast<std::size_t>(capacity));
                if (!path) {
                    state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                    return std::nullopt;
                }
                std::copy(path->begin(), path->end(), buffer.begin());
                result = ::PathCchRemoveFileSpec(buffer.data(),
                                                  static_cast<std::size_t>(capacity));
            } else {
                const auto base = state.x[2] == 0
                    ? std::optional<std::wstring>(std::wstring{})
                    : read_path(state.x[2], PATHCCH_MAX_CCH);
                const auto more = state.x[3] == 0
                    ? std::optional<std::wstring>(std::wstring{})
                    : read_path(state.x[3], PATHCCH_MAX_CCH);
                if (!base || !more) {
                    state.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
                    return std::nullopt;
                }
                const auto base_input = state.x[2] == 0 ? nullptr : base->c_str();
                const auto more_input = state.x[3] == 0 ? nullptr : more->c_str();
                result = function == "PathCchCombine"
                    ? ::PathCchCombine(buffer.data(), static_cast<std::size_t>(capacity),
                                       base_input, more_input)
                    : ::PathCchCombineEx(buffer.data(),
                          static_cast<std::size_t>(capacity), base_input, more_input,
                          static_cast<ULONG>(state.x[4]));
            }
            if (SUCCEEDED(result)) write_path(output_address, buffer.data());
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
    }
    // pathcch.h marks the legacy Shlwapi path APIs deprecated, but these
    // imported entry points must retain their original guest-visible behavior.
#pragma warning(push)
#pragma warning(disable: 4995)
    if (dll == "shlwapi.dll") {
        const auto write_ansi = [&memory](std::uint64_t address,
                                           const char* value) {
            for (std::size_t index = 0;; ++index) {
                const auto character = static_cast<std::uint8_t>(value[index]);
                memory.write8(address + index, character);
                if (character == 0) break;
            }
        };
        const auto write_wide = [&memory](std::uint64_t address,
                                           const wchar_t* value) {
            for (std::size_t index = 0;; ++index) {
                const auto character = static_cast<std::uint16_t>(value[index]);
                memory.write8(address + index * 2,
                              static_cast<std::uint8_t>(character));
                memory.write8(address + index * 2 + 1,
                              static_cast<std::uint8_t>(character >> 8));
                if (character == 0) break;
            }
        };
        if (function == "PathIsRelativeW" ||
            function == "PathIsNetworkPathW" ||
            function == "PathIsDirectoryEmptyW") {
            const auto path = guest_string(memory, state.x[0], true);
            state.x[0] = function == "PathIsRelativeW"
                ? PathIsRelativeW(path.c_str())
                : function == "PathIsNetworkPathW"
                    ? PathIsNetworkPathW(path.c_str())
                    : PathIsDirectoryEmptyW(path.c_str());
            return std::nullopt;
        }
        if (function == "PathFileExistsA") {
            const auto path = guest_ansi_string(memory, state.x[0]);
            state.x[0] = PathFileExistsA(path.c_str());
            return std::nullopt;
        }
        if (function == "PathFindFileNameW") {
            const auto path = guest_string(memory, state.x[0], true);
            state.x[0] += static_cast<std::uint64_t>(
                PathFindFileNameW(path.c_str()) - path.c_str()) * 2;
            return std::nullopt;
        }
        if (function == "PathAppendW" || function == "PathRemoveFileSpecW" ||
            function == "PathCombineW") {
            const auto initial = function == "PathCombineW" ? std::wstring{} :
                guest_string(memory, state.x[0], true);
            if (initial.size() >= MAX_PATH) {
                throw std::runtime_error("guest path exceeds legacy path limit");
            }
            std::array<wchar_t, MAX_PATH> buffer{};
            std::copy(initial.begin(), initial.end(), buffer.begin());
            BOOL success = FALSE;
            if (function == "PathAppendW") {
                const auto addition = guest_string(memory, state.x[1], true);
                if (addition.size() >= MAX_PATH) {
                    throw std::runtime_error("guest path exceeds legacy path limit");
                }
                success = PathAppendW(buffer.data(), addition.c_str());
            } else if (function == "PathRemoveFileSpecW") {
                success = PathRemoveFileSpecW(buffer.data());
            } else {
                const auto directory = state.x[1] == 0 ? std::wstring{} :
                    guest_string(memory, state.x[1], true);
                const auto file = state.x[2] == 0 ? std::wstring{} :
                    guest_string(memory, state.x[2], true);
                if (directory.size() >= MAX_PATH || file.size() >= MAX_PATH) {
                    throw std::runtime_error("guest path exceeds legacy path limit");
                }
                success = PathCombineW(buffer.data(),
                    state.x[1] == 0 ? nullptr : directory.c_str(),
                    state.x[2] == 0 ? nullptr : file.c_str()) != nullptr;
            }
            if (success) write_wide(state.x[0], buffer.data());
            state.x[0] = function == "PathCombineW"
                ? (success ? state.x[0] : 0) : static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (function == "PathAppendA" || function == "PathRemoveFileSpecA" ||
            function == "PathStripPathA") {
            const auto initial = guest_ansi_string(memory, state.x[0]);
            if (initial.size() >= MAX_PATH) {
                throw std::runtime_error("guest path exceeds legacy path limit");
            }
            std::array<char, MAX_PATH> buffer{};
            std::copy(initial.begin(), initial.end(), buffer.begin());
            if (function == "PathAppendA") {
                const auto addition = guest_ansi_string(memory, state.x[1]);
                if (addition.size() >= MAX_PATH) {
                    throw std::runtime_error("guest path exceeds legacy path limit");
                }
                const auto success = PathAppendA(buffer.data(), addition.c_str());
                if (success) write_ansi(state.x[0], buffer.data());
                state.x[0] = static_cast<std::uint32_t>(success);
            } else if (function == "PathRemoveFileSpecA") {
                const auto success = PathRemoveFileSpecA(buffer.data());
                if (success) write_ansi(state.x[0], buffer.data());
                state.x[0] = static_cast<std::uint32_t>(success);
            } else {
                PathStripPathA(buffer.data());
                write_ansi(state.x[0], buffer.data());
            }
            return std::nullopt;
        }
    }
#pragma warning(pop)
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        dispatch_text_conversion(function, state, memory)) {
        return std::nullopt;
    }
    if (dll == "kernel32.dll" || dll == "kernelbase.dll") {
        if (function == "ExitProcess") {
            {
                std::lock_guard lock(state_->mutex);
                state_->process_exit_callbacks_suppressed = true;
            }
            return dispatch_win32_import(imported, state, memory);
        }
        if (function == "SetUnhandledExceptionFilter") {
            std::lock_guard lock(state_->mutex);
            const auto previous = state_->unhandled_exception_filter;
            state_->unhandled_exception_filter = state.x[0];
            state.x[0] = previous;
            return std::nullopt;
        }
        if (function == "UnhandledExceptionFilter") {
            std::uint64_t filter{};
            std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
            {
                std::lock_guard lock(state_->mutex);
                filter = state_->unhandled_exception_filter;
                invoke = state_->guest_callback;
            }
            if (filter != 0) {
                if (!invoke) {
                    throw std::runtime_error(
                        "guest unhandled-exception filter invoker is unavailable");
                }
                const auto result = static_cast<std::uint32_t>(
                    invoke(filter, state.x[0]));
                if (result != EXCEPTION_CONTINUE_SEARCH) {
                    state.x[0] = result;
                    return std::nullopt;
                }
            }
            // Without a guest debugger or a host-compatible exception record,
            // use the non-debugged Windows default without displaying a dialog.
            state.x[0] = EXCEPTION_EXECUTE_HANDLER;
            return std::nullopt;
        }
        const auto guest_handle = [this](HANDLE value, bool owned) {
            std::lock_guard lock(state_->mutex);
            const auto address = state_->next_host_handle;
            state_->next_host_handle += 8;
            state_->host_handles.emplace(address, State::HostHandle{value, owned});
            return address;
        };
        const auto host_handle = [this](std::uint64_t address) {
            if (address == UINT64_MAX) return GetCurrentProcess();
            if (address == UINT64_MAX - 1) return GetCurrentThread();
            std::lock_guard lock(state_->mutex);
            const auto found = state_->host_handles.find(address);
            if (found == state_->host_handles.end()) {
                SetLastError(ERROR_INVALID_HANDLE);
                return INVALID_HANDLE_VALUE;
            }
            return found->second.value;
        };
        const auto wait_handle = [this](std::uint64_t address)
            -> std::optional<HANDLE> {
            if (address == UINT64_MAX) return GetCurrentProcess();
            if (address == UINT64_MAX - 1) return GetCurrentThread();
            std::lock_guard lock(state_->mutex);
            const auto found = state_->host_handles.find(address);
            if (found == state_->host_handles.end()) {
                SetLastError(ERROR_INVALID_HANDLE);
                return std::nullopt;
            }
            return found->second.value;
        };
        if (function == "CreateThread") {
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = 0;
                return std::nullopt;
            };
            // Guest SECURITY_ATTRIBUTES can contain a guest security descriptor
            // and inheritance policy, neither of which is a host pointer.
            if (state.x[0] != 0) return fail(ERROR_NOT_SUPPORTED);
            const auto flags = static_cast<std::uint32_t>(state.x[4]);
            if (state.x[1] > 1024ULL * 1024U * 1024U || state.x[4] != flags ||
                (flags & ~(CREATE_SUSPENDED |
                           STACK_SIZE_PARAM_IS_A_RESERVATION)) != 0) {
                return fail(ERROR_INVALID_PARAMETER);
            }
            const auto entry = state.x[2];
            const auto code = entry == 0 || entry > UINT64_MAX - 4
                ? std::optional<GuestMemory::RegionInfo>{}
                : memory.query_region(entry);
            if (!code || !code->executable ||
                entry + 4 > code->base + code->size) {
                return fail(ERROR_INVALID_ADDRESS);
            }
            const auto id = state.x[5];
            const auto id_region = id == 0
                ? std::optional<GuestMemory::RegionInfo>{}
                : memory.query_region(id);
            if (id != 0 && (!id_region || !id_region->writable ||
                            id > UINT64_MAX - 4 ||
                            id + 4 > id_region->base + id_region->size)) {
                return fail(ERROR_NOACCESS);
            }
            auto request = state;
            SetLastError(ERROR_SUCCESS);
            static_cast<void>(dispatch(
                {"api-ms-win-crt-runtime-l1-1-0.dll", "_beginthreadex", 0},
                request, memory));
            if (request.x[0] == 0) {
                auto error = GetLastError();
                if (error == ERROR_SUCCESS) {
                    const auto crt_error = state_->thread_errno().value;
                    error = crt_error == EINVAL ? ERROR_INVALID_PARAMETER :
                            crt_error == EACCES ? ERROR_ACCESS_DENIED :
                            ERROR_NOT_ENOUGH_MEMORY;
                }
                return fail(error);
            }
            state.x[0] = request.x[0];
            return std::nullopt;
        }
        if (function == "CreateFileMappingA" ||
            function == "CreateFileMappingW") {
            if (state.x[1] != 0) {
                SetLastError(ERROR_NOT_SUPPORTED);
                state.x[0] = 0;
                return std::nullopt;
            }
            HANDLE file = INVALID_HANDLE_VALUE;
            if (state.x[0] != UINT64_MAX) {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->host_handles.find(state.x[0]);
                if (found == state_->host_handles.end()) {
                    SetLastError(ERROR_INVALID_HANDLE);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                file = found->second.value;
            }
            HANDLE mapping = nullptr;
            if (function.ends_with('W')) {
                const auto name = state.x[5] == 0 ? std::wstring{} :
                                  guest_string(memory, state.x[5], true);
                mapping = CreateFileMappingW(
                    file, nullptr, static_cast<DWORD>(state.x[2]),
                    static_cast<DWORD>(state.x[3]),
                    static_cast<DWORD>(state.x[4]),
                    state.x[5] == 0 ? nullptr : name.c_str());
            } else {
                const auto name = state.x[5] == 0 ? std::string{} :
                                  guest_ansi_string(memory, state.x[5]);
                mapping = CreateFileMappingA(
                    file, nullptr, static_cast<DWORD>(state.x[2]),
                    static_cast<DWORD>(state.x[3]),
                    static_cast<DWORD>(state.x[4]),
                    state.x[5] == 0 ? nullptr : name.c_str());
            }
            const auto error = GetLastError();
            state.x[0] = mapping == nullptr ? 0 : guest_handle(mapping, true);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "OpenFileMappingA" ||
            function == "OpenFileMappingW") {
            HANDLE mapping = nullptr;
            if (function.ends_with('W')) {
                const auto name = guest_string(memory, state.x[2], true);
                mapping = OpenFileMappingW(static_cast<DWORD>(state.x[0]),
                                           static_cast<BOOL>(state.x[1]),
                                           name.c_str());
            } else {
                const auto name = guest_ansi_string(memory, state.x[2]);
                mapping = OpenFileMappingA(static_cast<DWORD>(state.x[0]),
                                           static_cast<BOOL>(state.x[1]),
                                           name.c_str());
            }
            const auto error = GetLastError();
            state.x[0] = mapping == nullptr ? 0 : guest_handle(mapping, true);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "MapViewOfFile") {
            HANDLE mapping = nullptr;
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->host_handles.find(state.x[0]);
                if (found != state_->host_handles.end()) mapping = found->second.value;
            }
            if (mapping == nullptr) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto access = static_cast<DWORD>(state.x[1]);
            auto* view = static_cast<std::uint8_t*>(MapViewOfFile(
                mapping, access, static_cast<DWORD>(state.x[2]),
                static_cast<DWORD>(state.x[3]),
                static_cast<SIZE_T>(state.x[4])));
            if (view == nullptr) {
                state.x[0] = 0;
                return std::nullopt;
            }
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(view, &info, sizeof(info)) != sizeof(info) ||
                info.RegionSize < state.x[4]) {
                UnmapViewOfFile(view);
                SetLastError(ERROR_INVALID_ADDRESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto size = static_cast<std::size_t>(info.RegionSize);
            std::lock_guard lock(state_->mutex);
            auto guest = state_->next_file_view_address;
            const auto overlaps_reservation = [this, size](std::uint64_t base) {
                const auto next = state_->virtual_allocations.lower_bound(base);
                return (next != state_->virtual_allocations.end() &&
                        base + size > next->first) ||
                       (next != state_->virtual_allocations.begin() &&
                        std::prev(next)->first +
                            std::prev(next)->second.size > base);
            };
            while (guest <= UINT64_MAX - size &&
                   (!memory.is_range_available(guest, size) ||
                    overlaps_reservation(guest))) {
                guest += guest_allocation_granularity;
            }
            if (guest > UINT64_MAX - size) {
                UnmapViewOfFile(view);
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                state.x[0] = 0;
                return std::nullopt;
            }
            const bool writable = (access & (FILE_MAP_WRITE | FILE_MAP_COPY)) != 0;
            bool guest_mapped = false;
            try {
                memory.map_external_memory(guest, view, size, writable);
                if ((access & FILE_MAP_EXECUTE) != 0) {
                    memory.set_memory_access(guest, true, writable, true);
                }
                guest_mapped = true;
                state_->mapped_views.emplace(guest, State::MappedView{view, size});
            } catch (const std::exception&) {
                if (guest_mapped) memory.unmap_memory(guest);
                UnmapViewOfFile(view);
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                state.x[0] = 0;
                return std::nullopt;
            }
            state_->next_file_view_address =
                (guest + size + guest_allocation_granularity - 1) &
                ~(guest_allocation_granularity - 1);
            state.x[0] = guest;
            return std::nullopt;
        }
        if (function == "UnmapViewOfFile" || function == "FlushViewOfFile") {
            const auto address = state.x[0];
            std::lock_guard lock(state_->mutex);
            auto view = state_->mapped_views.upper_bound(address);
            if (view == state_->mapped_views.begin()) {
                SetLastError(ERROR_INVALID_ADDRESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            --view;
            if (address - view->first >= view->second.size ||
                (function == "UnmapViewOfFile" && address != view->first)) {
                SetLastError(ERROR_INVALID_ADDRESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (function == "FlushViewOfFile") {
                const auto remaining = view->second.size -
                    static_cast<std::size_t>(address - view->first);
                if (state.x[1] > remaining) {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                state.x[0] = static_cast<std::uint32_t>(FlushViewOfFile(
                    static_cast<std::uint8_t*>(view->second.host) +
                        (address - view->first),
                    static_cast<SIZE_T>(state.x[1])));
            } else {
                memory.unmap_memory(view->first);
                state.x[0] = static_cast<std::uint32_t>(
                    UnmapViewOfFile(view->second.host));
                state_->mapped_views.erase(view);
            }
            return std::nullopt;
        }
        if (function == "CreateEventA" || function == "CreateEventW" ||
            function == "CreateEventExA" || function == "CreateEventExW" ||
            function == "CreateMutexA" || function == "CreateMutexW" ||
            function == "CreateSemaphoreA" || function == "CreateSemaphoreW" ||
            function == "CreateSemaphoreExA" ||
            function == "CreateSemaphoreExW" ||
            function == "CreateWaitableTimerExW") {
            if (state.x[0] != 0) {
                SetLastError(ERROR_NOT_SUPPORTED);
                state.x[0] = 0;
                return std::nullopt;
            }
            const bool wide = function.ends_with('W');
            const auto name_address = function.starts_with("CreateEventEx") ||
                                      function == "CreateWaitableTimerExW"
                ? state.x[1]
                : function.starts_with("CreateMutex") ? state.x[2]
                : function.starts_with("CreateSemaphore") ? state.x[3]
                : state.x[3];
            const auto name_w = wide && name_address != 0
                ? guest_string(memory, name_address, true) : std::wstring{};
            const auto name_a = !wide && name_address != 0
                ? guest_ansi_string(memory, name_address) : std::string{};
            const auto* wide_name = name_address == 0 ? nullptr : name_w.c_str();
            const auto* ansi_name = name_address == 0 ? nullptr : name_a.c_str();
            HANDLE created = nullptr;
            if (function == "CreateEventA") {
                created = CreateEventA(nullptr, static_cast<BOOL>(state.x[1]),
                                       static_cast<BOOL>(state.x[2]), ansi_name);
            } else if (function == "CreateEventW") {
                created = CreateEventW(nullptr, static_cast<BOOL>(state.x[1]),
                                       static_cast<BOOL>(state.x[2]), wide_name);
            } else if (function == "CreateEventExA") {
                created = CreateEventExA(nullptr, ansi_name,
                    static_cast<DWORD>(state.x[2]), static_cast<DWORD>(state.x[3]));
            } else if (function == "CreateEventExW") {
                created = CreateEventExW(nullptr, wide_name,
                    static_cast<DWORD>(state.x[2]), static_cast<DWORD>(state.x[3]));
            } else if (function == "CreateMutexA") {
                created = CreateMutexA(nullptr, static_cast<BOOL>(state.x[1]),
                                       ansi_name);
            } else if (function == "CreateMutexW") {
                created = CreateMutexW(nullptr, static_cast<BOOL>(state.x[1]),
                                       wide_name);
            } else if (function == "CreateSemaphoreA") {
                created = CreateSemaphoreA(nullptr, static_cast<LONG>(state.x[1]),
                    static_cast<LONG>(state.x[2]), ansi_name);
            } else if (function == "CreateSemaphoreW") {
                created = CreateSemaphoreW(nullptr, static_cast<LONG>(state.x[1]),
                    static_cast<LONG>(state.x[2]), wide_name);
            } else if (function == "CreateSemaphoreExA") {
                created = CreateSemaphoreExA(nullptr,
                    static_cast<LONG>(state.x[1]), static_cast<LONG>(state.x[2]),
                    ansi_name, static_cast<DWORD>(state.x[4]),
                    static_cast<DWORD>(state.x[5]));
            } else if (function == "CreateSemaphoreExW") {
                created = CreateSemaphoreExW(nullptr,
                    static_cast<LONG>(state.x[1]), static_cast<LONG>(state.x[2]),
                    wide_name, static_cast<DWORD>(state.x[4]),
                    static_cast<DWORD>(state.x[5]));
            } else {
                created = CreateWaitableTimerExW(nullptr, wide_name,
                    static_cast<DWORD>(state.x[2]), static_cast<DWORD>(state.x[3]));
            }
            const auto error = GetLastError();
            state.x[0] = created == nullptr ? 0 : guest_handle(created, true);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "OpenEventA" || function == "OpenEventW" ||
            function == "OpenMutexA" || function == "OpenMutexW" ||
            function == "OpenSemaphoreA" || function == "OpenSemaphoreW") {
            const bool wide = function.ends_with('W');
            HANDLE opened = nullptr;
            if (wide) {
                const auto name = guest_string(memory, state.x[2], true);
                if (function.starts_with("OpenEvent")) {
                    opened = OpenEventW(static_cast<DWORD>(state.x[0]),
                                        static_cast<BOOL>(state.x[1]), name.c_str());
                } else if (function.starts_with("OpenMutex")) {
                    opened = OpenMutexW(static_cast<DWORD>(state.x[0]),
                                        static_cast<BOOL>(state.x[1]), name.c_str());
                } else {
                    opened = OpenSemaphoreW(static_cast<DWORD>(state.x[0]),
                                            static_cast<BOOL>(state.x[1]), name.c_str());
                }
            } else {
                const auto name = guest_ansi_string(memory, state.x[2]);
                if (function.starts_with("OpenEvent")) {
                    opened = OpenEventA(static_cast<DWORD>(state.x[0]),
                                        static_cast<BOOL>(state.x[1]), name.c_str());
                } else if (function.starts_with("OpenMutex")) {
                    opened = OpenMutexA(static_cast<DWORD>(state.x[0]),
                                        static_cast<BOOL>(state.x[1]), name.c_str());
                } else {
                    opened = OpenSemaphoreA(static_cast<DWORD>(state.x[0]),
                                            static_cast<BOOL>(state.x[1]), name.c_str());
                }
            }
            state.x[0] = opened == nullptr ? 0 : guest_handle(opened, true);
            return std::nullopt;
        }
        if (function == "SetEvent" || function == "ResetEvent" ||
            function == "PulseEvent" || function == "ReleaseMutex") {
            const auto handle = host_handle(state.x[0]);
            state.x[0] = static_cast<std::uint32_t>(
                function == "SetEvent" ? SetEvent(handle)
                : function == "ResetEvent" ? ResetEvent(handle)
                : function == "PulseEvent" ? PulseEvent(handle)
                : ReleaseMutex(handle));
            return std::nullopt;
        }
        if (function == "ReleaseSemaphore") {
            LONG previous = 0;
            const auto success = ReleaseSemaphore(host_handle(state.x[0]),
                static_cast<LONG>(state.x[1]), &previous);
            if (success && state.x[2] != 0) {
                memory.write32(state.x[2], static_cast<DWORD>(previous));
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (function == "WaitForSingleObject" ||
            function == "WaitForSingleObjectEx") {
            const auto guest = state.x[0];
            const auto handle = wait_handle(guest);
            if (!handle) {
                state.x[0] = WAIT_FAILED;
                return std::nullopt;
            }
            state.x[0] = function == "WaitForSingleObject"
                ? WaitForSingleObject(*handle, static_cast<DWORD>(state.x[1]))
                : WaitForSingleObjectEx(*handle, static_cast<DWORD>(state.x[1]),
                                        static_cast<BOOL>(state.x[2]));
            if (state.x[0] == WAIT_OBJECT_0) {
                std::shared_ptr<GuestThreadRecord> worker;
                {
                    std::lock_guard lock(state_->mutex);
                    for (const auto& thread : state_->guest_threads) {
                        if (thread->guest_handle == guest) {
                            worker = thread;
                            break;
                        }
                    }
                }
                if (worker) {
                    std::lock_guard lock(worker->mutex);
                    if (!worker->failure.empty()) {
                        throw std::runtime_error("guest worker thread failed: " +
                                                 worker->failure);
                    }
                }
            }
            return std::nullopt;
        }
        if (function == "WaitForMultipleObjects" ||
            function == "WaitForMultipleObjectsEx") {
            const auto count = static_cast<DWORD>(state.x[0]);
            if (count == 0 || count > MAXIMUM_WAIT_OBJECTS ||
                state.x[0] != count) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = WAIT_FAILED;
                return std::nullopt;
            }
            std::vector<HANDLE> handles;
            handles.reserve(count);
            for (DWORD index = 0; index < count; ++index) {
                const auto handle = wait_handle(memory.read64(
                    state.x[1] + static_cast<std::uint64_t>(index) * 8));
                if (!handle) {
                    state.x[0] = WAIT_FAILED;
                    return std::nullopt;
                }
                handles.push_back(*handle);
            }
            state.x[0] = function == "WaitForMultipleObjects"
                ? WaitForMultipleObjects(count, handles.data(),
                    static_cast<BOOL>(state.x[2]), static_cast<DWORD>(state.x[3]))
                : WaitForMultipleObjectsEx(count, handles.data(),
                    static_cast<BOOL>(state.x[2]), static_cast<DWORD>(state.x[3]),
                    static_cast<BOOL>(state.x[4]));
            return std::nullopt;
        }
        if (function == "SignalObjectAndWait") {
            const auto signal = wait_handle(state.x[0]);
            const auto waiting = wait_handle(state.x[1]);
            if (!signal || !waiting) {
                state.x[0] = WAIT_FAILED;
                return std::nullopt;
            }
            state.x[0] = SignalObjectAndWait(*signal,
                *waiting, static_cast<DWORD>(state.x[2]),
                static_cast<BOOL>(state.x[3]));
            return std::nullopt;
        }
        if (function == "Sleep" || function == "SleepEx") {
            if (function == "Sleep") {
                Sleep(static_cast<DWORD>(state.x[0]));
            } else {
                state.x[0] = SleepEx(static_cast<DWORD>(state.x[0]),
                                     static_cast<BOOL>(state.x[1]));
            }
            return std::nullopt;
        }
        if (function == "SetWaitableTimerEx" ||
            function == "CancelWaitableTimer") {
            const auto timer = host_handle(state.x[0]);
            if (function == "CancelWaitableTimer") {
                state.x[0] = static_cast<std::uint32_t>(CancelWaitableTimer(timer));
            } else if (state.x[3] != 0 || state.x[5] != 0) {
                SetLastError(ERROR_NOT_SUPPORTED);
                state.x[0] = 0;
            } else {
                LARGE_INTEGER due{};
                due.QuadPart = static_cast<LONGLONG>(memory.read64(state.x[1]));
                state.x[0] = static_cast<std::uint32_t>(SetWaitableTimerEx(
                    timer, &due, static_cast<LONG>(state.x[2]), nullptr,
                    nullptr, nullptr, static_cast<ULONG>(state.x[6])));
            }
            return std::nullopt;
        }
        if (function == "GetCurrentProcess" || function == "GetCurrentThread") {
            state.x[0] = function == "GetCurrentProcess" ? UINT64_MAX
                                                          : UINT64_MAX - 1;
            return std::nullopt;
        }
        if (function == "GetCurrentThreadStackLimits") {
            if (state.x[18] == 0) {
                throw std::runtime_error("guest thread environment is not initialized");
            }
            const auto low = memory.read64(state.x[18] + 0x10);
            const auto high = memory.read64(state.x[18] + 0x08);
            memory.write64(state.x[0], low);
            memory.write64(state.x[1], high);
            return std::nullopt;
        }
        if (function == "GetProcessAffinityMask") {
            DWORD_PTR process_mask = 0;
            DWORD_PTR system_mask = 0;
            const auto success = GetProcessAffinityMask(
                host_handle(state.x[0]), &process_mask, &system_mask);
            if (success) {
                memory.write64(state.x[1], process_mask);
                memory.write64(state.x[2], system_mask);
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (function == "SetProcessAffinityMask") {
            state.x[0] = static_cast<std::uint32_t>(SetProcessAffinityMask(
                host_handle(state.x[0]), static_cast<DWORD_PTR>(state.x[1])));
            return std::nullopt;
        }
        if (function == "SetThreadAffinityMask") {
            state.x[0] = static_cast<std::uint64_t>(SetThreadAffinityMask(
                host_handle(state.x[0]), static_cast<DWORD_PTR>(state.x[1])));
            return std::nullopt;
        }
        if (function == "ResumeThread" || function == "SuspendThread") {
            state.x[0] = function == "ResumeThread"
                ? ResumeThread(host_handle(state.x[0]))
                : SuspendThread(host_handle(state.x[0]));
            return std::nullopt;
        }
        if (function == "ExitThread") {
            return static_cast<std::uint32_t>(state.x[0]);
        }
        if (function == "SetThreadIdealProcessor") {
            state.x[0] = SetThreadIdealProcessor(
                host_handle(state.x[0]), static_cast<DWORD>(state.x[1]));
            return std::nullopt;
        }
        if (function == "SwitchToThread") {
            state.x[0] = static_cast<std::uint32_t>(SwitchToThread());
            return std::nullopt;
        }
        if (function == "GetThreadPriority" ||
            function == "SetThreadPriority") {
            const auto handle = host_handle(state.x[0]);
            state.x[0] = function == "GetThreadPriority"
                ? static_cast<std::uint32_t>(GetThreadPriority(handle))
                : static_cast<std::uint32_t>(SetThreadPriority(
                      handle, static_cast<int>(state.x[1])));
            return std::nullopt;
        }
        if (function == "GetExitCodeProcess" ||
            function == "GetExitCodeThread") {
            DWORD code = 0;
            const auto success = function == "GetExitCodeProcess"
                ? GetExitCodeProcess(host_handle(state.x[0]), &code)
                : GetExitCodeThread(host_handle(state.x[0]), &code);
            if (success) memory.write32(state.x[1], code);
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (function == "GetProcessTimes" || function == "GetThreadTimes") {
            FILETIME created{}, exited{}, kernel{}, user{};
            const auto success = function == "GetProcessTimes"
                ? GetProcessTimes(host_handle(state.x[0]), &created, &exited,
                                  &kernel, &user)
                : GetThreadTimes(host_handle(state.x[0]), &created, &exited,
                                 &kernel, &user);
            if (success) {
                const std::array values{created, exited, kernel, user};
                for (std::size_t index = 0; index < values.size(); ++index) {
                    if (state.x[index + 1] != 0) {
                        write_guest_structure(memory, state.x[index + 1],
                                              values[index]);
                    }
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (function == "GetProcessId" || function == "GetThreadId") {
            state.x[0] = function == "GetProcessId"
                ? GetProcessId(host_handle(state.x[0]))
                : GetThreadId(host_handle(state.x[0]));
            return std::nullopt;
        }
        if (function == "OpenProcess") {
            const auto handle = OpenProcess(static_cast<DWORD>(state.x[0]),
                                            static_cast<BOOL>(state.x[1]),
                                            static_cast<DWORD>(state.x[2]));
            state.x[0] = handle == nullptr ? 0 : guest_handle(handle, true);
            return std::nullopt;
        }
        if (function == "InitializeProcThreadAttributeList") {
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = 0;
                return std::nullopt;
            };
            const auto address = state.x[0];
            const auto size_address = state.x[3];
            if (!guest_range(memory, size_address, sizeof(std::uint64_t), true) ||
                state.x[1] == 0 || state.x[1] > 64 || state.x[2] != 0) {
                return fail(size_address == 0 ? ERROR_NOACCESS
                                              : ERROR_INVALID_PARAMETER);
            }
            SIZE_T required = 0;
            static_cast<void>(::InitializeProcThreadAttributeList(
                nullptr, static_cast<DWORD>(state.x[1]), 0, &required));
            const auto query_error = GetLastError();
            if (query_error != ERROR_INSUFFICIENT_BUFFER || required == 0 ||
                required > 1024U * 1024U) {
                return fail(query_error == ERROR_INSUFFICIENT_BUFFER
                    ? ERROR_NOT_ENOUGH_MEMORY : query_error);
            }
            const auto capacity = memory.read64(size_address);
            memory.write64(size_address, required);
            if (address == 0) return fail(ERROR_INSUFFICIENT_BUFFER);
            if (capacity < required) return fail(ERROR_INSUFFICIENT_BUFFER);
            if (!guest_range(memory, address, required, true)) {
                return fail(ERROR_NOACCESS);
            }
            {
                std::lock_guard lock(state_->mutex);
                if (state_->process_attributes.contains(address)) {
                    return fail(ERROR_INVALID_PARAMETER);
                }
            }
            State::ProcessAttributeList attributes;
            attributes.native.resize(required);
            attributes.guest_size = required;
            SIZE_T initialized_size = required;
            const auto success = ::InitializeProcThreadAttributeList(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                    attributes.native.data()),
                static_cast<DWORD>(state.x[1]), 0, &initialized_size);
            const auto error = GetLastError();
            if (success) {
                for (SIZE_T index = 0; index < required; ++index) {
                    memory.write8(address + index, 0);
                }
                std::lock_guard lock(state_->mutex);
                state_->process_attributes.emplace(address,
                                                   std::move(attributes));
            }
            memory.write64(size_address, initialized_size);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "UpdateProcThreadAttribute") {
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = 0;
                return std::nullopt;
            };
            const auto address = state.x[0];
            const auto attribute = static_cast<DWORD_PTR>(state.x[2]);
            const auto bytes = state.x[4];
            if (state.x[1] != 0 || state.x[5] != 0 || state.x[6] != 0 ||
                attribute != PROC_THREAD_ATTRIBUTE_HANDLE_LIST ||
                bytes == 0 || bytes > 64U * sizeof(HANDLE) ||
                bytes % sizeof(HANDLE) != 0) {
                return fail(ERROR_INVALID_PARAMETER);
            }
            if (!guest_range(memory, state.x[3], bytes, false)) {
                return fail(ERROR_NOACCESS);
            }
            std::lock_guard lock(state_->mutex);
            const auto found = state_->process_attributes.find(address);
            if (found == state_->process_attributes.end()) {
                return fail(ERROR_INVALID_PARAMETER);
            }
            std::vector<HANDLE> handles;
            std::vector<std::uint64_t> guest_tokens;
            handles.reserve(bytes / sizeof(HANDLE));
            guest_tokens.reserve(bytes / sizeof(HANDLE));
            for (std::uint64_t index = 0; index < bytes; index += sizeof(HANDLE)) {
                const auto guest = memory.read64(state.x[3] + index);
                if (guest == UINT64_MAX || guest == UINT64_MAX - 1) {
                    return fail(ERROR_INVALID_HANDLE);
                }
                const auto mapped = state_->host_handles.find(guest);
                if (mapped == state_->host_handles.end()) {
                    return fail(ERROR_INVALID_HANDLE);
                }
                DWORD flags = 0;
                if (!GetHandleInformation(mapped->second.value, &flags) ||
                    (flags & HANDLE_FLAG_INHERIT) == 0) {
                    return fail(ERROR_INVALID_PARAMETER);
                }
                handles.push_back(mapped->second.value);
                guest_tokens.push_back(guest);
            }
            const auto success = ::UpdateProcThreadAttribute(
                reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                    found->second.native.data()), 0, attribute,
                handles.data(), bytes, nullptr, nullptr);
            const auto error = GetLastError();
            if (success) {
                found->second.inherited_handles = std::move(handles);
                found->second.guest_handle_tokens = std::move(guest_tokens);
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "DeleteProcThreadAttributeList") {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->process_attributes.find(state.x[0]);
            if (found != state_->process_attributes.end()) {
                ::DeleteProcThreadAttributeList(
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                        found->second.native.data()));
                state_->process_attributes.erase(found);
            }
            return std::nullopt;
        }
        if (function == "CreateProcessW") {
            static_assert(sizeof(STARTUPINFOW) == 104);
            static_assert(sizeof(STARTUPINFOEXW) == 112);
            static_assert(sizeof(PROCESS_INFORMATION) == 24);
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = 0;
                return std::nullopt;
            };
            const auto startup_address = memory.read64(state.sp);
            const auto output_address = memory.read64(state.sp + 8);
            const auto flags = static_cast<DWORD>(state.x[5]);
            const bool extended = (flags & EXTENDED_STARTUPINFO_PRESENT) != 0;
            if (state.x[5] != flags ||
                !guest_range(memory, startup_address,
                             extended ? sizeof(STARTUPINFOEXW)
                                      : sizeof(STARTUPINFOW), false) ||
                !guest_range(memory, output_address,
                             sizeof(PROCESS_INFORMATION), true)) {
                return fail(ERROR_NOACCESS);
            }
            auto startup = guest_structure<STARTUPINFOW>(memory, startup_address);
            if (startup.cb != (extended ? sizeof(STARTUPINFOEXW)
                                        : sizeof(STARTUPINFOW)) ||
                startup.cbReserved2 != 0 || startup.lpReserved2 != nullptr) {
                return fail(ERROR_INVALID_PARAMETER);
            }
            std::optional<std::wstring> application;
            std::optional<std::wstring> command_line;
            std::optional<std::wstring> current_directory;
            std::optional<std::wstring> desktop;
            std::optional<std::wstring> title;
            try {
                if (state.x[0] != 0)
                    application = guest_string(memory, state.x[0], true);
                if (state.x[1] != 0)
                    command_line = guest_string(memory, state.x[1], true);
                if (state.x[7] != 0)
                    current_directory = guest_string(memory, state.x[7], true);
                if (startup.lpDesktop != nullptr)
                    desktop = guest_string(memory,
                        reinterpret_cast<std::uint64_t>(startup.lpDesktop), true);
                if (startup.lpTitle != nullptr)
                    title = guest_string(memory,
                        reinterpret_cast<std::uint64_t>(startup.lpTitle), true);
            } catch (const std::runtime_error&) {
                return fail(ERROR_NOACCESS);
            }
            if (startup.lpReserved != nullptr) return fail(ERROR_NOT_SUPPORTED);
            startup.lpDesktop = desktop ? desktop->data() : nullptr;
            startup.lpTitle = title ? title->data() : nullptr;
            std::wstring candidate;
            if (application && !application->empty()) {
                candidate = *application;
            } else if (command_line && !command_line->empty()) {
                const auto first = command_line->find_first_not_of(L" \t");
                if (first == std::wstring::npos) return fail(ERROR_INVALID_PARAMETER);
                if ((*command_line)[first] == L'"') {
                    const auto closing = command_line->find(L'"', first + 1);
                    if (closing == std::wstring::npos)
                        return fail(ERROR_INVALID_PARAMETER);
                    candidate = command_line->substr(first + 1,
                                                     closing - first - 1);
                } else {
                    const auto end = command_line->find_first_of(L" \t", first);
                    candidate = command_line->substr(first, end - first);
                }
            } else {
                return fail(ERROR_INVALID_PARAMETER);
            }
            std::array<wchar_t, 32768> resolved{};
            const auto path_length = ::SearchPathW(
                nullptr, candidate.c_str(), L".exe",
                static_cast<DWORD>(resolved.size()), resolved.data(), nullptr);
            if (path_length == 0 || path_length >= resolved.size()) {
                return fail(path_length == 0 ? GetLastError() : ERROR_FILENAME_EXCED_RANGE);
            }
            const auto machine = pe_machine(resolved.data());
            if (!machine) return fail(ERROR_BAD_EXE_FORMAT);
            const bool arm_guest = *machine == IMAGE_FILE_MACHINE_ARM64;
            if (*machine == 0xa641U || *machine == 0xa64eU) {
                return fail(ERROR_NOT_SUPPORTED); // ARM64EC / ARM64X hybrid PE.
            }
            if (!arm_guest && *machine != IMAGE_FILE_MACHINE_AMD64 &&
                *machine != IMAGE_FILE_MACHINE_I386) {
                return fail(ERROR_BAD_EXE_FORMAT);
            }
            SECURITY_ATTRIBUTES process_security{};
            SECURITY_ATTRIBUTES thread_security{};
            const auto security = [&](std::uint64_t guest,
                                      SECURITY_ATTRIBUTES& host) -> DWORD {
                if (guest == 0) return ERROR_SUCCESS;
                if (!guest_range(memory, guest, sizeof(host), false))
                    return ERROR_NOACCESS;
                if (memory.read32(guest) != sizeof(host))
                    return ERROR_INVALID_PARAMETER;
                if (memory.read64(guest + 8) != 0)
                    return ERROR_NOT_SUPPORTED;
                host.nLength = sizeof(host);
                host.bInheritHandle = memory.read32(guest + 16) != 0;
                return ERROR_SUCCESS;
            };
            if (const auto error = security(state.x[2], process_security);
                error != ERROR_SUCCESS) return fail(error);
            if (const auto error = security(state.x[3], thread_security);
                error != ERROR_SUCCESS) return fail(error);
            const auto process_attributes = state.x[2] == 0
                ? nullptr : &process_security;
            const auto thread_attributes = state.x[3] == 0
                ? nullptr : &thread_security;
            std::vector<wchar_t> wide_environment;
            std::vector<char> narrow_environment;
            DWORD host_flags = flags;
            try {
                if (state.x[6] != 0) {
                    const bool wide = (flags & CREATE_UNICODE_ENVIRONMENT) != 0;
                    bool preceding_zero = false;
                    for (std::uint64_t index = 0; index < 524288; ++index) {
                        const auto character = wide
                            ? static_cast<std::uint16_t>(
                                  memory.read8(state.x[6] + 2 * index) |
                                  (memory.read8(state.x[6] + 2 * index + 1) << 8))
                            : memory.read8(state.x[6] + index);
                        if (wide) wide_environment.push_back(
                            static_cast<wchar_t>(character));
                        else narrow_environment.push_back(
                            static_cast<char>(character));
                        if (character == 0 && preceding_zero) break;
                        preceding_zero = character == 0;
                    }
                    const bool terminated = wide
                        ? wide_environment.size() >= 2 &&
                          wide_environment.back() == 0 &&
                          wide_environment[wide_environment.size() - 2] == 0
                        : narrow_environment.size() >= 2 &&
                          narrow_environment.back() == 0 &&
                          narrow_environment[narrow_environment.size() - 2] == 0;
                    if (!terminated) return fail(ERROR_INVALID_PARAMETER);
                } else {
                    ensure_guest_environment();
                    std::lock_guard lock(state_->mutex);
                    for (const auto& [name, value] : state_->guest_environment) {
                        const auto entry = name + L"=" + value;
                        wide_environment.insert(wide_environment.end(),
                                                entry.begin(), entry.end());
                        wide_environment.push_back(0);
                    }
                    if (wide_environment.empty()) wide_environment.push_back(0);
                    wide_environment.push_back(0);
                    host_flags |= CREATE_UNICODE_ENVIRONMENT;
                }
            } catch (const std::runtime_error&) {
                return fail(ERROR_NOACCESS);
            }
            std::array<std::uint64_t, 3> standard_tokens{};
            if ((startup.dwFlags & STARTF_USESTDHANDLES) != 0) {
                const std::array original{
                    reinterpret_cast<std::uint64_t>(startup.hStdInput),
                    reinterpret_cast<std::uint64_t>(startup.hStdOutput),
                    reinterpret_cast<std::uint64_t>(startup.hStdError)};
                standard_tokens = original;
                for (auto* slot : {&startup.hStdInput, &startup.hStdOutput,
                                   &startup.hStdError}) {
                    const auto guest = reinterpret_cast<std::uint64_t>(*slot);
                    if (guest == 0) continue;
                    *slot = host_handle(guest);
                    if (*slot == INVALID_HANDLE_VALUE)
                        return fail(ERROR_INVALID_HANDLE);
                }
            }
            std::unique_lock lock(state_->mutex);
            STARTUPINFOEXW startup_ex{};
            LPSTARTUPINFOW host_startup = &startup;
            ChildBootstrap bootstrap;
            bootstrap.standard_handle_tokens = standard_tokens;
            if (extended) {
                const auto guest_list = memory.read64(
                    startup_address + offsetof(STARTUPINFOEXW, lpAttributeList));
                const auto found = state_->process_attributes.find(guest_list);
                if (found == state_->process_attributes.end())
                    return fail(ERROR_INVALID_PARAMETER);
                if (!found->second.inherited_handles.empty() &&
                    state.x[4] == 0) return fail(ERROR_INVALID_PARAMETER);
                if (state.x[4] != 0 &&
                    found->second.inherited_handles.empty()) {
                    return fail(ERROR_NOT_SUPPORTED);
                }
                if (arm_guest) {
                    for (std::size_t index = 0;
                         index < found->second.inherited_handles.size(); ++index) {
                        bootstrap.inherited_handles.emplace_back(
                            found->second.guest_handle_tokens[index],
                            reinterpret_cast<std::uint64_t>(
                                found->second.inherited_handles[index]));
                    }
                }
                startup_ex.StartupInfo = startup;
                startup_ex.lpAttributeList =
                    reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                        found->second.native.data());
                host_startup = &startup_ex.StartupInfo;
            } else if (state.x[4] != 0 && !arm_guest) {
                // Native TRUE would also leak inheritable translator handles.
                return fail(ERROR_NOT_SUPPORTED);
            }
            if (arm_guest && !extended && state.x[4] != 0) {
                for (const auto& [guest, handle] : state_->host_handles) {
                    DWORD handle_flags = 0;
                    if (GetHandleInformation(handle.value, &handle_flags) &&
                        (handle_flags & HANDLE_FLAG_INHERIT) != 0) {
                        bootstrap.inherited_handles.emplace_back(
                            guest, reinterpret_cast<std::uint64_t>(
                                handle.value));
                    }
                }
                if (bootstrap.inherited_handles.size() > 64)
                    return fail(ERROR_NOT_SUPPORTED);
            }
            std::wstring mutable_command = command_line.value_or(L"");
            PROCESS_INFORMATION child{};
            BOOL success = FALSE;
            DWORD error = ERROR_SUCCESS;
            if (arm_guest) {
                constexpr DWORD permitted_flags =
                    CREATE_NO_WINDOW | CREATE_NEW_CONSOLE | DETACHED_PROCESS |
                    CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED |
                    CREATE_DEFAULT_ERROR_MODE | CREATE_UNICODE_ENVIRONMENT |
                    EXTENDED_STARTUPINFO_PRESENT | CREATE_BREAKAWAY_FROM_JOB |
                    IDLE_PRIORITY_CLASS | BELOW_NORMAL_PRIORITY_CLASS |
                    NORMAL_PRIORITY_CLASS | ABOVE_NORMAL_PRIORITY_CLASS |
                    HIGH_PRIORITY_CLASS;
                if ((flags & ~permitted_flags) != 0 ||
                    state_->child_host_executable.empty() ||
                    ((startup.dwFlags & STARTF_USESTDHANDLES) != 0 &&
                     state.x[4] == 0)) {
                    return fail(ERROR_NOT_SUPPORTED);
                }
                for (const auto token : standard_tokens) {
                    if (token == 0) continue;
                    const auto found = std::find_if(
                        bootstrap.inherited_handles.begin(),
                        bootstrap.inherited_handles.end(),
                        [token](const auto& item) {
                            return item.first == token;
                        });
                    if (found == bootstrap.inherited_handles.end())
                        return fail(ERROR_INVALID_PARAMETER);
                }
                bootstrap.guest_command_line = command_line
                    ? *command_line : application.value_or(L"");
                if (!wide_environment.empty()) {
                    bootstrap.guest_environment = wide_environment;
                } else {
                    for (std::size_t index = 0;
                         index + 1 < narrow_environment.size();) {
                        const auto end = std::find(narrow_environment.begin() + index,
                                                   narrow_environment.end(), '\0');
                        if (end == narrow_environment.end())
                            return fail(ERROR_INVALID_PARAMETER);
                        if (end == narrow_environment.begin() + index) break;
                        const std::string_view entry(
                            narrow_environment.data() + index,
                            static_cast<std::size_t>(end -
                                (narrow_environment.begin() + index)));
                        const auto converted = environment_wide(entry);
                        bootstrap.guest_environment.insert(
                            bootstrap.guest_environment.end(),
                            converted.begin(), converted.end());
                        bootstrap.guest_environment.push_back(0);
                        index += entry.size() + 1;
                    }
                    if (bootstrap.guest_environment.empty())
                        bootstrap.guest_environment.push_back(0);
                    bootstrap.guest_environment.push_back(0);
                }
                const auto mapping = make_child_bootstrap_mapping(bootstrap);
                if (!mapping) return fail(GetLastError());
                const auto mapping_handle = mapping->first;
                SIZE_T list_bytes = 0;
                static_cast<void>(::InitializeProcThreadAttributeList(
                    nullptr, 1, 0, &list_bytes));
                std::vector<std::byte> list_storage(list_bytes);
                auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                    list_storage.data());
                if (!::InitializeProcThreadAttributeList(list, 1, 0,
                                                         &list_bytes)) {
                    error = GetLastError();
                    CloseHandle(mapping_handle);
                    return fail(error);
                }
                std::vector<HANDLE> inherited;
                inherited.reserve(bootstrap.inherited_handles.size() + 1);
                for (const auto& [guest, native] : bootstrap.inherited_handles) {
                    static_cast<void>(guest);
                    const auto handle = reinterpret_cast<HANDLE>(native);
                    DWORD handle_flags = 0;
                    if (!GetHandleInformation(handle, &handle_flags) ||
                        (handle_flags & HANDLE_FLAG_INHERIT) == 0) {
                        ::DeleteProcThreadAttributeList(list);
                        CloseHandle(mapping_handle);
                        return fail(ERROR_INVALID_HANDLE);
                    }
                    inherited.push_back(handle);
                }
                inherited.push_back(mapping_handle);
                if (!::UpdateProcThreadAttribute(
                        list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                        inherited.data(), inherited.size() * sizeof(HANDLE),
                        nullptr, nullptr)) {
                    error = GetLastError();
                    ::DeleteProcThreadAttributeList(list);
                    CloseHandle(mapping_handle);
                    return fail(error);
                }
                startup_ex.StartupInfo = startup;
                startup_ex.StartupInfo.cb = sizeof(STARTUPINFOEXW);
                startup_ex.lpAttributeList = list;
                std::wstring launcher = quote_child_argument(
                    state_->child_host_executable.wstring());
                if (state_->child_jit) launcher += L" --jit";
                if (state_->child_instruction_limit) {
                    launcher += L" --instruction-limit " +
                        std::to_wstring(*state_->child_instruction_limit);
                }
                for (const auto& directory : state_->child_dll_directories) {
                    launcher += L" --dll-dir " +
                        quote_child_argument(directory.wstring());
                }
                launcher += L" --child-bootstrap " +
                    std::to_wstring(reinterpret_cast<std::uint64_t>(
                        mapping_handle)) + L" " +
                    std::to_wstring(mapping->second) + L" " +
                    quote_child_argument(resolved.data());
                if (launcher.size() >= 32767) {
                    ::DeleteProcThreadAttributeList(list);
                    CloseHandle(mapping_handle);
                    return fail(ERROR_FILENAME_EXCED_RANGE);
                }
                success = ::CreateProcessW(
                    state_->child_host_executable.c_str(), launcher.data(),
                    process_attributes, thread_attributes, TRUE,
                    host_flags | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                    current_directory ? current_directory->c_str() : nullptr,
                    &startup_ex.StartupInfo, &child);
                error = GetLastError();
                ::DeleteProcThreadAttributeList(list);
                CloseHandle(mapping_handle);
            } else {
                success = ::CreateProcessW(
                    application ? application->c_str() : nullptr,
                    command_line ? mutable_command.data() : nullptr,
                    process_attributes, thread_attributes,
                    state.x[4] != 0, host_flags,
                    wide_environment.empty()
                        ? static_cast<void*>(narrow_environment.data())
                        : static_cast<void*>(wide_environment.data()),
                    current_directory ? current_directory->c_str() : nullptr,
                    host_startup, &child);
                error = GetLastError();
            }
            lock.unlock();
            if (success) {
                memory.write64(output_address,
                               guest_handle(child.hProcess, true));
                memory.write64(output_address + 8,
                               guest_handle(child.hThread, true));
                memory.write32(output_address + 16, child.dwProcessId);
                memory.write32(output_address + 20, child.dwThreadId);
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "DuplicateHandle") {
            if (!guest_range(memory, state.x[3], sizeof(std::uint64_t), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto source_process = host_handle(state.x[0]);
            const auto source = host_handle(state.x[1]);
            const auto target_process = host_handle(state.x[2]);
            if (GetProcessId(source_process) != GetCurrentProcessId() ||
                GetProcessId(target_process) != GetCurrentProcessId()) {
                SetLastError(ERROR_NOT_SUPPORTED);
                state.x[0] = 0;
                return std::nullopt;
            }
            HANDLE duplicate = nullptr;
            const auto options = static_cast<DWORD>(state.x[6]);
            const auto success = DuplicateHandle(
                source_process, source, target_process, &duplicate,
                static_cast<DWORD>(state.x[4]), static_cast<BOOL>(state.x[5]),
                options);
            const auto error = GetLastError();
            if ((options & DUPLICATE_CLOSE_SOURCE) != 0) {
                // Windows closes the source even when duplication fails.
                std::lock_guard lock(state_->mutex);
                state_->host_handles.erase(state.x[1]);
            }
            if (success) {
                memory.write64(state.x[3], guest_handle(duplicate, true));
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetHandleInformation" ||
            function == "SetHandleInformation") {
            const auto handle = host_handle(state.x[0]);
            if (handle == INVALID_HANDLE_VALUE) {
                state.x[0] = 0;
                return std::nullopt;
            }
            if (function == "SetHandleInformation") {
                state.x[0] = static_cast<std::uint32_t>(SetHandleInformation(
                    handle, static_cast<DWORD>(state.x[1]),
                    static_cast<DWORD>(state.x[2])));
                return std::nullopt;
            }
            if (!guest_range(memory, state.x[1], sizeof(DWORD), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            DWORD flags = 0;
            const auto success = GetHandleInformation(handle, &flags);
            const auto error = GetLastError();
            if (success) memory.write32(state.x[1], flags);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        const auto standard_handle = [this, &guest_handle](DWORD which) {
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->standard_handles.find(which);
                if (found != state_->standard_handles.end()) return found->second;
            }
            const auto value = GetStdHandle(which);
            if (value == INVALID_HANDLE_VALUE) return UINT64_MAX;
            if (value == nullptr) return std::uint64_t{0};
            const auto mapped = guest_handle(value, false);
            std::lock_guard lock(state_->mutex);
            state_->standard_handles.emplace(which, mapped);
            return mapped;
        };
        if (function == "GetStdHandle") {
            state.x[0] = standard_handle(static_cast<DWORD>(state.x[0]));
            return std::nullopt;
        }
        if (function == "GetCommandLineW" || function == "GetCommandLineA") {
            state.x[0] = command_line_base +
                         (function == "GetCommandLineA" ? 65536 : 0);
            return std::nullopt;
        }
        if (function == "GetACP") {
            state.x[0] = GetACP();
            return std::nullopt;
        }
        if (function == "GetOEMCP") {
            state.x[0] = GetOEMCP();
            return std::nullopt;
        }
        if (function == "GetConsoleCP") {
            state.x[0] = GetConsoleCP();
            return std::nullopt;
        }
        if (function == "GetConsoleOutputCP") {
            state.x[0] = GetConsoleOutputCP();
            return std::nullopt;
        }
        if (function == "GetSystemInfo" ||
            function == "GetNativeSystemInfo") {
            SYSTEM_INFO info{};
            GetNativeSystemInfo(&info);
            info.wProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
            info.dwProcessorType = 0;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&info);
            for (std::size_t index = 0; index < sizeof(info); ++index) {
                memory.write8(state.x[0] + index, bytes[index]);
            }
            return std::nullopt;
        }
        if (function == "GetLargePageMinimum") {
            state.x[0] = GetLargePageMinimum();
            return std::nullopt;
        }
        if (function == "GetLogicalProcessorInformation" ||
            function == "GetLogicalProcessorInformationEx") {
            const bool extended = function == "GetLogicalProcessorInformationEx";
            const auto output_address = state.x[extended ? 1 : 0];
            const auto length_address = state.x[extended ? 2 : 1];
            if (length_address == 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            DWORD length = memory.read32(length_address);
            if (length > 16U * 1024U * 1024U) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::vector<std::uint8_t> output(length);
            auto* buffer = output_address == 0 || length == 0
                ? nullptr : output.data();
            const auto success = extended
                ? GetLogicalProcessorInformationEx(
                      static_cast<LOGICAL_PROCESSOR_RELATIONSHIP>(state.x[0]),
                      reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
                          buffer), &length)
                : GetLogicalProcessorInformation(
                      reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(
                          buffer), &length);
            const auto error = GetLastError();
            if (success || error == ERROR_INSUFFICIENT_BUFFER) {
                memory.write32(length_address, length);
            }
            if (success) {
                for (DWORD index = 0; index < length; ++index) {
                    memory.write8(output_address + index, output[index]);
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetActiveProcessorCount" ||
            function == "GetMaximumProcessorCount") {
            const auto group = static_cast<WORD>(state.x[0]);
            state.x[0] = function == "GetActiveProcessorCount"
                ? GetActiveProcessorCount(group) : GetMaximumProcessorCount(group);
            return std::nullopt;
        }
        if (function == "GetActiveProcessorGroupCount" ||
            function == "GetMaximumProcessorGroupCount") {
            state.x[0] = function == "GetActiveProcessorGroupCount"
                ? GetActiveProcessorGroupCount()
                : GetMaximumProcessorGroupCount();
            return std::nullopt;
        }
        if (function == "GetCurrentProcessorNumberEx") {
            PROCESSOR_NUMBER number{};
            GetCurrentProcessorNumberEx(&number);
            write_guest_structure(memory, state.x[0], number);
            return std::nullopt;
        }
        if (function == "GetNumaHighestNodeNumber") {
            ULONG number = 0;
            const auto success = GetNumaHighestNodeNumber(&number);
            const auto error = GetLastError();
            if (success) memory.write32(state.x[0], number);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GlobalMemoryStatusEx") {
            if (memory.read32(state.x[0]) != sizeof(MEMORYSTATUSEX)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            const auto success = GlobalMemoryStatusEx(&status);
            const auto error = GetLastError();
            if (success) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&status);
                for (std::size_t index = 0; index < sizeof(status); ++index) {
                    memory.write8(state.x[0] + index, bytes[index]);
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetPhysicallyInstalledSystemMemory") {
            ULONGLONG kilobytes = 0;
            const auto success = GetPhysicallyInstalledSystemMemory(&kilobytes);
            const auto error = GetLastError();
            if (success) memory.write64(state.x[0], kilobytes);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "IsProcessorFeaturePresent") {
            const auto feature = static_cast<DWORD>(state.x[0]);
            state.x[0] = feature == PF_ARM_V8_INSTRUCTIONS_AVAILABLE ||
                         feature == PF_ARM_NEON_INSTRUCTIONS_AVAILABLE;
            return std::nullopt;
        }
        if (function == "AreFileApisANSI") {
            state.x[0] = AreFileApisANSI();
            return std::nullopt;
        }
        if (function == "IsValidCodePage") {
            state.x[0] = IsValidCodePage(static_cast<UINT>(state.x[0]));
            return std::nullopt;
        }
        if (function == "GetCPInfo" || function == "GetCPInfoExW") {
            const auto output = state.x[1];
            if (function == "GetCPInfo") {
                CPINFO info{};
                const auto success = GetCPInfo(static_cast<UINT>(state.x[0]), &info);
                const auto error = GetLastError();
                if (success) {
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&info);
                    for (std::size_t index = 0; index < sizeof(info); ++index) {
                        memory.write8(output + index, bytes[index]);
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
            } else {
                CPINFOEXW info{};
                const auto success = GetCPInfoExW(static_cast<UINT>(state.x[0]),
                                                  static_cast<DWORD>(state.x[1]),
                                                  &info);
                const auto error = GetLastError();
                if (success) {
                    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&info);
                    for (std::size_t index = 0; index < sizeof(info); ++index) {
                        memory.write8(state.x[2] + index, bytes[index]);
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
            }
            return std::nullopt;
        }
        if (function == "LCMapStringW" || function == "LCMapStringEx") {
            const bool extended = function == "LCMapStringEx";
            const auto flags = static_cast<DWORD>(state.x[1]);
            const auto source = state.x[2];
            const auto count = static_cast<int>(state.x[3]);
            const auto destination = state.x[4];
            const auto capacity = static_cast<int>(state.x[5]);
            constexpr int maximum_count = 1024 * 1024;
            if (count < -1 || count > maximum_count || capacity < 0 ||
                capacity > maximum_count ||
                (extended && (state.x[6] != 0 || state.x[7] != 0 ||
                              !guest_range(memory, state.sp, 8, false)))) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto input = guest_wide_span(memory, source, count);
            const auto locale_name = extended && state.x[0] != 0
                ? guest_string(memory, state.x[0], true) : std::wstring{};
            const bool sort_key = (flags & LCMAP_SORTKEY) != 0;
            std::vector<wchar_t> result(capacity == 0 ? 0 :
                (sort_key ? (static_cast<std::size_t>(capacity) + 1) / 2
                          : static_cast<std::size_t>(capacity)));
            auto* output = capacity == 0 ? nullptr : result.data();
            const auto mapped = extended
                ? LCMapStringEx(state.x[0] == 0 ? nullptr : locale_name.c_str(),
                                flags, input.c_str(), count, output, capacity,
                                nullptr, nullptr,
                                static_cast<LPARAM>(memory.read64(state.sp)))
                : LCMapStringW(static_cast<LCID>(state.x[0]), flags,
                               input.c_str(), count, output, capacity);
            const auto error = GetLastError();
            if (mapped > 0 && output != nullptr) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(output);
                const auto byte_count = static_cast<std::size_t>(mapped) *
                                        (sort_key ? 1U : sizeof(wchar_t));
                for (std::size_t index = 0; index < byte_count; ++index) {
                    memory.write8(destination + index, bytes[index]);
                }
            }
            state.x[0] = static_cast<std::uint32_t>(mapped);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "CompareStringW" || function == "CompareStringA" ||
            function == "CompareStringEx" ||
            function == "CompareStringOrdinal") {
            const bool extended = function == "CompareStringEx";
            const bool ordinal = function == "CompareStringOrdinal";
            const bool ansi = function == "CompareStringA";
            const auto first_address = state.x[ordinal ? 0 : 2];
            const auto first_count = static_cast<int>(state.x[ordinal ? 1 : 3]);
            const auto second_address = state.x[ordinal ? 2 : 4];
            const auto second_count = static_cast<int>(state.x[ordinal ? 3 : 5]);
            constexpr int maximum_count = 1024 * 1024;
            if (first_count < -1 || first_count > maximum_count ||
                second_count < -1 || second_count > maximum_count ||
                (extended && (state.x[6] != 0 || state.x[7] != 0 ||
                              !guest_range(memory, state.sp, 8, false)))) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            int comparison = 0;
            if (ansi) {
                const auto first = guest_ansi_span(memory, first_address,
                                                    first_count);
                const auto second = guest_ansi_span(memory, second_address,
                                                     second_count);
                comparison = CompareStringA(static_cast<LCID>(state.x[0]),
                    static_cast<DWORD>(state.x[1]), first.c_str(), first_count,
                    second.c_str(), second_count);
            } else {
                const auto first = guest_wide_span(memory, first_address,
                                                    first_count);
                const auto second = guest_wide_span(memory, second_address,
                                                     second_count);
                if (ordinal) {
                    comparison = CompareStringOrdinal(first.c_str(), first_count,
                        second.c_str(), second_count, state.x[4] != 0);
                } else if (extended) {
                    const auto name = state.x[0] == 0
                        ? std::wstring{} : guest_string(memory, state.x[0], true);
                    comparison = CompareStringEx(
                        state.x[0] == 0 ? nullptr : name.c_str(),
                        static_cast<DWORD>(state.x[1]), first.c_str(), first_count,
                        second.c_str(), second_count, nullptr, nullptr,
                        static_cast<LPARAM>(memory.read64(state.sp)));
                } else {
                    comparison = CompareStringW(static_cast<LCID>(state.x[0]),
                        static_cast<DWORD>(state.x[1]), first.c_str(), first_count,
                        second.c_str(), second_count);
                }
            }
            const auto error = GetLastError();
            state.x[0] = static_cast<std::uint32_t>(comparison);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetStringTypeW" ||
            function == "GetStringTypeExW" ||
            function == "GetStringTypeA") {
            const bool ansi = function == "GetStringTypeA";
            const bool extended = function != "GetStringTypeW";
            const auto info_type = static_cast<DWORD>(state.x[extended ? 1 : 0]);
            const auto source = state.x[extended ? 2 : 1];
            const auto source_count = static_cast<int>(state.x[extended ? 3 : 2]);
            const auto output_address = state.x[extended ? 4 : 3];
            constexpr int maximum_count = 1024 * 1024;
            if (source_count < -1 || source_count == 0 ||
                source_count > maximum_count) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            int length = source_count;
            if (length == -1) {
                length = 0;
                while (length < maximum_count) {
                    const auto address = source +
                        static_cast<std::uint64_t>(length) * (ansi ? 1U : 2U);
                    const auto character = ansi
                        ? memory.read8(address)
                        : static_cast<std::uint16_t>(memory.read8(address)) |
                              (static_cast<std::uint16_t>(
                                   memory.read8(address + 1)) << 8);
                    ++length;
                    if (character == 0) break;
                }
                if (length == maximum_count) {
                    throw std::runtime_error("guest classification string is not terminated");
                }
            }
            std::vector<WORD> categories(static_cast<std::size_t>(length));
            BOOL success = FALSE;
            if (ansi) {
                std::string input(static_cast<std::size_t>(length), '\0');
                for (int index = 0; index < length; ++index) {
                    input[index] = static_cast<char>(memory.read8(source + index));
                }
                success = GetStringTypeA(static_cast<LCID>(state.x[0]), info_type,
                                         input.data(), source_count,
                                         categories.data());
            } else {
                std::wstring input(static_cast<std::size_t>(length), L'\0');
                for (int index = 0; index < length; ++index) {
                    input[index] = static_cast<wchar_t>(
                        static_cast<std::uint16_t>(memory.read8(source + index * 2ULL)) |
                        (static_cast<std::uint16_t>(
                             memory.read8(source + index * 2ULL + 1)) << 8));
                }
                success = extended
                    ? GetStringTypeExW(static_cast<LCID>(state.x[0]), info_type,
                                       input.data(), source_count,
                                       categories.data())
                    : GetStringTypeW(info_type, input.data(), source_count,
                                     categories.data());
            }
            const auto error = GetLastError();
            if (success) {
                for (int index = 0; index < length; ++index) {
                    memory.write8(output_address + index * 2ULL,
                                  static_cast<std::uint8_t>(categories[index]));
                    memory.write8(output_address + index * 2ULL + 1,
                                  static_cast<std::uint8_t>(categories[index] >> 8));
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetFileType") {
            const auto value = host_handle(state.x[0]);
            state.x[0] = value == INVALID_HANDLE_VALUE ? FILE_TYPE_UNKNOWN
                                                      : GetFileType(value);
            return std::nullopt;
        }
        if (function == "K32GetModuleFileNameExA" || function == "K32GetModuleFileNameExW") {
            const auto capacity = static_cast<DWORD>(state.x[3]);
            if (state.x[1] != 0 || capacity == 0 || capacity > 32768) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto query = [&]<class Character>() {
                if (!guest_range(memory, state.x[2], static_cast<std::uint64_t>(capacity) * sizeof(Character), true)) {
                    SetLastError(ERROR_NOACCESS);
                    state.x[0] = 0;
                    return;
                }
                const auto process = host_handle(state.x[0]);
                if (process == INVALID_HANDLE_VALUE) { state.x[0] = 0; return; }
                const auto native = reinterpret_cast<DWORD(WINAPI*)(HANDLE, HMODULE, Character*, DWORD)>(
                    ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), function.c_str()));
                if (native == nullptr) { state.x[0] = 0; SetLastError(ERROR_PROC_NOT_FOUND); return; }
                std::vector<Character> text(capacity);
                const auto count = native(process, nullptr, text.data(), capacity);
                const auto error = GetLastError();
                if (count != 0) {
                    const auto copied = std::min(static_cast<std::uint64_t>(count) + 1, static_cast<std::uint64_t>(capacity));
                    memory.write_bytes(state.x[2], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), copied * sizeof(Character)));
                }
                state.x[0] = count;
                SetLastError(error);
            };
            if (function.ends_with('W')) query.template operator()<wchar_t>();
            else query.template operator()<char>();
            return std::nullopt;
        }
        if (function == "CreateToolhelp32Snapshot") {
            const auto snapshot = ::CreateToolhelp32Snapshot(static_cast<DWORD>(state.x[0]), static_cast<DWORD>(state.x[1]));
            const auto error = GetLastError();
            state.x[0] = snapshot == INVALID_HANDLE_VALUE ? UINT64_MAX : guest_handle(snapshot, true);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "Process32First" || function == "Process32Next" ||
            function == "Process32FirstW" || function == "Process32NextW") {
            struct AnsiEntry {
                DWORD dwSize, cntUsage, th32ProcessID;
                ULONG_PTR th32DefaultHeapID;
                DWORD th32ModuleID, cntThreads, th32ParentProcessID;
                LONG pcPriClassBase;
                DWORD dwFlags;
                char szExeFile[MAX_PATH];
            };
            static_assert(sizeof(AnsiEntry) == 304 && sizeof(PROCESSENTRY32W) == 568);
            const auto enumerate = [&]<class Entry>() {
                if (!guest_range(memory, state.x[1], sizeof(Entry), true) ||
                    !guest_range(memory, state.x[1], sizeof(DWORD), false)) {
                    SetLastError(ERROR_NOACCESS);
                    state.x[0] = 0;
                    return;
                }
                if (memory.read32(state.x[1]) != sizeof(Entry)) {
                    SetLastError(ERROR_BAD_LENGTH);
                    state.x[0] = 0;
                    return;
                }
                const auto snapshot = host_handle(state.x[0]);
                Entry entry{};
                entry.dwSize = sizeof(Entry);
                const auto native = reinterpret_cast<BOOL(WINAPI*)(HANDLE, Entry*)>(
                    ::GetProcAddress(::GetModuleHandleW(L"kernel32.dll"), function.c_str()));
                if (native == nullptr) {
                    SetLastError(ERROR_PROC_NOT_FOUND);
                    state.x[0] = 0;
                    return;
                }
                const auto success = snapshot != INVALID_HANDLE_VALUE && native(snapshot, &entry);
                const auto error = GetLastError();
                if (success) memory.write_bytes(state.x[1], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&entry), sizeof(entry)));
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
            };
            if (function.ends_with('W')) enumerate.template operator()<PROCESSENTRY32W>();
            else enumerate.template operator()<AnsiEntry>();
            return std::nullopt;
        }
        if (function == "GetConsoleWindow") {
            const auto host = ::GetConsoleWindow();
            std::lock_guard lock(state_->mutex);
            state.x[0] = 0;
            if (host == nullptr) return std::nullopt;
            if (const auto found = state_->host_windows.find(host); found != state_->host_windows.end()) {
                state.x[0] = found->second;
                return std::nullopt;
            }
            auto context = std::make_shared<GuestWindowContext>();
            context->bridge = this;
            context->host = host;
            context->guest_handle = state_->next_window_handle;
            state_->next_window_handle += 16;
            std::array<wchar_t, 256> name{};
            ::GetClassNameW(host, name.data(), static_cast<int>(name.size()));
            context->registered_class_name = name.data();
            state_->host_windows.emplace(host, context->guest_handle);
            state_->windows.emplace(context->guest_handle, context);
            state.x[0] = context->guest_handle;
            return std::nullopt;
        }
        if (function == "GetConsoleMode" || function == "GetNumberOfConsoleInputEvents") {
            if (!guest_range(memory, state.x[1], sizeof(DWORD), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto value = host_handle(state.x[0]);
            DWORD mode = 0;
            const auto success = value != INVALID_HANDLE_VALUE &&
                (function == "GetConsoleMode" ? GetConsoleMode(value, &mode)
                                             : GetNumberOfConsoleInputEvents(value, &mode));
            const auto error = GetLastError();
            if (success) memory.write32(state.x[1], mode);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "SetConsoleMode") {
            const auto value = host_handle(state.x[0]);
            state.x[0] = value != INVALID_HANDLE_VALUE &&
                         SetConsoleMode(value, static_cast<DWORD>(state.x[1]));
            return std::nullopt;
        }
        if (function == "GetConsoleScreenBufferInfo") {
            const auto value = host_handle(state.x[0]);
            CONSOLE_SCREEN_BUFFER_INFO info{};
            const auto success = value != INVALID_HANDLE_VALUE &&
                                 GetConsoleScreenBufferInfo(value, &info);
            const auto error = GetLastError();
            if (success) {
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&info);
                for (std::size_t index = 0; index < sizeof(info); ++index) {
                    memory.write8(state.x[1] + index, bytes[index]);
                }
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetStartupInfoW" || function == "GetStartupInfoA") {
            if (state.x[0] == 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return std::nullopt;
            }
            {
                std::lock_guard lock(state_->mutex);
                if (!state_->startup_strings_mapped) {
                    memory.map_memory(startup_strings_base, 4096);
                    state_->startup_strings_mapped = true;
                }
            }
            const bool wide = function == "GetStartupInfoW";
            STARTUPINFOW info_w{};
            STARTUPINFOA info_a{};
            if (wide) GetStartupInfoW(&info_w);
            else GetStartupInfoA(&info_a);
            const auto* bytes = wide
                ? reinterpret_cast<const std::uint8_t*>(&info_w)
                : reinterpret_cast<const std::uint8_t*>(&info_a);
            static_assert(sizeof(STARTUPINFOW) == sizeof(STARTUPINFOA));
            static_assert(sizeof(STARTUPINFOW) == 104);
            for (std::size_t index = 0; index < sizeof(STARTUPINFOW); ++index) {
                memory.write8(state.x[0] + index, bytes[index]);
            }
            const auto mirror_string = [&memory, wide](const void* source,
                                                        std::uint64_t offset) {
                if (!source) return std::uint64_t{0};
                const auto address = startup_strings_base + offset;
                for (std::size_t index = 0; index < 511; ++index) {
                    const auto character = wide
                        ? static_cast<std::uint16_t>(
                              static_cast<const wchar_t*>(source)[index])
                        : static_cast<std::uint16_t>(
                              static_cast<const char*>(source)[index]);
                    memory.write8(address + index * (wide ? 2U : 1U),
                                  static_cast<std::uint8_t>(character));
                    if (wide) {
                        memory.write8(address + index * 2 + 1,
                                      static_cast<std::uint8_t>(character >> 8));
                    }
                    if (character == 0) return address;
                }
                throw std::runtime_error("host startup string is too long");
            };
            memory.write64(state.x[0] + 8,
                           mirror_string(wide ? static_cast<const void*>(info_w.lpReserved)
                                              : info_a.lpReserved, 0));
            memory.write64(state.x[0] + 16,
                           mirror_string(wide ? static_cast<const void*>(info_w.lpDesktop)
                                              : info_a.lpDesktop, 1024));
            memory.write64(state.x[0] + 24,
                           mirror_string(wide ? static_cast<const void*>(info_w.lpTitle)
                                              : info_a.lpTitle, 2048));
            const auto reserved_count = wide ? info_w.cbReserved2 : info_a.cbReserved2;
            const auto* reserved = wide ? info_w.lpReserved2 : info_a.lpReserved2;
            if (reserved_count > 512) {
                throw std::runtime_error("host startup reserved data is too large");
            }
            if (reserved != nullptr) {
                for (WORD index = 0; index < reserved_count; ++index) {
                    memory.write8(startup_strings_base + 3072 + index,
                                  reserved[index]);
                }
            }
            memory.write64(state.x[0] + 72,
                           reserved == nullptr ? 0 : startup_strings_base + 3072);
            const auto flags = wide ? info_w.dwFlags : info_a.dwFlags;
            if ((flags & STARTF_USESTDHANDLES) != 0) {
                memory.write64(state.x[0] + 80,
                               standard_handle(STD_INPUT_HANDLE));
                memory.write64(state.x[0] + 88,
                               standard_handle(STD_OUTPUT_HANDLE));
                memory.write64(state.x[0] + 96,
                               standard_handle(STD_ERROR_HANDLE));
            } else {
                memory.write64(state.x[0] + 80, 0);
                memory.write64(state.x[0] + 88, 0);
                memory.write64(state.x[0] + 96, 0);
            }
            return std::nullopt;
        }
        if (function == "FindFirstFileA" || function == "FindFirstFileW" ||
            function == "FindFirstFileExA" || function == "FindFirstFileExW" ||
            function == "FindNextFileA" || function == "FindNextFileW" ||
            function == "FindClose") {
            static_assert(sizeof(WIN32_FIND_DATAA) == 320);
            static_assert(sizeof(WIN32_FIND_DATAW) == 592);
            static_assert(offsetof(WIN32_FIND_DATAA, cFileName) == 44);
            static_assert(offsetof(WIN32_FIND_DATAW, cFileName) == 44);
            const auto writable = [&memory](std::uint64_t address,
                                            std::uint64_t size) {
                if (address == 0 || size > UINT64_MAX - address) return false;
                const auto region = memory.query_region(address);
                return region && region->writable &&
                       address + size <= region->base + region->size;
            };
            if (function == "FindClose") {
                HANDLE handle = INVALID_HANDLE_VALUE;
                BOOL success = FALSE;
                DWORD error = ERROR_INVALID_HANDLE;
                {
                    std::lock_guard lock(state_->mutex);
                    const auto found = state_->find_handles.find(state.x[0]);
                    if (found != state_->find_handles.end()) {
                        handle = found->second;
                        success = FindClose(handle);
                        error = GetLastError();
                        if (success) state_->find_handles.erase(found);
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
                return std::nullopt;
            }
            const bool wide = function.ends_with('W');
            const bool first = function.starts_with("FindFirstFile");
            const auto output = state.x[first && function.starts_with(
                "FindFirstFileEx") ? 2 : 1];
            if (!writable(output, wide ? sizeof(WIN32_FIND_DATAW)
                                       : sizeof(WIN32_FIND_DATAA))) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = first ? UINT64_MAX : 0;
                return std::nullopt;
            }
            WIN32_FIND_DATAA data_a{};
            WIN32_FIND_DATAW data_w{};
            BOOL success = FALSE;
            DWORD error = ERROR_SUCCESS;
            HANDLE handle = INVALID_HANDLE_VALUE;
            if (first) {
                const bool extended = function.starts_with("FindFirstFileEx");
                if (state.x[0] == 0 ||
                    (extended && (state.x[4] != 0 ||
                                  state.x[5] > UINT32_MAX))) {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    state.x[0] = UINT64_MAX;
                    return std::nullopt;
                }
                if (wide) {
                    const auto pattern = guest_string(memory, state.x[0], true);
                    handle = extended
                        ? FindFirstFileExW(pattern.c_str(),
                            static_cast<FINDEX_INFO_LEVELS>(state.x[1]),
                            &data_w,
                            static_cast<FINDEX_SEARCH_OPS>(state.x[3]),
                            nullptr, static_cast<DWORD>(state.x[5]))
                        : FindFirstFileW(pattern.c_str(), &data_w);
                } else {
                    const auto pattern = guest_ansi_string(memory, state.x[0]);
                    handle = extended
                        ? FindFirstFileExA(pattern.c_str(),
                            static_cast<FINDEX_INFO_LEVELS>(state.x[1]),
                            &data_a,
                            static_cast<FINDEX_SEARCH_OPS>(state.x[3]),
                            nullptr, static_cast<DWORD>(state.x[5]))
                        : FindFirstFileA(pattern.c_str(), &data_a);
                }
                error = GetLastError();
                success = handle != INVALID_HANDLE_VALUE;
                if (success) {
                    std::lock_guard lock(state_->mutex);
                    const auto guest = state_->next_host_handle;
                    state_->next_host_handle += 8;
                    state_->find_handles.emplace(guest, handle);
                    state.x[0] = guest;
                } else {
                    state.x[0] = UINT64_MAX;
                }
            } else {
                {
                    std::lock_guard lock(state_->mutex);
                    const auto found = state_->find_handles.find(state.x[0]);
                    if (found == state_->find_handles.end()) {
                        error = ERROR_INVALID_HANDLE;
                    } else {
                        success = wide ? FindNextFileW(found->second, &data_w)
                                       : FindNextFileA(found->second, &data_a);
                        error = GetLastError();
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(success);
            }
            if (success) {
                if (wide) write_guest_structure(memory, output, data_w);
                else write_guest_structure(memory, output, data_a);
            }
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "CreatePipe") {
            static_assert(sizeof(SECURITY_ATTRIBUTES) == 24);
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = 0;
                return std::nullopt;
            };
            const auto read_output = state.x[0];
            const auto write_output = state.x[1];
            if (!guest_range(memory, read_output, sizeof(std::uint64_t), true) ||
                !guest_range(memory, write_output, sizeof(std::uint64_t), true)) {
                return fail(ERROR_NOACCESS);
            }
            const auto separation = read_output < write_output
                ? write_output - read_output : read_output - write_output;
            if (separation < sizeof(std::uint64_t)) {
                return fail(ERROR_INVALID_PARAMETER);
            }
            SECURITY_ATTRIBUTES security{};
            SECURITY_ATTRIBUTES* attributes = nullptr;
            if (state.x[2] != 0) {
                if (!guest_range(memory, state.x[2], sizeof(security), false)) {
                    return fail(ERROR_NOACCESS);
                }
                if (memory.read32(state.x[2]) != sizeof(security)) {
                    return fail(ERROR_INVALID_PARAMETER);
                }
                if (memory.read64(state.x[2] + 8) != 0) {
                    // A guest SECURITY_DESCRIPTOR cannot be passed to the host.
                    return fail(ERROR_NOT_SUPPORTED);
                }
                security.nLength = sizeof(security);
                security.bInheritHandle = memory.read32(state.x[2] + 16) != 0;
                attributes = &security;
            }
            HANDLE read_end = nullptr;
            HANDLE write_end = nullptr;
            const auto success = ::CreatePipe(&read_end, &write_end,
                                              attributes,
                                              static_cast<DWORD>(state.x[3]));
            const auto error = GetLastError();
            if (success) {
                memory.write64(read_output, guest_handle(read_end, true));
                memory.write64(write_output, guest_handle(write_end, true));
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "CreateFileW" || function == "CreateFileA") {
            if (state.x[3] != 0 || state.x[6] != 0) {
                throw std::runtime_error(
                    "guest CreateFile security attributes or template handle are unsupported");
            }
            HANDLE value = INVALID_HANDLE_VALUE;
            if (function == "CreateFileW") {
                const auto path = guest_string(memory, state.x[0], true);
                value = CreateFileW(path.c_str(), static_cast<DWORD>(state.x[1]),
                                    static_cast<DWORD>(state.x[2]), nullptr,
                                    static_cast<DWORD>(state.x[4]),
                                    static_cast<DWORD>(state.x[5]), nullptr);
            } else {
                const auto path = guest_ansi_string(memory, state.x[0]);
                value = CreateFileA(path.c_str(), static_cast<DWORD>(state.x[1]),
                                    static_cast<DWORD>(state.x[2]), nullptr,
                                    static_cast<DWORD>(state.x[4]),
                                    static_cast<DWORD>(state.x[5]), nullptr);
            }
            const auto error = GetLastError();
            state.x[0] = value == INVALID_HANDLE_VALUE ? UINT64_MAX
                                                      : guest_handle(value, true);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "CloseHandle") {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->host_handles.find(state.x[0]);
            if (found == state_->host_handles.end()) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto success = CloseHandle(found->second.value);
            const auto error = GetLastError();
            if (success) state_->host_handles.erase(found);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "ReadFile" || function == "WriteFile") {
            constexpr DWORD maximum_io = 64U * 1024U * 1024U;
            const auto value = host_handle(state.x[0]);
            const auto count = static_cast<DWORD>(state.x[2]);
            if (count > maximum_io || state.x[4] != 0) {
                throw std::runtime_error("guest file I/O size or OVERLAPPED is unsupported");
            }
            DWORD transferred = 0;
            BOOL success = FALSE;
            if (value != INVALID_HANDLE_VALUE) {
                std::vector<std::uint8_t> buffer(count);
                if (function == "WriteFile") {
                    copy_from_guest(memory, state.x[1], buffer);
                    success = WriteFile(value, buffer.data(), count,
                                        &transferred, nullptr);
                } else {
                    success = ReadFile(value, buffer.data(), count,
                                       &transferred, nullptr);
                    if (success) {
                        copy_to_guest(memory, state.x[1],
                                      std::span(buffer).first(transferred));
                    }
                }
            }
            const auto error = GetLastError();
            if (state.x[3] != 0) memory.write32(state.x[3], transferred);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "PeekNamedPipe") {
            constexpr DWORD maximum_peek = 64U * 1024U * 1024U;
            const auto requested = static_cast<DWORD>(state.x[2]);
            const auto count = state.x[1] == 0 ? 0U : requested;
            if (count > maximum_peek ||
                (count != 0 && !guest_range(memory, state.x[1], count, true)) ||
                (state.x[3] != 0 &&
                 !guest_range(memory, state.x[3], sizeof(DWORD), true)) ||
                (state.x[4] != 0 &&
                 !guest_range(memory, state.x[4], sizeof(DWORD), true)) ||
                (state.x[5] != 0 &&
                 !guest_range(memory, state.x[5], sizeof(DWORD), true))) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto value = host_handle(state.x[0]);
            if (value == INVALID_HANDLE_VALUE) {
                state.x[0] = 0;
                return std::nullopt;
            }
            std::vector<std::uint8_t> buffer(count);
            DWORD copied = 0, available = 0, remaining = 0;
            const auto success = ::PeekNamedPipe(value,
                state.x[1] == 0 ? nullptr : buffer.data(), count,
                &copied, &available, &remaining);
            const auto error = GetLastError();
            if (success) {
                if (state.x[5] != 0) {
                    DWORD pipe_flags = 0;
                    if (::GetNamedPipeInfo(value, &pipe_flags, nullptr, nullptr,
                                           nullptr) &&
                        (pipe_flags & PIPE_TYPE_MESSAGE) == 0) {
                        // The documented byte-pipe result is zero. Some Windows
                        // versions return an unsigned underflow after a short peek.
                        remaining = 0;
                    }
                }
                for (DWORD index = 0; index < copied; ++index) {
                    memory.write8(state.x[1] + index, buffer[index]);
                }
                if (state.x[3] != 0) memory.write32(state.x[3], copied);
                if (state.x[4] != 0) memory.write32(state.x[4], available);
                if (state.x[5] != 0) memory.write32(state.x[5], remaining);
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetFileSize") {
            if (state.x[1] != 0 &&
                !guest_range(memory, state.x[1], sizeof(DWORD), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = INVALID_FILE_SIZE;
                return std::nullopt;
            }
            const auto value = host_handle(state.x[0]);
            if (value == INVALID_HANDLE_VALUE) {
                state.x[0] = INVALID_FILE_SIZE;
                return std::nullopt;
            }
            DWORD high = 0;
            const auto low = ::GetFileSize(value,
                state.x[1] == 0 ? nullptr : &high);
            const auto error = GetLastError();
            if (state.x[1] != 0) memory.write32(state.x[1], high);
            state.x[0] = low;
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetFileSizeEx") {
            LARGE_INTEGER size{};
            const auto value = host_handle(state.x[0]);
            const auto success = value != INVALID_HANDLE_VALUE &&
                                 GetFileSizeEx(value, &size);
            const auto error = GetLastError();
            if (success) memory.write64(state.x[1], size.QuadPart);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "SetEndOfFile") {
            const auto value = host_handle(state.x[0]);
            if (value == INVALID_HANDLE_VALUE) {
                state.x[0] = 0;
                return std::nullopt;
            }
            state.x[0] = static_cast<std::uint32_t>(::SetEndOfFile(value));
            return std::nullopt;
        }
        if (function == "GetFileInformationByHandle" ||
            function == "GetFileInformationByHandleEx" ||
            function == "GetFileTime" || function == "SetFileTime" ||
            function == "GetFinalPathNameByHandleW" ||
            function == "GetFinalPathNameByHandleA") {
            static_assert(sizeof(FILETIME) == 8);
            static_assert(sizeof(BY_HANDLE_FILE_INFORMATION) == 52);
            static_assert(offsetof(BY_HANDLE_FILE_INFORMATION,
                                   dwVolumeSerialNumber) == 28);
            static_assert(offsetof(BY_HANDLE_FILE_INFORMATION,
                                   nFileIndexLow) == 48);
            static_assert(sizeof(FILE_BASIC_INFO) == 40);
            static_assert(sizeof(FILE_STANDARD_INFO) == 24);
            static_assert(offsetof(FILE_NAME_INFO, FileName) == 4);
            static_assert(sizeof(FILE_ATTRIBUTE_TAG_INFO) == 8);
            static_assert(sizeof(FILE_ID_INFO) == 24);
            const auto write_bytes = [&memory](std::uint64_t address,
                                               const void* source,
                                               std::size_t size) {
                const auto* bytes = static_cast<const std::uint8_t*>(source);
                for (std::size_t index = 0; index < size; ++index) {
                    memory.write8(address + index, bytes[index]);
                }
            };
            const auto read_filetime = [&memory](std::uint64_t address,
                                                 FILETIME& value) {
                if (!guest_range(memory, address, sizeof(FILETIME), false)) {
                    return false;
                }
                value.dwLowDateTime = memory.read32(address);
                value.dwHighDateTime = memory.read32(address + 4);
                return true;
            };
            const auto value = host_handle(state.x[0]);
            if (value == INVALID_HANDLE_VALUE) {
                state.x[0] = function.starts_with("GetFinalPathName") ? 0 : FALSE;
                return std::nullopt;
            }
            if (function == "GetFileInformationByHandle") {
                const auto output = state.x[1];
                if (!guest_range(memory, output,
                                 sizeof(BY_HANDLE_FILE_INFORMATION), true)) {
                    SetLastError(ERROR_NOACCESS);
                    state.x[0] = FALSE;
                    return std::nullopt;
                }
                BY_HANDLE_FILE_INFORMATION information{};
                const auto success = ::GetFileInformationByHandle(
                    value, &information);
                const auto error = GetLastError();
                if (success) write_bytes(output, &information, sizeof(information));
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
                return std::nullopt;
            }
            if (function == "GetFileInformationByHandleEx") {
                const auto info_class = static_cast<FILE_INFO_BY_HANDLE_CLASS>(
                    static_cast<std::uint32_t>(state.x[1]));
                const auto output = state.x[2];
                const auto length = static_cast<std::uint32_t>(state.x[3]);
                const bool supported_class =
                    info_class == FileBasicInfo ||
                    info_class == FileStandardInfo ||
                    info_class == FileNameInfo ||
                    info_class == FileStreamInfo ||
                    info_class == FileCompressionInfo ||
                    info_class == FileAttributeTagInfo ||
                    info_class == FileStorageInfo ||
                    info_class == FileAlignmentInfo ||
                    info_class == FileIdInfo ||
                    info_class == FileNormalizedNameInfo;
                if (state.x[1] != static_cast<std::uint32_t>(state.x[1]) ||
                    state.x[3] != length || length == 0 ||
                    length > 1024U * 1024U || !supported_class) {
                    SetLastError(!supported_class ? ERROR_NOT_SUPPORTED :
                                 ERROR_INVALID_PARAMETER);
                    state.x[0] = FALSE;
                    return std::nullopt;
                }
                if (!guest_range(memory, output, length, true)) {
                    SetLastError(ERROR_NOACCESS);
                    state.x[0] = FALSE;
                    return std::nullopt;
                }
                std::vector<std::uint8_t> information(length);
                if (guest_range(memory, output, length, false)) {
                    for (std::uint32_t index = 0; index < length; ++index) {
                        information[index] = memory.read8(output + index);
                    }
                }
                const auto success = ::GetFileInformationByHandleEx(
                    value, info_class, information.data(), length);
                const auto error = GetLastError();
                if (success) {
                    write_bytes(output, information.data(), length);
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
                return std::nullopt;
            }
            if (function == "GetFileTime" || function == "SetFileTime") {
                const std::array<std::uint64_t, 3> addresses{
                    state.x[1], state.x[2], state.x[3]};
                std::array<FILETIME, 3> times{};
                for (std::size_t index = 0; index < addresses.size(); ++index) {
                    if (addresses[index] == 0) continue;
                    if (function == "SetFileTime"
                            ? !read_filetime(addresses[index], times[index])
                            : !guest_range(memory, addresses[index],
                                           sizeof(FILETIME), true)) {
                        SetLastError(ERROR_NOACCESS);
                        state.x[0] = FALSE;
                        return std::nullopt;
                    }
                }
                const auto first = addresses[0] == 0 ? nullptr : &times[0];
                const auto second = addresses[1] == 0 ? nullptr : &times[1];
                const auto third = addresses[2] == 0 ? nullptr : &times[2];
                const auto success = function == "GetFileTime"
                    ? ::GetFileTime(value, first, second, third)
                    : ::SetFileTime(value, first, second, third);
                const auto error = GetLastError();
                if (success && function == "GetFileTime") {
                    for (std::size_t index = 0; index < addresses.size(); ++index) {
                        if (addresses[index] != 0) {
                            write_bytes(addresses[index], &times[index],
                                        sizeof(FILETIME));
                        }
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
                return std::nullopt;
            }
            const bool wide = function.ends_with('W');
            const auto output = state.x[1];
            const auto capacity = static_cast<std::uint32_t>(state.x[2]);
            const auto flags = static_cast<std::uint32_t>(state.x[3]);
            if (state.x[2] != capacity || state.x[3] != flags ||
                capacity > 1024U * 1024U) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (capacity != 0 &&
                !guest_range(memory, output,
                             static_cast<std::uint64_t>(capacity) *
                                 (wide ? 2ULL : 1ULL), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::vector<wchar_t> wide_path(wide ? capacity : 0);
            std::vector<char> ansi_path(wide ? 0 : capacity);
            const auto result = wide
                ? ::GetFinalPathNameByHandleW(value,
                    capacity == 0 ? nullptr : wide_path.data(),
                    capacity, flags)
                : ::GetFinalPathNameByHandleA(value,
                    capacity == 0 ? nullptr : ansi_path.data(),
                    capacity, flags);
            const auto error = GetLastError();
            if (result != 0 && result < capacity) {
                if (wide) {
                    write_bytes(output, wide_path.data(),
                                (static_cast<std::size_t>(result) + 1) * 2);
                } else {
                    write_bytes(output, ansi_path.data(),
                                static_cast<std::size_t>(result) + 1);
                }
            }
            state.x[0] = result;
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "SetFilePointer" || function == "SetFilePointerEx") {
            const auto value = host_handle(state.x[0]);
            if (function == "SetFilePointerEx") {
                LARGE_INTEGER distance{};
                distance.QuadPart = static_cast<LONGLONG>(state.x[1]);
                LARGE_INTEGER position{};
                const auto success = value != INVALID_HANDLE_VALUE &&
                    SetFilePointerEx(value, distance, &position,
                                     static_cast<DWORD>(state.x[3]));
                const auto error = GetLastError();
                if (success && state.x[2] != 0) {
                    memory.write64(state.x[2], position.QuadPart);
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
            } else {
                LONG high = state.x[2] == 0 ? 0 :
                    static_cast<LONG>(memory.read32(state.x[2]));
                const auto result = value == INVALID_HANDLE_VALUE
                    ? INVALID_SET_FILE_POINTER
                    : SetFilePointer(value, static_cast<LONG>(state.x[1]),
                                     state.x[2] == 0 ? nullptr : &high,
                                     static_cast<DWORD>(state.x[3]));
                const auto error = GetLastError();
                if (state.x[2] != 0) memory.write32(state.x[2], high);
                state.x[0] = result;
                SetLastError(error);
            }
            return std::nullopt;
        }
        if (function == "GetLogicalDrives") {
            state.x[0] = ::GetLogicalDrives();
            return std::nullopt;
        }
        if (function == "GetLogicalDriveStringsA" ||
            function == "GetLogicalDriveStringsW") {
            const bool wide = function.ends_with('W');
            const auto capacity = static_cast<std::uint32_t>(state.x[0]);
            if (state.x[0] != capacity) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto output = state.x[1];
            if (capacity != 0) {
                const auto bytes = static_cast<std::uint64_t>(capacity) *
                                   (wide ? 2 : 1);
                const auto region = output == 0 || output > UINT64_MAX - bytes
                    ? std::optional<GuestMemory::RegionInfo>{}
                    : memory.query_region(output);
                if (!region || !region->writable ||
                    output + bytes > region->base + region->size) {
                    SetLastError(ERROR_NOACCESS);
                    state.x[0] = 0;
                    return std::nullopt;
                }
            }
            // A drive-letter list can contain at most 26 four-character roots
            // plus the final NUL; host memory is never exposed to the guest.
            constexpr DWORD host_capacity = 128;
            if (wide) {
                std::array<wchar_t, host_capacity> native{};
                const auto result = ::GetLogicalDriveStringsW(
                    host_capacity, native.data());
                const auto error = GetLastError();
                if (result != 0 && result < host_capacity &&
                    capacity > result) {
                    for (DWORD index = 0; index <= result; ++index) {
                        const auto unit = static_cast<std::uint16_t>(native[index]);
                        memory.write8(output + index * 2,
                                      static_cast<std::uint8_t>(unit));
                        memory.write8(output + index * 2 + 1,
                                      static_cast<std::uint8_t>(unit >> 8));
                    }
                }
                state.x[0] = capacity <= result && result != 0
                    ? result + 1 : result;
                SetLastError(error);
            } else {
                std::array<char, host_capacity> native{};
                const auto result = ::GetLogicalDriveStringsA(
                    host_capacity, native.data());
                const auto error = GetLastError();
                if (result != 0 && result < host_capacity &&
                    capacity > result) {
                    for (DWORD index = 0; index <= result; ++index) {
                        memory.write8(output + index,
                                      static_cast<std::uint8_t>(native[index]));
                    }
                }
                state.x[0] = capacity <= result && result != 0
                    ? result + 1 : result;
                SetLastError(error);
            }
            return std::nullopt;
        }
        if (function == "GetDriveTypeA" || function == "GetDriveTypeW" ||
            function == "GetDiskFreeSpaceExA" ||
            function == "GetDiskFreeSpaceExW") {
            const bool wide = function.ends_with('W');
            const bool space = function.starts_with("GetDiskFreeSpaceEx");
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = space ? 0 : DRIVE_UNKNOWN;
                return std::nullopt;
            };
            const auto path_address = state.x[0];
            if (path_address != 0) {
                const auto width = wide ? 2ULL : 1ULL;
                const auto region = memory.query_region(path_address);
                if (!region || !region->readable ||
                    path_address > UINT64_MAX - width ||
                    path_address + width > region->base + region->size) {
                    return fail(ERROR_NOACCESS);
                }
                const auto limit = std::min<std::uint64_t>(32768,
                    (region->base + region->size - path_address) / width);
                bool terminated = false;
                for (std::uint64_t index = 0; index < limit; ++index) {
                    const auto unit = wide
                        ? static_cast<std::uint16_t>(
                            memory.read8(path_address + index * 2) |
                            (memory.read8(path_address + index * 2 + 1) << 8))
                        : memory.read8(path_address + index);
                    if (unit == 0) {
                        terminated = true;
                        break;
                    }
                }
                if (!terminated) return fail(ERROR_INVALID_NAME);
            }
            if (space) {
                for (unsigned index = 1; index <= 3; ++index) {
                    const auto output = state.x[index];
                    if (output == 0) continue;
                    const auto region = output > UINT64_MAX - 8
                        ? std::optional<GuestMemory::RegionInfo>{}
                        : memory.query_region(output);
                    if (!region || !region->writable ||
                        output + 8 > region->base + region->size) {
                        return fail(ERROR_NOACCESS);
                    }
                }
                ULARGE_INTEGER available{}, total{}, free{};
                const auto success = wide
                    ? ::GetDiskFreeSpaceExW(path_address == 0 ? nullptr :
                        guest_string(memory, path_address, true).c_str(),
                        &available, &total, &free)
                    : ::GetDiskFreeSpaceExA(path_address == 0 ? nullptr :
                        guest_ansi_string(memory, path_address).c_str(),
                        &available, &total, &free);
                const auto error = GetLastError();
                if (success) {
                    if (state.x[1] != 0) memory.write64(state.x[1], available.QuadPart);
                    if (state.x[2] != 0) memory.write64(state.x[2], total.QuadPart);
                    if (state.x[3] != 0) memory.write64(state.x[3], free.QuadPart);
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
            } else if (wide) {
                const auto path = path_address == 0 ? std::wstring{} :
                    guest_string(memory, path_address, true);
                state.x[0] = ::GetDriveTypeW(
                    path_address == 0 ? nullptr : path.c_str());
            } else {
                const auto path = path_address == 0 ? std::string{} :
                    guest_ansi_string(memory, path_address);
                state.x[0] = ::GetDriveTypeA(
                    path_address == 0 ? nullptr : path.c_str());
            }
            return std::nullopt;
        }
        if (function == "GetFileAttributesW" ||
            function == "GetFileAttributesA" ||
            function == "GetFileAttributesExW" ||
            function == "GetFileAttributesExA" ||
            function == "SetFileAttributesW" ||
            function == "SetFileAttributesA" ||
            function == "CreateDirectoryW" ||
            function == "CreateDirectoryA" ||
            function == "RemoveDirectoryW" ||
            function == "RemoveDirectoryA") {
            const bool wide = function.ends_with('W');
            const bool basic_attributes = function.starts_with("GetFileAttributes") &&
                                          !function.starts_with("GetFileAttributesEx");
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = basic_attributes ? INVALID_FILE_ATTRIBUTES : 0;
                return std::nullopt;
            };
            const auto path_address = state.x[0];
            const auto path_region = path_address == 0
                ? std::optional<GuestMemory::RegionInfo>{}
                : memory.query_region(path_address);
            const auto width = wide ? 2ULL : 1ULL;
            if (!path_region || !path_region->readable ||
                path_address > UINT64_MAX - width ||
                path_address + width > path_region->base + path_region->size) {
                return fail(ERROR_NOACCESS);
            }
            const auto length_limit = std::min<std::uint64_t>(
                32768, (path_region->base + path_region->size - path_address) /
                           width);
            bool terminated = false;
            for (std::uint64_t index = 0; index < length_limit; ++index) {
                const auto unit = wide
                    ? static_cast<std::uint16_t>(
                          memory.read8(path_address + index * 2) |
                          (memory.read8(path_address + index * 2 + 1) << 8))
                    : memory.read8(path_address + index);
                if (unit == 0) {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) return fail(ERROR_INVALID_NAME);
            if (function.starts_with("CreateDirectory") && state.x[1] != 0) {
                // A guest SECURITY_ATTRIBUTES can contain guest pointers and
                // must never be passed to the host API unchanged.
                return fail(ERROR_NOT_SUPPORTED);
            }
            if (function.starts_with("GetFileAttributesEx")) {
                static_assert(sizeof(WIN32_FILE_ATTRIBUTE_DATA) == 36);
                const auto output = state.x[2];
                const auto region = output == 0 || output > UINT64_MAX - 36
                    ? std::optional<GuestMemory::RegionInfo>{}
                    : memory.query_region(output);
                if (state.x[1] != GetFileExInfoStandard ||
                    !region || !region->writable ||
                    output + 36 > region->base + region->size) {
                    return fail(ERROR_INVALID_PARAMETER);
                }
                WIN32_FILE_ATTRIBUTE_DATA data{};
                const auto success = wide
                    ? ::GetFileAttributesExW(
                          guest_string(memory, path_address, true).c_str(),
                          GetFileExInfoStandard, &data)
                    : ::GetFileAttributesExA(
                          guest_ansi_string(memory, path_address).c_str(),
                          GetFileExInfoStandard, &data);
                const auto error = GetLastError();
                if (success) {
                    memory.write32(output, data.dwFileAttributes);
                    memory.write64(output + 4,
                        (static_cast<std::uint64_t>(data.ftCreationTime.dwHighDateTime)
                             << 32) | data.ftCreationTime.dwLowDateTime);
                    memory.write64(output + 12,
                        (static_cast<std::uint64_t>(data.ftLastAccessTime.dwHighDateTime)
                             << 32) | data.ftLastAccessTime.dwLowDateTime);
                    memory.write64(output + 20,
                        (static_cast<std::uint64_t>(data.ftLastWriteTime.dwHighDateTime)
                             << 32) | data.ftLastWriteTime.dwLowDateTime);
                    memory.write32(output + 28, data.nFileSizeHigh);
                    memory.write32(output + 32, data.nFileSizeLow);
                }
                state.x[0] = static_cast<std::uint32_t>(success);
                SetLastError(error);
                return std::nullopt;
            }
            if (wide) {
                const auto path = guest_string(memory, path_address, true);
                if (basic_attributes) {
                    state.x[0] = ::GetFileAttributesW(path.c_str());
                } else if (function.starts_with("SetFileAttributes")) {
                    state.x[0] = static_cast<std::uint32_t>(
                        ::SetFileAttributesW(path.c_str(),
                            static_cast<DWORD>(state.x[1])));
                } else if (function.starts_with("CreateDirectory")) {
                    state.x[0] = static_cast<std::uint32_t>(
                        ::CreateDirectoryW(path.c_str(), nullptr));
                } else {
                    state.x[0] = static_cast<std::uint32_t>(
                        ::RemoveDirectoryW(path.c_str()));
                }
            } else {
                const auto path = guest_ansi_string(memory, path_address);
                if (basic_attributes) {
                    state.x[0] = ::GetFileAttributesA(path.c_str());
                } else if (function.starts_with("SetFileAttributes")) {
                    state.x[0] = static_cast<std::uint32_t>(
                        ::SetFileAttributesA(path.c_str(),
                            static_cast<DWORD>(state.x[1])));
                } else if (function.starts_with("CreateDirectory")) {
                    state.x[0] = static_cast<std::uint32_t>(
                        ::CreateDirectoryA(path.c_str(), nullptr));
                } else {
                    state.x[0] = static_cast<std::uint32_t>(
                        ::RemoveDirectoryA(path.c_str()));
                }
            }
            return std::nullopt;
        }
        if (function == "MoveFileExW" || function == "MoveFileExA" ||
            function == "MoveFileW" || function == "MoveFileA" ||
            function == "CopyFileW" || function == "CopyFileA" ||
            function == "DeleteFileW" || function == "DeleteFileA" ||
            function == "ReplaceFileW" || function == "ReplaceFileA" ||
            function == "CreateHardLinkW" || function == "CreateHardLinkA") {
            const bool wide = function.ends_with('W');
            const auto fail = [&](DWORD error) {
                SetLastError(error);
                state.x[0] = FALSE;
                return std::nullopt;
            };
            const auto path_error = [&](std::uint64_t address,
                                        bool optional = false)
                -> std::optional<DWORD> {
                if (optional && address == 0) return std::nullopt;
                const auto region = address == 0
                    ? std::optional<GuestMemory::RegionInfo>{}
                    : memory.query_region(address);
                const auto width = wide ? 2ULL : 1ULL;
                if (!region || !region->readable ||
                    address > UINT64_MAX - width ||
                    address + width > region->base + region->size) {
                    return ERROR_NOACCESS;
                }
                const auto length_limit = std::min<std::uint64_t>(
                    32768, (region->base + region->size - address) / width);
                for (std::uint64_t index = 0; index < length_limit; ++index) {
                    const auto character = wide
                        ? static_cast<std::uint16_t>(
                            memory.read8(address + index * 2) |
                            (memory.read8(address + index * 2 + 1) << 8))
                        : memory.read8(address + index);
                    if (character == 0) return std::nullopt;
                }
                return ERROR_INVALID_NAME;
            };
            const bool single_path = function.starts_with("DeleteFile");
            const bool replace = function.starts_with("ReplaceFile");
            const bool hard_link = function.starts_with("CreateHardLink");
            if (const auto error = path_error(state.x[0]); error) {
                return fail(*error);
            }
            if (!single_path) {
                if (const auto error = path_error(state.x[1]); error) {
                    return fail(*error);
                }
            }
            if (replace) {
                if (const auto error = path_error(state.x[2], true); error) {
                    return fail(*error);
                }
                constexpr DWORD allowed = REPLACEFILE_IGNORE_MERGE_ERRORS |
                                          REPLACEFILE_IGNORE_ACL_ERRORS;
                if (state.x[3] > UINT32_MAX ||
                    (state.x[3] & ~static_cast<std::uint64_t>(allowed)) != 0 ||
                    state.x[4] != 0 || state.x[5] != 0) {
                    return fail(ERROR_INVALID_PARAMETER);
                }
            } else if (hard_link && state.x[2] != 0) {
                // SECURITY_ATTRIBUTES may contain a guest security descriptor.
                return fail(ERROR_NOT_SUPPORTED);
            } else if (function.starts_with("MoveFileEx")) {
                constexpr DWORD allowed = MOVEFILE_REPLACE_EXISTING |
                                          MOVEFILE_COPY_ALLOWED |
                                          MOVEFILE_WRITE_THROUGH |
                                          MOVEFILE_FAIL_IF_NOT_TRACKABLE;
                if (state.x[2] > UINT32_MAX ||
                    (state.x[2] & MOVEFILE_DELAY_UNTIL_REBOOT) != 0) {
                    return fail(ERROR_NOT_SUPPORTED);
                }
                if ((state.x[2] & ~static_cast<std::uint64_t>(allowed)) != 0) {
                    return fail(ERROR_INVALID_PARAMETER);
                }
            }
            if (wide) {
                const auto first = guest_string(memory, state.x[0], true);
                const auto second = single_path ? std::wstring{} :
                    guest_string(memory, state.x[1], true);
                if (function.starts_with("MoveFileEx")) {
                    state.x[0] = ::MoveFileExW(first.c_str(), second.c_str(),
                                              static_cast<DWORD>(state.x[2]));
                } else if (function.starts_with("MoveFile")) {
                    state.x[0] = ::MoveFileW(first.c_str(), second.c_str());
                } else if (function.starts_with("CopyFile")) {
                    state.x[0] = ::CopyFileW(first.c_str(), second.c_str(),
                                            state.x[2] != 0);
                } else if (single_path) {
                    state.x[0] = ::DeleteFileW(first.c_str());
                } else if (replace) {
                    const auto backup = state.x[2] == 0 ? std::wstring{} :
                        guest_string(memory, state.x[2], true);
                    state.x[0] = ::ReplaceFileW(first.c_str(), second.c_str(),
                        state.x[2] == 0 ? nullptr : backup.c_str(),
                        static_cast<DWORD>(state.x[3]), nullptr, nullptr);
                } else {
                    state.x[0] = ::CreateHardLinkW(first.c_str(),
                                                   second.c_str(), nullptr);
                }
            } else {
                const auto first = guest_ansi_string(memory, state.x[0]);
                const auto second = single_path ? std::string{} :
                    guest_ansi_string(memory, state.x[1]);
                if (function.starts_with("MoveFileEx")) {
                    state.x[0] = ::MoveFileExA(first.c_str(), second.c_str(),
                                              static_cast<DWORD>(state.x[2]));
                } else if (function.starts_with("MoveFile")) {
                    state.x[0] = ::MoveFileA(first.c_str(), second.c_str());
                } else if (function.starts_with("CopyFile")) {
                    state.x[0] = ::CopyFileA(first.c_str(), second.c_str(),
                                            state.x[2] != 0);
                } else if (single_path) {
                    state.x[0] = ::DeleteFileA(first.c_str());
                } else if (replace) {
                    const auto backup = state.x[2] == 0 ? std::string{} :
                        guest_ansi_string(memory, state.x[2]);
                    state.x[0] = ::ReplaceFileA(first.c_str(), second.c_str(),
                        state.x[2] == 0 ? nullptr : backup.c_str(),
                        static_cast<DWORD>(state.x[3]), nullptr, nullptr);
                } else {
                    state.x[0] = ::CreateHardLinkA(first.c_str(),
                                                   second.c_str(), nullptr);
                }
            }
            return std::nullopt;
        }
        if (function == "GetProcessHeap") {
            state.x[0] = process_heap_handle;
            return std::nullopt;
        }
        if (function == "HeapAlloc" || function == "HeapReAlloc" ||
            function == "HeapFree" || function == "HeapSize" ||
            function == "HeapValidate") {
            if (state.x[0] != process_heap_handle) {
                SetLastError(ERROR_INVALID_HANDLE);
                state.x[0] = function == "HeapSize" ? UINT64_MAX : 0;
                return std::nullopt;
            }
            const auto flags = static_cast<DWORD>(state.x[1]);
            if ((flags & ~(HEAP_ZERO_MEMORY | HEAP_REALLOC_IN_PLACE_ONLY)) != 0) {
                throw std::runtime_error("unsupported guest process-heap flags");
            }
            const auto allocate = [this, &memory](std::uint64_t requested) {
                if (requested > 1024ULL * 1024ULL * 1024ULL) return std::uint64_t{0};
                const auto size = static_cast<std::size_t>(
                    (std::max(requested, 1ULL) + 15) & ~15ULL);
                const auto mapped_size = (size + 4095) & ~std::size_t{4095};
                std::lock_guard lock(state_->mutex);
                if (state_->next_heap_address > UINT64_MAX - 4095) {
                    return std::uint64_t{0};
                }
                const auto address = (state_->next_heap_address + 4095) &
                                     ~std::uint64_t{4095};
                if (address > UINT64_MAX - mapped_size) return std::uint64_t{0};
                try {
                    memory.map_memory(address, mapped_size);
                } catch (const std::bad_alloc&) {
                    return std::uint64_t{0};
                }
                state_->next_heap_address = address + mapped_size;
                state_->heap_allocations.emplace(
                    address, State::HeapAllocation{size, false, true});
                return address;
            };
            if (function == "HeapAlloc") {
                state.x[0] = allocate(state.x[2]);
                if (state.x[0] == 0) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                return std::nullopt;
            }
            const auto address = state.x[2];
            std::size_t old_size = 0;
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->heap_allocations.find(address);
                if (found != state_->heap_allocations.end() &&
                    found->second.process_heap) old_size = found->second.size;
            }
            if (old_size == 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = function == "HeapSize" ? UINT64_MAX : 0;
                return std::nullopt;
            }
            if (function == "HeapSize") {
                state.x[0] = old_size;
                return std::nullopt;
            }
            if (function == "HeapValidate") {
                state.x[0] = 1;
                return std::nullopt;
            }
            if (function == "HeapFree") {
                memory.unmap_memory(address);
                std::lock_guard lock(state_->mutex);
                state_->heap_allocations.erase(address);
                state.x[0] = 1;
                return std::nullopt;
            }
            if ((flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto replacement = allocate(state.x[3]);
            if (replacement == 0) {
                SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto copy_size = std::min<std::uint64_t>(old_size, state.x[3]);
            copy_guest(memory, replacement, address, copy_size);
            memory.unmap_memory(address);
            {
                std::lock_guard lock(state_->mutex);
                state_->heap_allocations.erase(address);
            }
            state.x[0] = replacement;
            return std::nullopt;
        }
        if (function == "GetFullPathNameW" ||
            function == "GetFullPathNameA") {
            const bool wide = function.ends_with('W');
            const auto input_address = state.x[0];
            const auto capacity = static_cast<std::uint32_t>(state.x[1]);
            const auto output_address = state.x[2];
            const auto file_part_address = state.x[3];
            if (state.x[1] != capacity || capacity > 1024U * 1024U) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (!guest_range(memory, input_address, wide ? 2 : 1, false) ||
                (capacity != 0 &&
                 !guest_range(memory, output_address,
                              static_cast<std::uint64_t>(capacity) *
                                  (wide ? 2ULL : 1ULL), true)) ||
                (file_part_address != 0 &&
                 !guest_range(memory, file_part_address, 8, true))) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::wstring wide_input;
            std::string ansi_input;
            try {
                if (wide) wide_input = guest_string(memory, input_address, true);
                else ansi_input = guest_ansi_string(memory, input_address);
            } catch (const std::runtime_error&) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::vector<wchar_t> wide_output(wide ? capacity : 0);
            std::vector<char> ansi_output(wide ? 0 : capacity);
            wchar_t* wide_file_part = nullptr;
            char* ansi_file_part = nullptr;
            const auto result = wide
                ? ::GetFullPathNameW(wide_input.c_str(), capacity,
                    capacity == 0 ? nullptr : wide_output.data(),
                    file_part_address == 0 || capacity == 0
                        ? nullptr : &wide_file_part)
                : ::GetFullPathNameA(ansi_input.c_str(), capacity,
                    capacity == 0 ? nullptr : ansi_output.data(),
                    file_part_address == 0 || capacity == 0
                        ? nullptr : &ansi_file_part);
            const auto error = GetLastError();
            if (result != 0 && result < capacity) {
                for (std::uint32_t index = 0; index <= result; ++index) {
                    const auto character = wide
                        ? static_cast<std::uint16_t>(wide_output[index])
                        : static_cast<std::uint8_t>(ansi_output[index]);
                    const auto address = output_address +
                        static_cast<std::uint64_t>(index) *
                            (wide ? 2ULL : 1ULL);
                    memory.write8(address,
                                  static_cast<std::uint8_t>(character));
                    if (wide) {
                        memory.write8(address + 1,
                                      static_cast<std::uint8_t>(character >> 8));
                    }
                }
                if (file_part_address != 0) {
                    const auto offset = wide
                        ? (wide_file_part == nullptr ? std::uint64_t{0}
                              : static_cast<std::uint64_t>(
                                    wide_file_part - wide_output.data()) * 2)
                        : (ansi_file_part == nullptr ? std::uint64_t{0}
                              : static_cast<std::uint64_t>(
                                    ansi_file_part - ansi_output.data()));
                    memory.write64(file_part_address,
                        (wide ? wide_file_part == nullptr
                              : ansi_file_part == nullptr)
                            ? 0 : output_address + offset);
                }
            }
            state.x[0] = result;
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "ExpandEnvironmentStringsW" ||
            function == "ExpandEnvironmentStringsA") {
            const bool wide = function.ends_with('W');
            const auto input_address = state.x[0];
            const auto output_address = state.x[1];
            const auto capacity = static_cast<std::uint32_t>(state.x[2]);
            if (state.x[2] != capacity || capacity > 1024U * 1024U) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (!guest_range(memory, input_address, wide ? 2 : 1, false) ||
                (capacity != 0 &&
                 !guest_range(memory, output_address,
                              static_cast<std::uint64_t>(capacity) *
                                  (wide ? 2ULL : 1ULL), true)) ||
                (output_address != 0 && output_address == input_address)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::wstring input;
            try {
                input = wide ? guest_string(memory, input_address, true) :
                    environment_wide(guest_ansi_string(memory, input_address));
            } catch (const std::runtime_error&) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::wstring expanded;
            for (std::size_t index = 0; index < input.size();) {
                if (input[index] == L'%') {
                    const auto end = input.find(L'%', index + 1);
                    if (end != std::wstring::npos && end > index + 1) {
                        const auto value = guest_environment_value(
                            std::wstring_view(input).substr(
                                index + 1, end - index - 1), false);
                        if (value) {
                            expanded += *value;
                            index = end + 1;
                        } else {
                            expanded.append(input, index, end - index + 1);
                            index = end + 1;
                        }
                        if (expanded.size() > 1024U * 1024U) {
                            SetLastError(ERROR_MORE_DATA);
                            state.x[0] = 0;
                            return std::nullopt;
                        }
                        continue;
                    }
                }
                expanded.push_back(input[index++]);
                if (expanded.size() > 1024U * 1024U) {
                    SetLastError(ERROR_MORE_DATA);
                    state.x[0] = 0;
                    return std::nullopt;
                }
            }
            const auto ansi = wide ? std::string{} : environment_ansi(expanded);
            const auto required = static_cast<std::uint64_t>(
                wide ? expanded.size() + 1 : ansi.size() + 1);
            if (required > UINT32_MAX) {
                SetLastError(ERROR_MORE_DATA);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (output_address != 0 && capacity >= required) {
                for (std::size_t index = 0; index < required; ++index) {
                    const auto character = wide
                        ? static_cast<std::uint16_t>(
                              index == expanded.size() ? 0 : expanded[index])
                        : static_cast<std::uint8_t>(
                              index == ansi.size() ? 0 : ansi[index]);
                    const auto address = output_address + index *
                        (wide ? 2ULL : 1ULL);
                    memory.write8(address,
                                  static_cast<std::uint8_t>(character));
                    if (wide) memory.write8(address + 1,
                        static_cast<std::uint8_t>(character >> 8));
                }
            }
            state.x[0] = static_cast<std::uint32_t>(required);
            return std::nullopt;
        }
        if (function == "GetEnvironmentStringsW" ||
            function == "GetEnvironmentStringsA" ||
            function == "FreeEnvironmentStringsW" ||
            function == "FreeEnvironmentStringsA") {
            if (function.starts_with("Free")) {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->environment_allocations.find(state.x[0]);
                if (found == state_->environment_allocations.end()) {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    state.x[0] = 0;
                } else {
                    memory.unmap_memory(found->first);
                    state_->environment_allocations.erase(found);
                    state.x[0] = 1;
                }
                return std::nullopt;
            }
            const bool wide = function.ends_with('W');
            ensure_guest_environment();
            std::vector<std::uint8_t> bytes;
            {
                std::lock_guard lock(state_->mutex);
                for (const auto& [name, value] : state_->guest_environment) {
                    const auto entry = name + L"=" + value;
                    if (wide) {
                        for (const auto character : entry) {
                            bytes.push_back(static_cast<std::uint8_t>(character));
                            bytes.push_back(static_cast<std::uint8_t>(character >> 8));
                        }
                        bytes.push_back(0);
                        bytes.push_back(0);
                    } else {
                        const auto ansi = environment_ansi(entry);
                        for (const auto character : ansi) {
                            bytes.push_back(static_cast<std::uint8_t>(character));
                        }
                        bytes.push_back(0);
                    }
                }
            }
            bytes.push_back(0);
            if (wide) bytes.push_back(0);
            if (bytes.size() == (wide ? 2U : 1U)) {
                bytes.push_back(0);
                if (wide) bytes.push_back(0);
            }
            if (bytes.size() > 2 * 1024 * 1024) {
                throw std::runtime_error("guest environment block is too large");
            }
            const auto size = (bytes.size() + 15U) & ~std::size_t{15};
            std::lock_guard lock(state_->mutex);
            const auto address = state_->next_environment_address;
            memory.map_memory(address, size);
            state_->next_environment_address += size;
            state_->environment_allocations.emplace(address, size);
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                memory.write8(address + index, bytes[index]);
            }
            state.x[0] = address;
            return std::nullopt;
        }
        if (function == "GetEnvironmentVariableW" ||
            function == "GetEnvironmentVariableA") {
            constexpr DWORD maximum_chars = 1024U * 1024U;
            const bool wide = function.ends_with('W');
            const auto capacity = state.x[2];
            if (capacity > maximum_chars || state.x[0] == 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto name = wide ? guest_string(memory, state.x[0], true)
                : environment_wide(guest_ansi_string(memory, state.x[0]));
            const auto found = guest_environment_value(name, false);
            if (!found) {
                SetLastError(ERROR_ENVVAR_NOT_FOUND);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto ansi = wide ? std::string{} : environment_ansi(*found);
            const auto length = wide ? found->size() : ansi.size();
            if (capacity <= length) {
                state.x[0] = length + 1;
                SetLastError(ERROR_SUCCESS);
                return std::nullopt;
            }
            if (!guest_range(memory, state.x[1],
                    (length + 1) * (wide ? 2U : 1U), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            for (std::size_t index = 0; index <= length; ++index) {
                const auto character = index == length ? 0 :
                    wide ? static_cast<std::uint16_t>((*found)[index]) :
                           static_cast<std::uint8_t>(ansi[index]);
                memory.write8(state.x[1] + index * (wide ? 2U : 1U),
                              static_cast<std::uint8_t>(character));
                if (wide) memory.write8(state.x[1] + index * 2 + 1,
                    static_cast<std::uint8_t>(character >> 8));
            }
            state.x[0] = length;
            SetLastError(ERROR_SUCCESS);
            return std::nullopt;
        }
        if (function == "SetEnvironmentVariableW" ||
            function == "SetEnvironmentVariableA") {
            const bool wide = function.ends_with('W');
            if (state.x[0] == 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto name = wide ? guest_string(memory, state.x[0], true)
                : environment_wide(guest_ansi_string(memory, state.x[0]));
            if (name.empty() || name.find(L'=') != std::wstring::npos ||
                name.size() > 32767) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::optional<std::wstring> value;
            if (state.x[1] != 0) {
                value = wide ? guest_string(memory, state.x[1], true)
                    : environment_wide(guest_ansi_string(memory, state.x[1]));
                if (value->size() > 32767) {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    state.x[0] = 0;
                    return std::nullopt;
                }
            }
            update_guest_environment(memory, name, std::move(value), false);
            SetLastError(ERROR_SUCCESS);
            state.x[0] = 1;
            return std::nullopt;
        }
        if (function == "GetComputerNameExW" ||
            function == "GetComputerNameExA" ||
            function == "GetComputerNameW" ||
            function == "GetComputerNameA") {
            constexpr DWORD maximum_chars = 1024U * 1024U;
            const bool extended = function.starts_with("GetComputerNameEx");
            const bool wide = function.ends_with('W');
            const auto guest_buffer = state.x[extended ? 1 : 0];
            const auto guest_size = state.x[extended ? 2 : 1];
            if (!guest_range(memory, guest_size, sizeof(DWORD), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            DWORD capacity = memory.read32(guest_size);
            if (capacity > maximum_chars ||
                (guest_buffer != 0 && capacity != 0 &&
                 !guest_range(memory, guest_buffer,
                              static_cast<std::uint64_t>(capacity) *
                                  (wide ? sizeof(wchar_t) : sizeof(char)), true))) {
                SetLastError(capacity > maximum_chars ? ERROR_INVALID_PARAMETER
                                                   : ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            BOOL success = FALSE;
            if (wide) {
                std::vector<wchar_t> output(capacity);
                auto* buffer = guest_buffer == 0 || capacity == 0
                    ? nullptr : output.data();
                success = extended
                    ? ::GetComputerNameExW(
                          static_cast<COMPUTER_NAME_FORMAT>(state.x[0]),
                          buffer, &capacity)
                    : ::GetComputerNameW(buffer, &capacity);
                if (success && buffer != nullptr) {
                    for (DWORD index = 0; index <= capacity; ++index) {
                        const auto character =
                            static_cast<std::uint16_t>(output[index]);
                        memory.write8(guest_buffer + index * 2ULL,
                                      static_cast<std::uint8_t>(character));
                        memory.write8(guest_buffer + index * 2ULL + 1,
                                      static_cast<std::uint8_t>(character >> 8));
                    }
                }
            } else {
                std::vector<char> output(capacity);
                auto* buffer = guest_buffer == 0 || capacity == 0
                    ? nullptr : output.data();
                success = extended
                    ? ::GetComputerNameExA(
                          static_cast<COMPUTER_NAME_FORMAT>(state.x[0]),
                          buffer, &capacity)
                    : ::GetComputerNameA(buffer, &capacity);
                if (success && buffer != nullptr) {
                    for (DWORD index = 0; index <= capacity; ++index) {
                        memory.write8(guest_buffer + index,
                                      static_cast<std::uint8_t>(output[index]));
                    }
                }
            }
            const auto error = GetLastError();
            memory.write32(guest_size, capacity);
            state.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetLocaleInfoA" ||
            function == "GetLocaleInfoW" ||
            function == "GetLocaleInfoEx" ||
            function == "LCIDToLocaleName") {
            constexpr std::uint64_t maximum_chars = 1024 * 1024;
            const bool ansi = function == "GetLocaleInfoA";
            const bool locale_name = function == "GetLocaleInfoEx";
            const bool lcid_name = function == "LCIDToLocaleName";
            const auto output = state.x[lcid_name ? 1 : 2];
            const auto capacity_arg = state.x[lcid_name ? 2 : 3];
            if (capacity_arg > maximum_chars ||
                (capacity_arg != 0 && output != 0 &&
                 !guest_range(memory, output,
                              capacity_arg * (ansi ? 1ULL : 2ULL), true))) {
                SetLastError(capacity_arg > maximum_chars
                    ? ERROR_INVALID_PARAMETER : ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto capacity = static_cast<int>(capacity_arg);
            int result = 0;
            if (ansi) {
                std::vector<char> data(capacity);
                result = ::GetLocaleInfoA(static_cast<LCID>(state.x[0]),
                    static_cast<LCTYPE>(state.x[1]),
                    capacity == 0 || output == 0 ? nullptr : data.data(),
                    capacity);
                const auto error = GetLastError();
                if (result > 0 && output != 0 && capacity != 0) {
                    for (int index = 0; index < result; ++index)
                        memory.write8(output + index,
                                      static_cast<std::uint8_t>(data[index]));
                }
                state.x[0] = static_cast<std::uint32_t>(result);
                SetLastError(error);
                return std::nullopt;
            }
            std::vector<wchar_t> data(capacity);
            if (locale_name) {
                const auto name = state.x[0] == 0 ? std::wstring{} :
                    guest_string(memory, state.x[0], true);
                result = ::GetLocaleInfoEx(state.x[0] == 0
                    ? nullptr : name.c_str(), static_cast<LCTYPE>(state.x[1]),
                    capacity == 0 || output == 0 ? nullptr : data.data(),
                    capacity);
            } else if (lcid_name) {
                result = ::LCIDToLocaleName(static_cast<LCID>(state.x[0]),
                    capacity == 0 || output == 0 ? nullptr : data.data(),
                    capacity, static_cast<DWORD>(state.x[3]));
            } else {
                result = ::GetLocaleInfoW(static_cast<LCID>(state.x[0]),
                    static_cast<LCTYPE>(state.x[1]),
                    capacity == 0 || output == 0 ? nullptr : data.data(),
                    capacity);
            }
            const auto error = GetLastError();
            if (result > 0 && output != 0 && capacity != 0) {
                for (int index = 0; index < result; ++index) {
                    const auto character =
                        static_cast<std::uint16_t>(data[index]);
                    memory.write8(output + index * 2ULL,
                                  static_cast<std::uint8_t>(character));
                    memory.write8(output + index * 2ULL + 1,
                                  static_cast<std::uint8_t>(character >> 8));
                }
            }
            state.x[0] = static_cast<std::uint32_t>(result);
            SetLastError(error);
            return std::nullopt;
        }
        if (function == "GetTempPathA" || function == "GetTempPathW" ||
            function == "GetSystemDirectoryA" ||
            function == "GetSystemDirectoryW" ||
            function == "GetWindowsDirectoryA" ||
            function == "GetWindowsDirectoryW" ||
            function == "GetCurrentDirectoryA" ||
            function == "GetCurrentDirectoryW") {
            constexpr UINT maximum_chars = 1024U * 1024U;
            const bool buffer_first =
                function.starts_with("GetSystemDirectory") ||
                function.starts_with("GetWindowsDirectory");
            const auto guest_buffer = state.x[buffer_first ? 0 : 1];
            const auto capacity_arg = state.x[buffer_first ? 1 : 0];
            if (capacity_arg > maximum_chars) {
                throw std::runtime_error(
                    "guest path buffer is too large in " + function +
                    " (x0=" + std::to_string(state.x[0]) +
                    ", x1=" + std::to_string(state.x[1]) + ")");
            }
            const auto capacity = static_cast<UINT>(capacity_arg);
            const bool wide = function.ends_with('W');
            if (wide) {
                std::vector<wchar_t> output(capacity);
                auto* buffer = capacity == 0 ? nullptr : output.data();
                const auto result = function == "GetTempPathW"
                    ? GetTempPathW(capacity, buffer)
                    : function == "GetSystemDirectoryW"
                          ? GetSystemDirectoryW(buffer, capacity)
                    : function == "GetWindowsDirectoryW"
                          ? GetWindowsDirectoryW(buffer, capacity)
                          : GetCurrentDirectoryW(capacity, buffer);
                const auto error = GetLastError();
                if (result != 0 && result < capacity) {
                    for (UINT index = 0; index <= result; ++index) {
                        memory.write8(guest_buffer + index * 2ULL,
                                      static_cast<std::uint8_t>(output[index]));
                        memory.write8(guest_buffer + index * 2ULL + 1,
                                      static_cast<std::uint8_t>(output[index] >> 8));
                    }
                }
                state.x[0] = result;
                SetLastError(error);
            } else {
                std::vector<char> output(capacity);
                auto* buffer = capacity == 0 ? nullptr : output.data();
                const auto result = function == "GetTempPathA"
                    ? GetTempPathA(capacity, buffer)
                    : function == "GetSystemDirectoryA"
                          ? GetSystemDirectoryA(buffer, capacity)
                    : function == "GetWindowsDirectoryA"
                          ? GetWindowsDirectoryA(buffer, capacity)
                          : GetCurrentDirectoryA(capacity, buffer);
                const auto error = GetLastError();
                if (result != 0 && result < capacity) {
                    for (UINT index = 0; index <= result; ++index) {
                        memory.write8(guest_buffer + index,
                                      static_cast<std::uint8_t>(output[index]));
                    }
                }
                state.x[0] = result;
                SetLastError(error);
            }
            return std::nullopt;
        }
    }
    if (dll == "api-ms-win-crt-string-l1-1-0.dll" &&
        (function == "_strdup" || function == "_wcsdup")) {
        if (state.x[0] == 0) return std::nullopt;
        const bool wide = function == "_wcsdup";
        const auto source_address = state.x[0];
        const auto value = wide
            ? guest_string(memory, source_address, true)
            : guest_string(memory, source_address, false);
        state.x[0] = (value.size() + 1) * (wide ? 2 : 1);
        static_cast<void>(dispatch(
            {"api-ms-win-crt-heap-l1-1-0.dll", "malloc", 0}, state, memory));
        if (state.x[0] != 0) {
            const auto copy = state.x[0];
            for (std::size_t index = 0; index < value.size(); ++index) {
                const auto character = static_cast<std::uint16_t>(value[index]);
                memory.write8(copy + index * (wide ? 2 : 1),
                              static_cast<std::uint8_t>(character));
                if (wide) {
                    memory.write8(copy + index * 2 + 1,
                                  static_cast<std::uint8_t>(character >> 8));
                }
            }
            memory.write8(copy + value.size() * (wide ? 2 : 1), 0);
            if (wide) memory.write8(copy + value.size() * 2 + 1, 0);
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-string-l1-1-0.dll" &&
        (function == "strtok" || function == "strtok_s" ||
         function == "wcstok" || function == "wcstok_s")) {
        const bool wide = function.starts_with("wcs");
        const bool explicit_context = function != "strtok";
        const auto context = state.x[2];
        if (state.x[1] == 0 || (explicit_context && context == 0)) {
            auto& slot = state_->thread_errno();
            slot.value = EINVAL;
            if (slot.address != 0) memory.write32(slot.address, EINVAL);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto separators = guest_string(memory, state.x[1], wide);
        std::uint64_t cursor = state.x[0];
        if (cursor == 0) {
            if (explicit_context) cursor = memory.read64(context);
            else {
                std::lock_guard lock(state_->mutex);
                cursor = state_->strtok_cursors[GetCurrentThreadId()];
            }
        }
        const auto save_cursor = [&](std::uint64_t next) {
            if (explicit_context) memory.write64(context, next);
            else {
                std::lock_guard lock(state_->mutex);
                state_->strtok_cursors[GetCurrentThreadId()] = next;
            }
        };
        const auto unit = wide ? 2ULL : 1ULL;
        const auto read_character = [&memory, unit](std::uint64_t address)
            -> std::uint16_t {
            if (address > UINT64_MAX - (unit - 1)) {
                throw std::runtime_error("guest tokenizer address overflow");
            }
            return static_cast<std::uint16_t>(memory.read8(address) |
                (unit == 2 ? static_cast<std::uint16_t>(
                    memory.read8(address + 1)) << 8 : 0));
        };
        const auto is_separator = [&separators](std::uint16_t character) {
            return separators.find(static_cast<wchar_t>(character)) !=
                   std::wstring::npos;
        };
        constexpr std::uint64_t maximum_units = 1024ULL * 1024ULL * 1024ULL;
        if (cursor == 0) {
            state.x[0] = 0;
            return std::nullopt;
        }
        std::uint64_t index = 0;
        while (index < maximum_units && is_separator(read_character(cursor))) {
            if (cursor > UINT64_MAX - unit) {
                throw std::runtime_error("guest tokenizer address overflow");
            }
            cursor += unit;
            ++index;
        }
        if (index == maximum_units) {
            throw std::runtime_error("guest tokenizer string is too large");
        }
        if (read_character(cursor) == 0) {
            save_cursor(0);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto token = cursor;
        while (index < maximum_units) {
            const auto character = read_character(cursor);
            if (character == 0 || is_separator(character)) {
                if (character != 0) {
                    memory.write8(cursor, 0);
                    if (wide) memory.write8(cursor + 1, 0);
                    save_cursor(cursor + unit);
                } else save_cursor(0);
                state.x[0] = token;
                return std::nullopt;
            }
            if (cursor > UINT64_MAX - unit) {
                throw std::runtime_error("guest tokenizer address overflow");
            }
            cursor += unit;
            ++index;
        }
        throw std::runtime_error("guest tokenizer string is too large");
    }
    if (dll == "api-ms-win-crt-string-l1-1-0.dll" &&
        (function == "_stricmp" || function == "_strnicmp" ||
         function == "_wcsicmp" || function == "_wcsnicmp" ||
         function == "_stricmp_l" || function == "_strnicmp_l" ||
         function == "_wcsicmp_l" || function == "_wcsnicmp_l")) {
        const bool wide = function.starts_with("_wcs");
        const bool counted = function.find("nicmp") != std::string::npos;
        const bool explicit_locale = function.ends_with("_l");
        const auto count = counted ? state.x[2] : 0;
        if (counted && count == 0) {
            state.x[0] = 0;
            return std::nullopt;
        }
        if (state.x[0] == 0 || state.x[1] == 0 || count > 1024 * 1024) {
            throw std::runtime_error("invalid guest case-insensitive comparison");
        }
        const auto narrow_input = [&](std::uint64_t address) {
            if (!counted) return guest_ansi_string(memory, address);
            std::string value;
            value.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 4096)));
            for (std::uint64_t index = 0; index < count; ++index) {
                if (index > UINT64_MAX - address) {
                    throw std::runtime_error("guest comparison address overflow");
                }
                const auto character = memory.read8(address + index);
                if (character == 0) break;
                value.push_back(static_cast<char>(character));
            }
            return value;
        };
        const auto wide_input = [&](std::uint64_t address) {
            if (!counted) return guest_string(memory, address, true);
            std::wstring value;
            value.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(count, 4096)));
            for (std::uint64_t index = 0; index < count; ++index) {
                if (index > (UINT64_MAX - address - 1) / 2) {
                    throw std::runtime_error("guest comparison address overflow");
                }
                const auto position = address + index * 2;
                const auto character = static_cast<wchar_t>(
                    memory.read8(position) | (memory.read8(position + 1) << 8));
                if (character == 0) break;
                value.push_back(character);
            }
            return value;
        };
        const auto left_a = wide ? std::string{} : narrow_input(state.x[0]);
        const auto right_a = wide ? std::string{} : narrow_input(state.x[1]);
        const auto left_w = wide ? wide_input(state.x[0]) : std::wstring{};
        const auto right_w = wide ? wide_input(state.x[1]) : std::wstring{};
        std::scoped_lock lock(state_->mutex, state_->locale_mutex);
        _locale_t locale = nullptr;
        if (explicit_locale) {
            const auto guest_locale = state.x[counted ? 3 : 2];
            if (guest_locale != 0) {
                const auto found = state_->locale_objects.find(guest_locale);
                if (found == state_->locale_objects.end()) {
                    throw std::runtime_error("invalid guest comparison locale");
                }
                locale = found->second.host;
            }
        }
        const auto result = wide
            ? explicit_locale
                ? counted ? _wcsnicmp_l(left_w.c_str(), right_w.c_str(),
                            static_cast<std::size_t>(count), locale)
                          : _wcsicmp_l(left_w.c_str(), right_w.c_str(), locale)
                : counted ? _wcsnicmp(left_w.c_str(), right_w.c_str(),
                            static_cast<std::size_t>(count))
                          : _wcsicmp(left_w.c_str(), right_w.c_str())
            : explicit_locale
                ? counted ? _strnicmp_l(left_a.c_str(), right_a.c_str(),
                            static_cast<std::size_t>(count), locale)
                          : _stricmp_l(left_a.c_str(), right_a.c_str(), locale)
                : counted ? _strnicmp(left_a.c_str(), right_a.c_str(),
                            static_cast<std::size_t>(count))
                          : _stricmp(left_a.c_str(), right_a.c_str());
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-string-l1-1-0.dll" &&
        (function == "wcscoll" || function == "wcsxfrm")) {
        constexpr std::uint64_t maximum_transform_units = 1024U * 1024U;
        const auto set_guest_errno = [this, &memory](int value) {
            std::lock_guard lock(state_->mutex);
            auto& slot = state_->thread_errno();
            slot.value = value;
            if (slot.address != 0) {
                memory.write32(slot.address, static_cast<std::uint32_t>(value));
            }
        };
        const bool transform = function == "wcsxfrm";
        const auto count = transform ? state.x[2] : 0;
        if (state.x[1] == 0 || (!transform && state.x[0] == 0) ||
            (transform && (count > INT_MAX ||
             (state.x[0] == 0 && count != 0) ||
             (state.x[0] != 0 &&
              (count > maximum_transform_units ||
               !guest_range(memory, state.x[0], count * 2ULL, true)))))) {
            set_guest_errno(EINVAL);
            state.x[0] = static_cast<std::uint32_t>(INT_MAX);
            return std::nullopt;
        }
        std::wstring source;
        std::wstring other;
        try {
            source = guest_string(memory, state.x[1], true);
            if (!transform) other = guest_string(memory, state.x[0], true);
        } catch (const std::runtime_error&) {
            set_guest_errno(EINVAL);
            state.x[0] = static_cast<std::uint32_t>(INT_MAX);
            return std::nullopt;
        }
        int guest_errno = 0;
        {
            std::lock_guard lock(state_->mutex);
            const auto& slot = state_->thread_errno();
            guest_errno = slot.address == 0 ? slot.value :
                static_cast<int>(memory.read32(slot.address));
        }
        const auto saved_host_errno = errno;
        std::vector<wchar_t> output(transform && state.x[0] != 0
            ? static_cast<std::size_t>(count) : 0);
        wchar_t empty_output{};
        std::uint64_t result = 0;
        int result_errno = 0;
        {
            std::lock_guard lock(state_->locale_mutex);
            errno = guest_errno;
            result = transform
                ? ::wcsxfrm(state.x[0] == 0 || count == 0
                                ? &empty_output : output.data(),
                            source.c_str(), static_cast<std::size_t>(count))
                : static_cast<std::uint32_t>(
                    ::wcscoll(other.c_str(), source.c_str()));
            result_errno = errno;
            errno = saved_host_errno;
        }
        set_guest_errno(result_errno);
        if (transform && state.x[0] != 0) {
            const auto written = std::min<std::size_t>(
                output.size(), static_cast<std::size_t>(result) + 1);
            for (std::size_t index = 0; index < written; ++index) {
                const auto character = static_cast<std::uint16_t>(output[index]);
                memory.write8(state.x[0] + index * 2,
                    static_cast<std::uint8_t>(character));
                memory.write8(state.x[0] + index * 2 + 1,
                    static_cast<std::uint8_t>(character >> 8));
            }
        }
        state.x[0] = result;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-string-l1-1-0.dll" &&
        dispatch_crt_string(function, state, memory)) {
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-math-l1-1-0.dll" &&
        dispatch_crt_math(function, state, memory)) {
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        (function == "_lock_locales" || function == "_unlock_locales")) {
        if (function == "_lock_locales") state_->locale_mutex.lock();
        else state_->locale_mutex.unlock();
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "_configthreadlocale") {
        const auto requested = static_cast<std::int32_t>(state.x[0]);
        std::lock_guard lock(state_->locale_mutex);
        const auto thread = GetCurrentThreadId();
        const auto found = state_->thread_locale_modes.find(thread);
        const auto previous = found == state_->thread_locale_modes.end()
                                  ? _DISABLE_PER_THREAD_LOCALE : found->second;
        if (requested == 0) {
            state.x[0] = static_cast<std::uint32_t>(previous);
            return std::nullopt;
        }
        if (requested != _ENABLE_PER_THREAD_LOCALE &&
            requested != _DISABLE_PER_THREAD_LOCALE) {
            auto& slot = state_->thread_errno();
            slot.value = EINVAL;
            if (slot.address != 0) {
                memory.write32(slot.address, EINVAL);
            }
            state.x[0] = UINT32_MAX;
            return std::nullopt;
        }
        const auto host_previous = _configthreadlocale(requested);
        if (host_previous == -1) {
            auto& slot = state_->thread_errno();
            slot.value = errno;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(slot.value));
            }
            state.x[0] = UINT32_MAX;
            return std::nullopt;
        }
        state_->original_host_locale_modes.try_emplace(thread, host_previous);
        state_->thread_locale_modes[thread] = requested;
        state.x[0] = static_cast<std::uint32_t>(previous);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "__initialize_lconv_for_unsigned_char") {
        std::lock_guard lock(state_->locale_mutex);
        state_->unsigned_char_lconv = true;
        if (const auto page = state_->lconv_pages.find(GetCurrentThreadId());
            page != state_->lconv_pages.end()) {
            mirror_lconv(memory, page->second, *std::localeconv(), true);
        }
        state.x[0] = 0;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "localeconv") {
        std::lock_guard lock(state_->locale_mutex);
        const auto thread = GetCurrentThreadId();
        auto page = state_->lconv_pages.find(thread);
        if (page == state_->lconv_pages.end()) {
            const auto address = state_->next_lconv_address;
            if (address > UINT64_MAX - 4096) {
                throw std::runtime_error("guest lconv address space exhausted");
            }
            memory.map_memory(address, 4096);
            state_->next_lconv_address += 4096;
            page = state_->lconv_pages.emplace(thread, address).first;
        }
        mirror_lconv(memory, page->second, *std::localeconv(),
                     state_->unsigned_char_lconv);
        state.x[0] = page->second;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" && function == "setlocale") {
        std::string requested;
        const char* requested_pointer = nullptr;
        if (state.x[1] != 0) {
            const auto wide = guest_string(memory, state.x[1], false);
            for (const auto character : wide) {
                requested.push_back(static_cast<char>(character));
            }
            requested_pointer = requested.c_str();
        }
        std::lock_guard lock(state_->locale_mutex);
        const auto* result = std::setlocale(static_cast<int>(state.x[0]),
                                            requested_pointer);
        if (result == nullptr) {
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto size = std::strlen(result);
        if (size >= 4096) throw std::runtime_error("guest locale name is too long");
        for (std::size_t index = 0; index <= size; ++index) {
            memory.write8(locale_base + index,
                          static_cast<std::uint8_t>(result[index]));
        }
        mirror_ctype_table(memory);
        mirror_locale_names(memory);
        if (const auto page = state_->lconv_pages.find(GetCurrentThreadId());
            page != state_->lconv_pages.end()) {
            mirror_lconv(memory, page->second, *std::localeconv(),
                         state_->unsigned_char_lconv);
        }
        state.x[0] = locale_base;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "__pctype_func") {
        mirror_ctype_table(memory);
        state.x[0] = ctype_base + 127 * 2;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "___lc_locale_name_func") {
        mirror_locale_names(memory);
        state.x[0] = locale_names_base;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "___lc_codepage_func") {
        state.x[0] = ___lc_codepage_func();
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "___lc_collate_cp_func") {
        state.x[0] = ___lc_collate_cp_func();
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        function == "___mb_cur_max_func") {
        state.x[0] = static_cast<std::uint32_t>(___mb_cur_max_func());
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-locale-l1-1-0.dll" &&
        (function == "_create_locale" || function == "_get_current_locale" ||
         function == "_free_locale")) {
        if (function == "_free_locale") {
            if (state.x[0] == 0) return std::nullopt;
            std::lock_guard lock(state_->mutex);
            const auto found = state_->locale_objects.find(state.x[0]);
            if (found == state_->locale_objects.end()) {
                throw std::runtime_error("invalid guest CRT locale pointer");
            }
            _free_locale(found->second.host);
            memory.unmap_memory(state.x[0]);
            state_->locale_objects.erase(found);
            return std::nullopt;
        }
        _locale_t host = nullptr;
        if (function == "_get_current_locale") {
            host = _get_current_locale();
        } else {
            const auto wide = guest_string(memory, state.x[1], false);
            std::string name;
            for (const auto character : wide) {
                name.push_back(static_cast<char>(character));
            }
            host = _create_locale(static_cast<int>(state.x[0]), name.c_str());
        }
        if (!host) {
            state.x[0] = 0;
            return std::nullopt;
        }
        std::lock_guard lock(state_->mutex);
        const auto address = state_->next_locale_address;
        if (address > UINT64_MAX - 4096) {
            _free_locale(host);
            state.x[0] = 0;
            return std::nullopt;
        }
        try {
            memory.map_memory(address, 4096);
        } catch (const std::bad_alloc&) {
            _free_locale(host);
            state.x[0] = 0;
            return std::nullopt;
        }
        state_->next_locale_address += 4096;
        state_->locale_objects.emplace(address, State::LocaleObject{host});
        const auto* prefix = reinterpret_cast<const __crt_locale_data_public*>(
            host->locinfo);
        const auto table = address + 0x200 + 127 * 2;
        memory.write64(address, address + 0x20); // locinfo
        memory.write64(address + 8, address + 0x100); // mbcinfo
        memory.write64(address + 0x20, table); // _locale_pctype
        memory.write32(address + 0x28,
                       static_cast<std::uint32_t>(prefix->_locale_mb_cur_max));
        memory.write32(address + 0x2c, prefix->_locale_lc_codepage);
        mirror_ctype_table(memory, address + 0x200, prefix->_locale_pctype);
        state.x[0] = address;
        return std::nullopt;
    }
    if ((dll == "api-ms-win-crt-heap-l1-1-0.dll" ||
         dll == "api-ms-win-crt-runtime-l1-1-0.dll") &&
        (function == "_set_new_mode" || function == "_query_new_mode" ||
         function == "_set_new_handler" ||
         function == "_query_new_handler" || function == "_callnewh")) {
        if (function == "_set_new_mode") {
            if (state.x[0] > 1) {
                std::lock_guard lock(state_->mutex);
                auto& slot = state_->thread_errno();
                slot.value = EINVAL;
                if (slot.address != 0) {
                    memory.write32(slot.address, EINVAL);
                }
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
            std::lock_guard lock(state_->mutex);
            const auto previous = state_->crt_new_mode;
            state_->crt_new_mode = static_cast<std::int32_t>(state.x[0]);
            state.x[0] = static_cast<std::uint32_t>(previous);
            return std::nullopt;
        }
        if (function == "_query_new_mode") {
            std::lock_guard lock(state_->mutex);
            state.x[0] = static_cast<std::uint32_t>(state_->crt_new_mode);
            return std::nullopt;
        }
        if (function == "_set_new_handler") {
            std::lock_guard lock(state_->mutex);
            const auto previous = state_->crt_new_handler;
            state_->crt_new_handler = state.x[0];
            state.x[0] = previous;
            return std::nullopt;
        }
        if (function == "_query_new_handler") {
            std::lock_guard lock(state_->mutex);
            state.x[0] = state_->crt_new_handler;
            return std::nullopt;
        }
        std::uint64_t handler{};
        std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
        {
            std::lock_guard lock(state_->mutex);
            handler = state_->crt_new_handler;
            invoke = state_->guest_callback;
        }
        if (handler == 0) {
            state.x[0] = 0;
            return std::nullopt;
        }
        if (!invoke) {
            throw std::runtime_error("guest new-handler invoker is unavailable");
        }
        state.x[0] = static_cast<std::uint32_t>(invoke(handler, state.x[0])) != 0;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-heap-l1-1-0.dll" &&
        (function == "malloc" || function == "calloc" ||
         function == "realloc" || function == "free" || function == "_msize" ||
         function == "_aligned_malloc" || function == "_aligned_free" ||
         function == "_aligned_realloc" || function == "_aligned_msize")) {
        const auto allocate = [this, &memory](std::uint64_t requested,
                                               std::uint64_t alignment,
                                               bool aligned) {
            if (requested > 1024ULL * 1024ULL * 1024ULL) return std::uint64_t{0};
            if (alignment == 0 || alignment > 1024ULL * 1024ULL * 1024ULL ||
                (alignment & (alignment - 1)) != 0) return std::uint64_t{0};
            const auto size = static_cast<std::size_t>((std::max(requested, 1ULL) + 15) &
                                                       ~15ULL);
            const auto mapped_size = (size + 4095) & ~std::size_t{4095};
            const auto mapping_alignment = std::max(alignment, 4096ULL);
            std::lock_guard lock(state_->mutex);
            if (state_->next_heap_address > UINT64_MAX - mapping_alignment + 1) {
                return std::uint64_t{0};
            }
            const auto address = (state_->next_heap_address + mapping_alignment - 1) &
                                 ~(mapping_alignment - 1);
            if (address > UINT64_MAX - mapped_size) return std::uint64_t{0};
            try {
                memory.map_memory(address, mapped_size);
            } catch (const std::bad_alloc&) {
                return std::uint64_t{0};
            }
            state_->next_heap_address = address + mapped_size;
            state_->heap_allocations.emplace(address,
                                               State::HeapAllocation{size, aligned});
            return address;
        };
        const auto allocate_with_handler = [&](std::uint64_t requested,
                                               std::uint64_t alignment,
                                               bool aligned) {
            if (alignment == 0 || alignment > 1024ULL * 1024ULL * 1024ULL ||
                (alignment & (alignment - 1)) != 0) {
                return std::uint64_t{0};
            }
            for (;;) {
                const auto address = allocate(requested, alignment, aligned);
                if (address != 0) return address;
                bool call_handler{};
                {
                    std::lock_guard lock(state_->mutex);
                    call_handler = state_->crt_new_mode != 0;
                }
                if (!call_handler) return std::uint64_t{0};
                CpuState callback_state{};
                callback_state.x[0] = requested;
                static_cast<void>(dispatch(
                    {"api-ms-win-crt-heap-l1-1-0.dll", "_callnewh", 0},
                    callback_state, memory));
                if (callback_state.x[0] == 0) return std::uint64_t{0};
            }
        };
        if (function == "malloc" || function == "calloc" ||
            function == "_aligned_malloc") {
            const auto requested = function != "calloc" ? state.x[0] :
                (state.x[1] != 0 && state.x[0] > UINT64_MAX / state.x[1]
                     ? UINT64_MAX : state.x[0] * state.x[1]);
            const bool aligned = function == "_aligned_malloc";
            state.x[0] = allocate_with_handler(
                requested, aligned ? state.x[1] : 16, aligned);
            return std::nullopt;
        }
        const auto address = state.x[0];
        const bool aligned = function == "_aligned_free" ||
                             function == "_aligned_realloc" ||
                             function == "_aligned_msize";
        if (address == 0 && (function == "free" || function == "_aligned_free")) {
            return std::nullopt;
        }
        if (address == 0 && (function == "realloc" ||
                             function == "_aligned_realloc")) {
            state.x[0] = allocate_with_handler(
                state.x[1], aligned ? state.x[2] : 16, aligned);
            return std::nullopt;
        }
        std::size_t old_size = 0;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->heap_allocations.find(address);
            if (found == state_->heap_allocations.end()) {
                throw std::runtime_error("invalid guest CRT heap pointer");
            }
            old_size = found->second.size;
            if (found->second.aligned != aligned) {
                throw std::runtime_error("mismatched guest CRT heap API");
            }
        }
        if (function == "_msize" || function == "_aligned_msize") {
            state.x[0] = old_size;
            return std::nullopt;
        }
        if ((function == "realloc" || function == "_aligned_realloc") &&
            state.x[1] != 0) {
            const auto resized = allocate_with_handler(
                state.x[1], aligned ? state.x[2] : 16, aligned);
            if (resized == 0) {
                state.x[0] = 0;
                return std::nullopt;
            }
            copy_guest(memory, resized, address,
                       std::min(old_size, static_cast<std::size_t>(state.x[1])));
            state.x[0] = resized;
        } else if (function == "realloc" || function == "_aligned_realloc") {
            state.x[0] = 0;
        }
        memory.unmap_memory(address);
        {
            std::lock_guard lock(state_->mutex);
            state_->heap_allocations.erase(address);
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-stdio-l1-1-0.dll" &&
        function == "__acrt_iob_func") {
        const auto index = static_cast<std::uint32_t>(state.x[0]);
        if (index >= 3 || state.x[0] != index) {
            throw std::runtime_error("invalid guest standard stream index");
        }
        state.x[0] = stdio_base + index * 32;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-stdio-l1-1-0.dll" &&
        function == "_get_stream_buffer_pointers") {
        const auto stream = state.x[0];
        const bool standard = stream >= stdio_base &&
            stream < stdio_base + 3 * 32 &&
            (stream - stdio_base) % 32 == 0;
        if (!standard) {
            std::lock_guard lock(state_->mutex);
            if (!state_->streams.contains(stream)) {
                throw std::runtime_error("invalid guest CRT stream pointer");
            }
        }
        if (state.x[1] != 0) memory.write64(state.x[1], stream);
        if (state.x[2] != 0) memory.write64(state.x[2], stream + 8);
        if (state.x[3] != 0) memory.write64(state.x[3], stream + 16);
        state.x[0] = 0;
        return std::nullopt;
    }
    if ((dll == "api-ms-win-crt-stdio-l1-1-0.dll" ||
         dll == "api-ms-win-crt-runtime-l1-1-0.dll") &&
        (function == "_set_fmode" || function == "_get_fmode" ||
         function == "__p__fmode" || function == "__p__commode")) {
        if (function == "__p__fmode" || function == "__p__commode") {
            const bool commit = function == "__p__commode";
            auto& address = commit ? state_->crt_commode_address
                                   : state_->crt_fmode_address;
            if (address == 0) {
                address = state_->next_environment_address;
                memory.map_memory(address, 16);
                state_->next_environment_address += 16;
                memory.write32(address, static_cast<std::uint32_t>(
                    commit ? state_->crt_commode : state_->crt_fmode));
            }
            state.x[0] = address;
            return std::nullopt;
        }
        const auto set_errno = [this, &memory](int error) {
            auto& slot = state_->thread_errno();
            slot.value = error;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(error));
            }
        };
        if (function == "_set_fmode") {
            const auto mode = static_cast<std::int32_t>(state.x[0]);
            if (mode != _O_TEXT && mode != _O_BINARY && mode != _O_WTEXT) {
                set_errno(EINVAL);
                state.x[0] = EINVAL;
            } else {
                state_->crt_fmode = mode;
                if (state_->crt_fmode_address != 0) {
                    memory.write32(state_->crt_fmode_address,
                                   static_cast<std::uint32_t>(mode));
                }
                state.x[0] = 0;
            }
            return std::nullopt;
        }
        if (state.x[0] == 0) {
            set_errno(EINVAL);
            state.x[0] = EINVAL;
        } else {
            const auto mode = state_->crt_fmode_address == 0
                ? state_->crt_fmode
                : static_cast<std::int32_t>(
                    memory.read32(state_->crt_fmode_address));
            memory.write32(state.x[0], static_cast<std::uint32_t>(mode));
            state.x[0] = 0;
        }
        return std::nullopt;
    }
    if ((dll == "api-ms-win-crt-filesystem-l1-1-0.dll" &&
         (function == "_chdir" || function == "_wchdir" ||
          function == "_unlink" || function == "_wunlink" ||
          function == "remove" || function == "_wremove")) ||
        (dll == "api-ms-win-crt-environment-l1-1-0.dll" &&
         (function == "_getcwd" || function == "_wgetcwd"))) {
        const auto set_guest_errno = [this, &memory](int error) {
            std::lock_guard lock(state_->mutex);
            auto& slot = state_->thread_errno();
            slot.value = error;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(error));
            }
        };
        const bool wide = function == "_wgetcwd" ||
                          function == "_wchdir" ||
                          function == "_wunlink" ||
                          function == "_wremove";
        if (function == "_getcwd" || function == "_wgetcwd") {
            const auto guest_buffer = state.x[0];
            const auto maxlen = static_cast<int>(
                static_cast<std::int32_t>(state.x[1]));
            if (maxlen <= 0 || maxlen > 32768) {
                set_guest_errno(EINVAL);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::vector<char> narrow(static_cast<std::size_t>(wide ? 0 : maxlen));
            std::vector<wchar_t> unicode(static_cast<std::size_t>(wide ? maxlen : 0));
            const auto saved_errno = errno;
            errno = 0;
            const bool success = wide
                ? ::_wgetcwd(unicode.data(), maxlen) != nullptr
                : ::_getcwd(narrow.data(), maxlen) != nullptr;
            const auto result_errno = errno;
            errno = saved_errno;
            if (!success) {
                set_guest_errno(result_errno);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto length = wide ? std::wcslen(unicode.data())
                                     : std::strlen(narrow.data());
            const auto bytes = (length + 1) * (wide ? 2ULL : 1ULL);
            if (guest_buffer != 0 &&
                !guest_range(memory, guest_buffer, bytes, true)) {
                set_guest_errno(EINVAL);
                state.x[0] = 0;
                return std::nullopt;
            }
            auto output = guest_buffer;
            if (output == 0) {
                CpuState allocation{};
                allocation.x[0] = static_cast<std::uint64_t>(maxlen) *
                    (wide ? 2ULL : 1ULL);
                static_cast<void>(dispatch(
                    {"api-ms-win-crt-heap-l1-1-0.dll", "malloc", 0},
                    allocation, memory));
                output = allocation.x[0];
                if (output == 0) {
                    set_guest_errno(ENOMEM);
                    state.x[0] = 0;
                    return std::nullopt;
                }
            }
            for (std::size_t index = 0; index <= length; ++index) {
                const auto character = wide
                    ? static_cast<std::uint16_t>(unicode[index])
                    : static_cast<std::uint8_t>(narrow[index]);
                memory.write8(output + index * (wide ? 2ULL : 1ULL),
                              static_cast<std::uint8_t>(character));
                if (wide) {
                    memory.write8(output + index * 2ULL + 1,
                                  static_cast<std::uint8_t>(character >> 8));
                }
            }
            state.x[0] = output;
            return std::nullopt;
        }
        if (state.x[0] == 0 ||
            !guest_range(memory, state.x[0], wide ? 2 : 1, false)) {
            set_guest_errno(EINVAL);
            state.x[0] = UINT32_MAX;
            return std::nullopt;
        }
        const auto saved_errno = errno;
        errno = 0;
        int result = -1;
        if (wide) {
            const auto path = guest_string(memory, state.x[0], true);
            result = function == "_wchdir" ? ::_wchdir(path.c_str())
                : function == "_wremove" ? ::_wremove(path.c_str())
                : ::_wunlink(path.c_str());
        } else {
            const auto path = guest_ansi_string(memory, state.x[0]);
            result = function == "_chdir" ? ::_chdir(path.c_str())
                : function == "remove" ? ::remove(path.c_str())
                : ::_unlink(path.c_str());
        }
        const auto result_errno = errno;
        errno = saved_errno;
        if (result != 0) set_guest_errno(result_errno);
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-filesystem-l1-1-0.dll" &&
        (function == "_access" || function == "_waccess" ||
         function == "_access_s" || function == "_waccess_s")) {
        const bool wide = function.starts_with("_w");
        const bool secure = function.ends_with("_s");
        const auto path = state.x[0];
        const auto mode = static_cast<int>(static_cast<std::uint32_t>(state.x[1]));
        const auto region = path == 0
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(path);
        const auto set_guest_errno = [this, &memory](int error) {
            auto& slot = state_->thread_errno();
            slot.value = error;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(error));
            }
        };
        if (!region || !region->readable ||
            path > UINT64_MAX - (wide ? 2ULL : 1ULL) ||
            path + (wide ? 2ULL : 1ULL) > region->base + region->size ||
            (mode & ~6) != 0) {
            set_guest_errno(EINVAL);
            state.x[0] = secure ? EINVAL : UINT32_MAX;
            return std::nullopt;
        }
        int result = 0;
        if (wide) {
            const auto native_path = guest_string(memory, path, true);
            result = secure ? _waccess_s(native_path.c_str(), mode)
                            : _waccess(native_path.c_str(), mode);
        } else {
            const auto native_path = guest_ansi_string(memory, path);
            result = secure ? _access_s(native_path.c_str(), mode)
                            : _access(native_path.c_str(), mode);
        }
        if (result != 0) set_guest_errno(secure ? result : errno);
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-filesystem-l1-1-0.dll" &&
        (function == "_stat64" || function == "_wstat64" ||
         function == "_fstat64" || function == "_stat64i32" ||
         function == "_wstat64i32" || function == "_fstat64i32" ||
         function == "_stat32" || function == "_wstat32" ||
         function == "_fstat32" || function == "_stat32i64" ||
         function == "_wstat32i64" || function == "_fstat32i64")) {
        const bool descriptor = function.starts_with("_fstat");
        const bool wide = function.starts_with("_wstat");
        const auto output = state.x[1];
        const auto set_guest_errno = [this, &memory](int error) {
            auto& slot = state_->thread_errno();
            slot.value = error;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(error));
            }
        };
        if (output == 0 || (!descriptor && state.x[0] == 0)) {
            set_guest_errno(EINVAL);
            state.x[0] = UINT32_MAX;
            return std::nullopt;
        }
        const auto fd = static_cast<int>(static_cast<std::uint32_t>(state.x[0]));
        if (descriptor) {
            std::lock_guard lock(state_->mutex);
            if (!((fd >= 0 && fd <= 2) || state_->file_descriptors.contains(fd))) {
                set_guest_errno(EBADF);
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
        }
        const auto narrow_path = !descriptor && !wide
            ? guest_ansi_string(memory, state.x[0]) : std::string{};
        const auto wide_path = !descriptor && wide
            ? guest_string(memory, state.x[0], true) : std::wstring{};
        const auto run = [&](auto& value) {
            using Stat = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Stat, struct _stat64>) {
                if (descriptor) return _fstat64(fd, &value);
                return wide ? _wstat64(wide_path.c_str(), &value)
                            : _stat64(narrow_path.c_str(), &value);
            } else if constexpr (std::is_same_v<Stat, struct _stat64i32>) {
                if (descriptor) return _fstat64i32(fd, &value);
                return wide ? _wstat64i32(wide_path.c_str(), &value)
                            : _stat64i32(narrow_path.c_str(), &value);
            } else if constexpr (std::is_same_v<Stat, struct _stat32i64>) {
                if (descriptor) return _fstat32i64(fd, &value);
                return wide ? _wstat32i64(wide_path.c_str(), &value)
                            : _stat32i64(narrow_path.c_str(), &value);
            } else {
                if (descriptor) return _fstat32(fd, &value);
                return wide ? _wstat32(wide_path.c_str(), &value)
                            : _stat32(narrow_path.c_str(), &value);
            }
        };
        const auto finish = [&](auto& value) {
            const auto result = run(value);
            if (result == 0) write_guest_stat(memory, output, value);
            else set_guest_errno(errno);
            state.x[0] = static_cast<std::uint32_t>(result);
        };
        if (function.ends_with("64i32")) {
            struct _stat64i32 value{};
            finish(value);
        } else if (function.ends_with("32i64")) {
            struct _stat32i64 value{};
            finish(value);
        } else if (function.ends_with("64")) {
            struct _stat64 value{};
            finish(value);
        } else {
            struct _stat32 value{};
            finish(value);
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-stdio-l1-1-0.dll" ||
        (dll == "api-ms-win-crt-math-l1-1-0.dll" &&
         (function == "_fdopen" || function == "_wfdopen")) ||
        (dll == "api-ms-win-crt-filesystem-l1-1-0.dll" &&
         (function == "_lock_file" || function == "_unlock_file"))) {
        const auto descriptor = static_cast<int>(static_cast<std::uint32_t>(state.x[0]));
        const auto known_descriptor = [this](int fd) {
            std::lock_guard lock(state_->mutex);
            return (fd >= 0 && fd <= 2) || state_->file_descriptors.contains(fd);
        };
        if (function == "_mktemp_s" || function == "_wmktemp_s" ||
            function == "_mktemp" || function == "_wmktemp") {
            const bool wide = function.starts_with("_w");
            const bool secure = function.ends_with("_s");
            const auto address = state.x[0];
            const auto width = wide ? 2ULL : 1ULL;
            const auto region = address == 0
                ? std::optional<GuestMemory::RegionInfo>{}
                : memory.query_region(address);
            const auto set_guest_errno = [this, &memory](int error) {
                auto& slot = state_->thread_errno();
                slot.value = error;
                if (slot.address != 0) {
                    memory.write32(slot.address,
                                   static_cast<std::uint32_t>(error));
                }
            };
            const auto fail = [&](int error, bool clear) {
                if (clear) {
                    memory.write8(address, 0);
                    if (wide) memory.write8(address + 1, 0);
                }
                set_guest_errno(error);
                state.x[0] = secure ? static_cast<std::uint32_t>(error) : 0;
                return std::nullopt;
            };
            if (!region || !region->readable || !region->writable ||
                address > UINT64_MAX - width ||
                address + width > region->base + region->size) {
                return fail(EINVAL, false);
            }
            constexpr std::uint64_t maximum_template_chars = 32768;
            const auto available = (region->base + region->size - address) / width;
            const auto capacity = secure ? state.x[1] :
                std::min(available, maximum_template_chars);
            if (capacity == 0 || capacity > available ||
                capacity > maximum_template_chars) {
                return fail(EINVAL, true);
            }
            std::size_t length = 0;
            for (; length < capacity; ++length) {
                const auto character = wide
                    ? static_cast<std::uint16_t>(
                        memory.read8(address + length * 2) |
                        (memory.read8(address + length * 2 + 1) << 8))
                    : memory.read8(address + length);
                if (character == 0) break;
            }
            if (length == capacity || length < 6) return fail(EINVAL, true);
            for (std::size_t index = length - 6; index < length; ++index) {
                const auto character = wide
                    ? static_cast<std::uint16_t>(
                        memory.read8(address + index * 2) |
                        (memory.read8(address + index * 2 + 1) << 8))
                    : memory.read8(address + index);
                if (character != 'X') return fail(EINVAL, true);
            }
            if (wide) {
                std::vector<wchar_t> native(static_cast<std::size_t>(
                    secure ? capacity : length + 1), 0);
                for (std::size_t index = 0; index < length; ++index) {
                    native[index] = static_cast<wchar_t>(
                        memory.read8(address + index * 2) |
                        (memory.read8(address + index * 2 + 1) << 8));
                }
                const auto result = _wmktemp_s(native.data(), native.size());
                if (result != 0) return fail(result, true);
                for (std::size_t index = 0; index <= length; ++index) {
                    const auto character = static_cast<std::uint16_t>(native[index]);
                    memory.write8(address + index * 2,
                                  static_cast<std::uint8_t>(character));
                    memory.write8(address + index * 2 + 1,
                                  static_cast<std::uint8_t>(character >> 8));
                }
            } else {
                std::vector<char> native(static_cast<std::size_t>(
                    secure ? capacity : length + 1), 0);
                for (std::size_t index = 0; index < length; ++index) {
                    native[index] = static_cast<char>(memory.read8(address + index));
                }
                const auto result = _mktemp_s(native.data(), native.size());
                if (result != 0) return fail(result, true);
                for (std::size_t index = 0; index <= length; ++index) {
                    memory.write8(address + index,
                                  static_cast<std::uint8_t>(native[index]));
                }
            }
            state.x[0] = secure ? 0 : address;
            return std::nullopt;
        }
        if (function == "_sopen_dispatch" || function == "_wsopen_dispatch" ||
            function == "_sopen_s" || function == "_wsopen_s" ||
            function == "_sopen" || function == "_wsopen") {
            const bool wide = function.starts_with("_w");
            const bool dispatch_form = function.ends_with("_dispatch");
            const bool secure_form = function.ends_with("_s");
            const auto path_address = secure_form ? state.x[1] : state.x[0];
            const auto output = dispatch_form ? state.x[4] :
                                secure_form ? state.x[0] : 0;
            const auto set_guest_errno = [this, &memory](int error) {
                auto& slot = state_->thread_errno();
                slot.value = error;
                if (slot.address != 0) {
                    memory.write32(slot.address,
                                   static_cast<std::uint32_t>(error));
                }
            };
            const auto path_region = path_address == 0
                ? std::optional<GuestMemory::RegionInfo>{}
                : memory.query_region(path_address);
            const auto out_region = output == 0
                ? std::optional<GuestMemory::RegionInfo>{}
                : memory.query_region(output);
            const auto path_width = wide ? 2ULL : 1ULL;
            if (!path_region || !path_region->readable ||
                path_address > UINT64_MAX - path_width ||
                path_address + path_width > path_region->base + path_region->size ||
                ((dispatch_form || secure_form) &&
                 (!out_region || !out_region->writable ||
                  output > UINT64_MAX - 4 ||
                  output + 4 > out_region->base + out_region->size))) {
                // A null/invalid output cannot receive the required -1 value.
                if (out_region && out_region->writable &&
                    output <= UINT64_MAX - 4 &&
                    output + 4 <= out_region->base + out_region->size) {
                    memory.write32(output, UINT32_MAX);
                }
                set_guest_errno(EINVAL);
                state.x[0] = dispatch_form || secure_form ? EINVAL : UINT32_MAX;
                return std::nullopt;
            }
            auto flags = static_cast<int>(static_cast<std::uint32_t>(
                state.x[secure_form ? 2 : 1]));
            const auto share = static_cast<int>(static_cast<std::uint32_t>(
                state.x[secure_form ? 3 : 2]));
            const auto mode = static_cast<int>(static_cast<std::uint32_t>(
                state.x[secure_form ? 4 : 3]));
            if ((flags & (_O_TEXT | _O_BINARY | _O_WTEXT |
                          _O_U16TEXT | _O_U8TEXT)) == 0) {
                flags |= state_->crt_fmode_address == 0
                    ? state_->crt_fmode
                    : static_cast<int>(memory.read32(state_->crt_fmode_address));
            }
            int fd = -1;
            int result = 0;
            const auto secure = secure_form ? 1 :
                dispatch_form ? static_cast<int>(state.x[5]) : 0;
            const ScopedHostCrtParameterGuard parameter_guard;
            if (wide) {
                const auto path = guest_string(memory, path_address, true);
                result = _wsopen_dispatch(path.c_str(), flags, share,
                                          mode, &fd, secure);
            } else {
                const auto path = guest_ansi_string(memory, path_address);
                result = _sopen_dispatch(path.c_str(), flags, share,
                                         mode, &fd, secure);
            }
            if (result == 0 && fd >= 0) {
                std::lock_guard lock(state_->mutex);
                state_->file_descriptors.insert(fd);
            } else {
                fd = -1;
                if (result == 0) result = errno != 0 ? errno : EINVAL;
                set_guest_errno(result);
            }
            if (output != 0) memory.write32(output,
                static_cast<std::uint32_t>(fd));
            state.x[0] = dispatch_form || secure_form
                ? static_cast<std::uint32_t>(result)
                : static_cast<std::uint32_t>(fd);
            return std::nullopt;
        }
        if (function == "_open" || function == "_wopen") {
            auto flags = static_cast<int>(state.x[1]);
            constexpr int supported_flags = _O_WRONLY | _O_RDWR | _O_APPEND |
                _O_CREAT | _O_TRUNC | _O_EXCL | _O_TEXT | _O_BINARY |
                _O_WTEXT | _O_U16TEXT | _O_U8TEXT | _O_NOINHERIT |
                _O_TEMPORARY | _O_SHORT_LIVED | _O_SEQUENTIAL | _O_RANDOM;
            if (state.x[1] > INT_MAX ||
                (flags & ~supported_flags) != 0 ||
                (flags & (_O_WRONLY | _O_RDWR)) ==
                    (_O_WRONLY | _O_RDWR)) {
                auto& slot = state_->thread_errno();
                slot.value = EINVAL;
                if (slot.address != 0) memory.write32(slot.address, EINVAL);
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
            if ((flags & (_O_TEXT | _O_BINARY | _O_WTEXT |
                          _O_U16TEXT | _O_U8TEXT)) == 0) {
                flags |= state_->crt_fmode_address == 0
                    ? state_->crt_fmode
                    : static_cast<int>(memory.read32(state_->crt_fmode_address));
            }
            // The open mode is ignored unless the file is created. Python and
            // other callers may still pass POSIX group/other permission bits;
            // the host secure CRT rejects those even for a non-creating open.
            const auto mode = (flags & _O_CREAT) == 0
                ? (_S_IREAD | _S_IWRITE)
                : static_cast<int>(state.x[2]) & (_S_IREAD | _S_IWRITE);
            int fd = -1;
            int result = 0;
            const ScopedHostCrtParameterGuard parameter_guard;
            if (function == "_wopen") {
                const auto path = guest_string(memory, state.x[0], true);
                result = _wsopen_s(&fd, path.c_str(), flags, _SH_DENYNO, mode);
            } else {
                const auto path = guest_ansi_string(memory, state.x[0]);
                result = _sopen_s(&fd, path.c_str(), flags, _SH_DENYNO, mode);
            }
            if (fd >= 0) {
                std::lock_guard lock(state_->mutex);
                state_->file_descriptors.insert(fd);
            } else {
                auto& slot = state_->thread_errno();
                slot.value = result != 0 ? result : errno;
                if (slot.address != 0) {
                    memory.write32(slot.address,
                                   static_cast<std::uint32_t>(slot.value));
                }
            }
            state.x[0] = static_cast<std::uint32_t>(fd);
            return std::nullopt;
        }
        if (function == "_get_osfhandle") {
            if (!known_descriptor(descriptor)) {
                state.x[0] = UINT64_MAX;
                return std::nullopt;
            }
            const auto native = _get_osfhandle(descriptor);
            if (native == -1 || native == -2) {
                state.x[0] = static_cast<std::uint64_t>(native);
                return std::nullopt;
            }
            std::lock_guard lock(state_->mutex);
            const auto cached = state_->descriptor_handles.find(descriptor);
            if (cached != state_->descriptor_handles.end()) {
                state.x[0] = cached->second;
                return std::nullopt;
            }
            const auto guest = state_->next_host_handle;
            state_->next_host_handle += 8;
            state_->host_handles.emplace(guest, State::HostHandle{
                reinterpret_cast<HANDLE>(native), false});
            state_->descriptor_handles.emplace(descriptor, guest);
            state.x[0] = guest;
            return std::nullopt;
        }
        if (function == "_open_osfhandle") {
            const auto guest = state.x[0];
            const auto flags = static_cast<int>(state.x[1]);
            HANDLE native = nullptr;
            bool owned = false;
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->host_handles.find(guest);
                if (found == state_->host_handles.end()) {
                    state.x[0] = UINT32_MAX;
                    return std::nullopt;
                }
                native = found->second.value;
                owned = found->second.owned;
            }
            HANDLE transferred = native;
            if (!owned && !DuplicateHandle(GetCurrentProcess(), native,
                                           GetCurrentProcess(), &transferred,
                                           0, FALSE, DUPLICATE_SAME_ACCESS)) {
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
            const auto fd = _open_osfhandle(
                reinterpret_cast<std::intptr_t>(transferred), flags);
            if (fd < 0) {
                if (!owned) CloseHandle(transferred);
            } else {
                std::lock_guard lock(state_->mutex);
                state_->host_handles.erase(guest);
                for (auto it = state_->descriptor_handles.begin();
                     it != state_->descriptor_handles.end();) {
                    it = it->second == guest
                        ? state_->descriptor_handles.erase(it) : std::next(it);
                }
                state_->file_descriptors.insert(fd);
            }
            state.x[0] = static_cast<std::uint32_t>(fd);
            return std::nullopt;
        }
        if (function == "_close" || function == "_dup" ||
            function == "_dup2" || function == "_isatty" ||
            function == "_setmode" || function == "_lseeki64" ||
            function == "_lseek" || function == "_commit" ||
            function == "_chsize_s" || function == "_locking") {
            if (!known_descriptor(descriptor)) {
                state.x[0] = function == "_lseeki64" ? UINT64_MAX : UINT32_MAX;
                return std::nullopt;
            }
            if (function == "_close") {
                const auto result = _close(descriptor);
                if (result == 0) {
                    std::lock_guard lock(state_->mutex);
                    state_->file_descriptors.erase(descriptor);
                    const auto handle = state_->descriptor_handles.find(descriptor);
                    if (handle != state_->descriptor_handles.end()) {
                        state_->host_handles.erase(handle->second);
                        state_->descriptor_handles.erase(handle);
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(result);
            } else if (function == "_dup") {
                const auto result = _dup(descriptor);
                if (result >= 0) {
                    std::lock_guard lock(state_->mutex);
                    state_->file_descriptors.insert(result);
                }
                state.x[0] = static_cast<std::uint32_t>(result);
            } else if (function == "_dup2") {
                const auto target = static_cast<int>(state.x[1]);
                const auto result = _dup2(descriptor, target);
                if (result == 0) {
                    std::lock_guard lock(state_->mutex);
                    state_->file_descriptors.insert(target);
                    const auto handle = state_->descriptor_handles.find(target);
                    if (handle != state_->descriptor_handles.end()) {
                        state_->host_handles.erase(handle->second);
                        state_->descriptor_handles.erase(handle);
                    }
                }
                state.x[0] = static_cast<std::uint32_t>(result);
            } else if (function == "_isatty") {
                state.x[0] = static_cast<std::uint32_t>(_isatty(descriptor));
            } else if (function == "_setmode") {
                state.x[0] = static_cast<std::uint32_t>(_setmode(
                    descriptor, static_cast<int>(state.x[1])));
            } else if (function == "_lseek") {
                state.x[0] = static_cast<std::uint32_t>(_lseek(
                    descriptor, static_cast<long>(state.x[1]),
                    static_cast<int>(state.x[2])));
            } else if (function == "_commit") {
                state.x[0] = static_cast<std::uint32_t>(_commit(descriptor));
            } else if (function == "_chsize_s") {
                state.x[0] = static_cast<std::uint32_t>(_chsize_s(
                    descriptor, static_cast<__int64>(state.x[1])));
            } else if (function == "_locking") {
                state.x[0] = static_cast<std::uint32_t>(_locking(
                    descriptor, static_cast<int>(state.x[1]),
                    static_cast<long>(state.x[2])));
            } else {
                state.x[0] = static_cast<std::uint64_t>(_lseeki64(
                    descriptor, static_cast<__int64>(state.x[1]),
                    static_cast<int>(state.x[2])));
            }
            return std::nullopt;
        }
        if (function == "_getmaxstdio") {
            state.x[0] = static_cast<std::uint32_t>(_getmaxstdio());
            return std::nullopt;
        }
        if (function == "_read" || function == "_write") {
            if (!known_descriptor(descriptor) || state.x[2] > UINT32_MAX) {
                state.x[0] = UINT32_MAX;
                return std::nullopt;
            }
            const auto address = state.x[1];
            const auto count = static_cast<std::uint32_t>(state.x[2]);
            std::array<std::uint8_t, 64 * 1024> bytes{};
            std::uint32_t transferred = 0;
            while (transferred < count) {
                const auto chunk = static_cast<unsigned>(std::min<std::uint32_t>(
                    static_cast<std::uint32_t>(bytes.size()), count - transferred));
                if (function == "_write") {
                    copy_from_guest(memory, address + transferred,
                                    std::span(bytes).first(chunk));
                }
                const auto result = function == "_read"
                    ? _read(descriptor, bytes.data(), chunk)
                    : _write(descriptor, bytes.data(), chunk);
                if (result < 0) {
                    state.x[0] = transferred == 0 ? UINT32_MAX : transferred;
                    return std::nullopt;
                }
                if (function == "_read") {
                    copy_to_guest(memory, address + transferred,
                                  std::span(bytes).first(static_cast<std::size_t>(result)));
                }
                transferred += static_cast<std::uint32_t>(result);
                if (static_cast<unsigned>(result) < chunk) break;
            }
            state.x[0] = transferred;
            return std::nullopt;
        }
        const auto host_stream = [this](std::uint64_t address) -> FILE* {
            std::lock_guard lock(state_->mutex);
            return state_->stream_record(address).host;
        };
        if (function == "_lock_file" || function == "_unlock_file") {
            const auto guest = state.x[0];
            FILE* stream = nullptr;
            const auto thread = GetCurrentThreadId();
            if (function == "_lock_file") {
                {
                    std::lock_guard lock(state_->mutex);
                    stream = host_stream(guest);
                    auto& record = state_->stream_locks[guest];
                    if (record.owner == thread && record.depth != 0) {
                        ++record.depth;
                        return std::nullopt;
                    }
                    ++record.waiters;
                }
                _lock_file(stream);
                {
                    std::lock_guard lock(state_->mutex);
                    auto& record = state_->stream_locks.at(guest);
                    --record.waiters;
                    record.owner = thread;
                    record.depth = 1;
                }
                return std::nullopt;
            }
            bool release = false;
            {
                std::lock_guard lock(state_->mutex);
                stream = host_stream(guest);
                const auto found = state_->stream_locks.find(guest);
                if (found == state_->stream_locks.end() ||
                    found->second.owner != thread ||
                    found->second.depth == 0) {
                    throw std::runtime_error(
                        "guest CRT stream unlock is not owned by this thread");
                }
                auto& record = found->second;
                state_->sync_stream_buffer(memory, guest, state_->stream_record(guest));
                release = --record.depth == 0;
                if (release) {
                    record.owner = 0;
                    if (record.waiters == 0) state_->stream_locks.erase(found);
                }
            }
            if (release) _unlock_file(stream);
            return std::nullopt;
        }
        if (function == "_fdopen" || function == "_wfdopen" ||
            function == "fopen" || function == "_wfopen" ||
            function == "_fsopen" || function == "_wfsopen" ||
            function == "fopen_s" || function == "_wfopen_s" ||
            function == "freopen" || function == "_wfreopen" ||
            function == "freopen_s" || function == "_wfreopen_s") {
            const bool adopt = function == "_fdopen" || function == "_wfdopen";
            const bool wide = function == "_wfdopen" || function == "_wfopen" ||
                function == "_wfsopen" || function == "_wfopen_s" ||
                function == "_wfreopen" || function == "_wfreopen_s";
            const bool shared = function == "_fsopen" ||
                                function == "_wfsopen";
            const bool secure = function.ends_with("_s");
            const bool reopen = function == "freopen" ||
                function == "_wfreopen" || function == "freopen_s" ||
                function == "_wfreopen_s";
            const auto output = secure ? state.x[0] : 0;
            const auto path_address = secure ? state.x[1] : state.x[0];
            const auto mode_address = secure ? state.x[2] : state.x[1];
            const auto guest_stream = reopen
                ? (secure ? state.x[3] : state.x[2]) : 0;
            const auto set_guest_errno = [this, &memory](int error) {
                auto& slot = state_->thread_errno();
                slot.value = error;
                if (slot.address != 0) {
                    memory.write32(slot.address,
                                   static_cast<std::uint32_t>(error));
                }
            };
            if (secure) {
                const auto region = output == 0
                    ? std::optional<GuestMemory::RegionInfo>{}
                    : memory.query_region(output);
                if (!region || !region->writable ||
                    output > UINT64_MAX - 8 ||
                    output + 8 > region->base + region->size) {
                    set_guest_errno(EINVAL);
                    state.x[0] = EINVAL;
                    return std::nullopt;
                }
                memory.write64(output, 0);
            }
            if (adopt && !known_descriptor(descriptor)) {
                set_guest_errno(EBADF);
                state.x[0] = 0;
                return std::nullopt;
            }
            if ((!adopt && path_address == 0) || mode_address == 0 ||
                (shared && state.x[2] != _SH_DENYNO &&
                 state.x[2] != _SH_DENYRD &&
                 state.x[2] != _SH_DENYWR &&
                 state.x[2] != _SH_DENYRW) ||
                (reopen && (guest_stream == stdio_base ||
                            guest_stream == stdio_base + 32 ||
                            guest_stream == stdio_base + 64))) {
                set_guest_errno(EINVAL);
                state.x[0] = secure ? EINVAL : 0;
                return std::nullopt;
            }
            const auto with_guest_defaults = [this, &memory](auto mode) {
                using Character = typename decltype(mode)::value_type;
                const auto has_modifier = [&mode](char modifier) {
                    const auto comma = mode.find(static_cast<Character>(','));
                    const auto end = comma == mode.npos ? mode.size() : comma;
                    return mode.find(static_cast<Character>(modifier)) < end;
                };
                const auto add_modifier = [&mode](char modifier) {
                    const auto comma = mode.find(static_cast<Character>(','));
                    mode.insert(comma == mode.npos ? mode.size() : comma, 1,
                                static_cast<Character>(modifier));
                };
                const auto translation = state_->crt_fmode_address == 0
                    ? state_->crt_fmode
                    : static_cast<std::int32_t>(
                        memory.read32(state_->crt_fmode_address));
                if (!has_modifier('b') && !has_modifier('t')) {
                    if (translation == _O_BINARY) add_modifier('b');
                    else if (translation == _O_TEXT) add_modifier('t');
                    else if (translation == _O_WTEXT &&
                             mode.find(static_cast<Character>(',')) == mode.npos) {
                        for (const char* suffix = ", ccs=UNICODE"; *suffix;
                             ++suffix) {
                            mode.push_back(static_cast<Character>(*suffix));
                        }
                    }
                }
                const auto commit = state_->crt_commode_address == 0
                    ? state_->crt_commode
                    : static_cast<std::int32_t>(
                        memory.read32(state_->crt_commode_address));
                if (commit != 0 && !has_modifier('c') && !has_modifier('n')) {
                    add_modifier('c');
                }
                return mode;
            };
            FILE* old_stream = nullptr;
            int old_fd = -1;
            std::unique_ptr<State::StreamLifecycle> lifecycle;
            if (reopen) {
                lifecycle = std::make_unique<State::StreamLifecycle>(*state_, memory, guest_stream);
                old_stream = lifecycle->host;
                old_fd = _fileno(old_stream);
            }
            FILE* opened = nullptr;
            int host_error = 0;
            bool attempted_reopen = false;
            if (adopt) {
                // fdopen shares the descriptor and its existing translation
                // mode. fclose owns the eventual close, just as in the CRT.
                const ScopedHostCrtParameterGuard parameter_guard;
                if (wide) {
                    const auto mode = guest_string(memory, mode_address, true);
                    opened = adopt_descriptor_wide(descriptor, mode.c_str());
                } else {
                    const auto mode = guest_ansi_string(memory, mode_address);
                    opened = adopt_descriptor(descriptor, mode.c_str());
                }
            } else if (wide) {
                const auto path = guest_string(memory, path_address, true);
                const auto mode = with_guest_defaults(
                    guest_string(memory, mode_address, true));
                if (path.empty() || mode.empty()) host_error = EINVAL;
                else if (shared) {
                    opened = _wfsopen(path.c_str(), mode.c_str(),
                                      static_cast<int>(state.x[2]));
                } else if (reopen) {
                    attempted_reopen = true;
                    if (secure) {
                        host_error = _wfreopen_s(&opened, path.c_str(),
                                                mode.c_str(), old_stream);
                    } else opened = reopen_plain_wide(path.c_str(),
                                                       mode.c_str(), old_stream);
                } else if (secure) {
                    host_error = _wfopen_s(&opened, path.c_str(), mode.c_str());
                } else opened = open_plain_wide(path.c_str(), mode.c_str());
            } else {
                const auto path = guest_ansi_string(memory, path_address);
                const auto mode = with_guest_defaults(
                    guest_ansi_string(memory, mode_address));
                if (path.empty() || mode.empty()) host_error = EINVAL;
                else if (shared) {
                    opened = _fsopen(path.c_str(), mode.c_str(),
                                     static_cast<int>(state.x[2]));
                } else if (reopen) {
                    attempted_reopen = true;
                    if (secure) {
                        host_error = freopen_s(&opened, path.c_str(),
                                               mode.c_str(), old_stream);
                    } else opened = reopen_plain(path.c_str(), mode.c_str(),
                                                  old_stream);
                } else if (secure) {
                    host_error = fopen_s(&opened, path.c_str(), mode.c_str());
                } else opened = open_plain(path.c_str(), mode.c_str());
            }
            if (host_error == 0 && opened == nullptr) {
                host_error = errno == 0 ? EIO : errno;
            }
            if (attempted_reopen) {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->streams.find(guest_stream);
                if (found == state_->streams.end()) {
                    throw std::runtime_error("invalid guest CRT stream pointer");
                }
                state_->reset_stream_buffer(memory, guest_stream, found->second, true);
                state_->stream_locks.erase(guest_stream);
                state_->file_descriptors.erase(old_fd);
                if (const auto handle = state_->descriptor_handles.find(old_fd);
                    handle != state_->descriptor_handles.end()) {
                    state_->host_handles.erase(handle->second);
                    state_->descriptor_handles.erase(handle);
                }
                if (opened != nullptr) found->second.host = opened;
                else {
                    state_->streams.erase(found);
                    memory.unmap_memory(guest_stream);
                }
            }
            if (opened == nullptr) {
                set_guest_errno(host_error);
                state.x[0] = secure ? static_cast<std::uint32_t>(host_error) : 0;
                return std::nullopt;
            }
            if (reopen) {
                if (secure) memory.write64(output, guest_stream);
                state.x[0] = secure ? 0 : guest_stream;
                return std::nullopt;
            }
            std::lock_guard lock(state_->mutex);
            const auto address = state_->next_stream_address;
            if (address > UINT64_MAX - 4096) {
                std::fclose(opened);
                set_guest_errno(ENOMEM);
                state.x[0] = secure ? ENOMEM : 0;
                return std::nullopt;
            }
            try {
                memory.map_memory(address, 4096);
            } catch (const std::bad_alloc&) {
                std::fclose(opened);
                set_guest_errno(ENOMEM);
                state.x[0] = secure ? ENOMEM : 0;
                return std::nullopt;
            }
            state_->next_stream_address += 4096;
            state_->streams.emplace(address, State::Stream{opened});
            if (secure) memory.write64(output, address);
            state.x[0] = secure ? 0 : address;
            return std::nullopt;
        }
        if (function == "fclose") {
            const auto address = state.x[0];
            const State::StreamLifecycle lifecycle(*state_, memory, address);
            FILE* stream = nullptr;
            {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->streams.find(address);
                if (found == state_->streams.end()) {
                    throw std::runtime_error("invalid guest CRT stream pointer");
                }
                state_->stream_locks.erase(address);
                state_->reset_stream_buffer(memory, address, found->second, true);
                stream = found->second.host;
                const auto fd = _fileno(stream);
                state_->file_descriptors.erase(fd);
                const auto handle = state_->descriptor_handles.find(fd);
                if (handle != state_->descriptor_handles.end()) {
                    state_->host_handles.erase(handle->second);
                    state_->descriptor_handles.erase(handle);
                }
                state_->streams.erase(found);
            }
            memory.unmap_memory(address);
            state.x[0] = static_cast<std::uint32_t>(std::fclose(stream));
            return std::nullopt;
        }
        if (function == "fread" || function == "fwrite") {
            const auto buffer_address = state.x[0];
            const auto element_size = state.x[1];
            const auto element_count = state.x[2];
            if (element_size != 0 &&
                (element_count > UINT64_MAX / element_size ||
                 element_count * element_size > 1024ULL * 1024ULL * 1024ULL)) {
                throw std::runtime_error("guest CRT file transfer is too large");
            }
            const auto byte_count = element_size * element_count;
            std::array<std::uint8_t, 64 * 1024> bytes{};
            State::StreamAccess operation(*state_, memory, state.x[3]);
            auto* stream = operation.host;
            std::uint64_t transferred = 0;
            while (transferred < byte_count) {
                const auto chunk = static_cast<std::size_t>(
                    std::min<std::uint64_t>(bytes.size(),
                                            byte_count - transferred));
                if (function == "fwrite") {
                    copy_from_guest(memory, buffer_address + transferred,
                                    std::span(bytes).first(chunk));
                }
                const auto completed = function == "fwrite"
                    ? std::fwrite(bytes.data(), 1, chunk, stream)
                    : std::fread(bytes.data(), 1, chunk, stream);
                if (function == "fread") {
                    copy_to_guest(memory, buffer_address + transferred,
                                  std::span(bytes).first(completed));
                }
                transferred += completed;
                if (completed != chunk) break;
            }
            if (byte_count != 0) {
                if (function == "fread") operation.publish(true);
                else operation.reset();
            }
            state.x[0] = element_size == 0 ? 0 : transferred / element_size;
            return std::nullopt;
        }
        if (function == "fseek" || function == "_fseeki64") {
            State::StreamAccess operation(*state_, memory, state.x[0]);
            auto* stream = operation.host;
            const auto origin = static_cast<int>(state.x[2]);
            const auto result = function == "_fseeki64"
                ? _fseeki64(stream, static_cast<__int64>(state.x[1]), origin)
                : std::fseek(stream, static_cast<long>(state.x[1]), origin);
            operation.reset();
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        if (function == "ftell" || function == "_ftelli64") {
            State::StreamAccess operation(*state_, memory, state.x[0]);
            auto* stream = operation.host;
            state.x[0] = function == "_ftelli64"
                ? static_cast<std::uint64_t>(_ftelli64(stream))
                : static_cast<std::uint32_t>(std::ftell(stream));
            return std::nullopt;
        }
        if (function == "fflush") {
            if (state.x[0] == 0) state.x[0] = static_cast<std::uint32_t>(std::fflush(nullptr));
            else {
                State::StreamAccess operation(*state_, memory, state.x[0]);
                state.x[0] = static_cast<std::uint32_t>(std::fflush(operation.host));
                operation.reset();
            }
            return std::nullopt;
        }
        if (function == "feof" || function == "ferror" ||
            function == "clearerr") {
            State::StreamAccess operation(*state_, memory, state.x[0]);
            auto* stream = operation.host;
            if (function == "clearerr") {
                std::clearerr(stream);
            } else {
                state.x[0] = function == "feof" ? std::feof(stream)
                                                 : std::ferror(stream);
            }
            return std::nullopt;
        }
        if (function == "fgetc" || function == "getc" ||
            function == "fputc" || function == "putc" ||
            function == "ungetc") {
            const bool read = function == "fgetc" || function == "getc";
            State::StreamAccess operation(*state_, memory, state.x[read ? 0 : 1]);
            auto* stream = operation.host;
            const auto result = read ? std::fgetc(stream) :
                function == "ungetc"
                    ? std::ungetc(static_cast<int>(state.x[0]), stream)
                    : std::fputc(static_cast<int>(state.x[0]), stream);
            if (read || function == "ungetc") operation.publish(function == "ungetc");
            else operation.reset();
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        if (function == "fputs") {
            const auto value = guest_ansi_string(memory, state.x[0]);
            State::StreamAccess operation(*state_, memory, state.x[1]);
            state.x[0] = static_cast<std::uint32_t>(
                std::fputs(value.c_str(), operation.host));
            operation.reset();
            return std::nullopt;
        }
        if (function == "puts") {
            const auto value = guest_ansi_string(memory, state.x[0]);
            State::StreamAccess operation(*state_, memory, stdio_base + 32);
            const auto result = std::puts(value.c_str());
            operation.reset();
            if (result == EOF) {
                auto& slot = state_->thread_errno();
                slot.value = errno;
                if (slot.address != 0) {
                    memory.write32(slot.address,
                                   static_cast<std::uint32_t>(slot.value));
                }
            }
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        if (function == "fgets") {
            const auto capacity = static_cast<int>(state.x[1]);
            if (capacity <= 0 || capacity > 1024 * 1024) {
                throw std::runtime_error("invalid guest fgets buffer size");
            }
            std::vector<char> output(static_cast<std::size_t>(capacity));
            State::StreamAccess operation(*state_, memory, state.x[2]);
            const auto* result = std::fgets(output.data(), capacity,
                                            operation.host);
            operation.publish(true);
            const auto destination = state.x[0];
            if (result == nullptr) {
                state.x[0] = 0;
            } else {
                copy_to_guest(memory, destination,
                              {reinterpret_cast<const std::uint8_t*>(output.data()),
                               std::strlen(output.data()) + 1});
            }
            return std::nullopt;
        }
        if (function == "fgetpos" || function == "fsetpos") {
            static_assert(sizeof(fpos_t) == 8);
            State::StreamAccess operation(*state_, memory, state.x[0]);
            auto* stream = operation.host;
            fpos_t position{};
            if (function == "fsetpos") {
                position = guest_structure<fpos_t>(memory, state.x[1]);
            }
            const auto result = function == "fgetpos"
                ? std::fgetpos(stream, &position)
                : std::fsetpos(stream, &position);
            if (function == "fsetpos") operation.reset();
            if (result == 0 && function == "fgetpos") {
                write_guest_structure(memory, state.x[1], position);
            }
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        if (function == "setvbuf") {
            if (state.x[3] > 1024ULL * 1024ULL) {
                throw std::runtime_error("guest stream buffer is too large");
            }
            State::StreamAccess operation(*state_, memory, state.x[0]);
            state.x[0] = static_cast<std::uint32_t>(std::setvbuf(
                operation.host, nullptr, static_cast<int>(state.x[2]),
                static_cast<std::size_t>(state.x[3])));
            operation.reset();
            return std::nullopt;
        }
        if (function == "_fileno") {
            State::StreamAccess operation(*state_, memory, state.x[0]);
            const auto fd = _fileno(operation.host);
            if (fd >= 0) {
                std::lock_guard lock(state_->mutex);
                state_->file_descriptors.insert(fd);
            }
            state.x[0] = static_cast<std::uint32_t>(fd);
            return std::nullopt;
        }
        if (function == "__stdio_common_vfprintf" ||
            function == "__stdio_common_vfprintf_s" ||
            function == "__stdio_common_vfwprintf") {
            static_assert(std::is_pointer_v<va_list>);
            State::StreamAccess operation(*state_, memory, state.x[1]);
            auto* stream = operation.host;
            const bool wide = function == "__stdio_common_vfwprintf";
            const auto wide_format = wide ? guest_string(memory, state.x[2], true)
                                          : std::wstring{};
            const auto format = wide ? printf_format_syntax(wide_format)
                                     : guest_ansi_string(memory, state.x[2]);
            auto arguments = marshal_printf_arguments(
                memory, format, state.x[4], wide,
                (state.x[0] & _CRT_INTERNAL_PRINTF_LEGACY_WIDE_SPECIFIERS) != 0);
            _locale_t locale = nullptr;
            if (state.x[3] != 0) {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->locale_objects.find(state.x[3]);
                if (found == state_->locale_objects.end()) {
                    throw std::runtime_error("invalid guest printf locale");
                }
                locale = found->second.host;
            }
            const auto arglist = reinterpret_cast<va_list>(arguments.slots.data());
            ScopedHostCrtParameterGuard guard;
            const auto result = wide
                ? __stdio_common_vfwprintf(state.x[0], stream,
                                           wide_format.c_str(), locale, arglist)
                : function == "__stdio_common_vfprintf_s"
                ? __stdio_common_vfprintf_s(state.x[0], stream, format.c_str(),
                                             locale, arglist)
                : __stdio_common_vfprintf(state.x[0], stream, format.c_str(),
                                           locale, arglist);
            operation.reset();
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        (function == "GetModuleHandleW" || function == "GetModuleHandleA")) {
        std::wstring name;
        if (state.x[0] != 0) {
            name = lowercase(guest_string(memory, state.x[0],
                                          function == "GetModuleHandleW"));
            if (!name.empty() && name.back() != L'.' &&
                module_basename(name).find(L'.') == std::wstring::npos) {
                name += L".dll";
            }
        }
        std::lock_guard lock(state_->mutex);
        if (state.x[0] == 0) {
            state.x[0] = state_->executable_base;
        } else {
            const auto found = state_->modules.find(name);
            state.x[0] = found == state_->modules.end() ? 0 : found->second;
        }
        if (state.x[0] == 0) SetLastError(ERROR_MOD_NOT_FOUND);
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        function == "SetDefaultDllDirectories") {
        constexpr DWORD allowed = LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
            LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS |
            LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        const auto flags = static_cast<DWORD>(state.x[0]);
        if (state.x[0] != flags || flags == 0 || (flags & ~allowed) != 0 ||
            ((flags & LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) != 0 &&
             flags != LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        std::lock_guard lock(state_->mutex);
        state_->default_dll_search_flags = flags;
        state.x[0] = 1;
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        (function == "LoadLibraryW" || function == "LoadLibraryA" ||
         function == "LoadLibraryExW" || function == "LoadLibraryExA")) {
        {
            std::lock_guard lock(state_->mutex);
            if ((state_->dynamic_module_initializing &&
                 state_->dynamic_module_initializing_thread == GetCurrentThreadId()) ||
                state_->dll_notification_thread == GetCurrentThreadId()) {
                SetLastError(ERROR_NOT_SUPPORTED);
                state.x[0] = 0;
                return std::nullopt;
            }
        }
        const bool extended = function == "LoadLibraryExW" ||
                              function == "LoadLibraryExA";
        if (extended && state.x[1] != 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        if (state.x[0] == 0 || !guest_range(memory, state.x[0],
                                           function.ends_with('W') ? 2 : 1, false)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto raw_name = guest_string(memory, state.x[0],
                                           function.ends_with('W'));
        auto name = lowercase(raw_name);
        if (!name.empty() && name.back() != L'.' &&
            module_basename(name).find(L'.') == std::wstring::npos) {
            name += L".dll";
        }
        constexpr DWORD search_flags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
            LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_USER_DIRS |
            LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
        const auto flags = extended ? static_cast<DWORD>(state.x[2]) : 0;
        if ((extended && state.x[2] != flags) || (flags & ~search_flags) != 0 ||
            ((flags & LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) != 0 &&
             (flags & ~(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS |
                        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR)) != 0)) {
            SetLastError(ERROR_NOT_SUPPORTED);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto requested = std::filesystem::path(name);
        if ((flags & LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) != 0 &&
            !requested.is_absolute()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        std::vector<std::filesystem::path> directories;
        std::function<std::uint64_t(const std::filesystem::path&,
            const std::vector<std::filesystem::path>&, bool,
            ReferenceEngine&)> loader;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->modules.find(name);
            if (found != state_->modules.end()) {
                state.x[0] = found->second;
                return std::nullopt;
            }
            loader = state_->guest_dll_loader;
            auto effective = flags == 0 ? state_->default_dll_search_flags : flags;
            if (effective == LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) {
                effective = LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                    LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS;
            }
            if (flags == 0 && effective == 0) {
                effective = LOAD_LIBRARY_SEARCH_APPLICATION_DIR;
            }
            if ((effective & LOAD_LIBRARY_SEARCH_APPLICATION_DIR) != 0) {
                const auto executable = state_->guest_modules.find(
                    state_->executable_base);
                if (executable != state_->guest_modules.end()) {
                    directories.push_back(std::filesystem::path(
                        executable->second->source()).parent_path());
                }
            }
            if ((effective & LOAD_LIBRARY_SEARCH_USER_DIRS) != 0) {
                for (const auto& [cookie, directory] : state_->dll_directories) {
                    static_cast<void>(cookie);
                    directories.emplace_back(directory.path);
                }
            }
        }
        auto* engine = dynamic_cast<ReferenceEngine*>(&memory);
        if (loader && engine) {
            const auto try_load = [&](const std::filesystem::path& path) {
                if (!std::filesystem::is_regular_file(path)) return false;
                try {
                    state.x[0] = loader(path, directories,
                        (flags & LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) != 0,
                        *engine);
                } catch (const std::system_error& error) {
                    SetLastError(static_cast<DWORD>(error.code().value()));
                    state.x[0] = 0;
                } catch (const std::exception&) {
                    SetLastError(ERROR_NOT_SUPPORTED);
                    state.x[0] = 0;
                }
                return true;
            };
            if (requested.is_absolute()) {
                if (try_load(requested)) return std::nullopt;
            } else {
                for (const auto& directory : directories) {
                    if (try_load(directory / requested)) return std::nullopt;
                }
            }
        }
        SetLastError(ERROR_MOD_NOT_FOUND);
        state.x[0] = 0;
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        function == "FreeLibrary") {
        std::lock_guard lock(state_->mutex);
        const auto known = state_->guest_modules.contains(state.x[0]) ||
                           state.x[0] == kernel32_handle ||
                           state.x[0] == kernelbase_handle ||
                           state.x[0] == opengl32_handle;
        state.x[0] = known ? 1 : 0;
        if (!known) SetLastError(ERROR_INVALID_HANDLE);
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        (function == "GetModuleHandleExW" || function == "GetModuleHandleExA")) {
        const auto flags = static_cast<DWORD>(state.x[0]);
        if (state.x[0] != flags || (flags & ~7U) != 0 || state.x[2] == 0) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        std::wstring name;
        if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) == 0 &&
            state.x[1] != 0) {
            name = lowercase(guest_string(memory, state.x[1],
                                          function == "GetModuleHandleExW"));
            if (!name.empty() && name.back() != L'.' &&
                module_basename(name).find(L'.') == std::wstring::npos) {
                name += L".dll";
            }
        }
        std::uint64_t handle = 0;
        {
            std::lock_guard lock(state_->mutex);
            if ((flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) != 0) {
                for (const auto& [base, module] : state_->guest_modules) {
                    if (state.x[1] >= base &&
                        state.x[1] - base < module->size()) {
                        handle = base;
                        break;
                    }
                }
            } else if (state.x[1] == 0) {
                handle = state_->executable_base;
            } else {
                const auto found = state_->modules.find(name);
                if (found != state_->modules.end()) handle = found->second;
            }
        }
        if (handle == 0) {
            SetLastError(ERROR_MOD_NOT_FOUND);
            state.x[0] = 0;
        } else {
            memory.write64(state.x[2], handle);
            state.x[0] = 1;
        }
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        function == "AddDllDirectory") {
        if (state.x[0] == 0 || !guest_range(memory, state.x[0], 2, false)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto path = guest_string(memory, state.x[0], true);
        const auto host_cookie = ::AddDllDirectory(path.c_str());
        if (host_cookie == nullptr) {
            state.x[0] = 0;
            return std::nullopt;
        }
        std::lock_guard lock(state_->mutex);
        const auto guest_cookie = state_->next_dll_cookie;
        state_->next_dll_cookie += 8;
        state_->dll_directories.emplace(guest_cookie,
            State::GuestDllDirectory{host_cookie, path});
        state.x[0] = guest_cookie;
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        function == "RemoveDllDirectory") {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->dll_directories.find(state.x[0]);
        if (found == state_->dll_directories.end()) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto success = ::RemoveDllDirectory(found->second.host);
        if (success) state_->dll_directories.erase(found);
        state.x[0] = static_cast<std::uint32_t>(success);
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        function == "GetProcAddress") {
        const auto handle = state.x[0];
        const auto symbol_address = state.x[1];
        if (symbol_address <= 0xffff) {
            throw std::runtime_error("ordinal GetProcAddress is not yet supported");
        }
        const auto wide_name = guest_string(memory, symbol_address, false);
        std::string name;
        name.reserve(wide_name.size());
        for (const auto character : wide_name) {
            name.push_back(static_cast<char>(character));
        }
        if (handle == opengl32_handle) {
            CpuState lookup = state;
            if (dispatch_gui({"opengl32.dll", "transmuteGetProcAddress", 0}, lookup, memory)) {
                state.x[0] = lookup.x[0];
                return std::nullopt;
            }
        }
        const bool dynamic_kernel32_api =
            (handle == kernel32_handle || handle == kernelbase_handle) &&
            (name == "AddDllDirectory" || name == "RemoveDllDirectory" ||
             name == "SetDefaultDllDirectories" || name == "GetLastError" ||
             name == "SetLastError");
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->guest_modules.find(handle);
            const auto exported = found == state_->guest_modules.end()
                                      ? std::nullopt : found->second->find_export(name);
            state.x[0] = exported.value_or(0);
            if (state.x[0] == 0 && dynamic_kernel32_api) {
                const auto existing = state_->dynamic_kernel32_exports.find(name);
                if (existing != state_->dynamic_kernel32_exports.end()) {
                    state.x[0] = existing->second;
                }
            }
        }
        if (state.x[0] == 0 && dynamic_kernel32_api) {
            if (auto* engine = dynamic_cast<ReferenceEngine*>(&memory)) {
                // Register outside State's lock: guest-call registration takes
                // the shared address-space lock, which guest dispatch can hold.
                const auto candidate = engine->register_guest_call(
                    [this, name](CpuState& call, GuestMemory& guest) {
                        static_cast<void>(dispatch(
                            {"kernel32.dll", name, 0}, call, guest));
                    });
                bool published = false;
                {
                    std::lock_guard lock(state_->mutex);
                    const auto [entry, inserted] =
                        state_->dynamic_kernel32_exports.try_emplace(name, candidate);
                    state.x[0] = entry->second;
                    published = inserted;
                }
                if (!published) engine->unregister_guest_call(candidate);
            }
        }
        if (state.x[0] == 0) SetLastError(ERROR_PROC_NOT_FOUND);
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        function == "RtlPcToFileHeader") {
        const auto address = state.x[0];
        std::uint64_t base = 0;
        {
            std::lock_guard lock(state_->mutex);
            for (const auto& [candidate_base, module] : state_->guest_modules) {
                if (address >= candidate_base &&
                    address - candidate_base < module->size()) {
                    base = candidate_base;
                    break;
                }
            }
        }
        if (state.x[1] != 0) memory.write64(state.x[1], base);
        state.x[0] = base;
        return std::nullopt;
    }
    if ((dll == "kernel32.dll" || dll == "kernelbase.dll") &&
        (function == "GetModuleFileNameW" || function == "GetModuleFileNameA")) {
        const auto handle = state.x[0];
        const auto output = state.x[1];
        const auto capacity = static_cast<std::uint32_t>(state.x[2]);
        const PeImage* module = nullptr;
        {
            std::lock_guard lock(state_->mutex);
            const auto selected = handle == 0 ? state_->executable_base : handle;
            const auto found = state_->guest_modules.find(selected);
            if (found != state_->guest_modules.end()) module = found->second;
        }
        if (!module) {
            SetLastError(ERROR_INVALID_HANDLE);
            state.x[0] = 0;
            return std::nullopt;
        }
        if (capacity == 0) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const bool wide = function == "GetModuleFileNameW";
        const auto name = wide ? std::filesystem::path(module->source()).wstring()
                               : std::wstring(module->source().begin(),
                                              module->source().end());
        const auto copied = std::min<std::size_t>(name.size(), capacity - 1);
        for (std::size_t index = 0; index < copied; ++index) {
            const auto character = static_cast<std::uint16_t>(name[index]);
            memory.write8(output + index * (wide ? 2 : 1),
                          static_cast<std::uint8_t>(character));
            if (wide) {
                memory.write8(output + index * 2 + 1,
                              static_cast<std::uint8_t>(character >> 8));
            }
        }
        memory.write8(output + copied * (wide ? 2 : 1), 0);
        if (wide) memory.write8(output + copied * 2 + 1, 0);
        if (copied < name.size()) {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            state.x[0] = capacity;
        } else {
            state.x[0] = copied;
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_set_app_type" || function == "_query_app_type")) {
        if (function == "_set_app_type") {
            if (state.x[0] > 2) {
                throw std::runtime_error("invalid guest CRT application type");
            }
            state_->crt_app_type = static_cast<std::uint32_t>(state.x[0]);
        } else {
            state.x[0] = state_->crt_app_type;
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "__p___argc" || function == "__p___argv" ||
         function == "__p___wargv")) {
        std::lock_guard lock(state_->mutex);
        if (!state_->argv_mapped) {
            memory.map_memory(argv_base, argv_region_size);
            state_->argv_mapped = true;
        }
        state.x[0] = argv_base + (function == "__p___argc" ? 0 :
                                  function == "__p___argv" ? 8 : 16);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_configure_narrow_argv" ||
         function == "_configure_wide_argv")) {
        const auto mode = static_cast<std::uint32_t>(state.x[0]);
        if (mode > 2) {
            state.x[0] = EINVAL;
        } else {
            const bool wide = function == "_configure_wide_argv";
            if (mode != 0) {
                std::wstring command_line = state_->command_line_w;
                if (command_line.empty()) {
                    std::lock_guard lock(state_->mutex);
                    const auto found = state_->guest_modules.find(
                        state_->executable_base);
                    if (found != state_->guest_modules.end()) {
                        command_line = L'"' +
                            std::filesystem::path(found->second->source()).wstring() +
                            L'"';
                    }
                }
                const auto arguments = parse_crt_command_line(command_line);
                if (mode == 2) {
                    for (std::size_t index = 1; index < arguments.size(); ++index) {
                        if (arguments[index].find_first_of(L"*?") !=
                            std::wstring::npos) {
                            throw std::runtime_error(
                                "guest CRT wildcard argv expansion is not yet supported");
                        }
                    }
                }
                std::vector<std::string> narrow_arguments;
                if (!wide) {
                    narrow_arguments.reserve(arguments.size());
                    for (const auto& argument : arguments) {
                        const auto required = WideCharToMultiByte(
                            CP_ACP, 0, argument.c_str(), -1,
                            nullptr, 0, nullptr, nullptr);
                        if (required <= 0) {
                            throw std::runtime_error(
                                "cannot encode guest CRT argv argument");
                        }
                        std::string encoded(static_cast<std::size_t>(required), '\0');
                        if (WideCharToMultiByte(
                                CP_ACP, 0, argument.c_str(), -1,
                                encoded.data(), required, nullptr, nullptr) !=
                            required) {
                            throw std::runtime_error(
                                "cannot write encoded guest CRT argv argument");
                        }
                        narrow_arguments.push_back(std::move(encoded));
                    }
                }
                const auto table = argv_base + (wide ? 0x50000 : 0x1000);
                const auto limit = table + argv_table_region_size;
                auto cursor = table + (arguments.size() + 1) * 8;
                for (std::size_t index = 0; index < arguments.size(); ++index) {
                    cursor += wide ? (arguments[index].size() + 1) * 2
                                   : narrow_arguments[index].size();
                }
                if (cursor > limit) {
                    state.x[0] = ENOMEM;
                    return std::nullopt;
                }
                {
                    std::lock_guard lock(state_->mutex);
                    if (!state_->argv_mapped) {
                        memory.map_memory(argv_base, argv_region_size);
                        state_->argv_mapped = true;
                    }
                }
                cursor = table + (arguments.size() + 1) * 8;
                for (std::size_t index = 0; index < arguments.size(); ++index) {
                    memory.write64(table + index * 8, cursor);
                    if (wide) {
                        for (const auto character : arguments[index]) {
                            memory.write8(cursor++,
                                static_cast<std::uint8_t>(character));
                            memory.write8(cursor++,
                                static_cast<std::uint8_t>(character >> 8));
                        }
                        memory.write8(cursor++, 0);
                        memory.write8(cursor++, 0);
                    } else {
                        for (const auto character : narrow_arguments[index]) {
                            memory.write8(cursor++,
                                static_cast<std::uint8_t>(character));
                        }
                    }
                }
                memory.write64(table + arguments.size() * 8, 0);
                memory.write64(argv_base + (wide ? 16 : 8), table);
                memory.write32(argv_base,
                    static_cast<std::uint32_t>(arguments.size()));
            }
            if (wide) {
                state_->wide_argv_mode = mode;
            } else {
                state_->narrow_argv_mode = mode;
            }
            state.x[0] = 0;
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_get_wide_winmain_command_line" ||
         function == "_get_narrow_winmain_command_line")) {
        const bool wide = function == "_get_wide_winmain_command_line";
        const auto length = wide ? state_->command_line_w.size()
                                 : state_->command_line_a.empty()
                                     ? 0 : state_->command_line_a.size() - 1;
        std::size_t offset = 0;
        while (offset < length && (wide
                ? std::iswspace(state_->command_line_w[offset]) != 0
                : std::isspace(static_cast<unsigned char>(
                      state_->command_line_a[offset])) != 0)) {
            ++offset;
        }
        bool quoted = false;
        unsigned backslashes = 0;
        for (; offset < length; ++offset) {
            const auto character = wide
                ? static_cast<std::uint32_t>(state_->command_line_w[offset])
                : static_cast<std::uint8_t>(state_->command_line_a[offset]);
            if (character == '\\') {
                ++backslashes;
                continue;
            }
            if (character == '"' && (backslashes & 1U) == 0) quoted = !quoted;
            if (!quoted && character != '"' &&
                (wide ? std::iswspace(static_cast<wint_t>(character)) != 0
                      : std::isspace(static_cast<unsigned char>(character)) != 0)) {
                break;
            }
            backslashes = 0;
        }
        while (offset < length && (wide
                ? std::iswspace(state_->command_line_w[offset]) != 0
                : std::isspace(static_cast<unsigned char>(
                      state_->command_line_a[offset])) != 0)) {
            ++offset;
        }
        state.x[0] = command_line_base + (wide ? offset * 2 : 65536 + offset);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        function == "__fpe_flt_rounds") {
        // C FLT_ROUNDS uses 1/2/3/0 for nearest/up/down/toward zero.
        // Arm FPCR.RMode uses the same order starting at zero.
        constexpr std::array<std::uint32_t, 4> rounds{1, 2, 3, 0};
        state.x[0] = rounds[(state.fpcr >> 22) & 3U];
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "feclearexcept" || function == "fetestexcept" ||
         function == "fegetexceptflag" || function == "fesetexceptflag" ||
         function == "fegetround" || function == "fesetround" ||
         function == "fegetenv" || function == "fesetenv" ||
         function == "feholdexcept" || function == "feraiseexcept" ||
         function == "feupdateenv")) {
        static_assert(sizeof(fenv_t) == 8 && sizeof(fexcept_t) == 4);
        constexpr std::array exception_bits{
            std::pair{FE_INVALID, 0U}, std::pair{FE_DIVBYZERO, 1U},
            std::pair{FE_OVERFLOW, 2U}, std::pair{FE_UNDERFLOW, 3U},
            std::pair{FE_INEXACT, 4U}};
        const auto status = [&] {
            std::uint32_t result = 0;
            for (const auto [flag, bit] : exception_bits) {
                if ((state.fpsr & (1U << bit)) != 0) result |= flag;
            }
            return result;
        };
        const auto set_status = [&](std::uint32_t flags, std::uint32_t mask) {
            for (const auto [flag, bit] : exception_bits) {
                if ((mask & flag) != 0) {
                    state.fpsr = (state.fpsr & ~(1U << bit)) |
                        (((flags & flag) != 0 ? 1U : 0U) << bit);
                }
            }
        };
        const auto control = [&] {
            std::uint32_t result = 0;
            constexpr std::array masks{
                std::pair{FE_INVALID, 8U}, std::pair{FE_DIVBYZERO, 9U},
                std::pair{FE_OVERFLOW, 10U}, std::pair{FE_UNDERFLOW, 11U},
                std::pair{FE_INEXACT, 12U}, std::pair{0x20, 15U}};
            for (const auto [mask, bit] : masks) {
                if ((state.fpcr & (1U << bit)) == 0) result |= mask;
            }
            switch ((state.fpcr >> 22) & 3U) {
            case 1: result |= FE_UPWARD; break;
            case 2: result |= FE_DOWNWARD; break;
            case 3: result |= FE_TOWARDZERO; break;
            default: break;
            }
            if ((state.fpcr & (1U << 24)) != 0) result |= _DN_FLUSH;
            return result;
        };
        const auto set_control = [&](std::uint32_t value) {
            constexpr std::array masks{
                std::pair{FE_INVALID, 8U}, std::pair{FE_DIVBYZERO, 9U},
                std::pair{FE_OVERFLOW, 10U}, std::pair{FE_UNDERFLOW, 11U},
                std::pair{FE_INEXACT, 12U}, std::pair{0x20, 15U}};
            for (const auto [mask, bit] : masks) {
                if ((value & mask) != 0) state.fpcr &= ~(1U << bit);
                else state.fpcr |= 1U << bit;
            }
            state.fpcr &= ~((3U << 22) | (1U << 24));
            switch (value & FE_ROUND_MASK) {
            case FE_UPWARD: state.fpcr |= 1U << 22; break;
            case FE_DOWNWARD: state.fpcr |= 2U << 22; break;
            case FE_TOWARDZERO: state.fpcr |= 3U << 22; break;
            default: break;
            }
            if ((value & _MCW_DN) == _DN_FLUSH) state.fpcr |= 1U << 24;
        };
        const auto flags = static_cast<std::uint32_t>(state.x[0]) & FE_ALL_EXCEPT;
        if (function == "feclearexcept") {
            set_status(0, flags);
            state.x[0] = 0;
        } else if (function == "fetestexcept") {
            state.x[0] = status() & flags;
        } else if (function == "feraiseexcept") {
            // Enabled guest exception traps need architectural delivery, which
            // this bridge cannot yet provide. Do not report successful trapping.
            std::uint32_t enabled = 0;
            for (const auto [flag, bit] : exception_bits) {
                if ((state.fpcr & (1U << (bit + 8))) != 0) enabled |= flag;
            }
            if ((flags & enabled) != 0) state.x[0] = UINT32_MAX;
            else {
                set_status(flags, flags);
                state.x[0] = 0;
            }
        } else if (function == "fegetround") {
            state.x[0] = control() & FE_ROUND_MASK;
        } else if (function == "fesetround") {
            const auto next = static_cast<std::uint32_t>(state.x[0]);
            if (next != FE_TONEAREST && next != FE_UPWARD &&
                next != FE_DOWNWARD && next != FE_TOWARDZERO) {
                state.x[0] = UINT32_MAX;
            } else {
                set_control((control() & ~FE_ROUND_MASK) | next);
                state.x[0] = 0;
            }
        } else if (function == "fegetexceptflag" ||
                   function == "fesetexceptflag") {
            const auto pointer = state.x[0];
            const auto selected = static_cast<std::uint32_t>(state.x[1]) &
                                  FE_ALL_EXCEPT;
            if (pointer == 0) state.x[0] = UINT32_MAX;
            else if (function == "fegetexceptflag") {
                memory.write32(pointer, status() & selected);
                state.x[0] = 0;
            } else {
                set_status(memory.read32(pointer), selected);
                state.x[0] = 0;
            }
        } else {
            const auto pointer = state.x[0];
            if (pointer == 0) {
                state.x[0] = UINT32_MAX;
            } else if (function == "fegetenv" || function == "feholdexcept") {
                memory.write32(pointer, control());
                memory.write32(pointer + 4, status());
                if (function == "feholdexcept") {
                    set_status(0, FE_ALL_EXCEPT);
                    state.fpcr &= ~((0x1fU << 8) | (1U << 15));
                }
                state.x[0] = 0;
            } else {
                const auto saved_control = memory.read32(pointer);
                const auto saved_status = memory.read32(pointer + 4);
                constexpr auto valid_control = 0x3fU | FE_ROUND_MASK | _MCW_DN;
                if ((saved_control & ~valid_control) != 0 ||
                    ((saved_control & _MCW_DN) != 0 &&
                     (saved_control & _MCW_DN) != _DN_FLUSH)) {
                    state.x[0] = UINT32_MAX;
                } else {
                    const auto raised = function == "feupdateenv" ? status() : 0;
                    set_control(saved_control);
                    set_status(saved_status | raised, FE_ALL_EXCEPT);
                    state.x[0] = 0;
                }
            }
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_control87" || function == "_controlfp" ||
         function == "_controlfp_s")) {
        const bool secure = function == "_controlfp_s";
        const auto output = secure ? state.x[0] : 0;
        const auto next = static_cast<std::uint32_t>(state.x[secure ? 1 : 0]);
        auto mask = static_cast<std::uint32_t>(state.x[secure ? 2 : 1]);
        const auto output_region = output == 0
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(output);
        const auto invalid = [&] {
            auto& slot = state_->thread_errno();
            slot.value = EINVAL;
            if (slot.address != 0) memory.write32(slot.address, EINVAL);
            state.x[0] = EINVAL;
            return std::nullopt;
        };
        constexpr auto supported = _MCW_RC | _MCW_DN | _MCW_EM;
        if ((secure && (!output_region || !output_region->writable ||
                        output > UINT64_MAX - 4 ||
                        output + 4 > output_region->base + output_region->size)) ||
            (mask & ~supported) != 0 ||
            ((mask & _MCW_DN) != 0 &&
             (next & _MCW_DN) != _DN_SAVE &&
             (next & _MCW_DN) != _DN_FLUSH)) {
            return invalid();
        }
        if (function != "_control87") mask &= ~_EM_DENORMAL;
        auto fpcr = state.fpcr;
        if ((mask & _MCW_RC) != 0) {
            fpcr &= ~(3U << 22);
            switch (next & _MCW_RC) {
            case _RC_UP: fpcr |= 1U << 22; break;
            case _RC_DOWN: fpcr |= 2U << 22; break;
            case _RC_CHOP: fpcr |= 3U << 22; break;
            default: break;
            }
        }
        if ((mask & _MCW_DN) != 0) {
            fpcr &= ~(1U << 24);
            if ((next & _MCW_DN) == _DN_FLUSH) fpcr |= 1U << 24;
        }
        constexpr std::array exception_bits{
            std::pair{_EM_INVALID, 8U}, std::pair{_EM_ZERODIVIDE, 9U},
            std::pair{_EM_OVERFLOW, 10U}, std::pair{_EM_UNDERFLOW, 11U},
            std::pair{_EM_INEXACT, 12U}, std::pair{_EM_DENORMAL, 15U}};
        for (const auto [crt_mask, fpcr_bit] : exception_bits) {
            if ((mask & crt_mask) == 0) continue;
            if ((next & crt_mask) == 0) fpcr |= 1U << fpcr_bit;
            else fpcr &= ~(1U << fpcr_bit);
        }
        state.fpcr = fpcr;
        std::uint32_t control = 0;
        switch ((fpcr >> 22) & 3U) {
        case 1: control |= _RC_UP; break;
        case 2: control |= _RC_DOWN; break;
        case 3: control |= _RC_CHOP; break;
        default: break;
        }
        if ((fpcr & (1U << 24)) != 0) control |= _DN_FLUSH;
        for (const auto [crt_mask, fpcr_bit] : exception_bits) {
            if ((fpcr & (1U << fpcr_bit)) == 0) control |= crt_mask;
        }
        if (secure) {
            memory.write32(output, control);
            state.x[0] = 0;
        } else {
            state.x[0] = control;
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_set_invalid_parameter_handler" ||
         function == "_get_invalid_parameter_handler" ||
         function == "_set_thread_local_invalid_parameter_handler" ||
         function == "_get_thread_local_invalid_parameter_handler" ||
         function == "_invalid_parameter" ||
         function == "_invalid_parameter_noinfo" ||
         function == "_invalid_parameter_noinfo_noreturn" ||
         function == "_invoke_watson")) {
        const auto thread = GetCurrentThreadId();
        if (function == "_set_invalid_parameter_handler" ||
            function == "_get_invalid_parameter_handler" ||
            function == "_set_thread_local_invalid_parameter_handler" ||
            function == "_get_thread_local_invalid_parameter_handler") {
            std::lock_guard lock(state_->mutex);
            const bool thread_local_handler =
                function.find("thread_local") != std::string::npos;
            auto& handler = thread_local_handler
                ? state_->crt_thread_invalid_parameter_handlers[thread]
                : state_->crt_invalid_parameter_handler;
            const auto previous = handler;
            if (function.starts_with("_set_")) handler = state.x[0];
            state.x[0] = previous;
            return std::nullopt;
        }
        constexpr std::uint32_t guest_fast_fail_invalid_argument = 0xc0000409U;
        if (function == "_invoke_watson") {
            return guest_fast_fail_invalid_argument;
        }
        std::uint64_t handler = 0;
        {
            std::lock_guard lock(state_->mutex);
            if (const auto found =
                    state_->crt_thread_invalid_parameter_handlers.find(thread);
                found != state_->crt_thread_invalid_parameter_handlers.end()) {
                handler = found->second;
            }
            if (handler == 0) handler = state_->crt_invalid_parameter_handler;
        }
        if (handler != 0) {
            auto* engine = dynamic_cast<ReferenceEngine*>(&memory);
            if (engine == nullptr) {
                throw std::runtime_error(
                    "guest invalid-parameter callback requires a reference engine");
            }
            const std::array arguments{
                function == "_invalid_parameter" ? state.x[0] : std::uint64_t{0},
                function == "_invalid_parameter" ? state.x[1] : std::uint64_t{0},
                function == "_invalid_parameter" ? state.x[2] : std::uint64_t{0},
                function == "_invalid_parameter" ? state.x[3] : std::uint64_t{0},
                function == "_invalid_parameter" ? state.x[4] : std::uint64_t{0}};
            static_cast<void>(engine->invoke_callback(
                handler, std::span<const std::uint64_t>(arguments)));
        }
        if (handler == 0 || function == "_invalid_parameter_noinfo_noreturn") {
            return guest_fast_fail_invalid_argument;
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_errno" || function == "_get_errno" ||
         function == "_set_errno")) {
        std::lock_guard lock(state_->mutex);
        auto& slot = state_->thread_errno();
        if (function == "_errno") {
            if (slot.address == 0) {
                slot.address = state_->next_environment_address;
                memory.map_memory(slot.address, 16);
                state_->next_environment_address += 16;
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(slot.value));
            }
            state.x[0] = slot.address;
        } else if (function == "_get_errno") {
            if (state.x[0] == 0) {
                state.x[0] = EINVAL;
            } else {
                if (slot.address != 0) {
                    slot.value = static_cast<std::int32_t>(
                        memory.read32(slot.address));
                }
                memory.write32(state.x[0],
                               static_cast<std::uint32_t>(slot.value));
                state.x[0] = 0;
            }
        } else {
            slot.value = static_cast<std::int32_t>(state.x[0]);
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(slot.value));
            }
            state.x[0] = 0;
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "__sys_nerr" || function == "__sys_errlist" ||
         function == "strerror" || function == "strerror_s" ||
         function == "perror")) {
        const auto set_guest_errno = [this, &memory](int error) {
            auto& slot = state_->thread_errno();
            slot.value = error;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(error));
            }
        };
        if (function == "__sys_nerr" || function == "__sys_errlist") {
            std::lock_guard lock(state_->mutex);
            if (!state_->crt_error_table_mapped) {
                const auto count = host_system_error_count();
                const auto* messages = host_system_error_list();
                if (count <= 0 || count > 1024 || messages == nullptr) {
                    throw std::runtime_error("host CRT error table is invalid");
                }
                if (!state_->crt_error_region_mapped) {
                    memory.map_memory(crt_errors_base, 0x100000);
                    state_->crt_error_region_mapped = true;
                }
                memory.write32(crt_errors_base,
                               static_cast<std::uint32_t>(count));
                auto cursor = crt_errors_base + 0x2000;
                for (int index = 0; index < count; ++index) {
                    const std::string_view message = messages[index] == nullptr
                        ? std::string_view{} : std::string_view(messages[index]);
                    if (cursor >= crt_errors_base + 0x40000 ||
                        message.size() >= crt_errors_base + 0x40000 - cursor) {
                        throw std::runtime_error("host CRT error table is too large");
                    }
                    memory.write64(crt_errors_base + 0x100 +
                                   static_cast<std::uint64_t>(index) * 8,
                                   cursor);
                    for (const auto character : message) {
                        memory.write8(cursor++,
                                      static_cast<std::uint8_t>(character));
                    }
                    memory.write8(cursor++, 0);
                }
                state_->crt_error_table_mapped = true;
            }
            state.x[0] = function == "__sys_nerr"
                ? crt_errors_base : crt_errors_base + 0x100;
            return std::nullopt;
        }
        if (function == "strerror_s") {
            const auto output = state.x[0];
            const auto capacity = state.x[1];
            if (output == 0 || capacity == 0 || capacity > 1024 * 1024 ||
                !guest_range(memory, output, capacity, true)) {
                set_guest_errno(EINVAL);
                state.x[0] = EINVAL;
                return std::nullopt;
            }
            std::vector<char> text(static_cast<std::size_t>(capacity));
            const ScopedHostCrtParameterGuard parameter_guard;
            const auto result = strerror_s(text.data(), text.size(),
                                           static_cast<int>(state.x[2]));
            for (std::size_t index = 0; index < text.size(); ++index) {
                memory.write8(output + index,
                              static_cast<std::uint8_t>(text[index]));
                if (text[index] == '\0') break;
            }
            if (result != 0) set_guest_errno(result);
            state.x[0] = static_cast<std::uint32_t>(result);
            return std::nullopt;
        }
        const auto error = function == "perror"
            ? state_->thread_errno().value : static_cast<int>(state.x[0]);
        std::array<char, 512> text{};
        {
            const ScopedHostCrtParameterGuard parameter_guard;
            static_cast<void>(strerror_s(text.data(), text.size(), error));
        }
        if (function == "perror") {
            const auto prefix = state.x[0] == 0
                ? std::string{} : guest_ansi_string(memory, state.x[0]);
            if (!prefix.empty()) fprintf(stderr, "%s: %s\n",
                                         prefix.c_str(), text.data());
            else fprintf(stderr, "%s\n", text.data());
            return std::nullopt;
        }
        std::lock_guard lock(state_->mutex);
        auto& guest_buffer = state_->crt_error_buffers[GetCurrentThreadId()];
        if (guest_buffer == 0) {
            if (!state_->crt_error_region_mapped) {
                memory.map_memory(crt_errors_base, 0x100000);
                state_->crt_error_region_mapped = true;
            }
            if (!state_->free_crt_error_buffers.empty()) {
                guest_buffer = state_->free_crt_error_buffers.back();
                state_->free_crt_error_buffers.pop_back();
            } else {
                if (state_->next_crt_error_buffer >
                    crt_errors_base + 0x100000 - text.size()) {
                    throw std::runtime_error("guest CRT error buffers exhausted");
                }
                guest_buffer = state_->next_crt_error_buffer;
                state_->next_crt_error_buffer += text.size();
            }
        }
        for (std::size_t index = 0; index < text.size(); ++index) {
            memory.write8(guest_buffer + index,
                          static_cast<std::uint8_t>(text[index]));
            if (text[index] == '\0') break;
        }
        state.x[0] = guest_buffer;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        function == "_getpid") {
        state.x[0] = GetCurrentProcessId();
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        function == "_endthreadex") {
        return static_cast<std::uint32_t>(state.x[0]);
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        function == "_beginthreadex") {
        const auto set_guest_errno = [this, &memory](int error) {
            auto& slot = state_->thread_errno();
            slot.value = error;
            if (slot.address != 0) {
                memory.write32(slot.address,
                               static_cast<std::uint32_t>(error));
            }
        };
        const auto flags = static_cast<std::uint32_t>(state.x[4]);
        const auto thread_id_address = state.x[5];
        const auto thread_id_region = thread_id_address == 0
            ? std::optional<GuestMemory::RegionInfo>{}
            : memory.query_region(thread_id_address);
        if (state.x[0] != 0 || state.x[1] > 1024ULL * 1024U * 1024U ||
            state.x[2] == 0 || state.x[4] != flags ||
            (flags & ~(CREATE_SUSPENDED |
                       STACK_SIZE_PARAM_IS_A_RESERVATION)) != 0 ||
            (thread_id_address != 0 &&
                (!thread_id_region || !thread_id_region->writable ||
                 thread_id_address + 4 >
                     thread_id_region->base + thread_id_region->size))) {
            set_guest_errno(EINVAL);
            state.x[0] = 0;
            return std::nullopt;
        }
        {
            std::lock_guard lock(state_->mutex);
            if (state_->shutting_down_guest_threads) {
                set_guest_errno(EAGAIN);
                state.x[0] = 0;
                return std::nullopt;
            }
        }
        auto* parent = dynamic_cast<ReferenceEngine*>(&memory);
        if (parent == nullptr) {
            throw std::runtime_error("guest thread requires a reference engine");
        }
        const auto requested_stack = state.x[1];
        const auto stack_reserve = (flags & STACK_SIZE_PARAM_IS_A_RESERVATION) != 0 ||
                                   requested_stack > parent->default_worker_stack_size()
            ? requested_stack : 0;
        auto worker = std::shared_ptr<ReferenceEngine>(
            parent->fork_thread(state.x[2], state.x[3], stack_reserve));
        auto record = std::make_shared<GuestThreadRecord>();
        const auto image_base = state_->executable_base;
        auto start = std::make_unique<GuestThreadStart>();
        start->record = record;
        start->run = [this, worker, image_base, record] {
            {
                std::unique_lock lock(state_->mutex);
                state_->dynamic_module_ready.wait(lock, [this] {
                    return !state_->dynamic_module_initializing;
                });
            }
            initialize_worker_thread_environment(*worker, image_base);
            std::uint32_t exit = 0;
            try {
                exit = worker->run().exit_code;
            } catch (...) {
                const auto original = std::current_exception();
                // Guest destructors can block or fail too. Publish the run
                // failure before calling them so diagnosis preserves its cause.
                try {
                    std::rethrow_exception(original);
                } catch (const std::exception& error) {
                    std::lock_guard lock(record->mutex);
                    record->failure = error.what();
                } catch (...) {
                    std::lock_guard lock(record->mutex);
                    record->failure = "unknown guest thread failure";
                }
                try {
                    cleanup_worker_thread(*worker);
                } catch (...) {
                    // The original run failure is the useful diagnostic.
                }
                std::rethrow_exception(original);
            }
            cleanup_worker_thread(*worker);
            return exit;
        };
        unsigned native_thread_id = 0;
        const auto native_value = _beginthreadex(
            nullptr, static_cast<unsigned>(state.x[1]), run_guest_thread,
            start.get(), CREATE_SUSPENDED |
                (flags & STACK_SIZE_PARAM_IS_A_RESERVATION),
            &native_thread_id);
        if (native_value == 0) {
            set_guest_errno(errno);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto native = reinterpret_cast<HANDLE>(native_value);
        HANDLE retained = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), native, GetCurrentProcess(),
                             &retained, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            TerminateThread(native, 0xe0000001U);
            CloseHandle(native);
            set_guest_errno(EAGAIN);
            state.x[0] = 0;
            return std::nullopt;
        }
        record->retained = retained;
        bool registered = false;
        {
            std::lock_guard lock(state_->mutex);
            if (!state_->shutting_down_guest_threads) {
                std::erase_if(state_->guest_threads, [](const auto& previous) {
                    if (WaitForSingleObject(previous->retained, 0) != WAIT_OBJECT_0) {
                        return false;
                    }
                    std::lock_guard record_lock(previous->mutex);
                    return previous->failure.empty();
                });
                record->guest_handle = state_->next_host_handle;
                state_->next_host_handle += 8;
                state_->host_handles.emplace(record->guest_handle,
                                             State::HostHandle{native, true});
                state_->guest_threads.push_back(record);
                registered = true;
            }
        }
        if (!registered) {
            TerminateThread(native, 0xe0000002U);
            CloseHandle(native);
            set_guest_errno(EAGAIN);
            state.x[0] = 0;
            return std::nullopt;
        }
        start.release();
        if (thread_id_address != 0) {
            memory.write32(thread_id_address, native_thread_id);
        }
        if ((flags & CREATE_SUSPENDED) == 0 && ResumeThread(native) == UINT32_MAX) {
            throw std::runtime_error("could not resume newly created guest thread");
        }
        state.x[0] = record->guest_handle;
        return std::nullopt;
    }
    const auto valid_crt_signal = [](int number) {
        return number == SIGABRT || number == SIGABRT_COMPAT ||
               number == SIGFPE || number == SIGILL || number == SIGINT ||
               number == SIGSEGV || number == SIGTERM || number == SIGBREAK;
    };
    const auto signal_key = [](int number) {
        return number == SIGABRT_COMPAT ? SIGABRT : number;
    };
    const auto deliver_crt_signal = [this, &signal_key](int number) {
        std::uint64_t handler = 0;
        std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
        {
            std::lock_guard lock(state_->mutex);
            const auto key = signal_key(number);
            if (const auto found = state_->crt_signal_handlers.find(key);
                found != state_->crt_signal_handlers.end()) {
                handler = found->second;
                if (number != SIGFPE && handler > 1) found->second = 0;
            }
            invoke = state_->guest_callback;
        }
        if (handler == 0) return false;
        if (handler == 1) return true; // SIG_IGN
        if (!invoke) {
            throw std::runtime_error("guest CRT signal callback invoker is unavailable");
        }
        invoke(handler, static_cast<std::uint32_t>(number));
        return true;
    };
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "signal" || function == "raise")) {
        const auto number = static_cast<int>(static_cast<std::uint32_t>(state.x[0]));
        const auto set_invalid_signal = [this, &memory] {
            auto& slot = state_->thread_errno();
            slot.value = EINVAL;
            if (slot.address != 0) {
                memory.write32(slot.address, EINVAL);
            }
        };
        if (!valid_crt_signal(number)) {
            set_invalid_signal();
            state.x[0] = function == "signal" ? UINT64_MAX : UINT32_MAX;
            return std::nullopt;
        }
        if (function == "signal") {
            const auto action = state.x[1];
            if ((action >= 3 && action <= 5) || action == UINT64_MAX) {
                set_invalid_signal();
                state.x[0] = UINT64_MAX;
                return std::nullopt;
            }
            std::lock_guard lock(state_->mutex);
            auto& previous = state_->crt_signal_handlers[signal_key(number)];
            state.x[0] = previous;
            if (action != 2) previous = action; // SIG_GET only queries.
            return std::nullopt;
        }
        if (deliver_crt_signal(number) || number == SIGTERM) {
            state.x[0] = 0;
            return std::nullopt;
        }
        return std::uint32_t{3}; // UCRT default termination, not the host process.
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        function == "_set_abort_behavior") {
        const auto previous = state_->crt_abort_behavior;
        const auto flags = static_cast<std::uint32_t>(state.x[0]);
        const auto mask = static_cast<std::uint32_t>(state.x[1]);
        state_->crt_abort_behavior = (previous & ~mask) | (flags & mask);
        state.x[0] = previous;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        function == "_set_error_mode") {
        const auto mode = static_cast<std::int32_t>(state.x[0]);
        if (mode == 3) {
            state.x[0] = static_cast<std::uint32_t>(state_->crt_error_mode);
        } else if (mode >= 0 && mode <= 2) {
            const auto previous = state_->crt_error_mode;
            state_->crt_error_mode = mode;
            state.x[0] = static_cast<std::uint32_t>(previous);
        } else {
            state.x[0] = UINT32_MAX;
        }
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        function == "_register_thread_local_exe_atexit_callback") {
        std::lock_guard lock(state_->mutex);
        if (state_->thread_local_exe_atexit_callback != 0) {
            throw std::runtime_error(
                "guest CRT thread-local exit callback registered twice");
        }
        state_->thread_local_exe_atexit_callback = state.x[0];
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_crt_atexit" || function == "_crt_at_quick_exit" ||
         function == "_cexit" || function == "_c_exit" ||
         function == "exit" || function == "_exit" ||
         function == "_Exit" || function == "quick_exit" ||
         function == "abort" || function == "terminate")) {
        if (function == "_crt_atexit" || function == "_crt_at_quick_exit") {
            if (state.x[0] == 0) {
                state.x[0] = UINT32_MAX;
            } else {
                auto& callbacks = function == "_crt_atexit"
                    ? state_->crt_exit_callbacks
                    : state_->crt_quick_exit_callbacks;
                callbacks.push_back(state.x[0]);
                state.x[0] = 0;
            }
            return std::nullopt;
        }
        const bool normal_cleanup = function == "_cexit" || function == "exit";
        const bool quick_cleanup = function == "quick_exit";
        if (function == "abort") {
            static_cast<void>(deliver_crt_signal(SIGABRT));
        }
        if (normal_cleanup) cleanup_process_exit();
        if (quick_cleanup || function == "abort" || function == "terminate" ||
            function == "_exit" || function == "_Exit") {
            std::lock_guard lock(state_->mutex);
            state_->process_exit_callbacks_suppressed = true;
        }
        if (normal_cleanup || quick_cleanup) {
            auto& registered = quick_cleanup ? state_->crt_quick_exit_callbacks
                                             : state_->crt_exit_callbacks;
            auto callbacks = std::move(registered);
            std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
            {
                std::lock_guard lock(state_->mutex);
                invoke = state_->guest_callback;
            }
            if (!callbacks.empty() && !invoke) {
                throw std::runtime_error("guest CRT exit callback invoker is unavailable");
            }
            for (auto callback = callbacks.rbegin(); callback != callbacks.rend();
                 ++callback) {
                invoke(*callback, 0);
            }
        }
        if (function == "_cexit" || function == "_c_exit") {
            return std::nullopt;
        }
        return function == "abort" || function == "terminate"
            ? std::optional<std::uint32_t>{3}
            : std::optional<std::uint32_t>{static_cast<std::uint32_t>(state.x[0])};
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_initterm" || function == "_initterm_e")) {
        const auto first = state.x[0];
        const auto last = state.x[1];
        if (last < first || ((last - first) & 7U) != 0 ||
            last - first > 1024U * 1024U) {
            throw std::runtime_error("invalid guest CRT initializer range");
        }
        std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
        {
            std::lock_guard lock(state_->mutex);
            invoke = state_->guest_callback;
        }
        for (auto address = first; address < last; address += 8) {
            const auto callback = memory.read64(address);
            if (callback == 0) continue;
            if (!invoke) throw std::runtime_error("guest CRT callback invoker is unavailable");
            const auto result = invoke(callback, 0);
            if (function == "_initterm_e" && result != 0) {
                state.x[0] = static_cast<std::uint32_t>(result);
                return std::nullopt;
            }
        }
        state.x[0] = 0;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
        (function == "_initialize_onexit_table" ||
         function == "_register_onexit_function" ||
         function == "_execute_onexit_table")) {
        const auto address = state.x[0];
        if (function == "_initialize_onexit_table") {
            for (std::uint64_t offset = 0; offset < 24; ++offset) {
                memory.write8(address + offset, 0);
            }
            std::lock_guard lock(state_->mutex);
            state_->onexit_tables[address].clear();
            state.x[0] = 0;
            return std::nullopt;
        }
        std::vector<std::uint64_t> callbacks;
        std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->onexit_tables.find(address);
            if (found == state_->onexit_tables.end()) {
                throw std::runtime_error("guest onexit table is not initialized");
            }
            if (function == "_register_onexit_function") {
                if (state.x[1] == 0) {
                    state.x[0] = UINT32_MAX;
                    return std::nullopt;
                }
                found->second.push_back(state.x[1]);
                state.x[0] = 0;
                return std::nullopt;
            }
            callbacks = std::move(found->second);
            invoke = state_->guest_callback;
            state_->onexit_tables.erase(found);
        }
        if (!callbacks.empty() && !invoke) {
            throw std::runtime_error("guest onexit callback invoker is unavailable");
        }
        for (auto callback = callbacks.rbegin(); callback != callbacks.rend(); ++callback) {
            invoke(*callback, 0);
        }
        for (std::uint64_t offset = 0; offset < 24; ++offset) {
            memory.write8(address + offset, 0);
        }
        state.x[0] = 0;
        return std::nullopt;
    }
    const bool narrow_crt_environment =
        (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
         (function == "_initialize_narrow_environment" ||
          function == "_get_initial_narrow_environment")) ||
        (dll == "api-ms-win-crt-environment-l1-1-0.dll" &&
         function == "__p__environ");
    const bool wide_crt_environment =
        (dll == "api-ms-win-crt-runtime-l1-1-0.dll" &&
         (function == "_initialize_wide_environment" ||
          function == "_get_initial_wide_environment")) ||
        (dll == "api-ms-win-crt-environment-l1-1-0.dll" &&
         function == "__p__wenviron");
    if (narrow_crt_environment || wide_crt_environment) {
        const bool wide = wide_crt_environment;
        auto& initial = wide ? state_->initial_wide_environment
                             : state_->initial_narrow_environment;
        auto& slot = wide ? state_->wide_environment_slot
                          : state_->narrow_environment_slot;
        if (initial == 0) {
            rebuild_crt_environment(memory, wide);
        }
        state.x[0] = function.starts_with("_initialize_") ? 0 :
            function.starts_with("__p__") ? slot : initial;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-environment-l1-1-0.dll" &&
        (function == "getenv" || function == "_wgetenv")) {
        const bool wide = function == "_wgetenv";
        const auto name_w = wide ? guest_string(memory, state.x[0], true)
                                 : std::wstring{};
        const auto name_a = wide ? std::string{}
                                 : guest_ansi_string(memory, state.x[0]);
        const auto value = guest_environment_value(
            wide ? name_w : environment_wide(name_a), true);
        if (!value) {
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto value_w = wide ? *value : std::wstring{};
        const auto value_a = wide ? std::string{} : environment_ansi(*value);
        std::lock_guard lock(state_->mutex);
        if (wide) {
            auto found = state_->getenv_wide.find(name_w);
            if (found != state_->getenv_wide.end() && found->second.second == value_w) {
                state.x[0] = found->second.first;
                return std::nullopt;
            }
            if (found != state_->getenv_wide.end()) {
                memory.unmap_memory(found->second.first);
                state_->getenv_wide.erase(found);
            }
        } else {
            auto found = state_->getenv_ansi.find(name_a);
            if (found != state_->getenv_ansi.end() && found->second.second == value_a) {
                state.x[0] = found->second.first;
                return std::nullopt;
            }
            if (found != state_->getenv_ansi.end()) {
                memory.unmap_memory(found->second.first);
                state_->getenv_ansi.erase(found);
            }
        }
        const auto length = wide ? value_w.size() : value_a.size();
        const auto bytes = (length + 1) * (wide ? 2U : 1U);
        const auto size = (bytes + 15U) & ~std::size_t{15};
        const auto address = state_->next_environment_address;
        memory.map_memory(address, size);
        state_->next_environment_address += size;
        for (std::size_t index = 0; index < length; ++index) {
            const auto character = wide
                ? static_cast<std::uint16_t>(value_w[index])
                : static_cast<std::uint8_t>(value_a[index]);
            memory.write8(address + index * (wide ? 2U : 1U),
                          static_cast<std::uint8_t>(character));
            if (wide) {
                memory.write8(address + index * 2U + 1,
                              static_cast<std::uint8_t>(character >> 8));
            }
        }
        if (wide) state_->getenv_wide.emplace(name_w, std::make_pair(address, value_w));
        else state_->getenv_ansi.emplace(name_a, std::make_pair(address, value_a));
        state.x[0] = address;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-environment-l1-1-0.dll" &&
        (function == "_putenv_s" || function == "_wputenv_s" ||
         function == "_putenv" || function == "_wputenv")) {
        const bool wide = function.starts_with("_w");
        const bool secure = function.ends_with("_s");
        const auto invalid = [&] {
            state_->thread_errno().value = EINVAL;
            state.x[0] = secure ? EINVAL : UINT32_MAX;
        };
        if (state.x[0] == 0 || (secure && state.x[1] == 0)) {
            invalid();
            return std::nullopt;
        }
        std::wstring name;
        std::wstring value;
        if (secure) {
            name = wide ? guest_string(memory, state.x[0], true) :
                environment_wide(guest_ansi_string(memory, state.x[0]));
            value = wide ? guest_string(memory, state.x[1], true) :
                environment_wide(guest_ansi_string(memory, state.x[1]));
        } else {
            const auto assignment = wide ? guest_string(memory, state.x[0], true)
                : environment_wide(guest_ansi_string(memory, state.x[0]));
            const auto separator = assignment.find(L'=');
            if (separator == std::wstring::npos) {
                invalid();
                return std::nullopt;
            }
            name = assignment.substr(0, separator);
            value = assignment.substr(separator + 1);
        }
        if (name.empty() || name.find(L'=') != std::wstring::npos ||
            name.size() > 32767 || value.size() > 32767) {
            invalid();
            return std::nullopt;
        }
        update_guest_environment(memory, name,
            value.empty() ? std::nullopt :
                std::optional<std::wstring>(std::move(value)), true);
        state.x[0] = 0;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-environment-l1-1-0.dll" &&
        (function == "getenv_s" || function == "_wgetenv_s" ||
         function == "_dupenv_s" || function == "_wdupenv_s")) {
        const bool wide = function.starts_with("_w");
        const bool duplicate = function.find("dupenv") != std::string::npos;
        const auto required_output = duplicate ? state.x[1] : state.x[0];
        const auto buffer = duplicate ? state.x[0] : state.x[1];
        const auto capacity = duplicate ? 0 : state.x[2];
        const auto name_address = state.x[duplicate ? 2 : 3];
        const auto invalid = [&] {
            state_->thread_errno().value = EINVAL;
            state.x[0] = EINVAL;
        };
        if ((!duplicate && !guest_range(memory, required_output, 8, true)) ||
            (duplicate && required_output != 0 &&
             !guest_range(memory, required_output, 8, true)) ||
            name_address == 0 || capacity > 1024 * 1024 ||
            (!duplicate && capacity != 0 &&
             !guest_range(memory, buffer, capacity * (wide ? 2U : 1U), true)) ||
            (duplicate && !guest_range(memory, buffer, 8, true))) {
            invalid();
            return std::nullopt;
        }
        const auto name = wide ? guest_string(memory, name_address, true) :
            environment_wide(guest_ansi_string(memory, name_address));
        if (name.empty()) {
            invalid();
            return std::nullopt;
        }
        const auto found = guest_environment_value(name, true);
        const auto ansi = wide || !found ? std::string{} :
            environment_ansi(*found);
        const auto length = !found ? 0 : wide ? found->size() : ansi.size();
        const auto required = found ? length + 1 : 0;
        if (required_output != 0) memory.write64(required_output, required);
        if (duplicate) {
            memory.write64(buffer, 0);
            if (!found) {
                state.x[0] = 0;
                return std::nullopt;
            }
            CpuState allocation{};
            allocation.x[0] = required * (wide ? 2U : 1U);
            static_cast<void>(dispatch(
                {"api-ms-win-crt-heap-l1-1-0.dll", "malloc", 0},
                allocation, memory));
            if (allocation.x[0] == 0) {
                if (required_output != 0) memory.write64(required_output, 0);
                state_->thread_errno().value = ENOMEM;
                state.x[0] = ENOMEM;
                return std::nullopt;
            }
            memory.write64(buffer, allocation.x[0]);
            for (std::size_t index = 0; index <= length; ++index) {
                const auto character = index == length ? 0 : wide ?
                    static_cast<std::uint16_t>((*found)[index]) :
                    static_cast<std::uint8_t>(ansi[index]);
                memory.write8(allocation.x[0] + index * (wide ? 2U : 1U),
                              static_cast<std::uint8_t>(character));
                if (wide) memory.write8(allocation.x[0] + index * 2 + 1,
                    static_cast<std::uint8_t>(character >> 8));
            }
            state.x[0] = 0;
            return std::nullopt;
        }
        if (capacity != 0) {
            memory.write8(buffer, 0);
            if (wide) memory.write8(buffer + 1, 0);
        }
        if (!found) {
            state.x[0] = 0;
            return std::nullopt;
        }
        if (capacity < required) {
            state_->thread_errno().value = ERANGE;
            state.x[0] = ERANGE;
            return std::nullopt;
        }
        for (std::size_t index = 0; index <= length; ++index) {
            const auto character = index == length ? 0 : wide ?
                static_cast<std::uint16_t>((*found)[index]) :
                static_cast<std::uint8_t>(ansi[index]);
            memory.write8(buffer + index * (wide ? 2U : 1U),
                          static_cast<std::uint8_t>(character));
            if (wide) memory.write8(buffer + index * 2 + 1,
                static_cast<std::uint8_t>(character >> 8));
        }
        state.x[0] = 0;
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-stdio-l1-1-0.dll" &&
        (function == "__stdio_common_vsscanf" ||
         function == "__stdio_common_vswscanf")) {
        _locale_t locale = nullptr;
        if (state.x[4] != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->locale_objects.find(state.x[4]);
            if (found == state_->locale_objects.end())
                throw std::runtime_error("invalid guest scanf locale");
            locale = found->second.host;
        }
        auto& error = state_->thread_errno();
        errno = error.value;
        ScopedHostCrtParameterGuard guard;
        const auto result = scan_guest_string(state, memory,
            function == "__stdio_common_vswscanf", locale);
        error.value = errno;
        if (error.address != 0)
            memory.write32(error.address, static_cast<std::uint32_t>(error.value));
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-stdio-l1-1-0.dll" &&
        (function == "__stdio_common_vsprintf" ||
         function == "__stdio_common_vsprintf_s" ||
         function == "__stdio_common_vsnprintf_s")) {
        static_assert(std::is_pointer_v<va_list>);
        const bool bounded = function == "__stdio_common_vsnprintf_s";
        const auto capacity = state.x[2];
        const bool unbounded_vsprintf =
            function == "__stdio_common_vsprintf" && capacity == UINT64_MAX;
        if (!unbounded_vsprintf && capacity > 1024ULL * 1024ULL) {
            throw std::runtime_error("guest formatted output buffer is too large");
        }
        const auto format = guest_ansi_string(memory, state.x[bounded ? 4 : 3]);
        // The marshaller rejects %n and unsupported conversions, so counting
        // and formatting cannot replay a guest-visible writeback conversion.
        auto arguments = marshal_printf_arguments(
            memory, format, state.x[bounded ? 6 : 5]);
        _locale_t locale = nullptr;
        const auto guest_locale = state.x[bounded ? 5 : 4];
        if (guest_locale != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->locale_objects.find(guest_locale);
            if (found == state_->locale_objects.end()) {
                throw std::runtime_error("invalid guest printf locale");
            }
            locale = found->second.host;
        }
        const auto arglist = reinterpret_cast<va_list>(arguments.slots.data());
        std::uint64_t output_capacity = capacity;
        if (unbounded_vsprintf) {
            constexpr auto legacy_termination =
                _CRT_INTERNAL_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION;
            constexpr auto standard_count =
                _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR;
            const auto count_options =
                (state.x[0] & ~legacy_termination) | standard_count;
            ScopedHostCrtParameterGuard guard;
            const auto count = __stdio_common_vsprintf(
                count_options, nullptr, 0, format.c_str(), locale, arglist);
            if (count < 0) {
                state.x[0] = static_cast<std::uint32_t>(-1);
                return std::nullopt;
            }
            output_capacity = static_cast<std::uint64_t>(count) + 1;
            if (!guest_range(memory, state.x[1], output_capacity, true)) {
                throw std::runtime_error(
                    "guest vsprintf result exceeds writable output memory");
            }
        } else if (capacity != 0 &&
                   !guest_range(memory, state.x[1], capacity, true)) {
            throw std::runtime_error("invalid guest formatted output buffer");
        }
        std::vector<char> output(static_cast<std::size_t>(
            std::max<std::uint64_t>(output_capacity, 1)));
        auto* buffer = output_capacity == 0 ? nullptr : output.data();
        ScopedHostCrtParameterGuard guard;
        const auto result = bounded
            ? __stdio_common_vsnprintf_s(state.x[0], buffer,
                static_cast<std::size_t>(output_capacity),
                static_cast<std::size_t>(state.x[3]), format.c_str(),
                locale, arglist)
            : function == "__stdio_common_vsprintf_s"
                ? __stdio_common_vsprintf_s(state.x[0], buffer,
                    static_cast<std::size_t>(output_capacity), format.c_str(),
                    locale, arglist)
                : __stdio_common_vsprintf(state.x[0], buffer,
                    static_cast<std::size_t>(output_capacity), format.c_str(),
                    locale, arglist);
        if (unbounded_vsprintf && result >= 0 &&
            static_cast<std::uint64_t>(result) >= output_capacity) {
            throw std::runtime_error("guest vsprintf result grew during formatting");
        }
        const auto written = result < 0 ? output_capacity :
            std::min<std::uint64_t>(output_capacity,
                static_cast<std::uint64_t>(result) + 1);
        for (std::uint64_t index = 0; index < written; ++index) {
            memory.write8(state.x[1] + index,
                          static_cast<std::uint8_t>(output[index]));
        }
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll == "api-ms-win-crt-stdio-l1-1-0.dll" &&
        (function == "__stdio_common_vswprintf" ||
         function == "__stdio_common_vswprintf_s" ||
         function == "__stdio_common_vsnwprintf_s")) {
        static_assert(sizeof(wchar_t) == 2 && std::is_pointer_v<va_list>);
        const bool bounded = function == "__stdio_common_vsnwprintf_s";
        const auto capacity = state.x[2]; // UTF-16 code units, not bytes.
        const bool unbounded_vswprintf =
            function == "__stdio_common_vswprintf" && capacity == UINT64_MAX;
        if (!unbounded_vswprintf && capacity > 1024ULL * 1024ULL) {
            throw std::runtime_error("guest wide formatted output buffer is too large");
        }
        const auto format = guest_string(memory, state.x[bounded ? 4 : 3], true);
        // The shared marshaller rejects %n, so the count pass cannot replay
        // a guest-visible side effect. In wide printf, %s is UTF-16 and %S ANSI.
        const auto syntax = printf_format_syntax(format);
        auto arguments = marshal_printf_arguments(
            memory, syntax, state.x[bounded ? 6 : 5], true,
            (state.x[0] & _CRT_INTERNAL_PRINTF_LEGACY_WIDE_SPECIFIERS) != 0);
        _locale_t locale = nullptr;
        const auto guest_locale = state.x[bounded ? 5 : 4];
        if (guest_locale != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->locale_objects.find(guest_locale);
            if (found == state_->locale_objects.end()) {
                throw std::runtime_error("invalid guest wide printf locale");
            }
            locale = found->second.host;
        }
        const auto arglist = reinterpret_cast<va_list>(arguments.slots.data());
        std::uint64_t output_capacity = capacity;
        if (unbounded_vswprintf) {
            constexpr auto legacy_termination =
                _CRT_INTERNAL_PRINTF_LEGACY_VSPRINTF_NULL_TERMINATION;
            constexpr auto standard_count =
                _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR;
            const auto count_options =
                (state.x[0] & ~legacy_termination) | standard_count;
            ScopedHostCrtParameterGuard guard;
            const auto count = __stdio_common_vswprintf(
                count_options, nullptr, 0, format.c_str(), locale, arglist);
            if (count < 0) {
                state.x[0] = static_cast<std::uint32_t>(-1);
                return std::nullopt;
            }
            output_capacity = static_cast<std::uint64_t>(count) + 1;
        }
        if (output_capacity > UINT64_MAX / sizeof(wchar_t) ||
            (output_capacity != 0 && !guest_range(
                memory, state.x[1], output_capacity * sizeof(wchar_t), true))) {
            throw std::runtime_error(
                "guest wide formatted result exceeds writable output memory");
        }
        std::vector<wchar_t> output(static_cast<std::size_t>(
            std::max<std::uint64_t>(output_capacity, 1)));
        auto* buffer = output_capacity == 0 ? nullptr : output.data();
        ScopedHostCrtParameterGuard guard;
        const auto result = bounded
            ? __stdio_common_vsnwprintf_s(state.x[0], buffer,
                static_cast<std::size_t>(output_capacity),
                static_cast<std::size_t>(state.x[3]), format.c_str(),
                locale, arglist)
            : function == "__stdio_common_vswprintf_s"
                ? __stdio_common_vswprintf_s(state.x[0], buffer,
                    static_cast<std::size_t>(output_capacity), format.c_str(),
                    locale, arglist)
                : __stdio_common_vswprintf(state.x[0], buffer,
                    static_cast<std::size_t>(output_capacity), format.c_str(),
                    locale, arglist);
        if (unbounded_vswprintf && result >= 0 &&
            static_cast<std::uint64_t>(result) >= output_capacity) {
            throw std::runtime_error(
                "guest wide formatted result grew during formatting");
        }
        const auto written = result < 0 ? output_capacity :
            std::min<std::uint64_t>(output_capacity,
                static_cast<std::uint64_t>(result) + 1);
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(output.data());
        for (std::uint64_t index = 0; index < written * sizeof(wchar_t); ++index) {
            memory.write8(state.x[1] + index, bytes[index]);
        }
        state.x[0] = static_cast<std::uint32_t>(result);
        return std::nullopt;
    }
    if (dll != "kernel32.dll" && dll != "kernelbase.dll") {
        return dispatch_win32_import(imported, state, memory);
    }

    if (function == "GetTimeZoneInformation" ||
        function == "GetSystemTime" || function == "GetLocalTime" ||
        function == "GetSystemTimeAdjustment" ||
        function == "GetSystemTimeAsFileTime" ||
        function == "GetSystemTimePreciseAsFileTime" ||
        function == "FileTimeToSystemTime" ||
        function == "SystemTimeToFileTime" ||
        function == "SystemTimeToTzSpecificLocalTime" ||
        function == "GetTickCount" || function == "GetTickCount64") {
        static_assert(sizeof(FILETIME) == 8);
        static_assert(sizeof(SYSTEMTIME) == 16);
        static_assert(sizeof(TIME_ZONE_INFORMATION) == 172);
        static_assert(offsetof(TIME_ZONE_INFORMATION, StandardName) == 4);
        static_assert(offsetof(TIME_ZONE_INFORMATION, StandardDate) == 68);
        static_assert(offsetof(TIME_ZONE_INFORMATION, StandardBias) == 84);
        static_assert(offsetof(TIME_ZONE_INFORMATION, DaylightName) == 88);
        static_assert(offsetof(TIME_ZONE_INFORMATION, DaylightDate) == 152);
        static_assert(offsetof(TIME_ZONE_INFORMATION, DaylightBias) == 168);
        const auto read_struct = [&]<typename T>(std::uint64_t address,
                                                  T& value) {
            if (!guest_range(memory, address, sizeof(T), false)) return false;
            auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
            for (std::size_t index = 0; index < sizeof(T); ++index) {
                bytes[index] = memory.read8(address + index);
            }
            return true;
        };
        const auto write_struct = [&]<typename T>(std::uint64_t address,
                                                   const T& value) {
            if (!guest_range(memory, address, sizeof(T), true)) return false;
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
            for (std::size_t index = 0; index < sizeof(T); ++index) {
                memory.write8(address + index, bytes[index]);
            }
            return true;
        };
        if (function == "GetTickCount" || function == "GetTickCount64") {
            state.x[0] = function == "GetTickCount"
                ? static_cast<std::uint64_t>(::GetTickCount())
                : ::GetTickCount64();
            return std::nullopt;
        }
        if (function == "GetTimeZoneInformation") {
            if (!guest_range(memory, state.x[0],
                             sizeof(TIME_ZONE_INFORMATION), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = TIME_ZONE_ID_INVALID;
                return std::nullopt;
            }
            const auto output = state.x[0];
            TIME_ZONE_INFORMATION zone{};
            const auto result = ::GetTimeZoneInformation(&zone);
            if (result != TIME_ZONE_ID_INVALID) {
                static_cast<void>(write_struct(output, zone));
            }
            state.x[0] = result;
            return std::nullopt;
        }
        if (function == "GetSystemTime" || function == "GetLocalTime") {
            SYSTEMTIME value{};
            if (function == "GetSystemTime") ::GetSystemTime(&value);
            else ::GetLocalTime(&value);
            if (!write_struct(state.x[0], value)) {
                throw std::runtime_error("invalid guest SYSTEMTIME output");
            }
            return std::nullopt;
        }
        if (function == "GetSystemTimeAsFileTime" ||
            function == "GetSystemTimePreciseAsFileTime") {
            FILETIME value{};
            if (function == "GetSystemTimeAsFileTime") {
                ::GetSystemTimeAsFileTime(&value);
            } else {
                ::GetSystemTimePreciseAsFileTime(&value);
            }
            if (!write_struct(state.x[0], value)) {
                throw std::runtime_error("invalid guest FILETIME output");
            }
            return std::nullopt;
        }
        if (function == "GetSystemTimeAdjustment") {
            if (!guest_range(memory, state.x[0], 4, true) ||
                !guest_range(memory, state.x[1], 4, true) ||
                !guest_range(memory, state.x[2], 4, true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = FALSE;
                return std::nullopt;
            }
            const auto adjustment_address = state.x[0];
            const auto increment_address = state.x[1];
            const auto disabled_address = state.x[2];
            DWORD adjustment = 0;
            DWORD increment = 0;
            BOOL disabled = FALSE;
            const auto success = ::GetSystemTimeAdjustment(
                &adjustment, &increment, &disabled);
            if (success) {
                memory.write32(adjustment_address, adjustment);
                memory.write32(increment_address, increment);
                memory.write32(disabled_address, static_cast<std::uint32_t>(disabled));
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (function == "FileTimeToSystemTime") {
            FILETIME input{};
            if (!read_struct(state.x[0], input) ||
                !guest_range(memory, state.x[1], sizeof(SYSTEMTIME), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = FALSE;
                return std::nullopt;
            }
            const auto output = state.x[1];
            SYSTEMTIME converted{};
            const auto success = ::FileTimeToSystemTime(&input, &converted);
            if (success) static_cast<void>(write_struct(output, converted));
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (function == "SystemTimeToFileTime") {
            SYSTEMTIME input{};
            if (!read_struct(state.x[0], input) ||
                !guest_range(memory, state.x[1], sizeof(FILETIME), true)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = FALSE;
                return std::nullopt;
            }
            const auto output = state.x[1];
            FILETIME converted{};
            const auto success = ::SystemTimeToFileTime(&input, &converted);
            if (success) static_cast<void>(write_struct(output, converted));
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        TIME_ZONE_INFORMATION zone{};
        SYSTEMTIME input{};
        if ((state.x[0] != 0 && !read_struct(state.x[0], zone)) ||
            !read_struct(state.x[1], input) ||
            !guest_range(memory, state.x[2], sizeof(SYSTEMTIME), true)) {
            SetLastError(ERROR_NOACCESS);
            state.x[0] = FALSE;
            return std::nullopt;
        }
        const auto output = state.x[2];
        SYSTEMTIME converted{};
        const auto success = ::SystemTimeToTzSpecificLocalTime(
            state.x[0] == 0 ? nullptr : &zone, &input, &converted);
        if (success) static_cast<void>(write_struct(output, converted));
        state.x[0] = static_cast<std::uint32_t>(success);
        return std::nullopt;
    }

    if (function == "FormatMessageW" || function == "FormatMessageA") {
        const bool wide = function == "FormatMessageW";
        const auto flags = static_cast<DWORD>(state.x[0]);
        const auto source_address = state.x[1];
        const auto message_id = static_cast<DWORD>(state.x[2]);
        const auto language_id = static_cast<DWORD>(state.x[3]);
        const auto output_address = state.x[4];
        const auto capacity = static_cast<DWORD>(state.x[5]);
        const auto guest_arguments = state.x[6];
        constexpr DWORD supported = FORMAT_MESSAGE_MAX_WIDTH_MASK |
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS |
            FORMAT_MESSAGE_FROM_STRING | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_ARGUMENT_ARRAY;
        const bool allocate = (flags & FORMAT_MESSAGE_ALLOCATE_BUFFER) != 0;
        const bool from_string = (flags & FORMAT_MESSAGE_FROM_STRING) != 0;
        const bool from_system = (flags & FORMAT_MESSAGE_FROM_SYSTEM) != 0;
        if (state.x[0] != flags || state.x[2] != message_id ||
            state.x[3] != language_id || state.x[5] != capacity ||
            (flags & ~supported) != 0 || from_string == from_system ||
            (flags & FORMAT_MESSAGE_FROM_HMODULE) != 0 ||
            (allocate ? !guest_range(memory, output_address, 8, true)
                      : capacity == 0 ||
                        !guest_range(memory, output_address,
                                     static_cast<std::uint64_t>(capacity) *
                                         (wide ? 2ULL : 1ULL), true))) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        if (allocate) memory.write64(output_address, 0);
        std::wstring wide_source;
        std::string ansi_source;
        if (from_string) {
            if (!guest_range(memory, source_address, wide ? 2 : 1, false)) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            try {
                if (wide) wide_source = guest_string(memory, source_address, true);
                else ansi_source = guest_ansi_string(memory, source_address);
            } catch (const std::runtime_error&) {
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
        }
        const auto host_source = from_string
            ? static_cast<LPCVOID>(wide
                  ? static_cast<const void*>(wide_source.c_str())
                  : static_cast<const void*>(ansi_source.c_str()))
            : nullptr;
        const auto format_host = [&](DWORD call_flags, va_list* arguments,
                                     LPWSTR& result_w, LPSTR& result_a) {
            if (wide) {
                return ::FormatMessageW(call_flags, host_source, message_id,
                    language_id, reinterpret_cast<LPWSTR>(&result_w),
                    allocate ? capacity : 0, arguments);
            }
            return ::FormatMessageA(call_flags, host_source, message_id,
                language_id, reinterpret_cast<LPSTR>(&result_a),
                allocate ? capacity : 0, arguments);
        };
        LPWSTR result_w = nullptr;
        LPSTR result_a = nullptr;
        auto length = format_host(flags | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                      FORMAT_MESSAGE_IGNORE_INSERTS,
                                  nullptr, result_w, result_a);
        if (length == 0) {
            state.x[0] = 0;
            return std::nullopt;
        }
        auto release_host = [&] {
            if (wide) ::LocalFree(result_w);
            else ::LocalFree(result_a);
            result_w = nullptr;
            result_a = nullptr;
        };
        std::array<unsigned char, 100> insert_types{};
        std::size_t highest_insert = 0;
        bool unsupported_insert = false;
        if ((flags & FORMAT_MESSAGE_IGNORE_INSERTS) == 0) {
            std::wstring template_text;
            if (wide) template_text.assign(result_w, length);
            else {
                template_text.reserve(length);
                for (DWORD index = 0; index < length; ++index) {
                    template_text.push_back(
                        static_cast<unsigned char>(result_a[index]));
                }
            }
            for (std::size_t index = 0; index < template_text.size(); ++index) {
                if (template_text[index] != L'%' ||
                    index + 1 == template_text.size()) continue;
                if (template_text[index + 1] == L'%') {
                    ++index;
                    continue;
                }
                if (template_text[index + 1] < L'1' ||
                    template_text[index + 1] > L'9') continue;
                std::size_t insert = static_cast<std::size_t>(
                    template_text[++index] - L'0');
                if (index + 1 < template_text.size() &&
                    template_text[index + 1] >= L'0' &&
                    template_text[index + 1] <= L'9') {
                    insert = insert * 10 + static_cast<std::size_t>(
                        template_text[++index] - L'0');
                }
                if (insert >= insert_types.size() ||
                    (index + 1 < template_text.size() &&
                     template_text[index + 1] >= L'0' &&
                     template_text[index + 1] <= L'9')) {
                    unsupported_insert = true;
                    break;
                }
                unsigned char kind = 1;
                if (index + 1 < template_text.size() &&
                    template_text[index + 1] == L'!') {
                    const auto end = template_text.find(L'!', index + 2);
                    if (end == std::wstring::npos) {
                        unsupported_insert = true;
                        break;
                    }
                    const auto specifier = std::wstring_view(template_text).substr(
                        index + 2, end - index - 2);
                    if (specifier == L"d" || specifier == L"i" ||
                        specifier == L"u" || specifier == L"x" ||
                        specifier == L"X") {
                        kind = 2;
                    } else if (specifier != L"s") {
                        unsupported_insert = true;
                        break;
                    }
                    index = end;
                }
                if (insert_types[insert] != 0 &&
                    insert_types[insert] != kind) {
                    unsupported_insert = true;
                    break;
                }
                insert_types[insert] = kind;
                highest_insert = std::max(highest_insert, insert);
            }
        }
        if (unsupported_insert) {
            release_host();
            SetLastError(ERROR_NOT_SUPPORTED);
            state.x[0] = 0;
            return std::nullopt;
        }
        if (highest_insert != 0) {
            std::uint64_t arguments_address = guest_arguments;
            if ((flags & FORMAT_MESSAGE_ARGUMENT_ARRAY) == 0) {
                if (!guest_range(memory, arguments_address, 8, false)) {
                    release_host();
                    SetLastError(ERROR_NOACCESS);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                arguments_address = memory.read64(arguments_address);
            }
            if (!guest_range(memory, arguments_address,
                             highest_insert * 8, false)) {
                release_host();
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::array<ULONG_PTR, 99> native_arguments{};
            std::array<std::wstring, 100> wide_inserts{};
            std::array<std::string, 100> ansi_inserts{};
            for (std::size_t index = 1; index <= highest_insert; ++index) {
                const auto guest_value = memory.read64(
                    arguments_address + (index - 1) * 8);
                if (insert_types[index] != 1) {
                    native_arguments[index - 1] =
                        static_cast<ULONG_PTR>(guest_value);
                    continue;
                }
                if (!guest_range(memory, guest_value, wide ? 2 : 1, false)) {
                    unsupported_insert = true;
                    break;
                }
                try {
                    if (wide) {
                        wide_inserts[index] = guest_string(memory, guest_value, true);
                        native_arguments[index - 1] = reinterpret_cast<ULONG_PTR>(
                            wide_inserts[index].c_str());
                    } else {
                        ansi_inserts[index] = guest_ansi_string(memory, guest_value);
                        native_arguments[index - 1] = reinterpret_cast<ULONG_PTR>(
                            ansi_inserts[index].c_str());
                    }
                } catch (const std::runtime_error&) {
                    unsupported_insert = true;
                    break;
                }
            }
            if (unsupported_insert) {
                release_host();
                SetLastError(ERROR_NOACCESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            release_host();
            length = format_host((flags | FORMAT_MESSAGE_ALLOCATE_BUFFER |
                                  FORMAT_MESSAGE_ARGUMENT_ARRAY) &
                                     ~FORMAT_MESSAGE_IGNORE_INSERTS,
                reinterpret_cast<va_list*>(native_arguments.data()),
                result_w, result_a);
            if (length == 0) {
                state.x[0] = 0;
                return std::nullopt;
            }
        }
        const auto host_error = GetLastError();
        const auto units = static_cast<std::uint64_t>(length) + 1;
        if (!allocate && units > capacity) {
            release_host();
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            state.x[0] = 0;
            return std::nullopt;
        }
        std::uint64_t target = output_address;
        if (allocate) {
            CpuState allocation{};
            allocation.x[0] = LMEM_ZEROINIT;
            allocation.x[1] = std::max<std::uint64_t>(units, capacity) *
                              (wide ? 2ULL : 1ULL);
            static_cast<void>(dispatch({"kernel32.dll", "LocalAlloc", 0},
                                       allocation, memory));
            target = allocation.x[0];
            if (target == 0) {
                release_host();
                state.x[0] = 0;
                return std::nullopt;
            }
        }
        for (std::uint64_t index = 0; index < units; ++index) {
            const auto character = wide
                ? static_cast<std::uint16_t>(result_w[index])
                : static_cast<std::uint8_t>(result_a[index]);
            memory.write8(target + index * (wide ? 2ULL : 1ULL),
                          static_cast<std::uint8_t>(character));
            if (wide) {
                memory.write8(target + index * 2 + 1,
                              static_cast<std::uint8_t>(character >> 8));
            }
        }
        if (allocate) memory.write64(output_address, target);
        release_host();
        SetLastError(host_error);
        state.x[0] = length;
        return std::nullopt;
    }

    if (function == "VirtualAlloc" || function == "VirtualFree" ||
        function == "VirtualProtect" || function == "VirtualQuery") {
        std::lock_guard lock(state_->mutex);
        auto containing_allocation = [this](std::uint64_t address) {
            auto entry = state_->virtual_allocations.upper_bound(address);
            if (entry == state_->virtual_allocations.begin()) {
                return state_->virtual_allocations.end();
            }
            --entry;
            if (address - entry->first >= entry->second.size) {
                return state_->virtual_allocations.end();
            }
            return entry;
        };
        if (function == "VirtualAlloc") {
            const auto requested = state.x[0];
            const auto size = state.x[1];
            const auto type = static_cast<DWORD>(state.x[2]);
            const auto protection = static_cast<DWORD>(state.x[3]);
            const bool reserve = (type & MEM_RESERVE) != 0 ||
                                 ((type & MEM_COMMIT) != 0 && requested == 0);
            const bool commit = (type & MEM_COMMIT) != 0;
            if (size == 0 || !supported_page_protection(protection) ||
                (type & ~(MEM_RESERVE | MEM_COMMIT)) != 0 ||
                (!reserve && !commit) || requested > UINT64_MAX - size) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            std::uint64_t base = requested;
            const auto requested_end = align_up_page(requested + size);
            if (!requested_end) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto end = reserve && requested != 0
                ? *requested_end : align_up_page(size).value_or(0);
            if (end == 0) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (reserve) {
                if (requested != 0) {
                    base &= ~(guest_allocation_granularity - 1);
                } else {
                    base = (state_->next_virtual_address +
                            guest_allocation_granularity - 1) &
                           ~(guest_allocation_granularity - 1);
                }
                const auto reserved_size = requested == 0 ? end : end - base;
                auto available = [this, &memory](std::uint64_t candidate,
                                                   std::uint64_t length) {
                    if (!memory.is_range_available(candidate, length)) return false;
                    const auto next = state_->virtual_allocations.lower_bound(candidate);
                    return (next == state_->virtual_allocations.end() ||
                            candidate + length <= next->first) &&
                           (next == state_->virtual_allocations.begin() ||
                            std::prev(next)->first +
                                std::prev(next)->second.size <= candidate);
                };
                if (requested == 0) {
                    while (base <= UINT64_MAX - reserved_size &&
                           !available(base, reserved_size)) {
                        if (base > virtual_memory_base + (1ULL << 44)) break;
                        base += guest_allocation_granularity;
                    }
                }
                if (base > UINT64_MAX - reserved_size ||
                    !available(base, reserved_size)) {
                    SetLastError(ERROR_INVALID_ADDRESS);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                state_->virtual_allocations.emplace(
                    base, State::VirtualAllocation{reserved_size, protection, {}});
                if (requested == 0) {
                    state_->next_virtual_address =
                        (base + reserved_size + guest_allocation_granularity - 1) &
                        ~(guest_allocation_granularity - 1);
                }
            } else {
                base &= ~(guest_page_size - 1);
            }
            if (commit) {
                const auto commit_end = reserve
                    ? base + state_->virtual_allocations.at(base).size
                    : *requested_end;
                auto allocation = containing_allocation(base);
                if (allocation == state_->virtual_allocations.end() ||
                    commit_end > allocation->first + allocation->second.size ||
                    commit_end - base > 1024ULL * 1024ULL * 1024ULL) {
                    if (reserve) state_->virtual_allocations.erase(base);
                    SetLastError(ERROR_INVALID_ADDRESS);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                std::vector<std::uint64_t> new_pages;
                try {
                    for (auto page = base; page < commit_end; page += guest_page_size) {
                        if (allocation->second.committed_pages.contains(page)) continue;
                        if (!memory.is_range_available(page, guest_page_size)) {
                            throw std::runtime_error("guest virtual page is occupied");
                        }
                        memory.map_memory(page, guest_page_size);
                        new_pages.push_back(page);
                        apply_guest_page_protection(memory, page, protection);
                        allocation->second.committed_pages.emplace(page, protection);
                    }
                } catch (const std::exception&) {
                    for (const auto page : new_pages) {
                        allocation->second.committed_pages.erase(page);
                        memory.unmap_memory(page);
                    }
                    if (reserve) state_->virtual_allocations.erase(base);
                    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
                    state.x[0] = 0;
                    return std::nullopt;
                }
            }
            state.x[0] = base;
            return std::nullopt;
        }
        if (function == "VirtualFree") {
            const auto address = state.x[0];
            const auto size = state.x[1];
            const auto type = static_cast<DWORD>(state.x[2]);
            auto allocation = containing_allocation(address);
            if (allocation == state_->virtual_allocations.end() ||
                (type != MEM_RELEASE && type != MEM_DECOMMIT)) {
                SetLastError(ERROR_INVALID_ADDRESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (type == MEM_RELEASE) {
                if (address != allocation->first || size != 0) {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                for (const auto& [page, protection] : allocation->second.committed_pages) {
                    static_cast<void>(protection);
                    memory.unmap_memory(page);
                }
                state_->virtual_allocations.erase(allocation);
            } else {
                if (size == 0 || address > UINT64_MAX - size ||
                    !align_up_page(address + size)) {
                    SetLastError(ERROR_INVALID_PARAMETER);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                const auto base = address & ~(guest_page_size - 1);
                const auto end = *align_up_page(address + size);
                if (end > allocation->first + allocation->second.size) {
                    SetLastError(ERROR_INVALID_ADDRESS);
                    state.x[0] = 0;
                    return std::nullopt;
                }
                for (auto page = base; page < end; page += guest_page_size) {
                    if (allocation->second.committed_pages.erase(page) != 0) {
                        memory.unmap_memory(page);
                    }
                }
            }
            state.x[0] = 1;
            return std::nullopt;
        }
        if (function == "VirtualProtect") {
            const auto address = state.x[0];
            const auto size = state.x[1];
            const auto protection = static_cast<DWORD>(state.x[2]);
            const auto previous = state.x[3];
            if (size == 0 || address > UINT64_MAX - size ||
                !supported_page_protection(protection) ||
                !align_up_page(address + size)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto base = address & ~(guest_page_size - 1);
            const auto end = *align_up_page(address + size);
            auto allocation = containing_allocation(base);
            if (allocation == state_->virtual_allocations.end() ||
                end > allocation->first + allocation->second.size) {
                SetLastError(ERROR_INVALID_ADDRESS);
                state.x[0] = 0;
                return std::nullopt;
            }
            for (auto page = base; page < end; page += guest_page_size) {
                if (!allocation->second.committed_pages.contains(page)) {
                    SetLastError(ERROR_INVALID_ADDRESS);
                    state.x[0] = 0;
                    return std::nullopt;
                }
            }
            memory.write32(previous,
                           allocation->second.committed_pages.at(base));
            for (auto page = base; page < end; page += guest_page_size) {
                apply_guest_page_protection(memory, page, protection);
                allocation->second.committed_pages.at(page) = protection;
            }
            state.x[0] = 1;
            return std::nullopt;
        }
        const auto page = state.x[0] & ~(guest_page_size - 1);
        const auto output = state.x[1];
        if (state.x[2] < sizeof(MEMORY_BASIC_INFORMATION)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 48);
        std::uint64_t allocation_base = 0;
        std::uint64_t region_size = guest_page_size;
        DWORD allocation_protect = 0;
        DWORD state_value = MEM_FREE;
        DWORD protection = 0;
        DWORD type = 0;
        const auto allocation = containing_allocation(page);
        if (allocation != state_->virtual_allocations.end()) {
            allocation_base = allocation->first;
            allocation_protect = allocation->second.allocation_protect;
            type = MEM_PRIVATE;
            const auto first = allocation->second.committed_pages.find(page);
            if (first == allocation->second.committed_pages.end()) {
                state_value = MEM_RESERVE;
                for (auto next = page + guest_page_size;
                     next < allocation->first + allocation->second.size;
                     next += guest_page_size) {
                    if (allocation->second.committed_pages.contains(next)) break;
                    region_size += guest_page_size;
                }
            } else {
                state_value = MEM_COMMIT;
                protection = first->second;
                for (auto next = std::next(first);
                     next != allocation->second.committed_pages.end() &&
                     next->first == page + region_size &&
                     next->second == protection; ++next) {
                    region_size += guest_page_size;
                }
            }
        } else if (const auto region = memory.query_region(page)) {
            allocation_base = region->base;
            region_size = region->base + region->size - page;
            state_value = MEM_COMMIT;
            protection = region->executable
                ? (region->writable ? PAGE_EXECUTE_READWRITE :
                   region->readable ? PAGE_EXECUTE_READ : PAGE_EXECUTE)
                : (region->writable ? PAGE_READWRITE :
                   region->readable ? PAGE_READONLY : PAGE_NOACCESS);
            allocation_protect = protection;
            type = region->image ? MEM_IMAGE :
                   region->mapped ? MEM_MAPPED : MEM_PRIVATE;
        }
        for (std::uint64_t offset = 0; offset < 48; ++offset) {
            memory.write8(output + offset, 0);
        }
        memory.write64(output, page);
        memory.write64(output + 8, allocation_base);
        memory.write32(output + 16, allocation_protect);
        memory.write64(output + 24, region_size);
        memory.write32(output + 32, state_value);
        memory.write32(output + 36, protection);
        memory.write32(output + 40, type);
        state.x[0] = sizeof(MEMORY_BASIC_INFORMATION);
        return std::nullopt;
    }

    if (function == "TlsAlloc") {
        std::lock_guard lock(state_->mutex);
        if (state_->next_tls_index >= 1024) {
            SetLastError(ERROR_NO_MORE_ITEMS);
            state.x[0] = TLS_OUT_OF_INDEXES;
        } else {
            const auto index = state_->next_tls_index++;
            state_->tls_slots.insert(index);
            state.x[0] = index;
        }
        return std::nullopt;
    }
    if (function == "LocalAlloc") {
        const auto flags = static_cast<UINT>(state.x[0]);
        const auto requested = state.x[1];
        if (state.x[0] != flags || (flags & ~LMEM_ZEROINIT) != 0 ||
            requested > 1024ULL * 1024ULL * 1024ULL) {
            SetLastError(ERROR_INVALID_PARAMETER);
            state.x[0] = 0;
            return std::nullopt;
        }
        const auto size = static_cast<std::size_t>((std::max(requested, 1ULL) + 15) &
                                                   ~15ULL);
        std::lock_guard lock(state_->mutex);
        const auto address = state_->next_local_address;
        if (address > UINT64_MAX - size) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            state.x[0] = 0;
            return std::nullopt;
        }
        try {
            memory.map_memory(address, size);
        } catch (const std::bad_alloc&) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            state.x[0] = 0;
            return std::nullopt;
        }
        state_->next_local_address += size;
        state_->local_allocations.emplace(address, size);
        state.x[0] = address;
        return std::nullopt;
    }
    if (function == "LocalFree" || function == "LocalSize") {
        const auto address = state.x[0];
        if (address == 0 && function == "LocalFree") return std::nullopt;
        std::lock_guard lock(state_->mutex);
        const auto found = state_->local_allocations.find(address);
        if (found == state_->local_allocations.end()) {
            SetLastError(ERROR_INVALID_HANDLE);
            if (function == "LocalSize") state.x[0] = 0;
            return std::nullopt;
        }
        if (function == "LocalSize") {
            state.x[0] = found->second;
        } else {
            memory.unmap_memory(address);
            state_->local_allocations.erase(found);
            state.x[0] = 0;
        }
        return std::nullopt;
    }
    if (function == "DisableThreadLibraryCalls") {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->guest_modules.find(state.x[0]);
        if (found == state_->guest_modules.end() || !found->second->is_dll() ||
            found->second->tls()) {
            SetLastError(ERROR_INVALID_HANDLE);
            state.x[0] = 0;
        } else {
            state_->thread_notifications_disabled.insert(state.x[0]);
            state.x[0] = 1;
        }
        return std::nullopt;
    }
    if (function == "TlsFree" || function == "TlsGetValue" ||
        function == "TlsSetValue") {
        const auto index = static_cast<DWORD>(state.x[0]);
        {
            std::lock_guard lock(state_->mutex);
            if (state.x[0] != index || !state_->tls_slots.contains(index)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (function == "TlsFree") {
                state_->tls_slots.erase(index);
                state_->tls_values.erase(index);
            }
        }
        if (function == "TlsFree") {
            state.x[0] = 1;
        } else if (function == "TlsGetValue") {
            std::lock_guard lock(state_->mutex);
            state.x[0] = state_->tls_values[index][GetCurrentThreadId()];
            SetLastError(ERROR_SUCCESS);
        } else {
            std::lock_guard lock(state_->mutex);
            state_->tls_values[index][GetCurrentThreadId()] = state.x[1];
            state.x[0] = 1;
        }
        return std::nullopt;
    }

    if (function == "InitializeSRWLock" ||
        function == "AcquireSRWLockExclusive" ||
        function == "AcquireSRWLockShared" ||
        function == "TryAcquireSRWLockExclusive" ||
        function == "TryAcquireSRWLockShared" ||
        function == "ReleaseSRWLockExclusive" ||
        function == "ReleaseSRWLockShared") {
        const auto address = state.x[0];
        static_cast<void>(memory.read64(address));
        std::shared_ptr<State::SlimLock> lock;
        {
            std::lock_guard guard(state_->mutex);
            auto& entry = state_->slim_locks[address];
            if (!entry || function == "InitializeSRWLock") {
                entry = std::make_shared<State::SlimLock>();
            }
            lock = entry;
        }
        if (function == "InitializeSRWLock") {
            memory.write64(address, 0);
        } else if (function == "AcquireSRWLockExclusive") {
            AcquireSRWLockExclusive(&lock->value);
        } else if (function == "AcquireSRWLockShared") {
            AcquireSRWLockShared(&lock->value);
        } else if (function == "TryAcquireSRWLockExclusive") {
            state.x[0] = static_cast<std::uint32_t>(
                TryAcquireSRWLockExclusive(&lock->value));
        } else if (function == "TryAcquireSRWLockShared") {
            state.x[0] = static_cast<std::uint32_t>(
                TryAcquireSRWLockShared(&lock->value));
        } else if (function == "ReleaseSRWLockExclusive") {
            ReleaseSRWLockExclusive(&lock->value);
        } else {
            ReleaseSRWLockShared(&lock->value);
        }
        return std::nullopt;
    }
    if (function == "InitializeConditionVariable" ||
        function == "WakeConditionVariable" ||
        function == "WakeAllConditionVariable" ||
        function == "SleepConditionVariableSRW") {
        const auto address = state.x[0];
        static_cast<void>(memory.read64(address));
        std::shared_ptr<State::Condition> condition;
        std::shared_ptr<State::SlimLock> lock;
        {
            std::lock_guard guard(state_->mutex);
            auto& entry = state_->conditions[address];
            if (!entry || function == "InitializeConditionVariable") {
                entry = std::make_shared<State::Condition>();
            }
            condition = entry;
            if (function == "SleepConditionVariableSRW") {
                static_cast<void>(memory.read64(state.x[1]));
                auto& lock_entry = state_->slim_locks[state.x[1]];
                if (!lock_entry) lock_entry = std::make_shared<State::SlimLock>();
                lock = lock_entry;
            }
        }
        if (function == "InitializeConditionVariable") {
            memory.write64(address, 0);
        } else if (function == "WakeConditionVariable") {
            WakeConditionVariable(&condition->value);
        } else if (function == "WakeAllConditionVariable") {
            WakeAllConditionVariable(&condition->value);
        } else {
            const auto success = SleepConditionVariableSRW(
                &condition->value, &lock->value,
                static_cast<DWORD>(state.x[2]), static_cast<ULONG>(state.x[3]));
            state.x[0] = static_cast<std::uint32_t>(success);
        }
        return std::nullopt;
    }
    if (function == "InitOnceInitialize" ||
        function == "InitOnceBeginInitialize" ||
        function == "InitOnceComplete") {
        const auto address = state.x[0];
        static_cast<void>(memory.read64(address));
        std::shared_ptr<State::Once> once;
        {
            std::lock_guard lock(state_->mutex);
            auto& entry = state_->once_objects[address];
            if (!entry || function == "InitOnceInitialize") {
                entry = std::make_shared<State::Once>();
            }
            once = entry;
        }
        if (function == "InitOnceInitialize") {
            memory.write64(address, 0);
            return std::nullopt;
        }
        if (function == "InitOnceBeginInitialize") {
            BOOL pending = FALSE;
            void* context = nullptr;
            const auto success = InitOnceBeginInitialize(
                &once->value, static_cast<DWORD>(state.x[1]), &pending,
                state.x[3] != 0 ? &context : nullptr);
            if (success) {
                memory.write32(state.x[2], static_cast<std::uint32_t>(pending));
                if (state.x[3] != 0) {
                    memory.write64(state.x[3],
                                   reinterpret_cast<std::uintptr_t>(context));
                }
                if (pending) memory.write64(address, 1);
            }
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        const auto success = InitOnceComplete(
            &once->value, static_cast<DWORD>(state.x[1]),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(state.x[2])));
        if (success) memory.write64(address, 2);
        state.x[0] = static_cast<std::uint32_t>(success);
        return std::nullopt;
    }

    if (function == "InitializeSListHead" ||
        function == "InterlockedPushEntrySList" ||
        function == "InterlockedPopEntrySList" ||
        function == "InterlockedFlushSList" ||
        function == "QueryDepthSList") {
        const auto address = state.x[0];
        if ((address & 15U) != 0) {
            throw std::runtime_error("guest SList header is not 16-byte aligned");
        }
        std::lock_guard lock(state_->mutex);
        if (function == "InitializeSListHead") {
            for (std::uint64_t offset = 0; offset < 16; ++offset) {
                memory.write8(address + offset, 0);
            }
            state_->slists[address] = {};
            return std::nullopt;
        }
        const auto found = state_->slists.find(address);
        if (found == state_->slists.end()) {
            throw std::runtime_error("guest SList header is not initialized");
        }
        auto& list = found->second;
        if (function == "InterlockedPushEntrySList") {
            const auto entry = state.x[1];
            if ((entry & 15U) != 0) {
                throw std::runtime_error("guest SList entry is not 16-byte aligned");
            }
            memory.write64(entry, list.first);
            state.x[0] = list.first;
            list.first = entry;
            ++list.depth;
            return std::nullopt;
        }
        if (function == "QueryDepthSList") {
            state.x[0] = list.depth;
            return std::nullopt;
        }
        state.x[0] = list.first;
        if (function == "InterlockedPopEntrySList" && list.first != 0) {
            list.first = memory.read64(list.first);
            --list.depth;
        } else if (function == "InterlockedFlushSList") {
            list.first = 0;
            list.depth = 0;
        }
        return std::nullopt;
    }

    if (function == "FlsAlloc") {
        std::lock_guard lock(state_->mutex);
        for (DWORD index = 0; index < FLS_MAXIMUM_AVAILABLE; ++index) {
            if (!state_->fls_slots.contains(index)) {
                state_->fls_slots.emplace(index, State::FlsSlot{state.x[0], {}});
                state.x[0] = index;
                return std::nullopt;
            }
        }
        state.x[0] = FLS_OUT_OF_INDEXES;
        SetLastError(ERROR_NO_MORE_ITEMS);
        return std::nullopt;
    }
    if (function == "FlsGetValue" || function == "FlsSetValue" ||
        function == "FlsFree") {
        const auto index = static_cast<DWORD>(state.x[0]);
        std::uint64_t callback = 0;
        std::uint64_t value = 0;
        std::function<std::uint64_t(std::uint64_t, std::uint64_t)> invoke;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->fls_slots.find(index);
            if (found == state_->fls_slots.end()) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            auto& slot = found->second;
            const auto thread = GetCurrentThreadId();
            if (function == "FlsGetValue") {
                const auto stored = slot.values.find(thread);
                state.x[0] = stored == slot.values.end() ? 0 : stored->second;
                SetLastError(ERROR_SUCCESS);
                return std::nullopt;
            }
            if (function == "FlsSetValue") {
                slot.values[thread] = state.x[1];
                state.x[0] = 1;
                return std::nullopt;
            }
            for (const auto& [owner, stored] : slot.values) {
                if (owner != thread && stored != 0 && slot.callback != 0) {
                    throw std::runtime_error("multi-thread guest FLS cleanup is unsupported");
                }
            }
            const auto stored = slot.values.find(thread);
            if (stored != slot.values.end()) value = stored->second;
            callback = slot.callback;
            invoke = state_->guest_callback;
            state_->fls_slots.erase(found);
        }
        if (callback != 0 && value != 0) {
            if (!invoke) throw std::runtime_error("guest FLS callback invoker is unavailable");
            invoke(callback, value);
        }
        state.x[0] = 1;
        return std::nullopt;
    }
    if (function == "InitializeCriticalSection" ||
        function == "InitializeCriticalSectionAndSpinCount" ||
        function == "InitializeCriticalSectionEx") {
        const auto address = state.x[0];
        for (std::uint64_t offset = 0; offset < sizeof(CRITICAL_SECTION); ++offset) {
            static_cast<void>(memory.read8(address + offset));
        }
        std::lock_guard lock(state_->mutex);
        if (state_->sections.contains(address)) {
            throw std::runtime_error("guest critical section was initialized twice");
        }
        auto section = std::make_shared<State::CriticalSection>();
        const auto spin = function == "InitializeCriticalSection"
                              ? 0U : static_cast<DWORD>(state.x[1]);
        const auto flags = function == "InitializeCriticalSectionEx"
                               ? static_cast<DWORD>(state.x[2]) : 0U;
        const auto success = InitializeCriticalSectionEx(&section->value, spin, flags);
        if (!success) {
            if (function == "InitializeCriticalSection") {
                throw std::runtime_error("could not initialize guest critical section");
            }
            state.x[0] = 0;
            return std::nullopt;
        }
        section->initialized = true;
        mirror_critical_section(memory, address, section->value);
        state_->sections.emplace(address, std::move(section));
        if (function != "InitializeCriticalSection") state.x[0] = 1;
        return std::nullopt;
    }

    if (function == "EnterCriticalSection" || function == "TryEnterCriticalSection" ||
        function == "LeaveCriticalSection" || function == "DeleteCriticalSection" ||
        function == "SetCriticalSectionSpinCount") {
        const auto address = state.x[0];
        std::shared_ptr<State::CriticalSection> section;
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->sections.find(address);
            if (found == state_->sections.end()) {
                throw std::runtime_error("guest critical section is not initialized");
            }
            section = found->second;
        }
        if (function == "EnterCriticalSection") {
            EnterCriticalSection(&section->value);
        } else if (function == "TryEnterCriticalSection") {
            const auto success = TryEnterCriticalSection(&section->value);
            mirror_critical_section(memory, address, section->value);
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        } else if (function == "LeaveCriticalSection") {
            LeaveCriticalSection(&section->value);
        } else if (function == "SetCriticalSectionSpinCount") {
            const auto previous = ::SetCriticalSectionSpinCount(
                &section->value, static_cast<DWORD>(state.x[1]));
            mirror_critical_section(memory, address, section->value);
            state.x[0] = previous;
            return std::nullopt;
        } else {
            if (section->value.RecursionCount != 0) {
                throw std::runtime_error("cannot delete an owned guest critical section");
            }
            {
                std::lock_guard lock(state_->mutex);
                state_->sections.erase(address);
            }
            for (std::uint64_t offset = 0; offset < sizeof(CRITICAL_SECTION); ++offset) {
                memory.write8(address + offset, 0);
            }
            return std::nullopt;
        }
        mirror_critical_section(memory, address, section->value);
        return std::nullopt;
    }
    return dispatch_win32_import(imported, state, memory);
}

std::optional<std::uint32_t> dispatch_win32_import(const Import& imported,
                                                    CpuState& state,
                                                    GuestMemory& memory) {
    std::string dll = imported.dll;
    std::transform(dll.begin(), dll.end(), dll.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (dll == "kernel32.dll" || dll == "kernelbase.dll") {
        if (imported.function == "GetLastError") {
            state.x[0] = GetLastError();
            if (state.x[18] != 0) memory.write32(state.x[18] + 0x68,
                                                  static_cast<std::uint32_t>(state.x[0]));
            return std::nullopt;
        }
        if (imported.function == "SetLastError") {
            SetLastError(static_cast<DWORD>(state.x[0]));
            if (state.x[18] != 0) memory.write32(state.x[18] + 0x68,
                                                  static_cast<std::uint32_t>(state.x[0]));
            return std::nullopt;
        }
        if (imported.function == "SetErrorMode") {
            state.x[0] = SetErrorMode(static_cast<UINT>(state.x[0]));
            return std::nullopt;
        }
        if (imported.function == "GetErrorMode") {
            state.x[0] = GetErrorMode();
            return std::nullopt;
        }
        if (imported.function == "IsDebuggerPresent") {
            state.x[0] = static_cast<std::uint32_t>(IsDebuggerPresent());
            return std::nullopt;
        }
        if (imported.function == "EncodePointer" ||
            imported.function == "DecodePointer") {
            const auto value = reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(state.x[0]));
            const auto transformed = imported.function == "EncodePointer"
                                         ? EncodePointer(value) : DecodePointer(value);
            state.x[0] = reinterpret_cast<std::uintptr_t>(transformed);
            return std::nullopt;
        }
        if (imported.function == "ExitProcess") {
            return static_cast<std::uint32_t>(state.x[0]);
        }
        if (imported.function == "GetCurrentProcessId") {
            state.x[0] = GetCurrentProcessId();
            return std::nullopt;
        }
        if (imported.function == "GetCurrentThreadId") {
            state.x[0] = GetCurrentThreadId();
            return std::nullopt;
        }
        if (imported.function == "GetTickCount64") {
            state.x[0] = GetTickCount64();
            return std::nullopt;
        }
        if (imported.function == "GetSystemTimeAsFileTime") {
            FILETIME filetime{};
            GetSystemTimeAsFileTime(&filetime);
            memory.write64(state.x[0],
                           (static_cast<std::uint64_t>(filetime.dwHighDateTime) << 32) |
                               filetime.dwLowDateTime);
            return std::nullopt;
        }
        if (imported.function == "QueryPerformanceCounter" ||
            imported.function == "QueryPerformanceFrequency") {
            LARGE_INTEGER value{};
            const auto success = imported.function == "QueryPerformanceCounter"
                                     ? QueryPerformanceCounter(&value)
                                     : QueryPerformanceFrequency(&value);
            if (success) memory.write64(state.x[0], static_cast<std::uint64_t>(value.QuadPart));
            state.x[0] = static_cast<std::uint32_t>(success);
            return std::nullopt;
        }
        if (imported.function == "GetVersionExW" ||
            imported.function == "GetVersionExA") {
            static_assert(sizeof(OSVERSIONINFOW) == 276 &&
                          sizeof(OSVERSIONINFOEXW) == 284 &&
                          sizeof(OSVERSIONINFOA) == 148 &&
                          sizeof(OSVERSIONINFOEXA) == 156);
            const bool wide = imported.function == "GetVersionExW";
            const auto address = state.x[0];
            if (!guest_range(memory, address, 4, false)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto size = memory.read32(address);
            if (size != (wide ? sizeof(OSVERSIONINFOW) : sizeof(OSVERSIONINFOA)) &&
                size != (wide ? sizeof(OSVERSIONINFOEXW) : sizeof(OSVERSIONINFOEXA))) {
                SetLastError(ERROR_BAD_LENGTH);
                state.x[0] = 0;
                return std::nullopt;
            }
            if (!guest_range(memory, address, size, true)) {
                SetLastError(ERROR_INVALID_PARAMETER);
                state.x[0] = 0;
                return std::nullopt;
            }
            const auto version = guest_os_version();
            if (wide) {
                auto output = version;
                output.dwOSVersionInfoSize = size;
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&output);
                for (std::size_t index = 0; index < size; ++index) {
                    memory.write8(address + index, bytes[index]);
                }
            } else {
                OSVERSIONINFOEXA output{};
                output.dwOSVersionInfoSize = size;
                output.dwMajorVersion = version.dwMajorVersion;
                output.dwMinorVersion = version.dwMinorVersion;
                output.dwBuildNumber = version.dwBuildNumber;
                output.dwPlatformId = version.dwPlatformId;
                output.wServicePackMajor = version.wServicePackMajor;
                output.wServicePackMinor = version.wServicePackMinor;
                output.wSuiteMask = version.wSuiteMask;
                output.wProductType = version.wProductType;
                if (!WideCharToMultiByte(CP_ACP, 0, version.szCSDVersion, -1,
                                         output.szCSDVersion,
                                         static_cast<int>(sizeof(output.szCSDVersion)),
                                         nullptr, nullptr)) {
                    state.x[0] = 0;
                    return std::nullopt;
                }
                const auto* bytes = reinterpret_cast<const std::uint8_t*>(&output);
                for (std::size_t index = 0; index < size; ++index) {
                    memory.write8(address + index, bytes[index]);
                }
            }
            state.x[0] = 1;
            return std::nullopt;
        }
        if (imported.function == "GetVersion") {
            const auto version = guest_os_version();
            state.x[0] = ((version.dwBuildNumber & 0xffffU) << 16) |
                         ((version.dwMinorVersion & 0xffU) << 8) |
                         (version.dwMajorVersion & 0xffU);
            return std::nullopt;
        }
        if (imported.function == "VerSetConditionMask") {
            state.x[0] = VerSetConditionMask(state.x[0],
                                             static_cast<DWORD>(state.x[1]),
                                             static_cast<BYTE>(state.x[2]));
            return std::nullopt;
        }
        if (imported.function == "VerifyVersionInfoW" ||
            imported.function == "VerifyVersionInfoA") {
            const bool wide = imported.function == "VerifyVersionInfoW";
            const auto address = state.x[0];
            const auto size = wide ? sizeof(OSVERSIONINFOEXW) : sizeof(OSVERSIONINFOEXA);
            if (!guest_range(memory, address, size, false) ||
                memory.read32(address) != size) {
                SetLastError(ERROR_BAD_LENGTH);
                state.x[0] = 0;
                return std::nullopt;
            }
            OSVERSIONINFOEXW requested{};
            requested.dwOSVersionInfoSize = sizeof(requested);
            if (wide) {
                auto* bytes = reinterpret_cast<std::uint8_t*>(&requested);
                for (std::size_t index = 0; index < size; ++index) {
                    bytes[index] = memory.read8(address + index);
                }
            } else {
                OSVERSIONINFOEXA input{};
                auto* bytes = reinterpret_cast<std::uint8_t*>(&input);
                for (std::size_t index = 0; index < size; ++index) {
                    bytes[index] = memory.read8(address + index);
                }
                requested.dwMajorVersion = input.dwMajorVersion;
                requested.dwMinorVersion = input.dwMinorVersion;
                requested.dwBuildNumber = input.dwBuildNumber;
                requested.dwPlatformId = input.dwPlatformId;
                requested.wServicePackMajor = input.wServicePackMajor;
                requested.wServicePackMinor = input.wServicePackMinor;
                requested.wSuiteMask = input.wSuiteMask;
                requested.wProductType = input.wProductType;
            }
            const auto error = verify_guest_os_version(
                requested, static_cast<DWORD>(state.x[1]), state.x[2]);
            if (error != ERROR_SUCCESS) SetLastError(error);
            state.x[0] = error == ERROR_SUCCESS ? 1U : 0U;
            return std::nullopt;
        }
    }
    std::ostringstream message;
    message << "unsupported Windows import " << imported.dll << '!'
            << imported.function << " (x0=0x" << std::hex << state.x[0]
            << ", x1=0x" << state.x[1] << ", x2=0x" << state.x[2]
            << ", x3=0x" << state.x[3] << ')';
    throw std::runtime_error(message.str());
}

} // namespace transmute
