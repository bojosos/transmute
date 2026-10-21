if(BUILD_TESTING AND TARGET alchemy_personality_tests AND NOT TEST alchemy_object_lookup)
    alchemy_personality_object(object_lookup object_lookup.c)
    add_custom_command(OUTPUT "${macos_dir}/object_lookup"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
            "${macos_dir}/object_lookup.o" "${macos_dir}/link_system.dylib"
            "${macos_dir}/link_objc.dylib" "${macos_dir}/link_core_foundation.dylib"
            "${macos_dir}/link_foundation.dylib" -o "${macos_dir}/object_lookup"
        DEPENDS "${macos_dir}/object_lookup.o" "${macos_dir}/link_system.dylib"
            "${macos_dir}/link_objc.dylib" "${macos_dir}/link_core_foundation.dylib"
            "${macos_dir}/link_foundation.dylib" VERBATIM)
    add_custom_target(alchemy_object_lookup_fixture DEPENDS "${macos_dir}/object_lookup")
    add_dependencies(alchemy_object_lookup_fixture alchemy_personality_fixtures)
    add_dependencies(alchemy_personality_tests alchemy_object_lookup_fixture)
    add_test(NAME alchemy_object_lookup COMMAND alchemy_personality_tests
        "${macos_dir}/object_lookup" "Alchemy object provider lookup: 27")
    set_tests_properties(alchemy_object_lookup PROPERTIES TIMEOUT 120)
endif()
