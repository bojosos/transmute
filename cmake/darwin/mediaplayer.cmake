# MediaPlayer (src/alchemy/darwin/mediaplayer/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/mediaplayer/install.cpp)
alchemy_gui_add_framework_case(mediaplayer src/alchemy/darwin/mediaplayer/tests/mediaplayer_tests.cpp)
