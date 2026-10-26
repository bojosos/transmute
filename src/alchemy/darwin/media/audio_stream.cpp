#include "alchemy/darwin/media/audio_stream.hpp"

#include "alchemy/darwin/media/audio_file_core.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace alchemy::gui::completion_kit::media::audio_stream {

namespace {

using container::fourcc;
namespace af = audio_file;

std::uint32_t file_type_for(std::uint32_t brand) {
    switch (brand) {
    case fourcc("mp3 "): return af::four("MPG3");      // kAudioFileMP3Type
    case fourcc("fLaC"): return af::four("flac");      // kAudioFileFLACType
    case fourcc("M4A "): case fourcc("M4B "): case fourcc("M4P "): return af::four("m4af");   // kAudioFileM4AType
    case fourcc("OggS"): case fourcc("webm"): case fourcc("matr"): return brand;
    default: return af::four("mp4f");                  // kAudioFileMPEG4Type (ISO files)
    }
}

void put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) { std::memcpy(out.data() + at, &value, 4); }

} // namespace

std::unique_ptr<Stream> Stream::open(container::ReadAt read, std::uint64_t size, std::int32_t& status, std::string& why) {
    std::unique_ptr<Stream> s(new Stream);
    s->read_ = std::move(read);
    if (!container::parse(s->read_, size, s->movie_, why)) {
        status = af::error::unsupported_file_type;
        return nullptr;
    }
    const auto& tracks = s->movie_.tracks;
    const auto audio = std::find_if(tracks.begin(), tracks.end(), [](const container::Track& t) { return t.kind == container::Track::Kind::audio; });
    if (audio == tracks.end()) {
        why = "the file has no audio track";
        status = af::error::unsupported_file_type;
        return nullptr;
    }
    s->index_ = static_cast<std::size_t>(audio - tracks.begin());
    const auto& t = *audio;
    if (!codecs::audio_decoder_available(t.codec)) {
        why = "no decoder for the " + (t.codec_name.empty() ? std::string("audio") : t.codec_name) + " track";
        status = af::error::unsupported_data_format;
        return nullptr;
    }
    s->decoder_ = codecs::make_audio_decoder(t.codec, t.sample_rate, t.channels, t.config, why);
    if (!s->decoder_ || t.samples.empty() || t.timescale == 0) {
        if (why.empty()) why = "the audio track has no packets";
        status = af::error::unsupported_data_format;
        return nullptr;
    }
    s->file_type_ = file_type_for(s->movie_.brand);

    // Packet starts in decoded frames, from the packets' durations.
    const double scale = s->decoder_->rate / static_cast<double>(t.timescale);
    s->starts_.reserve(t.samples.size() + 1);
    std::uint64_t at = 0;
    std::map<std::uint32_t, std::size_t> durations;
    for (const auto& sample : t.samples) {
        s->starts_.push_back(at);
        const auto frames = static_cast<std::uint64_t>(std::llround(sample.duration * scale));
        at += frames;
        ++durations[static_cast<std::uint32_t>(frames)];
        s->maximum_packet_ = std::max(s->maximum_packet_, static_cast<std::uint32_t>(sample.size));
    }
    s->starts_.push_back(at);
    s->frames_per_packet_ = std::max_element(durations.begin(), durations.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first;
    s->priming_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(std::llround(t.priming * (t.codec == fourcc("opus") ? 1.0 : scale)), at));
    // MP3 and FLAC tracks know their exact length; elsewhere it is the packets less the priming.
    const auto exact = s->movie_.brand == fourcc("mp3 ") || s->movie_.brand == fourcc("fLaC");
    s->playable_ = exact && t.duration > 0 ? std::min<std::uint64_t>(static_cast<std::uint64_t>(std::llround(t.duration * scale)), at - s->priming_)
                                           : at - s->priming_;
    s->remainder_ = static_cast<std::uint32_t>(at - s->priming_ - s->playable_);
    // Decoding restarts this far before a seek target: MP3's bit reservoir and filter bank need a
    // few frames; after Opus's 80 ms minimum CELT's predicted band energies are still about a
    // decibel off, so it gets half a second, after which they match a decode from the start.
    s->preroll_ = t.codec == fourcc("opus") ? 24000 : t.codec == fourcc("flac") ? 0 : 4 * 1152;
    s->buffer_start_ = 0;
    s->next_ = 0;
    status = 0;
    return s;
}

std::vector<std::uint8_t> Stream::packet(std::size_t index) const {
    if (index >= track().samples.size()) return {};
    return container::read_sample(read_, track(), track().samples[index]);
}

std::vector<std::uint8_t> Stream::asbd() const {
    std::vector<std::uint8_t> out(40, 0);
    const double rate = decoder_->rate;
    std::memcpy(out.data(), &rate, 8);
    auto format = track().codec;
    std::uint32_t flags = 0;
    if (format == fourcc(".mp3") && packet_count()) {
        // The layer of the first frame: '.mp1', '.mp2' or '.mp3'.
        const auto first = packet(0);
        if (first.size() >= 4) {
            const auto layer = (first[1] >> 1) & 3;
            format = layer == 3 ? fourcc(".mp1") : layer == 2 ? fourcc(".mp2") : fourcc(".mp3");
        }
    } else if (format == fourcc("flac")) {
        // kAppleLosslessFormatFlag_16/20/24/32BitSourceData
        flags = decoder_->bits <= 16 ? 1 : decoder_->bits <= 20 ? 2 : decoder_->bits <= 24 ? 3 : 4;
    }
    put32(out, 8, format);
    put32(out, 12, flags);
    put32(out, 16, 0);                       // mBytesPerPacket: variable
    put32(out, 20, frames_per_packet_);
    put32(out, 24, 0);                       // mBytesPerFrame
    put32(out, 28, decoder_->channels);
    put32(out, 32, 0);                       // mBitsPerChannel
    return out;
}

std::size_t Stream::packet_for(std::uint64_t decoded_frame) const {
    const auto found = std::upper_bound(starts_.begin(), starts_.end() - 1, decoded_frame);
    return found == starts_.begin() ? 0 : static_cast<std::size_t>(found - starts_.begin() - 1);
}

void Stream::seek(std::uint64_t decoded_frame) {
    next_ = packet_for(decoded_frame > preroll_ ? decoded_frame - preroll_ : 0);
    decoder_->reset();
    buffer_.clear();
    buffer_start_ = starts_[next_];
}

std::size_t Stream::read(std::uint64_t start, std::size_t count, std::vector<float>& out) {
    const std::uint64_t end_of_stream = priming_ + playable_;
    const std::uint64_t from = start + priming_;
    const std::uint64_t to = std::min<std::uint64_t>(from + count, end_of_stream);
    if (start >= playable_ || from >= to) return 0;
    const auto channels = decoder_->channels;
    const auto buffered = [&] { return buffer_.size() / channels; };
    if (from < buffer_start_ || from > buffer_start_ + buffered() + frames_per_packet_ * 2ULL) seek(from);
    // Decode until the buffer covers [from, to), dropping what lies before `from`.
    std::vector<float> decoded;
    while (buffer_start_ + buffered() < to && next_ < packet_count()) {
        const auto expected = static_cast<std::size_t>(starts_[next_ + 1] - starts_[next_]);
        decoded.clear();
        std::string why;
        const auto bytes = packet(next_);
        if (bytes.empty() || !decoder_->decode(bytes, decoded, why)) {
            last_error_ = bytes.empty() ? "a packet lies beyond the end of the file" : why;
            decoded.clear();
        }
        // A packet that decodes to nothing (MP3 before its bit reservoir fills, a rejected
        // packet) still occupies its duration.
        if (decoded.empty()) decoded.assign(expected * channels, 0.0f);
        buffer_.insert(buffer_.end(), decoded.begin(), decoded.end());
        ++next_;
        if (from > buffer_start_) {
            const auto drop = std::min<std::uint64_t>(from - buffer_start_, buffered());
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop * channels));
            buffer_start_ += drop;
        }
    }
    if (from > buffer_start_) {
        const auto drop = std::min<std::uint64_t>(from - buffer_start_, buffered());
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(drop * channels));
        buffer_start_ += drop;
    }
    if (buffer_start_ != from) return 0;   // the packets ended before `from`
    const auto got = static_cast<std::size_t>(std::min<std::uint64_t>(to - from, buffered()));
    out.insert(out.end(), buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(got * channels));
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(got * channels));
    buffer_start_ += got;
    return got;
}

} // namespace alchemy::gui::completion_kit::media::audio_stream
