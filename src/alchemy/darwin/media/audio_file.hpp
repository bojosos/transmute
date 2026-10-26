#pragma once

// Guest-side audio file access shared by AudioFile/ExtAudioFile (audio_file.cpp)
// and AVAsset/AVAssetReader (avfoundation.cpp): files are opened and read
// through the guest's libSystem, so paths resolve in the root.

#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"

#include <span>

namespace alchemy::gui::completion_kit::media {

struct GuestAudio {
    std::int64_t fd{-1};
    std::uint64_t size{};
    audio_file::Container container;
    [[nodiscard]] std::uint64_t frames() const { return container.data_bytes / container.format.frame_bytes(); }
};

// Opens and parses `path` (for reading, or reading and writing): an OSStatus (fnfErr,
// kAudioFilePermissionsError, the parser's errors).
std::int32_t open_guest_audio(Support& support, const std::string& path, GuestAudio& out, bool read_write = false);
// pread through the guest; the byte count read.
std::size_t read_guest(Support& support, std::int64_t fd, std::uint64_t offset, std::span<std::uint8_t> out);
void close_guest_audio(Support& support, GuestAudio& audio);

} // namespace alchemy::gui::completion_kit::media
