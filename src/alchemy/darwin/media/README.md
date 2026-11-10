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
rate) and sink nodes (receiver blocks), effect nodes (AVAudioUnitEQ with RBJ-cookbook biquad bands,
AVAudioUnitDelay, a Schroeder-Moorer AVAudioUnitReverb, a soft-clipping AVAudioUnitDistortion,
AVAudioUnitVarispeed, and an overlap-add AVAudioUnitTimePitch), AVAudioUnitSampler (SoundFont 2 presets or
audio files, played by the AVAudioUnitMIDIInstrument messages; DLS, .aupreset and .exs answer NO with an
error), AVAudioEnvironmentNode (distance attenuation by the documented models and equal-power panning of
mono inputs around the listener, with a reverb send), AVAudioSequencer and AVMusicTrack (Standard MIDI
Files into the engine's instruments, events at their render cycle; loops, mute, solo, rate, positions;
MIDI endpoints as destinations stop explicitly), volume and pan into mixers, node and player times, and
taps. Manual rendering (offline and the realtime block) renders the same graph with no device, the
input node then taking its audio from its manual input block; outside it, the input node is the
default input device (its format, opened while the node feeds the graph or has a tap). AVAudioConverter converts PCM
between layouts, channel counts (channel maps, downmix) and rates, and decodes the compressed
formats that have a decoder (MPEG audio, Opus, FLAC) from AVAudioCompressedBuffers. Unavailable:
compressed files (nil with an error), compressed output (no encoder: the converter is nil), input
without a device or refused by the host (an engine fed by the input node fails to start), scheduling
at a time and taps at a rate other than the engine's (explicit stops).

**Capture (real, `av_capture.cpp`, `capture_host.cpp`):** AVCaptureDevice lists the host's
cameras (`host::camera`: Media Foundation on win32) and the HAL's input devices (`host::audio`
capture streams: WASAPI), with formats, frame-rate ranges, the active format and frame durations,
and discovery sessions. AVCaptureSession with device inputs runs AVCaptureVideoDataOutput
(CMSampleBuffers of IOSurface-backed CVPixelBuffers in the asked pixel format and size, mirrored on
request, on the delegate's queue; late pictures dropped by default) and AVCaptureAudioDataOutput
(float32 linear PCM at the device's rate). The lists are read once per process (no hot-plug).
Focus, exposure, torch and flash answer "not supported"; audioSettings are kept but not applied.

**Who decides access:** the host. `+authorizationStatusForMediaType:` reports Windows' privacy
settings for the camera and microphone (the consent store, read without a prompt): Denied when
they deny this desktop app, Authorized when they allow it, NotDetermined when they say nothing.
`+requestAccessForMediaType:completionHandler:` shows no prompt of its own: when the settings say
nothing, the host decides by opening the default device once (not started), and that answer
holds for the process; with no device the handler gets NO and the status stays NotDetermined. An
app's own explanation before asking is the app's; Alchemy never answers Authorized on its own.
Cloaked runs list no cameras (`ALCHEMY_CAMERA=device` lists them; `ALCHEMY_CAMERA=none` hides
them in any run) and capture silence from the microphones (`ALCHEMY_AUDIO_INPUT=device` opens
them; `=null` keeps them silent in any run).

**Test devices (opt-in, no real device is opened):** `ALCHEMY_CAMERA=pattern` makes "Alchemy Test
Pattern" the only camera (1280x720 and 640x480 colour bars with a moving white square, up to 30
frames per second), and `ALCHEMY_AUDIO_INPUT=tone` makes "Alchemy Test Tone" the only microphone
and the default input (mono 48 kHz, a 440 Hz sine at a quarter of full scale). Access to them is
allowed, since the host's privacy settings concern its real devices. `scripts/chrome/pages/media_capture.html`
checks them end to end through Chrome's getUserMedia (with `--use-fake-ui-for-media-stream`), and
the cloaked silent microphone with `expect=silent`. `ALCHEMY_HAL_LOG=1` prints each CoreAudio HAL
property a program asks for that the HAL does not have, once.

**Audio files (real, `audio_file.cpp`):** AudioFile and ExtAudioFile read and write linear PCM
WAVE, AIFF, AIFC and CAF, by URL or through the client's callbacks
(`AudioFileOpenWithCallbacks`, `AudioFileInitializeWithCallbacks`). Writes go into the data, and
the header's sizes follow at `AudioFileOptimize` and `AudioFileClose`. ExtAudioFile converts
from and to any linear PCM client format at the file's rate. An existing file opens for writing
when its header is the one Alchemy writes (a canonical WAVE, or an Alchemy-written file); others,
and formats a container cannot hold, answer with the AudioFile errors.

**AVAssetWriter (real, `av_asset_writer.cpp`, `movie_file.cpp`):**
- Audio files (WAVE, AIFF, AIFC, CAF, M4A) go through ExtAudioFile.
- MPEG-4, M4V and QuickTime movies go through Alchemy's muxer, and may hold any number of video and audio inputs:
  - Video with H.264 or HEVC settings is encoded by a VTCompressionSession of its own (the host's encoders),
    from `AVAssetWriterInputPixelBufferAdaptor` pixel buffers (its pool is the session's) or from sample buffers
    of images.
  - Audio with AAC or Apple Lossless settings is encoded from linear PCM, converted to the settings' channels
    and rate.
  - Inputs without settings take compressed samples as they are.
- The files have:
  - avcC/hvcC, esds or alac configurations (QuickTime audio in a version 1 sound description with `wave`);
  - ctts for reordered frames, stss, and edit lists for priming;
  - half-second interleaved chunks;
  - each input's transform;
  - the moov in front when `shouldOptimizeForNetworkUse` is set.
- Refused, as documented: settings no encoder here takes; linear PCM, and uncompressed video without settings,
  in movies; MP3 files.

**AVAssetExportSession (real, `av_asset_export.cpp`):**
- Passthrough remuxes an MPEG-4 or QuickTime file's H.264, HEVC, AAC and Apple Lossless samples. A time range keeps the
  video from the sync sample before it, and the audio's pre-roll, trimmed by edit lists.
- AppleM4A encodes the decodable audio (a composition's too, with its audio mix) as AAC.
- The H.264 and HEVC presets read the first video track decoded and the audio, and encode them through AVAssetWriter:
  - the sizes fit the preset's box and are never larger than the source (Alchemy's choices; the documentation
    names the presets only);
  - audio is AAC at the source's rate where the encoder takes it, at most two channels.
- Discovery (`exportPresetsCompatibleWithAsset:`, `determineCompatibility...`, `supportedFileTypes`) lists what this
  machine's decoders and encoders allow for the asset.
- Errors: AVErrorEncoderNotFound, AVErrorDecoderNotFound, AVErrorFileAlreadyExists, and
  AVErrorOperationNotSupportedForAsset (composition video; Passthrough of what does not remux).
- An AVVideoComposition stops with an explicit message.

**CoreMIDI 2.0 event lists (real, `coremidi.cpp`, `midi_ump.cpp`):** `MIDIEventListInit`/`Add`,
`MIDISendEventList`, `MIDIReceivedEventList`, and ports and endpoints with a protocol or a block
(`MIDIInputPortCreateWithProtocol`/`WithBlock`, `MIDIDestinationCreateWithProtocol`/`WithBlock`,
`MIDISourceCreateWithProtocol`). Universal MIDI Packets are translated to MIDI 1.0 at the API, and
receive blocks get event lists in their protocol. MIDI 2.0-only messages (per-note and
registered/assignable controllers, 128-bit data) have no MIDI 1.0 form and are dropped.

**Audio input (real):** input devices capture (`hal_io.cpp` InputIo over `host::audio`
capture streams; silence in cloaked runs). IOProcs on an input device get one float input
buffer of the device's I/O buffer frame size per call on the device's input thread. AUHAL input
(`kAudioOutputUnitProperty_EnableIO` on bus 1, output optional) calls the input callback per buffer,
and `AudioUnitRender` on bus 1 hands the buffer over in the client's linear PCM format at the
device's rate (another rate fails `AudioUnitInitialize` with `kAudioUnitErr_FormatNotSupported`,
as AUHAL does not convert input rates). AudioQueue input queues record linear PCM in any layout,
channel count and rate into their enqueued buffers. AVFoundation capture and AVAudioEngine's input
node open their own streams. Without an input device these fail as documented
(`kAudioUnitErr_FailedInitialization`, `kAudioUnitErr_NoConnection`, `kAudioQueueErr_InvalidDevice`).

**Unavailable, as documented (audio):** version 3 audio
units (AUAudioUnit), other components (`AudioComponentRegister` registers none),
aggregate devices and process taps. Block IOProcs run on the I/O thread, not their queue.

**Host dependencies:** `host::audio` (endpoints, output and capture streams, the microphone
privacy setting; WASAPI / MMDevice on win32), `host::camera` (Media Foundation on win32),
`host::midi` (ports, input, output; WinMM on win32), `host::time`,
`host::process`. Adapters: `hal_host.cpp`, `midi_host.cpp`, `capture_host.cpp`.

**Unsupported (explicit failures):** for example AVAssetReader output that
needs encoding, sample-rate conversion or channel remapping, and audio-mix
volume ramps.

**Unverified values:** string constants carry the values of the public documentation. Where it gives none, the
value follows the framework's naming convention (usually the name without its `kVT...`/`kCM...` prefix) and the
code marks it "(unverified: ...)": VideoToolbox compression keys, profile levels, entropy modes and encoder list
keys (`video.cpp`, `media_gaps.cpp`), `kCMSampleAttachmentKey_PartialSync` and
`kCMFormatDescriptionExtension_VerbatimSampleDescription` (`coremedia.cpp`), and the CoreVideo, AVFoundation and
AudioToolbox names marked in `media_gaps.cpp` and `pixel_transfer.cpp`. AVAssetWriter's video settings keys
(`av_asset_writer.cpp`) are another such case:
- `AVVideoWidthKey` and the other dictionary keys carry their own names;
- the compression properties carry the VideoToolbox keys' values (`AverageBitRate`, ...);
- the codec types (`AVVideoCodecTypeH264`, `AVVideoCodecTypeHEVC`) carry the codecs' four-char codes (`avc1`,
  `hvc1`). The `AVAssetExportPreset...` constants carry their own names (`av_asset_export.cpp`).

## Moved from darwin/identities/

* `pixel_buffers.cpp`: headless, every build: CoreVideo pixel buffers are unsupported (kCVReturnUnsupported).
* `audio_components.cpp`: alchemy.exe only (no audio component); `audio_components_fx.cpp` is its empty alchemy-fx twin.
