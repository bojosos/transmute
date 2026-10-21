# MediaPlayer

`/System/Library/Frameworks/MediaPlayer.framework/Versions/A/MediaPlayer` (alchemy-fx).

* **Real:** MPNowPlayingInfoCenter keeps the now-playing information and playback state; MPRemoteCommandCenter hands out its commands, which keep their enabled state; MPMediaItemArtwork keeps its handler.
* **Unavailable, as documented:** The host never sends a remote command (media keys, Now Playing widgets), so no target is ever called, and nothing shows the now-playing information. The host media transport controls (Windows SystemMediaTransportControls) would make it real.
* **Host dependencies:** none.
* **Registration:** `install_mediaplayer`, one row of the installer table after the
  fill-only set (fill-only, through `gui::values::Provider`). Chrome 155 first
  linked the framework.
* **Tests:** `tests/mediaplayer_tests.cpp` (ctest `alchemy_gui_mediaplayer`).
