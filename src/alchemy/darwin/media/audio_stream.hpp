#pragma once

// Compressed audio files as sample streams (host-only core, media group): the first audio track
// of a container (media_container.hpp) whose codec has a decoder here (media_codecs.hpp) --
// MP3 files, native and Ogg FLAC, Ogg Opus, and MP4/M4A or Matroska/WebM audio in those codecs.
// Frames are counted without the encoder's priming and padding (the MP3 encoder delay, the Opus
// pre-skip, an MP4 edit), so frame 0 is the first audible frame. Reads are sample-accurate: after
// a seek the decoder starts far enough before the target (MP3's bit reservoir, Opus's energy
// prediction) and the pre-roll is discarded. AudioFile describes the packets themselves.

#include "alchemy/darwin/media/media_codecs.hpp"
#include "alchemy/darwin/media/media_container.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alchemy::gui::completion_kit::media::audio_stream {

class Stream {
public:
    // The stream of the file's first audio track. Null with an OSStatus in `status`:
    // kAudioFileUnsupportedFileTypeError ('typ?') when no container parser recognises the file or
    // it has no audio, kAudioFileUnsupportedDataFormatError ('fmt?') when its codec has no
    // decoder here (AAC, ALAC, ...), naming the codec in `why`.
    static std::unique_ptr<Stream> open(container::ReadAt read, std::uint64_t size, std::int32_t& status, std::string& why);

    [[nodiscard]] const container::Movie& movie() const { return movie_; }
    [[nodiscard]] const container::Track& track() const { return movie_.tracks[index_]; }
    // AudioFileTypeID: 'MPG3', 'flac', 'mp4f' or 'm4af'; for Ogg and Matroska files Alchemy
    // answers the container's own tag ('OggS', 'webm' or 'matr').
    [[nodiscard]] std::uint32_t file_type() const { return file_type_; }
    [[nodiscard]] double rate() const { return decoder_->rate; }
    [[nodiscard]] std::uint32_t channels() const { return decoder_->channels; }
    [[nodiscard]] std::uint32_t bits() const { return decoder_->bits; }
    [[nodiscard]] std::uint64_t frames() const { return playable_; }
    [[nodiscard]] std::uint32_t priming() const { return priming_; }
    [[nodiscard]] std::uint32_t remainder() const { return remainder_; }

    // The packets (AudioFile's view).
    [[nodiscard]] std::size_t packet_count() const { return track().samples.size(); }
    [[nodiscard]] std::uint32_t frames_per_packet() const { return frames_per_packet_; }
    [[nodiscard]] std::uint32_t maximum_packet_bytes() const { return maximum_packet_; }
    [[nodiscard]] std::uint64_t packet_start(std::size_t index) const { return starts_[index]; }   // decoded frames, priming included
    std::vector<std::uint8_t> packet(std::size_t index) const;
    // The compressed AudioStreamBasicDescription (40 bytes) and the decoder's magic cookie.
    [[nodiscard]] std::vector<std::uint8_t> asbd() const;
    [[nodiscard]] const std::vector<std::uint8_t>& magic_cookie() const { return track().config; }

    // Appends frames [start, start + count) (clipped to the stream) as interleaved floats; returns
    // the frames appended. A packet the decoder rejects reads as silence (its duration), and
    // `last_error` says why.
    std::size_t read(std::uint64_t start, std::size_t count, std::vector<float>& out);
    [[nodiscard]] const std::string& last_error() const { return last_error_; }

private:
    Stream() = default;
    void seek(std::uint64_t decoded_frame);
    std::size_t packet_for(std::uint64_t decoded_frame) const;

    container::ReadAt read_;
    container::Movie movie_;
    std::size_t index_{};
    std::uint32_t file_type_{};
    std::unique_ptr<codecs::AudioDecoder> decoder_;
    std::vector<std::uint64_t> starts_;      // decoded frame of each packet, then the end
    std::uint64_t playable_{};
    std::uint32_t priming_{}, remainder_{}, frames_per_packet_{}, maximum_packet_{}, preroll_{};
    // Decoding position: buffer_ holds decoded frames from buffer_start_; next_ is the next packet.
    std::vector<float> buffer_;
    std::uint64_t buffer_start_{};
    std::size_t next_{};
    std::string last_error_;
};

} // namespace alchemy::gui::completion_kit::media::audio_stream
