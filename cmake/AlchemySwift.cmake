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
endif()
