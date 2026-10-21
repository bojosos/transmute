# CoreML (src/alchemy/darwin/coreml/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coreml/install.cpp)
alchemy_gui_add_framework_case(coreml src/alchemy/darwin/coreml/tests/coreml_tests.cpp)
