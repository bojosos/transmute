# libaom test vectors (vendored)

| Field | Value |
| --- | --- |
| Project | libaom, the Alliance for Open Media's AV1 reference codec; only two of its decoder test vectors are vendored, no code |
| Upstream | <https://aomedia.googlesource.com/aom> |
| Tag | `v3.15.2` |
| Commit | `af3dc9aadc793c1b00edc003e3fc19a44fdeb574` |
| License | BSD-2-Clause (`LICENSE`) with the Alliance for Open Media Patent License 1.0 (`PATENTS`), both copied verbatim from the tag (`https://aomedia.googlesource.com/aom/+/refs/tags/v3.15.2/LICENSE`, `.../PATENTS`) |
| Files | `test-vectors/av1-1-b8-01-size-16x16.ivf` (318 bytes) and `test-vectors/av1-1-b10-00-quantizer-00.ivf` (416,797 bytes), with their `.md5` per-frame checksums (154 and 164 bytes), byte-identical to the downloads below (`.gitattributes` keeps them out of end-of-line conversion) |
| Modifications | none |

Source: libaom's test-data storage, which `test/test_data_util.cmake` names and `test/test_vectors.cc` lists:

- <https://storage.googleapis.com/aom-test-data/av1-1-b8-01-size-16x16.ivf> and `.ivf.md5`
- <https://storage.googleapis.com/aom-test-data/av1-1-b10-00-quantizer-00.ivf> and `.ivf.md5`

Each download matches the SHA-1 listed for it in the tag's `test/test-data.sha1`
(`838388fbda4a1d91be81ff62694c3bf13c460d38`, `4229c1caf8e25eb3073456fb90ceed206753901e`,
`9bbe8499796aa588ff02e313fb0d4349940d2fea`, `36b402eedad2bacee8ac09acce44e2fc356dd80b`).

Use: `alchemy_gui_media_codecs` and `alchemy_gui_media_guest` decode them with dav1d (8 and 10 bits per
component), directly and through VTDecompressionSession, and compare every frame with its MD5. Nothing is compiled
into Alchemy.

To update: download the vectors again from the storage above, check them against `test/test-data.sha1` of the new
tag, and update this file and `THIRD_PARTY_NOTICES.md`.

SHA-256 of the vendored files:

```
4764a286d8b2faeaf42f4418e7d7a28d58fc8fd4d00a3d0a7f44b0a4099de7f2  LICENSE
661fb8e504744e95587b556b94a58343448300606a41bea8c7a9b97125696e61  PATENTS
fe6053ce4ee20a1c0ec6f7fe35db097e92ad25d8a3505598bd89162c74d7944f  test-vectors/av1-1-b10-00-quantizer-00.ivf
72e5971c7589bfffbe06fac5a2b1848b8edf09ac32a907a296a78b2ab79f4086  test-vectors/av1-1-b10-00-quantizer-00.ivf.md5
8bb6d2c4bfdddca26815e4545c829c8d25ed11341a8d2858ada792e50e4f1ee9  test-vectors/av1-1-b8-01-size-16x16.ivf
895afaacb16aa4c5fab46fa66d344fdbb8c2f1befa5cce9e17919b601f89b59f  test-vectors/av1-1-b8-01-size-16x16.ivf.md5
```
