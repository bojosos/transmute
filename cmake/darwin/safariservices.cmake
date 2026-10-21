# SafariServices (src/alchemy/darwin/safariservices/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/safariservices/install.cpp)
alchemy_gui_add_framework_case(safariservices src/alchemy/darwin/safariservices/tests/safariservices_tests.cpp)
