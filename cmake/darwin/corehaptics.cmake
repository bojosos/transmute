# CoreHaptics (src/alchemy/darwin/corehaptics/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/corehaptics/install.cpp)
alchemy_gui_add_framework_case(corehaptics src/alchemy/darwin/corehaptics/tests/corehaptics_tests.cpp)
