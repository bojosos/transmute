#include "alchemy/host/win32/gui/window_server.hpp"

#include "alchemy/host/win32/gui/clipboard_codec.hpp"

#include <ole2.h>
#include <shellapi.h>
#include <ShellScalingApi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cwctype>
#include <cmath>
#include <cstring>

// Cursors, clipboard, screens, keyboard layouts, file drops (owner: A3).
namespace alchemy::gui::win32 {
namespace {

// ---- clipboard ------------------------------------------------------------------

class ClipboardLease {
public:
    explicit ClipboardLease(HWND owner) {
        // Another process may hold the clipboard for a moment.
        for (int attempt = 0; attempt < 20 && !open_; ++attempt) {
            open_ = OpenClipboard(owner) != FALSE;
            if (!open_) Sleep(5);
        }
        if (!open_) fail_win32("OpenClipboard");
    }
    ~ClipboardLease() {
        if (open_) CloseClipboard();
    }
    ClipboardLease(const ClipboardLease&) = delete;
    ClipboardLease& operator=(const ClipboardLease&) = delete;

private:
    bool open_{};
};

class GlobalLease {
public:
    explicit GlobalLease(HANDLE handle) : handle_(handle), data_(handle ? GlobalLock(handle) : nullptr) {}
    ~GlobalLease() {
        if (data_) GlobalUnlock(handle_);
    }
    GlobalLease(const GlobalLease&) = delete;
    GlobalLease& operator=(const GlobalLease&) = delete;
    [[nodiscard]] const void* data() const noexcept { return data_; }
    [[nodiscard]] std::size_t size() const noexcept { return data_ ? GlobalSize(handle_) : 0; }

private:
    HANDLE handle_;
    void* data_;
};

void put_clipboard(UINT format, const void* data, std::size_t size) {
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory == nullptr) fail_win32("GlobalAlloc (clipboard)");
    if (auto* target = GlobalLock(memory)) {
        std::memcpy(target, data, size);
        GlobalUnlock(memory);
    } else {
        GlobalFree(memory);
        fail_win32("GlobalLock (clipboard)");
    }
    if (SetClipboardData(format, memory) == nullptr) {
        GlobalFree(memory);
        fail_win32("SetClipboardData");
    }
}

// ---- cursors --------------------------------------------------------------------

HCURSOR make_cursor(const Image& image, Point hot_spot, double scale) {
    const auto width = std::max<LONG>(1, std::lround(image.width * scale));
    const auto height = std::max<LONG>(1, std::lround(image.height * scale));
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = width;
    header.bV5Height = -height;   // top-down
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    const auto screen = GetDC(nullptr);
    const auto color = CreateDIBSection(screen, reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits,
                                        nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (color == nullptr || bits == nullptr) fail_win32("CreateDIBSection (cursor)");
    auto* pixels = static_cast<std::uint8_t*>(bits);
    for (LONG y = 0; y < height; ++y) {
        const auto source_y = std::min<std::uint32_t>(image.height - 1, static_cast<std::uint32_t>(y / scale));
        for (LONG x = 0; x < width; ++x) {
            const auto source_x = std::min<std::uint32_t>(image.width - 1, static_cast<std::uint32_t>(x / scale));
            const auto* source = image.rgba.data() + (static_cast<std::size_t>(source_y) * image.width + source_x) * 4;
            auto* target = pixels + (static_cast<std::size_t>(y) * width + x) * 4;
            target[0] = source[2];
            target[1] = source[1];
            target[2] = source[0];
            target[3] = source[3];
        }
    }
    const auto mask = CreateBitmap(width, height, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = FALSE;
    info.xHotspot = static_cast<DWORD>(std::clamp<LONG>(std::lround(hot_spot.x * scale), 0, width - 1));
    info.yHotspot = static_cast<DWORD>(std::clamp<LONG>(std::lround(hot_spot.y * scale), 0, height - 1));
    info.hbmMask = mask;
    info.hbmColor = color;
    const auto cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    if (cursor == nullptr) fail_win32("CreateIconIndirect (cursor)");
    return cursor;
}

// ---- screens --------------------------------------------------------------------

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto& list = *reinterpret_cast<std::vector<MonitorPixels>*>(context);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    UINT dpi_x = USER_DEFAULT_SCREEN_DPI, dpi_y = USER_DEFAULT_SCREEN_DPI;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y)) || dpi_x == 0)
        dpi_x = USER_DEFAULT_SCREEN_DPI;
    MonitorPixels pixels;
    pixels.bounds = {info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom};
    pixels.work = {info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
    pixels.scale = static_cast<double>(dpi_x) / USER_DEFAULT_SCREEN_DPI;
    pixels.handle = reinterpret_cast<std::uintptr_t>(monitor);
    if (info.dwFlags & MONITORINFOF_PRIMARY) list.insert(list.begin(), pixels);
    else list.push_back(pixels);
    return TRUE;
}

std::wstring monitor_device(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return {};
    return info.szDevice;
}

// GDI device name ("\\.\DISPLAY1") -> monitor friendly name (EDID).
std::vector<std::pair<std::wstring, std::wstring>> friendly_names() {
    std::vector<std::pair<std::wstring, std::wstring>> names;
    UINT32 path_count = 0, mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) return names;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count, modes.data(), nullptr) !=
        ERROR_SUCCESS)
        return names;
    paths.resize(path_count);
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS &&
            DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS && target.monitorFriendlyDeviceName[0])
            names.emplace_back(source.viewGdiDeviceName, target.monitorFriendlyDeviceName);
    }
    return names;
}

// ---- keyboard layouts -------------------------------------------------------------

struct LayoutName {
    const wchar_t* klid;
    const char* identifier;
};

// Windows KLIDs -> the TISInputSource ids of the corresponding Apple
// layouts (so applications keying behavior on well-known ids see familiar
// values). Unlisted layouts get com.alchemy.keylayout.win32.<KLID>.
constexpr LayoutName layout_names[] = {
    {L"00000409", "com.apple.keylayout.US"},           {L"00020409", "com.apple.keylayout.USInternational-PC"},
    {L"00010409", "com.apple.keylayout.Dvorak"},       {L"00000809", "com.apple.keylayout.British"},
    {L"00000407", "com.apple.keylayout.German"},       {L"0000040C", "com.apple.keylayout.French"},
    {L"0000080C", "com.apple.keylayout.Belgian"},      {L"00000813", "com.apple.keylayout.Belgian"},
    {L"00000410", "com.apple.keylayout.Italian"},      {L"0000040A", "com.apple.keylayout.Spanish-ISO"},
    {L"0000080A", "com.apple.keylayout.LatinAmerican"}, {L"00000816", "com.apple.keylayout.Portuguese"},
    {L"00000416", "com.apple.keylayout.Brazilian-Pro"}, {L"00000807", "com.apple.keylayout.SwissGerman"},
    {L"0000100C", "com.apple.keylayout.SwissFrench"},  {L"00000406", "com.apple.keylayout.Danish"},
    {L"00000414", "com.apple.keylayout.Norwegian"},    {L"0000041D", "com.apple.keylayout.Swedish-Pro"},
    {L"0000040B", "com.apple.keylayout.Finnish"},      {L"00000413", "com.apple.keylayout.Dutch"},
    {L"00000415", "com.apple.keylayout.PolishPro"},    {L"00000405", "com.apple.keylayout.Czech"},
    {L"0000040E", "com.apple.keylayout.Hungarian"},    {L"00000408", "com.apple.keylayout.Greek"},
    {L"0000041F", "com.apple.keylayout.Turkish-QWERTY-PC"}, {L"00000419", "com.apple.keylayout.RussianWin"},
    {L"00000402", "com.apple.keylayout.Bulgarian"},    {L"00000422", "com.apple.keylayout.Ukrainian"},
    {L"00000411", "com.apple.keylayout.ABC"},          {L"00000412", "com.apple.keylayout.ABC"},
    {L"00000804", "com.apple.keylayout.ABC"},          {L"00000404", "com.apple.keylayout.ABC"},
};

std::uint32_t pack_dead_key(UINT virtual_key, std::uint64_t flags) {
    std::uint32_t value = virtual_key & 0xffffU;
    if (flags & modifier::shift) value |= 1U << 16;
    if (flags & modifier::option) value |= 1U << 17;
    if (flags & modifier::caps_lock) value |= 1U << 18;
    if (flags & modifier::control) value |= 1U << 19;
    return value;
}

void key_state_for(BYTE (&state)[256], std::uint64_t flags) {
    std::memset(state, 0, sizeof(state));
    if (flags & modifier::shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (flags & modifier::caps_lock) state[VK_CAPITAL] = 0x01;
    if (flags & modifier::control) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    // Option selects the layout's third level: AltGr (Ctrl+Alt) on Windows.
    if (flags & modifier::option) {
        state[VK_MENU] = state[VK_RMENU] = 0x80;
        state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
    }
}

std::uint64_t unpack_dead_flags(std::uint32_t value) {
    std::uint64_t flags = 0;
    if (value & (1U << 16)) flags |= modifier::shift;
    if (value & (1U << 17)) flags |= modifier::option;
    if (value & (1U << 18)) flags |= modifier::caps_lock;
    if (value & (1U << 19)) flags |= modifier::control;
    return flags;
}

// ---- file drops ------------------------------------------------------------------

} // namespace

std::vector<std::string> dropped_paths(HANDLE handle) {
    const auto drop = static_cast<HDROP>(handle);
    std::vector<std::string> paths;
    const auto count = DragQueryFileW(drop, 0xffffffff, nullptr, 0);
    for (UINT index = 0; index < count; ++index) {
        const auto length = DragQueryFileW(drop, index, nullptr, 0);
        std::wstring path(length + 1, L'\0');
        DragQueryFileW(drop, index, path.data(), length + 1);
        path.resize(length);
        paths.push_back(wide_to_utf8(path));
    }
    return paths;
}

namespace {

FORMATETC hdrop_format() { return {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}; }

std::vector<std::string> data_object_paths(IDataObject* data) {
    std::vector<std::string> paths;
    auto format = hdrop_format();
    STGMEDIUM medium{};
    if (data == nullptr || data->GetData(&format, &medium) != S_OK) return paths;
    if (const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal))) {
        paths = dropped_paths(drop);
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return paths;
}

} // namespace

std::uint64_t drag_operations_for(DWORD allowed_effects, DWORD key_state) {
    std::uint64_t operations = drag_operation::none;
    if (allowed_effects & DROPEFFECT_COPY) operations |= drag_operation::copy;
    if (allowed_effects & DROPEFFECT_LINK) operations |= drag_operation::link;
    if (allowed_effects & DROPEFFECT_MOVE) operations |= drag_operation::move | drag_operation::generic;
    // AppKit narrows the source mask by the modifier keys; on Windows the
    // Explorer convention applies: Ctrl copies, Shift moves, Ctrl+Shift or
    // Alt links.
    const bool control = (key_state & MK_CONTROL) != 0, shift = (key_state & MK_SHIFT) != 0;
    if ((control && shift) || (key_state & MK_ALT)) operations &= drag_operation::link;
    else if (control) operations &= drag_operation::copy;
    else if (shift) operations &= drag_operation::move | drag_operation::generic;
    return operations;
}

DWORD drop_effect_for(std::uint64_t operations) {
    if (operations & drag_operation::copy) return DROPEFFECT_COPY;
    if (operations & drag_operation::link) return DROPEFFECT_LINK;
    if (operations & (drag_operation::move | drag_operation::generic)) return DROPEFFECT_MOVE;
    return DROPEFFECT_NONE;
}

namespace {

// OLE drop target for CF_HDROP (Explorer file drags): drag_entered,
// drag_updated, drag_exited and files_dropped. Callbacks arrive from OLE's
// window messages inside dispatch_messages, so sink exceptions are fenced
// here like in the WndProc.
class DropTarget final : public IDropTarget {
public:
    DropTarget(Win32WindowServer& server, WindowId window) : server_(server), window_(window) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) return E_POINTER;
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(IDropTarget))) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD key_state, POINTL point, DWORD* effect) override {
        if (effect == nullptr) return E_INVALIDARG;
        reset();
        auto format = hdrop_format();
        if (data == nullptr || data->QueryGetData(&format) != S_OK || !server_.accepting_drops(window_)) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        paths_ = data_object_paths(data);
        if (paths_.empty()) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        dragging_ = true;
        return report(Notification::drag_entered, key_state, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD key_state, POINTL point, DWORD* effect) override {
        if (effect == nullptr) return E_INVALIDARG;
        if (!dragging_) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        return report(Notification::drag_updated, key_state, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (dragging_) {
            try {
                static_cast<void>(server_.deliver_drag(window_, Notification::drag_exited, paths_, point_, operations_));
            } catch (...) {
                server_.stash_exception();
            }
        }
        reset();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD key_state, POINTL point, DWORD* effect) override {
        if (effect == nullptr) return E_INVALIDARG;
        if (!dragging_) {
            *effect = DROPEFFECT_NONE;
            return S_OK;
        }
        // The answer of the last drag_entered / drag_updated decides; the
        // key state may have changed since.
        const auto operations = drag_operations_for(*effect, key_state);
        const auto chosen = drop_effect_for(operations & accepted_);
        auto paths = data_object_paths(data);
        if (paths.empty()) paths = paths_;
        const POINT screen{point.x, point.y};
        try {
            if (chosen == DROPEFFECT_NONE)
                static_cast<void>(server_.deliver_drag(window_, Notification::drag_exited, paths, screen, operations));
            else
                server_.deliver_drop(window_, std::move(paths), screen, operations);
        } catch (...) {
            server_.stash_exception();
        }
        *effect = chosen;
        reset();
        return S_OK;
    }

private:
    HRESULT report(Notification notification, DWORD key_state, POINTL point, DWORD* effect) {
        point_ = {point.x, point.y};
        operations_ = drag_operations_for(*effect, key_state);
        accepted_ = drag_operation::none;
        try {
            accepted_ = server_.deliver_drag(window_, notification, paths_, point_, operations_);
        } catch (...) {
            server_.stash_exception();
            accepted_ = drag_operation::none;
        }
        *effect = drop_effect_for(operations_ & accepted_);
        return S_OK;
    }
    void reset() {
        dragging_ = false;
        paths_.clear();
        operations_ = accepted_ = drag_operation::none;
    }

    Win32WindowServer& server_;
    WindowId window_;
    std::atomic<ULONG> references_{1};
    bool dragging_{};
    std::vector<std::string> paths_;
    POINT point_{};
    std::uint64_t operations_{};   // what the source allows (after modifier keys)
    std::uint64_t accepted_{};     // the window's last answer
};

} // namespace

// ---- screens --------------------------------------------------------------------

std::vector<MonitorPixels> enumerate_monitors() {
    std::vector<MonitorPixels> monitors;
    EnumDisplayMonitors(nullptr, nullptr, collect_monitor, reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}

ScreenId screen_id_for_monitor(HMONITOR monitor) {
    // CGDirectDisplayID: the GDI display number ("\\.\DISPLAY2" -> 2), stable
    // while the display stays attached.
    const auto device = monitor_device(monitor);
    std::uint32_t number = 0;
    std::size_t index = device.size();
    while (index > 0 && device[index - 1] >= L'0' && device[index - 1] <= L'9') --index;
    for (auto at = index; at < device.size() && number < 100000; ++at) number = number * 10 + (device[at] - L'0');
    if (number != 0) return number;
    std::uint32_t hash = 2166136261U;   // FNV-1a of the device name
    for (const auto unit : device) hash = (hash ^ static_cast<std::uint32_t>(unit)) * 16777619U;
    return hash | 0x80000000U;
}

std::vector<ScreenInfo> Win32WindowServer::screens() const {
    // Any thread.
    if (!interactive_) return {};
    std::uint64_t generation{};
    {
        std::lock_guard lock(mutex_);
        if (screens_cache_) return *screens_cache_;
        generation = screens_generation_;
    }
    ensure_dpi_awareness();
    ThreadDpiScope scope;
    const auto monitors = enumerate_monitors();
    const ScreenLayout screen_layout(monitors);
    const auto names = friendly_names();
    std::vector<ScreenInfo> result;
    for (std::size_t index = 0; index < monitors.size(); ++index) {
        const auto handle = reinterpret_cast<HMONITOR>(monitors[index].handle);
        const auto device = monitor_device(handle);
        ScreenInfo info;
        info.id = screen_id_for_monitor(handle);
        info.frame = screen_layout.frame(index);
        info.visible_frame = screen_layout.visible_frame(index);
        info.backing_scale = monitors[index].scale;
        info.pixel_width = static_cast<std::uint32_t>(monitors[index].bounds.width());
        info.pixel_height = static_cast<std::uint32_t>(monitors[index].bounds.height());
        info.primary = index == 0;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!device.empty() && EnumDisplaySettingsW(device.c_str(), ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency > 1)
            info.refresh_rate = mode.dmDisplayFrequency;
        if (!device.empty()) {
            if (const auto context = CreateDCW(L"DISPLAY", device.c_str(), nullptr, nullptr)) {
                info.millimeters_width = GetDeviceCaps(context, HORZSIZE);
                info.millimeters_height = GetDeviceCaps(context, VERTSIZE);
                DeleteDC(context);
            }
        }
        for (const auto& [gdi, friendly] : names)
            if (gdi == device) info.name = wide_to_utf8(friendly);
        if (info.name.empty()) info.name = "Display " + std::to_string(info.id);
        result.push_back(std::move(info));
    }
    // Cached only while the application window observes display changes
    // (invalidate_layout drops it), as the layout cache.
    if (initialized_) {
        std::lock_guard lock(mutex_);
        if (generation == screens_generation_) screens_cache_ = result;
    }
    return result;
}

// ---- cursors --------------------------------------------------------------------

CursorId Win32WindowServer::system_cursor(SystemCursor cursor) {
    require_main_thread("system_cursor");
    if (const auto found = system_cursors_.find(cursor); found != system_cursors_.end()) return found->second;
    // Windows has no open/closed hand, drag link/copy or contextual-menu
    // cursors; the nearest system shapes stand in for them.
    LPCWSTR shape = IDC_ARROW;
    switch (cursor) {
    case SystemCursor::arrow: shape = IDC_ARROW; break;
    case SystemCursor::ibeam: case SystemCursor::ibeam_vertical: shape = IDC_IBEAM; break;
    case SystemCursor::crosshair: shape = IDC_CROSS; break;
    case SystemCursor::closed_hand: case SystemCursor::open_hand: shape = IDC_SIZEALL; break;
    case SystemCursor::pointing_hand: shape = IDC_HAND; break;
    case SystemCursor::resize_left: case SystemCursor::resize_right: case SystemCursor::resize_left_right:
        shape = IDC_SIZEWE; break;
    case SystemCursor::resize_up: case SystemCursor::resize_down: case SystemCursor::resize_up_down:
        shape = IDC_SIZENS; break;
    case SystemCursor::disappearing_item: case SystemCursor::operation_not_allowed: shape = IDC_NO; break;
    case SystemCursor::drag_link: case SystemCursor::drag_copy: case SystemCursor::contextual_menu:
        shape = IDC_ARROW; break;
    case SystemCursor::wait: shape = IDC_WAIT; break;
    default: fail("system_cursor: unknown cursor " + std::to_string(static_cast<std::uint32_t>(cursor)));
    }
    const auto handle = LoadCursorW(nullptr, shape);
    if (handle == nullptr) fail_win32("LoadCursor");
    const auto id = next_cursor_++;
    cursors_[id].system = handle;
    system_cursors_[cursor] = id;
    return id;
}

CursorId Win32WindowServer::create_cursor(const Image& image, Point hot_spot) {
    require_main_thread("create_cursor");
    if (image.width == 0 || image.height == 0 || image.width > 1024 || image.height > 1024 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4)
        fail("create_cursor: invalid cursor image");
    // Created eagerly at scale 1 so a bad image fails here, not at set time.
    const auto handle = make_cursor(image, hot_spot, 1.0);
    const auto id = next_cursor_++;
    auto& cursor = cursors_[id];
    cursor.image = image;
    cursor.hot_spot = hot_spot;
    cursor.scaled[100] = handle;
    return id;
}

void Win32WindowServer::destroy_cursor(CursorId cursor) {
    require_main_thread("destroy_cursor");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto found = cursors_.find(cursor);
    if (found == cursors_.end()) fail("destroy_cursor: unknown cursor " + std::to_string(cursor));
    if (found->second.system) return;   // shared system cursors live for the process
    if (current_cursor_ == cursor) {
        current_cursor_ = 0;
        if (cursor_over_client()) apply_cursor();
    }
    for (auto& [scale, handle] : found->second.scaled) DestroyCursor(handle);
    cursors_.erase(found);
}

HCURSOR Win32WindowServer::cursor_handle(CursorId cursor) {
    if (cursor == 0) cursor = system_cursor(SystemCursor::arrow);
    auto found = cursors_.find(cursor);
    if (found == cursors_.end()) return LoadCursorW(nullptr, IDC_ARROW);
    auto& record = found->second;
    if (record.system) return record.system;
    // Cursor images are in points: scale to the DPI of the window under the
    // pointer (or the key window).
    double scale = 1;
    POINT position{};
    GetCursorPos(&position);
    if (const auto window = record_for(WindowFromPoint(position))) scale = window_scale(window->hwnd);
    else if (key_window_ != 0) {
        std::lock_guard lock(mutex_);
        if (const auto key = windows_.find(key_window_); key != windows_.end()) scale = window_scale(key->second->hwnd);
    }
    const auto percent = static_cast<int>(std::lround(scale * 100));
    if (const auto scaled = record.scaled.find(percent); scaled != record.scaled.end()) return scaled->second;
    const auto handle = make_cursor(record.image, record.hot_spot, percent / 100.0);
    record.scaled[percent] = handle;
    return handle;
}

bool Win32WindowServer::cursor_over_client() const {
    if (const auto capture = GetCapture(); capture && record_for(capture)) return true;
    POINT position{};
    if (!GetCursorPos(&position)) return false;
    const auto window = record_for(WindowFromPoint(position));
    if (!window) return false;
    POINT client = position;
    ScreenToClient(window->hwnd, &client);
    RECT area{};
    GetClientRect(window->hwnd, &area);
    return PtInRect(&area, client) != FALSE;
}

void Win32WindowServer::apply_cursor() {
    SetCursor(cursor_hide_count_ > 0 ? nullptr : cursor_handle(current_cursor_));
}

void Win32WindowServer::set_cursor(CursorId cursor) {
    require_main_thread("set_cursor");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    if (cursor != 0 && cursors_.find(cursor) == cursors_.end())
        fail("set_cursor: unknown cursor " + std::to_string(cursor));
    current_cursor_ = cursor;
    if (cursor_over_client()) apply_cursor();
}

void Win32WindowServer::set_cursor_visible(bool visible) {
    require_main_thread("set_cursor_visible");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    if (!visible) ++cursor_hide_count_;
    else if (cursor_hide_count_ > 0) --cursor_hide_count_;
    if (cursor_over_client()) apply_cursor();
}

void Win32WindowServer::warp_cursor(Point location) {
    require_main_thread("warp_cursor");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    const auto pixel = layout().to_pixels(location);
    const POINT target{static_cast<LONG>(std::lround(pixel.x)), static_cast<LONG>(std::lround(pixel.y))};
    if (relative_mode_) {
        relative_anchor_ = target;
        apply_clip();
    }
    if (!SetCursorPos(target.x, target.y)) fail_win32("SetCursorPos");
    warp_target_ = target;
    last_mouse_ = target;
    have_last_mouse_ = true;
}

void Win32WindowServer::associate_mouse_and_cursor(bool associated) {
    require_main_thread("associate_mouse_and_cursor");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    if (associated == !relative_mode_) return;
    ensure_initialized();
    RAWINPUTDEVICE device{0x01, 0x02, associated ? static_cast<DWORD>(RIDEV_REMOVE) : 0U, nullptr};   // generic mouse
    if (!associated) {
        if (!GetCursorPos(&relative_anchor_)) fail_win32("GetCursorPos");
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) fail_win32("RegisterRawInputDevices");
        relative_mode_ = true;
        raw_have_absolute_ = false;
    } else {
        RegisterRawInputDevices(&device, 1, sizeof(device));
        relative_mode_ = false;
        POINT position{};
        if (GetCursorPos(&position)) {
            last_mouse_ = position;
            have_last_mouse_ = true;
        }
    }
    apply_clip();
}

void Win32WindowServer::confine_cursor(WindowId window, std::optional<Rect> content) {
    require_main_thread("confine_cursor");
    const ThreadDpiScope dpi_scope;   // physical pixels whatever the caller's DPI awareness
    if (content) static_cast<void>(record(window, "confine_cursor"));
    confine_window_ = content ? window : 0;
    confine_rect_ = content;
    apply_clip();
}

void Win32WindowServer::apply_clip() {
    // ClipCursor is global: only clip while one of our windows has the Win32
    // keyboard focus (not a merely logical key window), and only release a
    // clip we set.
    RECT clip{};
    bool want = false;
    if (focus_window_ != 0 && relative_mode_) {
        // Relative mode pins the cursor: raw input still reports motion.
        clip = {relative_anchor_.x, relative_anchor_.y, relative_anchor_.x + 1, relative_anchor_.y + 1};
        want = true;
    } else if (focus_window_ != 0 && confine_window_ != 0 && confine_rect_) {
        std::shared_ptr<WindowRecord> target;
        {
            std::lock_guard lock(mutex_);
            if (const auto found = windows_.find(confine_window_); found != windows_.end()) target = found->second;
        }
        if (target && target->hwnd) {
            RECT client{};
            GetClientRect(target->hwnd, &client);
            const auto scale = window_scale(target->hwnd);
            const auto& rect = *confine_rect_;
            POINT top_left{static_cast<LONG>(std::lround(rect.origin.x * scale)),
                           static_cast<LONG>(std::lround(client.bottom - (rect.origin.y + rect.size.height) * scale))};
            POINT bottom_right{static_cast<LONG>(std::lround((rect.origin.x + rect.size.width) * scale)),
                               static_cast<LONG>(std::lround(client.bottom - rect.origin.y * scale))};
            ClientToScreen(target->hwnd, &top_left);
            ClientToScreen(target->hwnd, &bottom_right);
            clip = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
            want = true;
        }
    }
    if (want) {
        ClipCursor(&clip);
        clipping_ = true;
    } else if (clipping_) {
        ClipCursor(nullptr);
        clipping_ = false;
    }
}

// ---- clipboard ------------------------------------------------------------------

std::optional<std::string> Win32WindowServer::clipboard_text() const {
    require_main_thread("clipboard_text");
    ensure_initialized();
    ClipboardLease lease(application_window_);
    const auto handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == nullptr) return std::nullopt;
    GlobalLease data(handle);
    if (data.data() == nullptr) return std::nullopt;
    return clipboard_text_from_windows(
        std::wstring_view(static_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t)));
}

void Win32WindowServer::set_clipboard_text(std::string_view text) {
    require_main_thread("set_clipboard_text");
    ensure_initialized();
    const auto wide = clipboard_text_to_windows(text);
    ClipboardLease lease(application_window_);
    if (!EmptyClipboard()) fail_win32("EmptyClipboard");
    put_clipboard(CF_UNICODETEXT, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
}

std::optional<Image> Win32WindowServer::clipboard_image() const {
    require_main_thread("clipboard_image");
    ensure_initialized();
    ClipboardLease lease(application_window_);
    for (const UINT format : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
        if (!IsClipboardFormatAvailable(format)) continue;
        const auto handle = GetClipboardData(format);
        if (handle == nullptr) continue;
        GlobalLease data(handle);
        if (data.data() == nullptr) continue;
        return image_from_dib(static_cast<const std::uint8_t*>(data.data()), data.size());
    }
    return std::nullopt;
}

void Win32WindowServer::set_clipboard_image(const Image& image) {
    require_main_thread("set_clipboard_image");
    ensure_initialized();
    const auto dib = image_to_dibv5(image);
    ClipboardLease lease(application_window_);
    if (!EmptyClipboard()) fail_win32("EmptyClipboard");
    put_clipboard(CF_DIBV5, dib.data(), dib.size());
}

std::int64_t Win32WindowServer::clipboard_change_count() const {
    return static_cast<std::int64_t>(GetClipboardSequenceNumber());
}

// ---- keyboard layouts -------------------------------------------------------------

KeyboardLayout Win32WindowServer::keyboard_layout() const {
    require_main_thread("keyboard_layout");
    wchar_t klid[KL_NAMELENGTH]{};
    if (!GetKeyboardLayoutNameW(klid)) fail_win32("GetKeyboardLayoutName");
    std::wstring name(klid);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    KeyboardLayout layout;
    for (const auto& entry : layout_names)
        if (name == entry.klid) layout.identifier = entry.identifier;
    if (layout.identifier.empty()) layout.identifier = "com.alchemy.keylayout.win32." + wide_to_utf8(name);
    wchar_t text[256]{};
    DWORD size = sizeof(text);
    const auto key = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\" + name;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Layout Text", RRF_RT_REG_SZ, nullptr, text, &size) ==
        ERROR_SUCCESS)
        layout.localized_name = wide_to_utf8(text);
    else
        layout.localized_name = wide_to_utf8(name);
    const auto language = LOWORD(reinterpret_cast<std::uintptr_t>(GetKeyboardLayout(0)));
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (LCIDToLocaleName(MAKELCID(language, SORT_DEFAULT), locale, LOCALE_NAME_MAX_LENGTH, 0) > 0)
        layout.language = wide_to_utf8(locale);
    return layout;
}

std::string Win32WindowServer::translate_key(std::uint16_t key_code, std::uint64_t flags,
                                             DeadKeyState& state) const {
    if (const auto special = translate_special_character(key_code)) {
        state.value = 0;
        std::string text;
        append_utf8(text, *special);
        return text;
    }
    const auto layout = GetKeyboardLayout(main_thread_);
    const auto scan = scan_code_for_mac_key(key_code);
    const auto virtual_key = scan ? MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, layout) : 0;
    if (virtual_key == 0) {
        state.value = 0;
        return {};
    }
    BYTE keys[256];
    key_state_for(keys, flags);
    wchar_t buffer[16]{};
    const auto translate = [&](UINT key, UINT scan_code, const BYTE* key_state, UINT translate_flags) {
        return ToUnicodeEx(key, scan_code & 0xff, key_state, buffer, 16, translate_flags, layout);
    };
    // Clears a dead key left in the thread's keyboard state.
    const auto flush = [&] {
        BYTE plain[256]{};
        for (int attempt = 0; attempt < 4 && translate(VK_SPACE, MapVirtualKeyExW(VK_SPACE, MAPVK_VK_TO_VSC, layout),
                                                          plain, 0) < 0;
             ++attempt) {
        }
    };
    if (state.value != 0 && !state.no_dead_keys) {
        // Replay the pending dead key, then this key, with state-changing
        // translations so ToUnicodeEx composes them.
        const UINT dead_key = state.value & 0xffffU;
        BYTE dead_keys[256];
        key_state_for(dead_keys, unpack_dead_flags(state.value));
        state.value = 0;
        flush();
        translate(dead_key, MapVirtualKeyExW(dead_key, MAPVK_VK_TO_VSC, layout), dead_keys, 0);
        const auto count = translate(virtual_key, scan, keys, 0);
        if (count < 0) {
            flush();
            state.value = pack_dead_key(virtual_key, flags);
            return {};
        }
        return wide_to_utf8(std::wstring_view(buffer, static_cast<std::size_t>(count)));
    }
    // Probe without changing the keyboard state (bit 2, Windows 10 1607+).
    const auto count = translate(virtual_key, scan, keys, 0x4);
    if (count < 0) {
        if (state.no_dead_keys) return wide_to_utf8(std::wstring_view(buffer, 1));   // the spacing accent
        state.value = pack_dead_key(virtual_key, flags);
        return {};
    }
    state.value = 0;
    return wide_to_utf8(std::wstring_view(buffer, static_cast<std::size_t>(count)));
}

// ---- file drops ------------------------------------------------------------------

void Win32WindowServer::register_drop_target(WindowRecord& record, bool accepts) {
    if (accepts) {
        if (record.drop_target || record.drop_files) return;
        if (!ole_initialized_ && !ole_unavailable_) {
            // OLE drag and drop needs a single-threaded apartment on the
            // main thread; when COM was already initialized multithreaded
            // there, fall back to WM_DROPFILES.
            if (SUCCEEDED(OleInitialize(nullptr))) ole_initialized_ = true;
            else ole_unavailable_ = true;
        }
        if (ole_initialized_) {
            auto* target = new DropTarget(*this, record.id);
            if (SUCCEEDED(RegisterDragDrop(record.hwnd, target))) {
                record.drop_target = target;
                return;
            }
            target->Release();
        }
        DragAcceptFiles(record.hwnd, TRUE);
        record.drop_files = true;
        return;
    }
    if (record.drop_target) {
        RevokeDragDrop(record.hwnd);
        record.drop_target->Release();
        record.drop_target = nullptr;
    }
    if (record.drop_files) {
        DragAcceptFiles(record.hwnd, FALSE);
        record.drop_files = false;
    }
}

bool Win32WindowServer::accepting_drops(WindowId window) const {
    std::lock_guard lock(mutex_);
    const auto found = windows_.find(window);
    return found != windows_.end() && !found->second->destroyed &&
           (found->second->drop_target != nullptr || found->second->drop_files);
}

IDropTarget* Win32WindowServer::drop_target(WindowId window) const {
    const auto target = live_record(window);
    return target ? target->drop_target : nullptr;
}

void Win32WindowServer::deliver_drop(WindowId window, std::vector<std::string> paths, POINT screen_pixel,
                                     std::uint64_t operations) {
    const ThreadDpiScope dpi_scope;
    const auto target = live_record(window);
    if (!target || paths.empty()) return;
    POINT client = screen_pixel;
    ScreenToClient(target->hwnd, &client);
    Event event;
    event.kind = EventKind::notification;
    event.notification = Notification::files_dropped;
    event.location = content_point(target->hwnd, client.x, client.y);
    event.screen_location = screen_point(screen_pixel);
    event.modifier_flags = current_modifier_flags();
    event.paths = std::move(paths);
    event.drag_operations = operations;
    deliver(*target, event);
}

std::uint64_t Win32WindowServer::deliver_drag(WindowId window, Notification notification,
                                              const std::vector<std::string>& paths, POINT screen_pixel,
                                              std::uint64_t operations) {
    const ThreadDpiScope dpi_scope;
    const auto target = live_record(window);
    if (!target || target->creating || pending_ || shutting_down_ || !target->sink) return drag_operation::none;
    POINT client = screen_pixel;
    ScreenToClient(target->hwnd, &client);
    Event event;
    event.kind = EventKind::notification;
    event.notification = notification;
    event.location = content_point(target->hwnd, client.x, client.y);
    event.screen_location = screen_point(screen_pixel);
    event.modifier_flags = current_modifier_flags();
    event.paths = paths;
    event.drag_operations = operations;
    deliver(*target, event);
    if (notification == Notification::drag_exited || pending_ || target->destroyed || !target->sink)
        return drag_operation::none;
    const auto sink = target->sink;
    CallbackScope scope(*this, *target);
    return sink->drag_operation(window);
}

// ---- misc -------------------------------------------------------------------------

void Win32WindowServer::beep() { MessageBeep(MB_OK); }

void Win32WindowServer::request_attention(bool critical) {
    require_main_thread("request_attention");
    HWND hwnd = nullptr;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, record] : windows_) {
            if (record->destroyed || !IsWindowVisible(record->hwnd)) continue;
            if (record->parent == 0) {
                hwnd = record->hwnd;
                break;
            }
        }
    }
    if (hwnd == nullptr || GetForegroundWindow() == hwnd) return;   // nothing to flash / already active
    FLASHWINFO flash{sizeof(flash), hwnd, critical ? static_cast<DWORD>(FLASHW_ALL | FLASHW_TIMERNOFG)
                                                   : static_cast<DWORD>(FLASHW_TRAY),
                     critical ? 0U : 2U, 0};
    FlashWindowEx(&flash);
}

} // namespace alchemy::gui::win32
