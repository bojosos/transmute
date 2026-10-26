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
linear PCM layout) and AVAudioFile for reading and writing (the files AudioFile reads and writes,
converted to and from the processing format). AVAudioPlayer decodes its file or data and plays it on the default output with
volume, fades, pan, rate, loops, seeking, `playAtTime:` and metering. AVAudioEngine renders its
graph on the default output: output, mixer and player nodes (buffers, files, segments, loop and
interrupt options, completion handlers), source nodes (render blocks, resampled from their format's
rate) and sink nodes (receiver blocks), volume and pan into mixers, node and player times, and
taps. Manual rendering (offline and the realtime block) renders the same graph with no device, the
input node then taking its audio from its manual input block. AVAudioConverter converts PCM
between layouts, channel counts (channel maps, downmix) and rates, and decodes the compressed
formats that have a decoder (MPEG audio, Opus, FLAC) from AVAudioCompressedBuffers. Unavailable:
compressed files (nil with an error), compressed output (no encoder: the converter is nil), input
(no device: an engine fed by the input node fails to start outside manual rendering), scheduling
at a time and taps at a rate other than the engine's (explicit stops).

**Audio files (real, `audio_file.cpp`):** AudioFile and ExtAudioFile read and write linear PCM
WAVE, AIFF, AIFC and CAF, by URL or through the client's callbacks
(`AudioFileOpenWithCallbacks`, `AudioFileInitializeWithCallbacks`). Writes go into the data, and
the header's sizes follow at `AudioFileOptimize` and `AudioFileClose`. ExtAudioFile converts
from and to any linear PCM client format at the file's rate. An existing file opens for writing
when its header is the one Alchemy writes (a canonical WAVE, or an Alchemy-written file); others,
and formats a container cannot hold, answer with the AudioFile errors.

**CoreMIDI 2.0 event lists (real, `coremidi.cpp`, `midi_ump.cpp`):** `MIDIEventListInit`/`Add`,
`MIDISendEventList`, `MIDIReceivedEventList`, and ports and endpoints with a protocol or a block
(`MIDIInputPortCreateWithProtocol`/`WithBlock`, `MIDIDestinationCreateWithProtocol`/`WithBlock`,
`MIDISourceCreateWithProtocol`). Universal MIDI Packets are translated to MIDI 1.0 at the API, and
receive blocks get event lists in their protocol. MIDI 2.0-only messages (per-note and
registered/assignable controllers, 128-bit data) have no MIDI 1.0 form and are dropped.

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
