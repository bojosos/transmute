# libiconv (src/alchemy/darwin/libiconv/): see its README. See
# cmake/AlchemyDarwin.cmake for the fragment pattern. Headless (both builds).
target_sources(alchemy_core PRIVATE src/alchemy/darwin/libiconv/install.cpp)
alchemy_gui_add_framework_case(libiconv src/alchemy/darwin/libiconv/tests/libiconv_tests.cpp)
