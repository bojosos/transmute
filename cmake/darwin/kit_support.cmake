# Qt 6 Cocoa QPA support (owner: qt-vfb, src/alchemy/darwin/kit/support/): IOSurface,
# CoreVideo CVDisplayLink, CALayer contents compositing for layer-backed
# views (include/alchemy/gui/layer_contents.hpp), AGL/OpenGL identities.
# Installed by install_support_kit after Quartz 2D and before the export-fill gap
# fillers (include/alchemy/gui/install.hpp).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/kit/support/install.cpp)
