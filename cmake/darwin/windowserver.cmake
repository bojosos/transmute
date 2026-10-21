# The window server: windows, input, cursors, clipboard, screens, IME.
# Owner: r3 (src/alchemy/darwin/windowserver/: installation and screen
# geometry). Contract: include/alchemy/gui/window_server.hpp; the host's
# implementation is host::ui::WindowSystem (src/alchemy/host/win32/gui/,
# picked up by cmake/AlchemyHost.cmake).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/windowserver/install.cpp
    src/alchemy/darwin/windowserver/screen_layout.cpp)
# System libraries of every supported Windows version (user32/gdi32 for
# windows and input, imm32 for IME, ole32/shell32 for file drops, shcore for
# per-monitor DPI). Nothing touches them before the guest asks for a window.
list(APPEND ALCHEMY_GUI_LINK_LIBRARIES user32 gdi32 imm32 ole32 shell32 shcore dwmapi)
function(alchemy_gui_windowserver_tests)
    alchemy_gui_add_unit_test(alchemy_gui_windowserver_smoke
        src/alchemy/darwin/windowserver/tests/windowserver_smoke_tests.cpp)
    alchemy_gui_add_unit_test(alchemy_gui_windowserver_tables
        src/alchemy/darwin/windowserver/tests/windowserver_table_tests.cpp)
    alchemy_gui_add_unit_test(alchemy_gui_windowserver_win32
        src/alchemy/darwin/windowserver/tests/windowserver_win32_tests.cpp)
    # The GUI-only host areas the window server and media run on (host::ui,
    # host::audio, host::midi): alchemy_host_tests links alchemy_core alone.
    alchemy_gui_add_unit_test(alchemy_gui_host src/alchemy/host/tests/host_gui_tests.cpp)
endfunction()
alchemy_gui_register_tests(alchemy_gui_windowserver_tests)
