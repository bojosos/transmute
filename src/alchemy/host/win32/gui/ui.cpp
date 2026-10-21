// Win32 host backend, area "ui" (include/alchemy/host/ui.hpp): the display
// configuration (QueryDisplayConfig, EDID ids), input idle time, the
// keyboard type, the accessibility settings and high-resolution waitable
// timers.
#include "alchemy/host/ui.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#pragma comment(lib, "user32.lib")

namespace alchemy::host::ui {

Result<std::vector<Monitor>> monitors() {
    UINT32 path_count{}, mode_count{};
    if (const auto status = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count); status != ERROR_SUCCESS)
        return win32::error(static_cast<DWORD>(status));
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (const auto status = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr);
        status != ERROR_SUCCESS)
        return win32::error(static_cast<DWORD>(status));
    paths.resize(path_count);
    std::vector<Monitor> result;
    for (const auto& path : paths) {
        Monitor monitor;
        const auto source_index = path.sourceInfo.modeInfoIdx;
        if (source_index < mode_count && modes[source_index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const auto& source = modes[source_index].sourceMode;
            monitor.left = source.position.x;
            monitor.top = source.position.y;
            monitor.width = source.width;
            monitor.height = source.height;
            monitor.primary = source.position.x == 0 && source.position.y == 0;
        }
        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof name;
        name.header.adapterId = path.targetInfo.adapterId;
        name.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&name.header) == ERROR_SUCCESS && name.flags.edidIdsValid) {
            // edidManufactureId holds EDID bytes 8-9 in little-endian order.
            monitor.edid_vendor = static_cast<std::uint16_t>((name.edidManufactureId >> 8) | (name.edidManufactureId << 8));
            monitor.edid_product = name.edidProductCodeId;
        }
        result.push_back(monitor);
    }
    return result;
}

Result<PixelSize> window_client_size(std::uintptr_t window) {
    RECT client{};
    const auto handle = reinterpret_cast<HWND>(window);
    if (!IsWindow(handle) || !GetClientRect(handle, &client)) return Error::not_found;
    return PixelSize{static_cast<std::uint32_t>(client.right - client.left), static_cast<std::uint32_t>(client.bottom - client.top)};
}

Result<std::uint32_t> idle_ms() {
    LASTINPUTINFO info{sizeof(LASTINPUTINFO), 0};
    if (!GetLastInputInfo(&info)) return win32::last_error();
    return static_cast<std::uint32_t>(GetTickCount() - info.dwTime);
}

Result<KeyboardKind> keyboard_kind() {
    if (GetKeyboardType(0) == 7) return KeyboardKind::jis;   // the Japanese keyboard
    if (PRIMARYLANGID(LOWORD(reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(0)))) != LANG_ENGLISH) return KeyboardKind::iso;
    return KeyboardKind::ansi;
}

Result<bool> reduce_motion() {
    BOOL animations = TRUE;
    if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0)) return win32::last_error();
    return !animations;
}

Result<bool> high_contrast() {
    HIGHCONTRASTW contrast{sizeof(contrast)};
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) return win32::last_error();
    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

Result<FrameTimer> FrameTimer::create() {
    const auto timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) return win32::last_error();
    FrameTimer result;
    result.handle_.reset(win32::wrap(timer));
    return result;
}

FrameTimer::FrameTimer() = default;
FrameTimer::FrameTimer(FrameTimer&&) noexcept = default;
FrameTimer& FrameTimer::operator=(FrameTimer&&) noexcept = default;
FrameTimer::~FrameTimer() = default;

Status FrameTimer::wait(std::uint64_t delay_ns, std::uint32_t limit_ms) {
    if (!handle_) return Error::bad_handle;
    LARGE_INTEGER due{};
    due.QuadPart = -static_cast<LONGLONG>(delay_ns / 100);   // relative, 100 ns units
    if (!SetWaitableTimer(win32::native(handle_.get()), &due, 0, nullptr, nullptr, FALSE)) return win32::last_error();
    WaitForSingleObject(win32::native(handle_.get()), limit_ms);
    return {};
}

Result<std::vector<StackFrame>> sample_thread_stack(std::uint64_t thread_id, std::size_t max_frames) {
    const auto thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                   static_cast<DWORD>(thread_id));
    if (!thread) return Error::not_found;
    const OwnedHandle owned(win32::wrap(thread));
    DWORD code = 0;
    if (GetExitCodeThread(thread, &code) && code != STILL_ACTIVE) return Error::shut_down;
    std::vector<std::uint64_t> addresses;
    if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(thread, &context)) {
            for (std::size_t depth = 0; depth < max_frames && context.Rip; ++depth) {
                addresses.push_back(context.Rip);
                DWORD64 base = 0;
                const auto entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
                if (!entry) break;
                void* handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, entry, &context, &handler_data, &establisher, nullptr);
            }
        }
        ResumeThread(thread);
    }
    std::vector<StackFrame> frames;
    for (const auto address : addresses) {
        StackFrame frame{address};
        HMODULE module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module) && module) {
            char name[MAX_PATH] = "?";
            GetModuleFileNameA(module, name, MAX_PATH);
            const char* base_name = std::strrchr(name, '\\');
            frame.module = base_name ? base_name + 1 : name;
            frame.offset = address - reinterpret_cast<std::uint64_t>(module);
        }
        frames.push_back(std::move(frame));
    }
    return frames;
}

} // namespace alchemy::host::ui
