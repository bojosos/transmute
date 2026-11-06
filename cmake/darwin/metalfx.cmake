# MetalFX (src/alchemy/darwin/metalfx/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/metalfx/install.cpp)
alchemy_gui_add_framework_case(metalfx src/alchemy/darwin/metalfx/tests/metalfx_tests.cpp)
