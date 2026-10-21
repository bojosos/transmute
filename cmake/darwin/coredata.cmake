# CoreData (src/alchemy/darwin/coredata/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coredata/install.cpp)
alchemy_gui_add_framework_case(coredata src/alchemy/darwin/coredata/tests/coredata_tests.cpp)
