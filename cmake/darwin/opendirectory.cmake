# OpenDirectory (src/alchemy/darwin/opendirectory/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/opendirectory/constants.cpp
    src/alchemy/darwin/opendirectory/sessions.cpp)
alchemy_gui_add_framework_case(opendirectory src/alchemy/darwin/opendirectory/tests/opendirectory_tests.cpp)
