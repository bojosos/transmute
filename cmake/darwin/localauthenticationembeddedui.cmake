# LocalAuthenticationEmbeddedUI (src/alchemy/darwin/localauthenticationembeddedui/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/localauthenticationembeddedui/install.cpp)
alchemy_gui_add_framework_case(localauthenticationembeddedui src/alchemy/darwin/localauthenticationembeddedui/tests/localauthenticationembeddedui_tests.cpp)
