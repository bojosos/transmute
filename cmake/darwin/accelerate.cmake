# Accelerate (src/alchemy/darwin/accelerate/): see its README.
# See cmake/AlchemyDarwin.cmake for the fragment pattern.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/accelerate/vdsp.cpp
    src/alchemy/darwin/accelerate/vdsp_vectors.cpp
    src/alchemy/darwin/accelerate/vforce.cpp
    src/alchemy/darwin/accelerate/vimage.cpp
    src/alchemy/darwin/accelerate/blas.cpp
    src/alchemy/darwin/accelerate/vdsp_more.cpp
    src/alchemy/darwin/accelerate/vimage_cg.cpp
    src/alchemy/darwin/accelerate/bnns.cpp)
alchemy_gui_add_framework_case(accelerate src/alchemy/darwin/accelerate/tests/accelerate_tests.cpp)

# LAPACK decomposition drivers (both builds; installed by install_darwin_numeric).
target_sources(alchemy_core PRIVATE src/alchemy/darwin/accelerate/lapack.cpp)
