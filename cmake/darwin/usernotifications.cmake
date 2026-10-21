# UserNotifications (src/alchemy/darwin/usernotifications/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/usernotifications/install.cpp)
alchemy_gui_add_framework_case(usernotifications src/alchemy/darwin/usernotifications/tests/usernotifications_tests.cpp)
