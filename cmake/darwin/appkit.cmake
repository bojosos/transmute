# AppKit providers (NSApplication, NSWindow, NSView, NSEvent, ...) and the
# Carbon/CoreGraphics input and display functions. Owner: A4
# (src/alchemy/darwin/appkit/). Built only against include/alchemy/gui/*.hpp.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/appkit/install.cpp
    src/alchemy/darwin/appkit/appkit.cpp
    src/alchemy/darwin/appkit/application.cpp
    src/alchemy/darwin/appkit/window.cpp
    src/alchemy/darwin/appkit/frame_view.cpp
    src/alchemy/darwin/appkit/window_tabs.cpp
    src/alchemy/darwin/appkit/view.cpp
    src/alchemy/darwin/appkit/event.cpp
    src/alchemy/darwin/appkit/resources.cpp
    src/alchemy/darwin/appkit/alert.cpp
    src/alchemy/darwin/appkit/autopress.cpp
    src/alchemy/darwin/appkit/modal.cpp
    src/alchemy/darwin/appkit/carbon.cpp
    src/alchemy/darwin/appkit/image_codec.cpp
    src/alchemy/darwin/appkit/drawing.cpp
    src/alchemy/darwin/appkit/graphics.cpp
    src/alchemy/darwin/appkit/controls.cpp
    src/alchemy/darwin/appkit/controls_more.cpp
    src/alchemy/darwin/appkit/panels.cpp
    src/alchemy/darwin/appkit/hitoolbox.cpp
    src/alchemy/darwin/appkit/controllers.cpp
    src/alchemy/darwin/appkit/text.cpp
    src/alchemy/darwin/appkit/containers.cpp
    src/alchemy/darwin/appkit/nib_archive.cpp
    src/alchemy/darwin/appkit/nib_keyed.cpp
    src/alchemy/darwin/appkit/nib.cpp
    src/alchemy/darwin/appkit/asset_catalog.cpp
    src/alchemy/darwin/appkit/rtf.cpp
    src/alchemy/darwin/appkit/documents.cpp)
# QLab's controls (text system, tables, outlines, collections, accessibility)
# and windows (panels, popovers, documents, workspace, drag and drop, Auto
# Layout, stack and grid views), installed by install_completion_kit through
# src/alchemy/darwin/kit/completion/{controls,windows}/install.cpp; its AppKit drawing
# additions, WebKit shells and NSAnimatablePropertyContainer.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/appkit/controls/layout.cpp
    src/alchemy/darwin/appkit/controls/date_elements.cpp
    src/alchemy/darwin/appkit/controls/segmented.cpp
    src/alchemy/darwin/appkit/controls/path_control.cpp
    src/alchemy/darwin/appkit/controls/search_field.cpp
    src/alchemy/darwin/appkit/controls/date_picker.cpp
    src/alchemy/darwin/appkit/controls/color_well.cpp
    src/alchemy/darwin/appkit/controls/text_system.cpp
    src/alchemy/darwin/appkit/controls/tables.cpp
    src/alchemy/darwin/appkit/controls/collection.cpp
    src/alchemy/darwin/appkit/controls/ruler.cpp
    src/alchemy/darwin/appkit/controls/accessibility.cpp
    src/alchemy/darwin/appkit/controls/small.cpp
    src/alchemy/darwin/appkit/controls/appearance.cpp
    src/alchemy/darwin/appkit/windows/layout_solver.cpp
    src/alchemy/darwin/appkit/windows/layout_geometry.cpp
    src/alchemy/darwin/appkit/windows/auto_layout.cpp
    src/alchemy/darwin/appkit/windows/control_metrics.cpp
    src/alchemy/darwin/appkit/windows/stack_grid.cpp
    src/alchemy/darwin/appkit/windows/window_extras.cpp
    src/alchemy/darwin/appkit/windows/workspace_dragging.cpp
    src/alchemy/darwin/appkit/windows/popover.cpp
    src/alchemy/darwin/appkit/windows/drag_session.cpp
    src/alchemy/darwin/appkit/webkit.cpp
    src/alchemy/darwin/appkit/appkit_drawing.cpp
    src/alchemy/darwin/appkit/animator.cpp)
# The AppKit pieces Qt's Cocoa platform plugin binds (installed by
# install_support_kit): AppKit additions, accessibility names, the Carbon
# Pasteboard Manager, and the ALCHEMY_SELECTOR_AUDIT diagnostic.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/appkit/accessibility.cpp
    src/alchemy/darwin/appkit/selector_audit.cpp
    src/alchemy/darwin/appkit/appkit_extra.cpp
    src/alchemy/darwin/appkit/pasteboard.cpp)
# The AppKit API Chrome's start-up reaches (installed by install_values_kit
# through src/alchemy/gui/value_provider.cpp): constants and
# class shells, menus, window additions, NSSpellChecker over the Windows Spell
# Checking API, and selectors on other providers' classes.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/appkit/appkit_gaps.cpp
    src/alchemy/darwin/appkit/selectors.cpp
    src/alchemy/darwin/appkit/menus.cpp
    src/alchemy/darwin/appkit/window_additions.cpp
    src/alchemy/darwin/appkit/spelling.cpp)
if(WIN32)
    # NSAlert (TaskDialogIndirect, loaded from comctl32 v6 at run time),
    # NSColorSampler (GDI), NSWorkspace and
    # NSDocumentController (shell), image files (Windows Imaging Component).
    list(APPEND ALCHEMY_GUI_LINK_LIBRARIES user32 gdi32 shell32 shlwapi ole32 windowscodecs)
endif()

# Headless shell (src/alchemy/darwin/shells.hpp): alchemy.exe's answers for
# names this framework's GUI provider owns in alchemy-fx, where the _fx twin
# defines the same entry point empty.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/appkit/appkit_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/appkit/appkit_headless.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/appkit/appkit_fx.cpp)

# Cocoa bindings (NSObjectController, NSArrayController, ...) over
# Foundation's KVO, and AppKit's NSRunningApplication, NSAnimationContext,
# NSSpellChecker shell, speech and NSWorkspace constants
# (install_appkit_gaps, called by the fwgaps aggregator).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/appkit/bindings.cpp
    src/alchemy/darwin/appkit/application_services.cpp)

function(alchemy_gui_appkit_tests)
    alchemy_gui_add_unit_test(alchemy_gui_appkit_smoke src/alchemy/darwin/appkit/tests/appkit_smoke_tests.cpp)
    # NSApplication / NSWindow / NSView / NSEvent / resources / Carbon over a
    # fake window server (src/alchemy/darwin/appkit/tests/fake_window_server.hpp).
    alchemy_gui_add_unit_test(alchemy_gui_appkit_unit src/alchemy/darwin/appkit/tests/appkit_tests.cpp)
    # Chrome stage 2 input paths (w2): event monitors, NSTrackingArea
    # subclasses and options, enter/exit events, the CGEvent bridge, NSCursor
    # subclasses, over the fake window server.
    alchemy_gui_add_unit_test(alchemy_gui_appkit_events src/alchemy/darwin/appkit/tests/event_input_tests.cpp)
    # Software view drawing, standard controls and nib loading over the
    # fake window server (headless; parses the user's QLab nibs when present).
    alchemy_gui_add_unit_test(alchemy_gui_appkit_drawing src/alchemy/darwin/appkit/tests/appkit_drawing_tests.cpp)
    # QLab's controls: the host-only geometry and formatting cores (no guest),
    # and the controls over AppKit and the fake window server (drawing, mouse
    # tracking, nib decoding from project-authored NIBArchives).
    alchemy_gui_add_unit_test(alchemy_gui_appkit_controls_unit src/alchemy/darwin/appkit/tests/controls_core_tests.cpp)
    alchemy_gui_add_unit_test(alchemy_gui_appkit_controls_behaviour src/alchemy/darwin/appkit/tests/controls_behaviour_tests.cpp)
    # QLab's windows: host-only cores (the Auto Layout constraint solver, stack /
    # grid view and popover geometry), and the providers over AppKit and the
    # fake window server (headless).
    alchemy_gui_add_unit_test(alchemy_gui_appkit_windows_unit
        src/alchemy/darwin/appkit/tests/windows_tests.cpp
        src/alchemy/darwin/appkit/tests/layout_solver_tests.cpp
        src/alchemy/darwin/appkit/tests/layout_geometry_tests.cpp)
    alchemy_gui_add_unit_test(alchemy_gui_appkit_windows
        src/alchemy/darwin/appkit/tests/windows_appkit_tests.cpp)
    # AppKit string drawing with arm64 NSTextAlignment values (pixels, fake window server).
    alchemy_gui_add_unit_test(alchemy_gui_appkit_text_alignment src/alchemy/darwin/appkit/tests/text_alignment_tests.cpp)
    # The public API added for the selectors real applications send, over
    # every provider (run_program's order) and the fake window server.
    alchemy_gui_add_unit_test(alchemy_gui_appkit_api src/alchemy/darwin/appkit/tests/appkit_api_tests.cpp TIMEOUT 180)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # Guest ARM64 subclasses of the AppKit classes under alchemy-fx (no window
    # server needed). The source lives with the AppKit tests.
    alchemy_gui_object(gui_appkit_fixture ../../../src/alchemy/darwin/appkit/tests/appkit_fixture.m)
    alchemy_gui_program(gui_appkit_fixture gui_appkit_fixture)
    add_custom_target(alchemy_gui_appkit_fixtures ALL DEPENDS "${macos_dir}/gui_appkit_fixture")
    add_dependencies(alchemy_gui_appkit_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_gui_appkit_fixture "${macos_dir}/gui_appkit_fixture"
        PROGRESS "Alchemy AppKit fixture: launched" COMPLETE "Alchemy AppKit fixture: complete")
    # Guest subclasses of the standard controls (as wxOSX makes them) and
    # HIShape regions, with their own AppKit / Carbon link identities.
    set(source_dir ../../../src/alchemy/darwin/appkit/tests)
    alchemy_gui_object(controls_link_appkit ${source_dir}/controls_link_stubs.m -DCONTROLS_STUB_APPKIT)
    alchemy_gui_object(controls_link_carbon ${source_dir}/controls_link_stubs.m -DCONTROLS_STUB_CARBON)
    set(stubs)
    foreach(entry IN ITEMS
            "controls_link_appkit|/System/Library/Frameworks/AppKit.framework/Versions/C/AppKit|objc"
            "controls_link_carbon|/System/Library/Frameworks/Carbon.framework/Versions/A/Carbon|")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 stub)
        list(GET parts 1 install_name)
        list(GET parts 2 needs)
        set(libraries)
        if(needs STREQUAL "objc")
            set(libraries "${macos_dir}/link_objc.dylib" "${macos_dir}/link_foundation.dylib")
        endif()
        add_custom_command(OUTPUT "${macos_dir}/${stub}.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -install_name "${install_name}"
                "${macos_dir}/${stub}.o" ${libraries} -o "${macos_dir}/${stub}.dylib"
            DEPENDS "${macos_dir}/${stub}.o" ${libraries} VERBATIM)
        list(APPEND stubs "${macos_dir}/${stub}.dylib")
    endforeach()
    alchemy_gui_object(controls_fixture ${source_dir}/controls_fixture.m)
    add_custom_command(OUTPUT "${macos_dir}/controls_fixture"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/controls_fixture.o"
            ${stubs} "${macos_dir}/link_foundation.dylib" "${macos_dir}/link_core_foundation.dylib"
            "${macos_dir}/link_objc.dylib" "${macos_dir}/link_system.dylib" -o "${macos_dir}/controls_fixture"
        DEPENDS "${macos_dir}/controls_fixture.o" ${stubs} VERBATIM)
    add_custom_target(alchemy_gui_controls_fixtures ALL DEPENDS "${macos_dir}/controls_fixture")
    add_dependencies(alchemy_gui_controls_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_gui_controls_fixture "${macos_dir}/controls_fixture"
        PROGRESS "controls_fixture: start"
        COMPLETE "controls_fixture: sliders ok.*controls_fixture: combo ok.*controls_fixture: hishape ok.*controls_fixture: done")
endfunction()
alchemy_gui_register_tests(alchemy_gui_appkit_tests)

# Chrome stage 2 w4: NSWorkspace, NSRunningApplication and the accessibility
# surface over every provider (no window server needed).
function(alchemy_gui_appkit_workspace_tests)
    alchemy_gui_add_unit_test(alchemy_gui_appkit_workspace src/alchemy/darwin/appkit/tests/workspace_tests.cpp)
endfunction()
alchemy_gui_register_tests(alchemy_gui_appkit_workspace_tests)

# Moved from darwin/identities/qt_imports.cpp: AppKit constants,
# NSBitsPerPixelFromDepth and class shells (installed by the support kit).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/appkit/constants_and_shells.cpp)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/appkit/color_alpha.cpp
    src/alchemy/darwin/appkit/carbon_events.cpp
    src/alchemy/darwin/appkit/class_shells.cpp)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/appkit/base_classes.cpp
    src/alchemy/darwin/appkit/input_sources.cpp)
