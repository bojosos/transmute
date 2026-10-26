#pragma once

// Guest-side audio file access shared by AudioFile/ExtAudioFile (audio_file.cpp)
// and AVAsset/AVAssetReader (avfoundation.cpp): files are opened and read
// through the guest's libSystem, so paths resolve in the root.
//
// Linear PCM files (audio_file_core.hpp) are read as they are. Compressed files a
// decoder exists for (audio_stream.hpp: MP3, FLAC, Ogg Opus, and MP4 or Matroska
// audio in those codecs) carry their stream, and their container describes the
// decoded audio: 32-bit float, the decoder's rate and channels, data_bytes for the
// decoded length.

#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/audio_stream.hpp"

#include <memory>
#include <span>

namespace alchemy::gui::completion_kit::media {

struct GuestAudio {
    std::int64_t fd{-1};
    std::uint64_t size{};
    audio_file::Container container;
    std::shared_ptr<audio_stream::Stream> stream;   // compressed files only
    [[nodiscard]] std::uint64_t frames() const { return container.data_bytes / container.format.frame_bytes(); }
};

// Opens and parses `path` (for reading, or reading and writing): an OSStatus (fnfErr,
// kAudioFilePermissionsError, the parser's errors).
std::int32_t open_guest_audio(Support& support, const std::string& path, GuestAudio& out, bool read_write = false);
// For a file the PCM parser refused (`pcm_status`): its compressed stream through `read`, and the
// container describing the decoded audio. An OSStatus; kAudioFileUnsupportedDataFormatError
// wins over kAudioFileUnsupportedFileTypeError.
std::int32_t open_audio_stream(GuestAudio& audio, audio_file::ReadAt read, std::int32_t pcm_status, std::string& why);
// pread through the guest; the byte count read.
std::size_t read_guest(Support& support, std::int64_t fd, std::uint64_t offset, std::span<std::uint8_t> out);
void close_guest_audio(Support& support, GuestAudio& audio);
// Frames [start, start + count) of the audio as interleaved samples in [-1, 1] (the container
// format's channels) appended to `out`; PCM is read through `read` (the file's bytes). The frames
// read.
std::size_t read_audio_frames(const GuestAudio& audio, const audio_file::ReadAt& read, std::uint64_t start, std::size_t count,
                              std::vector<double>& out);
std::size_t read_audio_frames(Support& support, const GuestAudio& audio, std::uint64_t start, std::size_t count, std::vector<double>& out);

} // namespace alchemy::gui::completion_kit::media
