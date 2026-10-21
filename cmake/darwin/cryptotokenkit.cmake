# CryptoTokenKit (src/alchemy/darwin/cryptotokenkit/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/cryptotokenkit/install.cpp)
alchemy_gui_add_framework_case(cryptotokenkit src/alchemy/darwin/cryptotokenkit/tests/cryptotokenkit_tests.cpp)
