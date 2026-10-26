# QuickLookThumbnailing (src/alchemy/darwin/quicklookthumbnailing/): see its
# README. See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/quicklookthumbnailing/install.cpp)
alchemy_gui_add_framework_case(quicklookthumbnailing src/alchemy/darwin/quicklookthumbnailing/tests/quicklookthumbnailing_tests.cpp)
