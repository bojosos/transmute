# libxslt and libexslt (vendored)

| Field | Value |
| --- | --- |
| Project | libxslt, the GNOME XSLT C library, and libexslt, its EXSLT extensions |
| Upstream | <https://gitlab.gnome.org/GNOME/libxslt> |
| Version | `1.1.45` (release tarball; libexslt 0.8.25) |
| Archive | <https://download.gnome.org/sources/libxslt/1.1/libxslt-1.1.45.tar.xz>, SHA-256 `9acfe68419c4d06a45c550321b3212762d92f41465062ca4ea19e632ee5d216e` (1,519,992 bytes; matches GNOME's `libxslt-1.1.45.sha256sum`) |
| License | MIT (`Copyright`, copied verbatim: one notice for libxslt, one for libexslt) |
| Files | `Copyright`; the library sources of `libxslt/` and `libexslt/` that the upstream `CMakeLists.txt` lists, with every header they include; all public headers of both directories and the `xsltconfig.h.in` and `exsltconfig.h.in` templates. No tests, programs (xsltproc), Python bindings or build files. All are byte-identical to the archive above (`.gitattributes` keeps them out of end-of-line conversion) |
| Modifications | none |

Use: Darwin's `/usr/lib/libxslt.1.dylib` and `/usr/lib/libexslt.0.dylib` for guest programs. `scripts/build_alchemy_libxslt.ps1` compiles these sources against `runtime/include` (no SDK), the configuration header in `runtime/libxslt/` and the public headers of the libxml2 runtime build, and links two ARM64 Mach-O guest runtime libraries.

Feature set (upstream CMake option names): on: PROFILER, THREADS. Off: XSLT_DEBUG, DEBUGGER, MODULES, CRYPTO (it needs libgcrypt), TRIO.

To update: download the new release from the upstream above, check its published hash, copy the same set of files, and update this file, the hashes and `THIRD_PARTY_NOTICES.md`.

SHA-256 of the vendored files:

```
7e48e290b6bfccc2ec1b297023a1d77f2fd87417f71fbb9f50aabef40a851819  Copyright
45781794b5d6593dbd6638e5455a7e6d70f55dc4e3f3db674e8b6b851c434c34  libexslt/common.c
899d6f7cd1d0c08a40713bb4c99b2bb1f44f8f7ec769d0e91d93f286a63cde67  libexslt/crypto.c
d9b5b165bc81f03e6bfc03663c9fc46f82b2cf2cc61b6991127e44eb67c7b519  libexslt/date.c
e463f9f0bfc5157eb7d4dd24ebdd768a5e020acc345db814a0658ae48c14e144  libexslt/dynamic.c
42760b9a5d26e8e7f175bcf58db20d365ea0900be952e29a8f6b5963c7ff0e67  libexslt/exslt.c
7925b9b4c2407b049400d2ea93b00612253c6f605b08d4726ae1e77b3ee72925  libexslt/exslt.h
b87c78027d8d896e75705a69c1557fd6226fead238e8e1d698f8c0df971fc618  libexslt/exsltconfig.h.in
d499bd2935e6d8551424867a57af94c39e721b404c50b5f7fe0a32c437762cbf  libexslt/exsltexports.h
a48a66254036ac5589aed5afeefd69362d14d2287d209ad81ac4d8b67da094c7  libexslt/functions.c
7dc6ee51ec4309f9717399606470a813d7923fe9537e8561f8f7fdba34054c96  libexslt/libexslt.h
c6652c96ea7a7afd3d1638794e9b042276135b1ffe378e1c284a20c10560b1e1  libexslt/math.c
5bdc117776ff7c94859f43db28e2ec527191190fd98c7395f245658916404ae0  libexslt/saxon.c
9c105d047f8961deed22ab40f993c978bcc03444197e7f017413ce6048b85f27  libexslt/sets.c
f5a2984825b92addb18a3551b781a11f46ac4adaaa69bc805e994cf82e399f91  libexslt/strings.c
461a22088c4793a12619ec9f48709b9f4f1db69dd256e00c3431290fa367a9cc  libxslt/attributes.c
a8ac337c67fbafcf5eb0b0bae6cf7a898256440facf84cecb36fd5e8c9a8d619  libxslt/attributes.h
e7f75c2d59129ecacf4d46f49593d7f2cd22e44d9fa64c3df9918d368c672f4d  libxslt/attrvt.c
7f70a5b93877486087797c62fd2b3bb326be0de25b24f921dc064065c31c848d  libxslt/documents.c
9018a1807e69a91bc56a5866fdf3851ed2531614e905c9baf35c93e5dc60c1fc  libxslt/documents.h
df42bfafa0f012a0c603098eadccc7cd762044586a4a0d708344380381557697  libxslt/extensions.c
5b950cc89a943ffd73b7ab176749a07348002300c9ad9f208c1ca1cab5aabddf  libxslt/extensions.h
a4645e0e2f52f0ac2baf8800c1d7a4a926c757f1010b1dcb71c00aeb4df71efb  libxslt/extra.c
e97dd6bb735d3edae5ab3f8aa28edd07eadc7279ecce2ea3df3839f7d8130728  libxslt/extra.h
607457ca3591f2df8b9fa002fee4c5ff31850d19a8034219a282744a5cac3afa  libxslt/functions.c
7dce02663fbd29e047ccef7e59653f6cdaa6684640cf79fb34dc02948057935e  libxslt/functions.h
bd4d61c12e7510cb43eabffdd0974eebd3307d5f918940138d67a04cc9d86a6a  libxslt/imports.c
d7c9088e81aa7455d1eb709edd9b73c6c4e2615dd718aa5c8586a43143c3b942  libxslt/imports.h
714903cd7471fdfc5f60e77873f30ef3216a9eab46945f5494abacfd765bdb70  libxslt/keys.c
d7abf6e55125b92ee3621820ea0605c15c6018c9fed5cb67961856593e117016  libxslt/keys.h
8ee07a553aebf1aab2c9cf1381231386027c85ced6135288d90bf531ca4944f5  libxslt/libxslt.h
73b2b3f63c44fd25d856568de1d0b16789564c661835de13cfa7393050a44e11  libxslt/namespaces.c
5687d29f62a49fe6b927244a0a66378cfa7b6a6432de7d3dbf3cb429442de2ad  libxslt/namespaces.h
908ad3fb384fcee3780603d6bc052c2e3130bf3481dd0f51817ffff24edc88e7  libxslt/numbers.c
120e60619e69de1d3f7b9c3223ad52fb413afc0ad5273bf4cabeb78fa054d9f7  libxslt/numbersInternals.h
68791b0c8ed5caeb3f0041fb31baf0c3fbc7b2c6ecd32c7e75cc51276b55228a  libxslt/pattern.c
b49f813dfb3d51882264c3284196e18a3de0ef1569a58abb4ebacebb6e5e47b4  libxslt/pattern.h
8c32447816267de8a8cd3acaafca068199a17d82d5dc37a6d0db2703bf1d008a  libxslt/preproc.c
0ff2e311d1e1b1dc819c402f7e59f0160a11e211946fbda480485792498f46cc  libxslt/preproc.h
8b85dfa5e0f68cb4cb1e4a08f117867c7e4a52789d0cb4347d57da053cb9ce70  libxslt/security.c
7d40f5732fd6c45093bd334017458e408538a7908d5a7d4b1c5c9925df85c795  libxslt/security.h
f32a2e77c9dfc3f2c3222e3c0a06088fb038517c4e64a60c00aafc5df742c39f  libxslt/templates.c
6e7b7a26aba2e8a539a4d51d30d3db419919e5df954d6a82d1330690e955a08a  libxslt/templates.h
635bdee679d614c515dcb6aaacb3da23ebe9c65c31b4a371ec7746bdbfc87a04  libxslt/transform.c
2024fb99e515d0e4c0c76ed668a56b2a3f9a52647d2d2a5335a38025dd944ad8  libxslt/transform.h
c4b10bb2726c2dd1424679f6515cbbba9b7b5a0a52d91acb2fd92f247630e210  libxslt/transformInternals.h
c1389eb78fd55dc17e3627d0407a7f2852888380181c8ed613751c533524ff3f  libxslt/trio.h
975f94d69e63d0e500762eec517a8be6627f4c16e6ccd2641ef59f4f22e9c75f  libxslt/triodef.h
afa8bec96d9c436608f0dd59e6a6ce596c2d87a99e037b7bd8f8746698b59589  libxslt/variables.c
7100203dee1009c2b6b8a6d683b233fbda5e33dc561a6ee6dccea3426d2c8c80  libxslt/variables.h
f305a89873928d656a616d74467155635d2eeacaf5aa42c1f6cdf06cd486824b  libxslt/win32config.h
771862e6fe5fff6693a225a7b4c2e26d7a82edab10575a49dac3e42657c3381c  libxslt/xslt.c
c26171d90df53ddf08ab6a61010a58f49dd041ab5bf255a0de0001b6a285804c  libxslt/xslt.h
d846c42989a7619ab41e31a750c025a6a9ea649bab457ce396093926cd5661a6  libxslt/xsltInternals.h
aadf47219f23e3302e5b894f750b017b8abb4545eaf1376769e8672024cd88f4  libxslt/xsltconfig.h.in
d7e96e1fed1b0888010252805e157e76a1c17f038040d0da9a261bc489537f49  libxslt/xsltexports.h
46b1766b5ecdc21a42e8524d3230b3a63a23d4dd1f523ede0663fc0b864e9c4a  libxslt/xsltlocale.c
a69c4612625f648260c1143308cd3fefba7d58d7a4116ab536b75866b425e081  libxslt/xsltlocale.h
6357dbf93d8518f426aafa7c0310839db11027852ba8ea46d9f94e50337bc9f5  libxslt/xsltutils.c
d5e82e62047dfa378d395941524b476e86aaf952d7e895eb68ef344df6c044a3  libxslt/xsltutils.h
```
