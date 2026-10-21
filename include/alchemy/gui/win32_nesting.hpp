#pragma once

// How deeply the current thread is nested in the window server's window
// procedure. Win32 modal loops (window move/size, menus, scroll bars) run
// inside a window procedure (DefWindowProc) or under the run loop's
// modal-panel pump, so at depth 0 outside that pump the thread cannot be in
// one: the run loop then skips its GetGUIThreadInfo query (a system call per
// step, about 0.4 ms per frame while orbiting with real input).

namespace alchemy::gui {

inline thread_local unsigned window_procedure_depth = 0;

struct WindowProcedureScope {
    WindowProcedureScope() noexcept { ++window_procedure_depth; }
    ~WindowProcedureScope() { --window_procedure_depth; }
    WindowProcedureScope(const WindowProcedureScope&) = delete;
    WindowProcedureScope& operator=(const WindowProcedureScope&) = delete;
};

} // namespace alchemy::gui
