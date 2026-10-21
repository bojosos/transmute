# StoreKit (src/alchemy/darwin/storekit/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/storekit/install.cpp)
alchemy_gui_add_framework_case(storekit src/alchemy/darwin/storekit/tests/storekit_tests.cpp)
