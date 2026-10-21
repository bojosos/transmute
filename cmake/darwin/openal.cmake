# OpenAL (src/alchemy/darwin/openal/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern. Headless (both builds).
target_sources(alchemy_core PRIVATE src/alchemy/darwin/openal/install.cpp)
alchemy_gui_add_framework_case(openal src/alchemy/darwin/openal/tests/openal_tests.cpp)
