#include "alchemy/host/win32/gui/cloak.hpp"

#include <dwmapi.h>

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace alchemy::gui::win32 {

bool gui_cloak_requested() noexcept {
    static const bool requested = [] {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "ALCHEMY_GUI_CLOAK") != 0 || value == nullptr) return false;
        const std::string text(value);
        std::free(value);
        return !text.empty() && text != "0";
    }();
    return requested;
}

DWORD gui_cloak_state(HWND hwnd) noexcept {
    DWORD cloaked = 0;
    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) return 0;
    return cloaked;
}

void apply_gui_cloak(HWND hwnd) {
    if (!gui_cloak_requested() || hwnd == nullptr) return;
    const BOOL cloak = TRUE;
    const auto result = DwmSetWindowAttribute(hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    if (FAILED(result)) {
        char text[96];
        std::snprintf(text, sizeof text, "Alchemy window server: ALCHEMY_GUI_CLOAK: DwmSetWindowAttribute failed 0x%08lx",
                      static_cast<unsigned long>(result));
        throw std::runtime_error(text);
    }
    if (gui_cloak_state(hwnd) == 0)
        throw std::runtime_error("Alchemy window server: ALCHEMY_GUI_CLOAK: the window is not cloaked after DWMWA_CLOAK");
}

} // namespace alchemy::gui::win32
