# swift-corelibs-foundation (code ported into the CoreFoundation provider)

| Field | Value |
| --- | --- |
| Project | swift-corelibs-foundation, whose `Sources/CoreFoundation` is CF-Lite, the open-source CoreFoundation |
| Upstream | <https://github.com/swiftlang/swift-corelibs-foundation> |
| Tag | `swift-6.4.0-RELEASE` |
| Archive | <https://github.com/swiftlang/swift-corelibs-foundation/archive/refs/tags/swift-6.4.0-RELEASE.tar.gz>, 2,349,951 bytes, SHA-256 `4d54f115ca4f24f77117c97e64760f6379bb45745a5c79d52a2877fe84c6ad5c` |
| License | Apache-2.0 with the Runtime Library Exception (`LICENSE`, copied verbatim) |
| Vendored | only `LICENSE`. No upstream file is in the repository. The ported pieces are rewritten in C++ inside the provider files listed below, and each of those files names its sources in its header. |

## What is ported, and where

| Provider file | Upstream sources | What |
| --- | --- | --- |
| `src/alchemy/darwin/corefoundation/cf_encodings.cpp` | `CFStringEncodingDatabase.c`, `CFStringUtilities.c`, `CFICUConverters.c` | The known-encoding table (Windows code pages, canonical/IANA names, display names, the most compatible Mac script); the IANA, Windows code page, NSStringEncoding and ICU converter name rules |
| `src/alchemy/darwin/corefoundation/cf_strings.cpp` | `CFString.c`, `CFStringScanner.c`, `CFStringTransform.c` | The line and paragraph bound rules, the integer and double scanner rules, the separate/combine and trim/pad semantics, and the CFStringTransform identifier table |

Local changes: the parallel C arrays became one table of rows; the CF dictionaries
and locks became C++ containers; converters, transliterators and break iterators come
from the host's ICU (`include/alchemy/host/icu.hpp`) instead of the bundled
`_FoundationICU`; the code works on the object store's UTF-16 strings instead of
CF's inline buffers.

SHA-256 of the upstream files at the tag, for review:

```
0ca84095d1de77cad1a866ebbad28f8db98a0826c6055d92b0608c5baa1643e2  LICENSE
1c57831d5732e173e5cdd3675bec97e00063517bcd2873258635f47c08775ed6  Sources/CoreFoundation/CFStringEncodingDatabase.c
53691d002959d3c8e2c02835ccc9201e0c0e3a350c64976427c1b581b866f7dc  Sources/CoreFoundation/CFStringUtilities.c
942244e1dbc8f587af167cbb886af7a9d151e7bdef03879b62893b87834dc6d0  Sources/CoreFoundation/CFICUConverters.c
3930afe8a4c9403d1e7e2c7ff5f9dc80fed6e2e945ed21d4c80e4690a28c1d79  Sources/CoreFoundation/CFString.c
21c0e63c1c560bc676717dc659c39074a086c8ff7da45e57e5ba8715685cf202  Sources/CoreFoundation/CFStringScanner.c
5ef78a9d27f43cf63994ccb69d5aadfd6ee5f427c50ac576c9b3e8e964dcb7da  Sources/CoreFoundation/CFStringTransform.c
```

To port more: download the same tag (or a newer one, updating this table), port into
the provider, name the upstream file and tag in the provider file's header comment,
and add a row above.
