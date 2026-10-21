# Vision (src/alchemy/darwin/vision/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/vision/install.cpp)
alchemy_gui_add_framework_case(vision src/alchemy/darwin/vision/tests/vision_tests.cpp)
