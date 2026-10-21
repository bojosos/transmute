# Swift overlays

**Covers:** the Apple SDK Swift modules QLab imports, authored here and
built into the Swift runtime by `scripts/build_alchemy_swift.ps1`: Dispatch,
OSLog, CryptoKit, AVFoundation, CoreAudio, CoreMedia and CoreMIDI, with the
Clang modules in `include/`.

**Host dependencies:** none at run time; the overlays call the providers.

**Note:** the Swift runtime in `artifacts/alchemy-swift-arm64` is not
rebuilt by the CMake build; rebuilding it is a separate, explicit step.
