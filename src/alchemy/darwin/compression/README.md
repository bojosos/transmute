# Compression

libz (`zlib_*.cpp`, over the project's DEFLATE codec), libcompression
(`compression.cpp`, `compression_provider.cpp`: zlib and LZ4), bzip2
decoding (`bzip2.cpp`) and xar archives (`xar.cpp`). All alchemy-fx (GUI).

* **Host dependencies:** none (pure computation).
* **Unsupported:** libcompression's LZFSE, LZMA, Brotli and LZBITMAP (the
  documented failure results); bzip2 encoding beyond what the decoder's
  callers need.
* **Tests:** the shared `kit/export_fill/tests` and `kit/completion/data/tests` until
  their cases move to `tests/` here.
