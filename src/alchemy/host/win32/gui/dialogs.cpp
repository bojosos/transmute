// Win32 host backend, area "ui": dialogs (include/alchemy/host/ui.hpp). The
// alert is a task dialog (TaskDialogIndirect, comctl32 v6); the file dialog
// is the common item dialog (IFileOpenDialog / IFileSaveDialog).
#include "alchemy/host/ui.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <commctrl.h>
#include <shobjidl.h>

#include <algorithm>
#include <cstdio>
#include <exception>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace alchemy::host::ui {
namespace {

// comctl32 v6 (the only comctl32 with TaskDialogIndirect) is chosen by an
// activation context naming Microsoft.Windows.Common-Controls 6.0. The host
// executable carries no manifest, so the context comes from the manifest
// shell32.dll embeds for that purpose (resource 124).
struct CommonControls {
    HANDLE context{INVALID_HANDLE_VALUE};
    decltype(&TaskDialogIndirect) task_dialog{};
    std::string error;
};
const CommonControls& common_controls() {
    static const CommonControls loaded = [] {
        CommonControls result;
        wchar_t directory[MAX_PATH]{};
        const auto length = GetSystemDirectoryW(directory, MAX_PATH);
        if (length == 0 || length >= MAX_PATH) { result.error = "GetSystemDirectory failed"; return result; }
        const std::wstring shell32 = std::wstring(directory) + L"\\shell32.dll";
        ACTCTXW description{};
        description.cbSize = sizeof(description);
        description.dwFlags = ACTCTX_FLAG_RESOURCE_NAME_VALID | ACTCTX_FLAG_ASSEMBLY_DIRECTORY_VALID;
        description.lpSource = shell32.c_str();
        description.lpAssemblyDirectory = directory;
        description.lpResourceName = MAKEINTRESOURCEW(124);
        result.context = CreateActCtxW(&description);
        if (result.context == INVALID_HANDLE_VALUE) {
            result.error = "CreateActCtx for Common Controls 6 failed (" + std::to_string(GetLastError()) + ")";
            return result;
        }
        ULONG_PTR cookie{};
        if (!ActivateActCtx(result.context, &cookie)) {
            result.error = "ActivateActCtx failed (" + std::to_string(GetLastError()) + ")";
            return result;
        }
        const auto module = LoadLibraryW(L"comctl32.dll");
        DeactivateActCtx(0, cookie);
        if (!module) { result.error = "comctl32.dll did not load (" + std::to_string(GetLastError()) + ")"; return result; }
        result.task_dialog = reinterpret_cast<decltype(&TaskDialogIndirect)>(
            reinterpret_cast<void*>(GetProcAddress(module, "TaskDialogIndirect")));
        if (!result.task_dialog) result.error = "comctl32.dll has no TaskDialogIndirect (not version 6)";
        return result;
    }();
    return loaded;
}

// '&' marks a mnemonic in Win32 button text; AppKit titles are literal.
std::wstring button_text(std::string_view title) {
    std::wstring result;
    for (const auto c : win32::widen(title)) {
        result += c;
        if (c == L'&') result += L'&';
    }
    return result;
}

#ifndef TDF_NO_SET_FOREGROUND
#define TDF_NO_SET_FOREGROUND 0x00010000
#endif

constexpr int first_button_id = 1000;

struct CallbackData { const std::function<void(std::uintptr_t)>* created; std::exception_ptr error; };
HRESULT CALLBACK task_dialog_callback(HWND window, UINT notification, WPARAM, LPARAM, LONG_PTR reference) {
    auto& data = *reinterpret_cast<CallbackData*>(reference);
    if (notification == TDN_CREATED && data.created && *data.created) {
        try { (*data.created)(reinterpret_cast<std::uintptr_t>(window)); } catch (...) { data.error = std::current_exception(); }
    }
    return S_OK;
}

} // namespace

Result<AlertAnswer> show_alert(const AlertDialog& alert) {
    if (alert.buttons.empty()) return Error::invalid_argument;
    const auto& controls = common_controls();
    if (!controls.task_dialog) {
        std::fprintf(stderr, "alchemy: the Win32 task dialog is unavailable: %s\n", controls.error.c_str());
        return Error::unsupported;
    }
    // macOS lays the first button out rightmost; Win32 lays buttons out left
    // to right in array order.
    std::vector<std::wstring> titles;
    for (const auto& title : alert.buttons) titles.push_back(button_text(title));
    std::vector<TASKDIALOG_BUTTON> buttons;
    for (std::size_t index = alert.buttons.size(); index-- > 0;)
        buttons.push_back({static_cast<int>(first_button_id + index), titles[index].c_str()});
    const auto caption = win32::widen(alert.caption), instruction = win32::widen(alert.instruction),
               content = win32::widen(alert.content);
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = reinterpret_cast<HWND>(alert.owner);
    config.dwFlags = TDF_SIZE_TO_CONTENT | (alert.owner ? TDF_POSITION_RELATIVE_TO_WINDOW : 0) |
                     (alert.cancel_button ? TDF_ALLOW_DIALOG_CANCELLATION : 0);
    if (alert.no_foreground) config.dwFlags |= TDF_NO_SET_FOREGROUND;
    config.pszWindowTitle = caption.empty() ? nullptr : caption.c_str();
    config.pszMainIcon = alert.icon == AlertIcon::error ? TD_ERROR_ICON
                       : alert.icon == AlertIcon::warning ? TD_WARNING_ICON : TD_INFORMATION_ICON;
    config.pszMainInstruction = instruction.empty() ? nullptr : instruction.c_str();
    config.pszContent = content.empty() ? nullptr : content.c_str();
    const auto verification = win32::widen(alert.verification);
    if (!alert.verification.empty()) {
        config.pszVerificationText = verification.c_str();
        if (alert.verification_checked) config.dwFlags |= TDF_VERIFICATION_FLAG_CHECKED;
    }
    config.cButtons = static_cast<UINT>(buttons.size());
    config.pButtons = buttons.data();
    config.nDefaultButton = first_button_id;
    if (alert.width > 0) {
        // Points -> dialog units of the dialog font (4 units per average
        // character width).
        const auto base = LOWORD(GetDialogBaseUnits());
        config.cxWidth = static_cast<UINT>(std::clamp(alert.width * 4 / std::max<int>(base, 1), 0.0, 2000.0));
    }
    CallbackData data{&alert.created, nullptr};
    config.pfCallback = task_dialog_callback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&data);
    ULONG_PTR cookie{};
    const bool activated = ActivateActCtx(controls.context, &cookie) != FALSE;
    int pressed{};
    BOOL verified{};
    const auto result = controls.task_dialog(&config, &pressed, nullptr, &verified);
    if (activated) DeactivateActCtx(0, cookie);
    if (data.error) std::rethrow_exception(data.error);
    if (FAILED(result)) {
        std::fprintf(stderr, "alchemy: TaskDialogIndirect failed (0x%08lx)\n", static_cast<unsigned long>(result));
        return Error::other;
    }
    AlertAnswer answer;
    answer.verification_checked = verified != FALSE;
    if (pressed == IDCANCEL && alert.cancel_button) {
        answer.button = *alert.cancel_button;
        return answer;
    }
    const auto index = static_cast<std::int64_t>(pressed) - first_button_id;
    if (index < 0 || index >= static_cast<std::int64_t>(alert.buttons.size())) {
        std::fprintf(stderr, "alchemy: the task dialog returned an unknown button %d\n", pressed);
        return Error::out_of_range;
    }
    answer.button = static_cast<std::size_t>(index);
    return answer;
}

Result<std::optional<std::vector<std::string>>> run_file_dialog(const FileDialog& request) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Uninitialize { bool active; ~Uninitialize() { if (active) CoUninitialize(); } } uninitialize{SUCCEEDED(initialized)};
    IFileDialog* dialog{};
    if (FAILED(CoCreateInstance(request.save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        return Error::unsupported;
    struct Release { IUnknown* object; ~Release() { if (object) object->Release(); } } release_dialog{dialog};
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
    if (!request.save && request.folders) options |= FOS_PICKFOLDERS;
    if (!request.save && request.multiple) options |= FOS_ALLOWMULTISELECT;
    if (request.show_hidden) options |= FOS_FORCESHOWHIDDEN;
    if (request.save) options |= FOS_OVERWRITEPROMPT;
    dialog->SetOptions(options);
    const auto title = win32::widen(request.title);
    if (!title.empty()) dialog->SetTitle(title.c_str());
    const auto prompt = win32::widen(request.ok_label), label = win32::widen(request.name_label), name = win32::widen(request.name);
    if (!prompt.empty()) dialog->SetOkButtonLabel(prompt.c_str());
    if (!label.empty()) dialog->SetFileNameLabel(label.c_str());
    if (!name.empty()) dialog->SetFileName(name.c_str());
    if (!request.directory.empty()) {
        IShellItem* folder{};
        const auto host = win32::widen(request.directory);
        if (SUCCEEDED(SHCreateItemFromParsingName(host.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    std::wstring pattern, any = L"*.*";
    for (const auto& extension : request.extensions) pattern += (pattern.empty() ? L"*." : L";*.") + win32::widen(extension);
    std::vector<COMDLG_FILTERSPEC> filters;
    if (!pattern.empty()) filters.push_back({L"Supported files", pattern.c_str()});
    if (pattern.empty() || request.allow_other_types) filters.push_back({L"All files", any.c_str()});
    dialog->SetFileTypes(static_cast<UINT>(filters.size()), filters.data());
    if (request.save && !request.extensions.empty()) dialog->SetDefaultExtension(win32::widen(request.extensions.front()).c_str());
    const auto shown = dialog->Show(reinterpret_cast<HWND>(request.owner));
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return std::optional<std::vector<std::string>>{};
    if (FAILED(shown)) return Error::io;
    const auto path_of = [](IShellItem* item) -> std::optional<std::string> {
        PWSTR text{};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &text))) return std::nullopt;
        auto path = win32::narrow(text);
        CoTaskMemFree(text);
        return path;
    };
    std::vector<std::string> paths;
    if (request.save) {
        IShellItem* item{};
        if (FAILED(dialog->GetResult(&item))) return std::optional<std::vector<std::string>>{};
        if (const auto path = path_of(item)) paths.push_back(*path);
        item->Release();
    } else {
        IShellItemArray* list{};
        if (FAILED(static_cast<IFileOpenDialog*>(dialog)->GetResults(&list))) return std::optional<std::vector<std::string>>{};
        DWORD count{};
        list->GetCount(&count);
        for (DWORD i = 0; i < count; ++i) {
            IShellItem* item{};
            if (FAILED(list->GetItemAt(i, &item))) continue;
            if (const auto path = path_of(item)) paths.push_back(*path);
            item->Release();
        }
        list->Release();
    }
    return std::optional<std::vector<std::string>>{std::move(paths)};
}

std::vector<std::uintptr_t> disable_windows(std::span<const std::uintptr_t> windows, std::uintptr_t keep) {
    const auto keep_root = keep ? GetAncestor(reinterpret_cast<HWND>(keep), GA_ROOT) : nullptr;
    std::vector<std::uintptr_t> disabled;
    for (const auto handle : windows) {
        const auto root = handle ? GetAncestor(reinterpret_cast<HWND>(handle), GA_ROOT) : nullptr;
        if (!root || root == keep_root || !IsWindowEnabled(root)) continue;
        const auto value = reinterpret_cast<std::uintptr_t>(root);
        if (std::find(disabled.begin(), disabled.end(), value) != disabled.end()) continue;
        EnableWindow(root, FALSE);
        disabled.push_back(value);
    }
    return disabled;
}

void enable_windows(std::span<const std::uintptr_t> windows) {
    for (const auto handle : windows)
        if (const auto window = reinterpret_cast<HWND>(handle); IsWindow(window)) EnableWindow(window, TRUE);
}

} // namespace alchemy::host::ui
