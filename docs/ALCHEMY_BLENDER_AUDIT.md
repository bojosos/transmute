# Blender 4.5.14 ARM64 audit

Alchemy inspects the official macOS ARM64 bundle and resolves available symbols
without executing Blender. Blender is not yet runnable. Background mode still
loads framework and Objective-C dependencies, and scanning every Python extension
finds requirements that may not be needed by a particular startup command.

## Input and reproduction

The input is [Blender's official 4.5.14 ARM64 disk image](https://download.blender.org/release/Blender4.5/blender-4.5.14-macos-arm64.dmg),
311,909,763 bytes. Its SHA-256 matches the
[published release manifest](https://download.blender.org/release/Blender4.5/blender-4.5.14.sha256):

```text
65134d9b07b20e2fa8d3c9e44f6f44ffb5c9774dd521b95f50387310241ca170
```

The local extraction is under ignored `artifacts/alchemy-blender/`. The app
payload extracted successfully. Windows denied creation of the disk image's
unrelated Applications-volume symlink, so the archive extraction reported a
warning. Neither the image nor its app binaries are redistributed fixtures.

The inspection on 2026-10-01 used the independently built LLVM 19.1.5 guest
runtimes and an explicit MaterialX Python-extension search directory:

```powershell
pwsh -NoProfile -File scripts/audit_alchemy_bundle.ps1 `
  -Bundle artifacts/alchemy-blender/extracted/Blender/Blender.app `
  -RuntimeDirectory artifacts/alchemy-llvm-arm64 `
  -DylibDirectory artifacts/alchemy-blender/extracted/Blender/Blender.app/Contents/Resources/4.5/python/lib/python3.11/site-packages/MaterialX `
  -OutputDirectory artifacts/alchemy-blender/report-final
```

The script writes `dependencies.json` and `summary.json`. Paths identify the
original resources, dylibs and Python extensions. The extra MaterialX directory
is explicit because a static scan does not reproduce Python's runtime loading
context. Omit it to inspect the stricter default search policy.

## Measured requirements

| Measurement | Result |
| --- | --- |
| Bundle Mach-O files | 204 |
| Inspected files including four supplied LLVM guest libraries | 208 |
| Unresolved strong binding records | 2,713 of 153,996 import records |
| Missing library identities across the scan | 29 |
| Main executable dependencies | 42, including 15 framework identities |
| Main executable unresolved strong binding records | 639 |
| Main executable minimum OS / build SDK metadata | 11.2.0 / 14.0.0 |
| Main executable requested stack | 1 MiB, supported by the existing engine |
| Main executable fixups | Conventional dyld binding/rebasing |

Counts are binding records, so a symbol referenced in many slots or files
appears repeatedly. These numbers measure symbol lookup, not behavior or a
compatibility percentage. The report sets `supported` and `execution_verified`
to false. There are no unresolved libc++ records with the selected LLVM profile,
but stock Apple object layouts and vendor ABI choices still need validation.

One scanned file is rejected: the bundled Python build object
`Contents/Resources/4.5/python/lib/python3.11/config-3.11-darwin/python.o`.
It is an `MH_OBJECT`, not a loadable executable, dylib or bundle. This rejection
does not indicate a malformed runtime library.

| Missing provider | Strong binding records | Examples and implications |
| --- | --- | --- |
| libSystem | 1,806 | Fortified memory/string entry points, rune helpers, stack probing, frame registration, more POSIX/Darwin services |
| CoreFoundation | 243 | Constant strings, arrays, dictionaries, URLs, numbers, retain/release |
| ncurses | 159 | Terminal data and functions used by bundled Python modules |
| Objective-C runtime | 85 | Message dispatch, class registration, retain/release and autorelease pools |
| Accelerate | 67 | BLAS/LAPACK names and BNNS interfaces |
| Foundation | 54 | Bundle, process, string, file-manager and error classes |
| Metal | 54 | Device queries and class references remain present in a background-capable binary |
| AppKit | 52 | Application/class/data references remain linked before any window is opened |
| libedit | 43 | Readline/history interfaces in bundled modules |
| OpenAL | 41 | Audio device, context and source interfaces |

Further missing identities include SystemConfiguration, CoreServices,
CoreGraphics, IOKit, ColorSync, Carbon, CoreAudio, AudioToolbox, AudioUnit,
CoreVideo, Cocoa, QuartzCore, OpenGL, MetalPerformanceShaders,
MetalPerformanceShadersGraph, QuickLookThumbnailing, zlib, bzip2, iconv and panel.
Missing identities can have no recorded direct imports and still prevent
dependency loading. The complete JSON lists every identity and import.

The pinned [Blender Apple build configuration](https://github.com/blender/blender/blob/v4.5.14/build_files/cmake/platform/platform_apple.cmake)
also declares framework dependencies. The audited binary is the evidence used
to select implementation work.

## Next execution gates

1. Bring up bundled CPython 3.11 with filesystem, entropy, locale and signal/error
   behavior that its startup actually uses. Load a real extension with a guest
   callback and verify its import scope, resources and TLS.
2. Implement the audit's libSystem gaps and build independently licensed guest
   zlib, bzip2, iconv and terminal libraries as workloads require them.
3. Add Objective-C registration and messaging, then measured non-windowing
   CoreFoundation/Foundation services. Validate constant objects and class/data
   layouts before claiming stock-binary compatibility.
4. Reach Blender `--version`, then background scene creation and saving. Trace
   actual startup requirements to select additional framework services. Required
   unsupported behavior must fail explicitly; empty framework exports do not
   establish compatibility.
5. Run a small CPU render with an exact image/output check. Windowing, graphics
   presentation and GPU translation remain deferred.

Engine dependencies stay with Transmute's owners. See
[the recorded engine requirements](ALCHEMY_ENGINE_REQUIREMENTS.md) for RCpc
instructions, VM ranges and worker termination. This audit neither boots macOS
nor uses Apple SDK or system-library payloads as providers.
