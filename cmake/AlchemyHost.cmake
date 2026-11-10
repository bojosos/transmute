# Host OS layer (include/alchemy/host/, src/alchemy/host/): the macOS layer's
# only way to the operating system. One backend per build, chosen here:
#
#   ALCHEMY_HOST=win32   Windows (the default on Windows)
#   ALCHEMY_HOST=linux   Linux (docs/ALCHEMY_HOST_LINUX.md; in progress, so not
#                        yet the default off Windows)
#   ALCHEMY_HOST=null    every call fails with host::Error::unsupported; builds
#                        on any compiler and proves that no OS header leaks out
#                        of the backends (docs/ALCHEMY_HOST_LINUX.md)
#
# Sources, all globbed, so a new area file needs no edit here:
#   src/alchemy/host/common/*.cpp             every backend (network policy, ...)
#   src/alchemy/host/<backend>/*.cpp          the backend: alchemy_core (alchemy and alchemy-fx)
#   src/alchemy/host/<backend>/gui/*.cpp      areas only fx-only GUI providers use
#                                             (ui, audio, midi, gpu): ALCHEMY_GUI_SOURCES
# Included at the end of cmake/Alchemy.cmake, after AlchemyGui.cmake.
if(WIN32)
    set(alchemy_host_default win32)
else()
    set(alchemy_host_default null)
endif()
set(ALCHEMY_HOST "${alchemy_host_default}" CACHE STRING "Alchemy host OS backend (win32, linux, null)")
set_property(CACHE ALCHEMY_HOST PROPERTY STRINGS win32 linux null)
if(NOT IS_DIRECTORY "${PROJECT_SOURCE_DIR}/src/alchemy/host/${ALCHEMY_HOST}")
    message(FATAL_ERROR "ALCHEMY_HOST=${ALCHEMY_HOST}: no backend src/alchemy/host/${ALCHEMY_HOST}/")
endif()
message(STATUS "Alchemy host backend: ${ALCHEMY_HOST}")

file(GLOB alchemy_host_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/alchemy/host/common/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/alchemy/host/${ALCHEMY_HOST}/*.cpp")
target_sources(alchemy_core PRIVATE ${alchemy_host_sources})
string(TOUPPER "${ALCHEMY_HOST}" alchemy_host_upper)
target_compile_definitions(alchemy_core PUBLIC ALCHEMY_HOST_${alchemy_host_upper}=1)

# With the null backend the OS headers are poisoned for the macOS layer: a
# directory of same-named headers that #error comes first on alchemy_core's
# and alchemy_core_fx's include path (searched before the SDK), so a Windows
# null build fails on any OS include outside the backends. That, not merely
# compiling, is the boundary proof. The fx engine (src/fx) keeps its own host
# seams and is not poisoned.
set(ALCHEMY_HOST_POISON_DIRECTORY "")
if(ALCHEMY_HOST STREQUAL "null")
    set(ALCHEMY_HOST_POISON_DIRECTORY "${CMAKE_BINARY_DIR}/alchemy-host-poison")
    file(REMOVE_RECURSE "${ALCHEMY_HOST_POISON_DIRECTORY}")
    set(alchemy_host_poisoned windows.h winsock2.h ws2tcpip.h mswsock.h afunix.h winhttp.h wininet.h bcrypt.h ncrypt.h
                              wincrypt.h schannel.h sspi.h security.h mmdeviceapi.h audioclient.h functiondiscoverykeys_devpkey.h
                              mmsystem.h mmreg.h d2d1.h d2d1_1.h d2d1_1helper.h dwrite.h dwrite_3.h wincodec.h dwmapi.h
                              shlobj.h shellapi.h shlwapi.h shobjidl.h objbase.h combaseapi.h ole2.h oleauto.h propidl.h
                              propvarutil.h iphlpapi.h windns.h winternl.h winioctl.h psapi.h tlhelp32.h sddl.h aclapi.h
                              commctrl.h imm.h windowsx.h spellcheck.h realtimeapiset.h vulkan/vulkan_win32.h wrl/client.h
                              dlfcn.h sys/mman.h sys/socket.h netinet/in.h arpa/inet.h)
    # On POSIX hosts the C++ standard library includes <unistd.h> and
    # <pthread.h> itself (libstdc++'s gthr-posix.h), so only Windows poisons
    # them; elsewhere scripts/check_host_boundary.ps1's scan covers them.
    if(WIN32)
        list(APPEND alchemy_host_poisoned unistd.h pthread.h)
    endif()
    foreach(header IN LISTS alchemy_host_poisoned)
        file(WRITE "${ALCHEMY_HOST_POISON_DIRECTORY}/${header}"
            "#error \"<${header}> is an OS header: only src/alchemy/host/<backend>/ may include it (ALCHEMY_HOST=null boundary check)\"\n")
    endforeach()
    target_include_directories(alchemy_core BEFORE PRIVATE "${ALCHEMY_HOST_POISON_DIRECTORY}")
endif()

file(GLOB alchemy_host_gui_sources CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/alchemy/host/${ALCHEMY_HOST}/gui/*.cpp")
list(APPEND ALCHEMY_GUI_SOURCES ${alchemy_host_gui_sources})
# The Linux GUI areas speak X11 through XCB, with xkbcommon for keyboards.
if(ALCHEMY_HOST STREQUAL "linux" AND alchemy_host_gui_sources)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALCHEMY_X11 REQUIRED IMPORTED_TARGET xcb xcb-xkb xcb-cursor xcb-icccm xkbcommon xkbcommon-x11)
    list(APPEND ALCHEMY_GUI_LINK_LIBRARIES PkgConfig::ALCHEMY_X11)
endif()

# Linux areas crypto and http (fw-foundation): OpenSSL 3 (libcrypto, libssl) and libcurl.
if(ALCHEMY_HOST STREQUAL "linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALCHEMY_LINUX_NET REQUIRED IMPORTED_TARGET libcurl openssl)
    target_link_libraries(alchemy_core PRIVATE PkgConfig::ALCHEMY_LINUX_NET)
endif()

if(BUILD_TESTING)
    add_executable(alchemy_host_tests src/alchemy/host/tests/host_tests.cpp)
    target_link_libraries(alchemy_host_tests PRIVATE alchemy_core)
    add_test(NAME alchemy_host COMMAND alchemy_host_tests)
endif()

# The boundary: outside the backends (and fx's own seams, src/fx/host/) no
# source includes an OS header. scripts/check_host_boundary.ps1 lists any
# that do; the test fails while one is left that is not in the script's
# documented allowance.
if(BUILD_TESTING AND WIN32)
    find_program(alchemy_host_powershell NAMES powershell pwsh)
    if(alchemy_host_powershell)
        add_test(NAME alchemy_host_boundary
            COMMAND "${alchemy_host_powershell}" -NoProfile -ExecutionPolicy Bypass
                -File "${PROJECT_SOURCE_DIR}/scripts/check_host_boundary.ps1" -Root "${PROJECT_SOURCE_DIR}")
    endif()
endif()

# fw-ui: the Linux canvas, fonts and images. Fonts are FreeType faces found
# through fontconfig; text layout is Pango's (HarfBuzz shaping, FriBidi,
# fontconfig fallback) over the same fontconfig configuration.
if(ALCHEMY_HOST STREQUAL "linux" AND alchemy_host_gui_sources)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALCHEMY_LINUX_TEXT REQUIRED IMPORTED_TARGET freetype2 fontconfig harfbuzz pangoft2)
    list(APPEND ALCHEMY_GUI_LINK_LIBRARIES PkgConfig::ALCHEMY_LINUX_TEXT)
endif()

# The Linux media areas (src/alchemy/host/linux/gui/audio.cpp, midi.cpp): PulseAudio's client library for audio
# (PipeWire and WSLg serve it too) and ALSA's sequencer for MIDI.
if(ALCHEMY_HOST STREQUAL "linux" AND alchemy_host_gui_sources)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ALCHEMY_MEDIA REQUIRED IMPORTED_TARGET libpulse alsa)
    list(APPEND ALCHEMY_GUI_LINK_LIBRARIES PkgConfig::ALCHEMY_MEDIA)
endif()
