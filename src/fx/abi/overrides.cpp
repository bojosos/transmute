// Guest-aware overrides for imports (owner: abi): loader, threads, memory,
// processor identity and SEH. CRT overrides live in overrides_crt.cpp.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/overrides_internal.hpp"
#include "fx/common/cpu_features.hpp"
#include "fx/loader/loader.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>
#include <psapi.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>

namespace transmute::fx::abi {

// ============================================================================
// Shared helpers
// ============================================================================

std::wstring guest_wide(std::uint64_t address) {
    if (address == 0) return {};
    return std::wstring(reinterpret_cast<const wchar_t*>(address));
}

std::wstring guest_ansi_to_wide(std::uint64_t address) {
    if (address == 0) return {};
    const char* text = reinterpret_cast<const char*>(address);
    const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), n);
    out.resize(static_cast<std::size_t>(n - 1));
    return out;
}

std::string wide_to_ansi(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_ACP, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool is_current_process(std::uint64_t handle) {
    if (handle == static_cast<std::uint64_t>(-1)) return true;   // GetCurrentProcess()
    return GetProcessId(reinterpret_cast<HANDLE>(handle)) == GetCurrentProcessId();
}

std::uint64_t main_guest_module_base() {
    for (const auto* module : guest_modules())
        if (module && !module->is_dll) return module->base;
    return 0;
}

const GuestModule* guest_module_by_handle(std::uint64_t handle) {
    if (handle == 0) {
        for (const auto* module : guest_modules())
            if (module && !module->is_dll) return module;
        return nullptr;
    }
    const auto* module = find_guest_module(handle);
    return (module && module->base == handle) ? module : nullptr;
}

namespace {

// GetModuleFileName semantics: truncates to size-1 characters plus NUL and
// reports ERROR_INSUFFICIENT_BUFFER (returning size) when it does not fit.
template <typename Char>
std::uint64_t copy_module_name(const std::basic_string<Char>& text, std::uint64_t buffer, std::uint64_t size_arg) {
    const auto size = static_cast<DWORD>(size_arg);
    auto* out = reinterpret_cast<Char*>(buffer);
    if (size == 0 || out == nullptr) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }
    if (text.size() < size) {
        std::memcpy(out, text.c_str(), (text.size() + 1) * sizeof(Char));
        SetLastError(ERROR_SUCCESS);
        return text.size();
    }
    std::memcpy(out, text.c_str(), (size - 1) * sizeof(Char));
    out[size - 1] = 0;
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return size;
}

using R = OverrideResult;
constexpr R kReturn = R::return_to_caller;
constexpr R kResume = R::resume;

template <typename Fn>
Fn native_fn(const OverrideBinding& b) { return reinterpret_cast<Fn>(const_cast<void*>(b.native)); }

using NativeFn4 = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
std::uint64_t call_native4(const OverrideBinding& b, const GuestContext& ctx) {
    if (!b.native) return 0;
    return native_fn<NativeFn4>(b)(ctx.x[0], ctx.x[1], ctx.x[2], ctx.x[3]);
}

// ============================================================================
// Loader
// ============================================================================

R o_LoadLibraryA(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_load_library(guest_ansi_to_wide(ctx.x[0]), 0);
    return kReturn;
}
R o_LoadLibraryW(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_load_library(guest_wide(ctx.x[0]), 0);
    return kReturn;
}
R o_LoadLibraryExA(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_load_library(guest_ansi_to_wide(ctx.x[0]), static_cast<std::uint32_t>(ctx.x[2]));
    return kReturn;
}
R o_LoadLibraryExW(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_load_library(guest_wide(ctx.x[0]), static_cast<std::uint32_t>(ctx.x[2]));
    return kReturn;
}

std::uint64_t module_handle(const std::wstring* name) {
    if (name == nullptr) {
        if (const auto base = main_guest_module_base()) return base;
        return reinterpret_cast<std::uint64_t>(GetModuleHandleW(nullptr));
    }
    return guest_get_module_handle(*name);
}

R o_GetModuleHandleA(GuestContext& ctx, const OverrideBinding&) {
    if (ctx.x[0] == 0) { ctx.x[0] = module_handle(nullptr); return kReturn; }
    const auto name = guest_ansi_to_wide(ctx.x[0]);
    ctx.x[0] = module_handle(&name);
    if (ctx.x[0] == 0) SetLastError(ERROR_MOD_NOT_FOUND);
    return kReturn;
}
R o_GetModuleHandleW(GuestContext& ctx, const OverrideBinding&) {
    if (ctx.x[0] == 0) { ctx.x[0] = module_handle(nullptr); return kReturn; }
    const auto name = guest_wide(ctx.x[0]);
    ctx.x[0] = module_handle(&name);
    if (ctx.x[0] == 0) SetLastError(ERROR_MOD_NOT_FOUND);
    return kReturn;
}

R module_handle_ex(GuestContext& ctx, const OverrideBinding& b, bool wide) {
    const auto flags = static_cast<DWORD>(ctx.x[0]);
    auto* out = reinterpret_cast<HMODULE*>(ctx.x[2]);
    if (out == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        ctx.x[0] = FALSE;
        return kReturn;
    }
    std::uint64_t handle = 0;
    if (flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) {
        if (const auto base = guest_module_from_address(ctx.x[1])) {
            handle = base;
        } else if (b.native) {
            ctx.x[0] = native_fn<NativeFn4>(b)(ctx.x[0], ctx.x[1], ctx.x[2], 0) & 0xffffffffULL;
            return kReturn;
        }
    } else if (ctx.x[1] == 0) {
        handle = module_handle(nullptr);
    } else {
        const auto name = wide ? guest_wide(ctx.x[1]) : guest_ansi_to_wide(ctx.x[1]);
        handle = module_handle(&name);
    }
    *out = reinterpret_cast<HMODULE>(handle);
    if (handle == 0) SetLastError(ERROR_MOD_NOT_FOUND);
    ctx.x[0] = handle != 0;
    return kReturn;
}
R o_GetModuleHandleExA(GuestContext& ctx, const OverrideBinding& b) { return module_handle_ex(ctx, b, false); }
R o_GetModuleHandleExW(GuestContext& ctx, const OverrideBinding& b) { return module_handle_ex(ctx, b, true); }

R o_GetProcAddress(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_get_proc_address(ctx.x[0], reinterpret_cast<const char*>(ctx.x[1]));
    return kReturn;
}
R o_FreeLibrary(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_free_library(ctx.x[0]) ? TRUE : FALSE;
    return kReturn;
}
R o_FreeLibraryAndExitThread(GuestContext& ctx, const OverrideBinding&) {
    guest_free_library(ctx.x[0]);
    exit_guest_thread(static_cast<std::uint32_t>(ctx.x[1]));
}
R o_DisableThreadLibraryCalls(GuestContext& ctx, const OverrideBinding& b) {
    if (guest_module_by_handle(ctx.x[0]) && ctx.x[0] != 0) {
        ctx.x[0] = guest_disable_thread_library_calls(ctx.x[0]) ? TRUE : FALSE;
        return kReturn;
    }
    ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    return kReturn;
}

// CreateProcess{W,A}(app, cmd, pa, ta, inherit, flags, env, dir, si, pi): an
// x64 host cannot start an ARM64 image (ERROR_EXE_MACHINE_TYPE_MISMATCH), so
// an ARM64 target starts as "transmute-fx.exe -- <image> <arguments>" with
// the same flags, handles, environment and directory. Other targets go to
// the native API unchanged.
bool is_arm64_image(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    IMAGE_DOS_HEADER dos{};
    DWORD got = 0;
    bool arm64 = false;
    if (ReadFile(file, &dos, sizeof(dos), &got, nullptr) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(file, dos.e_lfanew, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
        IMAGE_NT_HEADERS64 nt{};
        if (ReadFile(file, &nt, offsetof(IMAGE_NT_HEADERS64, OptionalHeader), &got, nullptr) &&
            got == offsetof(IMAGE_NT_HEADERS64, OptionalHeader) && nt.Signature == IMAGE_NT_SIGNATURE)
            arm64 = nt.FileHeader.Machine == IMAGE_FILE_MACHINE_ARM64;
    }
    CloseHandle(file);
    return arm64;
}

// Splits off the program token the way CreateProcess reads lpCommandLine
// (quoted, or up to the first blank); `rest` keeps everything after it.
std::wstring command_line_program(const std::wstring& cmd, std::wstring& rest) {
    std::size_t i = 0;
    while (i < cmd.size() && (cmd[i] == L' ' || cmd[i] == L'\t')) ++i;
    std::wstring program;
    if (i < cmd.size() && cmd[i] == L'"') {
        const std::size_t close = cmd.find(L'"', i + 1);
        program = cmd.substr(i + 1, close == std::wstring::npos ? std::wstring::npos : close - i - 1);
        i = close == std::wstring::npos ? cmd.size() : close + 1;
    } else {
        const std::size_t end = cmd.find_first_of(L" \t", i);
        program = cmd.substr(i, end == std::wstring::npos ? std::wstring::npos : end - i);
        i = end == std::wstring::npos ? cmd.size() : end;
    }
    rest = cmd.substr(i);
    return program;
}

std::wstring resolve_program(const std::wstring& program, const wchar_t* directory) {
    if (program.empty()) return {};
    wchar_t found[MAX_PATH * 2] = {};
    const bool has_path = program.find_first_of(L"\\/:") != std::wstring::npos;
    if (has_path) {
        std::wstring candidate = program;
        if (directory && candidate.size() > 1 && candidate[1] != L':' && candidate[0] != L'\\' && candidate[0] != L'/')
            candidate = std::wstring(directory) + L"\\" + candidate;
        for (const std::wstring& name : {candidate, candidate + L".exe"}) {
            if (GetFileAttributesW(name.c_str()) != INVALID_FILE_ATTRIBUTES &&
                GetFullPathNameW(name.c_str(), static_cast<DWORD>(std::size(found)), found, nullptr))
                return found;
        }
        return {};
    }
    if (SearchPathW(directory, program.c_str(), L".exe", static_cast<DWORD>(std::size(found)), found, nullptr) ||
        SearchPathW(nullptr, program.c_str(), L".exe", static_cast<DWORD>(std::size(found)), found, nullptr))
        return found;
    return {};
}

std::wstring arm64_launch_line(const std::wstring& image, const std::wstring& arguments) {
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    return L"\"" + std::wstring(self) + L"\" -- \"" + image + L"\"" + arguments;
}

// Returns the image when the call targets an ARM64 executable, else empty.
std::wstring arm64_target(const wchar_t* app, const wchar_t* cmd, const wchar_t* directory, std::wstring& arguments) {
    std::wstring program;
    arguments.clear();
    if (app) {
        program = app;
        if (cmd) command_line_program(cmd, arguments);
    } else if (cmd) {
        program = command_line_program(cmd, arguments);
    }
    const std::wstring image = resolve_program(program, directory);
    return !image.empty() && is_arm64_image(image) ? image : std::wstring();
}

// The guest environment reports an ARM64 processor (the loader sets
// PROCESSOR_ARCHITECTURE=ARM64 etc.). An x64 child is a host process: on
// ARM64 Windows an x64 process sees AMD64 there too, and host tools (pip,
// build scripts) depend on it. The host values come from the system
// environment (Session Manager\Environment), which our own environment no
// longer has.
const wchar_t* const kProcessorVariables[] = {L"PROCESSOR_ARCHITECTURE", L"PROCESSOR_IDENTIFIER", L"PROCESSOR_LEVEL",
                                              L"PROCESSOR_REVISION"};

const std::vector<std::pair<std::wstring, std::wstring>>& host_processor_values() {
    static const auto values = [] {
        std::vector<std::pair<std::wstring, std::wstring>> out;
        for (const wchar_t* name : kProcessorVariables) {
            wchar_t value[512] = {};
            DWORD size = sizeof(value);
            if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                             name, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS)
                out.emplace_back(name, value);
            else if (std::wstring_view(name) == L"PROCESSOR_ARCHITECTURE")
                out.emplace_back(name, L"AMD64");
        }
        return out;
    }();
    return values;
}

// Environment block for an x64 child: `environment` (the caller's block, ANSI
// unless `unicode`; nullptr = ours) with the processor variables set to the
// host's. Returns a Unicode block (pass CREATE_UNICODE_ENVIRONMENT).
std::wstring host_child_environment(const void* environment, bool unicode) {
    std::vector<std::wstring> entries;
    if (!environment) {
        if (wchar_t* own = GetEnvironmentStringsW()) {
            for (const wchar_t* p = own; *p; p += std::wcslen(p) + 1) entries.emplace_back(p);
            FreeEnvironmentStringsW(own);
        }
    } else if (unicode) {
        for (auto* p = static_cast<const wchar_t*>(environment); *p; p += std::wcslen(p) + 1) entries.emplace_back(p);
    } else {
        for (auto* p = static_cast<const char*>(environment); *p; p += std::strlen(p) + 1) {
            const int n = MultiByteToWideChar(CP_ACP, 0, p, -1, nullptr, 0);
            std::wstring w(n > 0 ? n - 1 : 0, L'\0');
            if (n > 1) MultiByteToWideChar(CP_ACP, 0, p, -1, w.data(), n);
            entries.push_back(std::move(w));
        }
    }
    const auto& host = host_processor_values();
    std::vector<bool> placed(host.size());
    for (auto& entry : entries) {
        const std::size_t eq = entry.find(L'=', 1);   // "=C:=C:\..." entries start with '='
        if (eq == std::wstring::npos) continue;
        for (std::size_t i = 0; i < host.size(); ++i) {
            if (_wcsnicmp(entry.c_str(), host[i].first.c_str(), eq) == 0 && host[i].first.size() == eq) {
                entry = host[i].first + L"=" + host[i].second;
                placed[i] = true;
            }
        }
    }
    for (std::size_t i = 0; i < host.size(); ++i)
        if (!placed[i]) entries.push_back(host[i].first + L"=" + host[i].second);
    std::wstring block;
    for (const auto& entry : entries) {
        block += entry;
        block.push_back(L'\0');
    }
    if (entries.empty()) block.push_back(L'\0');
    block.push_back(L'\0');
    return block;
}

using CreateProcessWFn = BOOL(WINAPI*)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                       LPCWSTR, LPSTARTUPINFOW, LPPROCESS_INFORMATION);
using CreateProcessAFn = BOOL(WINAPI*)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID,
                                       LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);

R o_CreateProcessW(GuestContext& ctx, const OverrideBinding& b) {
    const auto* app = reinterpret_cast<const wchar_t*>(ctx.x[0]);
    auto* cmd = reinterpret_cast<wchar_t*>(ctx.x[1]);
    const auto* dir = reinterpret_cast<const wchar_t*>(ctx.x[7]);
    auto* si = reinterpret_cast<LPSTARTUPINFOW>(guest_stack_arg(ctx, 0));
    auto* pi = reinterpret_cast<LPPROCESS_INFORMATION>(guest_stack_arg(ctx, 1));
    const auto native = native_fn<CreateProcessWFn>(b);
    std::wstring arguments;
    const std::wstring image = arm64_target(app, cmd, dir, arguments);
    BOOL ok;
    if (image.empty()) {
        // A host (x64) child: host processor variables.
        std::wstring environment = host_child_environment(reinterpret_cast<const void*>(ctx.x[6]),
                                                          (ctx.x[5] & CREATE_UNICODE_ENVIRONMENT) != 0);
        ok = native(app, cmd, reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[2]),
                    reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[3]), static_cast<BOOL>(ctx.x[4]),
                    static_cast<DWORD>(ctx.x[5]) | CREATE_UNICODE_ENVIRONMENT, environment.data(), dir, si, pi);
    } else {
        std::wstring line = arm64_launch_line(image, arguments);
        wchar_t self[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        ok = native(self, line.data(), reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[2]),
                    reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[3]), static_cast<BOOL>(ctx.x[4]),
                    static_cast<DWORD>(ctx.x[5]), reinterpret_cast<LPVOID>(ctx.x[6]), dir, si, pi);
    }
    ctx.x[0] = ok ? TRUE : FALSE;
    return kReturn;
}

std::wstring ansi_to_wide(const char* text) {
    if (!text) return {};
    const int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring out(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), n);
    return out;
}

R o_CreateProcessA(GuestContext& ctx, const OverrideBinding& b) {
    const auto* app = reinterpret_cast<const char*>(ctx.x[0]);
    auto* cmd = reinterpret_cast<char*>(ctx.x[1]);
    const auto* dir = reinterpret_cast<const char*>(ctx.x[7]);
    auto* si = reinterpret_cast<LPSTARTUPINFOA>(guest_stack_arg(ctx, 0));
    auto* pi = reinterpret_cast<LPPROCESS_INFORMATION>(guest_stack_arg(ctx, 1));
    const std::wstring wapp = ansi_to_wide(app), wcmd = ansi_to_wide(cmd), wdir = ansi_to_wide(dir);
    std::wstring arguments;
    const std::wstring image = arm64_target(app ? wapp.c_str() : nullptr, cmd ? wcmd.c_str() : nullptr,
                                            dir ? wdir.c_str() : nullptr, arguments);
    BOOL ok;
    if (image.empty()) {
        std::wstring environment = host_child_environment(reinterpret_cast<const void*>(ctx.x[6]),
                                                          (ctx.x[5] & CREATE_UNICODE_ENVIRONMENT) != 0);
        ok = native_fn<CreateProcessAFn>(b)(app, cmd, reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[2]),
                                            reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[3]), static_cast<BOOL>(ctx.x[4]),
                                            static_cast<DWORD>(ctx.x[5]) | CREATE_UNICODE_ENVIRONMENT,
                                            environment.data(), dir, si, pi);
    } else {
        // Same request through CreateProcessW. Without CREATE_UNICODE_ENVIRONMENT
        // the environment block stays ANSI, which CreateProcessW accepts.
        // STARTUPINFOEX (EXTENDED_STARTUPINFO_PRESENT) carries an attribute list.
        STARTUPINFOEXW wsi{};
        std::wstring desktop, title;
        if (si) {
            const bool extended = (ctx.x[5] & EXTENDED_STARTUPINFO_PRESENT) != 0;
            const std::size_t size = extended ? sizeof(STARTUPINFOEXW) : sizeof(STARTUPINFOW);
            std::memcpy(&wsi, si, std::min<std::size_t>(size, si->cb));
            wsi.StartupInfo.cb = static_cast<DWORD>(size);
            desktop = ansi_to_wide(si->lpDesktop);
            title = ansi_to_wide(si->lpTitle);
            wsi.StartupInfo.lpDesktop = si->lpDesktop ? desktop.data() : nullptr;
            wsi.StartupInfo.lpTitle = si->lpTitle ? title.data() : nullptr;
            wsi.StartupInfo.lpReserved = nullptr;
        }
        std::wstring line = arm64_launch_line(image, arguments);
        wchar_t self[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
        ok = CreateProcessW(self, line.data(), reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[2]),
                            reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[3]), static_cast<BOOL>(ctx.x[4]),
                            static_cast<DWORD>(ctx.x[5]), reinterpret_cast<LPVOID>(ctx.x[6]), dir ? wdir.c_str() : nullptr,
                            si ? &wsi.StartupInfo : nullptr, pi);
    }
    ctx.x[0] = ok ? TRUE : FALSE;
    return kReturn;
}

// AddDllDirectory(path) -> cookie, RemoveDllDirectory(cookie), SetDllDirectory{A,W}(path or NULL):
// the guest search path is the loader's.
R o_AddDllDirectory(GuestContext& ctx, const OverrideBinding&) {
    if (ctx.x[0] == 0) { SetLastError(ERROR_INVALID_PARAMETER); ctx.x[0] = 0; return kReturn; }
    ctx.x[0] = guest_add_dll_directory(guest_wide(ctx.x[0]));
    return kReturn;
}
R o_RemoveDllDirectory(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_remove_dll_directory(ctx.x[0]) ? TRUE : FALSE;
    return kReturn;
}
R o_SetDllDirectoryW(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_set_dll_directory(guest_wide(ctx.x[0])) ? TRUE : FALSE;
    return kReturn;
}
R o_SetDllDirectoryA(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_set_dll_directory(guest_ansi_to_wide(ctx.x[0])) ? TRUE : FALSE;
    return kReturn;
}
// ResolveDelayLoadedAPI(parent, descriptor, failure_dll_hook, failure_system_hook, thunk, flags)
R o_ResolveDelayLoadedAPI(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_resolve_delay_loaded_api(ctx.x[0], ctx.x[1], ctx.x[2], ctx.x[3], ctx.x[4],
                                              static_cast<std::uint32_t>(ctx.x[5]));
    return kReturn;
}

// Path of guest module `handle` (0 = the guest EXE); false for host modules.
bool guest_module_path(std::uint64_t handle, std::wstring& path) {
    if (handle != 0 && !guest_module_by_handle(handle)) return false;
    path = guest_module_file_name(handle);
    if (!path.empty()) return true;
    if (handle == 0) {
        path = guest_image_path();
        return !path.empty();
    }
    path = guest_module_by_handle(handle)->path;
    return true;
}

R o_GetModuleFileNameW(GuestContext& ctx, const OverrideBinding& b) {
    std::wstring path;
    if (guest_module_path(ctx.x[0], path)) ctx.x[0] = copy_module_name(path, ctx.x[1], ctx.x[2]);
    else ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    return kReturn;
}
R o_GetModuleFileNameA(GuestContext& ctx, const OverrideBinding& b) {
    std::wstring path;
    if (guest_module_path(ctx.x[0], path)) ctx.x[0] = copy_module_name(wide_to_ansi(path), ctx.x[1], ctx.x[2]);
    else ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    return kReturn;
}
// (K32)GetModuleFileNameEx{A,W}(process, module, buffer, size)
R module_file_name_ex(GuestContext& ctx, const OverrideBinding& b, bool wide) {
    std::wstring path;
    if (is_current_process(ctx.x[0]) && guest_module_path(ctx.x[1], path)) {
        ctx.x[0] = wide ? copy_module_name(path, ctx.x[2], ctx.x[3])
                        : copy_module_name(wide_to_ansi(path), ctx.x[2], ctx.x[3]);
        return kReturn;
    }
    ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    return kReturn;
}
R o_GetModuleFileNameExW(GuestContext& ctx, const OverrideBinding& b) { return module_file_name_ex(ctx, b, true); }
R o_GetModuleFileNameExA(GuestContext& ctx, const OverrideBinding& b) { return module_file_name_ex(ctx, b, false); }

R module_base_name(GuestContext& ctx, const OverrideBinding& b, bool wide) {
    if (is_current_process(ctx.x[0])) {
        if (const auto* module = guest_module_by_handle(ctx.x[1])) {
            ctx.x[0] = wide ? copy_module_name(module->name, ctx.x[2], ctx.x[3])
                            : copy_module_name(wide_to_ansi(module->name), ctx.x[2], ctx.x[3]);
            return kReturn;
        }
    }
    ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    return kReturn;
}
R o_GetModuleBaseNameW(GuestContext& ctx, const OverrideBinding& b) { return module_base_name(ctx, b, true); }
R o_GetModuleBaseNameA(GuestContext& ctx, const OverrideBinding& b) { return module_base_name(ctx, b, false); }

// (K32)GetModuleInformation(process, module, MODULEINFO*, cb)
R o_GetModuleInformation(GuestContext& ctx, const OverrideBinding& b) {
    if (is_current_process(ctx.x[0])) {
        if (const auto* module = guest_module_by_handle(ctx.x[1])) {
            if (ctx.x[3] < sizeof(MODULEINFO) || ctx.x[2] == 0) {
                SetLastError(ERROR_INSUFFICIENT_BUFFER);
                ctx.x[0] = FALSE;
                return kReturn;
            }
            auto* info = reinterpret_cast<MODULEINFO*>(ctx.x[2]);
            info->lpBaseOfDll = reinterpret_cast<void*>(module->base);
            info->SizeOfImage = static_cast<DWORD>(module->size);
            info->EntryPoint = reinterpret_cast<void*>(module->entry);
            ctx.x[0] = TRUE;
            return kReturn;
        }
    }
    ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    return kReturn;
}

// (K32)EnumProcessModules(Ex): guest modules first, then the host list.
R enum_process_modules(GuestContext& ctx, const OverrideBinding& b, bool ex) {
    if (!is_current_process(ctx.x[0]) || !b.native) {
        if (ex) {
            using Fn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
            ctx.x[0] = native_fn<Fn>(b)(reinterpret_cast<HANDLE>(ctx.x[0]), reinterpret_cast<HMODULE*>(ctx.x[1]),
                                        static_cast<DWORD>(ctx.x[2]), reinterpret_cast<LPDWORD>(ctx.x[3]),
                                        static_cast<DWORD>(ctx.x[4]));
        } else {
            ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
        }
        return kReturn;
    }
    std::vector<HMODULE> host(1024);
    DWORD needed = 0;
    BOOL ok;
    if (ex) {
        using Fn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD, DWORD);
        ok = native_fn<Fn>(b)(GetCurrentProcess(), host.data(), static_cast<DWORD>(host.size() * sizeof(HMODULE)),
                              &needed, static_cast<DWORD>(ctx.x[4]));
    } else {
        using Fn = BOOL(WINAPI*)(HANDLE, HMODULE*, DWORD, LPDWORD);
        ok = native_fn<Fn>(b)(GetCurrentProcess(), host.data(), static_cast<DWORD>(host.size() * sizeof(HMODULE)), &needed);
    }
    if (!ok) { ctx.x[0] = FALSE; return kReturn; }
    host.resize(std::min<std::size_t>(host.size(), needed / sizeof(HMODULE)));
    std::vector<HMODULE> all;
    for (const auto* module : guest_modules())
        if (module) all.push_back(reinterpret_cast<HMODULE>(module->base));
    all.insert(all.end(), host.begin(), host.end());
    auto* out = reinterpret_cast<HMODULE*>(ctx.x[1]);
    const auto capacity = static_cast<std::size_t>(ctx.x[2]) / sizeof(HMODULE);
    for (std::size_t i = 0; i < std::min(capacity, all.size()); ++i) out[i] = all[i];
    if (ctx.x[3]) *reinterpret_cast<DWORD*>(ctx.x[3]) = static_cast<DWORD>(all.size() * sizeof(HMODULE));
    ctx.x[0] = TRUE;
    return kReturn;
}
R o_EnumProcessModules(GuestContext& ctx, const OverrideBinding& b) { return enum_process_modules(ctx, b, false); }
R o_EnumProcessModulesEx(GuestContext& ctx, const OverrideBinding& b) { return enum_process_modules(ctx, b, true); }

R o_GetCommandLineW(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = reinterpret_cast<std::uint64_t>(guest_command_line().c_str());
    return kReturn;
}
R o_GetCommandLineA(GuestContext& ctx, const OverrideBinding&) {
    static const std::string* narrow = new std::string(wide_to_ansi(guest_command_line()));
    ctx.x[0] = reinterpret_cast<std::uint64_t>(narrow->c_str());
    return kReturn;
}

// RtlPcToFileHeader(pc, base*)
R o_RtlPcToFileHeader(GuestContext& ctx, const OverrideBinding& b) {
    if (const auto* module = find_guest_module(ctx.x[0])) {
        if (ctx.x[1]) *reinterpret_cast<std::uint64_t*>(ctx.x[1]) = module->base;
        ctx.x[0] = module->base;
        return kReturn;
    }
    ctx.x[0] = call_native4(b, ctx);
    return kReturn;
}

// ============================================================================
// Threads
// ============================================================================

std::uint64_t image_default_stack_reserve() {
    if (const auto reserve = guest_image_stack_reserve()) return reserve;
    if (const auto base = main_guest_module_base()) {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
                nt->OptionalHeader.SizeOfStackReserve != 0)
                return nt->OptionalHeader.SizeOfStackReserve;
        }
    }
    return 1ull << 20;
}

} // namespace

std::uint64_t thread_stack_reserve(std::uint64_t size, std::uint32_t flags) {
    if (size == 0) return 0;                                   // image default
    if (flags & STACK_SIZE_PARAM_IS_A_RESERVATION) return size;
    // `size` is the commit: the reserve is the image default, or the commit
    // rounded up to a 1 MiB multiple when larger (as CreateThread does).
    const auto reserve = image_default_stack_reserve();
    if (size <= reserve) return 0;                             // the default reserve suffices
    constexpr std::uint64_t mib = 1ull << 20;
    return (size + mib - 1) & ~(mib - 1);
}

std::uint64_t start_guest_thread(std::uint64_t entry, std::uint64_t argument, std::uint64_t stack_size,
                                 std::uint32_t flags, std::uint32_t* thread_id) {
    GuestThreadStart start{};
    start.entry = entry;
    start.argument = argument;
    start.stack_reserve = thread_stack_reserve(stack_size, flags);
    start.creation_flags = flags & ~static_cast<std::uint32_t>(STACK_SIZE_PARAM_IS_A_RESERVATION);
    std::uint32_t id = 0;
    const auto handle = create_guest_thread(start, &id);
    if (thread_id) *thread_id = id;
    return handle;
}

namespace {

// CreateThread(sa, stack, start, param, flags, tid*)
R o_CreateThread(GuestContext& ctx, const OverrideBinding&) {
    std::uint32_t id = 0;
    ctx.x[0] = start_guest_thread(ctx.x[2], ctx.x[3], ctx.x[1], static_cast<std::uint32_t>(ctx.x[4]), &id);
    if (ctx.x[0] && ctx.x[5]) *reinterpret_cast<DWORD*>(ctx.x[5]) = id;
    return kReturn;
}
// CreateRemoteThread(process, sa, stack, start, param, flags, tid*)
R o_CreateRemoteThread(GuestContext& ctx, const OverrideBinding& b) {
    if (!is_current_process(ctx.x[0])) {
        using Fn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD, LPDWORD);
        ctx.x[0] = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
            reinterpret_cast<HANDLE>(ctx.x[0]), reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[1]), ctx.x[2],
            reinterpret_cast<LPTHREAD_START_ROUTINE>(ctx.x[3]), reinterpret_cast<LPVOID>(ctx.x[4]),
            static_cast<DWORD>(ctx.x[5]), reinterpret_cast<LPDWORD>(ctx.x[6])));
        return kReturn;
    }
    std::uint32_t id = 0;
    ctx.x[0] = start_guest_thread(ctx.x[3], ctx.x[4], ctx.x[2], static_cast<std::uint32_t>(ctx.x[5]), &id);
    if (ctx.x[0] && ctx.x[6]) *reinterpret_cast<DWORD*>(ctx.x[6]) = id;
    return kReturn;
}
// CreateRemoteThreadEx(process, sa, stack, start, param, flags, attributes, tid*)
R o_CreateRemoteThreadEx(GuestContext& ctx, const OverrideBinding& b) {
    if (!is_current_process(ctx.x[0])) {
        using Fn = HANDLE(WINAPI*)(HANDLE, LPSECURITY_ATTRIBUTES, SIZE_T, LPTHREAD_START_ROUTINE, LPVOID, DWORD,
                                   LPPROC_THREAD_ATTRIBUTE_LIST, LPDWORD);
        ctx.x[0] = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
            reinterpret_cast<HANDLE>(ctx.x[0]), reinterpret_cast<LPSECURITY_ATTRIBUTES>(ctx.x[1]), ctx.x[2],
            reinterpret_cast<LPTHREAD_START_ROUTINE>(ctx.x[3]), reinterpret_cast<LPVOID>(ctx.x[4]),
            static_cast<DWORD>(ctx.x[5]), reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(ctx.x[6]),
            reinterpret_cast<LPDWORD>(ctx.x[7])));
        return kReturn;
    }
    std::uint32_t id = 0;
    ctx.x[0] = start_guest_thread(ctx.x[3], ctx.x[4], ctx.x[2], static_cast<std::uint32_t>(ctx.x[5]), &id);
    if (ctx.x[0] && ctx.x[7]) *reinterpret_cast<DWORD*>(ctx.x[7]) = id;
    return kReturn;
}
R o_ExitThread(GuestContext& ctx, const OverrideBinding&) {
    exit_guest_thread(static_cast<std::uint32_t>(ctx.x[0]));
}

// QueueUserWorkItem(Function, Context, Flags) / RtlQueueWorkItem: the legacy
// thread pool silently never runs a callback whose address is not inside a
// loaded module, which rules out code-arena trampolines. Queue a function of
// this image instead and call the trampoline from there.
struct QueuedWorkItem {
    const void* callback;   // host-callable (trampoline or native)
    void* context;
};
DWORD WINAPI queued_work_item_entry(void* parameter) {
    auto* item = static_cast<QueuedWorkItem*>(parameter);
    const auto callback = reinterpret_cast<LPTHREAD_START_ROUTINE>(const_cast<void*>(item->callback));
    void* context = item->context;
    delete item;
    return callback(context);
}
R queue_work_item(GuestContext& ctx, const OverrideBinding& b, const char* callback_sig, bool nt_status) {
    const auto callback = wrap_callback_argument(ctx.x[0], intern_signature(callback_sig));
    if (callback == 0 || !b.native) {
        ctx.x[0] = nt_status ? static_cast<std::uint32_t>(STATUS_INVALID_PARAMETER) : FALSE;
        if (!nt_status) SetLastError(ERROR_INVALID_PARAMETER);
        return kReturn;
    }
    auto* item = new QueuedWorkItem{reinterpret_cast<const void*>(callback), reinterpret_cast<void*>(ctx.x[1])};
    using Fn = std::uint32_t(WINAPI*)(LPTHREAD_START_ROUTINE, void*, ULONG);
    const auto result = native_fn<Fn>(b)(&queued_work_item_entry, item, static_cast<ULONG>(ctx.x[2]));
    const bool ok = nt_status ? static_cast<LONG>(result) >= 0 : result != 0;
    if (!ok) delete item;
    ctx.x[0] = result;
    return kReturn;
}
R o_QueueUserWorkItem(GuestContext& ctx, const OverrideBinding& b) { return queue_work_item(ctx, b, "I(p)", false); }

// QueueUserAPC(PAPCFUNC, thread, data): same treatment. With the guest's
// activation context active (the loader activates it), the APC is
// dispatched through kernelbase, which never ran code-arena routines in
// testing; a routine of this image is always run.
struct QueuedApc {
    const void* routine;   // host-callable
    ULONG_PTR data;
};
void CALLBACK queued_apc_entry(ULONG_PTR parameter) {
    auto* apc = reinterpret_cast<QueuedApc*>(parameter);
    const auto routine = reinterpret_cast<PAPCFUNC>(const_cast<void*>(apc->routine));
    const ULONG_PTR data = apc->data;
    delete apc;
    routine(data);
}
R o_QueueUserAPC(GuestContext& ctx, const OverrideBinding& b) {
    const auto routine = wrap_callback_argument(ctx.x[0], intern_signature("v(l)"));
    if (routine == 0 || !b.native) {
        SetLastError(ERROR_INVALID_PARAMETER);
        ctx.x[0] = 0;
        return kReturn;
    }
    auto* apc = new QueuedApc{reinterpret_cast<const void*>(routine), static_cast<ULONG_PTR>(ctx.x[2])};
    using Fn = DWORD(WINAPI*)(PAPCFUNC, HANDLE, ULONG_PTR);
    const DWORD queued = native_fn<Fn>(b)(&queued_apc_entry, reinterpret_cast<HANDLE>(ctx.x[1]),
                                          reinterpret_cast<ULONG_PTR>(apc));
    if (queued == 0) delete apc;
    ctx.x[0] = queued;
    return kReturn;
}
R o_RtlQueueWorkItem(GuestContext& ctx, const OverrideBinding& b) { return queue_work_item(ctx, b, "v(p)", true); }

// A native thread pool cannot retain a guest HMODULE using Windows Ldr.
// Retain these guest DLLs for the process lifetime and use this host image
// for the native pool's module race protection. Also bridge callbacks nested
// inside the environment, which the ordinary argument signatures cannot see.
PTP_CALLBACK_ENVIRON threadpool_environment(std::uint64_t address, TP_CALLBACK_ENVIRON& copy) {
    auto* original = reinterpret_cast<PTP_CALLBACK_ENVIRON>(address);
    if (!original) return nullptr;
    if (original->Version != 1 && original->Version != 3) return original;
    constexpr std::size_t version1_size = offsetof(TP_CALLBACK_ENVIRON, CallbackPriority) + sizeof(DWORD);
    std::memcpy(&copy, original, original->Version == 1 ? version1_size : sizeof(copy));
    if (const auto module = reinterpret_cast<std::uint64_t>(copy.RaceDll);
        module && guest_pin_library(module))
        copy.RaceDll = GetModuleHandleW(nullptr);
    if (copy.CleanupGroupCancelCallback)
        copy.CleanupGroupCancelCallback = reinterpret_cast<PTP_CLEANUP_GROUP_CANCEL_CALLBACK>(
            wrap_callback_argument(reinterpret_cast<std::uint64_t>(copy.CleanupGroupCancelCallback), intern_signature("v(pp)")));
    if (copy.FinalizationCallback)
        copy.FinalizationCallback = reinterpret_cast<PTP_SIMPLE_CALLBACK>(
            wrap_callback_argument(reinterpret_cast<std::uint64_t>(copy.FinalizationCallback), intern_signature("v(pp)")));
    return &copy;
}

R o_CreateThreadpoolObject(GuestContext& ctx, const OverrideBinding& b) {
    const bool io = b.name == "CreateThreadpoolIo";
    const unsigned callback_index = io ? 1 : 0;
    const unsigned environment_index = io ? 3 : 2;
    const char* signature = io ? "v(pppIlp)" : b.name == "CreateThreadpoolWait" ? "v(pppI)" : "v(ppp)";
    const auto callback = wrap_callback_argument(ctx.x[callback_index], intern_signature(signature));
    TP_CALLBACK_ENVIRON environment{};
    const auto env = threadpool_environment(ctx.x[environment_index], environment);
    using Fn = std::uint64_t(WINAPI*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    ctx.x[0] = io ? native_fn<Fn>(b)(ctx.x[0], callback, ctx.x[2], reinterpret_cast<std::uint64_t>(env))
                 : native_fn<Fn>(b)(callback, ctx.x[1], reinterpret_cast<std::uint64_t>(env), 0);
    return kReturn;
}

R o_FreeLibraryWhenCallbackReturns(GuestContext& ctx, const OverrideBinding& b) {
    if (guest_pin_library(ctx.x[1])) {
        guest_free_library(ctx.x[1]);   // pinned: deferred code remains mapped
    } else {
        call_native4(b, ctx);
    }
    return kReturn;
}

// GetCurrentThreadStackLimits(low*, high*): the guest stack, not the host one.
R o_GetCurrentThreadStackLimits(GuestContext& ctx, const OverrideBinding& b) {
    if (ctx.guest_stack_base == 0) {
        call_native4(b, ctx);
        return kReturn;
    }
    if (ctx.x[0]) *reinterpret_cast<ULONG_PTR*>(ctx.x[0]) = ctx.guest_stack_limit;
    if (ctx.x[1]) *reinterpret_cast<ULONG_PTR*>(ctx.x[1]) = ctx.guest_stack_base;
    return kReturn;
}

// SetThreadStackGuarantee(ULONG* size): kept in the guest TEB's
// GuaranteedStackBytes; only grows; fails if it does not fit the guest stack.
constexpr std::size_t kTebGuaranteedStackBytes = 0x1748;
R o_SetThreadStackGuarantee(GuestContext& ctx, const OverrideBinding& b) {
    auto* size = reinterpret_cast<ULONG*>(ctx.x[0]);
    if (ctx.shadow_teb == nullptr || ctx.guest_stack_base == 0) {
        ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
        return kReturn;
    }
    if (size == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        ctx.x[0] = FALSE;
        return kReturn;
    }
    auto* guarantee = reinterpret_cast<ULONG*>(static_cast<std::uint8_t*>(ctx.shadow_teb) + kTebGuaranteedStackBytes);
    const ULONG previous = *guarantee;
    const std::uint64_t requested = (static_cast<std::uint64_t>(*size) + 0xfff) & ~std::uint64_t{0xfff};
    const std::uint64_t stack_size = ctx.guest_stack_base - ctx.guest_stack_limit;
    if (requested >= stack_size) {
        SetLastError(ERROR_INVALID_PARAMETER);
        ctx.x[0] = FALSE;
        return kReturn;
    }
    if (requested > previous) *guarantee = static_cast<ULONG>(requested);
    *size = previous;
    ctx.x[0] = TRUE;
    return kReturn;
}

// ============================================================================
// Memory: guest code is never host-executable
// ============================================================================

constexpr DWORD kExecProtections = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kTargetsFlags = PAGE_TARGETS_INVALID;   // == PAGE_TARGETS_NO_UPDATE; exec-only modifiers
constexpr std::uint64_t kPage = 0x1000;

bool is_exec(DWORD protect) { return (protect & kExecProtections) != 0; }

DWORD non_exec(DWORD protect) {
    if (!is_exec(protect)) return protect;
    const DWORD modifiers = protect & ~0xffu & ~kTargetsFlags;
    switch (protect & 0xff) {
    case PAGE_EXECUTE:
    case PAGE_EXECUTE_READ: return PAGE_READONLY | modifiers;
    case PAGE_EXECUTE_READWRITE: return PAGE_READWRITE | modifiers;
    case PAGE_EXECUTE_WRITECOPY: return PAGE_WRITECOPY | modifiers;
    default: return protect;
    }
}

DWORD exec_equivalent(DWORD protect) {
    const DWORD modifiers = protect & ~0xffu;
    switch (protect & 0xff) {
    case PAGE_READONLY: return PAGE_EXECUTE_READ | modifiers;
    case PAGE_READWRITE: return PAGE_EXECUTE_READWRITE | modifiers;
    case PAGE_WRITECOPY: return PAGE_EXECUTE_WRITECOPY | modifiers;
    default: return protect;
    }
}

std::uint64_t page_down(std::uint64_t v) { return v & ~(kPage - 1); }
std::uint64_t page_up(std::uint64_t v) { return (v + kPage - 1) & ~(kPage - 1); }

// Guest code ranges created through these overrides (outside PE images).
struct CodeRanges {
    std::mutex mutex;
    std::map<std::uint64_t, std::uint64_t> ranges;   // begin -> end
    std::atomic<bool> any{false};
};
CodeRanges& code_ranges() {
    static auto* r = new CodeRanges();
    return *r;
}

void track_add(std::uint64_t begin, std::uint64_t end) {
    auto& r = code_ranges();
    std::lock_guard lock(r.mutex);
    auto it = r.ranges.upper_bound(begin);
    if (it != r.ranges.begin()) {
        auto prev = std::prev(it);
        if (prev->second >= begin) { begin = prev->first; end = std::max(end, prev->second); it = r.ranges.erase(prev); }
    }
    while (it != r.ranges.end() && it->first <= end) { end = std::max(end, it->second); it = r.ranges.erase(it); }
    r.ranges.emplace(begin, end);
    r.any = true;
}

void track_remove(std::uint64_t begin, std::uint64_t end) {
    auto& r = code_ranges();
    std::lock_guard lock(r.mutex);
    std::map<std::uint64_t, std::uint64_t> keep;
    for (auto it = r.ranges.begin(); it != r.ranges.end();) {
        if (it->second <= begin || it->first >= end) { ++it; continue; }
        if (it->first < begin) keep.emplace(it->first, begin);
        if (it->second > end) keep.emplace(end, it->second);
        it = r.ranges.erase(it);
    }
    r.ranges.insert(keep.begin(), keep.end());
    r.any = !r.ranges.empty();
}

bool track_intersects(std::uint64_t begin, std::uint64_t end) {
    auto& r = code_ranges();
    if (!r.any) return false;
    std::lock_guard lock(r.mutex);
    auto it = r.ranges.upper_bound(begin);
    if (it != r.ranges.begin() && std::prev(it)->second > begin) return true;
    return it != r.ranges.end() && it->first < end;
}

bool track_contains(std::uint64_t begin, std::uint64_t end) {
    auto& r = code_ranges();
    if (!r.any) return false;
    std::lock_guard lock(r.mutex);
    auto it = r.ranges.upper_bound(begin);
    if (it == r.ranges.begin()) return false;
    --it;
    return it->first <= begin && it->second >= end;
}

// [AllocationBase, end of the allocation) containing `address`.
std::pair<std::uint64_t, std::uint64_t> allocation_extent(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.AllocationBase == nullptr)
        return {address, address};
    const auto base = reinterpret_cast<std::uint64_t>(mbi.AllocationBase);
    std::uint64_t cursor = base;
    while (VirtualQuery(reinterpret_cast<void*>(cursor), &mbi, sizeof(mbi)) == sizeof(mbi) &&
           reinterpret_cast<std::uint64_t>(mbi.AllocationBase) == base && mbi.RegionSize != 0) {
        cursor = reinterpret_cast<std::uint64_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return {base, cursor};
}

void became_code(std::uint64_t begin, std::uint64_t end) {
    if (end <= begin || find_guest_module(begin) || track_contains(begin, end)) return;
    register_guest_code_memory(begin, end - begin);
    track_add(begin, end);
}

void stopped_being_code(std::uint64_t begin, std::uint64_t end, bool unregister) {
    if (end <= begin) return;
    invalidate_guest_code(begin, end - begin);
    if (unregister && track_intersects(begin, end)) {
        unregister_guest_code_memory(begin, end - begin);
        track_remove(begin, end);
    }
}

// VirtualAlloc(address, size, type, protect)
R o_VirtualAlloc(GuestContext& ctx, const OverrideBinding& b) {
    const auto protect = static_cast<DWORD>(ctx.x[3]);
    using Fn = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
    const auto result = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
        reinterpret_cast<LPVOID>(ctx.x[0]), ctx.x[1], static_cast<DWORD>(ctx.x[2]), non_exec(protect)));
    if (result && is_exec(protect)) {
        const auto start = ctx.x[0] ? ctx.x[0] : result;
        became_code(page_down(result), page_up(start + ctx.x[1]));
    }
    ctx.x[0] = result;
    return kReturn;
}
// VirtualAllocEx(process, address, size, type, protect)
R o_VirtualAllocEx(GuestContext& ctx, const OverrideBinding& b) {
    const auto protect = static_cast<DWORD>(ctx.x[4]);
    const bool local = is_current_process(ctx.x[0]);
    using Fn = LPVOID(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, DWORD);
    const auto result = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
        reinterpret_cast<HANDLE>(ctx.x[0]), reinterpret_cast<LPVOID>(ctx.x[1]), ctx.x[2],
        static_cast<DWORD>(ctx.x[3]), local ? non_exec(protect) : protect));
    if (result && local && is_exec(protect)) {
        const auto start = ctx.x[1] ? ctx.x[1] : result;
        became_code(page_down(result), page_up(start + ctx.x[2]));
    }
    ctx.x[0] = result;
    return kReturn;
}
// VirtualAlloc2(process, base, size, type, protect, params, count)
R o_VirtualAlloc2(GuestContext& ctx, const OverrideBinding& b) {
    const auto protect = static_cast<DWORD>(ctx.x[4]);
    const bool local = ctx.x[0] == 0 || is_current_process(ctx.x[0]);
    using Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    const auto result = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
        reinterpret_cast<HANDLE>(ctx.x[0]), reinterpret_cast<PVOID>(ctx.x[1]), ctx.x[2], static_cast<ULONG>(ctx.x[3]),
        local ? non_exec(protect) : protect, reinterpret_cast<MEM_EXTENDED_PARAMETER*>(ctx.x[5]),
        static_cast<ULONG>(ctx.x[6])));
    if (result && local && is_exec(protect)) {
        const auto start = ctx.x[1] ? ctx.x[1] : result;
        became_code(page_down(result), page_up(start + ctx.x[2]));
    }
    ctx.x[0] = result;
    return kReturn;
}

void protect_common(GuestContext& ctx, std::uint64_t address, std::uint64_t size, DWORD protect,
                    std::uint64_t old_out, bool ok) {
    const auto begin = page_down(address);
    const auto end = page_up(address + std::max<std::uint64_t>(size, 1));
    (void)ctx;
    if (!ok) return;
    const bool was_code = is_guest_code(address) || track_intersects(begin, end);
    if (old_out && was_code) {
        auto* old = reinterpret_cast<DWORD*>(old_out);
        *old = exec_equivalent(*old);
    }
    if (was_code || is_exec(protect)) invalidate_guest_code(begin, end - begin);
    if (is_exec(protect)) became_code(begin, end);
    else if (was_code) stopped_being_code(begin, end, true);
}

// VirtualProtect(address, size, protect, old*)
R o_VirtualProtect(GuestContext& ctx, const OverrideBinding& b) {
    const auto protect = static_cast<DWORD>(ctx.x[2]);
    using Fn = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD, PDWORD);
    const BOOL ok = native_fn<Fn>(b)(reinterpret_cast<LPVOID>(ctx.x[0]), ctx.x[1], non_exec(protect),
                                     reinterpret_cast<PDWORD>(ctx.x[3]));
    protect_common(ctx, ctx.x[0], ctx.x[1], protect, ctx.x[3], ok != FALSE);
    ctx.x[0] = static_cast<std::uint32_t>(ok);
    return kReturn;
}
// VirtualProtectEx(process, address, size, protect, old*)
R o_VirtualProtectEx(GuestContext& ctx, const OverrideBinding& b) {
    const auto protect = static_cast<DWORD>(ctx.x[3]);
    const bool local = is_current_process(ctx.x[0]);
    using Fn = BOOL(WINAPI*)(HANDLE, LPVOID, SIZE_T, DWORD, PDWORD);
    const BOOL ok = native_fn<Fn>(b)(reinterpret_cast<HANDLE>(ctx.x[0]), reinterpret_cast<LPVOID>(ctx.x[1]),
                                     ctx.x[2], local ? non_exec(protect) : protect, reinterpret_cast<PDWORD>(ctx.x[4]));
    if (local) protect_common(ctx, ctx.x[1], ctx.x[2], protect, ctx.x[4], ok != FALSE);
    ctx.x[0] = static_cast<std::uint32_t>(ok);
    return kReturn;
}

// VirtualQuery(address, MEMORY_BASIC_INFORMATION*, length): report guest
// code with its executable protection.
void report_exec(std::uint64_t info, std::uint64_t written) {
    if (written < sizeof(MEMORY_BASIC_INFORMATION) || info == 0) return;
    auto* mbi = reinterpret_cast<MEMORY_BASIC_INFORMATION*>(info);
    if (mbi->State != MEM_COMMIT) return;
    const auto base = reinterpret_cast<std::uint64_t>(mbi->BaseAddress);
    if (!is_guest_code(base) && !track_intersects(base, base + mbi->RegionSize)) return;
    mbi->Protect = exec_equivalent(mbi->Protect);
    mbi->AllocationProtect = exec_equivalent(mbi->AllocationProtect);
}
R o_VirtualQuery(GuestContext& ctx, const OverrideBinding& b) {
    const auto written = call_native4(b, ctx);
    report_exec(ctx.x[1], written);
    ctx.x[0] = written;
    return kReturn;
}
R o_VirtualQueryEx(GuestContext& ctx, const OverrideBinding& b) {
    const auto written = call_native4(b, ctx);
    if (is_current_process(ctx.x[0])) report_exec(ctx.x[2], written);
    ctx.x[0] = written;
    return kReturn;
}

// VirtualFree(address, size, type)
R free_common(GuestContext& ctx, std::uint64_t address, std::uint64_t size, DWORD type, BOOL ok_value,
              const std::pair<std::uint64_t, std::uint64_t>& extent, bool tracked) {
    (void)address; (void)size; (void)type;
    if (ok_value && tracked) stopped_being_code(extent.first, extent.second, true);
    ctx.x[0] = static_cast<std::uint32_t>(ok_value);
    return kReturn;
}
std::pair<std::uint64_t, std::uint64_t> free_extent(std::uint64_t address, std::uint64_t size, DWORD type) {
    if ((type & MEM_RELEASE) || size == 0) return allocation_extent(address);
    return {page_down(address), page_up(address + size)};
}
R o_VirtualFree(GuestContext& ctx, const OverrideBinding& b) {
    const auto type = static_cast<DWORD>(ctx.x[2]);
    std::pair<std::uint64_t, std::uint64_t> extent{};
    bool tracked = false;
    if (code_ranges().any) {
        extent = free_extent(ctx.x[0], ctx.x[1], type);
        tracked = track_intersects(extent.first, extent.second);
    }
    const auto ok = static_cast<BOOL>(call_native4(b, ctx) & 0xffffffffULL);
    return free_common(ctx, ctx.x[0], ctx.x[1], type, ok, extent, tracked);
}
// VirtualFreeEx(process, address, size, type)
R o_VirtualFreeEx(GuestContext& ctx, const OverrideBinding& b) {
    const auto type = static_cast<DWORD>(ctx.x[3]);
    std::pair<std::uint64_t, std::uint64_t> extent{};
    bool tracked = false;
    if (code_ranges().any && is_current_process(ctx.x[0])) {
        extent = free_extent(ctx.x[1], ctx.x[2], type);
        tracked = track_intersects(extent.first, extent.second);
    }
    const auto ok = static_cast<BOOL>(call_native4(b, ctx) & 0xffffffffULL);
    return free_common(ctx, ctx.x[1], ctx.x[2], type, ok, extent, tracked);
}

constexpr DWORD kFileMapExecute = FILE_MAP_EXECUTE;

void mapped_view(std::uint64_t view, bool exec) {
    if (view == 0 || !exec) return;
    const auto extent = allocation_extent(view);
    became_code(extent.first, extent.second);
}
// MapViewOfFile(mapping, access, offset_high, offset_low, size)
R o_MapViewOfFile(GuestContext& ctx, const OverrideBinding& b) {
    const auto access = static_cast<DWORD>(ctx.x[1]);
    using Fn = LPVOID(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T);
    const auto view = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
        reinterpret_cast<HANDLE>(ctx.x[0]), access & ~kFileMapExecute, static_cast<DWORD>(ctx.x[2]),
        static_cast<DWORD>(ctx.x[3]), ctx.x[4]));
    mapped_view(view, (access & kFileMapExecute) != 0);
    ctx.x[0] = view;
    return kReturn;
}
// MapViewOfFileEx(mapping, access, offset_high, offset_low, size, base)
R o_MapViewOfFileEx(GuestContext& ctx, const OverrideBinding& b) {
    const auto access = static_cast<DWORD>(ctx.x[1]);
    using Fn = LPVOID(WINAPI*)(HANDLE, DWORD, DWORD, DWORD, SIZE_T, LPVOID);
    const auto view = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
        reinterpret_cast<HANDLE>(ctx.x[0]), access & ~kFileMapExecute, static_cast<DWORD>(ctx.x[2]),
        static_cast<DWORD>(ctx.x[3]), ctx.x[4], reinterpret_cast<LPVOID>(ctx.x[5])));
    mapped_view(view, (access & kFileMapExecute) != 0);
    ctx.x[0] = view;
    return kReturn;
}
// MapViewOfFile3(mapping, process, base, offset, size, type, protect, params, count)
R o_MapViewOfFile3(GuestContext& ctx, const OverrideBinding& b) {
    const auto protect = static_cast<DWORD>(ctx.x[6]);
    const bool local = ctx.x[1] == 0 || is_current_process(ctx.x[1]);
    using Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, MEM_EXTENDED_PARAMETER*, ULONG);
    const auto view = reinterpret_cast<std::uint64_t>(native_fn<Fn>(b)(
        reinterpret_cast<HANDLE>(ctx.x[0]), reinterpret_cast<HANDLE>(ctx.x[1]), reinterpret_cast<PVOID>(ctx.x[2]),
        ctx.x[3], ctx.x[4], static_cast<ULONG>(ctx.x[5]), local ? non_exec(protect) : protect,
        reinterpret_cast<MEM_EXTENDED_PARAMETER*>(ctx.x[7]), static_cast<ULONG>(guest_stack_arg(ctx, 0))));
    if (local) mapped_view(view, is_exec(protect));
    ctx.x[0] = view;
    return kReturn;
}

R unmap_common(GuestContext& ctx, const OverrideBinding& b, std::uint64_t view, bool local) {
    std::pair<std::uint64_t, std::uint64_t> extent{};
    bool tracked = false;
    if (local && code_ranges().any) {
        extent = allocation_extent(view);
        tracked = track_intersects(extent.first, extent.second);
    }
    const auto ok = call_native4(b, ctx) & 0xffffffffULL;
    if (ok && tracked) stopped_being_code(extent.first, extent.second, true);
    ctx.x[0] = ok;
    return kReturn;
}
R o_UnmapViewOfFile(GuestContext& ctx, const OverrideBinding& b) { return unmap_common(ctx, b, ctx.x[0], true); }
R o_UnmapViewOfFileEx(GuestContext& ctx, const OverrideBinding& b) { return unmap_common(ctx, b, ctx.x[0], true); }
R o_UnmapViewOfFile2(GuestContext& ctx, const OverrideBinding& b) {
    return unmap_common(ctx, b, ctx.x[1], is_current_process(ctx.x[0]));
}

// FlushInstructionCache(process, address, size)
R o_FlushInstructionCache(GuestContext& ctx, const OverrideBinding& b) {
    if (is_current_process(ctx.x[0])) {
        if (ctx.x[1] != 0) invalidate_guest_code(ctx.x[1], std::max<std::uint64_t>(ctx.x[2], 1));
        ctx.x[0] = TRUE;
        return kReturn;
    }
    ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    return kReturn;
}

// ============================================================================
// Processor identity: an ARM64 machine matching what fx executes
// ============================================================================

R o_IsProcessorFeaturePresent(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = guest_processor_feature(static_cast<std::uint32_t>(ctx.x[0])) ? TRUE : FALSE;
    return kReturn;
}

void patch_system_info(std::uint64_t address) {
    if (address == 0) return;
    auto* info = reinterpret_cast<SYSTEM_INFO*>(address);
    info->wProcessorArchitecture = PROCESSOR_ARCHITECTURE_ARM64;
    info->dwProcessorType = 0;
}
R o_GetSystemInfo(GuestContext& ctx, const OverrideBinding& b) {
    call_native4(b, ctx);
    patch_system_info(ctx.x[0]);
    return kReturn;
}

// IsWow64Process2(process, process_machine*, native_machine*)
R o_IsWow64Process2(GuestContext& ctx, const OverrideBinding&) {
    if (ctx.x[1]) *reinterpret_cast<USHORT*>(ctx.x[1]) = IMAGE_FILE_MACHINE_UNKNOWN;
    if (ctx.x[2]) *reinterpret_cast<USHORT*>(ctx.x[2]) = IMAGE_FILE_MACHINE_ARM64;
    ctx.x[0] = TRUE;
    return kReturn;
}
// GetMachineTypeAttributes(machine, MACHINE_ATTRIBUTES*)
R o_GetMachineTypeAttributes(GuestContext& ctx, const OverrideBinding&) {
    const auto machine = static_cast<USHORT>(ctx.x[0]);
    int attributes = 0;
    if (machine == IMAGE_FILE_MACHINE_ARM64) attributes = 1 | 2;            // UserEnabled | KernelEnabled
    else if (machine == IMAGE_FILE_MACHINE_AMD64) attributes = 1;            // x64 emulation
    else if (machine == IMAGE_FILE_MACHINE_I386) attributes = 1 | 4;         // UserEnabled | Wow64Container
    if (ctx.x[1]) *reinterpret_cast<int*>(ctx.x[1]) = attributes;
    ctx.x[0] = S_OK;
    return kReturn;
}

// ============================================================================
// Window procedures: user32 stores them in structures and window/class
// data, out of reach of the generic function-pointer wrapping. Wrapping
// them here turns every message into a direct trampoline call instead of an
// execute fault, and reads hand the guest back its own function.
// ============================================================================

constexpr const char* kWndProcSig = "l(pIll)";

std::uint64_t wrap_wndproc(std::uint64_t proc) {
    return wrap_callback_argument(proc, intern_signature(kWndProcSig));
}
std::uint64_t unwrap_wndproc(std::uint64_t proc) {
    return unwrap_callback_result(proc, intern_signature(kWndProcSig));
}

template <typename Class>
R register_class(GuestContext& ctx, const OverrideBinding& b) {
    if (ctx.x[0] == 0 || !b.native) { ctx.x[0] = call_native4(b, ctx) & 0xffff; return kReturn; }
    Class copy = *reinterpret_cast<const Class*>(ctx.x[0]);
    copy.lpfnWndProc = reinterpret_cast<WNDPROC>(wrap_wndproc(reinterpret_cast<std::uint64_t>(copy.lpfnWndProc)));
    using Fn = ATOM(WINAPI*)(const Class*);
    ctx.x[0] = native_fn<Fn>(b)(&copy);
    return kReturn;
}
R o_RegisterClassW(GuestContext& ctx, const OverrideBinding& b) { return register_class<WNDCLASSW>(ctx, b); }
R o_RegisterClassA(GuestContext& ctx, const OverrideBinding& b) { return register_class<WNDCLASSA>(ctx, b); }
R o_RegisterClassExW(GuestContext& ctx, const OverrideBinding& b) { return register_class<WNDCLASSEXW>(ctx, b); }
R o_RegisterClassExA(GuestContext& ctx, const OverrideBinding& b) { return register_class<WNDCLASSEXA>(ctx, b); }

// GetClassInfo{,Ex}{A,W}(instance, name, WNDCLASS*): hand back the guest procedure.
template <typename Class>
R get_class_info(GuestContext& ctx, const OverrideBinding& b) {
    ctx.x[0] = call_native4(b, ctx) & 0xffffffffULL;
    if (ctx.x[0] && ctx.x[2]) {
        auto* info = reinterpret_cast<Class*>(ctx.x[2]);
        info->lpfnWndProc = reinterpret_cast<WNDPROC>(unwrap_wndproc(reinterpret_cast<std::uint64_t>(info->lpfnWndProc)));
    }
    return kReturn;
}
R o_GetClassInfoW(GuestContext& ctx, const OverrideBinding& b) { return get_class_info<WNDCLASSW>(ctx, b); }
R o_GetClassInfoA(GuestContext& ctx, const OverrideBinding& b) { return get_class_info<WNDCLASSA>(ctx, b); }
R o_GetClassInfoExW(GuestContext& ctx, const OverrideBinding& b) { return get_class_info<WNDCLASSEXW>(ctx, b); }
R o_GetClassInfoExA(GuestContext& ctx, const OverrideBinding& b) { return get_class_info<WNDCLASSEXA>(ctx, b); }

// {Set,Get}{Window,Class}LongPtr{A,W}(hwnd, index[, value]) with
// GWLP_WNDPROC / GCLP_WNDPROC (and DWLP_DLGPROC for dialogs).
bool is_proc_index(std::uint64_t index, bool window) {
    const auto i = static_cast<int>(static_cast<std::int32_t>(index));
    return window ? (i == GWLP_WNDPROC) : (i == GCLP_WNDPROC);
}
R set_long_ptr(GuestContext& ctx, const OverrideBinding& b, bool window) {
    const bool proc = is_proc_index(ctx.x[1], window);
    if (proc) ctx.x[2] = wrap_wndproc(ctx.x[2]);
    ctx.x[0] = call_native4(b, ctx);
    if (proc) ctx.x[0] = unwrap_wndproc(ctx.x[0]);
    return kReturn;
}
R get_long_ptr(GuestContext& ctx, const OverrideBinding& b, bool window) {
    ctx.x[0] = is_proc_index(ctx.x[1], window) ? unwrap_wndproc(call_native4(b, ctx)) : call_native4(b, ctx);
    return kReturn;
}
R o_SetWindowLongPtr(GuestContext& ctx, const OverrideBinding& b) { return set_long_ptr(ctx, b, true); }
R o_GetWindowLongPtr(GuestContext& ctx, const OverrideBinding& b) { return get_long_ptr(ctx, b, true); }
R o_SetClassLongPtr(GuestContext& ctx, const OverrideBinding& b) { return set_long_ptr(ctx, b, false); }
R o_GetClassLongPtr(GuestContext& ctx, const OverrideBinding& b) { return get_long_ptr(ctx, b, false); }

// wglGetProcAddress(name): driver entry points have no export name, so the
// runtime would bind them with the positional '?' signature on first call.
// Hand the guest a typed thunk instead when the database knows the name.
R o_wglGetProcAddress(GuestContext& ctx, const OverrideBinding& b) {
    const auto* name = reinterpret_cast<const char*>(ctx.x[0]);
    const std::uint64_t proc = call_native4(b, ctx);
    ctx.x[0] = proc;
    // Some ICDs return small sentinels (1, 2, 3, -1) instead of NULL on failure.
    if (name == nullptr || proc <= 0xffff || proc == ~std::uint64_t{0}) return kReturn;
    if (lookup_signature_text(name) == nullptr) return kReturn;
    if (const auto thunk = bind_host_pointer(reinterpret_cast<const void*>(proc), lookup_signature(name)))
        ctx.x[0] = thunk;
    return kReturn;
}

// ============================================================================
// SEH (implemented by the seh component on the guest context)
// ============================================================================

R resolution(GuestResolution r) {
    return r == GuestResolution::continue_at_caller ? kReturn : kResume;
}

#define FX_SEH_OVERRIDE(api)     R o_##api(GuestContext& ctx, const OverrideBinding&) { return resolution(guest_##api(ctx)); }
FX_SEH_OVERRIDE(RaiseException)
FX_SEH_OVERRIDE(RtlCaptureContext)
FX_SEH_OVERRIDE(RtlRestoreContext)
FX_SEH_OVERRIDE(RtlVirtualUnwind)
FX_SEH_OVERRIDE(RtlLookupFunctionEntry)
FX_SEH_OVERRIDE(RtlUnwind)
FX_SEH_OVERRIDE(RtlUnwindEx)
FX_SEH_OVERRIDE(RtlAddFunctionTable)
FX_SEH_OVERRIDE(RtlDeleteFunctionTable)
FX_SEH_OVERRIDE(RtlInstallFunctionTableCallback)
FX_SEH_OVERRIDE(RtlAddGrowableFunctionTable)
FX_SEH_OVERRIDE(RtlGrowFunctionTable)
FX_SEH_OVERRIDE(RtlDeleteGrowableFunctionTable)
FX_SEH_OVERRIDE(SetUnhandledExceptionFilter)
FX_SEH_OVERRIDE(UnhandledExceptionFilter)
FX_SEH_OVERRIDE(AddVectoredExceptionHandler)
FX_SEH_OVERRIDE(AddVectoredContinueHandler)
FX_SEH_OVERRIDE(RemoveVectoredExceptionHandler)
FX_SEH_OVERRIDE(longjmp)
FX_SEH_OVERRIDE(_resetstkoflw)   // guest stack, not the host UCRT's
#undef FX_SEH_OVERRIDE

// RtlRaiseException(EXCEPTION_RECORD*): same dispatch as RaiseException.
R o_RtlRaiseException(GuestContext& ctx, const OverrideBinding&) {
    const auto* record = reinterpret_cast<const EXCEPTION_RECORD*>(ctx.x[0]);
    std::uint64_t args[15] = {};
    const auto count = std::min<DWORD>(record->NumberParameters, 15);
    for (DWORD i = 0; i < count; ++i) args[i] = record->ExceptionInformation[i];
    guest_raise_exception(ctx, record->ExceptionCode, record->ExceptionFlags, count, args);
    return kResume;
}

// ============================================================================
// Registry
// ============================================================================

struct OverrideEntry {
    const char* name;
    OverrideHandler handler;
};

// Sorted by name (strcmp order); checked at first use.
constexpr OverrideEntry kOverrides[] = {
    {"AddDllDirectory", o_AddDllDirectory},
    {"AddVectoredContinueHandler", o_AddVectoredContinueHandler},
    {"AddVectoredExceptionHandler", o_AddVectoredExceptionHandler},
    {"CreateProcessA", o_CreateProcessA},
    {"CreateProcessW", o_CreateProcessW},
    {"CreateRemoteThread", o_CreateRemoteThread},
    {"CreateRemoteThreadEx", o_CreateRemoteThreadEx},
    {"CreateThread", o_CreateThread},
    {"CreateThreadpoolIo", o_CreateThreadpoolObject},
    {"CreateThreadpoolTimer", o_CreateThreadpoolObject},
    {"CreateThreadpoolWait", o_CreateThreadpoolObject},
    {"CreateThreadpoolWork", o_CreateThreadpoolObject},
    {"DisableThreadLibraryCalls", o_DisableThreadLibraryCalls},
    {"EnumProcessModules", o_EnumProcessModules},
    {"EnumProcessModulesEx", o_EnumProcessModulesEx},
    {"ExitThread", o_ExitThread},
    {"FlushInstructionCache", o_FlushInstructionCache},
    {"FreeLibrary", o_FreeLibrary},
    {"FreeLibraryAndExitThread", o_FreeLibraryAndExitThread},
    {"FreeLibraryWhenCallbackReturns", o_FreeLibraryWhenCallbackReturns},
    {"GetClassInfoA", o_GetClassInfoA},
    {"GetClassInfoExA", o_GetClassInfoExA},
    {"GetClassInfoExW", o_GetClassInfoExW},
    {"GetClassInfoW", o_GetClassInfoW},
    {"GetClassLongPtrA", o_GetClassLongPtr},
    {"GetClassLongPtrW", o_GetClassLongPtr},
    {"GetCommandLineA", o_GetCommandLineA},
    {"GetCommandLineW", o_GetCommandLineW},
    {"GetCurrentThreadStackLimits", o_GetCurrentThreadStackLimits},
    {"GetMachineTypeAttributes", o_GetMachineTypeAttributes},
    {"GetModuleBaseNameA", o_GetModuleBaseNameA},
    {"GetModuleBaseNameW", o_GetModuleBaseNameW},
    {"GetModuleFileNameA", o_GetModuleFileNameA},
    {"GetModuleFileNameExA", o_GetModuleFileNameExA},
    {"GetModuleFileNameExW", o_GetModuleFileNameExW},
    {"GetModuleFileNameW", o_GetModuleFileNameW},
    {"GetModuleHandleA", o_GetModuleHandleA},
    {"GetModuleHandleExA", o_GetModuleHandleExA},
    {"GetModuleHandleExW", o_GetModuleHandleExW},
    {"GetModuleHandleW", o_GetModuleHandleW},
    {"GetModuleInformation", o_GetModuleInformation},
    {"GetNativeSystemInfo", o_GetSystemInfo},
    {"GetProcAddress", o_GetProcAddress},
    {"GetSystemInfo", o_GetSystemInfo},
    {"GetWindowLongPtrA", o_GetWindowLongPtr},
    {"GetWindowLongPtrW", o_GetWindowLongPtr},
    {"IsProcessorFeaturePresent", o_IsProcessorFeaturePresent},
    {"IsWow64Process2", o_IsWow64Process2},
    {"K32EnumProcessModules", o_EnumProcessModules},
    {"K32EnumProcessModulesEx", o_EnumProcessModulesEx},
    {"K32GetModuleBaseNameA", o_GetModuleBaseNameA},
    {"K32GetModuleBaseNameW", o_GetModuleBaseNameW},
    {"K32GetModuleFileNameExA", o_GetModuleFileNameExA},
    {"K32GetModuleFileNameExW", o_GetModuleFileNameExW},
    {"K32GetModuleInformation", o_GetModuleInformation},
    {"LoadLibraryA", o_LoadLibraryA},
    {"LoadLibraryExA", o_LoadLibraryExA},
    {"LoadLibraryExW", o_LoadLibraryExW},
    {"LoadLibraryW", o_LoadLibraryW},
    {"MapViewOfFile", o_MapViewOfFile},
    {"MapViewOfFile3", o_MapViewOfFile3},
    {"MapViewOfFileEx", o_MapViewOfFileEx},
    {"QueueUserAPC", o_QueueUserAPC},
    {"QueueUserWorkItem", o_QueueUserWorkItem},
    {"RaiseException", o_RaiseException},
    {"RegisterClassA", o_RegisterClassA},
    {"RegisterClassExA", o_RegisterClassExA},
    {"RegisterClassExW", o_RegisterClassExW},
    {"RegisterClassW", o_RegisterClassW},
    {"RemoveDllDirectory", o_RemoveDllDirectory},
    {"RemoveVectoredContinueHandler", o_RemoveVectoredExceptionHandler},
    {"RemoveVectoredExceptionHandler", o_RemoveVectoredExceptionHandler},
    {"ResolveDelayLoadedAPI", o_ResolveDelayLoadedAPI},
    {"RtlAddFunctionTable", o_RtlAddFunctionTable},
    {"RtlAddGrowableFunctionTable", o_RtlAddGrowableFunctionTable},
    {"RtlAddVectoredContinueHandler", o_AddVectoredContinueHandler},
    {"RtlAddVectoredExceptionHandler", o_AddVectoredExceptionHandler},
    {"RtlCaptureContext", o_RtlCaptureContext},
    {"RtlDeleteFunctionTable", o_RtlDeleteFunctionTable},
    {"RtlDeleteGrowableFunctionTable", o_RtlDeleteGrowableFunctionTable},
    {"RtlGrowFunctionTable", o_RtlGrowFunctionTable},
    {"RtlInstallFunctionTableCallback", o_RtlInstallFunctionTableCallback},
    {"RtlLookupFunctionEntry", o_RtlLookupFunctionEntry},
    {"RtlPcToFileHeader", o_RtlPcToFileHeader},
    {"RtlQueueWorkItem", o_RtlQueueWorkItem},
    {"RtlRaiseException", o_RtlRaiseException},
    {"RtlRemoveVectoredContinueHandler", o_RemoveVectoredExceptionHandler},
    {"RtlRemoveVectoredExceptionHandler", o_RemoveVectoredExceptionHandler},
    {"RtlRestoreContext", o_RtlRestoreContext},
    {"RtlUnwind", o_RtlUnwind},
    {"RtlUnwindEx", o_RtlUnwindEx},
    {"RtlVirtualUnwind", o_RtlVirtualUnwind},
    {"SetClassLongPtrA", o_SetClassLongPtr},
    {"SetClassLongPtrW", o_SetClassLongPtr},
    {"SetDllDirectoryA", o_SetDllDirectoryA},
    {"SetDllDirectoryW", o_SetDllDirectoryW},
    {"SetThreadStackGuarantee", o_SetThreadStackGuarantee},
    {"SetUnhandledExceptionFilter", o_SetUnhandledExceptionFilter},
    {"SetWindowLongPtrA", o_SetWindowLongPtr},
    {"SetWindowLongPtrW", o_SetWindowLongPtr},
    {"UnhandledExceptionFilter", o_UnhandledExceptionFilter},
    {"UnmapViewOfFile", o_UnmapViewOfFile},
    {"UnmapViewOfFile2", o_UnmapViewOfFile2},
    {"UnmapViewOfFileEx", o_UnmapViewOfFileEx},
    {"VirtualAlloc", o_VirtualAlloc},
    {"VirtualAlloc2", o_VirtualAlloc2},
    {"VirtualAllocEx", o_VirtualAllocEx},
    {"VirtualFree", o_VirtualFree},
    {"VirtualFreeEx", o_VirtualFreeEx},
    {"VirtualProtect", o_VirtualProtect},
    {"VirtualProtectEx", o_VirtualProtectEx},
    {"VirtualQuery", o_VirtualQuery},
    {"VirtualQueryEx", o_VirtualQueryEx},
    {"_resetstkoflw", o__resetstkoflw},
    {"longjmp", o_longjmp},
    {"wglGetProcAddress", o_wglGetProcAddress},
};

} // namespace

bool guest_processor_feature(std::uint32_t feature) noexcept {
    const auto& host = cpu_features();
    switch (feature) {
    case PF_COMPARE_EXCHANGE_DOUBLE:
    case PF_NX_ENABLED:
    case PF_COMPARE_EXCHANGE128:
    case PF_FASTFAIL_AVAILABLE:
    case PF_ARM_VFP_32_REGISTERS_AVAILABLE:
    case PF_ARM_NEON_INSTRUCTIONS_AVAILABLE:
    case PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE:
    case PF_ARM_64BIT_LOADSTORE_ATOMIC:
    case PF_ARM_FMAC_INSTRUCTIONS_AVAILABLE:
    case PF_ARM_V8_INSTRUCTIONS_AVAILABLE:
    case PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE:     // ARMv8.1 baseline of Windows 11 on ARM
    case PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE:   // LSE -> lock-prefixed x64
    case PF_ARM_V83_LRCPC_INSTRUCTIONS_AVAILABLE:    // LDAPR/LDAPUR/STLUR
    case PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE:       // SDOT/UDOT
        return true;
    case PF_ARM_V8_CRYPTO_INSTRUCTIONS_AVAILABLE:    // AES/PMULL/SHA1/SHA256
        return host.aes && host.pclmul && host.sha;
    case PF_SECOND_LEVEL_ADDRESS_TRANSLATION:
    case PF_VIRT_FIRMWARE_ENABLED:
        return IsProcessorFeaturePresent(feature) != FALSE;
    default:
        // x86 features (MMX, SSE*, AVX*, RDTSC, ...) and ARM extensions fx
        // does not execute (SVE, SME, BF16, LSE2) are absent. JSCVT, FP16
        // (with FHM), I8MM, SHA3 and SHA512 have translator emitters but no
        // interpreter fallback (FP16 also needs host F16C), so they stay
        // unreported until the translator guarantees them: guests (numpy,
        // OIDN) pick code paths from these bits.
        return false;
    }
}

void sync_fpcr_from_mxcsr(GuestContext& ctx) noexcept {
    const unsigned csr = _mm_getcsr();
    static constexpr std::uint32_t kRmode[4] = {0, 2, 1, 3};   // MXCSR RC -> FPCR RMode
    std::uint32_t fpcr = ctx.fpcr & ~((3u << 22) | (1u << 24) | 0x9f00u);
    fpcr |= kRmode[(csr >> 13) & 3] << 22;
    if (csr & ((1u << 15) | (1u << 6))) fpcr |= 1u << 24;    // FTZ/DAZ -> FZ
    // Unmasked MXCSR exceptions -> FPCR trap enables (IOE, IDE, DZE, OFE, UFE, IXE).
    if (!(csr & (1u << 7))) fpcr |= 1u << 8;
    if (!(csr & (1u << 9))) fpcr |= 1u << 9;
    if (!(csr & (1u << 10))) fpcr |= 1u << 10;
    if (!(csr & (1u << 11))) fpcr |= 1u << 11;
    if (!(csr & (1u << 12))) fpcr |= 1u << 12;
    if (!(csr & (1u << 8))) fpcr |= 1u << 15;
    ctx.fpcr = fpcr;
    _mm_setcsr(csr | 0x1f80u);   // translated code runs with exceptions masked
}

static_assert(std::is_sorted(std::begin(kOverrides), std::end(kOverrides),
                             [](const OverrideEntry& a, const OverrideEntry& b) {
                                 return std::string_view(a.name) < std::string_view(b.name);
                             }),
              "kOverrides must be sorted by name");

OverrideHandler find_override(std::string_view dll, std::string_view name) noexcept {
    (void)dll;   // names are unique across the DLLs we override; the CRT table is shared
    const auto* it = std::lower_bound(std::begin(kOverrides), std::end(kOverrides), name,
        [](const OverrideEntry& e, std::string_view key) { return std::string_view(e.name) < key; });
    if (it != std::end(kOverrides) && std::string_view(it->name) == name) return it->handler;
    if (const auto handler = find_crt_override(name)) return handler;
    return find_fiber_override(name);
}

std::size_t override_count() noexcept {
    return std::size(kOverrides) + crt_override_count() + fiber_override_count();
}

} // namespace transmute::fx::abi
