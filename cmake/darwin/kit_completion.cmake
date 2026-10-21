# QLab 5 support (owner: mac/qlab, src/alchemy/darwin/kit/completion/): the interfaces
# QLab 5.6.3 and its bundled frameworks reach at launch that no other GUI
# provider implements (docs/ALCHEMY_QLAB_AUDIT.md). Installed last by
# install_completion_kit, after the support kit's gap fillers
# (include/alchemy/gui/install.hpp).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/kit/completion/install.cpp)
