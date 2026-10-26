# libFLAC (vendored)

| Field | Value |
| --- | --- |
| Project | libFLAC, the reference FLAC library (not the command-line tools) |
| Upstream | <https://xiph.org/flac/>, <https://github.com/xiph/flac> |
| Tag | `1.5.0` (release tarball) |
| Archive | <https://downloads.xiph.org/releases/flac/flac-1.5.0.tar.xz>, SHA-256 `f2c1c76592a82ffff8413ba3c4a1299b6c7ab06c734dee03fd88630485c2b920` (1,078,872 bytes; matches Xiph's `SHA256SUMS.txt`) |
| License | BSD-3-Clause (`COPYING.Xiph`, copied verbatim; libFLAC's licence). The GPL-licensed tools of the same tarball are not vendored. |
| Files | `COPYING.Xiph`; the library sources `src/libFLAC/CMakeLists.txt` lists, without the x86/ARM intrinsics variants and without Ogg, with the two generic files they include from `src/libFLAC/deduplication/`; `src/share/win_utf8_io/win_utf8_io.c` (part of the Windows library build); the headers of `include/FLAC/`, `include/share/` and `src/libFLAC/include/`. All are byte-identical to the archive above (`.gitattributes` keeps them out of end-of-line conversion) |
| Modifications | none |

Use: FLAC decoding (and, in the tests, encoding) for `src/alchemy/darwin/media/media_codecs.cpp`. `cmake/darwin/media_codecs.cmake` builds the static library `alchemy_flac` with a `config.h` it writes into the build tree (no Ogg, no intrinsics, little endian).

To update: download the new release from the upstream above, check its published hash, copy the same set of files with the vendoring rules above, and update this file, the hashes and `THIRD_PARTY_NOTICES.md`.

SHA-256 of the vendored files:

```
7866ee98760fc1f0156b4fe6bf530257e02be487ab3fd94e2b63799dd32d6b2c  COPYING.Xiph
ff88842fac3d3a79223df3cb2f80af926cd73cbeb32fb925e82d26c6ca65d9eb  include/FLAC/all.h
f819c5f88e7b6ea1efbcf5476faf4042db65915b519231449b7895e5d16ac539  include/FLAC/assert.h
72340f12ab7197b0492eb7ce0b3d465e6ac359e976dcd222759034f82e56a992  include/FLAC/callback.h
0020243b53cafd9a9f7f5185c02984e450b74d9318a02e717f8071cebb240a26  include/FLAC/export.h
7311a9c8e5be7aa44b3f48c0d98c96a6fde937a009ac9d4311d82f01b1b6d512  include/FLAC/format.h
ed54ca45d24c4e3d18a8ba867ef7e66d7ed510a3b87f3cb1a14d0efb517bce8f  include/FLAC/metadata.h
206c45b8573fd50fec359028e4d69c626a5dfbeb75a53027195704d5f9a60c63  include/FLAC/ordinals.h
375f08a6983ce0f75a00169b114d753965d0176fd95c620ecf265244e61be5f3  include/FLAC/stream_decoder.h
5aedffc9654871ed89055155b92ce3e784f3d059837b6d4d972a489d85f8dbb7  include/FLAC/stream_encoder.h
93498f69a58e9f6e34b5addc421ea6c88aadf117937cf746badd3f9f8fb3e23c  include/share/alloc.h
08bb2ab6f787f715c5a3c0da4ce51fa9bfa1c16bc6a33cf7856e53229de0b60a  include/share/compat.h
4238d57dbff1b4ec37ef6a2f188b77c12852ac9c9984d11343c9f3dc2eb1dac4  include/share/endswap.h
de17039a52cc664b852f280e1e3dea1e6398314c55f1602ac655c4e358df578e  include/share/getopt.h
62e2f9f382f3477526fa062e6d70cdb17eb71df677b4d7bd10ac0d8d704d141c  include/share/grabbag.h
16c106203e413d2f76f8aa304dc107ad6d541e1f6adbd0166d64111c820a4b92  include/share/macros.h
a3d488217bf6b8ad3a0f7b7f8281ed6288210508903add1174b1f840ca84df97  include/share/private.h
ec259a43ab698d4fbc9a568bd7bd9dba72b94073db4639a083badc6dfd7f74cd  include/share/replaygain_analysis.h
98f55316f165888da9ee2216d477ab386f002cc4baac2da7fbbacd2d06da89d1  include/share/replaygain_synthesis.h
50c06cf2861b7bac12df4e5865e2f49c61f6e99aff309de6779529fc202a5429  include/share/safe_str.h
b9668c8a20a2bff212945d705fbad09e0f68a84ad507b6f6111a6f106706f7c3  include/share/utf8.h
15fc0a9d5e8e9424c6d2c1f75b19172596b651aba62ffa366462a0566c51e0f6  include/share/win_utf8_io.h
2828549b728a53b95057648bf47493ea42a5ff09bcc6aa15f99058b67c661abe  src/libFLAC/bitmath.c
02c05b82e9602e361e6ca361781767d208b65f01730829e2f71948fa3169dd6d  src/libFLAC/bitreader.c
23758977a51e73c3ce88e45837bb889849e161bb950f2b7659a893ce38ea5672  src/libFLAC/bitwriter.c
eb55848f2804923a21fba28e41ffd4ce0b27e5257706d7fcd8e0e88cbcb1dcba  src/libFLAC/cpu.c
f68c52d4ef1ba4a554cac155eb056acca9b86f96059fbbf763da773ac223597e  src/libFLAC/crc.c
abaf9793452f60bfa91e6a7e94b2d3f0efbbb81ff683eb76f6649814bf8c8cf8  src/libFLAC/deduplication/bitreader_read_rice_signed_block.c
2dfd5b204c82b81f75ca31cda4465d42e4a4299105a6fed5f4cf26d07e1a14ea  src/libFLAC/deduplication/lpc_compute_autocorrelation_intrin.c
af00d6e403ba9462012505a6cefeb4539922ff38b8eadf91ec55a0b39b2f172d  src/libFLAC/fixed.c
3993091322789cfaff236b5d3aaa90608fcaadd354a59e8799cc568d9f682f50  src/libFLAC/float.c
3ce4418dab19436e83d1aa622a324241916e88c5829b5b9afb48352c6ad0fa51  src/libFLAC/format.c
d3b79ee02d5274717c08286b228421603159814d5dafc99f196c1de667d3b475  src/libFLAC/include/private/all.h
687511ca27090c1ff7ff080883a1f629a5e45d732ac2ab596db05e433608bd8f  src/libFLAC/include/private/bitmath.h
425fb01e1383d38e19b0c8cd3687292e58c76c2ba447ed18a1bc996f42e5100f  src/libFLAC/include/private/bitreader.h
2ea5dfdc6f1bb2d614dbe93549959635c2d0f60e8a6142bd924dd8e030a1505a  src/libFLAC/include/private/bitwriter.h
623b765b9cfa178ad49e862e0c07ad45825fc78a2ab8b38d25dce425f74b57f1  src/libFLAC/include/private/cpu.h
bb1a9af109fc678bfdfcb60d28425d2c7fd9b6472589c7026c69755931c38bc7  src/libFLAC/include/private/crc.h
0d1f62c4bdd6ba7652833e1cdd9302f7876d8adfc888335d3cc09cd87394cd3a  src/libFLAC/include/private/fixed.h
8c208a330ccaa8af64c223bfa9cbebb1fdf405344daa6e9c4c33b43b35e27e83  src/libFLAC/include/private/float.h
8584f6f273e035b4ecbd71498fad020363f5237b8deb539b002b799851953e6d  src/libFLAC/include/private/format.h
15dd96d33cca0a271e05cd4f0c414a27a8493a5d0bfcc7be4464f678614baba1  src/libFLAC/include/private/lpc.h
c65bf4aa24b94a9a9b3ba677ab5b7f5cf1701da95c4fb8dae5cde565b9480e6a  src/libFLAC/include/private/macros.h
12a739cb6b87c4ebcd77b4af2ccd9c9d7297b1d912f88bf968b3bcbaeca60cc0  src/libFLAC/include/private/md5.h
c551e0a46bd7f3853cce550ba4a84cac457a0179d42e170c8ff63e7ae046c5f9  src/libFLAC/include/private/memory.h
50d2764764f319033f510a7c02715a822799db6634a4ebbfca9facc91d694660  src/libFLAC/include/private/metadata.h
6815392f22328e5a66245dbcd5826d880026062e38e1260528a75f70229b3315  src/libFLAC/include/private/ogg_decoder_aspect.h
272794b33f1d7b3f21d81e314c68bdb5ac84bbf2ecaeea29a48198ceb257b7fc  src/libFLAC/include/private/ogg_encoder_aspect.h
d214802cc3f1187ed897e6f2e10c9ddc9810d7a4e7b8c9bd41c28c01a0e16ee8  src/libFLAC/include/private/ogg_helper.h
fc2d5b292ea5d67c4ac4c98a79ae228e887812ab0f516ecc7ff3246be813aa76  src/libFLAC/include/private/ogg_mapping.h
052ec5ef25220a26317eaebeaa649d5a0715a1819eb87b5057a7bd08ef6cd51a  src/libFLAC/include/private/stream_encoder.h
8ffcceef1fad0e45844df047fb45962738a7b4544d1d4a3af041257f52614262  src/libFLAC/include/private/stream_encoder_framing.h
741b0354ce8ea9959360f656625defd000d6944e4b6f80e2a3afa8e7df30997c  src/libFLAC/include/private/window.h
fd541cf8da8df9b2a9abe60136cbab6ca5b034c2b14505af706bc8f8d7fbacd1  src/libFLAC/include/protected/all.h
03427a7e84cab8d5c78da1d2c88824c415a145f30eb785a38372fcb1e2792d26  src/libFLAC/include/protected/stream_decoder.h
ed23d33ccaf3901c984eee97c1cc8d4d4d75d692cfa6a1e5e86b18a0c8736e3d  src/libFLAC/include/protected/stream_encoder.h
7f900f142b3567495147a1a68777b563fbe240186aabdd93c7ec237093895d91  src/libFLAC/lpc.c
c4204332491e5c4e58052a6f565c8f25937ddf38a094a8f5b8ddbf413a351e06  src/libFLAC/md5.c
eb185f7b5e14f6552ee65fb98b6a642eaf0663a84cec4887ff6c2ad94b1491ce  src/libFLAC/memory.c
6ad261c56e0ce200c60d06320d0b96e1da7af086cc4244c38b13ac3a5d85f5f3  src/libFLAC/metadata_iterators.c
a7c43083bf1f46873e1621fbfb2389cfff2ead5a663ede3e93d1ddb1cb77a2a2  src/libFLAC/metadata_object.c
9a075fbc07ebc19bac6e02bb7e3e1b1e4ca847485e15430d327c6c7130fd1723  src/libFLAC/stream_decoder.c
ff11a135d3c846fbf5c67be917f9779056967f5402f64e80ff66de135512d963  src/libFLAC/stream_encoder.c
ac0802c57400829473239aae8e8fc5ff22f8181144ad03862a613e5728e4addf  src/libFLAC/stream_encoder_framing.c
2ea68aaecab5ee0d228140e798d6c75259e97fd97f40b444ec80d42dfb4c8489  src/libFLAC/window.c
956b698835073a8a9c73fb3e133a531211fadfe09859c71ebd8c36c365a6b7c3  src/share/win_utf8_io/win_utf8_io.c
```
