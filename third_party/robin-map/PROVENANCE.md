# tsl::robin_map (vendored)

| Field | Value |
| --- | --- |
| Project | robin-map, a C++ open-addressing hash map and set using robin hood hashing |
| Upstream | <https://github.com/Tessil/robin-map> |
| Tag | `v1.4.1` (the latest release tag when vendored, 2026-10-04) |
| Commit | `bd14e6830a1474fed9d2d03f5c3b0683d818d540` |
| License | MIT (`LICENSE`, copied verbatim) |
| Files | `include/tsl/robin_growth_policy.h`, `robin_hash.h`, `robin_map.h`, `robin_set.h` and `LICENSE`, byte-identical to the upstream blobs at the commit above (`.gitattributes` keeps them out of end-of-line conversion) |
| Modifications | none |

SHA-256 of the vendored files:

```
11f2c685ba565a31aef1219f63a7a8845602c97bff70016b9e3278dfe51a2ec5  LICENSE
6bef4f8420dc1efee7a33ddc72f082dfe284ca5ba826f64aa940be596e8932eb  include/tsl/robin_growth_policy.h
c68eb9cac91221246a2bc6edc63cee7bae382ae5b2d345f0af3216ae28dd397c  include/tsl/robin_hash.h
23bf96fe93dc5aea9b1e002d0df1cb91fa24cccbfa74ce0573f44bba865d20ab  include/tsl/robin_map.h
1cfb73344b604c60cec68e21c67796945985f0924d79b145f151d717704c7f54  include/tsl/robin_set.h
```

Use: header-only, host side only, through `include/alchemy/hash_map.hpp`
(`alchemy::hash_map` / `alchemy::hash_set`). CMake option `ALCHEMY_ROBIN_MAP`
(default ON); OFF makes the aliases `std::unordered_map` / `std::unordered_set`
for bisecting.

To update: fetch the new tag, copy the same five files from `git cat-file blob
<tag>:<path>` (not from a working tree with end-of-line conversion), and
update this table, the hashes and `runtime/THIRD_PARTY_NOTICES.md`.
