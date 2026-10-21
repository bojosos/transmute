# OpenGL and AGL (src/alchemy/darwin/opengl/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/opengl/no_context.cpp)
alchemy_gui_add_framework_case(opengl src/alchemy/darwin/opengl/tests/opengl_tests.cpp)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/opengl/cgl.cpp)
