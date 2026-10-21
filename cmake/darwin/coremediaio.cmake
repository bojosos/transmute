# CoreMediaIO (src/alchemy/darwin/coremediaio/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coremediaio/install.cpp)
alchemy_gui_add_framework_case(coremediaio src/alchemy/darwin/coremediaio/tests/coremediaio_tests.cpp)
