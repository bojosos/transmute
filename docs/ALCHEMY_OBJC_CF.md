# Alchemy Objective-C and CoreFoundation profile

This milestone provides a bounded, general object runtime for command-line
programs and guest libraries. It adds independently authored providers for
`/usr/lib/libobjc.A.dylib`, CoreFoundation's `Versions/A` identity, and
Foundation's `Versions/C` identity. These providers expose a useful subset;
they do not claim complete framework compatibility or Blender execution.

No Apple SDK, runtime binary, framework payload, or implementation is required
to build or run the authored workloads. Alchemy continues to use Transmute's
existing execution, checked memory, callback, and worker interfaces. This
milestone changes Alchemy code and tests; engine development stays separate.

## Objective-C

The runtime registers ordinary ARM64 classes and metaclasses, superclass
relationships, ivars, selectors, methods, and categories from relocated Mach-O
metadata. It accepts pointer method lists and direct or indirect signed-relative
small method lists. Metadata addresses, extents, counts, instance sizes, and
implementation ranges are checked before registration. Nonlazy class and
category `+load` methods run once after binding and final permissions, before
C constructors. Class `+initialize` runs on first use, including inherited
implementations for subclasses, with coordination between workers.

`objc_msgSend`, `objc_msgSendSuper`, and `objc_msgSendSuper2` use authored ARM64
tail dispatchers. Lookup preserves integer arguments, SIMD arguments, the
indirect-result register, the original stack, and the return address. A guest
implementation therefore receives its original calling convention. Nil
messages return zero in integer and floating-point return registers. An
indirect structure result from a nil receiver is not promised by this profile.

The subset includes class, selector, method and ivar queries; dynamic class
registration; method addition and replacement; instance allocation; strong
ARC helpers; retain/release; and per-thread autorelease pools. `NSObject`
provides allocation, initialization, ownership, class queries, equality,
hashing, and basic selector/inheritance checks. Native method type encodings
use the target ARM64 types, including boolean `B`.

Class/category registration and selector patching are transactional. Failed
registration restores previous category methods and selector values before
the loader removes new mappings. Closed plugin modules remain resident, so
reopening does not repeat `+load`. Per-thread cleanup drains destructor-created
autoreleases too; excessive cleanup fails explicitly at 65,536 releases.

## CoreFoundation values

| Family | Implemented behavior |
| --- | --- |
| Ownership and identity | Retain/release/count, type IDs, equality, hashing, allocator query, object and type descriptions |
| Strings | Creation from C strings, bytes and UTF-16 characters; copy/substrings; length and character access; comparison; checked UTF-8, ASCII and UTF-16 conversion; compiler constant strings |
| Data | Immutable and mutable creation/copy, byte access, append, replace, delete and resize; writes through mutable guest byte pointers are observed |
| Numbers and booleans | Signed integer and floating-point creation/conversion, comparison, singleton booleans, NaN and infinity constants |
| Arrays | Immutable/mutable creation and copy, access, membership, insertion, replacement and removal |
| Dictionaries | Immutable/mutable creation and copy, lookup, membership, enumeration, add/replace/set and removal |
| File URLs | POSIX file-path creation, absolute resolution, filesystem representation, directory flag, URL string and path access |

Strings use UTF-16 code units for lengths and indexes. UTF-8 decoding rejects
overlong sequences, encoded surrogates and invalid scalars. Partial UTF-8
conversion does not split a surrogate pair. C string APIs accept UTF-8 and
ASCII; UTF-16 bytes use the byte/character APIs. Stable guest string storage
belongs to its object. Constant strings are checked, adopted without changing
read-only image bytes, and kept immortal for the process lifetime.

Arrays and dictionaries support standard CF-type callback records and null
callbacks. Standard callbacks retain children and use the authored equality
and hash behavior; null callbacks treat values as opaque pointers. Container
copies preserve child identity and ownership. Dictionary key comparisons occur
outside mutation locks, with revision checks before committing an update.
Callers must break ownership cycles and coordinate their shared mutable data.
Custom allocator and callback behavior is unsupported.

Type IDs and hashes are authored values; callers must obtain type IDs through
the corresponding query functions. Descriptions are authored text, with bounded
traversal. These values are not hardcoded Apple private representations. File
URLs use the existing Alchemy path namespace and file services; other URL
schemes, Windows/HFS CF path styles, and networking are deferred.

## Foundation value bridge

`NSString`, `NSNumber`, `NSData`, `NSArray`, and `NSDictionary` provide common
factories, initializers, accessors, equality, hashing, and copies over the same
CoreFoundation object registry. CF-to-Objective-C casts preserve the guest
address and ownership. Compiler string literals and runtime-created strings
work through both interfaces. `NSClassFromString`, `NSSelectorFromString`,
`NSStringFromClass`, and `NSStringFromSelector` are included.
The subsequent [dependency follow-up](ALCHEMY_PYTHON_FOLLOWUP.md) adds a bounded
`NSProcessInfo`, `NSAutoreleasePool` and checked `NSLog`/`NSLogv` subset.

This is a value bridge, not an implementation of arbitrary Foundation private
classes or layouts. Bare `NSNumber` initialization requires a typed numeric
initializer. Unknown selectors are resolved and forwarded as in objc4
(`+resolveInstanceMethod:`, `-forwardingTargetForSelector:`, then the
`objc_setForwardHandler` handler); a message nothing answers reports an
unsupported-message diagnostic.
No formatting method forwards guest arguments to a Windows formatter.

## Explicit boundaries

(Later work added declared protocols, weak references, Blocks, message
forwarding and Objective-C exceptions: `@throw`/`@catch` are C++ ABI exceptions
thrown and unwound by the guest LLVM libc++abi and libunwind through guest
frames; an exception that reaches a provider's native frame or nothing catches
stops with its name and reason.) Originally deferred: tagged pointers, Swift/GC/preoptimized metadata, arm64e,
copy properties, custom allocation zones, and full Foundation are deferred.
Unsupported metadata fails before guest constructors. Known deferred helper
functions produce specific diagnostics when invoked; doctor marks their
imports as unsupported. Other unavailable strong imports fail during linking.
Windowing, AppKit, graphics, GPU translation, run loops, bundles/preferences,
property-list serialization, notifications, and dispatch queues are later
milestones. The follow-up provides counting dispatch semaphores and deadlines.

`doctor --deps --json` includes partial provider profiles and Objective-C
section names. It identifies known unsupported protocol/stub-category sections
without running guest code. `objc_metadata_verified` and `execution_verified`
remain false: static symbol resolution does not validate every relocated
metadata pointer or predict selectors requested at runtime.

## Build and acceptance workloads

Use the normal build and test commands:

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --parallel 4 --output-on-failure
build\Release\alchemy.exe --jit-stats build\macos\objc_runtime
build\Release\alchemy.exe --jit-stats build\macos\cf_workload
```

Clang and Mach-O LLD build the permanent fixtures from project-owned C,
Objective-C and declarations using generic ARMv8-A. LLVM's compiler resource
headers supply standard scalar declarations; there is no Apple SDK search.
`link_objc`, `link_foundation`, and `link_core_foundation` are link-only inputs.
Their placeholder bodies never supply runtime behavior.

Acceptance covers dispatch, cross-dylib inheritance, categories, floating-point
and homogeneous/indirect structure results, stack arguments, strong ARC ivar
destruction, plugin registration while workers exist, per-thread pools, Unicode,
CF collections and the Foundation bridge. Each successful guest workload runs
in interpreter and JIT modes at the preferred base and at `0x120000000`, checks
exact output/exit/stderr, and requires nonzero native JIT instruction counts.
Separate units check malformed metadata, protected-selector rollback, shared
ownership, concurrent initialization, recursive dictionary keys, concurrent
mutation, and numeric conversion boundaries. A protocol fixture proves that
unsupported metadata cannot execute its main function.

## Verified result

On October 1, 2026, all 31 Alchemy CTest groups passed on the final Release
build, including the 15 object-profile groups, the original external sample,
and the existing guest LLVM C++ workloads. The new acceptance programs passed
in all four execution modes. Captured direct JIT runs produced:

| Workload | Exact stdout line | Native guest instructions |
| --- | --- | ---: |
| Cross-dylib Objective-C ABI/ARC | `Objective-C runtime: 27 42` | 771 |
| CF/Objective-C bridge | `CoreFoundation Objective-C bridge: 27` | 1,093 |
| C CoreFoundation workload | `CoreFoundation objects: 27 4 6` | 383 |
| External sample | `Hello World from MacOS: 27` | 22 |

Each line includes a final newline and each program returns zero. The supplied
sample's SHA-256 remained
`63097a1ee99e51840230c98692bade514ed5bbdfe4ef5005f0b4a4f9ad8d6602`.
All four existing LLVM runtime binary hashes and six notice hashes still match
their saved provenance.

An earlier complete snapshot passed all 35 groups. After subsequent concurrent
Transmute updates, the final complete run passed 34 of 35: Windows shard 2 fails
`test_guest_opengl_builtin_window` with "native helper pointers escaped into a
guest message". It reproduces when run alone in the engine test executable,
which does not link Alchemy. This remaining regression belongs to the separately
owned Transmute work; no engine code or test was changed or bypassed here.
The reproduction and ownership are recorded in
[engine requirements](ALCHEMY_ENGINE_REQUIREMENTS.md#regression-gate-during-object-profile-integration).
The later dependency follow-up passed all four Windows shards on its tested
snapshot. Its full result was 43/46 groups, with only the separately documented
Python RCpc gates failing; the earlier OpenGL regression did not reproduce.

## Provenance

Authored code, assembly, declarations and fixtures retain the project's MIT
license. Existing LLVM runtime notices and provenance remain separate. External
Apple application binaries and the supplied sample remain local audit/test
inputs. This work claims independent implementation, without a formal
clean-room or blanket legal claim.

Interface references include [Apple's ARM64 ABI](https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms),
the [CoreFoundation ownership policy](https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFMemoryMgmt/Concepts/Ownership.html),
and [Objective-C object initialization](https://developer.apple.com/library/archive/documentation/General/Conceptual/CocoaEncyclopedia/Initialization/Initialization.html).
The target compiler's emitted metadata and authored fixtures verify the
selected layouts and calling conventions.
