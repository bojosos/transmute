# AddressBook (src/alchemy/darwin/addressbook/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/addressbook/install.cpp)
alchemy_gui_add_framework_case(addressbook src/alchemy/darwin/addressbook/tests/addressbook_tests.cpp)
