# ColorSync (src/alchemy/darwin/colorsync/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern. Headless (both builds).
target_sources(alchemy_core PRIVATE src/alchemy/darwin/colorsync/install.cpp)
alchemy_gui_add_framework_case(colorsync src/alchemy/darwin/colorsync/tests/colorsync_tests.cpp)
# ColorSync profiles over CoreGraphics' ICC code (alchemy-fx; installer row in gui/install.cpp).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/colorsync/profiles.cpp)
