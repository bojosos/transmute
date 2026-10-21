# AuthenticationServices (src/alchemy/darwin/authenticationservices/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/authenticationservices/install.cpp)
alchemy_gui_add_framework_case(authenticationservices src/alchemy/darwin/authenticationservices/tests/authenticationservices_tests.cpp)
