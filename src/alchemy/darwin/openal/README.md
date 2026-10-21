# OpenAL

`/System/Library/Frameworks/OpenAL.framework` (both builds).

* **Real:** the enum tables (`alGetEnumValue`, `alcGetEnumValue`), the ALC
  error strings and the version query (OpenAL 1.1).
* **Unavailable, as documented:** there is no output or capture device:
  `alcOpenDevice` and `alcCreateContext` return NULL with
  ALC_INVALID_VALUE / ALC_INVALID_DEVICE, the device lists are empty, and AL
  calls without a current context have no effect (`alGetError` reports
  AL_INVALID_OPERATION). The host audio area (`host::audio`, WASAPI) could
  make a device real.
* **Host dependencies:** none.
* **Registration:** `darwin_headless::install_openal`, in the headless
  framework set (`darwin/frameworks.cpp`, every build).
* **Tests:** `tests/openal_tests.cpp` (ctest `alchemy_gui_openal`) and the
  headless `alchemy_frameworks`.
