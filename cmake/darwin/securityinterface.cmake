# SecurityInterface (src/alchemy/darwin/securityinterface/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/securityinterface/install.cpp)
alchemy_gui_add_framework_case(securityinterface src/alchemy/darwin/securityinterface/tests/securityinterface_tests.cpp)
