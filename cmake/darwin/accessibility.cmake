# Accessibility (src/alchemy/darwin/accessibility/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/accessibility/install.cpp)
alchemy_gui_add_framework_case(accessibility src/alchemy/darwin/accessibility/tests/accessibility_tests.cpp)
