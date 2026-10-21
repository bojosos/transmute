# Metal on Vulkan. Owner: A5 (src/alchemy/darwin/metal/). vulkan-1.dll is loaded at
# run time; only the Vulkan-Headers include directory is needed to build.
# Without the headers the provider compiles to an installer that keeps the
# headless Metal shells (no device).
if(ALCHEMY_VULKAN_INCLUDE)
    list(APPEND ALCHEMY_GUI_SOURCES
        src/alchemy/darwin/metal/install.cpp
        src/alchemy/darwin/metal/metal.cpp
        src/alchemy/darwin/metal/metal_objects.cpp
        src/alchemy/darwin/metal/metal_descriptors.cpp
        src/alchemy/darwin/metal/vulkan.cpp
        src/alchemy/darwin/metal/formats.cpp
        src/alchemy/darwin/metal/device.cpp
        src/alchemy/darwin/metal/gpu_identity.cpp
        src/alchemy/darwin/metal/resources.cpp
        src/alchemy/darwin/metal/pipeline.cpp
        src/alchemy/darwin/metal/commands.cpp
        src/alchemy/darwin/metal/quartzcore_link.cpp
        src/alchemy/darwin/metal/surface_textures.cpp
        src/alchemy/darwin/metal/external_memory.cpp
        src/alchemy/darwin/metal/shared_events.cpp)
    list(APPEND ALCHEMY_GUI_INCLUDE_DIRECTORIES "${ALCHEMY_VULKAN_INCLUDE}")
    list(APPEND ALCHEMY_GUI_COMPILE_DEFINITIONS ALCHEMY_HAVE_VULKAN_HEADERS=1)
else()
    list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/metal/metal_unavailable.cpp)
endif()
# MetalKit (MTKView, MTKTextureLoader) and Metal device discovery with the
# CoreGraphics display's Metal device; built with or without Vulkan headers.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/metal/metalkit.cpp
    src/alchemy/darwin/metal/metal_display.cpp)

# glslangValidator compiles the test shaders (GLSL written to the MSL
# contract's resource model) to SPIR-V word arrays at build time.
find_program(ALCHEMY_GLSLANG_VALIDATOR glslangValidator HINTS ${alchemy_gui_vulkan_bin_hints}
    DOC "glslangValidator for the Metal-on-Vulkan tests (test-only)")

# Headless shell (src/alchemy/darwin/shells.hpp): alchemy.exe's answers for
# names this framework's GUI provider owns in alchemy-fx, where the _fx twin
# defines the same entry point empty.
# Without the Vulkan headers metal_objects.cpp is not built, so alchemy-fx keeps the
# headless shell too: MTLCreateSystemDefaultDevice answers nil there as well.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/metal/metal_headless.cpp)
if(ALCHEMY_VULKAN_INCLUDE)
    list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/metal/metal_headless.cpp)
    list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/metal/metal_fx.cpp)
endif()

function(alchemy_gui_metal_tests)
    alchemy_gui_add_unit_test(alchemy_metal_smoke src/alchemy/darwin/metal/tests/metal_smoke_tests.cpp)
    if(NOT ALCHEMY_VULKAN_INCLUDE)
        return()
    endif()
    # The guest-visible objects through the Objective-C runtime (device
    # identity, descriptors, events); device parts skip without a GPU.
    alchemy_gui_add_unit_test(alchemy_metal_objects src/alchemy/darwin/metal/tests/metal_objects_tests.cpp TIMEOUT 120)
    # g2 (Chrome stage 4): libraries, functions, pipelines and argument encoders through the runtime.
    alchemy_gui_add_unit_test(alchemy_metal_libraries src/alchemy/darwin/metal/tests/metal_library_tests.cpp TIMEOUT 180)
    # g3: host memory imported into the device (external_memory.cpp); 77 without the extension.
    alchemy_gui_add_unit_test(alchemy_metal_external_memory src/alchemy/darwin/metal/tests/external_memory_tests.cpp TIMEOUT 120)
    set_tests_properties(alchemy_metal_external_memory PROPERTIES SKIP_RETURN_CODE 77)
    alchemy_gui_add_unit_test(alchemy_metal_vulkan src/alchemy/darwin/metal/tests/metal_vulkan_tests.cpp TIMEOUT 120)
    # 77: no Vulkan 1.3 device on this machine (reported, not a failure).
    set_tests_properties(alchemy_metal_vulkan PROPERTIES SKIP_RETURN_CODE 77)
    if(ALCHEMY_GLSLANG_VALIDATOR)
        set(shader_dir "${CMAKE_CURRENT_BINARY_DIR}/alchemy_metal_shaders")
        file(MAKE_DIRECTORY "${shader_dir}")
        set(shader_headers)
        foreach(shader IN ITEMS scale.comp triangle.vert triangle.frag sample.frag fetch_write.frag fetch_read.frag
                argument_samplers.frag)
            string(REPLACE "." "_" variable "${shader}")
            set(header "${shader_dir}/${variable}.h")
            add_custom_command(OUTPUT "${header}"
                COMMAND "${ALCHEMY_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 --vn "${variable}_spirv"
                    -o "${header}" "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/metal/tests/shaders/${shader}"
                DEPENDS "${PROJECT_SOURCE_DIR}/src/alchemy/darwin/metal/tests/shaders/${shader}" VERBATIM)
            list(APPEND shader_headers "${header}")
        endforeach()
        target_sources(alchemy_metal_vulkan PRIVATE ${shader_headers})
        target_include_directories(alchemy_metal_vulkan PRIVATE "${shader_dir}")
        target_compile_definitions(alchemy_metal_vulkan PRIVATE ALCHEMY_METAL_TEST_SHADERS=1)
    endif()
    if(NOT ALCHEMY_GUI_FIXTURES_ENABLED)
        return()
    endif()
    # The guest-visible Metal interface end to end (prints "no device" and
    # passes on machines without a Vulkan 1.3 GPU). With the MSL compiler
    # (gui/a6-msl) it also builds compute and render pipelines from
    # newLibraryWithSource: and prints "kernel dispatched" / "triangle drawn";
    # once that compiler is merged, COMPLETE can require
    # "Alchemy Metal probe: (triangle drawn|no device)".
    alchemy_gui_object(metal_probe metal_probe.m)
    alchemy_gui_program(metal_probe metal_probe)
    add_custom_target(alchemy_metal_fixtures ALL DEPENDS "${macos_dir}/metal_probe")
    add_dependencies(alchemy_metal_fixtures alchemy_gui_link_stubs)
    alchemy_gui_add_fixture_test(alchemy_metal_probe "${macos_dir}/metal_probe"
        PROGRESS "Alchemy Metal probe: (device|no device)"
        COMPLETE "Alchemy Metal probe: (triangle drawn.*Alchemy Metal probe: done|no device)")
endfunction()
alchemy_gui_register_tests(alchemy_gui_metal_tests)

# Moved from darwin/identities/qt_imports.cpp: the MTLStageInputOutputDescriptor
# class shell (the support kit).
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/metal/stage_input_output_shell.cpp)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/metal/base_classes.cpp)
