# Accelerate (src/alchemy/darwin/accelerate/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/accelerate/vdsp.cpp
    src/alchemy/darwin/accelerate/bnns.cpp)
alchemy_gui_add_framework_case(accelerate src/alchemy/darwin/accelerate/tests/accelerate_tests.cpp)
