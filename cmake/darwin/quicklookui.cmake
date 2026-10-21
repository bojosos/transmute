# QuickLookUI (src/alchemy/darwin/quicklookui/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/quicklookui/install.cpp)
alchemy_gui_add_framework_case(quicklookui src/alchemy/darwin/quicklookui/tests/quicklookui_tests.cpp)
