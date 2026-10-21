# Darwin process model (docs/ALCHEMY_PROCESS.md): pipes, fork/vfork
# emulation, posix_spawn, exec, wait, kill, popen/system with the built-in
# shell. The proc_model fixture runs in separate Alchemy processes at the
# default and a rebased load address, on the reference engine and alchemy-fx.
if(BUILD_TESTING AND ALCHEMY_CLANG AND ALCHEMY_LD64_LLD AND macos_dir)
    alchemy_object(proc_link_system proc_link_system.c)
    alchemy_object(proc_model proc_model.c)
    add_custom_command(OUTPUT "${macos_dir}/proc_link_system.dylib"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
            -install_name /usr/lib/libSystem.B.dylib "${macos_dir}/proc_link_system.o"
            -o "${macos_dir}/proc_link_system.dylib"
        DEPENDS "${macos_dir}/proc_link_system.o" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/proc_model"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/proc_model.o" "${macos_dir}/proc_link_system.dylib" -o "${macos_dir}/proc_model"
        DEPENDS "${macos_dir}/proc_model.o" "${macos_dir}/proc_link_system.dylib" VERBATIM)
    add_custom_target(alchemy_process_fixtures DEPENDS "${macos_dir}/proc_model")
    add_executable(alchemy_process_model_tests tests/alchemy_process_model_tests.cpp)
    add_dependencies(alchemy_process_model_tests alchemy_process_fixtures alchemy)
    # The reference engine runs every thread on its own stack: no fork emulation
    # (fork fails with ENOSYS, which the violation case also expects).
    add_test(NAME alchemy_process_model COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy>" "${macos_dir}/proc_model"
        pipe spawn wait popen cloexec kill violation shell installer exec)
    set_tests_properties(alchemy_process_model PROPERTIES TIMEOUT 900)
    # Descriptors 3 and up handed to the root guest process (alchemy --fd):
    # inherited anonymous pipes, named pipes and files, modes, close and EOF.
    alchemy_object(root_fds root_fds.c)
    add_custom_command(OUTPUT "${macos_dir}/root_fds"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/root_fds.o" "${macos_dir}/proc_link_system.dylib" -o "${macos_dir}/root_fds"
        DEPENDS "${macos_dir}/root_fds.o" "${macos_dir}/proc_link_system.dylib" VERBATIM)
    add_custom_target(alchemy_root_fds_fixture DEPENDS "${macos_dir}/root_fds")
    add_executable(alchemy_root_descriptor_tests tests/alchemy_root_descriptor_tests.cpp)
    add_dependencies(alchemy_root_descriptor_tests alchemy_root_fds_fixture alchemy)
    add_test(NAME alchemy_process_root_fds COMMAND alchemy_root_descriptor_tests "$<TARGET_FILE:alchemy>" "${macos_dir}/root_fds")
    set_tests_properties(alchemy_process_root_fds PROPERTIES TIMEOUT 600)
    if(TARGET alchemy-fx)
        add_dependencies(alchemy_process_model_tests alchemy-fx)
        add_test(NAME alchemy_process_model_fx COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/proc_model")
        set_tests_properties(alchemy_process_model_fx PROPERTIES TIMEOUT 900 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
        add_dependencies(alchemy_root_descriptor_tests alchemy-fx)
        add_test(NAME alchemy_process_root_fds_fx COMMAND alchemy_root_descriptor_tests "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/root_fds")
        set_tests_properties(alchemy_process_root_fds_fx PROPERTIES TIMEOUT 600 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
        # Cross-process shared memory (mmap MAP_SHARED, shm_open, ulock and
        # process-shared pthread objects): two alchemy-fx processes, a parent
        # and its posix_spawn'd child, share a segment and synchronize.
        alchemy_object(shm_link_system shm_link_system.c)
        alchemy_object(shm_model shm_model.c)
        add_custom_command(OUTPUT "${macos_dir}/shm_link_system.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name /usr/lib/libSystem.B.dylib "${macos_dir}/shm_link_system.o"
                -o "${macos_dir}/shm_link_system.dylib"
            DEPENDS "${macos_dir}/shm_link_system.o" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/shm_model"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                "${macos_dir}/shm_model.o" "${macos_dir}/shm_link_system.dylib" -o "${macos_dir}/shm_model"
            DEPENDS "${macos_dir}/shm_model.o" "${macos_dir}/shm_link_system.dylib" VERBATIM)
        add_custom_target(alchemy_shm_fixtures DEPENDS "${macos_dir}/shm_model")
        add_dependencies(alchemy_process_model_tests alchemy_shm_fixtures)
        add_test(NAME alchemy_shared_memory_fx COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/shm_model"
            filemap shm ulock pmutex)
        set_tests_properties(alchemy_shared_memory_fx PROPERTIES TIMEOUT 900 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
        # Chrome support, libSystem process level (c2): pids across processes,
        # proc_pidinfo/sysctl of other processes, dispatch PROC exit of a
        # non-child, fcntl/flock locks between processes, devfs nodes.
        alchemy_object(process_tree_system process_tree_system.c)
        alchemy_object(process_tree process_tree.c)
        add_custom_command(OUTPUT "${macos_dir}/process_tree_system.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name /usr/lib/libSystem.B.dylib "${macos_dir}/process_tree_system.o"
                -o "${macos_dir}/process_tree_system.dylib"
            DEPENDS "${macos_dir}/process_tree_system.o" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/process_tree"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                "${macos_dir}/process_tree.o" "${macos_dir}/process_tree_system.dylib" -o "${macos_dir}/process_tree"
            DEPENDS "${macos_dir}/process_tree.o" "${macos_dir}/process_tree_system.dylib" VERBATIM)
        add_custom_target(alchemy_process_tree_fixtures DEPENDS "${macos_dir}/process_tree")
        add_dependencies(alchemy_process_model_tests alchemy_process_tree_fixtures)
        add_test(NAME alchemy_process_tree_fx COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/process_tree"
            identity watch locks dev launchctl)
        set_tests_properties(alchemy_process_tree_fx PROPERTIES TIMEOUT 900 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_NETWORK=loopback")
        # The executable's load range is held from process start (rebased runs
        # used to fail intermittently under load): the regression check, then
        # the rebased cases from 6 drivers at once.
        add_test(NAME alchemy_process_tree_fx_rebased COMMAND alchemy_process_model_tests "$<TARGET_FILE:alchemy-fx>" "${macos_dir}/process_tree"
            --hold-check --loop=3,6 identity dev launchctl)
        set_tests_properties(alchemy_process_tree_fx_rebased PROPERTIES TIMEOUT 900 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_NETWORK=loopback")
    endif()
endif()
