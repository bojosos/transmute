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

**Audio output (real):** the CoreAudio HAL's output devices play (`hal_io.cpp`): IOProcs
(function, block and `AudioDeviceAddIOProc`) run on the device's I/O thread, a guest
thread, one buffer of the device's I/O buffer frame size at a time, and a
`host::audio` output stream (WASAPI shared mode) plays their sum. The output audio
units are real (`audio_units.cpp`): the default and system output units and AUHAL,
with any linear PCM input format, channel maps, sample-rate conversion
(`resampler.cpp`, windowed sinc), render notifications, property listeners and
`kHALOutputParam_Volume`. Cloaked runs (`ALCHEMY_GUI_CLOAK`) and
`ALCHEMY_AUDIO_OUTPUT=null` play into a silent stream that keeps real time;
`ALCHEMY_AUDIO_OUTPUT=device` plays to the device in a cloaked run.

**AVFAudio (real, `avaudio_*.cpp`):** AVAudioTime, AVAudioPCMBuffer (guest memory in any
linear PCM layout) and AVAudioFile for reading (the files AudioFile reads, converted to the
processing format). AVAudioPlayer decodes its file or data and plays it on the default output with
volume, fades, pan, rate, loops, seeking, `playAtTime:` and metering. AVAudioEngine renders its
graph on the default output: output, mixer and player nodes (buffers, files, segments, loop and
interrupt options, completion handlers), volume and pan into mixers, node and player times, and
taps. Unavailable: writing files (nil with an error), input (no device: an engine fed by the input
node fails to start), manual rendering (NO with an error), scheduling at a time and taps at a rate
other than the engine's (explicit stops).

**Unavailable, as documented (audio):** input devices and AUHAL input
(`kAudioUnitErr_FailedInitialization`, `kAudioUnitErr_NoConnection`), version 3 audio
units (AUAudioUnit), other components (`AudioComponentRegister` registers none),
aggregate devices and process taps. Block IOProcs run on the I/O thread, not their queue.

**Host dependencies:** `host::audio` (endpoints and output streams; WASAPI / MMDevice on win32),
`host::midi` (ports, input, output; WinMM on win32), `host::time`,
`host::process`. Adapters: `hal_host.cpp`, `midi_host.cpp`.

**Unsupported (explicit failures):** for example AVAssetReader output that
needs encoding, sample-rate conversion or channel remapping, and audio-mix
volume ramps.

## Moved from darwin/identities/

* `pixel_buffers.cpp`: headless, every build: CoreVideo pixel buffers are unsupported (kCVReturnUnsupported).
* `audio_components.cpp`: alchemy.exe only (no audio component); `audio_components_fx.cpp` is its empty alchemy-fx twin.
