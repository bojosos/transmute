# Apple Lossless Audio Codec (vendored)

| Field | Value |
| --- | --- |
| Project | Apple's open-source Apple Lossless Audio Codec (ALAC): the encoder and decoder library |
| Upstream | <https://github.com/macosforge/alac> |
| Commit | `c38887c5c5e64a4b31108733bd79ca9b2496d987` (2016-05-11; `codec/` is unchanged since the initial check-in of 2011-10-26) |
| License | Apache-2.0 (`LICENSE`, copied verbatim; every vendored source carries Apple's Apache-2.0 header). Upstream has no `NOTICE` file. `codec/APPLE_LICENSE.txt` (APSL 2.0) is not vendored: no vendored file is under it |
| Files | `LICENSE` and the 18 sources and headers of `codec/` that the upstream `codec/makefile` builds into `libalac`, taken from the commit's blobs (LF line endings; `.gitattributes` keeps them out of end-of-line conversion). The `convert-utility/` sample program is not vendored |
| Modifications | `codec/ALACDecoder.cpp` only, marked by a notice at its top (Apache-2.0, section 4(b)): `Decode()` rejects a partial frame whose sample count exceeds the configured frame length (`kALAC_ParamError`), in the mono and the stereo element, instead of decoding past its buffers. Upstream SHA-256 of that file: `b071c3a82bcb0b5e8ad704cafb514aaeba5b0869b7dcc83fa79561bae994c019` |

Use: ALAC decoding and encoding for `src/alchemy/darwin/media/media_codecs.cpp`, built by `cmake/darwin/media_codecs.cmake` as the static library `alchemy_alac`. The build defines `TARGET_OS_WIN32` so that `EndianPortable.c` byte-swaps on little-endian Windows (it tests only `__i386__`, `__x86_64__` and `TARGET_OS_WIN32`). Alchemy's wrapper validates the magic cookie (frame length 1-16384, 1-8 channels, 16/20/24/32 bits, compatible version 0) before `ALACDecoder::Init` and decodes each packet from a copy followed by 256 KiB of zeros, since the bit reader does not stop at the packet's end.

To update: clone the upstream above, check the licence, run the vendoring script on the new commit (the same file list), re-apply the modification if upstream has not fixed it, and update this file, the hashes and `THIRD_PARTY_NOTICES.md`.

SHA-256 of the vendored files:

```
a337314dd82d26791112c1e5a85725b2eb92f5b7e6f9102fa4eaa2903f77518f  LICENSE
a0fe17bf82191d28c500b9ee1e773d49700df5e33ad862ed61b13c8a094c03b9  codec/ALACAudioTypes.h
276ac7d7f2d240a4b0437d7e222afcba4c78bd8fd8104d0e9c6b0d45f67abcfe  codec/ALACBitUtilities.c
18d1c8f957a403cc5b9b7d99e9d3987001adeac6ceadb4a4b1b5dc1ea4bff8ca  codec/ALACBitUtilities.h
835c2372a6447f3a36fa1550190475e28718bf15fde82cb83bd25de3f2359fb3  codec/ALACDecoder.cpp
779a1686417687f3ba107f9a12bb0f4c892885c0654d0bd5db7133bc4edfe24c  codec/ALACDecoder.h
0328473c489d24801eab29de1d2785693a5017a87455a59248c26fd2a9d006bf  codec/ALACEncoder.cpp
e2a581c46c9233ac2eac30cfedfc683245fbb28abe879bcceb1fc21c989be58b  codec/ALACEncoder.h
34b4d4c4dd599e05ac43f4a58eef61ef2eb5a7093e46209eebdf6ac7024b3dfc  codec/EndianPortable.c
5c3b1df02e8bef3adb4feee6f0ed0684b2d000d44e57a0c2ed754668794d40b0  codec/EndianPortable.h
622d7551ef041d82fa6462292155c48fcc2d8b912ed0241e202b86afa3536747  codec/ag_dec.c
3be76326402060d4dbcb16141340ff8e0d67877792ebc4e56155da840c75880d  codec/ag_enc.c
ef715d64666e4a4faaa0fafcd5050bd5b7f8c45e9760382735f37cf183c55778  codec/aglib.h
1f8e8b25e61c5b667ad729a9c33e3916df7eb4883125dcac7266d719cbf23516  codec/dp_dec.c
140aaac67123e7db08bab2aedc3d38933899c91530f5cc410283e92a1e9631a9  codec/dp_enc.c
fd2b9806f8cf2386dcbdc919ddf56c18608ce1b0812a74b6b9d0b0eb9df67d78  codec/dplib.h
ae9dd8abc1a9d1cf7f6256c0b15ffd72f9d51c6ffdae9b833f6e96d9d8a03495  codec/matrix_dec.c
c7fcb3ff04b6830a046677155633a9e44b5e4f54f6e1c03713eeaed34efa80b5  codec/matrix_enc.c
00839b4672b4ae7887a4d0448d6258165753b1e5f85f84c53c67e7e7b8b9d76d  codec/matrixlib.h
```
