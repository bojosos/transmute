# minimp3 (vendored)

| Field | Value |
| --- | --- |
| Project | minimp3, a minimalistic MPEG audio layer I/II/III decoder in one header |
| Upstream | <https://github.com/lieff/minimp3> |
| Tag | none (the project has no release tags); the commit below, the head of `master` when vendored, 2026-10-26 |
| Commit | `ea99364f61c14656440e8d77e9c233ccf3124633` |
| Archive | <https://github.com/lieff/minimp3/archive/ea99364f61c14656440e8d77e9c233ccf3124633.tar.gz>, SHA-256 `5628166eb82a9bb581317918a334c317a2c0a30278bb14a20381307976768f34` (19,744,692 bytes; GitHub generates it, so the hash is ours) |
| License | CC0-1.0 (`LICENSE`, copied verbatim) |
| Files | `minimp3.h`, `LICENSE`, and three of the project's own conformance vectors with their reference output (`vectors/l1-fl4`, `l2-fl13`, `l3-hecommon`, `.bit` and `.pcm`) for the tests, byte-identical to the archive above (`.gitattributes` keeps them out of end-of-line conversion) |
| Modifications | none |

Use: MPEG audio decoding behind AudioConverter, AudioFile and AVFoundation (`src/alchemy/darwin/media/media_codecs.cpp`). `cmake/darwin/media_codecs.cmake` compiles the implementation (`MINIMP3_IMPLEMENTATION`, float output, no SIMD) as the static library `alchemy_minimp3`. The vectors are read by `alchemy_gui_media_codecs`.

To update: download the new release from the upstream above, check its published hash, copy the same set of files with the vendoring rules above, and update this file, the hashes and `THIRD_PARTY_NOTICES.md`.

SHA-256 of the vendored files:

```
6a1ee543e5282cd9061881edf462e6fdab181f328da71fc2c9a6950a80e94d01  LICENSE
57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad  minimp3.h
9ad3be8edffd7563cc4b9dd1598ac66d0dbf6905f3e24784e07548c6088d971b  vectors/l1-fl4.bit
b536704b47e22ebe9c64bf93e5fe3b85ace28dfd126828885b70e9c04671e5e1  vectors/l1-fl4.pcm
833af5ebf15e83fdd902f0d1b77286b177dba08cfac31cae0dd81270e01b7ce0  vectors/l2-fl13.bit
a358ac3867fef3303eb4793822498e1252b19c0b7917cac343e98b7f6a5f2503  vectors/l2-fl13.pcm
21999b77716aeba047c7f55c2c1ce17b54c780b6dc007912f251b7ea189676ef  vectors/l3-hecommon.bit
5af4952d4451dc977b1344f836975e7545a66e38425de1764f3c28994cc38397  vectors/l3-hecommon.pcm
```
