# CoreText over DirectWrite (src/alchemy/darwin/coretext/): fonts, font
# descriptors, attributed strings, lines, frames and the typesetter. Owner: Q.
# Drawing goes through Quartz 2D (cmake/darwin/coregraphics.cmake, contract
# src/alchemy/darwin/coregraphics/quartz2d.hpp); dwrite.dll is loaded at run time. Its
# tests are alchemy_gui_coretext_unit (coretext/tests) and the text stage of
# the Quartz 2D fixture.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/coretext/install.cpp
    src/alchemy/darwin/coretext/fonts.cpp
    src/alchemy/darwin/coretext/font_exports.cpp
    src/alchemy/darwin/coretext/glyph_names.cpp
    src/alchemy/darwin/coretext/attributed.cpp
    src/alchemy/darwin/coretext/typesetter.cpp)

# Headless shell (src/alchemy/darwin/shells.hpp): alchemy.exe's answers for
# names this framework's GUI provider owns in alchemy-fx, where the _fx twin
# defines the same entry point empty.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/coretext/coretext_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/coretext/coretext_headless.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coretext/coretext_fx.cpp)

# NSAttributedString's headless shell (darwin/foundation/headless_shells.hpp):
# alchemy.exe only; in alchemy-fx attributed.cpp registers those methods and
# the _fx twin is empty. foundation_extra.cpp calls it.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/coretext/attributed_string_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/coretext/attributed_string_headless.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coretext/attributed_string_fx.cpp)

function(alchemy_gui_coretext_tests)
    # Offscreen text tests on the Quartz 2D fixture
    # (coregraphics/tests/quartz2d_fixture.hpp): font metrics and names,
    # glyphs and advances, lines and runs, drawing, framesetter wrapping,
    # truncation, glyph outlines and UI fonts.
    alchemy_gui_add_unit_test(alchemy_gui_coretext_unit src/alchemy/darwin/coretext/tests/coretext_tests.cpp TIMEOUT 180)
endfunction()
alchemy_gui_register_tests(alchemy_gui_coretext_tests)
# Glyph names, the font manager's registrations and errors, font collections
# and run base advances, on the framework-case fixture.
alchemy_gui_add_framework_case(coretext_fonts src/alchemy/darwin/coretext/tests/coretext_fonts_tests.cpp)

# Moved from darwin/identities/qt_imports.cpp: kCTFontLanguagesAttribute and
# CTFontManagerCreateFontDescriptorFromData (the support kit).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coretext/font_data.cpp)
