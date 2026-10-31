# AVFAudio

`/System/Library/Frameworks/AVFAudio.framework/Versions/A/AVFAudio` (alchemy-fx).

* **Real:** AVSpeechUtterance (string, voice, rate, pitch, volume, delays) is a real value object; the speech-rate constants have their header values.
* **Real, elsewhere:** AVAudioTime, AVAudioPCMBuffer, AVAudioCompressedBuffer, AVAudioFile,
  AVAudioConverter, AVAudioPlayer and AVAudioEngine with its nodes (including the AVAudioUnit effects, the
  sampler and the environment node) are `media/avaudio_*.cpp` (see `media/README.md`); AVAudioFormat and
  AVAudioChannelLayout are `media/avfoundation.cpp`.
* **Real, elsewhere, speech:** AVSpeechSynthesisVoice and AVSpeechSynthesizer speak with the host's voices (`host::speech`: SAPI on Windows) through the audio output, with the delegate callbacks and `writeUtterance:toBufferCallback:` (`media/avaudio_speech.cpp`).
* **Unavailable, as documented:** On a host without voices (or without an output device) an utterance is cancelled (`didCancel`) instead of spoken, and a notice is printed once.
* **Host dependencies:** `host::speech` (through media/avaudio_speech.cpp).
* **Registration:** `install_avfaudio`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/avfaudio_tests.cpp` (ctest `alchemy_gui_avfaudio`).
