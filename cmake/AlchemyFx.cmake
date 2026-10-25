# Experimental: Alchemy on the fx engine (alchemy-fx.exe).
#
# Builds Alchemy's unchanged sources against transmute::FxEngine
# (src/alchemy/fx), which implements the ReferenceEngine subset Alchemy uses on
# the fx runtime. Requires TRANSMUTE_WITH_FX. The default alchemy.exe and its
# tests are unaffected.

if(NOT TARGET fx_runtime)
    return()
endif()

add_library(alchemy_fx_engine STATIC src/alchemy/fx/fx_engine.cpp src/alchemy/fx/fx_profile_window.cpp)
fx_component_options(alchemy_fx_engine)
alchemy_hash_map_options(alchemy_fx_engine)
foreach(lib fx_loader fx_runtime fx_abi fx_seh fx_cache fx_translator
            fx_emit_fp fx_emit_neon fx_emit_system fx_common fx_oracle)
    if(TARGET ${lib})
        target_link_libraries(alchemy_fx_engine PUBLIC ${lib})
    endif()
endforeach()

get_target_property(alchemy_fx_sources alchemy_core SOURCES)
# Headless shells whose names a GUI provider owns in alchemy-fx
# (cmake/AlchemyDarwin.cmake): alchemy.exe only.
if(ALCHEMY_HEADLESS_ONLY_SOURCES)
    list(REMOVE_ITEM alchemy_fx_sources ${ALCHEMY_HEADLESS_ONLY_SOURCES})
endif()
set(alchemy_fx_prelude "${PROJECT_SOURCE_DIR}/src/alchemy/fx/alchemy_fx_prelude.hpp")

add_library(alchemy_core_fx STATIC ${alchemy_fx_sources})
target_include_directories(alchemy_core_fx PUBLIC include ${PROJECT_SOURCE_DIR}/src)
if(ALCHEMY_HOST_POISON_DIRECTORY)
    # ALCHEMY_HOST=null: OS headers fail to compile here (cmake/AlchemyHost.cmake).
    target_include_directories(alchemy_core_fx BEFORE PRIVATE "${ALCHEMY_HOST_POISON_DIRECTORY}")
endif()
alchemy_hash_map_options(alchemy_core_fx)
target_compile_features(alchemy_core_fx PUBLIC cxx_std_20)
target_link_libraries(alchemy_core_fx PUBLIC alchemy_fx_engine)
if(WIN32)
    target_link_libraries(alchemy_core_fx PRIVATE mincore bcrypt advapi32 ws2_32 iphlpapi normaliz)
endif()
if(MSVC)
    target_compile_options(alchemy_core_fx PRIVATE /W3 /permissive- "/FI${alchemy_fx_prelude}")
endif()
# GUI providers (cmake/AlchemyGui.cmake): fx-only, never part of alchemy_core.
if(ALCHEMY_GUI_SOURCES)
    target_sources(alchemy_core_fx PRIVATE ${ALCHEMY_GUI_SOURCES})
    target_compile_definitions(alchemy_core_fx PUBLIC ALCHEMY_WITH_GUI PRIVATE ${ALCHEMY_GUI_COMPILE_DEFINITIONS})
    target_include_directories(alchemy_core_fx PRIVATE ${ALCHEMY_GUI_INCLUDE_DIRECTORIES})
    target_link_libraries(alchemy_core_fx PRIVATE ${ALCHEMY_GUI_LINK_LIBRARIES})
endif()

add_executable(alchemy-fx src/alchemy/main.cpp)
if(MSVC)
    # longPathAware: installed bundles (Chrome's helpers) exceed MAX_PATH.
    target_sources(alchemy-fx PRIVATE src/alchemy/alchemy.manifest)
endif()
target_link_libraries(alchemy-fx PRIVATE alchemy_core_fx)
if(MSVC)
    target_compile_options(alchemy-fx PRIVATE /W3 /permissive- "/FI${alchemy_fx_prelude}")
    # Host threads run fx dispatchers and nested guest<->host callback frames;
    # guest stacks are separate fx allocations. 8 MiB is reserve, not commit.
    target_link_options(alchemy-fx PRIVATE /DEBUG /STACK:8388608)
endif()

# Engine tests: Mach-O fixtures run in separate alchemy-fx processes.
if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir)
    alchemy_object(fx_link_system fx_link_system.c)
    alchemy_object(fx_threads fx_threads.c)
    alchemy_object(fx_tlv fx_tlv.c)
    alchemy_object(fx_tlv_plugin fx_tlv_plugin.c)
    alchemy_object(fx_frint_probe fx_frint_probe.s)
    alchemy_object(fx_vm_remap fx_vm_remap.c)
    alchemy_object(fx_vm_reserve fx_vm_reserve.c)
    alchemy_object(fx_jit_flush fx_jit_flush.c)
    alchemy_object(fx_vm_lazy fx_vm_lazy.c)
    alchemy_object(fx_mach_ports fx_mach_ports.c)
    alchemy_object(fx_mach_cross fx_mach_cross.c)
    alchemy_object(fx_mach_link_system fx_mach_link_system.c)
    alchemy_object(fx_leaf_calls fx_leaf_calls.c)
    alchemy_object(fx_table_shadow fx_table_shadow.c)
    add_custom_command(OUTPUT "${macos_dir}/fx_link_system.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /usr/lib/libSystem.B.dylib "${macos_dir}/fx_link_system.o"
            -o "${macos_dir}/fx_link_system.dylib"
        DEPENDS "${macos_dir}/fx_link_system.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_threads"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_threads.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_threads"
        DEPENDS "${macos_dir}/fx_threads.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_tlv"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_tlv.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_tlv"
        DEPENDS "${macos_dir}/fx_tlv.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_tlv_plugin.bundle"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -bundle -undefined dynamic_lookup
            "${macos_dir}/fx_tlv_plugin.o" -o "${macos_dir}/fx_tlv_plugin.bundle"
        DEPENDS "${macos_dir}/fx_tlv_plugin.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_frint_probe"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_frint_probe.o" -o "${macos_dir}/fx_frint_probe"
        DEPENDS "${macos_dir}/fx_frint_probe.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_vm_remap"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_vm_remap.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_vm_remap"
        DEPENDS "${macos_dir}/fx_vm_remap.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_vm_reserve"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_vm_reserve.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_vm_reserve"
        DEPENDS "${macos_dir}/fx_vm_reserve.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_jit_flush"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_jit_flush.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_jit_flush"
        DEPENDS "${macos_dir}/fx_jit_flush.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_vm_lazy"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_vm_lazy.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_vm_lazy"
        DEPENDS "${macos_dir}/fx_vm_lazy.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_mach_ports"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_mach_ports.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_mach_ports"
        DEPENDS "${macos_dir}/fx_mach_ports.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_mach_link_system.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /usr/lib/libSystem.B.dylib "${macos_dir}/fx_mach_link_system.o"
            -o "${macos_dir}/fx_mach_link_system.dylib"
        DEPENDS "${macos_dir}/fx_mach_link_system.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_mach_cross"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_mach_cross.o" "${macos_dir}/fx_mach_link_system.dylib" -o "${macos_dir}/fx_mach_cross"
        DEPENDS "${macos_dir}/fx_mach_cross.o" "${macos_dir}/fx_mach_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_leaf_calls"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_leaf_calls.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_leaf_calls"
        DEPENDS "${macos_dir}/fx_leaf_calls.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/fx_table_shadow"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/fx_table_shadow.o" "${macos_dir}/fx_link_system.dylib" -o "${macos_dir}/fx_table_shadow"
        DEPENDS "${macos_dir}/fx_table_shadow.o" "${macos_dir}/fx_link_system.dylib" VERBATIM)
    add_custom_target(alchemy_fx_engine_fixtures DEPENDS "${macos_dir}/fx_threads" "${macos_dir}/fx_frint_probe"
        "${macos_dir}/fx_tlv" "${macos_dir}/fx_tlv_plugin.bundle" "${macos_dir}/fx_leaf_calls" "${macos_dir}/fx_table_shadow"
        "${macos_dir}/fx_vm_remap" "${macos_dir}/fx_vm_reserve" "${macos_dir}/fx_vm_lazy" "${macos_dir}/fx_mach_ports"
        "${macos_dir}/fx_mach_cross" "${macos_dir}/fx_jit_flush")
    add_executable(alchemy_fx_engine_tests tests/alchemy_fx_engine_tests.cpp)
    add_dependencies(alchemy_fx_engine_tests alchemy_fx_engine_fixtures alchemy-fx)
    set(alchemy_fx_engine_test_arguments "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/fx_threads"
        "${macos_dir}/fx_frint_probe" "--vm-remap=${macos_dir}/fx_vm_remap" "--vm-reserve=${macos_dir}/fx_vm_reserve"
        "--jit-flush=${macos_dir}/fx_jit_flush" "--vm-lazy=${macos_dir}/fx_vm_lazy")
    if(TARGET alchemy_expanded_fixtures)
        add_dependencies(alchemy_fx_engine_tests alchemy_expanded_fixtures)
        list(APPEND alchemy_fx_engine_test_arguments "${macos_dir}/threaded")
    endif()
    add_test(NAME alchemy_fx_engine COMMAND alchemy_fx_engine_tests ${alchemy_fx_engine_test_arguments})
    set_tests_properties(alchemy_fx_engine PROPERTIES TIMEOUT 600)
    # In-process Mach IPC on guest threads (src/alchemy/darwin/libsystem/mach):
    # blocked senders, kevent64 EVFILT_MACHPORT, rights in messages, memory entries.
    add_test(NAME alchemy_fx_mach_ports COMMAND alchemy-fx "${macos_dir}/fx_mach_ports")
    set_tests_properties(alchemy_fx_mach_ports PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx mach ports: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1")
    # Mach IPC between two Alchemy processes: bootstrap rendezvous, special
    # ports from posix_spawn, rights and messages both ways, notifications on exit.
    add_test(NAME alchemy_fx_mach_cross COMMAND alchemy-fx "${macos_dir}/fx_mach_cross")
    set_tests_properties(alchemy_fx_mach_cross PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx mach cross: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1")
    # libSystem's pure memory/string calls: the engine's leaf fast path
    # (FxEngine::set_guest_call_leaf) and, with ALCHEMY_PERF=1 (no leaf
    # entries), their handlers give the same results and registers.
    add_test(NAME alchemy_fx_leaf_calls COMMAND alchemy-fx "${macos_dir}/fx_leaf_calls")
    set_tests_properties(alchemy_fx_leaf_calls PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx leaf calls: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1")
    add_test(NAME alchemy_fx_leaf_calls_no_probe_leaf COMMAND alchemy-fx "${macos_dir}/fx_leaf_calls")
    set_tests_properties(alchemy_fx_leaf_calls_no_probe_leaf PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx leaf calls: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_FX_STACK_PROBE_LEAF=0")
    add_test(NAME alchemy_fx_leaf_calls_handlers COMMAND alchemy-fx "${macos_dir}/fx_leaf_calls")
    set_tests_properties(alchemy_fx_leaf_calls_handlers PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx leaf calls: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_PERF=1")
    # x0/x1 in the cross-region convention: their context slots are stale at a
    # leaf entry, so the engine's leaves must fall back to the handlers.
    add_test(NAME alchemy_fx_leaf_calls_globals COMMAND alchemy-fx "${macos_dir}/fx_leaf_calls")
    set_tests_properties(alchemy_fx_leaf_calls_globals PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx leaf calls: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1;TRANSMUTE_FX_GLOBALS=sp,x0,x1,x19,x20,fp,lr")
    # BRs through a writable dispatch table (V8's Ignition) take fx's table
    # shadows: handlers rewritten, table entries changed, two tables, threads;
    # and the same with the shadows off.
    add_test(NAME alchemy_fx_table_shadow COMMAND alchemy-fx "${macos_dir}/fx_table_shadow")
    set_tests_properties(alchemy_fx_table_shadow PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx table shadow: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1")
    add_test(NAME alchemy_fx_table_shadow_off COMMAND alchemy-fx "${macos_dir}/fx_table_shadow")
    set_tests_properties(alchemy_fx_table_shadow_off PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx table shadow: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1;TRANSMUTE_FX_CORE=noshadows")
    # Mach-O thread-local variables through __tlv_bootstrap and its TPIDR_EL0
    # fast path: late dlopen, two images, contention, thread reuse, callbacks.
    add_test(NAME alchemy_fx_tlv COMMAND alchemy-fx "${macos_dir}/fx_tlv" "${macos_dir}/fx_tlv_plugin.bundle")
    set_tests_properties(alchemy_fx_tlv PROPERTIES TIMEOUT 120 PASS_REGULAR_EXPRESSION "fx tlv: ok"
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1")
    # ALCHEMY_GUI_NO_CRASH_DIALOG=1: a host crash prints its module+offset and
    # ends the process (no WER dialog); the self-test writes through null.
    add_test(NAME alchemy_fx_crash_report COMMAND alchemy-fx "${macos_dir}/fx_frint_probe")
    set_tests_properties(alchemy_fx_crash_report PROPERTIES TIMEOUT 60
        ENVIRONMENT "ALCHEMY_GUI_NO_CRASH_DIALOG=1;ALCHEMY_GUI_CRASH_SELFTEST=1"
        PASS_REGULAR_EXPRESSION "alchemy: host crash: exception 0xc0000005 at 0x[0-9a-f]+ alchemy-fx[.]exe[+]0x[0-9a-f]+.*write 0x0")
endif()
