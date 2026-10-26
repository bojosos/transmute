# Compression (src/alchemy/darwin/compression/; owner: refactor group r1):
# libz, libbz2, libcompression and xar. See cmake/AlchemyDarwin.cmake for the
# fragment pattern.

# GUI (alchemy-fx only):
#   install.cpp: compression's rows of the GUI installer table (install.hpp).
#   zlib_*.cpp: libz over the project's DEFLATE codec, fill-only
#     (compression::install_zlib's row; shared kit src/alchemy/darwin/kit/export_fill/).
#   {compression,bzip2,xar}.cpp: the libcompression (zlib, LZ4), bzip2 and xar
#     cores. compression_provider.cpp: libcompression (installed by
#     kit/completion/data/install.cpp, shared kit). bzip2_stream.cpp: libbz2's
#     streaming decompressor (installed by fill::install_libsystem_leftovers).
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/compression/install.cpp
    src/alchemy/darwin/compression/zlib_codec.cpp
    src/alchemy/darwin/compression/zlib_provider.cpp
    src/alchemy/darwin/compression/zlib_stream.cpp
    src/alchemy/darwin/compression/zlib_files.cpp
    src/alchemy/darwin/compression/compression.cpp
    src/alchemy/darwin/compression/bzip2.cpp
    src/alchemy/darwin/compression/bzip2_stream.cpp
    src/alchemy/darwin/compression/xar.cpp
    src/alchemy/darwin/compression/compression_provider.cpp)

function(alchemy_gui_compression_tests)
    # libz: codec vectors and the guest exports (split from the export-fill kit's tests).
    alchemy_gui_add_unit_test(alchemy_gui_zlib src/alchemy/darwin/compression/tests/zlib_tests.cpp TIMEOUT 120)
    # Host-only cores: libcompression, bzip2, xar (split from the QLab data tests).
    alchemy_gui_add_unit_test(alchemy_gui_compression_unit src/alchemy/darwin/compression/tests/compression_tests.cpp)
endfunction()
alchemy_gui_register_tests(alchemy_gui_compression_tests)
