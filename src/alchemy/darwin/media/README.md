# Media

**Covers:** CoreMedia, VideoToolbox, AVFoundation, CoreVideo pixel-buffer
gaps, AudioToolbox, CoreAudio, AudioUnit, CoreMIDI, MediaAccessibility and
ImageCaptureCore (`install.cpp`, fill-only), QLab's media providers (CMTime,
CoreMIDI over `host::midi`, the CoreAudio HAL over `host::audio`, MusicPlayer, AudioFile /
ExtAudioFile, AU listeners, AVFoundation, the CAClock media clock), and
Chrome's media keys (`media_gaps.cpp`). The Swift overlays of these
frameworks are in `darwin/swift/`.

**Headless:** `media_headless.cpp` (alchemy.exe only), empty
`media_fx.cpp` in alchemy-fx (`darwin/shells.hpp`).

**Host dependencies:** `host::audio` (endpoints; WASAPI / MMDevice on win32),
`host::midi` (ports, input, output; WinMM on win32), `host::time`,
`host::process`. Adapters: `hal_host.cpp`, `midi_host.cpp`.

**Unsupported (explicit failures):** for example AVAssetReader output that
needs encoding, sample-rate conversion or channel remapping, and audio-mix
volume ramps.

## Moved from darwin/identities/

* `pixel_buffers.cpp`, `audio_components.cpp`: headless, every build: CoreVideo pixel buffers are unsupported (kCVReturnUnsupported), and no audio component is registered.
