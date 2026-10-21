if(BUILD_TESTING)
    foreach(group IN ITEMS python_services python_support python_loader)
        add_executable(alchemy_${group}_tests "tests/alchemy_${group}_tests.cpp")
        target_link_libraries(alchemy_${group}_tests PRIVATE alchemy_core)
        if(NOT group STREQUAL "python_loader")
            add_test(NAME alchemy_${group} COMMAND alchemy_${group}_tests)
            set_tests_properties(alchemy_${group} PROPERTIES TIMEOUT 60)
        endif()
    endforeach()
    if(ALCHEMY_CLANG AND ALCHEMY_LD64_LLD)
        alchemy_object(python_ctypes_helper python_ctypes_helper.c)
        add_custom_command(OUTPUT "${macos_dir}/python_ctypes_helper.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name @loader_path/python_ctypes_helper.dylib
                "${macos_dir}/python_ctypes_helper.o" -o "${macos_dir}/python_ctypes_helper.dylib"
            DEPENDS "${macos_dir}/python_ctypes_helper.o" VERBATIM)
        add_custom_target(alchemy_python_ctypes_fixture DEPENDS "${macos_dir}/python_ctypes_helper.dylib")
        foreach(name IN ITEMS python_loader_main python_loader_extension python_loader_consumer python_loader_missing
                              python_loader_flat link_audio)
            alchemy_object(${name} ${name}.c)
        endforeach()
        add_custom_command(OUTPUT "${macos_dir}/link_audio.dylib"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -dylib
                -install_name /System/Library/Frameworks/AudioUnit.framework/Versions/A/AudioUnit "${macos_dir}/link_audio.o"
                -o "${macos_dir}/link_audio.dylib"
            DEPENDS "${macos_dir}/link_audio.o" VERBATIM)
        foreach(probe IN ITEMS python_rcpc_probe python_crc_probe)
            alchemy_object(${probe} ${probe}.s)
            add_custom_command(OUTPUT "${macos_dir}/${probe}"
                COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main
                    "${macos_dir}/${probe}.o" -o "${macos_dir}/${probe}"
                DEPENDS "${macos_dir}/${probe}.o" VERBATIM)
        endforeach()
        add_custom_target(alchemy_python_engine_probe DEPENDS
            "${macos_dir}/python_rcpc_probe" "${macos_dir}/python_crc_probe")
        add_custom_command(OUTPUT "${macos_dir}/python_loader_main"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main -export_dynamic
                "${macos_dir}/python_loader_main.o" "${macos_dir}/link_system.dylib"
                -needed_library "${macos_dir}/link_audio.dylib" -o "${macos_dir}/python_loader_main"
            DEPENDS "${macos_dir}/python_loader_main.o" "${macos_dir}/link_system.dylib" "${macos_dir}/link_audio.dylib" VERBATIM)
        foreach(name IN ITEMS python_loader_extension python_loader_consumer python_loader_missing python_loader_flat)
            add_custom_command(OUTPUT "${macos_dir}/${name}.so"
                COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -bundle -undefined dynamic_lookup
                    "${macos_dir}/${name}.o" "${macos_dir}/link_system.dylib" -o "${macos_dir}/${name}.so"
                DEPENDS "${macos_dir}/${name}.o" "${macos_dir}/link_system.dylib" VERBATIM)
        endforeach()
        add_custom_target(alchemy_python_loader_fixtures DEPENDS
            "${macos_dir}/python_loader_main" "${macos_dir}/python_loader_extension.so"
            "${macos_dir}/python_loader_consumer.so" "${macos_dir}/python_loader_missing.so"
            "${macos_dir}/python_loader_flat.so")
        add_dependencies(alchemy_python_loader_fixtures alchemy_macos_fixtures)
        add_dependencies(alchemy_python_loader_tests alchemy_python_loader_fixtures)
        add_test(NAME alchemy_python_loader COMMAND alchemy_python_loader_tests
            "${macos_dir}/python_loader_main" "${macos_dir}/python_loader_extension.so"
            "${macos_dir}/python_loader_consumer.so" "${macos_dir}/python_loader_missing.so"
            "${macos_dir}/python_loader_flat.so")
        set_tests_properties(alchemy_python_loader PROPERTIES TIMEOUT 60)
    endif()
    set(ALCHEMY_PYTHON_HOME "" CACHE PATH "Optional local CPython 3.11 guest resources; never redistributed")
    set(ALCHEMY_PYTHON_RUNTIME_DIRECTORY "" CACHE PATH "Optional authored embedding launcher built by build_alchemy_python.ps1")
    if(ALCHEMY_PYTHON_HOME AND ALCHEMY_PYTHON_RUNTIME_DIRECTORY)
        if(NOT EXISTS "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}/python_embed" OR
           NOT EXISTS "${ALCHEMY_PYTHON_HOME}/lib/python3.11/LICENSE.txt")
            message(FATAL_ERROR "Build the supplied guest CPython embedding runtime and preserve its license first")
        endif()
        add_executable(alchemy_python_tests tests/alchemy_python_tests.cpp)
        target_link_libraries(alchemy_python_tests PRIVATE alchemy_core)
        if(TARGET alchemy_python_ctypes_fixture)
            add_dependencies(alchemy_python_tests alchemy_python_ctypes_fixture)
        endif()
        foreach(module IN ITEMS ctypes numpy)
            if(module STREQUAL "ctypes")
                set(python_doctor_image "${ALCHEMY_PYTHON_HOME}/lib/python3.11/lib-dynload/_ctypes.cpython-311-darwin.so")
                set(python_doctor_symbol "_vm_remap")
                set(python_doctor_requirement "Mach VM shared guest-memory alias mapping")
            else()
                set(python_doctor_image "${ALCHEMY_PYTHON_HOME}/lib/python3.11/site-packages/numpy/linalg/_umath_linalg.cpython-311-darwin.so")
                set(python_doctor_symbol "_dgesdd$NEWLAPACK")
                set(python_doctor_requirement "LAPACK decomposition driver")
            endif()
            if(EXISTS "${python_doctor_image}")
                add_test(NAME alchemy_python_doctor_${module} COMMAND "${CMAKE_COMMAND}"
                    "-DALCHEMY=$<TARGET_FILE:alchemy>" "-DPROGRAM=${python_doctor_image}"
                    "-DSYMBOL=${python_doctor_symbol}" "-DREQUIREMENT=${python_doctor_requirement}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_python_doctor.cmake")
                set_tests_properties(alchemy_python_doctor_${module} PROPERTIES TIMEOUT 60)
            endif()
        endforeach()
        add_test(NAME alchemy_python_version COMMAND alchemy_python_tests
            "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}/python_embed" "${ALCHEMY_PYTHON_HOME}"
            --version "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Python 3.11.15")
        set_tests_properties(alchemy_python_version PROPERTIES TIMEOUT 120)
        set(python_command "import sys, _struct, math, _json, _random, binascii; assert sys.platform == 'darwin'; assert sys.version_info[:2] == (3, 11); assert math.sqrt(729) == 27; assert _struct.unpack('<i', _struct.pack('<i', 27)) == (27,); assert binascii.hexlify(bytes([27, 42])) == b'1b2a'; assert _json.scanstring('\"Alchemy\"', 1)[0] == 'Alchemy'; assert 0 <= _random.Random().random() < 1; print('Guest CPython command: 27')")
        add_test(NAME alchemy_python_command COMMAND alchemy_python_tests
            "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}/python_embed" "${ALCHEMY_PYTHON_HOME}"
            --command "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Guest CPython command: 27"
            "${python_command}")
        set_tests_properties(alchemy_python_command PROPERTIES TIMEOUT 600)
        add_test(NAME alchemy_python_site COMMAND alchemy_python_tests
            "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}/python_embed" "${ALCHEMY_PYTHON_HOME}"
            --site-command "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Guest CPython site: 27"
            "import sys, site; assert 'site' in sys.modules; assert sys.flags.no_site == 0; assert any('site-packages' in item for item in sys.path); print('Guest CPython site: 27')")
        add_test(NAME alchemy_python_exit COMMAND alchemy_python_tests
            "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}/python_embed" "${ALCHEMY_PYTHON_HOME}"
            --command "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Guest CPython exit: 27"
            "import sys; print('Guest CPython exit: 27'); sys.exit(27)" 27)
        set_tests_properties(alchemy_python_site alchemy_python_exit PROPERTIES TIMEOUT 600)
        if(EXISTS "${ALCHEMY_PYTHON_HOME}/bin/python3.11")
            add_test(NAME alchemy_python_standalone_version COMMAND alchemy_python_tests
                "${ALCHEMY_PYTHON_HOME}/bin/python3.11" "${ALCHEMY_PYTHON_HOME}"
                --version "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Python 3.11.15")
            set_tests_properties(alchemy_python_standalone_version PROPERTIES TIMEOUT 120)
            add_test(NAME alchemy_python_standalone_command COMMAND alchemy_python_tests
                "${ALCHEMY_PYTHON_HOME}/bin/python3.11" "${ALCHEMY_PYTHON_HOME}"
                --command "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Guest CPython command: 27"
                "${python_command}")
            add_test(NAME alchemy_python_standalone_site COMMAND alchemy_python_tests
                "${ALCHEMY_PYTHON_HOME}/bin/python3.11" "${ALCHEMY_PYTHON_HOME}"
                --site-command "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Guest CPython site: 27"
                "import sys, site; assert 'site' in sys.modules; assert sys.flags.no_site == 0; assert any('site-packages' in item for item in sys.path); print('Guest CPython site: 27')")
            add_test(NAME alchemy_python_standalone_exit COMMAND alchemy_python_tests
                "${ALCHEMY_PYTHON_HOME}/bin/python3.11" "${ALCHEMY_PYTHON_HOME}"
                --command "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}" "Guest CPython exit: 27"
                "import sys; print('Guest CPython exit: 27'); sys.exit(27)" 27)
            set_tests_properties(alchemy_python_standalone_command alchemy_python_standalone_site
                alchemy_python_standalone_exit PROPERTIES TIMEOUT 600)
        endif()
        foreach(workload IN ITEMS workload extensions compression threads ctypes_calls ctypes numpy network)
            set(python_extension_directory "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}")
            if(workload STREQUAL "workload")
                set(python_expected "Guest CPython workload: 27")
            elseif(workload STREQUAL "extensions")
                set(python_expected "Guest CPython native extensions: 69")
            elseif(workload STREQUAL "compression")
                set(python_expected "Guest CPython compression: 27")
            elseif(workload STREQUAL "threads")
                set(python_expected "Guest CPython threads: 27")
            elseif(workload STREQUAL "ctypes_calls")
                set(python_expected "Guest CPython ctypes: 69")
                set(python_extension_directory "${macos_dir}")
            elseif(workload STREQUAL "ctypes")
                set(python_expected "Guest CPython ctypes callbacks: 69")
                set(python_extension_directory "${macos_dir}")
            elseif(workload STREQUAL "numpy")
                set(python_expected "Alchemy NumPy: arrays BLAS LU Cholesky")
            else()
                set(python_expected "Alchemy Python networking passed")
            endif()
            add_test(NAME alchemy_python_${workload} COMMAND alchemy_python_tests
                "${ALCHEMY_PYTHON_RUNTIME_DIRECTORY}/python_embed" "${ALCHEMY_PYTHON_HOME}"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/python_${workload}.py"
                "${python_extension_directory}" "${python_expected}")
            set_tests_properties(alchemy_python_${workload} PROPERTIES TIMEOUT 600)
            if(EXISTS "${ALCHEMY_PYTHON_HOME}/bin/python3.11")
                add_test(NAME alchemy_python_standalone_${workload} COMMAND alchemy_python_tests
                    "${ALCHEMY_PYTHON_HOME}/bin/python3.11" "${ALCHEMY_PYTHON_HOME}"
                    "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/python_${workload}.py"
                    "${python_extension_directory}" "${python_expected}")
                set_tests_properties(alchemy_python_standalone_${workload} PROPERTIES TIMEOUT 600)
            endif()
        endforeach()
    endif()
endif()
