# AVFAudio

`/System/Library/Frameworks/AVFAudio.framework/Versions/A/AVFAudio` (alchemy-fx).

* **Real:** AVSpeechUtterance (string, voice, rate, pitch, volume, delays) is a real value object; the speech-rate constants have their header values.
* **Unavailable, as documented:** No voice is installed: `+speechVoices` is empty and the voice lookups return nil. Nothing is ever spoken, so `-isSpeaking` is NO and stop, pause and continue return NO. A host text-to-speech engine (a `host::audio` or `host::ui` speech call) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_avfaudio`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/avfaudio_tests.cpp` (ctest `alchemy_gui_avfaudio`).
