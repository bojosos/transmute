if(BUILD_TESTING)
    foreach(group IN ITEMS dispatch foundation_services)
        add_executable(alchemy_${group}_tests "tests/alchemy_${group}_tests.cpp")
        target_link_libraries(alchemy_${group}_tests PRIVATE alchemy_core)
        add_test(NAME alchemy_${group} COMMAND alchemy_${group}_tests)
        set_tests_properties(alchemy_${group} PROPERTIES TIMEOUT 60)
    endforeach()
    if(ALCHEMY_CLANG AND ALCHEMY_LD64_LLD)
        alchemy_personality_object(link_dispatch link_dispatch.c)
        alchemy_personality_object(dispatch_workload dispatch_workload.c)
        alchemy_personality_object(foundation_services foundation_services.m)
        add_custom_command(OUTPUT "${macos_dir}/link_dispatch.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name /usr/lib/system/libdispatch.dylib "${macos_dir}/link_dispatch.o"
                -o "${macos_dir}/link_dispatch.dylib"
            DEPENDS "${macos_dir}/link_dispatch.o" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/dispatch_workload"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                "${macos_dir}/dispatch_workload.o" "${macos_dir}/link_dispatch.dylib"
                "${macos_dir}/link_system.dylib" -o "${macos_dir}/dispatch_workload"
            DEPENDS "${macos_dir}/dispatch_workload.o" "${macos_dir}/link_dispatch.dylib"
                "${macos_dir}/link_system.dylib" VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/foundation_services"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                "${macos_dir}/foundation_services.o" "${macos_dir}/link_foundation.dylib"
                "${macos_dir}/link_objc.dylib" "${macos_dir}/link_core_foundation.dylib"
                "${macos_dir}/link_system.dylib" -o "${macos_dir}/foundation_services"
            DEPENDS "${macos_dir}/foundation_services.o" "${macos_dir}/link_foundation.dylib"
                "${macos_dir}/link_objc.dylib" "${macos_dir}/link_core_foundation.dylib"
                "${macos_dir}/link_system.dylib" VERBATIM)
        add_custom_target(alchemy_continuation_fixtures DEPENDS
            "${macos_dir}/dispatch_workload" "${macos_dir}/foundation_services")
        add_dependencies(alchemy_continuation_fixtures alchemy_personality_fixtures)
        add_dependencies(alchemy_personality_tests alchemy_continuation_fixtures)
        add_test(NAME alchemy_dispatch_workload COMMAND alchemy_personality_tests
            "${macos_dir}/dispatch_workload" "Guest dispatch semaphores: 27")
        add_test(NAME alchemy_foundation_workload COMMAND alchemy_personality_tests
            "${macos_dir}/foundation_services" "Foundation process services: 27")
        set_tests_properties(alchemy_dispatch_workload alchemy_foundation_workload PROPERTIES TIMEOUT 120)
    endif()
endif()
