# ScreenCaptureKit (src/alchemy/darwin/screencapturekit/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/screencapturekit/install.cpp)
alchemy_gui_add_framework_case(screencapturekit src/alchemy/darwin/screencapturekit/tests/screencapturekit_tests.cpp)
# Own-content capture with the in-memory window server: listings, screenshots,
# window-list images and stream frames never hold a host window's pixels.
function(alchemy_gui_screencapturekit_tests)
    alchemy_gui_add_unit_test(alchemy_gui_screencapturekit_own_content
        src/alchemy/darwin/screencapturekit/tests/own_content_tests.cpp TIMEOUT 120)
endfunction()
alchemy_gui_register_tests(alchemy_gui_screencapturekit_tests)
