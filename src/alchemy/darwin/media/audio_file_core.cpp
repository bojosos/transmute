#include "alchemy/darwin/media/audio_file_core.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

namespace alchemy::gui::completion_kit::media::audio_file {
namespace {

std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }
std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t(be32(p)) << 32) | be32(p + 4); }
void put32(std::vector<std::uint8_t>& out, std::uint32_t value) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }

// IEEE 754 80-bit extended (AIFF sample rates).
double extended(const std::uint8_t* p) {
    const int exponent = ((p[0] & 0x7f) << 8) | p[1];
    std::uint64_t mantissa = 0;
    for (int i = 0; i < 8; ++i) mantissa = (mantissa << 8) | p[2 + i];
    if (!exponent && !mantissa) return 0;
    const double value = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return (p[0] & 0x80) ? -value : value;
}

bool read_exact(const ReadAt& read, std::uint64_t offset, std::span<std::uint8_t> out) { return read(offset, out) == out.size(); }

std::int32_t parse_wave(const ReadAt& read, std::uint64_t size, Container& out, std::string& why) {
    out.file_type = four("WAVE");
    bool have_format = false, have_data = false;
    for (std::uint64_t at = 12; at + 8 <= size && !(have_format && have_data);) {
        std::array<std::uint8_t, 8> header{};
        if (!read_exact(read, at, header)) break;
        const auto id = be32(header.data());
        const std::uint64_t length = le32(header.data() + 4);
        const auto body = at + 8;
        if (id == four("fmt ")) {
            if (length < 16 || length > 4096) { why = "bad fmt chunk"; return error::invalid_file; }
            std::vector<std::uint8_t> fmt(static_cast<std::size_t>(length));
            if (!read_exact(read, body, fmt)) { why = "short fmt chunk"; return error::invalid_file; }
            auto tag = le16(fmt.data());
            const auto channels = le16(fmt.data() + 2);
            const auto rate = le32(fmt.data() + 4);
            const auto align = le16(fmt.data() + 12);
            auto bits = le16(fmt.data() + 14);
            if (tag == 0xfffe) {
                if (length < 40) { why = "short WAVE_FORMAT_EXTENSIBLE"; return error::invalid_file; }
                const auto valid = le16(fmt.data() + 18);
                out.channel_mask = le32(fmt.data() + 20);
                tag = le16(fmt.data() + 24);   // subformat GUID's first two bytes
                if (valid && valid <= bits) bits = valid;
            }
            if (tag != 1 && tag != 3) { why = "WAVE format tag " + std::to_string(tag) + " (compressed)"; return error::unsupported_data_format; }
            if (!channels || !rate || !align || align % channels) { why = "bad WAVE format"; return error::invalid_file; }
            auto& f = out.format;
            f.rate = rate;
            f.channels = channels;
            f.container = align / channels;
            f.bits = bits ? bits : f.container * 8;
            f.is_float = tag == 3;
            f.big_endian = false;
            f.is_signed = f.is_float || f.bits > 8;   // 8-bit WAVE is unsigned
            f.aligned_high = !f.is_float && f.bits < f.container * 8;   // WAVE samples are left-justified
            if ((f.is_float && f.bits != 32 && f.bits != 64) || f.container > 8 || (!f.is_float && (f.bits > 32 || f.container > 4))) {
                why = "unsupported WAVE sample size";
                return error::unsupported_data_format;
            }
            have_format = true;
        } else if (id == four("data")) {
            out.data_offset = body;
            out.data_bytes = std::min<std::uint64_t>(length, size - body);
            have_data = true;
        }
        at = body + length + (length & 1);
    }
    if (!have_format || !have_data) { why = "WAVE without fmt or data chunk"; return error::invalid_file; }
    return 0;
}

std::int32_t parse_aiff(const ReadAt& read, std::uint64_t size, bool compressed, Container& out, std::string& why) {
    out.file_type = compressed ? four("AIFC") : four("AIFF");
    bool have_format = false, have_data = false;
    std::uint64_t frames = 0;
    for (std::uint64_t at = 12; at + 8 <= size && !(have_format && have_data);) {
        std::array<std::uint8_t, 8> header{};
        if (!read_exact(read, at, header)) break;
        const auto id = be32(header.data());
        const std::uint64_t length = be32(header.data() + 4);
        const auto body = at + 8;
        if (id == four("COMM")) {
            if (length < 18 || length > 4096) { why = "bad COMM chunk"; return error::invalid_file; }
            std::vector<std::uint8_t> comm(static_cast<std::size_t>(length));
            if (!read_exact(read, body, comm)) { why = "short COMM chunk"; return error::invalid_file; }
            auto& f = out.format;
            f.channels = be16(comm.data());
            frames = be32(comm.data() + 2);
            f.bits = be16(comm.data() + 6);
            f.rate = extended(comm.data() + 8);
            f.big_endian = true;
            f.is_signed = true;
            f.is_float = false;
            std::uint32_t compression = four("NONE");
            if (compressed) {
                if (length < 22) { why = "AIFC COMM without compression type"; return error::invalid_file; }
                compression = be32(comm.data() + 18);
            }
            switch (compression) {
            case four("NONE"): case four("twos"): break;
            case four("sowt"): f.big_endian = false; break;
            case four("raw "): f.is_signed = false; break;
            case four("in24"): f.bits = 24; break;
            case four("in32"): f.bits = 32; break;
            case four("42ni"): f.bits = 24; f.big_endian = false; break;
            case four("23ni"): f.bits = 32; f.big_endian = false; break;
            case four("fl32"): case four("FL32"): f.is_float = true; f.bits = 32; break;
            case four("fl64"): case four("FL64"): f.is_float = true; f.bits = 64; break;
            default:
                why = "AIFC compression type is not linear PCM";
                return error::unsupported_data_format;
            }
            if (!f.channels || !(f.rate > 0) || !f.bits || f.bits > (f.is_float ? 64U : 32U)) { why = "bad AIFF format"; return error::invalid_file; }
            f.container = (f.bits + 7) / 8;
            f.aligned_high = !f.is_float && f.bits < f.container * 8;   // AIFF samples are left-justified
            have_format = true;
        } else if (id == four("SSND")) {
            std::array<std::uint8_t, 8> ssnd{};
            if (length < 8 || !read_exact(read, body, ssnd)) { why = "bad SSND chunk"; return error::invalid_file; }
            const auto offset = be32(ssnd.data());
            out.data_offset = body + 8 + offset;
            out.data_bytes = length >= 8 + offset ? std::min<std::uint64_t>(length - 8 - offset, size - std::min(size, out.data_offset)) : 0;
            have_data = true;
        }
        at = body + length + (length & 1);
    }
    if (!have_format) { why = "AIFF without COMM chunk"; return error::invalid_file; }
    if (!have_data) { out.data_offset = size; out.data_bytes = 0; }
    out.data_bytes = std::min<std::uint64_t>(out.data_bytes, frames * out.format.frame_bytes());
    return 0;
}

std::int32_t parse_caf(const ReadAt& read, std::uint64_t size, Container& out, std::string& why) {
    out.file_type = four("caff");
    bool have_format = false, have_data = false;
    for (std::uint64_t at = 8; at + 12 <= size && !(have_format && have_data);) {
        std::array<std::uint8_t, 12> header{};
        if (!read_exact(read, at, header)) break;
        const auto id = be32(header.data());
        const auto raw_length = static_cast<std::int64_t>(be64(header.data() + 4));
        const auto body = at + 12;
        const std::uint64_t length = raw_length < 0 ? size - body : static_cast<std::uint64_t>(raw_length);
        if (id == four("desc")) {
            std::array<std::uint8_t, 32> desc{};
            if (length < 32 || !read_exact(read, body, desc)) { why = "bad desc chunk"; return error::invalid_file; }
            const double rate = std::bit_cast<double>(be64(desc.data()));
            const auto format_id = be32(desc.data() + 8), flags = be32(desc.data() + 12), bytes_per_packet = be32(desc.data() + 16),
                frames_per_packet = be32(desc.data() + 20), channels = be32(desc.data() + 24), bits = be32(desc.data() + 28);
            if (format_id != four("lpcm")) { why = "CAF data is not linear PCM"; return error::unsupported_data_format; }
            if (!channels || frames_per_packet != 1 || !bytes_per_packet || bytes_per_packet % channels || !(rate > 0)) { why = "bad CAF format"; return error::invalid_file; }
            auto& f = out.format;
            f.rate = rate;
            f.channels = channels;
            f.container = bytes_per_packet / channels;
            f.bits = bits ? bits : f.container * 8;
            f.is_float = (flags & 1) != 0;         // kCAFLinearPCMFormatFlagIsFloat
            f.big_endian = (flags & 2) == 0;       // kCAFLinearPCMFormatFlagIsLittleEndian
            f.is_signed = true;
            f.aligned_high = false;
            if ((f.is_float && f.bits != 32 && f.bits != 64) || (!f.is_float && (f.bits > 32 || f.container > 4 || f.bits > f.container * 8))) {
                why = "unsupported CAF sample size";
                return error::unsupported_data_format;
            }
            have_format = true;
        } else if (id == four("data")) {
            if (length < 4) { why = "bad data chunk"; return error::invalid_file; }
            out.data_offset = body + 4;   // after mEditCount
            out.data_bytes = std::min<std::uint64_t>(length - 4, size - std::min(size, out.data_offset));
            have_data = true;
        }
        if (raw_length < 0) break;
        at = body + length;
    }
    if (!have_format || !have_data) { why = "CAF without desc or data chunk"; return error::invalid_file; }
    return 0;
}

std::uint64_t read_raw(const std::uint8_t* p, std::uint32_t bytes, bool big_endian) {
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < bytes; ++i) value |= std::uint64_t(p[big_endian ? i : bytes - 1 - i]) << (8 * (bytes - 1 - i));
    return value;
}
void write_raw(std::uint8_t* p, std::uint32_t bytes, bool big_endian, std::uint64_t value) {
    for (std::uint32_t i = 0; i < bytes; ++i) p[big_endian ? bytes - 1 - i : i] = static_cast<std::uint8_t>(value >> (8 * i));
}

double sample_in(const Pcm& f, const std::uint8_t* p) {
    const auto raw = read_raw(p, f.container, f.big_endian);
    if (f.is_float) return f.container == 8 ? std::bit_cast<double>(raw) : static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)));
    const auto container_bits = f.container * 8;
    std::uint64_t value = f.aligned_high ? raw >> (container_bits - f.bits) : raw & ((f.bits >= 64 ? ~0ULL : (1ULL << f.bits) - 1));
    const double scale = std::ldexp(1.0, static_cast<int>(f.bits) - 1);
    if (!f.is_signed) return (static_cast<double>(value) - scale) / scale;
    const auto sign = 1ULL << (f.bits - 1);
    const auto signed_value = static_cast<std::int64_t>(value ^ sign) - static_cast<std::int64_t>(sign);
    return static_cast<double>(signed_value) / scale;
}

void sample_out(const Pcm& f, std::uint8_t* p, double value) {
    if (f.is_float) {
        if (f.container == 8) write_raw(p, 8, f.big_endian, std::bit_cast<std::uint64_t>(value));
        else write_raw(p, 4, f.big_endian, std::bit_cast<std::uint32_t>(static_cast<float>(value)));
        return;
    }
    const double scale = std::ldexp(1.0, static_cast<int>(f.bits) - 1);
    const double clipped = std::clamp(std::round(value * scale), -scale, scale - 1);
    auto integer = static_cast<std::int64_t>(clipped);
    std::uint64_t raw = f.is_signed ? static_cast<std::uint64_t>(integer) & ((f.bits >= 64 ? ~0ULL : (1ULL << f.bits) - 1))
                                    : static_cast<std::uint64_t>(integer + static_cast<std::int64_t>(scale));
    if (f.aligned_high) raw <<= (f.container * 8 - f.bits);
    write_raw(p, f.container, f.big_endian, raw);
}

} // namespace

std::uint32_t Pcm::flags() const {
    std::uint32_t value = 0;
    if (is_float) value |= flag::is_float;
    if (big_endian) value |= flag::big_endian;
    if (!is_float && is_signed) value |= flag::signed_integer;
    if (bits == container * 8) value |= flag::packed;
    else if (aligned_high) value |= flag::aligned_high;
    if (non_interleaved) value |= flag::non_interleaved;
    return value;
}

std::optional<Pcm> pcm_from_asbd(std::span<const std::uint8_t> asbd, std::string& why) {
    if (asbd.size() < 40) { why = "short stream description"; return std::nullopt; }
    double rate{};
    std::memcpy(&rate, asbd.data(), 8);
    const auto format_id = le32(asbd.data() + 8), flags = le32(asbd.data() + 12), frames_per_packet = le32(asbd.data() + 20),
        bytes_per_frame = le32(asbd.data() + 24), channels = le32(asbd.data() + 28), bits = le32(asbd.data() + 32);
    if (format_id != four("lpcm")) { why = "not linear PCM"; return std::nullopt; }
    if ((flags >> 7) & 0x3f) { why = "fixed-point samples"; return std::nullopt; }
    if (!channels || channels > 64 || frames_per_packet != 1 || !bytes_per_frame || !(rate > 0)) { why = "incomplete linear PCM description"; return std::nullopt; }
    Pcm f;
    f.rate = rate;
    f.channels = channels;
    f.non_interleaved = (flags & flag::non_interleaved) != 0;
    if (!f.non_interleaved && bytes_per_frame % channels) { why = "frame size is not a multiple of the channels"; return std::nullopt; }
    f.container = f.non_interleaved ? bytes_per_frame : bytes_per_frame / channels;
    f.bits = bits ? bits : f.container * 8;
    f.is_float = (flags & flag::is_float) != 0;
    f.big_endian = (flags & flag::big_endian) != 0;
    f.is_signed = f.is_float || (flags & flag::signed_integer) != 0;
    f.aligned_high = (flags & flag::aligned_high) != 0;
    if (f.is_float ? !((f.bits == 32 && f.container == 4) || (f.bits == 64 && f.container == 8)) : (f.bits > 32 || f.container > 4 || f.bits > f.container * 8)) {
        why = "unsupported sample size";
        return std::nullopt;
    }
    return f;
}

std::vector<std::uint8_t> asbd_from_pcm(const Pcm& f) {
    std::vector<std::uint8_t> out(8);
    std::memcpy(out.data(), &f.rate, 8);
    put32(out, four("lpcm"));
    put32(out, f.flags());
    put32(out, f.frame_bytes());   // mBytesPerPacket
    put32(out, 1);                 // mFramesPerPacket
    put32(out, f.frame_bytes());   // mBytesPerFrame
    put32(out, f.channels);
    put32(out, f.bits);
    put32(out, 0);
    return out;
}

std::int32_t parse(const ReadAt& read, std::uint64_t size, Container& out, std::string& why) {
    std::array<std::uint8_t, 12> head{};
    if (size < 12 || !read_exact(read, 0, head)) { why = "file too short"; return error::invalid_file; }
    const auto magic = be32(head.data()), kind = be32(head.data() + 8);
    if (magic == four("RIFF") && kind == four("WAVE")) return parse_wave(read, size, out, why);
    if (magic == four("FORM") && (kind == four("AIFF") || kind == four("AIFC"))) return parse_aiff(read, size, kind == four("AIFC"), out, why);
    if (magic == four("caff") && be16(head.data() + 4) == 1) return parse_caf(read, size, out, why);
    why = "not a WAVE, AIFF or CAF file";
    return error::unsupported_file_type;
}

std::vector<double> decode(const Pcm& f, const std::vector<std::span<const std::uint8_t>>& buffers, std::size_t frames) {
    std::vector<double> out(frames * f.channels);
    for (std::size_t frame = 0; frame < frames; ++frame)
        for (std::uint32_t channel = 0; channel < f.channels; ++channel) {
            const auto& buffer = buffers[f.non_interleaved ? channel : 0];
            const auto at = frame * f.frame_bytes() + (f.non_interleaved ? 0 : channel * f.container);
            out[frame * f.channels + channel] = sample_in(f, buffer.data() + at);
        }
    return out;
}

void encode(const Pcm& f, std::span<const double> in, std::uint32_t source_channels, std::size_t frames, const std::vector<std::span<std::uint8_t>>& buffers) {
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto* row = in.data() + frame * source_channels;
        double mono = 0;
        if (f.channels == 1 && source_channels > 1) {
            for (std::uint32_t c = 0; c < source_channels; ++c) mono += row[c];
            mono /= source_channels;
        }
        for (std::uint32_t channel = 0; channel < f.channels; ++channel) {
            const double value = source_channels == f.channels ? row[channel] : source_channels == 1 ? row[0] : mono;
            auto& buffer = buffers[f.non_interleaved ? channel : 0];
            sample_out(f, buffer.data() + frame * f.frame_bytes() + (f.non_interleaved ? 0 : channel * f.container), value);
        }
    }
}

bool convertible(const Pcm& from, const Pcm& to) {
    return from.rate == to.rate && (from.channels == to.channels || from.channels == 1 || to.channels == 1);
}

std::vector<std::uint8_t> layout_for_mask(std::uint32_t mask, std::uint32_t channels) {
    std::vector<std::uint32_t> labels;
    for (std::uint32_t bit = 0; bit < 18 && labels.size() < channels; ++bit)
        if (mask & (1U << bit)) labels.push_back(bit + 1);   // kAudioChannelLabel_* follow the WAVE speaker order
    if (labels.size() != channels) return {};
    std::vector<std::uint8_t> out;
    put32(out, 0);   // kAudioChannelLayoutTag_UseChannelDescriptions
    put32(out, 0);
    put32(out, channels);
    for (const auto label : labels) {
        put32(out, label);
        for (int i = 0; i < 4; ++i) put32(out, 0);   // flags, coordinates
    }
    return out;
}

} // namespace alchemy::gui::completion_kit::media::audio_file
