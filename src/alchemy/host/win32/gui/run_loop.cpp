// Win32 host backend, area "ui": what the GUI run loop waits on and the
// dialog-loop hook (include/alchemy/host/ui.hpp): MsgWaitForMultipleObjectsEx
// with QS_ALLINPUT, a WH_MSGFILTER hook and a thread timer, GUITHREADINFO.
#include "alchemy/host/ui.hpp"
#include "alchemy/host/win32/win32.hpp"

#include <utility>

#pragma comment(lib, "user32.lib")

namespace alchemy::host::ui {
namespace {

static_assert(sizeof(Handle) == sizeof(HANDLE), "host::Handle carries a HANDLE");

// ---- the dialog-loop hook (main thread only) ----
struct DialogHook {
    HHOOK hook{};
    UINT_PTR timer{};
    UINT period_ms{};
    bool seen{};          // the hook saw the timer's tick inside a dialog loop
    bool enabled{};
    std::function<void()> tick;
};
DialogHook dialog_hook;

void kill_timer() {
    if (dialog_hook.timer) KillTimer(nullptr, dialog_hook.timer);
    dialog_hook.timer = 0;
    dialog_hook.seen = false;
}

void CALLBACK dialog_tick(HWND, UINT, UINT_PTR, DWORD) {
    // Dispatched by a loop that is not a dialog loop (the dialog ended): stop.
    if (!std::exchange(dialog_hook.seen, false)) { kill_timer(); return; }
    if (dialog_hook.enabled && dialog_hook.tick) dialog_hook.tick();
}

LRESULT CALLBACK dialog_filter(int code, WPARAM wparam, LPARAM lparam) {
    if ((code == MSGF_DIALOGBOX || code == MSGF_MESSAGEBOX) && dialog_hook.enabled) {
        const auto* message = reinterpret_cast<const MSG*>(lparam);
        if (!dialog_hook.timer) dialog_hook.timer = SetTimer(nullptr, 0, dialog_hook.period_ms, dialog_tick);
        else if (message->message == WM_TIMER && !message->hwnd && message->wParam == dialog_hook.timer) dialog_hook.seen = true;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

} // namespace

std::size_t wait_limit() noexcept { return MAXIMUM_WAIT_OBJECTS; }

Result<std::size_t> wait_for_input(std::span<const Handle> handles, std::uint32_t timeout_ms, bool messages) {
    if (handles.empty() || handles.size() > MAXIMUM_WAIT_OBJECTS) return Error::invalid_argument;
    const auto count = static_cast<DWORD>(handles.size());
    const auto* native = reinterpret_cast<const HANDLE*>(handles.data());
    // MWMO_INPUTAVAILABLE reports messages that arrived before the wait as well.
    const auto result = messages ? MsgWaitForMultipleObjectsEx(count, native, timeout_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE)
                                 : WaitForMultipleObjects(count, native, FALSE, timeout_ms);
    if (result == WAIT_FAILED) return win32::last_error();
    if (result == WAIT_TIMEOUT) return Error::timed_out;
    if (result < WAIT_OBJECT_0 + count) return static_cast<std::size_t>(result - WAIT_OBJECT_0);
    if (messages && result == WAIT_OBJECT_0 + count) return handles.size();
    if (result >= WAIT_ABANDONED_0 && result < WAIT_ABANDONED_0 + count) return Error::deadlock;
    return Error::other;
}

bool is_gui_thread() noexcept { return IsGUIThread(FALSE) != FALSE; }

bool in_modal_loop() noexcept {
    if (!IsGUIThread(FALSE)) return false;
    GUITHREADINFO info{sizeof(info)};
    if (!GetGUIThreadInfo(GetCurrentThreadId(), &info)) return false;
    return (info.flags & (GUI_INMOVESIZE | GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE)) != 0;
}

Status install_dialog_loop_hook(std::function<void()> tick, std::uint32_t period_ms) {
    if (dialog_hook.hook) return Error::already;
    const auto hook = SetWindowsHookExW(WH_MSGFILTER, dialog_filter, nullptr, GetCurrentThreadId());
    if (!hook) return win32::last_error();
    dialog_hook.hook = hook;
    dialog_hook.period_ms = period_ms;
    dialog_hook.tick = std::move(tick);
    return {};
}

// A disabled hook arms nothing; a running timer then ends on its next tick
// (no dialog loop saw it), on the thread that owns it.
void enable_dialog_loop_ticks(bool enabled) noexcept { dialog_hook.enabled = enabled; }

void remove_dialog_loop_hook() noexcept {
    kill_timer();
    dialog_hook.enabled = false;
    if (dialog_hook.hook) UnhookWindowsHookEx(dialog_hook.hook);
    dialog_hook.hook = nullptr;
    dialog_hook.tick = {};
}

} // namespace alchemy::host::ui
