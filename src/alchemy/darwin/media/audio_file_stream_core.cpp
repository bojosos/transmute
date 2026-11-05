#include "alchemy/darwin/media/audio_file_stream_core.hpp"

#include "alchemy/darwin/media/audio_file_core.hpp"
#include "alchemy/darwin/media/media_container.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace alchemy::gui::completion_kit::media::audio_file_stream {
namespace {

using audio_file::four;

// A header bigger than this without its data chunk is not a stream Alchemy can follow.
constexpr std::size_t header_limit = 1 << 20;
// What the PCM parsers are told the file's size is: the stream's end is not known.
constexpr std::uint64_t unbounded = 1ULL << 62;
// The largest MPEG audio frame (MPEG-2.5 layer II at 160 kbit/s and 8 kHz, padded).
constexpr std::uint32_t mpeg_frame_bound = 2881;

std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }

template <class T> void put(std::vector<std::uint8_t>& out, T value) {
    const auto at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
}
template <class T> T get(std::span<const std::uint8_t> in, std::size_t at) {
    T value{};
    if (at + sizeof(T) <= in.size()) std::memcpy(&value, in.data() + at, sizeof(T));
    return value;
}

// kAudioFileStreamProperty_*
enum : std::uint32_t {
    ready_to_produce_packets = four("redy"), file_format = four("ffmt"), data_format = four("dfmt"), format_list = four("flst"),
    magic_cookie = four("mgic"), byte_count = four("bcnt"), packet_count = four("pcnt"), maximum_packet_size = four("psze"),
    data_offset = four("doff"), channel_layout = four("cmap"), packet_to_frame = four("pkfr"), frame_to_packet = four("frpk"),
    packet_to_byte = four("pkby"), byte_to_packet = four("bypk"), packet_table_info = four("pnfo"), packet_size_upper_bound = four("pkub"),
    average_bytes_per_packet = four("abpp"), bit_rate = four("brat"), info_dictionary = four("info"),
};

} // namespace

void Parser::drop(std::size_t n) {
    n = std::min(n, pending_.size());
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    pending_at_ += n;
}

void Parser::skip_pending() {
    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(skip_, pending_.size()));
    drop(n);
    skip_ -= n;   // the rest is dropped as it arrives
}

std::int32_t Parser::parse(std::span<const std::uint8_t> bytes, bool discontinuity, std::vector<Event>& out) {
    if (failed_) return failed_;
    if (discontinuity && ready_) {
        // The bytes held for a partial packet do not continue into these.
        pending_at_ += pending_.size();
        pending_.clear();
        synced_ = false;
    }
    pending_.insert(pending_.end(), bytes.begin(), bytes.end());
    if (kind_ == Kind::unknown) {
        if (const auto result = sniff()) return failed_ = result;
        if (kind_ == Kind::unknown) return 0;
    }
    const auto result = kind_ == Kind::pcm ? parse_pcm(out) : parse_mpeg(out);
    if (result && !ready_) failed_ = result;
    return result;
}

std::int32_t Parser::sniff() {
    if (pending_.size() < 4) return 0;
    const auto* p = pending_.data();
    const auto head = be32(p);
    if (std::memcmp(p, "ID3", 3) == 0 || container::mpeg_frame(p).bytes) {
        kind_ = Kind::mpeg;
        return 0;
    }
    if (head == four("RIFF") || head == four("FORM") || head == four("caff")) {
        kind_ = Kind::pcm;
        return 0;
    }
    if (p[0] == 0xff && (p[1] & 0xf6) == 0xf0) return error::unsupported_data_format;   // ADTS AAC
    // A hint of MPEG audio lets the parser look for the first frame past leading junk.
    if (hint_ == four("MPG1") || hint_ == four("MPG2") || hint_ == four("MPG3")) {
        kind_ = Kind::mpeg;
        return 0;
    }
    return error::unsupported_file_type;
}

void Parser::announce(std::vector<Event>& out) {
    ready_ = true;
    for (const auto id : {file_format, data_format, format_list, data_offset}) out.push_back(Event{id});
    if (byte_count_) out.push_back(Event{byte_count});
    if (packet_count_) out.push_back(Event{packet_count});
    if (packet_count_ && frame_bytes_ == 0) out.push_back(Event{packet_table_info});
    if (!layout_.empty()) out.push_back(Event{channel_layout});
    out.push_back(Event{ready_to_produce_packets});
}

// ---- linear PCM files --------------------------------------------------------------------------

std::int32_t Parser::parse_pcm(std::vector<Event>& out) {
    if (!ready_) {
        const auto read = [this](std::uint64_t offset, std::span<std::uint8_t> to) -> std::size_t {
            if (offset >= pending_.size()) return 0;
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(to.size(), pending_.size() - offset));
            std::memcpy(to.data(), pending_.data() + offset, n);
            return n;
        };
        audio_file::Container c;
        std::string why;
        const auto result = audio_file::parse(read, unbounded, c, why);
        // A header cut short reads as an invalid file: wait for the rest.
        if (result == audio_file::error::invalid_file && pending_.size() < header_limit) return 0;
        if (result) return result == audio_file::error::invalid_file ? error::invalid_file : result;
        bool known = c.data_bytes < (unbounded >> 1);
        if (c.file_type == four("WAVE")) {
            const auto declared = le32(pending_.data() + c.data_offset - 4);
            known = declared != 0 && declared != 0xffffffffU;   // streamed WAVE leaves the size open
        }
        file_type_ = c.file_type;
        rate_ = c.format.rate;
        channels_ = c.format.channels;
        frame_bytes_ = maximum_packet_ = c.format.frame_bytes();
        asbd_ = audio_file::asbd_from_pcm(c.format);
        layout_ = audio_file::layout_for_mask(c.channel_mask, c.format.channels);
        data_offset_ = c.data_offset;
        if (known) {
            byte_count_ = c.data_bytes - c.data_bytes % frame_bytes_;
            packet_count_ = c.data_bytes / frame_bytes_;
        }
        announce(out);
        skip_ = data_offset_;
        position_ = 0;
    }
    skip_pending();
    if (skip_) return 0;
    auto available = pending_.size() - pending_.size() % frame_bytes_;
    if (byte_count_) available = static_cast<std::size_t>(std::min<std::uint64_t>(available, *byte_count_ - std::min(*byte_count_, position_)));
    if (available) {
        Event batch;
        batch.bytes.assign(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(available));
        batch.packets = static_cast<std::uint32_t>(available / frame_bytes_);
        drop(available);
        position_ += available;
        next_packet_ += batch.packets;
        seen_bytes_ += available;
        seen_packets_ += batch.packets;
        out.push_back(std::move(batch));
    }
    // What follows the audio data (more chunks) is not audio.
    if (byte_count_ && position_ >= *byte_count_) drop(pending_.size());
    return 0;
}

// ---- MPEG audio ---------------------------------------------------------------------------------

std::uint32_t Parser::signature(std::size_t at) const {
    const auto* p = pending_.data() + at;
    const auto channels = ((p[3] >> 6) & 3) == 3 ? 1U : 2U;
    return (std::uint32_t(p[1] & 0xfe) << 16) | (std::uint32_t(p[2] & 0x0c) << 8) | channels;   // version, layer; rate; channels
}

std::int32_t Parser::parse_mpeg(std::vector<Event>& out) {
    // ID3v2 tags in front of the first frame.
    while (first_frame_) {
        skip_pending();
        if (skip_) return 0;
        if (pending_.size() < 3) return 0;
        if (std::memcmp(pending_.data(), "ID3", 3) != 0) break;
        if (pending_.size() < 10) return 0;
        const auto* t = pending_.data();
        skip_ = 10 + ((std::uint64_t(t[6] & 0x7f) << 21) | ((t[7] & 0x7f) << 14) | ((t[8] & 0x7f) << 7) | (t[9] & 0x7f)) + ((t[5] & 0x10) ? 10 : 0);
    }
    skip_pending();
    if (skip_) return 0;

    Event batch;
    std::size_t at = 0;
    while (at + 4 <= pending_.size()) {
        const auto frame = container::mpeg_frame(pending_.data() + at);
        if (!frame.bytes || (!first_frame_ && signature(at) != signature_)) {
            synced_ = false;   // junk, or a header that does not belong to this stream
            ++at;
            continue;
        }
        if (at + frame.bytes > pending_.size()) break;
        if (!synced_) {
            // In step only when the next frame's header follows where this frame says it ends.
            if (at + frame.bytes + 4 > pending_.size()) break;
            const auto next = container::mpeg_frame(pending_.data() + at + frame.bytes);
            if (!next.bytes || signature(at + frame.bytes) != signature(at)) {
                ++at;
                continue;
            }
            synced_ = true;
        }
        const auto* p = pending_.data() + at;
        if (first_frame_) {
            first_frame_ = false;
            signature_ = signature(at);
            const auto layer = (p[1] >> 1) & 3;   // 1: III, 2: II, 3: I
            file_type_ = layer == 1 ? four("MPG3") : layer == 2 ? four("MPG2") : four("MPG1");
            rate_ = frame.rate;
            channels_ = frame.channels;
            frames_per_packet_ = frame.samples;
            asbd_.clear();
            put(asbd_, rate_);
            put(asbd_, layer == 1 ? four(".mp3") : layer == 2 ? four(".mp2") : four(".mp1"));
            put(asbd_, std::uint32_t{0});          // mFormatFlags
            put(asbd_, std::uint32_t{0});          // mBytesPerPacket (variable)
            put(asbd_, frames_per_packet_);
            put(asbd_, std::uint32_t{0});          // mBytesPerFrame
            put(asbd_, channels_);
            put(asbd_, std::uint32_t{0});          // mBitsPerChannel
            put(asbd_, std::uint32_t{0});          // mReserved
            layout_ = audio_file::layout_for_mask(0, channels_);
            // A Xing/Info frame carries no audio: its frame and byte counts, and the LAME tag's
            // encoder delay and padding (the decoder adds 529 samples of its own).
            const std::size_t side = (p[1] & 0x08) ? (frame.channels == 1 ? 17 : 32) : (frame.channels == 1 ? 9 : 17);
            if (frame.bytes >= 4 + side + 8 && (std::memcmp(p + 4 + side, "Xing", 4) == 0 || std::memcmp(p + 4 + side, "Info", 4) == 0)) {
                const auto* x = p + 4 + side + 8;
                const auto flags = be32(p + 4 + side + 4);
                std::size_t tag = 4 + side + 8;
                if ((flags & 1) && tag + 4 <= frame.bytes) packet_count_ = be32(x);
                if (flags & 1) tag += 4;
                if ((flags & 2) && tag + 4 <= frame.bytes) {
                    const auto bytes = be32(p + tag);
                    if (bytes > frame.bytes) byte_count_ = bytes - frame.bytes;
                }
                if (flags & 2) tag += 4;
                if (flags & 4) tag += 100;
                if (flags & 8) tag += 4;
                if (tag + 24 <= frame.bytes && packet_count_) {
                    const auto delay = (std::uint32_t(p[tag + 21]) << 4) | (p[tag + 22] >> 4);
                    const auto padding = (std::uint32_t(p[tag + 22] & 0x0f) << 8) | p[tag + 23];
                    if (delay || padding) {
                        priming_ = delay + 529;
                        remainder_ = padding;
                    }
                }
                at += frame.bytes;
                continue;
            }
        }
        if (!ready_) {
            data_offset_ = pending_at_ + at;
            announce(out);
        }
        const auto offset = pending_at_ + at - data_offset_;
        if (exact_ && next_packet_ == offsets_.size()) offsets_.push_back(offset);
        batch.descriptions.push_back(PacketDescription{static_cast<std::int64_t>(batch.bytes.size()), 0, frame.bytes});
        batch.bytes.insert(batch.bytes.end(), p, p + frame.bytes);
        ++batch.packets;
        ++next_packet_;
        ++seen_packets_;
        seen_bytes_ += frame.bytes;
        maximum_packet_ = std::max(maximum_packet_, frame.bytes);
        at += frame.bytes;
    }
    drop(at);
    if (batch.packets) out.push_back(std::move(batch));
    return 0;
}

// ---- seeking and properties ----------------------------------------------------------------------

double Parser::average_packet_bytes() const {
    if (frame_bytes_) return frame_bytes_;
    if (byte_count_ && packet_count_ && *packet_count_) return static_cast<double>(*byte_count_) / static_cast<double>(*packet_count_);
    if (seen_packets_) return static_cast<double>(seen_bytes_) / static_cast<double>(seen_packets_);
    return maximum_packet_;
}

std::int32_t Parser::seek(std::int64_t packet, std::int64_t& data_byte_offset, bool& estimated) {
    if (!ready_) return error::data_unavailable;
    if (packet < 0 || (packet_count_ && static_cast<std::uint64_t>(packet) > *packet_count_)) return error::invalid_packet_offset;
    const auto n = static_cast<std::uint64_t>(packet);
    if (kind_ == Kind::pcm) {
        estimated = false;
        position_ = n * frame_bytes_;
    } else if (n < offsets_.size()) {
        estimated = false;
        position_ = offsets_[n];
    } else {
        estimated = true;
        position_ = static_cast<std::uint64_t>(std::llround(static_cast<double>(n) * average_packet_bytes()));
    }
    data_byte_offset = static_cast<std::int64_t>(position_);
    pending_.clear();
    pending_at_ = data_offset_ + position_;
    skip_ = 0;
    synced_ = false;
    exact_ = !estimated;
    next_packet_ = n;
    return 0;
}

bool Parser::known_property(std::uint32_t id) const {
    switch (id) {
    case ready_to_produce_packets: case file_format: case data_format: case format_list: case magic_cookie: case byte_count:
    case packet_count: case maximum_packet_size: case data_offset: case channel_layout: case packet_to_frame: case frame_to_packet:
    case packet_to_byte: case byte_to_packet: case packet_table_info: case packet_size_upper_bound: case average_bytes_per_packet:
    case bit_rate: case info_dictionary:
        return true;
    default:
        return false;
    }
}

std::int32_t Parser::property(std::uint32_t id, std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out) const {
    out.clear();
    if (!known_property(id)) return error::unsupported_property;
    if (id == ready_to_produce_packets) {
        put(out, std::uint32_t{ready_ ? 1U : 0U});
        return 0;
    }
    if (!ready_) return error::data_unavailable;
    const auto fpp = static_cast<std::int64_t>(frames_per_packet_);
    switch (id) {
    case file_format: put(out, file_type_); return 0;
    case data_format: out = asbd_; return 0;
    case format_list:   // AudioFormatListItem {ASBD; AudioChannelLayoutTag kAudioChannelLayoutTag_Unknown | channels}
        out = asbd_;
        put(out, 0xffff0000U | channels_);
        out.resize(48);
        return 0;
    case byte_count:
        if (!byte_count_) return error::value_unknown;
        put(out, *byte_count_);
        return 0;
    case packet_count:
        if (!packet_count_) return error::value_unknown;
        put(out, *packet_count_);
        return 0;
    case maximum_packet_size: put(out, maximum_packet_); return 0;
    case packet_size_upper_bound: put(out, frame_bytes_ ? frame_bytes_ : mpeg_frame_bound); return 0;
    case data_offset: put(out, static_cast<std::int64_t>(data_offset_)); return 0;
    case average_bytes_per_packet: put(out, average_packet_bytes()); return 0;
    case bit_rate: {
        const double seconds_per_packet = static_cast<double>(frames_per_packet_) / rate_;
        put(out, static_cast<std::uint32_t>(std::llround(average_packet_bytes() * 8 / seconds_per_packet)));
        return 0;
    }
    case channel_layout:
        if (layout_.empty()) return error::unsupported_property;
        out = layout_;
        return 0;
    case packet_table_info:   // {SInt64 mNumberValidFrames; SInt32 mPrimingFrames; SInt32 mRemainderFrames}
        if (!packet_count_) return error::value_unknown;
        put(out, static_cast<std::int64_t>(*packet_count_ * frames_per_packet_) - priming_.value_or(0) - remainder_.value_or(0));
        put(out, static_cast<std::int32_t>(priming_.value_or(0)));
        put(out, static_cast<std::int32_t>(remainder_.value_or(0)));
        return 0;
    case packet_to_frame: case frame_to_packet: {
        // AudioFramePacketTranslation {SInt64 mFrame; SInt64 mPacket; UInt32 mFrameOffsetInPacket}
        if (in.size() < 24) return error::bad_property_size;
        auto frame = get<std::int64_t>(in, 0), packet = get<std::int64_t>(in, 8);
        std::uint32_t offset = 0;
        if (id == packet_to_frame) frame = packet * fpp;
        else {
            packet = frame / fpp;
            offset = static_cast<std::uint32_t>(frame % fpp);
        }
        put(out, frame);
        put(out, packet);
        put(out, offset);
        put(out, std::uint32_t{0});
        return 0;
    }
    case packet_to_byte: case byte_to_packet: {
        // AudioBytePacketTranslation {SInt64 mByte; SInt64 mPacket; UInt32 mByteOffsetInPacket; UInt32 mFlags (1: estimate)}
        if (in.size() < 24) return error::bad_property_size;
        auto byte = get<std::int64_t>(in, 0), packet = get<std::int64_t>(in, 8);
        std::uint32_t offset = 0, flags = 0;
        const auto average = average_packet_bytes();
        if (id == packet_to_byte) {
            if (frame_bytes_) byte = packet * frame_bytes_;
            else if (packet >= 0 && static_cast<std::uint64_t>(packet) < offsets_.size()) byte = static_cast<std::int64_t>(offsets_[static_cast<std::size_t>(packet)]);
            else {
                byte = std::llround(static_cast<double>(packet) * average);
                flags = 1;
            }
        } else if (frame_bytes_) {
            packet = byte / frame_bytes_;
            offset = static_cast<std::uint32_t>(byte % frame_bytes_);
        } else if (byte >= 0 && !offsets_.empty() && static_cast<std::uint64_t>(byte) < offsets_.back() + maximum_packet_) {
            const auto it = std::upper_bound(offsets_.begin(), offsets_.end(), static_cast<std::uint64_t>(byte)) - 1;
            packet = it - offsets_.begin();
            offset = static_cast<std::uint32_t>(static_cast<std::uint64_t>(byte) - *it);
            if (it + 1 == offsets_.end()) flags = 1;   // past the last packet parsed: its size is not known
        } else {
            packet = average > 0 ? static_cast<std::int64_t>(static_cast<double>(byte) / average) : 0;
            offset = static_cast<std::uint32_t>(std::max<std::int64_t>(0, byte - std::llround(static_cast<double>(packet) * average)));
            flags = 1;
        }
        put(out, byte);
        put(out, packet);
        put(out, offset);
        put(out, flags);
        return 0;
    }
    default:   // a magic cookie (none for PCM or MPEG audio), the info dictionary
        return error::unsupported_property;
    }
}

} // namespace alchemy::gui::completion_kit::media::audio_file_stream
