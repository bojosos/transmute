# DiskArbitration (src/alchemy/darwin/diskarbitration/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/diskarbitration/install.cpp
    src/alchemy/darwin/diskarbitration/sessions.cpp
    src/alchemy/darwin/diskarbitration/claims.cpp)
alchemy_gui_add_framework_case(diskarbitration src/alchemy/darwin/diskarbitration/tests/diskarbitration_tests.cpp)
