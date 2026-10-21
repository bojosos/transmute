// Win32 host backend, area "ui": the shell (ShellExecute, Explorer, recent
// documents, the desktop wallpaper) and the user's appearance settings
// (include/alchemy/host/ui.hpp).
#include "alchemy/host/ui.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <map>
#include <vector>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace alchemy::host::ui {
namespace {

// ShellExecute's result: > 32 on success, else an error code.
Status shell_result(HINSTANCE instance) {
    const auto result = reinterpret_cast<INT_PTR>(instance);
    if (result > 32) return {};
    return result == 0 ? Error::no_memory : win32::error(static_cast<DWORD>(result));
}

} // namespace

Result<bool> screen_reader() {
    BOOL reader{};
    if (!SystemParametersInfoW(SPI_GETSCREENREADER, 0, &reader, 0)) return win32::last_error();
    return reader != FALSE;
}

Result<bool> dark_appearance() {
    DWORD light = 1, size = sizeof(light);
    const auto status = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    if (status != ERROR_SUCCESS) return win32::error(static_cast<DWORD>(status));
    return light == 0;
}

Result<std::array<std::uint8_t, 3>> pixel_under_pointer() {
    POINT point{};
    if (!GetPhysicalCursorPos(&point)) return win32::last_error();
    const auto dc = GetDC(nullptr);
    if (!dc) return Error::unsupported;
    const auto value = GetPixel(dc, point.x, point.y);
    ReleaseDC(nullptr, dc);
    if (value == CLR_INVALID) return Error::out_of_range;
    return std::array<std::uint8_t, 3>{GetRValue(value), GetGValue(value), GetBValue(value)};
}

Status shell_open(std::string_view target, std::string_view verb, std::string_view parameters) {
    const auto wide_parameters = win32::widen(parameters);
    return shell_result(ShellExecuteW(nullptr, win32::widen(verb).c_str(), win32::widen(target).c_str(),
                                      parameters.empty() ? nullptr : wide_parameters.c_str(), nullptr, SW_SHOWNORMAL));
}

Status reveal_file(std::string_view path) {
    const auto arguments = L"/select,\"" + win32::widen(path) + L"\"";
    return shell_result(ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
}

Status reveal_files(std::span<const std::string> paths) {
    std::map<std::wstring, std::vector<std::wstring>> by_folder;
    for (const auto& path : paths) {
        const std::filesystem::path host(win32::widen(path));
        by_folder[host.parent_path().wstring()].push_back(host.wstring());
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (const auto& [folder, files] : by_folder) {
        PIDLIST_ABSOLUTE folder_id = ILCreateFromPathW(folder.c_str());
        if (!folder_id) continue;
        std::vector<PIDLIST_ABSOLUTE> items;
        for (const auto& file : files) if (auto id = ILCreateFromPathW(file.c_str())) items.push_back(id);
        std::vector<PCUITEMID_CHILD> children;
        for (const auto item : items) children.push_back(ILFindLastID(item));
        SHOpenFolderAndSelectItems(folder_id, static_cast<UINT>(children.size()), children.data(), 0);
        for (const auto item : items) ILFree(item);
        ILFree(folder_id);
    }
    return {};
}

void add_recent_document(std::string_view path) {
    const auto host = win32::widen(path);
    SHAddToRecentDocs(SHARD_PATHW, host.c_str());
}

Result<std::string> desktop_picture() {
    wchar_t path[MAX_PATH]{};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0)) return win32::last_error();
    return win32::narrow(path);
}

Status set_desktop_picture(std::string_view path) {
    auto host = win32::widen(path);
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, host.data(), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) return win32::last_error();
    return {};
}

} // namespace alchemy::host::ui
