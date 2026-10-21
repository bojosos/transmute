# ScreenCaptureKit (src/alchemy/darwin/screencapturekit/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/screencapturekit/install.cpp)
alchemy_gui_add_framework_case(screencapturekit src/alchemy/darwin/screencapturekit/tests/screencapturekit_tests.cpp)
