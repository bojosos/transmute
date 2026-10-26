# Media decoders (third_party/, vendored with licence, tag and per-file hashes in
# each PROVENANCE.md), built as plain C without assembly or intrinsics and
# linked into alchemy-fx for src/alchemy/darwin/media/media_codecs.cpp:
#   minimp3 (CC0) for MPEG audio, Opus (BSD-3), libFLAC (BSD-3).
# Upstream code keeps its own warnings; it is compiled with them off.
enable_language(C)

set(alchemy_codec_root "${PROJECT_SOURCE_DIR}/third_party")
set(alchemy_codec_gen "${CMAKE_CURRENT_BINARY_DIR}/alchemy_codecs")

function(alchemy_codec_library name)
    add_library(${name} STATIC ${ARGN})
    set_target_properties(${name} PROPERTIES C_STANDARD 11 FOLDER third_party)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W0 /utf-8)
    else()
        target_compile_options(${name} PRIVATE -w)
    endif()
endfunction()

# ---- minimp3: the header holds the implementation ----------------------------
file(WRITE "${alchemy_codec_gen}/minimp3_impl.c"
    "#define MINIMP3_IMPLEMENTATION\n#define MINIMP3_FLOAT_OUTPUT\n#define MINIMP3_NO_SIMD\n#include \"minimp3.h\"\n")
alchemy_codec_library(alchemy_minimp3 "${alchemy_codec_gen}/minimp3_impl.c")
target_include_directories(alchemy_minimp3 PUBLIC "${alchemy_codec_root}/minimp3")

# ---- Opus: celt, silk (float) and the Opus layer, from the upstream .mk lists --
set(alchemy_opus "${alchemy_codec_root}/opus")
file(GLOB alchemy_opus_sources CONFIGURE_DEPENDS
    "${alchemy_opus}/celt/*.c" "${alchemy_opus}/silk/*.c" "${alchemy_opus}/silk/float/*.c"
    "${alchemy_opus}/src/*.c")
alchemy_codec_library(alchemy_opus ${alchemy_opus_sources})
target_include_directories(alchemy_opus PUBLIC "${alchemy_opus}/include"
    PRIVATE "${alchemy_opus}" "${alchemy_opus}/celt" "${alchemy_opus}/silk" "${alchemy_opus}/silk/float")
target_compile_definitions(alchemy_opus PRIVATE OPUS_BUILD USE_ALLOCA)

# ---- libFLAC: the library only (not the GPL tools), without Ogg ---------------
set(alchemy_flac "${alchemy_codec_root}/flac")
file(WRITE "${alchemy_codec_gen}/flac/config.h" [=[
#define CPU_IS_BIG_ENDIAN 0
#define CPU_IS_LITTLE_ENDIAN 1
#define ENABLE_64_BIT_WORDS 1
#define FLAC__HAS_OGG 0
#define FLAC__HAS_X86INTRIN 0
#define FLAC__HAS_NEONINTRIN 0
#define FLAC__HAS_A64NEONINTRIN 0
#define HAVE_LROUND 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define PACKAGE_VERSION "1.5.0"
]=])
file(GLOB alchemy_flac_sources CONFIGURE_DEPENDS "${alchemy_flac}/src/libFLAC/*.c")
if(WIN32)
    list(APPEND alchemy_flac_sources "${alchemy_flac}/src/share/win_utf8_io/win_utf8_io.c")
endif()
alchemy_codec_library(alchemy_flac ${alchemy_flac_sources})
target_include_directories(alchemy_flac PUBLIC "${alchemy_flac}/include"
    PRIVATE "${alchemy_codec_gen}/flac" "${alchemy_flac}/src/libFLAC/include")
target_compile_definitions(alchemy_flac PUBLIC FLAC__NO_DLL PRIVATE HAVE_CONFIG_H)

# ---- libvpx: the VP8 and VP9 decoders, generic C, high bit depth -------------
# config/ holds what libvpx's configure writes for this configuration, and the
# source list its makefiles select (third_party/libvpx/PROVENANCE.md).
set(alchemy_vpx "${alchemy_codec_root}/libvpx")
file(STRINGS "${alchemy_vpx}/config/sources.txt" alchemy_vpx_listed)
set(alchemy_vpx_sources "${alchemy_vpx}/config/vpx_config.c")
foreach(source IN LISTS alchemy_vpx_listed)
    list(APPEND alchemy_vpx_sources "${alchemy_vpx}/${source}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${alchemy_vpx}/config/sources.txt")
alchemy_codec_library(alchemy_vpx ${alchemy_vpx_sources})
target_include_directories(alchemy_vpx PUBLIC "${alchemy_vpx}" PRIVATE "${alchemy_vpx}/config")
if(MSVC)
    target_compile_definitions(alchemy_vpx PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# ---- dav1d: the AV1 decoder, C only, 8 and 16 bits per component ------------
# The *_tmpl.c sources compile once per bit depth, as dav1d's meson build does.
set(alchemy_dav1d "${alchemy_codec_root}/dav1d")
file(WRITE "${alchemy_codec_gen}/dav1d/config.h" [=[
#define ARCH_AARCH64 0
#define ARCH_ARM 0
#define ARCH_LOONGARCH 0
#define ARCH_LOONGARCH32 0
#define ARCH_LOONGARCH64 0
#define ARCH_PPC64LE 0
#define ARCH_RISCV 0
#define ARCH_RV32 0
#define ARCH_RV64 0
#define ARCH_X86 0
#define ARCH_X86_32 0
#define ARCH_X86_64 0
#define CONFIG_8BPC 1
#define CONFIG_16BPC 1
#define CONFIG_LOG 0
#define ENDIANNESS_BIG 0
#define HAVE_ASM 0
#define TRIM_DSP_FUNCTIONS 0
#define HAVE_ALIGNED_ALLOC 0
#define HAVE_POSIX_MEMALIGN 0
#define HAVE_MEMALIGN 0
#define HAVE_CLOCK_GETTIME 0
#define HAVE_SIGACTION 0
#define HAVE_DLSYM 0
#define HAVE_UNISTD_H 0
#define HAVE_IO_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_PTHREAD_NP_H 0
#define HAVE_GETAUXVAL 0
#define HAVE_ELF_AUX_INFO 0
#define HAVE_PTHREAD_GETAFFINITY_NP 0
#define HAVE_PTHREAD_SETAFFINITY_NP 0
#define HAVE_PTHREAD_SETNAME_NP 0
#define HAVE_PTHREAD_SET_NAME_NP 0
#define HAVE_C11_GENERIC 1
#ifdef _WIN32
#define _WIN32_WINNT 0x0601
#define UNICODE 1
#define _UNICODE 1
#define _CRT_DECLARE_NONSTDC_NAMES 1
#endif
]=])
file(WRITE "${alchemy_codec_gen}/dav1d/vcs_version.h" "#define DAV1D_VERSION \"1.5.4\"\n")
file(STRINGS "${alchemy_dav1d}/config/sources.txt" alchemy_dav1d_listed)
file(STRINGS "${alchemy_dav1d}/config/tmpl_sources.txt" alchemy_dav1d_tmpl_listed)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${alchemy_dav1d}/config/sources.txt" "${alchemy_dav1d}/config/tmpl_sources.txt")
list(TRANSFORM alchemy_dav1d_listed PREPEND "${alchemy_dav1d}/")
list(TRANSFORM alchemy_dav1d_tmpl_listed PREPEND "${alchemy_dav1d}/")
if(NOT WIN32)
    list(FILTER alchemy_dav1d_listed EXCLUDE REGEX "/win32/")
endif()
set(alchemy_dav1d_includes "${alchemy_codec_gen}/dav1d" "${alchemy_dav1d}" "${alchemy_dav1d}/include")
if(MSVC)
    list(APPEND alchemy_dav1d_includes "${alchemy_dav1d}/include/compat/msvc")
endif()
foreach(depth 8 16)
    alchemy_codec_library(alchemy_dav1d_${depth} ${alchemy_dav1d_tmpl_listed})
    target_include_directories(alchemy_dav1d_${depth} PRIVATE ${alchemy_dav1d_includes})
    target_compile_definitions(alchemy_dav1d_${depth} PRIVATE BITDEPTH=${depth})
endforeach()
alchemy_codec_library(alchemy_dav1d ${alchemy_dav1d_listed})
target_include_directories(alchemy_dav1d PUBLIC "${alchemy_dav1d}/include" PRIVATE ${alchemy_dav1d_includes})
target_link_libraries(alchemy_dav1d PUBLIC alchemy_dav1d_8 alchemy_dav1d_16)

list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/media/media_codecs.cpp src/alchemy/darwin/media/audio_stream.cpp
    src/alchemy/darwin/media/video_frames.cpp src/alchemy/darwin/media/vt_decompression.cpp)
list(APPEND ALCHEMY_GUI_LINK_LIBRARIES alchemy_minimp3 alchemy_opus alchemy_flac alchemy_vpx alchemy_dav1d)

# Decode-and-compare tests: the conformance and test vectors of minimp3, libvpx
# and dav1d (vendored beside them), and Opus and FLAC signals encoded by those
# libraries' own encoders inside the test.
function(alchemy_gui_media_codec_tests)
    alchemy_gui_add_unit_test(alchemy_gui_media_codecs src/alchemy/darwin/media/tests/media_codec_tests.cpp)
    target_link_libraries(alchemy_gui_media_codecs PRIVATE alchemy_opus alchemy_flac)
    target_compile_definitions(alchemy_gui_media_codecs PRIVATE
        ALCHEMY_THIRD_PARTY="${PROJECT_SOURCE_DIR}/third_party")
endfunction()
alchemy_gui_register_tests(alchemy_gui_media_codec_tests)
