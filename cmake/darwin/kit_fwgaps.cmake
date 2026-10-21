# Gaps in frameworks Alchemy already provides, for strict mode
# (src/alchemy/darwin/kit/fwgaps/): CoreFoundation (CFSet, CFCharacterSet,
# CFNotificationCenter, ...), Foundation, AppKit, CoreGraphics, QuartzCore,
# CoreText, CGL, CoreServices, Security, SystemConfiguration, Carbon, vDSP,
# libobjc, plus OpenDirectory and SecurityInterface. Installed by
# gui::fill::install_fwgaps (fill-only).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/kit/fwgaps/install.cpp)

function(alchemy_gui_fwgaps_tests)
    alchemy_gui_add_unit_test(alchemy_gui_fwgaps src/alchemy/darwin/kit/fwgaps/tests/fwgaps_tests.cpp TIMEOUT 180)
endfunction()
alchemy_gui_register_tests(alchemy_gui_fwgaps_tests)
