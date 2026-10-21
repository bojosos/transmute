# SkyLight (src/alchemy/darwin/skylight/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/skylight/install.cpp)
alchemy_gui_add_framework_case(skylight src/alchemy/darwin/skylight/tests/skylight_tests.cpp)
