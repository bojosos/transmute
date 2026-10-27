# Media frameworks for strict mode (src/alchemy/darwin/media/): CoreMedia,
# VideoToolbox, AVFoundation, CoreVideo pixel-buffer gaps, AudioToolbox,
# CoreAudio, AudioUnit, CoreMIDI, MediaAccessibility, ImageCaptureCore.
# Installed by gui::fill::install_media (fill-only); media_gaps.cpp (Chrome's
# media keys and CoreVideo gaps) by install_values_kit. The identities are listed
# in darwin_framework_libraries.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/media/install.cpp
    src/alchemy/darwin/media/coremedia.cpp
    src/alchemy/darwin/media/core_video.cpp
    src/alchemy/darwin/media/pixel_transfer.cpp
    src/alchemy/darwin/media/pixel_convert.cpp
    src/alchemy/darwin/media/video.cpp
    src/alchemy/darwin/media/audio.cpp
    src/alchemy/darwin/media/misc.cpp
    src/alchemy/darwin/media/media_gaps.cpp)
# QLab media (installed by install_completion_kit through
# src/alchemy/darwin/kit/completion/media/install.cpp): CMTime, CoreMIDI over host::midi,
# the CoreAudio HAL over host::audio, AudioToolbox MusicPlayer / AudioFile /
# ExtAudioFile / AudioUnit listeners, AVFoundation, and the CAClock media clock.
list(APPEND ALCHEMY_GUI_SOURCES
    src/alchemy/darwin/media/cmtime.cpp
    src/alchemy/darwin/media/midi_core.cpp
    src/alchemy/darwin/media/midi_ump.cpp
    src/alchemy/darwin/media/media_container.cpp
    src/alchemy/darwin/media/midi_host.cpp
    src/alchemy/darwin/media/coremidi.cpp
    src/alchemy/darwin/media/hal_core.cpp
    src/alchemy/darwin/media/hal_host.cpp
    src/alchemy/darwin/media/hal_io.cpp
    src/alchemy/darwin/media/hal.cpp
    src/alchemy/darwin/media/resampler.cpp
    src/alchemy/darwin/media/audio_units.cpp
    src/alchemy/darwin/media/audio_converter.cpp
    src/alchemy/darwin/media/audio_queue.cpp
    src/alchemy/darwin/media/audio_format.cpp
    src/alchemy/darwin/media/avaudio_buffers.cpp
    src/alchemy/darwin/media/avaudio_player.cpp
    src/alchemy/darwin/media/avaudio_engine.cpp
    src/alchemy/darwin/media/avaudio_converter.cpp
    src/alchemy/darwin/media/avaudio_effects.cpp
    src/alchemy/darwin/media/music_core.cpp
    src/alchemy/darwin/media/music.cpp
    src/alchemy/darwin/media/audio_file_core.cpp
    src/alchemy/darwin/media/audio_file.cpp
    src/alchemy/darwin/media/au_listeners.cpp
    src/alchemy/darwin/media/avfoundation.cpp
    src/alchemy/darwin/media/av_player.cpp
    src/alchemy/darwin/media/av_sample_buffer.cpp
    src/alchemy/darwin/media/caclock.cpp)

# Headless shell (src/alchemy/darwin/shells.hpp): alchemy.exe's answers for
# names this framework's GUI provider owns in alchemy-fx, where the _fx twin
# defines the same entry point empty.
target_sources(alchemy_core PRIVATE src/alchemy/darwin/media/media_headless.cpp)
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/media/media_headless.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/media/media_fx.cpp)

function(alchemy_gui_media_tests)
    alchemy_gui_add_unit_test(alchemy_gui_media src/alchemy/darwin/media/tests/media_tests.cpp TIMEOUT 120)
    # Host-only cores of the QLab media providers (CMTime, MIDI, MIDI files,
    # audio files, HAL model).
    alchemy_gui_add_unit_test(alchemy_gui_media_unit src/alchemy/darwin/media/tests/media_core_tests.cpp)
    # Their guest-facing exports through the strict-mode provider fixture.
    alchemy_gui_add_unit_test(alchemy_gui_media_guest src/alchemy/darwin/media/tests/media_guest_tests.cpp TIMEOUT 180)
    # compressed_audio reads minimp3's conformance vectors.
    target_compile_definitions(alchemy_gui_media_guest PRIVATE ALCHEMY_THIRD_PARTY="${PROJECT_SOURCE_DIR}/third_party")
endfunction()
alchemy_gui_register_tests(alchemy_gui_media_tests)

# Headless framework set (both builds; darwin/headless_additions.hpp), moved
# from darwin/identities/frameworks.cpp:
target_sources(alchemy_core PRIVATE
    src/alchemy/darwin/media/pixel_buffers.cpp
    src/alchemy/darwin/media/audio_components.cpp)
# CoreVideo pixel buffers are core_video.cpp's in alchemy-fx: pixel_buffers.cpp
# (none can be created) is alchemy.exe's only, pixel_buffers_fx.cpp its twin.
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/media/pixel_buffers.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/media/pixel_buffers_fx.cpp)
# AudioComponent and the AudioUnit calls are audio_units.cpp's in alchemy-fx:
# audio_components.cpp (no component) is alchemy.exe's only, and
# audio_components_fx.cpp its empty twin.
list(APPEND ALCHEMY_HEADLESS_ONLY_SOURCES src/alchemy/darwin/media/audio_components.cpp)
list(APPEND ALCHEMY_GUI_SOURCES src/alchemy/darwin/media/audio_components_fx.cpp)
