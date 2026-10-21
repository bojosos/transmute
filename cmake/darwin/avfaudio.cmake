# AVFAudio (src/alchemy/darwin/avfaudio/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/avfaudio/install.cpp)
alchemy_gui_add_framework_case(avfaudio src/alchemy/darwin/avfaudio/tests/avfaudio_tests.cpp)
