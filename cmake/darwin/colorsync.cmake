# ColorSync (src/alchemy/darwin/colorsync/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern. Headless (both builds).
target_sources(alchemy_core PRIVATE src/alchemy/darwin/colorsync/install.cpp)
alchemy_gui_add_framework_case(colorsync src/alchemy/darwin/colorsync/tests/colorsync_tests.cpp)
