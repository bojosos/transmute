// Included after Win32Bridge::State. Keep GUI forwarding out of the CRT bridge.
bool Win32Bridge::dispatch_gui(const Import& imported, CpuState& cpu, GuestMemory& memory) {
    const auto& dll = imported.dll;
    const auto& name = imported.function;
    const auto fail = [&cpu](DWORD error, std::uint64_t result = 0) {
        cpu.x[0] = result;
        SetLastError(error);
        return true;
    };
    const auto window_for = [this](std::uint64_t guest) -> HWND {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->windows.find(guest);
        return found == state_->windows.end() ? nullptr : found->second->host;
    };
    const auto token_for = [this](HWND host) -> std::uint64_t {
        if (host == nullptr) return 0;
        std::lock_guard lock(state_->mutex);
        const auto found = state_->host_windows.find(host);
        return found == state_->host_windows.end() ? 0 : found->second;
    };
    const auto dc_for = [this](std::uint64_t guest) -> HDC {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->gdi_dcs.find(guest);
        return found == state_->gdi_dcs.end() ? nullptr : found->second.host;
    };
    const auto context_for = [this](std::uint64_t guest) -> HGLRC {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->gl_contexts.find(guest);
        return found == state_->gl_contexts.end() ? nullptr : found->second;
    };
    const auto callback_failure = [this]() {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->window_failures.find(GetCurrentThreadId());
        if (found == state_->window_failures.end()) return;
        const auto message = std::move(found->second);
        state_->window_failures.erase(found);
        throw std::runtime_error("guest window procedure failed: " + message);
    };
    if (dll == "shell32.dll" && name == "SHGetPropertyStoreForWindow") {
        if (!guest_range(memory, cpu.x[1], sizeof(GUID), false) || !guest_range(memory, cpu.x[2], 8, true)) {
            cpu.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return true;
        }
        memory.write64(cpu.x[2], 0);
        cpu.x[0] = static_cast<std::uint32_t>(E_NOTIMPL);
        return true;
    }
    if (dll == "ole32.dll" && (name == "RegisterDragDrop" || name == "RevokeDragDrop")) {
        cpu.x[0] = static_cast<std::uint32_t>(name == "RegisterDragDrop" ? E_NOTIMPL : DRAGDROP_E_NOTREGISTERED);
        return true;
    }
    if (dll == "imm32.dll" && name == "ImmAssociateContextEx") {
        const auto window = window_for(cpu.x[0]);
        if (window == nullptr) return fail(ERROR_INVALID_HANDLE);
        if (cpu.x[1] != 0) return fail(ERROR_NOT_SUPPORTED);
        cpu.x[0] = static_cast<std::uint32_t>(::ImmAssociateContextEx(window, nullptr, static_cast<DWORD>(cpu.x[2])));
        return true;
    }
    if (dll == "dwmapi.dll" && (name == "DwmGetWindowAttribute" || name == "DwmSetWindowAttribute")) {
        const auto window = window_for(cpu.x[0]);
        const bool output = name == "DwmGetWindowAttribute";
        if (window == nullptr || cpu.x[3] > 256 || !guest_range(memory, cpu.x[2], cpu.x[3], output)) {
            cpu.x[0] = static_cast<std::uint32_t>(E_INVALIDARG);
            return true;
        }
        std::array<std::uint8_t, 256> data{};
        if (!output) memory.read_bytes(cpu.x[2], std::span<std::uint8_t>(data.data(), static_cast<std::size_t>(cpu.x[3])));
        const auto result = output ? ::DwmGetWindowAttribute(window, static_cast<DWORD>(cpu.x[1]), data.data(), static_cast<DWORD>(cpu.x[3]))
            : ::DwmSetWindowAttribute(window, static_cast<DWORD>(cpu.x[1]), data.data(), static_cast<DWORD>(cpu.x[3]));
        if (output && SUCCEEDED(result)) memory.write_bytes(cpu.x[2], std::span<const std::uint8_t>(data.data(), static_cast<std::size_t>(cpu.x[3])));
        cpu.x[0] = static_cast<std::uint32_t>(result);
        return true;
    }
    if (dll == "user32.dll" && (name == "GetActiveWindow" || name == "GetFocus" || name == "GetForegroundWindow" || name == "ReleaseCapture" || name == "GetMessageTime" || name == "GetMessagePos")) {
        if (name == "GetActiveWindow") cpu.x[0] = token_for(::GetActiveWindow());
        else if (name == "GetFocus") cpu.x[0] = token_for(::GetFocus());
        else if (name == "GetForegroundWindow") cpu.x[0] = token_for(::GetForegroundWindow());
        else if (name == "ReleaseCapture") cpu.x[0] = static_cast<std::uint32_t>(::ReleaseCapture());
        else if (name == "GetMessageTime") cpu.x[0] = static_cast<std::uint32_t>(::GetMessageTime());
        else cpu.x[0] = ::GetMessagePos();
        callback_failure();
        return true;
    }
    if (dll == "user32.dll" && (name == "GetSystemMenu" || name == "DeleteMenu")) {
        std::lock_guard lock(state_->mutex);
        if (name == "DeleteMenu") {
            const auto found = state_->gui_menus.find(cpu.x[0]);
            if (found == state_->gui_menus.end()) return fail(ERROR_INVALID_HANDLE);
            cpu.x[0] = static_cast<std::uint32_t>(::DeleteMenu(found->second, static_cast<UINT>(cpu.x[1]), static_cast<UINT>(cpu.x[2])));
            return true;
        }
        const auto window = window_for(cpu.x[0]);
        if (window == nullptr) return fail(ERROR_INVALID_HANDLE);
        const auto menu = ::GetSystemMenu(window, static_cast<BOOL>(cpu.x[1]));
        cpu.x[0] = 0;
        if (menu == nullptr) return true;
        for (const auto& [token, known] : state_->gui_menus) if (menu == known) { cpu.x[0] = token; return true; }
        const auto token = state_->next_gui_menu;
        state_->next_gui_menu += 16;
        state_->gui_menus.emplace(token, menu);
        cpu.x[0] = token;
        return true;
    }
    if (dll == "user32.dll" && (name == "GetWindowPlacement" || name == "SetWindowPlacement" ||
        name == "TrackMouseEvent" || name == "GetWindowThreadProcessId" || name == "RegisterTouchWindow" ||
        name == "GetClassLongPtrA" || name == "GetClassLongPtrW" || name == "SetClassLongPtrA" || name == "SetClassLongPtrW")) {
        if (name == "TrackMouseEvent") {
            if (!guest_range(memory, cpu.x[0], sizeof(TRACKMOUSEEVENT), false)) return fail(ERROR_NOACCESS);
            TRACKMOUSEEVENT event{};
            memory.read_bytes(cpu.x[0], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(&event), sizeof(event)));
            if (event.cbSize != sizeof(event)) return fail(ERROR_INVALID_PARAMETER);
            event.hwndTrack = window_for(reinterpret_cast<std::uintptr_t>(event.hwndTrack));
            if (event.hwndTrack == nullptr) return fail(ERROR_INVALID_HANDLE);
            cpu.x[0] = static_cast<std::uint32_t>(::TrackMouseEvent(&event));
            return true;
        }
        const auto window = window_for(cpu.x[0]);
        if (window == nullptr) return fail(ERROR_INVALID_HANDLE);
        if (name == "RegisterTouchWindow") cpu.x[0] = static_cast<std::uint32_t>(::RegisterTouchWindow(window, static_cast<ULONG>(cpu.x[1])));
        else if (name == "GetWindowThreadProcessId") {
            if (cpu.x[1] != 0 && !guest_range(memory, cpu.x[1], 4, true)) return fail(ERROR_NOACCESS);
            DWORD process = 0;
            const auto thread = ::GetWindowThreadProcessId(window, &process);
            if (cpu.x[1] != 0) memory.write32(cpu.x[1], process);
            cpu.x[0] = thread;
        } else if (name.find("ClassLongPtr") != std::string::npos) {
            const auto index = static_cast<int>(cpu.x[1]);
            if (index != GCLP_HBRBACKGROUND) return fail(ERROR_NOT_SUPPORTED);
            std::lock_guard lock(state_->mutex);
            HBRUSH brush = nullptr;
            if (name.starts_with("Set")) {
                if (cpu.x[2] <= COLOR_MENUBAR + 1) brush = reinterpret_cast<HBRUSH>(cpu.x[2]);
                else {
                    const auto found = state_->gdi_objects.find(cpu.x[2]);
                    if (found == state_->gdi_objects.end() || found->second.type != OBJ_BRUSH) return fail(ERROR_INVALID_HANDLE);
                    brush = static_cast<HBRUSH>(found->second.host);
                }
            }
            const auto previous = name.starts_with("Set") ? ::SetClassLongPtrW(window, index, reinterpret_cast<LONG_PTR>(brush)) : ::GetClassLongPtrW(window, index);
            cpu.x[0] = 0;
            if (static_cast<std::uintptr_t>(previous) <= COLOR_MENUBAR + 1) cpu.x[0] = static_cast<std::uintptr_t>(previous);
            else for (const auto& [token, object] : state_->gdi_objects)
                if (reinterpret_cast<std::uintptr_t>(object.host) == static_cast<std::uintptr_t>(previous)) cpu.x[0] = token;
        } else {
            const bool output = name == "GetWindowPlacement";
            if (!guest_range(memory, cpu.x[1], sizeof(WINDOWPLACEMENT), !output ? false : true)) return fail(ERROR_NOACCESS);
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            if (!output) memory.read_bytes(cpu.x[1], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(&placement), sizeof(placement)));
            const auto result = output ? ::GetWindowPlacement(window, &placement) : ::SetWindowPlacement(window, &placement);
            if (output && result) memory.write_bytes(cpu.x[1], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&placement), sizeof(placement)));
            cpu.x[0] = static_cast<std::uint32_t>(result);
        }
        callback_failure();
        return true;
    }
    if (dll == "user32.dll" && (name == "GetDC" || name == "ReleaseDC")) {
        const auto window = window_for(cpu.x[0]);
        if (cpu.x[0] != 0 && window == nullptr) return fail(ERROR_INVALID_HANDLE);
        std::lock_guard lock(state_->mutex);
        if (name == "GetDC") {
            const auto dc = ::GetDC(window);
            if (dc == nullptr) { cpu.x[0] = 0; return true; }
            for (auto& [token, known] : state_->gdi_dcs) {
                if (known.window_dc && known.host == dc && known.window == window) {
                    ++known.acquisitions;
                    cpu.x[0] = token;
                    return true;
                }
            }
            const auto token = state_->next_gdi_handle;
            state_->next_gdi_handle += 16;
            state_->gdi_dcs.emplace(token, State::GdiDc{dc, window, true});
            cpu.x[0] = token;
        } else {
            const auto found = state_->gdi_dcs.find(cpu.x[1]);
            if (found == state_->gdi_dcs.end() || !found->second.window_dc ||
                found->second.window != window) return fail(ERROR_INVALID_HANDLE);
            const auto success = ::ReleaseDC(window, found->second.host);
            const auto error = GetLastError();
            if (success && --found->second.acquisitions == 0) state_->gdi_dcs.erase(found);
            cpu.x[0] = static_cast<std::uint32_t>(success);
            SetLastError(error);
        }
        return true;
    }
    if (dll == "user32.dll" &&
        (name == "PeekMessageA" || name == "PeekMessageW" ||
         name == "GetMessageA" || name == "GetMessageW" ||
         name == "TranslateMessage" || name == "DispatchMessageA" ||
         name == "DispatchMessageW")) {
        static_assert(sizeof(MSG) == 48 && offsetof(MSG, lParam) == 24);
        const bool receive = name.starts_with("Peek") || name.starts_with("Get");
        if (!guest_range(memory, cpu.x[0], sizeof(MSG), receive))
            return fail(ERROR_NOACCESS, name.starts_with("Get") ? UINT32_MAX : 0);
        MSG message{};
        if (receive) {
            const auto filter = window_for(cpu.x[1]);
            if (cpu.x[1] != 0 && cpu.x[1] != UINT64_MAX && filter == nullptr)
                return fail(ERROR_INVALID_HANDLE);
            const auto native_filter = cpu.x[1] == UINT64_MAX ? reinterpret_cast<HWND>(-1) : filter;
            BOOL result = 0;
            std::uint64_t guest_window = 0;
            for (;;) {
                guest_window = 0;
                result = name.starts_with("Peek")
                    ? ::PeekMessageW(&message, native_filter, static_cast<UINT>(cpu.x[2]),
                                      static_cast<UINT>(cpu.x[3]), static_cast<UINT>(cpu.x[4]))
                    : ::GetMessageW(&message, native_filter, static_cast<UINT>(cpu.x[2]),
                                     static_cast<UINT>(cpu.x[3]));
                if (result <= 0) break;
                guest_window = token_for(message.hwnd);
                const bool native_only = (message.hwnd != nullptr && guest_window == 0) ||
                    (message.message == WM_TIMER && message.lParam != 0);
                if (!native_only) break;
                // Native drivers and system services own helper windows and
                // timer callbacks. Keep their pointers out of guest MSG records.
                if (name.starts_with("Peek") && !(cpu.x[4] & PM_REMOVE)) {
                    const auto helper_filter = message.hwnd != nullptr ? message.hwnd : reinterpret_cast<HWND>(-1);
                    if (!::PeekMessageW(&message, helper_filter, message.message, message.message, PM_REMOVE)) continue;
                }
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
                callback_failure();
            }
            const auto error = GetLastError();
            callback_failure();
            if (result > 0 || (result == 0 && name.starts_with("Get"))) {
                // Host pointers in system messages stay inside the bridge.
                memory.write64(cpu.x[0], guest_window);
                memory.write32(cpu.x[0] + 8, message.message);
                memory.write32(cpu.x[0] + 12, 0);
                memory.write64(cpu.x[0] + 16, message.wParam);
                memory.write64(cpu.x[0] + 24, static_cast<std::uint64_t>(message.lParam));
                memory.write32(cpu.x[0] + 32, message.time);
                memory.write32(cpu.x[0] + 36, static_cast<std::uint32_t>(message.pt.x));
                memory.write32(cpu.x[0] + 40, static_cast<std::uint32_t>(message.pt.y));
                memory.write32(cpu.x[0] + 44, 0);
            }
            cpu.x[0] = static_cast<std::uint32_t>(result);
            SetLastError(error);
        } else {
            message.hwnd = window_for(memory.read64(cpu.x[0]));
            if (memory.read64(cpu.x[0]) != 0 && message.hwnd == nullptr)
                return fail(ERROR_INVALID_HANDLE);
            message.message = memory.read32(cpu.x[0] + 8);
            message.wParam = memory.read64(cpu.x[0] + 16);
            message.lParam = static_cast<LPARAM>(memory.read64(cpu.x[0] + 24));
            message.time = memory.read32(cpu.x[0] + 32);
            message.pt.x = static_cast<LONG>(memory.read32(cpu.x[0] + 36));
            message.pt.y = static_cast<LONG>(memory.read32(cpu.x[0] + 40));
            cpu.x[0] = name == "TranslateMessage"
                ? static_cast<std::uint32_t>(::TranslateMessage(&message))
                : static_cast<std::uint64_t>(::DispatchMessageW(&message));
            callback_failure();
        }
        return true;
    }
    if (dll == "user32.dll" && name == "PostQuitMessage") {
        ::PostQuitMessage(static_cast<int>(cpu.x[0]));
        return true;
    }
    if (dll == "user32.dll" && (name == "PostMessageA" || name == "PostMessageW")) {
        const auto window = window_for(cpu.x[0]);
        if (window == nullptr) return fail(ERROR_INVALID_HANDLE);
        const auto message = static_cast<UINT>(cpu.x[1]);
        switch (message) {
        case WM_NULL: case WM_CLOSE:
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
        case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSELEAVE:
            break;
        default:
            if (message < WM_USER) return fail(ERROR_NOT_SUPPORTED);
        }
        cpu.x[0] = static_cast<std::uint32_t>(::PostMessageW(window, message,
            static_cast<WPARAM>(cpu.x[2]), static_cast<LPARAM>(cpu.x[3])));
        return true;
    }
    if (dll == "user32.dll" && name == "GetSystemMetrics") {
        cpu.x[0] = static_cast<std::uint32_t>(::GetSystemMetrics(static_cast<int>(cpu.x[0])));
        return true;
    }
    if (dll == "user32.dll" && (name == "EnumDisplaySettingsW" || name == "EnumDisplaySettingsA" ||
        name == "EnumDisplayDevicesW" || name == "EnumDisplayDevicesA" ||
        name == "MonitorFromWindow" || name == "MonitorFromRect" || name == "MonitorFromPoint" ||
        name == "GetMonitorInfoW" || name == "GetMonitorInfoA" ||
        name == "AdjustWindowRectEx" || name == "GetCursorPos")) {
        if (name == "GetCursorPos") {
            if (!guest_range(memory, cpu.x[0], sizeof(POINT), true)) return fail(ERROR_NOACCESS);
            POINT point{};
            const auto result = ::GetCursorPos(&point);
            if (result) memory.write_bytes(cpu.x[0], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&point), sizeof(point)));
            cpu.x[0] = static_cast<std::uint32_t>(result);
        } else if (name == "MonitorFromWindow") {
            const auto window = window_for(cpu.x[0]);
            if (window == nullptr) return fail(ERROR_INVALID_HANDLE);
            cpu.x[0] = reinterpret_cast<std::uintptr_t>(::MonitorFromWindow(window, static_cast<DWORD>(cpu.x[1])));
        } else if (name == "MonitorFromRect" || name == "AdjustWindowRectEx") {
            const bool adjust = name == "AdjustWindowRectEx";
            if (!guest_range(memory, cpu.x[0], sizeof(RECT), false) ||
                (adjust && !guest_range(memory, cpu.x[0], sizeof(RECT), true))) return fail(ERROR_NOACCESS);
            RECT rect{};
            memory.read_bytes(cpu.x[0], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(&rect), sizeof(rect)));
            if (!adjust) cpu.x[0] = reinterpret_cast<std::uintptr_t>(::MonitorFromRect(&rect, static_cast<DWORD>(cpu.x[1])));
            else {
                const auto result = ::AdjustWindowRectEx(&rect, static_cast<DWORD>(cpu.x[1]), static_cast<BOOL>(cpu.x[2]), static_cast<DWORD>(cpu.x[3]));
                if (result) memory.write_bytes(cpu.x[0], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&rect), sizeof(rect)));
                cpu.x[0] = static_cast<std::uint32_t>(result);
            }
        } else if (name == "MonitorFromPoint") {
            const POINT point{static_cast<LONG>(cpu.x[0]), static_cast<LONG>(cpu.x[0] >> 32)};
            cpu.x[0] = reinterpret_cast<std::uintptr_t>(::MonitorFromPoint(point, static_cast<DWORD>(cpu.x[1])));
        } else if (name.starts_with("GetMonitorInfo")) {
            const bool wide = name.ends_with('W');
            if (!guest_range(memory, cpu.x[1], 4, false)) return fail(ERROR_NOACCESS);
            const auto size = memory.read32(cpu.x[1]);
            if (size != sizeof(MONITORINFO) && size != (wide ? sizeof(MONITORINFOEXW) : sizeof(MONITORINFOEXA))) return fail(ERROR_INVALID_PARAMETER);
            if (!guest_range(memory, cpu.x[1], size, true)) return fail(ERROR_NOACCESS);
            MONITORINFOEXW info_w{};
            MONITORINFOEXA info_a{};
            info_w.cbSize = size;
            info_a.cbSize = size;
            const auto result = wide ? ::GetMonitorInfoW(reinterpret_cast<HMONITOR>(cpu.x[0]), &info_w)
                : ::GetMonitorInfoA(reinterpret_cast<HMONITOR>(cpu.x[0]), &info_a);
            if (result) memory.write_bytes(cpu.x[1], std::span<const std::uint8_t>(wide ? reinterpret_cast<const std::uint8_t*>(&info_w) : reinterpret_cast<const std::uint8_t*>(&info_a), size));
            cpu.x[0] = static_cast<std::uint32_t>(result);
        } else {
            const bool wide = name.ends_with('W');
            const auto device = cpu.x[0] == 0 || !wide ? std::wstring{} : guest_string(memory, cpu.x[0], true);
            const auto device_a = cpu.x[0] == 0 || wide ? std::string{} : guest_ansi_string(memory, cpu.x[0]);
            const auto output = cpu.x[2];
            const bool settings = name.starts_with("EnumDisplaySettings");
            const auto size = settings ? (wide ? sizeof(DEVMODEW) : sizeof(DEVMODEA))
                : (wide ? sizeof(DISPLAY_DEVICEW) : sizeof(DISPLAY_DEVICEA));
            if (!guest_range(memory, output, size, true)) return fail(ERROR_NOACCESS);
            DEVMODEW mode_w{};
            DEVMODEA mode_a{};
            DISPLAY_DEVICEW display_w{};
            DISPLAY_DEVICEA display_a{};
            mode_w.dmSize = sizeof(mode_w);
            mode_a.dmSize = sizeof(mode_a);
            display_w.cb = sizeof(display_w);
            display_a.cb = sizeof(display_a);
            const auto result = settings
                ? wide ? ::EnumDisplaySettingsW(cpu.x[0] == 0 ? nullptr : device.c_str(), static_cast<DWORD>(cpu.x[1]), &mode_w)
                       : ::EnumDisplaySettingsA(cpu.x[0] == 0 ? nullptr : device_a.c_str(), static_cast<DWORD>(cpu.x[1]), &mode_a)
                : wide ? ::EnumDisplayDevicesW(cpu.x[0] == 0 ? nullptr : device.c_str(), static_cast<DWORD>(cpu.x[1]), &display_w, static_cast<DWORD>(cpu.x[3]))
                       : ::EnumDisplayDevicesA(cpu.x[0] == 0 ? nullptr : device_a.c_str(), static_cast<DWORD>(cpu.x[1]), &display_a, static_cast<DWORD>(cpu.x[3]));
            const auto error = GetLastError();
            const auto data = settings ? wide ? reinterpret_cast<const std::uint8_t*>(&mode_w) : reinterpret_cast<const std::uint8_t*>(&mode_a)
                : wide ? reinterpret_cast<const std::uint8_t*>(&display_w) : reinterpret_cast<const std::uint8_t*>(&display_a);
            if (result) memory.write_bytes(output, std::span<const std::uint8_t>(data, size));
            cpu.x[0] = static_cast<std::uint32_t>(result);
            SetLastError(error);
        }
        return true;
    }
    if (dll == "user32.dll" && (name == "ShowWindow" || name == "UpdateWindow" ||
        name == "IsWindow" || name == "IsWindowVisible" || name == "IsIconic" || name == "IsZoomed" || name == "SetForegroundWindow" ||
        name == "SetFocus" || name == "SetCapture" || name == "InvalidateRect" ||
        name == "ValidateRect" || name == "GetClientRect" || name == "GetWindowRect" ||
        name == "ClientToScreen" || name == "ScreenToClient" || name == "GetMenu" ||
        name == "GetWindowLongA" || name == "GetWindowLongW" ||
        name == "GetWindowLongPtrA" || name == "GetWindowLongPtrW" ||
        name == "SetWindowLongPtrA" || name == "SetWindowLongPtrW" ||
        name == "GetClassNameW" || name == "GetWindowTextW" || name == "SetWindowTextW" ||
        name == "GetWindowTextLengthW" || name == "GetParent" || name == "GetWindow" || name == "SetWindowPos")) {
        const auto window = window_for(cpu.x[0]);
        if (window == nullptr) return fail(ERROR_INVALID_HANDLE);
        if (name == "ShowWindow") cpu.x[0] = static_cast<std::uint32_t>(::ShowWindow(window, static_cast<int>(cpu.x[1])));
        else if (name == "UpdateWindow") cpu.x[0] = static_cast<std::uint32_t>(::UpdateWindow(window));
        else if (name == "IsWindow") cpu.x[0] = static_cast<std::uint32_t>(::IsWindow(window));
        else if (name == "IsWindowVisible") cpu.x[0] = static_cast<std::uint32_t>(::IsWindowVisible(window));
        else if (name == "IsIconic") cpu.x[0] = static_cast<std::uint32_t>(::IsIconic(window));
        else if (name == "IsZoomed") cpu.x[0] = static_cast<std::uint32_t>(::IsZoomed(window));
        else if (name == "GetWindowTextLengthW") cpu.x[0] = static_cast<std::uint32_t>(::GetWindowTextLengthW(window));
        else if (name == "GetParent") cpu.x[0] = token_for(::GetParent(window));
        else if (name == "GetWindow") cpu.x[0] = token_for(::GetWindow(window, static_cast<UINT>(cpu.x[1])));
        else if (name == "SetForegroundWindow") cpu.x[0] = static_cast<std::uint32_t>(::SetForegroundWindow(window));
        else if (name == "SetFocus") cpu.x[0] = token_for(::SetFocus(window));
        else if (name == "SetCapture") cpu.x[0] = token_for(::SetCapture(window));
        else if (name == "GetMenu") cpu.x[0] = 0; // Guest window creation currently accepts no menus.
        else if (name == "GetWindowLongA" || name == "GetWindowLongW" || name.starts_with("GetWindowLongPtr")) {
            const auto index = static_cast<int>(cpu.x[1]);
            if (index == GWLP_WNDPROC || index == GWLP_HINSTANCE) {
                std::lock_guard lock(state_->mutex);
                const auto found = state_->windows.find(cpu.x[0]);
                if (found == state_->windows.end()) return fail(ERROR_INVALID_HANDLE);
                cpu.x[0] = index == GWLP_WNDPROC ? found->second->guest_proc : found->second->guest_instance;
            } else if (index == GWLP_HWNDPARENT) cpu.x[0] = token_for(::GetParent(window));
            else if (index == GWL_STYLE || index == GWL_EXSTYLE || index == GWLP_USERDATA || index >= 0) {
                if (index >= 0) {
                    std::lock_guard lock(state_->mutex);
                    const auto context = state_->windows.at(cpu.x[0]);
                    if (static_cast<unsigned>(index) > context->extra_bytes ||
                        sizeof(LONG_PTR) > context->extra_bytes - static_cast<unsigned>(index)) return fail(ERROR_INVALID_INDEX);
                }
                cpu.x[0] = static_cast<std::uint64_t>(::GetWindowLongPtrW(window, index));
            }
            else return fail(ERROR_NOT_SUPPORTED);
        } else if (name.starts_with("SetWindowLongPtr")) {
            const auto index = static_cast<int>(cpu.x[1]);
            if (index != GWLP_USERDATA && index != GWL_STYLE && index != GWL_EXSTYLE && index < 0)
                return fail(ERROR_NOT_SUPPORTED);
            if (index >= 0) {
                std::lock_guard lock(state_->mutex);
                const auto context = state_->windows.at(cpu.x[0]);
                if (static_cast<unsigned>(index) > context->extra_bytes ||
                    sizeof(LONG_PTR) > context->extra_bytes - static_cast<unsigned>(index)) return fail(ERROR_INVALID_INDEX);
            }
            cpu.x[0] = static_cast<std::uint64_t>(::SetWindowLongPtrW(window, index, static_cast<LONG_PTR>(cpu.x[2])));
        } else if (name == "GetClassNameW" || name == "GetWindowTextW") {
            const auto capacity = static_cast<int>(cpu.x[2]);
            if (capacity <= 0 || capacity > 65536 || !guest_range(memory, cpu.x[1], 2ULL * capacity, true))
                return fail(ERROR_NOACCESS);
            std::wstring text;
            if (name == "GetClassNameW") {
                std::lock_guard lock(state_->mutex);
                const auto context = state_->windows.find(cpu.x[0]);
                if (context == state_->windows.end()) return fail(ERROR_INVALID_HANDLE);
                text = context->second->registered_class_name;
            } else {
                std::vector<wchar_t> buffer(static_cast<std::size_t>(capacity));
                const auto count = ::GetWindowTextW(window, buffer.data(), capacity);
                text.assign(buffer.data(), static_cast<std::size_t>(count));
            }
            text.resize(std::min(text.size(), static_cast<std::size_t>(capacity - 1)));
            memory.write_bytes(cpu.x[1], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.c_str()), (text.size() + 1) * 2));
            cpu.x[0] = text.size();
        } else if (name == "SetWindowTextW") {
            const auto title = guest_string(memory, cpu.x[1], true);
            cpu.x[0] = static_cast<std::uint32_t>(::SetWindowTextW(window, title.c_str()));
        } else if (name == "SetWindowPos") {
            HWND after = nullptr;
            if (cpu.x[1] == 0 || cpu.x[1] == 1 || cpu.x[1] >= UINT64_MAX - 1)
                after = reinterpret_cast<HWND>(cpu.x[1]);
            else if ((after = window_for(cpu.x[1])) == nullptr) return fail(ERROR_INVALID_HANDLE);
            cpu.x[0] = static_cast<std::uint32_t>(::SetWindowPos(window, after, static_cast<int>(cpu.x[2]),
                static_cast<int>(cpu.x[3]), static_cast<int>(cpu.x[4]), static_cast<int>(cpu.x[5]), static_cast<UINT>(cpu.x[6])));
        } else {
            const bool point = name == "ClientToScreen" || name == "ScreenToClient";
            const bool output = point || name == "GetClientRect" || name == "GetWindowRect";
            const auto bytes = point ? sizeof(POINT) : sizeof(RECT);
            if ((output || cpu.x[1] != 0) && !guest_range(memory, cpu.x[1], bytes, output))
                return fail(ERROR_NOACCESS);
            RECT rectangle{};
            POINT coordinate{};
            if (point) memory.read_bytes(cpu.x[1], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(&coordinate), sizeof(coordinate)));
            else if (!output && cpu.x[1] != 0) memory.read_bytes(cpu.x[1], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(&rectangle), sizeof(rectangle)));
            const auto result = name == "GetClientRect" ? ::GetClientRect(window, &rectangle)
                : name == "GetWindowRect" ? ::GetWindowRect(window, &rectangle)
                : name == "ClientToScreen" ? ::ClientToScreen(window, &coordinate)
                : name == "ScreenToClient" ? ::ScreenToClient(window, &coordinate)
                : name == "InvalidateRect" ? ::InvalidateRect(window, cpu.x[1] == 0 ? nullptr : &rectangle, static_cast<BOOL>(cpu.x[2]))
                : ::ValidateRect(window, cpu.x[1] == 0 ? nullptr : &rectangle);
            if (result && output) memory.write_bytes(cpu.x[1], std::span<const std::uint8_t>(
                point ? reinterpret_cast<const std::uint8_t*>(&coordinate) : reinterpret_cast<const std::uint8_t*>(&rectangle), bytes));
            cpu.x[0] = static_cast<std::uint32_t>(result);
        }
        callback_failure();
        return true;
    }
    if (dll == "gdi32.dll" && (name == "GetDeviceCaps" || name == "ChoosePixelFormat" ||
        name == "DescribePixelFormat" || name == "SetPixelFormat" || name == "GetPixelFormat" || name == "SwapBuffers")) {
        const auto dc = dc_for(cpu.x[0]);
        if (dc == nullptr) return fail(ERROR_INVALID_HANDLE);
        static_assert(sizeof(PIXELFORMATDESCRIPTOR) == 40);
        if (name == "GetDeviceCaps") cpu.x[0] = static_cast<std::uint32_t>(::GetDeviceCaps(dc, static_cast<int>(cpu.x[1])));
        else if (name == "GetPixelFormat") cpu.x[0] = static_cast<std::uint32_t>(::GetPixelFormat(dc));
        else if (name == "SwapBuffers") cpu.x[0] = static_cast<std::uint32_t>(::SwapBuffers(dc));
        else {
            PIXELFORMATDESCRIPTOR descriptor{};
            const bool describe = name == "DescribePixelFormat";
            const auto pointer = cpu.x[describe ? 3 : name == "ChoosePixelFormat" ? 1 : 2];
            if ((!describe || pointer != 0) && !guest_range(memory, pointer, sizeof(descriptor), describe)) return fail(ERROR_NOACCESS);
            if (describe && cpu.x[2] != sizeof(descriptor)) return fail(ERROR_INVALID_PARAMETER);
            if (!describe) memory.read_bytes(pointer, std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(&descriptor), sizeof(descriptor)));
            const auto result = describe ? ::DescribePixelFormat(dc, static_cast<int>(cpu.x[1]), sizeof(descriptor), pointer == 0 ? nullptr : &descriptor)
                : name == "ChoosePixelFormat" ? ::ChoosePixelFormat(dc, &descriptor)
                : ::SetPixelFormat(dc, static_cast<int>(cpu.x[1]), &descriptor);
            const auto error = GetLastError();
            if (describe && result && pointer != 0) memory.write_bytes(pointer, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(&descriptor), sizeof(descriptor)));
            cpu.x[0] = static_cast<std::uint32_t>(result);
            SetLastError(error);
        }
        return true;
    }
    if (dll != "opengl32.dll") return false;
    if (name == "wglCreateContext") {
        const auto dc = dc_for(cpu.x[0]);
        if (dc == nullptr) return fail(ERROR_INVALID_HANDLE);
        const auto context = ::wglCreateContext(dc);
        const auto error = GetLastError();
        cpu.x[0] = 0;
        if (context != nullptr) {
            std::lock_guard lock(state_->mutex);
            cpu.x[0] = state_->next_gl_context;
            state_->next_gl_context += 16;
            state_->gl_contexts.emplace(cpu.x[0], context);
        }
        SetLastError(error);
        return true;
    }
    if (name == "wglMakeCurrent" || name == "wglDeleteContext" || name == "wglShareLists") {
        const auto context = context_for(cpu.x[name == "wglMakeCurrent" ? 1 : 0]);
        const auto dc = name == "wglMakeCurrent" ? dc_for(cpu.x[0]) : nullptr;
        const auto other = name == "wglShareLists" ? context_for(cpu.x[1]) : nullptr;
        if ((name != "wglMakeCurrent" && context == nullptr) ||
            (name == "wglMakeCurrent" && ((cpu.x[0] != 0 && dc == nullptr) || (cpu.x[1] != 0 && context == nullptr))) ||
            (name == "wglShareLists" && other == nullptr)) return fail(ERROR_INVALID_HANDLE);
        if (name == "wglDeleteContext" && (state_->gl_buffer_mappings->has_context(context) || state_->gl_sync_objects->has_context(context)))
            return fail(ERROR_BUSY);
        const auto result = name == "wglMakeCurrent" ? ::wglMakeCurrent(dc, context)
            : name == "wglShareLists" ? ::wglShareLists(context, other) : ::wglDeleteContext(context);
        const auto error = GetLastError();
        if (result && name == "wglDeleteContext") {
            std::lock_guard lock(state_->mutex);
            state_->gl_contexts.erase(cpu.x[0]);
        }
        cpu.x[0] = static_cast<std::uint32_t>(result);
        SetLastError(error);
        return true;
    }
    if (name == "wglGetCurrentContext" || name == "wglGetCurrentDC") {
        std::lock_guard lock(state_->mutex);
        cpu.x[0] = 0;
        if (name == "wglGetCurrentContext") {
            const auto current = ::wglGetCurrentContext();
            for (const auto& [token, context] : state_->gl_contexts) if (context == current) cpu.x[0] = token;
        } else {
            const auto current = ::wglGetCurrentDC();
            for (const auto& [token, dc] : state_->gdi_dcs) if (dc.host == current) cpu.x[0] = token;
        }
        return true;
    }
    if (name == "glGetString") {
        const auto text = ::glGetString(static_cast<GLenum>(cpu.x[0]));
        cpu.x[0] = copy_gl_string(reinterpret_cast<const char*>(text), memory);
        return true;
    }
    if (name == "glGetError") { cpu.x[0] = ::glGetError(); return true; }
    if (name == "glGetIntegerv" || name == "glGetFloatv" || name == "glGetBooleanv") {
        const auto parameter = static_cast<GLenum>(cpu.x[0]);
        unsigned count = 1;
        switch (parameter) {
        case GL_VIEWPORT: case GL_SCISSOR_BOX: case GL_COLOR_CLEAR_VALUE:
        case GL_COLOR_WRITEMASK: count = 4; break;
        case GL_DEPTH_RANGE: case GL_MAX_VIEWPORT_DIMS:
        case 0x846d: case 0x846e: count = 2; break; // aliased point/line size ranges
        case GL_MODELVIEW_MATRIX: case GL_PROJECTION_MATRIX: case GL_TEXTURE_MATRIX: count = 16; break;
        case 0x86a3: case 0x8df8: case 0x87ff:
            throw std::runtime_error("variable-length OpenGL state query is not yet supported");
        default: break;
        }
        const auto bytes = count * (name == "glGetBooleanv" ? 1 : 4);
        if (!guest_range(memory, cpu.x[1], bytes, true)) return fail(ERROR_NOACCESS);
        std::array<GLint, 16> integers{};
        std::array<GLfloat, 16> floats{};
        std::array<GLboolean, 16> booleans{};
        const std::uint8_t* output = nullptr;
        if (name == "glGetIntegerv") { ::glGetIntegerv(parameter, integers.data()); output = reinterpret_cast<const std::uint8_t*>(integers.data()); }
        else if (name == "glGetFloatv") { ::glGetFloatv(parameter, floats.data()); output = reinterpret_cast<const std::uint8_t*>(floats.data()); }
        else { ::glGetBooleanv(parameter, booleans.data()); output = booleans.data(); }
        memory.write_bytes(cpu.x[1], std::span<const std::uint8_t>(output, bytes));
        return true;
    }
    if (name == "glClear") { ::glClear(static_cast<GLbitfield>(cpu.x[0])); return true; }
    if (name == "glClearColor") {
        ::glClearColor(std::bit_cast<float>(static_cast<std::uint32_t>(cpu.v[0][0])),
            std::bit_cast<float>(static_cast<std::uint32_t>(cpu.v[1][0])),
            std::bit_cast<float>(static_cast<std::uint32_t>(cpu.v[2][0])),
            std::bit_cast<float>(static_cast<std::uint32_t>(cpu.v[3][0])));
        return true;
    }
    if (name == "glViewport") { ::glViewport(static_cast<GLint>(cpu.x[0]), static_cast<GLint>(cpu.x[1]), static_cast<GLsizei>(cpu.x[2]), static_cast<GLsizei>(cpu.x[3])); return true; }
    if (name == "glReadPixels") {
        gl_detail::read_pixels(cpu, memory, &::glReadPixels);
        return true;
    }
    if (name == "glFlush" || name == "glFinish") { if (name == "glFlush") ::glFlush(); else ::glFinish(); return true; }
    if (name == "wglGetProcAddress" || name == "transmuteGetProcAddress") {
        // Publish guest adapters, never x64 code addresses. GL loaders eagerly
        // enumerate unused exports; diagnose missing adapters only on invocation.
        const auto symbol = guest_ansi_string(memory, cpu.x[name == "wglGetProcAddress" ? 0 : 1]);
        constexpr std::array<std::string_view, 18> supported{
            "wglCreateContext", "wglDeleteContext", "wglMakeCurrent", "wglShareLists",
            "wglGetCurrentContext", "wglGetCurrentDC", "wglGetProcAddress", "glGetString",
            "glGetError", "glClear", "glClearColor", "glViewport", "glFinish", "glFlush",
            "glGetIntegerv", "glGetFloatv", "glGetBooleanv", "glReadPixels"};
        cpu.x[0] = 0;
        const bool extension = name == "wglGetProcAddress";
        const auto target = extension ? ::wglGetProcAddress(symbol.c_str())
            : ::GetProcAddress(::GetModuleHandleW(L"opengl32.dll"), symbol.c_str());
        const auto native = reinterpret_cast<std::uintptr_t>(target);
        if (native <= 3 || native == UINTPTR_MAX) return fail(ERROR_PROC_NOT_FOUND);
        const auto key = symbol + (extension ? "@" + std::to_string(
            reinterpret_cast<std::uintptr_t>(::wglGetCurrentContext())) : "@core");
        auto handler = gl_detail::resolve_buffer(symbol, native, state_->gl_buffer_mappings);
        if (!handler) handler = gl_detail::resolve_sync(symbol, native, state_->gl_sync_objects);
        if (!handler) handler = gl_detail::resolve_reflection(symbol, native);
        if (!handler) handler = gl_detail::resolve_scalar(symbol, native);
        if (!handler) handler = gl_detail::resolve_memory(symbol, native);
        const bool core = !extension && std::find(supported.begin(), supported.end(), symbol) != supported.end();
        const bool typed_extension = symbol == "wglCreateContextAttribsARB" || symbol == "wglChoosePixelFormatARB" ||
            symbol == "wglGetPixelFormatAttribivARB" || symbol == "wglGetPixelFormatAttribfvARB" ||
            symbol == "wglGetExtensionsStringARB" || symbol == "wglGetExtensionsStringEXT" || symbol == "glGetStringi";
        if (!handler && !core && !typed_extension)
            handler = [symbol](CpuState&, GuestMemory&) {
                throw std::runtime_error("OpenGL function needs a typed guest adapter: " + symbol);
            };
        auto* engine = dynamic_cast<ReferenceEngine*>(&memory);
        if (engine == nullptr) return fail(ERROR_NOT_SUPPORTED);
        {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->gl_exports.find(key);
            if (found != state_->gl_exports.end()) { cpu.x[0] = found->second; return true; }
        }
        if (!handler) handler = [this, symbol, native, core](CpuState& call, GuestMemory& guest) {
            const auto handled = core ? dispatch_gui({"opengl32.dll", symbol, 0}, call, guest)
                : dispatch_gl_extension(symbol, native, call, guest);
            if (!handled) throw std::runtime_error("unbound OpenGL guest call " + symbol);
        };
        const auto candidate = engine->register_guest_call(std::move(handler));
        bool inserted = false;
        {
            std::lock_guard lock(state_->mutex);
            const auto result = state_->gl_exports.try_emplace(key, candidate);
            inserted = result.second;
            cpu.x[0] = result.first->second;
        }
        if (!inserted) engine->unregister_guest_call(candidate);
        return true;
    }
    return false;
}

std::uint64_t Win32Bridge::copy_gl_string(const char* text, GuestMemory& memory) {
    if (text == nullptr) return 0;
    const std::string value(text);
    std::lock_guard lock(state_->mutex);
    const auto existing = state_->gl_strings.find(value);
    if (existing != state_->gl_strings.end()) return existing->second;
    const auto address = state_->next_gl_string;
    // ARM64 CRT string scans read whole vectors through the terminator. Mirror
    // readable Windows pages, with zero padding, rather than an exact string
    // extent that faults during a valid scan.
    const auto allocation = (value.size() + guest_page_size) & ~(guest_page_size - 1);
    memory.map_memory(address, allocation);
    memory.write_bytes(address, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(value.c_str()), value.size() + 1));
    memory.set_memory_access(address, true, false, false);
    state_->next_gl_string += allocation;
    state_->gl_strings.emplace(value, address);
    return address;
}

bool Win32Bridge::dispatch_gl_extension(std::string_view name, std::uintptr_t native,
                                       CpuState& cpu, GuestMemory& memory) {
    const auto dc_for = [this](std::uint64_t token) -> HDC {
        std::lock_guard lock(state_->mutex);
        const auto found = state_->gdi_dcs.find(token);
        return found == state_->gdi_dcs.end() ? nullptr : found->second.host;
    };
    const auto fail = [&cpu](DWORD error) { cpu.x[0] = 0; SetLastError(error); return true; };
    const auto attributes = [&memory](std::uint64_t pointer, bool floating) {
        std::vector<std::uint32_t> result;
        if (pointer == 0) return result;
        for (unsigned i = 0; i < 256; ++i) {
            const auto key = memory.read32(pointer + i * 8ULL);
            result.push_back(key);
            if ((floating ? (key & 0x7fffffffU) : key) == 0) return result;
            result.push_back(memory.read32(pointer + i * 8ULL + 4));
        }
        throw std::runtime_error("unterminated WGL attribute list");
    };
    if (name == "glGetStringi") {
        const auto fn = reinterpret_cast<const GLubyte*(APIENTRY*)(GLenum, GLuint)>(native);
        cpu.x[0] = copy_gl_string(reinterpret_cast<const char*>(fn(static_cast<GLenum>(cpu.x[0]), static_cast<GLuint>(cpu.x[1]))), memory);
        return true;
    }
    if (name == "wglGetExtensionsStringARB" || name == "wglGetExtensionsStringEXT") {
        const char* text = nullptr;
        if (name.ends_with("ARB")) {
            const auto dc = dc_for(cpu.x[0]);
            if (dc == nullptr) return fail(ERROR_INVALID_HANDLE);
            text = reinterpret_cast<const char*(WINAPI*)(HDC)>(native)(dc);
        } else text = reinterpret_cast<const char*(WINAPI*)()>(native)();
        cpu.x[0] = copy_gl_string(text, memory);
        return true;
    }
    if (name == "wglCreateContextAttribsARB") {
        const auto dc = dc_for(cpu.x[0]);
        if (dc == nullptr) return fail(ERROR_INVALID_HANDLE);
        HGLRC shared = nullptr;
        if (cpu.x[1] != 0) {
            std::lock_guard lock(state_->mutex);
            const auto found = state_->gl_contexts.find(cpu.x[1]);
            if (found == state_->gl_contexts.end()) return fail(ERROR_INVALID_HANDLE);
            shared = found->second;
        }
        const auto list = attributes(cpu.x[2], false);
        const auto context = reinterpret_cast<HGLRC(WINAPI*)(HDC, HGLRC, const int*)>(native)(
            dc, shared, list.empty() ? nullptr : reinterpret_cast<const int*>(list.data()));
        const auto error = GetLastError();
        cpu.x[0] = 0;
        if (context != nullptr) {
            std::lock_guard lock(state_->mutex);
            cpu.x[0] = state_->next_gl_context;
            state_->next_gl_context += 16;
            state_->gl_contexts.emplace(cpu.x[0], context);
        }
        SetLastError(error);
        return true;
    }
    if (name == "wglChoosePixelFormatARB") {
        const auto dc = dc_for(cpu.x[0]);
        const auto capacity = cpu.x[3];
        if (dc == nullptr) return fail(ERROR_INVALID_HANDLE);
        if (capacity > 4096 || !guest_range(memory, cpu.x[5], 4, true) ||
            (capacity != 0 && !guest_range(memory, cpu.x[4], capacity * 4, true))) return fail(ERROR_NOACCESS);
        const auto integers = attributes(cpu.x[1], false);
        const auto floats = attributes(cpu.x[2], true);
        std::vector<int> formats(static_cast<std::size_t>(capacity));
        UINT count = 0;
        const auto result = reinterpret_cast<BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*)>(native)(dc,
            integers.empty() ? nullptr : reinterpret_cast<const int*>(integers.data()),
            floats.empty() ? nullptr : reinterpret_cast<const FLOAT*>(floats.data()),
            static_cast<UINT>(capacity), formats.data(), &count);
        const auto error = GetLastError();
        if (result) {
            memory.write32(cpu.x[5], count);
            memory.write_bytes(cpu.x[4], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(formats.data()), std::min<std::size_t>(count, formats.size()) * 4));
        }
        cpu.x[0] = static_cast<std::uint32_t>(result);
        SetLastError(error);
        return true;
    }
    if (name == "wglGetPixelFormatAttribivARB" || name == "wglGetPixelFormatAttribfvARB") {
        const auto dc = dc_for(cpu.x[0]);
        const auto count = cpu.x[3];
        if (dc == nullptr) return fail(ERROR_INVALID_HANDLE);
        if (count > 4096 || !guest_range(memory, cpu.x[4], count * 4, false) ||
            !guest_range(memory, cpu.x[5], count * 4, true)) return fail(ERROR_NOACCESS);
        std::vector<int> requested(static_cast<std::size_t>(count));
        memory.read_bytes(cpu.x[4], std::span<std::uint8_t>(reinterpret_cast<std::uint8_t*>(requested.data()), requested.size() * 4));
        std::vector<std::uint32_t> values(static_cast<std::size_t>(count));
        const auto result = name == "wglGetPixelFormatAttribivARB"
            ? reinterpret_cast<BOOL(WINAPI*)(HDC, int, int, UINT, const int*, int*)>(native)(dc,
                static_cast<int>(cpu.x[1]), static_cast<int>(cpu.x[2]), static_cast<UINT>(count), requested.data(), reinterpret_cast<int*>(values.data()))
            : reinterpret_cast<BOOL(WINAPI*)(HDC, int, int, UINT, const int*, FLOAT*)>(native)(dc,
                static_cast<int>(cpu.x[1]), static_cast<int>(cpu.x[2]), static_cast<UINT>(count), requested.data(), reinterpret_cast<FLOAT*>(values.data()));
        const auto error = GetLastError();
        if (result) memory.write_bytes(cpu.x[5], std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values.data()), values.size() * 4));
        cpu.x[0] = static_cast<std::uint32_t>(result);
        SetLastError(error);
        return true;
    }
    return false;
}
