# QLab gap group G6 (graphics): QuartzCore animations/layers, CoreImage, MetalKit, OpenGL/GLKit, CryptoKit, Swift overlays, WebKit placeholders, AppKit drawing additions.
# Owner: the G6 implementation agent (docs/ALCHEMY_QLAB_GAPS.md). Only this
# group edits this file and src/alchemy/darwin/kit/completion/graphics/.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/kit/completion/graphics/install.cpp)
function(alchemy_gui_completion_graphics_tests)
endfunction()
alchemy_gui_register_tests(alchemy_gui_completion_graphics_tests)
