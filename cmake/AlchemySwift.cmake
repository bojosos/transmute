# Guest Swift runtime (/usr/lib/swift/libswiftCore.dylib) built from the pinned
# swift-6.2-RELEASE sources by scripts/build_alchemy_swift.ps1 with the
# swift.org Windows toolchain (docs/ALCHEMY_SWIFT.md), and its fixture test.
set(ALCHEMY_SWIFT_RUNTIME_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-swift-arm64" CACHE PATH
    "Independently built ARM64 Swift runtime directory (scripts/build_alchemy_swift.ps1)")
set(ALCHEMY_SWIFT_TOOLCHAIN_DIR "${CMAKE_SOURCE_DIR}/artifacts/swift-toolchain" CACHE PATH
    "Administrative extraction of the swift.org Swift 6.2 Windows toolchain")
set(ALCHEMY_SWIFT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/artifacts/alchemy-swift-source" CACHE PATH
    "Pinned swift-6.2-RELEASE source checkouts (swift, libdispatch)")
set(alchemy_swift_bin "${ALCHEMY_SWIFT_TOOLCHAIN_DIR}/LocalApp/Programs/Swift/Toolchains/6.2.0+Asserts/usr/bin")

if(WIN32 AND ALCHEMY_POWERSHELL)
    add_custom_target(alchemy_swift_runtime
        COMMAND "${ALCHEMY_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/scripts/build_alchemy_swift.ps1"
            -SourceDirectory "${ALCHEMY_SWIFT_SOURCE_DIR}" -ToolchainDirectory "${ALCHEMY_SWIFT_TOOLCHAIN_DIR}"
            -LlvmRuntimeDirectory "${ALCHEMY_LLVM_RUNTIME_DIR}" -OutputDirectory "${ALCHEMY_SWIFT_RUNTIME_DIR}" VERBATIM)
endif()

if(BUILD_TESTING AND ALCHEMY_LD64_LLD AND macos_dir AND TARGET alchemy-fx
   AND EXISTS "${alchemy_swift_bin}/swift-frontend.exe"
   AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCore.dylib"
   AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core/Swift.swiftmodule"
   AND EXISTS "${ALCHEMY_LLVM_RUNTIME_DIR}/libc++.1.dylib")
    # The frontend loads the toolchain's runtime DLLs from the extraction root.
    set(alchemy_swift_env "PATH=${alchemy_swift_bin}$<SEMICOLON>${ALCHEMY_SWIFT_TOOLCHAIN_DIR}")
    add_custom_command(OUTPUT "${macos_dir}/swift_core.o"
        COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
            -frontend -c -target arm64-apple-macosx11.0 -module-name swift_core -O
            -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core"
            -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
            -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_core.swift" -o "${macos_dir}/swift_core.o"
        DEPENDS tests/fixtures/macos/swift_core.swift "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCore.dylib" VERBATIM)
    add_custom_command(OUTPUT "${macos_dir}/swift_core"
        COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_core.o"
            "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCore.dylib" "${macos_dir}/link_system.dylib"
            "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_core"
        DEPENDS "${macos_dir}/swift_core.o" "${macos_dir}/link_system.dylib" VERBATIM)
    add_custom_target(alchemy_swift_fixtures ALL DEPENDS "${macos_dir}/swift_core")
    add_dependencies(alchemy_swift_fixtures alchemy_macos_fixtures)
    add_test(NAME alchemy_swift_core COMMAND "${CMAKE_COMMAND}"
        "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_core"
        "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
        "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_core.expected"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
    set_tests_properties(alchemy_swift_core PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")

    # The open-source stdlib modules beyond the core (_Concurrency, Observation,
    # Synchronization, Darwin), once the runtime directory has them.
    set(alchemy_swift_stdlib "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/stdlib")
    if(EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswift_Concurrency.dylib" AND EXISTS "${alchemy_swift_stdlib}/_Concurrency.swiftmodule")
        set(alchemy_swift_concurrency_libraries)
        foreach(library swift_Concurrency swiftObservation swiftSynchronization swiftDarwin swiftCore)
            list(APPEND alchemy_swift_concurrency_libraries "${ALCHEMY_SWIFT_RUNTIME_DIR}/lib${library}.dylib")
        endforeach()
        add_custom_command(OUTPUT "${macos_dir}/swift_concurrency.o"
            COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                -frontend -c -target arm64-apple-macosx15.0 -module-name swift_concurrency -O -parse-as-library
                -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${alchemy_swift_stdlib}"
                -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                -plugin-path "${alchemy_swift_bin}" -disable-implicit-string-processing-module-import
                -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_concurrency.swift" -o "${macos_dir}/swift_concurrency.o"
            DEPENDS tests/fixtures/macos/swift_concurrency.swift ${alchemy_swift_concurrency_libraries} VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/swift_concurrency"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_concurrency.o"
                ${alchemy_swift_concurrency_libraries} "${macos_dir}/link_system.dylib"
                "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_concurrency"
            DEPENDS "${macos_dir}/swift_concurrency.o" "${macos_dir}/link_system.dylib" VERBATIM)
        add_custom_target(alchemy_swift_concurrency_fixture ALL DEPENDS "${macos_dir}/swift_concurrency")
        add_dependencies(alchemy_swift_concurrency_fixture alchemy_macos_fixtures)
        add_test(NAME alchemy_swift_concurrency COMMAND "${CMAKE_COMMAND}"
            "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_concurrency"
            "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
            "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_concurrency.expected"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
        set_tests_properties(alchemy_swift_concurrency PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")

        # CoreFoundation and Foundation meeting Swift's bridged strings (guest-implemented NSString objects).
        if(EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib" AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays/Foundation.swiftmodule")
            set(alchemy_swift_cf_strings_libraries "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib"
                ${alchemy_swift_concurrency_libraries})
            add_custom_command(OUTPUT "${macos_dir}/swift_cf_strings.o"
                COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                    -frontend -c -target arm64-apple-macosx15.0 -module-name swift_cf_strings -O -parse-as-library
                    -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${alchemy_swift_stdlib}" -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine"
                    -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays"
                    -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                    -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                    -plugin-path "${alchemy_swift_bin}" -disable-implicit-string-processing-module-import
                    -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                    "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_cf_strings.swift" -o "${macos_dir}/swift_cf_strings.o"
                DEPENDS tests/fixtures/macos/swift_cf_strings.swift ${alchemy_swift_cf_strings_libraries} VERBATIM)
            add_custom_command(OUTPUT "${macos_dir}/swift_cf_strings"
                COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_cf_strings.o"
                    ${alchemy_swift_cf_strings_libraries} "${macos_dir}/link_system.dylib"
                    "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_cf_strings"
                DEPENDS "${macos_dir}/swift_cf_strings.o" "${macos_dir}/link_system.dylib" VERBATIM)
            add_custom_target(alchemy_swift_cf_strings_fixture ALL DEPENDS "${macos_dir}/swift_cf_strings")
            add_dependencies(alchemy_swift_cf_strings_fixture alchemy_macos_fixtures)
            add_test(NAME alchemy_swift_cf_strings COMMAND "${CMAKE_COMMAND}"
                "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_cf_strings"
                "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
                "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_cf_strings.expected"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
            set_tests_properties(alchemy_swift_cf_strings PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
        endif()

        # ... and Swift's bridged collections (guest-implemented NSArray and NSDictionary objects).
        if(EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib" AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays/Foundation.swiftmodule")
            set(alchemy_swift_cf_collections_libraries "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib"
                ${alchemy_swift_concurrency_libraries})
            add_custom_command(OUTPUT "${macos_dir}/swift_cf_collections.o"
                COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                    -frontend -c -target arm64-apple-macosx15.0 -module-name swift_cf_collections -O -parse-as-library
                    -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${alchemy_swift_stdlib}" -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine"
                    -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays"
                    -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                    -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                    -plugin-path "${alchemy_swift_bin}" -disable-implicit-string-processing-module-import
                    -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                    "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_cf_collections.swift" -o "${macos_dir}/swift_cf_collections.o"
                DEPENDS tests/fixtures/macos/swift_cf_collections.swift ${alchemy_swift_cf_collections_libraries} VERBATIM)
            add_custom_command(OUTPUT "${macos_dir}/swift_cf_collections"
                COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_cf_collections.o"
                    ${alchemy_swift_cf_collections_libraries} "${macos_dir}/link_system.dylib"
                    "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_cf_collections"
                DEPENDS "${macos_dir}/swift_cf_collections.o" "${macos_dir}/link_system.dylib" VERBATIM)
            add_custom_target(alchemy_swift_cf_collections_fixture ALL DEPENDS "${macos_dir}/swift_cf_collections")
            add_dependencies(alchemy_swift_cf_collections_fixture alchemy_macos_fixtures)
            add_test(NAME alchemy_swift_cf_collections COMMAND "${CMAKE_COMMAND}"
                "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_cf_collections"
                "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
                "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_cf_collections.expected"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
            set_tests_properties(alchemy_swift_cf_collections PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
        endif()

        # Combine.framework (OpenCombine, third_party/opencombine), once the runtime directory has it.
        if(EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/Combine" AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine/Combine.swiftmodule")
            add_custom_command(OUTPUT "${macos_dir}/swift_combine.o"
                COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                    -frontend -c -target arm64-apple-macosx15.0 -module-name swift_combine -O -parse-as-library
                    -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${alchemy_swift_stdlib}" -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine"
                    -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                    -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                    -plugin-path "${alchemy_swift_bin}" -disable-implicit-string-processing-module-import
                    -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                    "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_combine.swift" -o "${macos_dir}/swift_combine.o"
                DEPENDS tests/fixtures/macos/swift_combine.swift "${ALCHEMY_SWIFT_RUNTIME_DIR}/Combine" ${alchemy_swift_concurrency_libraries} VERBATIM)
            add_custom_command(OUTPUT "${macos_dir}/swift_combine"
                COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_combine.o"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/Combine" ${alchemy_swift_concurrency_libraries} "${macos_dir}/link_system.dylib"
                    "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_combine"
                DEPENDS "${macos_dir}/swift_combine.o" "${macos_dir}/link_system.dylib" VERBATIM)
            add_custom_target(alchemy_swift_combine_fixture ALL DEPENDS "${macos_dir}/swift_combine")
            add_dependencies(alchemy_swift_combine_fixture alchemy_macos_fixtures)
            add_test(NAME alchemy_swift_combine COMMAND "${CMAKE_COMMAND}"
                "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_combine"
                "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
                "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_combine.expected"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
            set_tests_properties(alchemy_swift_combine PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")

            # libswiftFoundation's value types and libswiftObjectiveC, once the runtime directory has the latter.
            if(EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftObjectiveC.dylib")
                set(alchemy_swift_foundation_libraries "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftObjectiveC.dylib" "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftos.dylib"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/Combine" ${alchemy_swift_concurrency_libraries})
                add_custom_command(OUTPUT "${macos_dir}/swift_foundation.o"
                    COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                        -frontend -c -target arm64-apple-macosx15.0 -module-name swift_foundation -O -parse-as-library
                        -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${alchemy_swift_stdlib}" -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine"
                        -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays"
                        -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                        -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                        -plugin-path "${alchemy_swift_bin}" -disable-implicit-string-processing-module-import
                        -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                        "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_foundation.swift" -o "${macos_dir}/swift_foundation.o"
                    DEPENDS tests/fixtures/macos/swift_foundation.swift ${alchemy_swift_foundation_libraries} VERBATIM)
                add_custom_command(OUTPUT "${macos_dir}/swift_foundation"
                    COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_foundation.o"
                        ${alchemy_swift_foundation_libraries} "${macos_dir}/link_system.dylib"
                        "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_foundation"
                    DEPENDS "${macos_dir}/swift_foundation.o" "${macos_dir}/link_system.dylib" VERBATIM)
                add_custom_target(alchemy_swift_foundation_fixture ALL DEPENDS "${macos_dir}/swift_foundation")
                add_dependencies(alchemy_swift_foundation_fixture alchemy_macos_fixtures)
                add_test(NAME alchemy_swift_foundation COMMAND "${CMAKE_COMMAND}"
                    "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_foundation"
                    "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
                    "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_foundation.expected"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
                set_tests_properties(alchemy_swift_foundation PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
            endif()

            # The overlay members OBS binds: String.cString(using:), NSDictionary literals, libswiftCoreFoundation's
            # CGFloat and _CFObject, and CMTime from libswiftCoreMedia.
            if(EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftObjectiveC.dylib" AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCoreFoundation.dylib"
               AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCoreMedia.dylib")
                set(alchemy_swift_overlay_members_libraries "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCoreMedia.dylib"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib" "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCoreFoundation.dylib"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftObjectiveC.dylib" "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftos.dylib"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/Combine" ${alchemy_swift_concurrency_libraries})
                add_custom_command(OUTPUT "${macos_dir}/swift_overlay_members.o"
                    COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                        -frontend -c -target arm64-apple-macosx15.0 -module-name swift_overlay_members -O -parse-as-library
                        -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${alchemy_swift_stdlib}" -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine"
                        -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays"
                        -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                        -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                        -Xcc "-I${CMAKE_SOURCE_DIR}/src/alchemy/darwin/swift/include"
                        -plugin-path "${alchemy_swift_bin}" -disable-implicit-string-processing-module-import
                        -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                        "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_overlay_members.swift" -o "${macos_dir}/swift_overlay_members.o"
                    DEPENDS tests/fixtures/macos/swift_overlay_members.swift ${alchemy_swift_overlay_members_libraries} VERBATIM)
                add_custom_command(OUTPUT "${macos_dir}/swift_overlay_members"
                    COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_overlay_members.o"
                        ${alchemy_swift_overlay_members_libraries} "${macos_dir}/link_system.dylib"
                        "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_overlay_members"
                    DEPENDS "${macos_dir}/swift_overlay_members.o" "${macos_dir}/link_system.dylib" VERBATIM)
                add_custom_target(alchemy_swift_overlay_members_fixture ALL DEPENDS "${macos_dir}/swift_overlay_members")
                add_dependencies(alchemy_swift_overlay_members_fixture alchemy_macos_fixtures)
                add_test(NAME alchemy_swift_overlay_members COMMAND "${CMAKE_COMMAND}"
                    "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_overlay_members"
                    "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
                    "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_overlay_members.expected"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
                set_tests_properties(alchemy_swift_overlay_members PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
            endif()

            # The Dispatch overlay's timer source (makeTimerSource, schedule(deadline:repeating:leeway:), DispatchTimeInterval).
            if(EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftObjectiveC.dylib" AND EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftDispatch.dylib")
                set(alchemy_swift_dispatch_timer_libraries "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftDispatch.dylib"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib" "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftObjectiveC.dylib"
                    "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftos.dylib" "${ALCHEMY_SWIFT_RUNTIME_DIR}/Combine" ${alchemy_swift_concurrency_libraries})
                add_custom_command(OUTPUT "${macos_dir}/swift_dispatch_timer.o"
                    COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                        -frontend -c -target arm64-apple-macosx15.0 -module-name swift_dispatch_timer -O -parse-as-library
                        -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${alchemy_swift_stdlib}" -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine"
                        -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays"
                        -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                        -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                        -Xcc "-I${CMAKE_SOURCE_DIR}/src/alchemy/darwin/swift/include"
                        -plugin-path "${alchemy_swift_bin}" -disable-implicit-string-processing-module-import
                        -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                        "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_dispatch_timer.swift" -o "${macos_dir}/swift_dispatch_timer.o"
                    DEPENDS tests/fixtures/macos/swift_dispatch_timer.swift ${alchemy_swift_dispatch_timer_libraries} VERBATIM)
                add_custom_command(OUTPUT "${macos_dir}/swift_dispatch_timer"
                    COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_dispatch_timer.o"
                        ${alchemy_swift_dispatch_timer_libraries} "${macos_dir}/link_system.dylib"
                        "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_dispatch_timer"
                    DEPENDS "${macos_dir}/swift_dispatch_timer.o" "${macos_dir}/link_system.dylib" VERBATIM)
                add_custom_target(alchemy_swift_dispatch_timer_fixture ALL DEPENDS "${macos_dir}/swift_dispatch_timer")
                add_dependencies(alchemy_swift_dispatch_timer_fixture alchemy_macos_fixtures)
                add_test(NAME alchemy_swift_dispatch_timer COMMAND "${CMAKE_COMMAND}"
                    "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_dispatch_timer"
                    "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
                    "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_dispatch_timer.expected"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
                set_tests_properties(alchemy_swift_dispatch_timer PROPERTIES TIMEOUT 120 ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1")
            endif()
        endif()
    endif()

    # The Metal overlay (libswiftMetal, metal-parity M8), once the runtime
    # directory has it: its array and Range forms on Alchemy's Metal provider.
    # Registered after the GUI fixtures (deferred like them), whose Metal
    # link stub the fixture links against for MTLCreateSystemDefaultDevice.
    function(alchemy_swift_metal_fixture)
        set(overlays "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/overlays")
        if(NOT TARGET alchemy_gui_link_stubs OR NOT EXISTS "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftMetal.dylib"
           OR NOT EXISTS "${overlays}/Metal.swiftmodule")
            return()
        endif()
        set(libraries "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftMetal.dylib" "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftFoundation.dylib"
            "${ALCHEMY_SWIFT_RUNTIME_DIR}/libswiftCore.dylib")
        add_custom_command(OUTPUT "${macos_dir}/swift_metal.o"
            COMMAND "${CMAKE_COMMAND}" -E env "${alchemy_swift_env}" "${alchemy_swift_bin}/swift-frontend.exe"
                -frontend -c -target arm64-apple-macosx11.0 -module-name swift_metal -O -parse-as-library
                -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/core" -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/stdlib"
                -I "${ALCHEMY_SWIFT_RUNTIME_DIR}/obj/combine" -I "${overlays}"
                -I "${ALCHEMY_SWIFT_SOURCE_DIR}/swift/stdlib/public/SwiftShims/swift/shims"
                -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/swift/include" -Xcc "-I${CMAKE_SOURCE_DIR}/runtime/include"
                -Xcc "-I${CMAKE_SOURCE_DIR}/src/alchemy/darwin/swift/include"
                -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import
                -runtime-compatibility-version none -module-cache-path "${CMAKE_CURRENT_BINARY_DIR}/swift-module-cache"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_metal.swift" -o "${macos_dir}/swift_metal.o"
            DEPENDS tests/fixtures/macos/swift_metal.swift ${libraries} VERBATIM)
        add_custom_command(OUTPUT "${macos_dir}/swift_metal"
            COMMAND "${ALCHEMY_LD64_LLD}" ${macos_link_options} -e _main "${macos_dir}/swift_metal.o" ${libraries}
                "${macos_dir}/link_metal.dylib" "${macos_dir}/link_objc.dylib" "${macos_dir}/link_system.dylib"
                "-L${ALCHEMY_LLVM_RUNTIME_DIR}" -undefined dynamic_lookup -o "${macos_dir}/swift_metal"
            DEPENDS "${macos_dir}/swift_metal.o" "${macos_dir}/link_system.dylib" VERBATIM)
        add_custom_target(alchemy_swift_metal_fixture ALL DEPENDS "${macos_dir}/swift_metal")
        add_dependencies(alchemy_swift_metal_fixture alchemy_gui_link_stubs alchemy_macos_fixtures)
        add_test(NAME alchemy_swift_metal COMMAND "${CMAKE_COMMAND}"
            "-DALCHEMY=$<TARGET_FILE:alchemy-fx>" "-DPROGRAM=${macos_dir}/swift_metal"
            "-DRUNTIME_DIRS=${ALCHEMY_LLVM_RUNTIME_DIR};${ALCHEMY_SWIFT_RUNTIME_DIR}"
            "-DEXPECTED=${CMAKE_SOURCE_DIR}/tests/fixtures/macos/swift_metal.expected"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/alchemy_swift_run.cmake")
        set_tests_properties(alchemy_swift_metal PROPERTIES TIMEOUT 120
            ENVIRONMENT "ALCHEMY_GUI_NO_ACTIVATE=1;ALCHEMY_GUI_CLOAK=1;ALCHEMY_GUI_NO_CRASH_DIALOG=1")
    endfunction()
    cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL alchemy_swift_metal_fixture)
endif()