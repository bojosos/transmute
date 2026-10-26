# OpenAL (src/alchemy/darwin/openal/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
# alchemy-fx: the mixer over the default output device (mixer.cpp, installer row
# install_openal_gui); alchemy.exe: no output device (install.cpp, headless only;
# openal_fx.cpp is its empty alchemy-fx twin). The enum names are both builds'.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/openal/install.cpp src/alchemy/darwin/openal/enums.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/openal/install.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/openal/mixer.cpp src/alchemy/darwin/openal/openal_fx.cpp)
alchemy_gui_add_framework_case(openal src/alchemy/darwin/openal/tests/openal_tests.cpp)
