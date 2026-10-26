# QuartzCore (CALayer, CAMetalLayer, presentation) and the GUI fixture apps.
# Owner: A7 (src/alchemy/darwin/quartzcore/, tests/fixtures/macos/gui_*,
# tests/alchemy_gui_*). Contracts: src/alchemy/darwin/quartzcore/quartzcore.hpp
# (AppKit layer hosting) and quartzcore_vulkan.hpp (Metal device bridge).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/quartzcore/install.cpp src/alchemy/darwin/quartzcore/quartzcore.cpp src/alchemy/darwin/quartzcore/presenter.cpp
    src/alchemy/darwin/quartzcore/layer_tree.cpp)
# QLab's QuartzCore, CoreImage and GLKit providers (installed by
# install_completion_kit through src/alchemy/darwin/kit/completion/graphics/install.cpp and
# src/alchemy/darwin/kit/completion/install.cpp): CAAnimation, CATransform3D,
# CAMediaTimingFunction, the CoreImage engine, GLKMath, and the shared value
# math and property bags (graphics_math, graphics_support).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/quartzcore/glkit.cpp
    src/alchemy/darwin/quartzcore/graphics_math.cpp
    src/alchemy/darwin/quartzcore/graphics_support.cpp
    src/alchemy/darwin/quartzcore/animation.cpp
    src/alchemy/darwin/quartzcore/transform3d.cpp
    src/alchemy/darwin/quartzcore/ci_engine.cpp
    src/alchemy/darwin/quartzcore/core_image.cpp
    src/alchemy/darwin/quartzcore/media_timing.cpp)
# Qt's IOSurface, CoreVideo CVDisplayLink and CALayer contents compositing
# for layer-backed views (include/alchemy/gui/layer_contents.hpp), installed
# by install_support_kit.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/quartzcore/iosurface.cpp
    src/alchemy/darwin/quartzcore/display_link.cpp
    src/alchemy/darwin/quartzcore/metal_display_link.cpp
    src/alchemy/darwin/quartzcore/layer_contents.cpp)
if(ALCHEMY_VULKAN_INCLUDE)
    # Same definitions as cmake/darwin/metal.cmake; repeated so presentation
    # does not depend on another fragment.
    list(APPEND ALCHEMY_GUI_INCLUDE_DIRECTORIES "${ALCHEMY_VULKAN_INCLUDE}")
    list(APPEND ALCHEMY_GUI_COMPILE_DEFINITIONS ALCHEMY_HAVE_VULKAN_HEADERS=1)
endif()

function(alchemy_gui_quartzcore_tests)
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_smoke
        src/alchemy/darwin/quartzcore/tests/quartzcore_smoke_tests.cpp)
    # CALayer tree composition on host pixels (layer_tree.hpp, no guest).
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_layer_tree
        src/alchemy/darwin/quartzcore/tests/layer_tree_tests.cpp)
    # Layer kinds and geometry over the Objective-C runtime: appearance, backing
    # stores, shape/text/gradient pixels, replicators, hit testing, scrolling and
    # constraints (the frameworks wave, U2).
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_layer_kinds
        src/alchemy/darwin/quartzcore/tests/layer_kinds_tests.cpp TIMEOUT 120)
    # Layer trees with IOSurface contents composited into AppKit windows on
    # commit, IOSurface seeds and Mach ports, CVDisplayLink cadence (every
    # provider, the in-memory window server).
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_compositing
        src/alchemy/darwin/quartzcore/tests/compositing_tests.cpp TIMEOUT 120)
    # Presentation against a real Vulkan device (test double for Metal's
    # bridge) and a real Win32 window. Exit code 77: no Vulkan driver or no
    # interactive desktop (skipped, not failed).
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_presenter
        src/alchemy/darwin/quartzcore/tests/quartzcore_presenter_tests.cpp
        src/alchemy/darwin/quartzcore/tests/vulkan_test_device.cpp TIMEOUT 120)
    set_tests_properties(alchemy_gui_quartzcore_presenter PROPERTIES SKIP_RETURN_CODE 77)
    # CALayer/CAMetalLayer/CAMetalDrawable/CATransaction through the
    # Objective-C runtime (native IMPs called with objc.send).
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_objc
        src/alchemy/darwin/quartzcore/tests/quartzcore_objc_tests.cpp
        src/alchemy/darwin/quartzcore/tests/vulkan_test_device.cpp TIMEOUT 120)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # Fixture apps (tests/fixtures/macos/gui_*). Every fixture prints
    # "<name>: start" first (PROGRESS) and "<name>: done" last; since the
    # wave-1 integration (AppKit, run loop, window server, Metal, MSL merged)
    # all of them must reach COMPLETE. The window and triangle fixtures need
    # an interactive desktop and a Vulkan 1.3 GPU.
    # Fixture sources share gui_fixture.h (rebuild them when it changes).
    list(APPEND ALCHEMY_GUI_RUNTIME_HEADERS "${PROJECT_SOURCE_DIR}/tests/fixtures/macos/gui_fixture.h")
    # stdio/string imports beyond the base libSystem link stub.
    alchemy_gui_object(gui_link_libc gui_link_libc.c)
    add_custom_command(OUTPUT "${macos_dir}/gui_link_libc.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /usr/lib/system/libsystem_c.dylib "${macos_dir}/gui_link_libc.o"
            -o "${macos_dir}/gui_link_libc.dylib"
        DEPENDS "${macos_dir}/gui_link_libc.o" VERBATIM)
    list(APPEND ALCHEMY_GUI_LINK_STUBS "${macos_dir}/gui_link_libc.dylib")
    set(fixtures gui_link_smoke gui_window_events gui_metal_triangle gui_metal_compute gui_text_input)
    set(outputs)
    foreach(fixture IN LISTS fixtures)
        alchemy_gui_object(${fixture} ${fixture}.m)
        alchemy_gui_program(${fixture} ${fixture})
        list(APPEND outputs "${macos_dir}/${fixture}")
    endforeach()
    add_custom_target(alchemy_gui_quartzcore_fixtures ALL DEPENDS ${outputs})
    add_dependencies(alchemy_gui_quartzcore_fixtures alchemy_gui_link_stubs)
    # Links every GUI framework stub and creates the shared NSApplication.
    alchemy_gui_add_fixture_test(alchemy_gui_link_smoke "${macos_dir}/gui_link_smoke"
        PROGRESS "Alchemy GUI link: displays [0-9]+, device (present|none)"
        COMPLETE "Alchemy GUI link: application")
    # (a) window + event echo. Without a harness driving input it exits after
    # its scripted self-test (posted events) and a resize; the harness
    # (scripts/alchemy_gui_harness.ps1) injects real input with --interactive.
    alchemy_gui_add_fixture_test(alchemy_gui_window_events "${macos_dir}/gui_window_events"
        PROGRESS "gui_window_events: start"
        COMPLETE "resize 500x340.*mouse down button=0 clicks=1 at 40,30.*key down keyCode=0 chars=a.*key down keyCode=12 chars=q.*gui_window_events: done")
    # (b)-(c) need a Metal device: Metal on Vulkan, built only with the Vulkan headers.
    if(ALCHEMY_VULKAN_INCLUDE)
        # (b) CAMetalLayer triangle: renders frames to the window, then reads the
        # centre and a corner pixel back through a blit into a shared buffer.
        alchemy_gui_add_fixture_test(alchemy_gui_metal_triangle "${macos_dir}/gui_metal_triangle"
            PROGRESS "gui_metal_triangle: layer pixelFormat=80 framebufferOnly=0 opaque=1 maximumDrawableCount=3"
            COMPLETE "gui_metal_triangle: centre=255,0,0,255 corner=0,0,255,255.*gui_metal_triangle: done")
        # (b') the same after a window resize to 400x300 points and a simulated
        # backing-scale change to 2 (drawableSize follows bounds x scale, as in
        # Blender): new swapchain, new drawable size, same picture.
        alchemy_gui_add_fixture_test(alchemy_gui_metal_triangle_resize "${macos_dir}/gui_metal_triangle"
            PROGRESS "gui_metal_triangle: ready drawable="
            COMPLETE "resized view=400x300.*drawable=800x600 scale=2\\.0.*readback 800x600.*centre=255,0,0,255 corner=0,0,255,255.*gui_metal_triangle: done"
            ARGS --frames 8 --resize --scale 2)
        # (b'') what the window really shows: the harness captures the client
        # area (PrintWindow) at 256x256 points, after a real resize and after a
        # simulated DPI change, and compares each with a blessed reference
        # (tests/fixtures/macos/reference/gui_metal_triangle_<w>x<h>.png,
        # tools/fx_imgdiff.py). Exit 77 (skipped) when no reference exists for
        # this display's client size (another DPI): bless with
        # scripts/alchemy_gui_harness.ps1 -ActionFile ... without -NoBless.
        if(ALCHEMY_POWERSHELL)
            add_test(NAME alchemy_gui_metal_triangle_capture COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile
                -ExecutionPolicy Bypass -File "${PROJECT_SOURCE_DIR}/scripts/alchemy_gui_harness.ps1"
                -Exe "$<TARGET_FILE:alchemy-fx>" -Program "${macos_dir}/gui_metal_triangle"
                -ProgramArgs "--interactive;--timeout;90"
                -ActionFile "${PROJECT_SOURCE_DIR}/tests/fixtures/macos/gui_metal_triangle.actions"
                -Expect "simulated scale 2;drawable=[0-9]+x[0-9]+ scale=2\\.0;gui_metal_triangle: done"
                -OutDir "${CMAKE_BINARY_DIR}/gui-harness" -NoBless -TimeoutSeconds 100)
            set_tests_properties(alchemy_gui_metal_triangle_capture PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
                RUN_SERIAL ON)
        endif()
        # (c) headless compute kernel + blit readback (no window).
        alchemy_gui_add_fixture_test(alchemy_gui_metal_compute "${macos_dir}/gui_metal_compute"
            PROGRESS "gui_metal_compute: start"
            COMPLETE "gui_metal_compute: checksum=[0-9]+ ok.*gui_metal_compute: done")
    endif()
    # (d) NSTextInputClient: committed and marked text through
    # interpretKeyEvents: (posted key events; IME composition with the harness).
    alchemy_gui_add_fixture_test(alchemy_gui_text_input "${macos_dir}/gui_text_input"
        PROGRESS "gui_text_input: start"
        COMPLETE "insertText \"h\".*insertText \"i\" buffer \"hi\".*command insertNewline:.*commit buffer \"hi\".*gui_text_input: done")
endfunction()
alchemy_gui_register_tests(alchemy_gui_quartzcore_tests)

function(alchemy_gui_quartzcore_qlab_tests)
    # Host-only cores of QLab's graphics providers (no guest).
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_graphics_math src/alchemy/darwin/quartzcore/tests/graphics_math_tests.cpp)
    alchemy_gui_add_unit_test(alchemy_gui_quartzcore_core_image src/alchemy/darwin/quartzcore/tests/ci_engine_tests.cpp)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # Guest fixture: AppKit drawing additions checked by pixels, HSB,
    # CATransform3D, GLKit, CAAnimation timing, WebKit objects, MTKTextureLoader.
    # It links stdio through the QuartzCore fixtures' libsystem_c stub.
    alchemy_gui_object(graphics_fixture ../../../src/alchemy/darwin/quartzcore/tests/graphics_fixture.m)
    set(ALCHEMY_GUI_LINK_STUBS ${ALCHEMY_GUI_LINK_STUBS} "${macos_dir}/gui_link_libc.dylib")
    alchemy_gui_program(graphics_fixture graphics_fixture)
    add_custom_target(alchemy_gui_quartzcore_graphics_fixtures ALL DEPENDS "${macos_dir}/graphics_fixture")
    add_dependencies(alchemy_gui_quartzcore_graphics_fixtures alchemy_gui_link_stubs alchemy_gui_quartzcore_fixtures)
    alchemy_gui_add_fixture_test(alchemy_gui_quartzcore_graphics "${macos_dir}/graphics_fixture"
        PROGRESS "qlab_graphics: start"
        COMPLETE "gradient ok.*shadow ok.*affine ok.*pattern ok.*images ok.*colors ok.*transforms ok.*animation ok.*webkit ok.*core image ok.*metalkit (ok|skipped).*qlab_graphics: done")
endfunction()
alchemy_gui_register_tests(alchemy_gui_quartzcore_qlab_tests)

function(alchemy_gui_quartzcore_qt_tests)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # Guest fixture of Qt's Cocoa backing-store pattern: a layer-backed view
    # drawing through -updateLayer into IOSurface layer contents, driven by a
    # CVDisplayLink marshalled through a DATA_ADD dispatch source.
    set(source_dir ../../../src/alchemy/darwin/quartzcore/tests)
    set(stubs)
    foreach(entry IN ITEMS
            "qt_link_iosurface|QT_STUB_IOSURFACE|/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface"
            "qt_link_corevideo|QT_STUB_COREVIDEO|/System/Library/Frameworks/CoreVideo.framework/Versions/A/CoreVideo"
            "qt_link_dispatch|QT_STUB_DISPATCH|/usr/lib/system/libdispatch.dylib")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 stub)
        list(GET parts 1 define)
        list(GET parts 2 install_name)
        alchemy_gui_object(${stub} ${source_dir}/layer_link_stubs.c -D${define})
        add_custom_command(OUTPUT "${macos_dir}/${stub}.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -install_name "${install_name}"
                "${macos_dir}/${stub}.o" -o "${macos_dir}/${stub}.dylib"
            DEPENDS "${macos_dir}/${stub}.o" VERBATIM)
        list(APPEND stubs "${macos_dir}/${stub}.dylib")
    endforeach()
    alchemy_gui_object(layer_backingstore_fixture ${source_dir}/layer_backingstore_fixture.m)
    add_custom_command(OUTPUT "${macos_dir}/layer_backingstore_fixture"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -rpath @loader_path
            "${macos_dir}/layer_backingstore_fixture.o" ${stubs} ${ALCHEMY_GUI_LINK_STUBS} "${macos_dir}/gui_link_libc.dylib"
            -o "${macos_dir}/layer_backingstore_fixture"
        DEPENDS "${macos_dir}/layer_backingstore_fixture.o" ${stubs} ${ALCHEMY_GUI_LINK_STUBS} "${macos_dir}/gui_link_libc.dylib" VERBATIM)
    add_custom_target(alchemy_gui_quartzcore_layer_fixtures ALL DEPENDS "${macos_dir}/layer_backingstore_fixture")
    # gui_link_libc.dylib (libsystem_c stdio) comes from the QuartzCore fixtures.
    add_dependencies(alchemy_gui_quartzcore_layer_fixtures alchemy_gui_link_stubs alchemy_gui_quartzcore_fixtures)
    alchemy_gui_add_fixture_test(alchemy_gui_quartzcore_backingstore "${macos_dir}/layer_backingstore_fixture"
        PROGRESS "layer_backingstore_fixture: start"
        COMPLETE "surface [0-9]+x[0-9]+ bpr=[0-9]+ alloc=[0-9]+.*updateLayer [1-9].*display link [1-9][0-9]* Hz.*running after stop 0.*frames [3-9][0-9] ticks [0-9]+ requests [1-9].*left=255,0,0,255 right=0,0,255,255.*layer_backingstore_fixture: done")
    # Never shown on the desktop (DWM-cloaked) and never activated.
    set_tests_properties(alchemy_gui_quartzcore_backingstore PROPERTIES ENVIRONMENT "ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_ACTIVATE=1")
endfunction()
alchemy_gui_register_tests(alchemy_gui_quartzcore_qt_tests)

function(alchemy_gui_quartzcore_cross_process_tests)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # An IOSurface between two Alchemy processes through its Mach port
    # (Chrome's GPU process -> browser): shared pages, one IOSurfaceID,
    # per-process use counts, release on both sides.
    set(source_dir ../../../src/alchemy/darwin/quartzcore/tests)
    set(stubs)
    foreach(entry IN ITEMS
            "iosurface_cross_system|CROSS_STUB_SYSTEM|/usr/lib/libSystem.B.dylib"
            "iosurface_cross_iosurface|CROSS_STUB_IOSURFACE|/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 stub)
        list(GET parts 1 define)
        list(GET parts 2 install_name)
        alchemy_gui_object(${stub} ${source_dir}/iosurface_cross_stubs.c -D${define})
        add_custom_command(OUTPUT "${macos_dir}/${stub}.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -install_name "${install_name}"
                "${macos_dir}/${stub}.o" -o "${macos_dir}/${stub}.dylib"
            DEPENDS "${macos_dir}/${stub}.o" VERBATIM)
        list(APPEND stubs "${macos_dir}/${stub}.dylib")
    endforeach()
    alchemy_gui_object(iosurface_cross_fixture ${source_dir}/iosurface_cross_fixture.c)
    add_custom_command(OUTPUT "${macos_dir}/iosurface_cross_fixture"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/iosurface_cross_fixture.o" ${stubs} "${macos_dir}/link_core_foundation.dylib"
            -o "${macos_dir}/iosurface_cross_fixture"
        DEPENDS "${macos_dir}/iosurface_cross_fixture.o" ${stubs} "${macos_dir}/link_core_foundation.dylib" VERBATIM)
    add_custom_target(alchemy_gui_iosurface_cross_fixtures ALL DEPENDS "${macos_dir}/iosurface_cross_fixture")
    add_dependencies(alchemy_gui_iosurface_cross_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_gui_quartzcore_iosurface_cross "${macos_dir}/iosurface_cross_fixture"
        PROGRESS "iosurface cross: ok" COMPLETE "iosurface cross: ok")
    set_tests_properties(alchemy_gui_quartzcore_iosurface_cross PROPERTIES ENVIRONMENT "ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_ACTIVATE=1")
endfunction()
alchemy_gui_register_tests(alchemy_gui_quartzcore_cross_process_tests)

# g3 (Chrome stage 4): IOSurface-backed MTLTextures, in one process and
# between a browser and a GPU process (metal_iosurface_fixture.c). The
# fixture's `bench <frames>` mode measures the coherence copies at 1184x1069
# (not a test).
function(alchemy_gui_quartzcore_metal_iosurface_tests)
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED OR NOT ALCHEMY_VULKAN_INCLUDE)
        return()
    endif()
    set(source_dir ../../../src/alchemy/darwin/quartzcore/tests)
    set(stubs)
    foreach(entry IN ITEMS
            "metal_iosurface_system|METAL_IOSURFACE_STUB_SYSTEM|/usr/lib/libSystem.B.dylib"
            "metal_iosurface_iosurface|METAL_IOSURFACE_STUB_IOSURFACE|/System/Library/Frameworks/IOSurface.framework/Versions/A/IOSurface")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 stub)
        list(GET parts 1 define)
        list(GET parts 2 install_name)
        alchemy_gui_object(${stub} ${source_dir}/metal_iosurface_stubs.c -D${define})
        add_custom_command(OUTPUT "${macos_dir}/${stub}.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib -install_name "${install_name}"
                "${macos_dir}/${stub}.o" -o "${macos_dir}/${stub}.dylib"
            DEPENDS "${macos_dir}/${stub}.o" VERBATIM)
        list(APPEND stubs "${macos_dir}/${stub}.dylib")
    endforeach()
    set(frameworks "${macos_dir}/link_metal.dylib" "${macos_dir}/link_objc.dylib" "${macos_dir}/link_core_foundation.dylib")
    alchemy_gui_object(metal_iosurface_fixture ${source_dir}/metal_iosurface_fixture.c)
    add_custom_command(OUTPUT "${macos_dir}/metal_iosurface_fixture"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/metal_iosurface_fixture.o" ${stubs} ${frameworks}
            -o "${macos_dir}/metal_iosurface_fixture"
        DEPENDS "${macos_dir}/metal_iosurface_fixture.o" ${stubs} ${frameworks} VERBATIM)
    add_custom_target(alchemy_gui_metal_iosurface_fixtures ALL DEPENDS "${macos_dir}/metal_iosurface_fixture")
    add_dependencies(alchemy_gui_metal_iosurface_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_gui_metal_iosurface "${macos_dir}/metal_iosurface_fixture"
        PROGRESS "metal iosurface: (ok|no Metal device)" COMPLETE "metal iosurface: (ok|no Metal device)")
    alchemy_gui_add_fixture_test(alchemy_gui_metal_iosurface_cross "${macos_dir}/metal_iosurface_fixture"
        PROGRESS "metal iosurface: cross: ok" COMPLETE "metal iosurface: cross: ok" ARGS cross)
    set_tests_properties(alchemy_gui_metal_iosurface alchemy_gui_metal_iosurface_cross PROPERTIES
        ENVIRONMENT "ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_ACTIVATE=1")
    # The GPU-process -> browser shape: a Metal-rendered IOSurface from a child
    # process shown as CALayer contents, checked in the contents capture
    # (guest path argument, host path in the environment).
    set(present_captures "${CMAKE_CURRENT_BINARY_DIR}/metal_iosurface_present")
    file(MAKE_DIRECTORY "${present_captures}")
    string(SUBSTRING "${present_captures}" 0 1 present_drive)
    string(TOUPPER "${present_drive}" present_drive)
    string(SUBSTRING "${present_captures}" 2 -1 present_rest)
    alchemy_gui_add_fixture_test(alchemy_gui_metal_iosurface_present "${macos_dir}/metal_iosurface_fixture"
        PROGRESS "metal iosurface: present: ok" COMPLETE "metal iosurface: present: ok"
        ARGS present "/Volumes/${present_drive}${present_rest}")
    set_tests_properties(alchemy_gui_metal_iosurface_present PROPERTIES
        ENVIRONMENT "ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_CONTENTS_CAPTURE=${present_captures}")
    # The same through host-memory staging (devices without VK_EXT_external_memory_host).
    alchemy_gui_add_fixture_test(alchemy_gui_metal_iosurface_staged "${macos_dir}/metal_iosurface_fixture"
        PROGRESS "metal iosurface: (ok|no Metal device)" COMPLETE "metal iosurface: (ok|no Metal device)" ARGS staged)
    set_tests_properties(alchemy_gui_metal_iosurface_staged PROPERTIES
        ENVIRONMENT "ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_METAL_IOSURFACE_IMPORT=0")
endfunction()
alchemy_gui_register_tests(alchemy_gui_quartzcore_metal_iosurface_tests)

# Moved from darwin/identities/qt_imports.cpp: the CAShapeLayer class shell
# (the support kit).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/quartzcore/shape_layer_shell.cpp)

# Headless additions (both builds; darwin/headless_additions.hpp), moved from
# darwin/identities/installer_frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/quartzcore/contents_formats.cpp)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/quartzcore/base_layers.cpp)
