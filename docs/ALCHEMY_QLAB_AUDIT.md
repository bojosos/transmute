# QLab 5.6.3 ARM64 audit

This audit inspects Figure 53's QLab 5.6.3 after it was installed into an
Alchemy root. No guest instruction ran. The goal is to select the provider
work for the first QLab target, which is launching to the main window. The
numbers count binding records and symbol references. They measure lookup
coverage, not behavior or compatibility.

## Input and reproduction

* The input is the user-supplied `QLab.dmg` (33,730,333 bytes, UDIF with
  bzip2 compression, HFS+). It contains `QLab 5.6.3/QLab.app` and an
  `Applications` symlink.
* The bundle is version 5.6.3 (build 5603). `LSMinimumSystemVersion` is 11.0,
  and it was built with Xcode 26.2 against the macOS 26.2 SDK
  (`DTSDKName macosx26.2`).
* `NSPrincipalClass` is `QLab`, a subclass of NSApplication. `NSMainNibFile`
  is `QLab`.
* The image is a local test input and is not redistributed.

```powershell
# Install into an Alchemy root, keeping the bundle's 18 symbolic links.
scripts/alchemy_install_dmg.ps1 -Dmg $HOME\Downloads\QLab.dmg `
  -Root C:\dev\transmute\artifacts\alchemy-root -Alchemy build-q/Release/alchemy-fx.exe
# Per-image audit (doctor JSON, section headers, symbol tables, Objective-C metadata).
scripts/audit_alchemy_app.ps1 -Bundle C:\dev\transmute\artifacts\alchemy-root\Applications\QLab.app `
  -Alchemy build-q/Release/alchemy-fx.exe -OutputDirectory C:\dev\transmute\artifacts\alchemy-qlab\audit
# Whole graph, as dyld would load it.
alchemy-fx doctor --deps --json --runtime-dir artifacts\alchemy-llvm-arm64 `
  C:\dev\transmute\artifacts\alchemy-root\Applications\QLab.app
```

All numbers come from `alchemy-fx` built from `mac/q-audit`, using the GUI
providers present at `main@9e2f343`, and the pinned LLVM 19.1.5 guest C++
runtimes. Every Mach-O in the bundle is universal (x86_64 + arm64), and only
the arm64 slice's load commands and symbol tables are read. Complete symbol lists are in
`artifacts/alchemy-qlab/audit/summary.json`.

## Images

| Image | Kind | Text | ObjC classes / categories | Swift types | Imports: provider / unresolved | Unresolved strong records |
| --- | --- | --- | --- | --- | --- | --- |
| `Contents/MacOS/QLab` | executable | 6.77 MB | 840 / 27 | 9 | 16,404 / 1,426 | **1,410** |
| `SnoizeMIDI.framework` (A) | dylib | 43 KB | 26 / 4 | 0 | 338 / 62 | 64 |
| `Syphon.framework` (A) | dylib | 38 KB | 30 / 2 | 0 | 231 / 95 | 95 |
| `Sparkle.framework` (B) | dylib | 240 KB | 56 / 0 | 0 | 890 / 109 | 112 |
| `Sparkle … /Autoupdate` | executable | 176 KB | 33 / 0 | 0 | 742 / 111 | 113 |
| `Sparkle … /Updater.app` | executable | 56 KB | 16 / 0 | 0 | 336 / 50 | 52 |
| `Sparkle … /Downloader.xpc` | executable | 12 KB | 5 / 0 | 0 | 86 / 16 | 16 |
| `Sparkle … /Installer.xpc` | executable | 33 KB | 9 / 0 | 0 | 222 / 51 | 51 |
| `Frameworks/libndi.dylib` | dylib (C++) | 1.96 MB | 0 | 0 | 197 / 87 | 87 |

* **Minimum OS.** All images declare macOS 11.0. Sparkle and libndi were
  built against SDK 12.3 and Syphon against SDK 11.3.
* **Dependency graph.** `QLab` loads `@executable_path/../Frameworks/SnoizeMIDI.framework/Versions/A/SnoizeMIDI`,
  `@rpath/Syphon.framework/Versions/A/Syphon` and `@rpath/Sparkle.framework/Versions/B/Sparkle`.
  `libndi.dylib` is not a static dependency; QLab `dlopen`s it for NDI video
  output. The Sparkle helper executables run as separate processes (XPC and
  `Autoupdate`). They are not needed to reach the main window.
* **Recursive doctor result.** The recursive `doctor --deps` run resolves all
  three bundled frameworks through the installed links and the LLVM C++
  runtimes. It reports **1,410 + 64 + 95 + 112 = 1,681 unresolved strong
  binding records**.
* **Missing library identities.** The graph lists 43 missing library
  identities:
  * Frameworks: MetalKit, DiskArbitration, CoreMIDI, CoreAudioKit,
    AudioVideoBridging, GLKit, Quartz, IOSurface, AVFoundation, CoreMedia,
    VideoToolbox, WebKit, Security, ApplicationServices, CFNetwork,
    CoreImage, CoreText, CryptoKit, ImageIO, OSLog, UniformTypeIdentifiers,
    ServiceManagement.
  * `libcompression`.
  * 21 Swift libraries under `/usr/lib/swift`.

### Unresolved strong records by provider

Main executable, in records:

| Provider | Records | Unique symbols | What is used |
| --- | --- | --- | --- |
| AppKit | 428 | 154 | 82 class references, 41 AppKit classes **subclassed** (see below), `NSApplicationMain`, attributed-string keys, accessibility notifications, `NSRectFill*`, `NSDrawThreePartImage`, `NSSetFocusRingStyle`, workspace mount notifications |
| libswiftCore | 170 | 143 | Swift runtime and stdlib internals, see [Swift](#swift) |
| Foundation | 124 | 117 | NSAppleScript/NSAppleEventManager, scripting commands (`NSDeleteCommand`, `NSMoveCommand`, `NSScriptClassDescription`), `NSTask`/`NSPipe`, `NSNetService(Browser)`, `NSURLSession`, `NSKeyedArchiver`, geometry functions (`NSIntersectionRect`, `NSStringFromRect`, ...) |
| CoreGraphics | 99 | 99 | A full CGContext path/stroke/fill/gradient/shadow/text-matrix set, CGPath, CGBitmapContext, CGDisplay mode/refresh and reconfiguration callbacks, `CGDirectDisplayCopyCurrentMetalDevice` |
| AudioToolbox | 69 | 69 | MusicPlayer/MusicSequence/MusicTrack (MIDI file cues), ExtAudioFile/AudioFile, AU parameter listeners, CAClock (MIDI timecode) |
| libswiftFoundation | 54 | 53 | Bridging for String/Array/Dictionary/Data/URL/Date/Locale, `_convertErrorToNSError`, `NSFileHandle` Swift API |
| libSystem | 46 | 46 | `asl_*`, `dispatch_io_*`/`dispatch_data_*`, termios (`tcgetattr`, `cfsetspeed` for serial devices), `getifaddrs`, `task_for_pid`, `mach_vm_read`, `CC_MD5`/`CC_SHA256`, `os_unfair_lock` |
| CoreFoundation | 38 | 29 | CFBundle function pointers, CFUUID, `NSCache`/`NSOrderedSet`/`NSTimer`/`NSUserDefaults` classes |
| CoreMedia / CoreVideo | 36 / 35 | 36 / 35 | Sample buffers, CMTime, CVDisplayLink, CVPixelBuffer, CVMetalTextureCache |
| AVFoundation | 34 | 34 | Asset reading/thumbnails (`AVAssetReader`, `AVAssetImageGenerator`, `AVURLAsset`), audio formats, `AVAudioUnit`, camera capture (`AVCaptureSession`/`Device`/`VideoDataOutput`) |
| CryptoKit | 28 | 24 | ChaChaPoly seal/open, P521 key agreement, HKDF (SHA512): Swift-only API |
| Security | 21 | 21 | The legacy SecureTransport `SSL*` API (OSC/network over TLS), `SecItemImport`, RSA raw decrypt |
| CoreImage | 20 | 17 | `CIFilter` (subclassed), `CIKernel`, `CIContext`, `CIRenderDestination`: video effects |
| DiskArbitration | 17 | 17 | Disk eject/unmount approval sessions |
| QuartzCore | 17 | 17 | CATransform3D, `CABasicAnimation`, `CAGradientLayer`, `CAShapeLayer` |
| CoreAudio / AudioUnit | 14 / 10 | 14 / 10 | HAL device IOProcs, property listeners, host time; AudioUnit render/parameters |
| CoreText | 14 | 14 | CTFramesetter/CTLine/CTRun layout and drawing |
| IOKit | 13 | 13 | Service matching notifications, power assertions |
| Others | ≤ 12 each | | Accelerate (vDSP, `cblas_dgemm`, `dgesv_`), UniformTypeIdentifiers, libswiftos/OSLog/Dispatch, IOSurface, libobjc exceptions and associated objects, CFNetwork, SystemConfiguration, VideoToolbox, ImageIO, Metal device notifications, GLKit, MetalKit (`MTKTextureLoader`), CoreAudioKit (`AUGenericView`), CoreMIDI (one property key) |

Bundled frameworks:

* **SnoizeMIDI** (Objective-C): 48 CoreMIDI client, port, endpoint, device
  and property calls, plus small Foundation, CoreFoundation and libobjc
  exception gaps. It is QLab's whole MIDI layer.
* **Syphon** (Objective-C):
  * 49 OpenGL/CGL calls (`CGLCreateContext`, FBO/shader GL 3.2 core).
  * 10 IOSurface calls.
  * 14 legacy `OSAtomic*`/`OSSpinLock*` calls.
  * `CFMessagePort` and `NSDistributedNotificationCenter` for inter-app
    server discovery.

  QLab references `SyphonMetalServer`/`SyphonMetalClient`, so the Metal path
  is the one that is used.
* **Sparkle 2** (Objective-C):
  * Security code-signing and Authorization calls.
  * ServiceManagement `SMJobSubmit`.
  * WebKit release notes (`WKWebView`, legacy `WebView`).
  * ImageIO, `NSXPCConnection`, ASL.

  QLab creates `SPUStandardUpdaterController` in code (it is not in any
  nib), so the updater is instantiated at launch. Its scheduled network
  checks must stay inert or fail explicitly.
* **libndi** (C++): with the LLVM libc++ runtime, 87 records remain. They
  are libSystem (25), CoreVideo (19), CoreMedia (15), VideoToolbox (11),
  Accelerate (9) and AudioToolbox (6). The library is loaded only when NDI
  output is configured.

## AppKit: controls and drawing

QLab's UI uses standard AppKit controls drawn by AppKit. It does not use
custom Metal UI. Three sources show which controls are used:

1. **Class references:** the 82 AppKit classes named directly by the main
   executable.
2. **Subclasses:** 41 `_OBJC_METACLASS_$_` imports. These classes must be real,
   subclassable host classes with correct `[super …]` behavior:
   * Application and document: `NSApplication`, `NSDocument`, `NSDocumentController`.
   * Windows and controllers: `NSWindow`, `NSWindowController`, `NSViewController`, `NSPanel`.
   * Views: `NSView`, `NSControl`, `NSScrollView`, `NSScroller`, `NSSplitView`, `NSStackView`, `NSRulerView`.
   * Tables and outlines: `NSTableView`, `NSOutlineView`, `NSTableRowView`,
     `NSTableCellView`, `NSTableHeaderView`, `NSTableHeaderCell`.
   * Collections: `NSCollectionView`, `NSCollectionViewItem`, `NSCollectionViewFlowLayout`.
   * Text: `NSTextField`, `NSTextFieldCell`, `NSTextView`, `NSSearchField`.
   * Buttons and menus: `NSButton`, `NSButtonCell`, `NSPopUpButton`,
     `NSPopUpButtonCell`, `NSMenu`, `NSMenuItem`.
   * Other controls: `NSSlider`, `NSSliderCell`, `NSStepperCell`, `NSColorWell`,
     `NSImageView`, `NSPathControl`.
   * Accessibility and drag-and-drop: `NSAccessibilityElement`, `NSFilePromiseProvider`.
3. **Interface Builder archives:** classes in the 104 compiled nibs. These
   are instantiated by name, so no symbol import shows them.

| Control | Code refs | Subclassed | Nib files using it |
| --- | --- | --- | --- |
| NSTableView / NSTableCellView / NSTableHeaderView | yes | yes | 37 / 33 / 23 |
| NSOutlineView | yes | yes | 13 |
| NSScrollView / NSClipView / NSScroller | yes | yes | 55 |
| NSSplitView | yes | yes | 5 |
| NSStackView / NSGridView | yes | yes / no | 5 / 2 |
| NSButton (push, check, radio, `NSSwitch` 13) | yes | yes | 81 |
| NSTextField / NSSecureTextField | yes | yes | 97 / 1 |
| NSTextView | yes | yes | 11 |
| NSPopUpButton | yes | yes | 47 |
| NSSlider | yes | yes | 3 |
| NSStepper | cell | yes (cell) | 9 |
| NSSegmentedControl | no | no | 3 |
| NSTabView | yes | no | 1 |
| NSImageView | yes | yes | 41 |
| NSProgressIndicator | yes | no | 11 |
| NSSearchField | yes | yes | 4 |
| NSPathControl | yes | yes | 4 |
| NSComboBox / NSDatePicker / NSBox | no | no | 1 / 1 / 1 |
| NSPopover | yes | no | 7 |
| NSCollectionView | yes | yes | 1 |
| NSWindow / NSPanel / NSViewController | yes | yes | 30 / 6 / 7 |
| NSMenu / NSMenuItem | yes | yes | 49 |
| NSArrayController / NSObjectController / NSUserDefaultsController (Cocoa bindings) | no | no | 4 / 1 / 10 |
| NSToolbar | no | no | 0 |

The selectors confirm the following usage:
* View-based tables and outlines: `tableView:viewForTableColumn:row:`,
  `outlineView:viewForTableColumn:item:`, `makeViewWithIdentifier:owner:`,
  `outlineView:child:ofItem:`.
* Cell-based data sources: `tableView:objectValueForTableColumn:row:`.
* Custom `drawRect:` views.
* Layer-backed views: `setWantsLayer:`, `updateLayer`, `makeBackingLayer`,
  and subclassed `CALayer`/`CAMetalLayer`.
* Sheets and modals: `beginSheet:completionHandler:`, `runModal`.
* Drag and drop: `registerForDraggedTypes:`, dragging sessions, file promises.
* KVO and Cocoa bindings.
* `NSUndoManager`, which is subclassed.
* `performSelector:withObject:afterDelay:` and timers.

The main-window target needs:
* nib loading (keyed unarchiving of these classes);
* Auto Layout (`NSLayoutConstraint`, stack and grid views);
* the AppKit control set drawn through the Quartz 2D / CoreText layer
  (CGContext over Direct2D, CoreText over DirectWrite);
* `drawRect:` compositing into a window backing store, as the addendum to
  the plan decides;
* `NSDocument`/`NSDocumentController` for the workspace document.

The 99 CoreGraphics and 14 CoreText calls above are QLab's own custom
drawing. They come on top of what AppKit's controls need internally.

## Swift

* **Defined code.** QLab's own Swift code is small: 9 type descriptors and
  522 defined Swift symbols. The main executable has 840 Objective-C classes
  and 30,904 Objective-C methods, so it is overwhelmingly Objective-C.
  SnoizeMIDI, Sparkle, Syphon and libndi contain no Swift.
* **Strong Swift libraries:** `libswiftCore` (143 symbols),
  `libswiftFoundation` (53), `libswiftDispatch` (14), `libswiftos` (9) and
  `libswiftOSLog`.
* **Weak Swift libraries:** 16 overlays, one symbol each (`AVFoundation`,
  `CoreAudio`, `CoreFoundation`, `CoreImage`, `CoreMIDI`, `CoreMedia`,
  `Darwin`, `IOKit`, `Metal`, `ObjectiveC`, `QuartzCore`, `Spatial`,
  `UniformTypeIdentifiers`, `XPC`, `simd`). Each one is a weak
  `__swift_FORCE_LOAD_$_swift*` anchor (Darwin's is
  `__swift_FORCE_LOAD_$_swift_Builtin_float`), so none of them carries
  API.
* **CryptoKit.** CryptoKit (24 symbols) is a Swift-only framework.
* **Runtime imports** include:
  * `swift_allocObject`, `swift_retain`/`release`
  * `swift_dynamicCast`, `swift_getTypeByMangledNameInContext`
  * `swift_updateClassMetadata2`, `swift_getObjCClassMetadata`
  * `swift_once`, `swift_beginAccess`
  * `swift_allocError`/`willThrow`

  Swift classes are present: the executable imports
  `_OBJC_CLASS_$__TtCs12_SwiftObject`.
* **Stdlib internals.** The stdlib imports reach into internals:
  `_StringGuts`, `_DictionaryStorage`, `_HashTable`, `_CocoaArrayWrapper`,
  `Hasher`, `_swiftEmptyArrayStorage`.
* **No concurrency.** There are no `swift_task_*` imports.

[The Swift strategy](ALCHEMY_SWIFT.md) evaluates how to provide these.

## Other frameworks of note

* **WebKit.** QLab imports no WebKit symbols. Only `ChangelogWindow.nib`
  archives `WKWebView` with `WKWebViewConfiguration`,
  `WKUserContentController` and `WKWebsiteDataStore`.
  Sparkle imports `WKWebView`/`WebView` for release notes. The main window
  does not need WebKit, because that nib is not loaded at launch.
  WebKit is a strong load dependency, so the identity must exist. Its
  classes then fail explicitly when they are instantiated.
* **AVFoundation, CoreMedia, CoreVideo, VideoToolbox.** These drive audio and
  video file cues and camera cues. Asset reading, sample buffers, display
  links and Metal texture caches are needed only when such cues load, not
  for an empty workspace.
* **CoreAudio, AudioToolbox, AudioUnit.** QLab drives HAL devices directly
  with IOProcs and property listeners, and hosts Audio Units
  (`AVAudioUnitComponentManager`, `AUGenericView`). Audio device enumeration
  probably happens at launch, for patch and preferences state.
* **CoreMIDI.** It is used through SnoizeMIDI (48 calls) for MIDI cues, MIDI
  input and MSC. QLab itself references one property key. SnoizeMIDI
  creates its client at startup.
* **Metal and resources.**
  * The bundle ships precompiled `default.metallib` and
    `QLabVFXKernels.metallib`. These are AIR, not MSL source, so the
    MSL → SPIR-V compiler does not cover them. Video output would need an
    AIR → SPIR-V path or a CoreImage provider.
  * 57 `.F53VFX` effects are JSON descriptions over built-in CoreImage
    filters (`CIBloom`, ...).
  * `titles.qtz` is a Quartz Composer composition (Quartz framework).
  * `Assets.car` is a compiled asset catalog (`NSImage imageNamed:`), and it
    needs a CoreUI-format reader.
  * One bundled font: `SF-Compact-Display-Light.otf`.

## Installation and links

The DMG's `QLab.app` contains 18 symbolic links, all framework links:
`Versions/Current` and the `Headers`, `Resources`, `Modules`,
`PrivateHeaders`, `Autoupdate`, `Updater.app`, `XPCServices` and binary
aliases. 7-Zip leaves them as empty files when it extracts without the
Windows symlink privilege. The earlier `artifacts/dmg/qlab` extraction shows
this.

`alchemy install` (see [the install notes](#installing-with-alchemy-install)) recreates them:
* directory links become NTFS junctions;
* file links become hard links;
* native relative symlinks are used whenever the host allows unprivileged
  creation (Developer Mode).

It records the original link text in `<root>/.alchemy/links/QLab.app.tsv`.
The loader, `doctor`, bundle scans and guest `readlink`/`lstat`/`realpath`
resolve through that manifest. Therefore `@rpath/Sparkle.framework/Versions/B/Sparkle`
and `Sparkle.framework/Sparkle` name the same image, and `@loader_path`
follows realpath semantics.

### Installing with `alchemy install`

```text
alchemy install [--root DIR] [--links FILE] [--replace] [--no-infer] [--no-native-links] Bundle.app
```

* **Root selection.** `--root`, then `ALCHEMY_ROOT`, then
  `%LOCALAPPDATA%\Alchemy\root`. The root holds `.alchemy-root`,
  `Applications/`, `Library/` and `.alchemy/links/`.
* **Path mapping.** A run or `doctor` of a program inside a root, or with
  `--root`/`ALCHEMY_ROOT`, maps guest `/Applications` and `/Library` into the
  root. `NSBundle` and `argv[0]` then see `/Applications/QLab.app/...`, and
  `/Applications/QLab.app` is accepted as the program path.
* **Link sources.**
  * `--links` gives the link list recovered from the image
    (`scripts/alchemy_install_dmg.ps1` writes it).
  * Native links already present in the source tree are kept.
  * Without a list, empty framework placeholders are repaired from the
    standard framework layout (`--no-infer` disables this).
  * An ambiguous `Versions/Current` fails explicitly. So do absolute,
    escaping or dangling targets.
* **Sandboxed sessions.** Under an MSIX-packaged host such as the Claude
  desktop app, `%LOCALAPPDATA%` is virtualized. Pass an explicit `--root`
  there.

## First execution stop

`alchemy-fx --runtime-dir artifacts\alchemy-llvm-arm64 <root>\Applications\QLab.app`
was run with `ALCHEMY_GUI_NO_ACTIVATE=1`. It cannot reach window creation,
because dependency loading fails first:

```text
alchemy: missing guest dylib: /System/Library/Frameworks/MetalKit.framework/Versions/A/MetalKit
```

* **Where it stops.** Loading follows load-command order. Metal is a
  provider, and MetalKit is the first missing identity. The other 42 missing
  identities listed above follow it.
* **What was fixed on the way.** Before this branch, loading would have
  stopped in Syphon. Alchemy's export-trie parser rejected the empty trie
  edge that older ld64 emits (`_OBJC_CLASS_$_SyphonServer`). dyld accepts
  that edge, and so does the parser now.

## Next gates for "launch to the main window"

1. **Library identities.** Make every strong framework identity loadable,
   with exports bound to real providers or explicit failure on use. Order the
   work by what launch actually touches: Security, CFNetwork, ImageIO,
   CoreText, UniformTypeIdentifiers, OSLog, IOSurface, CoreMedia,
   AVFoundation, CoreMIDI, WebKit, CryptoKit, DiskArbitration, GLKit,
   MetalKit, Quartz, CoreImage, CoreAudioKit, AudioVideoBridging,
   VideoToolbox, ServiceManagement and libcompression. Then the Swift
   libraries; see [the Swift strategy](ALCHEMY_SWIFT.md).
2. **Swift runtime** (S1–S3 of the strategy). It is required because
   `libswiftCore` is a strong dependency, and QLab's Swift classes are
   registered at image load.
3. **AppKit controls and nib loading** for the classes in the tables above,
   over the Quartz 2D/CoreText layer. The `q-quartz` and
   `q-appkit-controls` agents own this work.
4. **Startup services:** SnoizeMIDI's CoreMIDI client, audio device
   enumeration, `NSDocumentController` for the workspace, `NSUserDefaults`,
   and the Sparkle updater controller staying inert.
5. **Deferred until cues need them:** video, effects (AIR metallibs, CoreImage
   filters), NDI, Syphon, and camera capture.
