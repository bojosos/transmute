# SystemExtensions (src/alchemy/darwin/systemextensions/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/systemextensions/install.cpp)
alchemy_gui_add_framework_case(systemextensions src/alchemy/darwin/systemextensions/tests/systemextensions_tests.cpp)
