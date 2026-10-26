# OpenAL

`/System/Library/Frameworks/OpenAL.framework`.

* **Real (alchemy-fx, `mixer.cpp`):** OpenAL 1.1 with AL_EXT_float32, AL_EXT_MCFORMATS, AL_EXT_OFFSET and
  ALC_ENUMERATION_EXT, as a software mixer on the default output device's I/O (`media/hal_io.hpp`). The
  device is named "Alchemy Default Output".
  * **Contexts:** each has its own listener, sources and distance model; buffers belong to the device;
    a suspended context does not mix.
  * **Sources:** played through their buffers with pitch and linear interpolation. Static and streaming
    sources are supported (queue, unqueue, processed counts), along with looping and the second, sample
    and byte offsets.
  * **Gain:** the source gain times the listener gain times the distance model's attenuation (all six
    models) times the cone gain, clamped to the minimum and maximum gain.
  * **Placement:** mono sources are placed by equal-power panning relative to the listener's
    orientation. Multichannel buffers play their channels in order, downmixed to stereo when the device
    has fewer channels.
  * **Sound:** cloaked runs play silently (see `media/README.md`).
* **Unavailable, as documented:**
  * Capture devices: there is no input device. `alcCaptureOpenDevice` answers NULL with ALC_INVALID_VALUE.
  * Doppler shift is not applied; its values are kept.
  * alchemy.exe has no audio output. There `install.cpp` answers like OpenAL with no device: no device or
    context can be made, and AL calls have no current context.
* **Host dependencies:** none directly. Output goes through the CoreAudio HAL's device I/O (`host::audio`).
* **Registration:**
  * alchemy-fx: `openal::install_openal_gui`, the `install_openal_gui` row of the GUI installer table.
  * alchemy.exe: `darwin_headless::install_openal` in the headless framework set. `openal_fx.cpp` is its
    empty alchemy-fx twin.
  * The enum names (`enums.cpp`) are shared by both builds.
* **Tests:**
  * `tests/openal_tests.cpp` (ctest `alchemy_gui_openal`): enums, errors, a device when the host has one.
  * The mixer over a fake device: `media/tests/media_guest_tests.cpp`, case "openal".
