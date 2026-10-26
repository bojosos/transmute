# CoreGraphics (Quartz 2D drawing) and ImageIO over host::gpu (Direct2D and
# WIC on win32: src/alchemy/host/win32/gui/) (src/alchemy/darwin/coregraphics/,
# ImageIO in imageio/). Owner: r3. Contract:
# src/alchemy/darwin/coregraphics/quartz2d.hpp. cg_extra.cpp (CoreGraphics additions Qt binds) is
# installed by install_support_kit and graphics_gaps.cpp by fill::install_fwgaps.
# CoreText is cmake/darwin/coretext.cmake.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/coregraphics/install.cpp
    src/alchemy/darwin/coregraphics/provider.cpp
    src/alchemy/darwin/coregraphics/path.cpp
    src/alchemy/darwin/coregraphics/pixels.cpp
    src/alchemy/darwin/coregraphics/surface.cpp
    src/alchemy/darwin/coregraphics/color.cpp
    src/alchemy/darwin/coregraphics/icc_profiles.cpp
    src/alchemy/darwin/coregraphics/geometry_exports.cpp
    src/alchemy/darwin/coregraphics/path_exports.cpp
    src/alchemy/darwin/coregraphics/image.cpp
    src/alchemy/darwin/coregraphics/gradient.cpp
    src/alchemy/darwin/coregraphics/context.cpp
    src/alchemy/darwin/coregraphics/context_exports.cpp
    src/alchemy/darwin/coregraphics/appkit_drawing.cpp
    src/alchemy/darwin/coregraphics/displays.cpp
    src/alchemy/darwin/coregraphics/imageio/codec.cpp
    src/alchemy/darwin/coregraphics/imageio/imageio_exports.cpp
    src/alchemy/darwin/coregraphics/cg_extra.cpp
    src/alchemy/darwin/coregraphics/graphics_gaps.cpp)
if(WIN32)
    list(APPEND ALCHEMY_GUI_LINK_LIBRARIES ole32 oleaut32 windowscodecs)
endif()

# Headless shell (src/alchemy/darwin/shells.hpp): alchemy.exe's answers for
# names this framework's GUI provider owns in alchemy-fx, where the _fx twin
# defines the same entry point empty.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/coregraphics/coregraphics_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/coregraphics/coregraphics_headless.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coregraphics/coregraphics_fx.cpp)

function(alchemy_gui_quartz2d_tests)
    # Offscreen pixel tests: shapes, strokes, clipping, gradients, images,
    # blend modes, shadows, ImageIO round trips, AppKit drawing (CoreText's
    # text tests: cmake/darwin/coretext.cmake).
    alchemy_gui_add_unit_test(alchemy_gui_quartz2d_unit src/alchemy/darwin/coregraphics/tests/quartz2d_tests.cpp TIMEOUT 180)
    # Quartz Display Services over a fixed screen list and over the host's
    # screens (against host::ui's monitor list).
    alchemy_gui_add_unit_test(alchemy_gui_coregraphics_displays src/alchemy/darwin/coregraphics/tests/displays_tests.cpp)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # Guest fixture: CGBitmapContext + CoreText + ImageIO drawing, printing
    # pixel probes and a checksum. Its own link stubs give it the
    # CoreGraphics / CoreText / ImageIO identities it imports.
    set(source_dir ../../../src/alchemy/darwin/coregraphics/tests)
    alchemy_gui_object(quartz2d_link_cg ${source_dir}/quartz2d_link_stubs.c -DQUARTZ2D_STUB_COREGRAPHICS)
    alchemy_gui_object(quartz2d_link_ct ${source_dir}/quartz2d_link_stubs.c -DQUARTZ2D_STUB_CORETEXT)
    alchemy_gui_object(quartz2d_link_io ${source_dir}/quartz2d_link_stubs.c -DQUARTZ2D_STUB_IMAGEIO)
    alchemy_gui_object(quartz2d_link_cf ${source_dir}/quartz2d_link_stubs.c -DQUARTZ2D_STUB_COREFOUNDATION)
    set(stubs)
    foreach(entry IN ITEMS
            "quartz2d_link_cg|/System/Library/Frameworks/CoreGraphics.framework/Versions/A/CoreGraphics"
            "quartz2d_link_ct|/System/Library/Frameworks/CoreText.framework/Versions/A/CoreText"
            "quartz2d_link_io|/System/Library/Frameworks/ImageIO.framework/Versions/A/ImageIO"
            "quartz2d_link_cf|/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 stub)
        list(GET parts 1 install_name)
        add_custom_command(OUTPUT "${macos_dir}/${stub}.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -install_name "${install_name}"
                "${macos_dir}/${stub}.o" -o "${macos_dir}/${stub}.dylib"
            DEPENDS "${macos_dir}/${stub}.o" VERBATIM)
        list(APPEND stubs "${macos_dir}/${stub}.dylib")
    endforeach()
    alchemy_gui_object(quartz2d_fixture ${source_dir}/quartz2d_fixture.c)
    add_custom_command(OUTPUT "${macos_dir}/quartz2d_fixture"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/quartz2d_fixture.o"
            ${stubs} "${macos_dir}/link_core_foundation.dylib" "${macos_dir}/link_system.dylib"
            -o "${macos_dir}/quartz2d_fixture"
        DEPENDS "${macos_dir}/quartz2d_fixture.o" ${stubs} VERBATIM)
    add_custom_target(alchemy_gui_quartz2d_fixtures ALL DEPENDS "${macos_dir}/quartz2d_fixture")
    add_dependencies(alchemy_gui_quartz2d_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_gui_quartz2d_fixture "${macos_dir}/quartz2d_fixture"
        PROGRESS "quartz2d_fixture: start"
        COMPLETE "quartz2d_fixture: shapes ok.*quartz2d_fixture: text ok.*quartz2d_fixture: imageio ok.*quartz2d_fixture: checksum=[0-9a-f]+.*quartz2d_fixture: done")
endfunction()
alchemy_gui_register_tests(alchemy_gui_quartz2d_tests)

# Moved from darwin/identities/qt_imports.cpp: CGColorSpaceCopyPropertyList
# (the support kit).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/coregraphics/color_space_property_list.cpp)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/coregraphics/event_source.cpp)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/coregraphics/no_displays.cpp)

# CGEvent creation, fields and event sources, on the framework-case fixture
# (graphics_gaps.cpp).
alchemy_gui_add_framework_case(coregraphics_events src/alchemy/darwin/coregraphics/tests/cg_events_tests.cpp)
