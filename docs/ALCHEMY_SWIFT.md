# Swift runtime strategy

QLab 5.6.3 is the first Alchemy target that links Swift. Every Swift
dependency is a system library (`/usr/lib/swift/*.dylib`) that a Mac supplies
from its dyld shared cache, so Alchemy has to provide one. This document
records what is needed, the options that were evaluated, and the
recommendation. No toolchain or source was downloaded for this evaluation.
The findings come from the QLab bundle, the swift.org project layout and its
published ABI documents. An approval request for the download is at the end.

## What QLab needs

These numbers come from `scripts/audit_alchemy_app.ps1`. See
[the QLab audit](ALCHEMY_QLAB_AUDIT.md) for the full per-image tables.

| Image | Swift type descriptors | Swift libraries (strong / weak) | Imported Swift-library symbols |
| --- | --- | --- | --- |
| `QLab` (main) | 9 (versus 840 Objective-C classes) | 5 strong: `libswiftCore`, `libswiftFoundation`, `libswiftDispatch`, `libswiftos`, `libswiftOSLog`. 16 weak: Darwin, ObjectiveC, CoreFoundation, CoreAudio, CoreMIDI, CoreMedia, CoreImage, QuartzCore, AVFoundation, Metal, IOKit, XPC, simd, Spatial, UniformTypeIdentifiers | 143 libswiftCore, 54 libswiftFoundation, 15 libswiftDispatch, 10 libswiftos, 2 libswiftOSLog, 1 each from the weak overlays, and 24 from `CryptoKit.framework`, which is Swift-only |
| SnoizeMIDI, Sparkle (with its helpers), Syphon, libndi | 0 | none | none |

QLab is almost entirely Objective-C: 840 classes and 30,904 methods. Its Swift code is a few files, with 9 nominal types and 522 defined Swift
symbols. It uses 24 CryptoKit entry points (ChaChaPoly seal/open, P521 key
agreement, HKDF over SHA512) and some Foundation bridging. QLab was built
with Xcode 26.2, which ships Swift 6.2. It still needs
a real Swift runtime, for two reasons:

* `libswiftCore` is a strong dependency, and Alchemy binds strong imports
  before execution. The project rule forbids empty stubs.
* The imports include more than runtime calls (`swift_retain`,
  `swift_allocObject`, `swift_dynamicCast`, `swift_getTypeByMangledNameInContext`,
  `swift_once`, `swift_updateClassMetadata2`, `swift_beginAccess`, ...). They
  also reference standard-library internals that the app's inlined code
  manipulates directly:
  * String's `_StringGuts`/`_StringObject` (`grow`, `copyUTF8`, `sharedUTF8`, `_foreignCount`)
  * `_DictionaryStorage.allocate/resize/copy` and `_HashTable.previousHole`
  * `_ContiguousArrayStorage`, `_CocoaArrayWrapper`, `_swiftEmptyArrayStorage`
  * the `Hasher` internals
  * `_stringCompareWithSmolCheck`, `_SwiftObject`, and protocol witness tables
    for `Hashable`, `Equatable` and `_ObjectiveCBridgeable`

Because the stdlib is `@frozen`/`@inlinable`, the app's code already contains
the stdlib's private object layouts. Any provider must therefore match the
layouts of Apple's ABI-stable stdlib bit for bit.

Swift concurrency is not used: there are no `swift_task_*` or
`swift_continuation_*` imports. Swift classes do exist, because the main
executable imports `_OBJC_CLASS_$__TtCs12_SwiftObject` and
`swift_updateClassMetadata2`. Alchemy's Objective-C runtime must therefore
accept Swift class metadata: the FAST_IS_SWIFT bits in `class_ro` data, the
Swift class-metadata tail, and `SwiftObject` as a root class.

## Licensing and provenance

The Swift project is open source under Apache 2.0 with the Runtime Library
Exception. That covers the compiler, the runtime (`stdlib/public/runtime`,
C++), the standard library (`stdlib/public/core`, Swift), Demangling,
SwiftShims, Concurrency, the Darwin/Platform overlay sources, and
`swift-corelibs-libdispatch` (which includes Dispatch's Swift overlay
sources). Like the pinned LLVM runtimes, it would be a permissively licensed
third-party source. It would have recorded provenance (`provenance.json`,
license, NOTICE) and would be built from a pinned tag.

The following are **not** open source, and Alchemy must implement them itself:
* The Apple SDK, including `MacOSX.sdk` headers and module maps.
* The Apple-SDK overlays: `libswiftFoundation` (the Darwin build),
  `libswiftos`, `libswiftOSLog`, `libswiftAVFoundation`, `libswiftCoreAudio`,
  `libswiftCoreMIDI`, `libswiftCoreMedia`, `libswiftMetal`, and others.
* `CryptoKit`.

`swift-foundation` (Apache 2.0) is a useful reference for value-type
semantics. Its module names and mangling (`FoundationEssentials`) do not match
the `$s10Foundation...` symbols that QLab imports.

## Options evaluated

### (a) Build the open-source runtime and stdlib for `arm64-apple-macosx` on Windows

**What it takes**

1. **A Swift compiler at the same release tag as the stdlib sources.** The
   swift.org Windows x64 toolchain fits. It ships LLVM with the AArch64
   target, and Mach-O emission is target-independent in LLVM. `swiftc -c
   -target arm64-apple-macosx11.0` emits ARM64 Mach-O objects. Alchemy
   already links ARM64 Mach-O with `ld64.lld` and project-authored link stubs
   (`tests/fixtures/macos/link_*`).
2. **No Apple SDK for the core library.** The stdlib core (`-parse-stdlib
   -module-name Swift`) imports only `SwiftShims`. SwiftShims is shipped in
   the Swift repo and needs only Clang's builtin headers. This makes
   `libswiftCore.dylib` buildable SDK-free.
3. **A project-authored "Alchemy Darwin sysroot" for the runtime C++.** This
   replaces the SDK headers the C++ runtime includes: `<pthread.h>`,
   `<dlfcn.h>`, `<mach-o/dyld.h>`, `<mach/mach.h>`, `<malloc/malloc.h>`,
   `<os/lock.h>`, `<dispatch/dispatch.h>` (for `swift_once` →
   `dispatch_once_f`), `<objc/runtime.h>`, `<objc/objc-internal.h>`
   declarations, and `<crt_externs.h>`. These are declarations only, written
   from the published interfaces, following the existing convention of
   `runtime/include/` (project headers) plus `link_*.dylib` (link-only
   identities).
   * The same sysroot gets `module.modulemap` files.
   * Later, the Darwin overlay (`libswiftDarwin`, built from the Swift repo's
     `stdlib/public/Platform`) and project-authored overlays can import a
     small `Darwin`/`ObjectiveC`/`Foundation` Clang module.
4. **Our own build script, not the Swift CMake/build-script.** Cross-building
   Darwin stdlibs from a Windows host is not a supported upstream
   configuration. The script `scripts/build_alchemy_swift.ps1` would mirror
   the stdlib's CMake flags:
   * `-parse-stdlib`, `-enable-library-evolution`, `-library-level api`
   * the availability macros from `utils/availability-macros.def`
   * `-Xfrontend -enable-ossa-modules`
   * `gyb` expansion through the host Python 3.12.

   The runtime C++ (`stdlib/public/runtime`, `stubs`, `LLVMSupport`,
   `Demangling`, `SwiftObject.mm`, `ObjC bridging`) compiles with the
   toolchain's clang for `arm64-apple-macosx` with `SWIFT_OBJC_INTEROP=1`.
   `SwiftObject.mm` and the bridging `.mm` files are Objective-C++ and use the
   sysroot's project ObjC headers.
5. **Alchemy support, which is in-house work in any option:**
   * **dyld image callbacks.** `_dyld_register_func_for_add_image` with real
     `mach_header` pointers, plus `getsectiondata`. These are needed for the
     `__swift5_proto`, `__swift5_types` and `__swift5_replace` scans.
   * **ObjC runtime hooks** used by the Swift runtime:
     * `_objc_realizeClassFromSwift`
     * `objc_setHook_getClass`
     * `objc_addLoadImageFunc`
     * `_objc_setClassCopyFixupHandler`
     * `objc_readClassPair`
     * the `objc_debug_isa_class_mask` and `objc_debug_taggedpointer_*` data
       symbols, whose values must match Alchemy's object representation.
   * **Swift-class metadata in `objc_runtime.cpp`.** The runtime must
     recognise it and route `SwiftObject` retain/release to `swift_retain`.
   * **libSystem services:** `malloc_zone_*`, `malloc_size`,
     `os_unfair_lock`, `dispatch_once_f`, `__crashreporter_info__`. Most
     already exist; the doctor will list any that are missing.

**Result.** This produces the genuine stdlib layouts and witness tables by
construction, so the app's inlined stdlib code is correct. Effort, in agent
waves (one agent-week ≈ one wave slot):

| Step | Work | Estimate |
| --- | --- | --- |
| S1 | Sysroot headers, module maps, link stubs, `build_alchemy_swift.ps1`, `libswiftCore` + `libswiftDemangle`-in-core, provenance | 1.5–2 agent-weeks |
| S2 | Swift fixtures built by the toolchain (classes, generics, protocols, casts, errors, String/Array/Dictionary) running under `alchemy-fx`, plus the dyld callbacks | 1 agent-week |
| S3 | Swift ⇄ ObjC interop: Swift classes in the ObjC runtime, `SwiftObject`, NSString/NSArray/NSDictionary bridging (`_bridgeToObjectiveC`, `_CocoaArrayWrapper`), the ObjC runtime hooks | 2 agent-weeks |
| S4 | `libswiftDispatch` (from `swift-corelibs-libdispatch/src/swift`) and `libswiftDarwin`/`libswiftObjectiveC` from the Swift repo | 0.5–1 agent-week |
| S5 | Project-authored overlays for the measured symbol sets: Foundation (54), os (10), OSLog (2), CryptoKit (24: ChaCha20-Poly1305, P-521 ECDH, HKDF-SHA512, bit-exact; backed by Windows CNG where it offers the algorithm and project code otherwise), and the single-symbol weak overlays. Written in Swift as modules named `Foundation`/`os`/`CryptoKit`, so the mangled names match, and compiled against the sysroot | 2 agent-weeks |

**Total: about 7–8 agent-weeks, or 2–3 parallel waves.** The first QLab
benefit comes after S1–S3.

**Risks**
* **Flag drift.** Replicating the stdlib build flags exactly matters most;
  wrong flags change the ABI. Mitigation: compare the export list against the
  identity and symbol set the doctor reports for QLab, and add Swift fixtures
  from the same toolchain.
* **Apple-only exports.** Apple's `libswiftCore` may export symbols that
  open-source builds omit, for example back-deployment shims. The doctor
  shows any QLab still imports after S1.
* **Overlays need the ClangImporter.** Building the Foundation overlay
  requires the ClangImporter to see project ObjC declarations of
  `NSString`/`NSData`/`NSURL` with the right nullability and Swift names. A
  small header set exists already for fixtures. It grows with S5.

### (b) Implement the runtime C++ and the needed stdlib entry points ourselves

The runtime alone is around 60–80k lines of C++. It covers metadata
instantiation, value witnesses, generic metadata caches, protocol-conformance
lookup, dynamic casting, the type-by-mangled-name demangler, exclusivity
tracking, and ObjC interop. The imported stdlib symbols are not leaf
functions either. `_StringGuts.grow`, `_DictionaryStorage.resize` and
`Hasher._finalize` assume the exact private layouts of String (16-byte
`_StringObject` discriminators, small-string encoding, breadcrumbs, native
storage headers), of Dictionary and Set (`_RawDictionaryStorage` header,
bitmap words, seed, scale), and of SipHash-1-3 hashing. The app's inlined
code reads and writes the same bits.

A layout-exact reimplementation would amount to a manual port of the open-source
stdlib, with more risk and no licensing benefit, because the original is
Apache 2.0. **Rejected**, except for the in-house pieces that (a) also needs:
the dyld/ObjC hooks and the SDK overlays.

### (c) Other approaches

* **Host-native runtime.** fx maps guest VA to host VA, so a runtime compiled
  for x64 could in principle work on guest metadata. But every value-witness,
  vtable and closure call is guest ARM64 code with `swiftcc` (x20 self, x21
  error, x22 async context). The runtime makes these calls constantly, and
  `invoke_callback` round trips would dominate. The stdlib's Swift half is
  guest code in any case. **Rejected.**
* **Freestanding/minimal stdlib presets.** These include
  `stdlib_S_standalone_minimal_macho_x86_64` and the Embedded Swift mode.
  They drop ObjC interop and change the ABI (no library evolution, no
  runtime metadata in Embedded), so they cannot satisfy an Apple-built app.
  They are useful only as a smoke test that the toolchain cross-compiles to
  Mach-O. **Not a provider.**
* **Lazy failure stubs for Swift imports.** QLab would get past dyld, and the
  first Swift call would fail explicitly. This violates "empty stubs never
  count as support". `ALCHEMY_PROBE_MISSING` remains the diagnostic way to see
  how far Objective-C startup reaches before the first Swift call. **Not a
  provider.**

## Recommendation

Adopt **(a)**: build the open-source Swift runtime and stdlib as guest ARM64
libraries from a pinned swift.org release, using a project-authored Darwin
sysroot and Alchemy's own `ld64.lld` link step. The libraries are installed
like the LLVM C++ runtimes. Use `--runtime-dir`, or better, a
`runtime/swift/` identity map for `/usr/lib/swift/libswiftCore.dylib` and the
other Swift libraries. Implement the Apple-SDK overlays (Foundation subset,
os/OSLog, CryptoKit subset) as project-authored Swift modules, bounded by the
audited symbol sets. Extend the ObjC runtime for Swift classes.
Sequence: S1 → S2 → S3, then S4/S5 in parallel. Swift work for the renderer add-on is not
needed: its installer has no Swift dependencies.

### Download needing approval

Nothing has been downloaded. Approval is needed for:

1. **Swift toolchain for Windows x64**, from the swift.org release page:
   `https://download.swift.org/swift-6.2-release/windows10/swift-6.2-RELEASE/swift-6.2-RELEASE-windows10.exe`.
   It is an installer of about 0.6–0.8 GB, plus Visual Studio's Windows SDK
   component, which is already present. Any newer swift.org 6.x RELEASE works
   if it is pinned together with the matching source tag. The build uses only
   its `swiftc` and `clang`, never a macOS SDK.
2. **Sources at the same tag** (`swift-6.2-RELEASE`), as git sparse checkouts
   under ignored `artifacts/alchemy-swift-source/`:
   * `https://github.com/swiftlang/swift`. Only `stdlib/`, `include/swift/`,
     `lib/Demangling`, `cmake/`, `utils/` (gyb, availability macros), and
     `LICENSE.txt`/`NOTICE.txt` are needed. This is about 150 MB sparse.
   * `https://github.com/swiftlang/swift-corelibs-libdispatch`, for
     `src/swift` (the Dispatch overlay) and `LICENSE`.
   * Optional, read-only reference for S5 semantics:
     `https://github.com/swiftlang/swift-foundation`.

All three are Apache 2.0 with the Runtime Library Exception. Provenance
(URL, tag, commit SHA, licenses) would be recorded in
`artifacts/alchemy-swift/provenance.json` and in
`runtime/THIRD_PARTY_NOTICES.md`, as for the LLVM runtimes.
