#include "alchemy/darwin/media/packet_file.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace alchemy::gui::completion_kit::media::packet_file {

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t four(const char (&code)[5]) {
    return (std::uint32_t(std::uint8_t(code[0])) << 24) | (std::uint32_t(std::uint8_t(code[1])) << 16) |
           (std::uint32_t(std::uint8_t(code[2])) << 8) | std::uint32_t(std::uint8_t(code[3]));
}

void put(Bytes& out, std::uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void append(Bytes& out, const Bytes& more) { out.insert(out.end(), more.begin(), more.end()); }
Bytes box(std::uint32_t type, const Bytes& payload) {
    Bytes out;
    put(out, 8 + payload.size(), 4);
    put(out, type, 4);
    append(out, payload);
    return out;
}
// A full box: version and flags, then the payload.
Bytes full(std::uint32_t type, int version, std::uint32_t flags, const Bytes& payload) {
    Bytes body;
    put(body, static_cast<std::uint64_t>(version), 1);
    put(body, flags, 3);
    append(body, payload);
    return box(type, body);
}
// A BER integer: seven bits a byte, most significant first, the high bit on all but the last.
void put_ber(Bytes& out, std::uint64_t value) {
    std::uint8_t digits[10];
    int n = 0;
    do { digits[n++] = static_cast<std::uint8_t>(value & 0x7f); value >>= 7; } while (value);
    while (n--) out.push_back(static_cast<std::uint8_t>(digits[n] | (n ? 0x80 : 0)));
}
// The unity matrix of mvhd and tkhd.
void put_matrix(Bytes& out) {
    for (const std::uint32_t v : {0x00010000u, 0u, 0u, 0u, 0x00010000u, 0u, 0u, 0u, 0x40000000u}) put(out, v, 4);
}

} // namespace

bool writable(std::uint32_t file_type, std::uint32_t format_id) {
    return format_id == four("alac") && (file_type == four("caff") || file_type == four("m4af") || file_type == four("mp4f"));
}

std::optional<Writer> Writer::make(const Format& format, std::string& why) {
    if (!writable(format.file_type, format.format_id)) { why = "this format is not written into this file type"; return std::nullopt; }
    if (format.flags < 1 || format.flags > 4) { why = "Apple Lossless needs a source bit depth (kAppleLosslessFormatFlag_16BitSourceData to _32BitSourceData)"; return std::nullopt; }
    if (format.channels < 1 || format.channels > 8) { why = "Apple Lossless holds 1-8 channels"; return std::nullopt; }
    if (!(format.rate >= 1 && format.rate <= 4294967295.0) || format.rate != std::floor(format.rate)) { why = "an integer sample rate is needed"; return std::nullopt; }
    Writer w;
    w.format_ = format;
    if (!w.format_.frames_per_packet) w.format_.frames_per_packet = 4096;
    if (w.format_.frames_per_packet > 16384) { why = "Apple Lossless packets hold at most 16384 frames"; return std::nullopt; }
    auto& h = w.header_;
    if (format.file_type == four("caff")) {
        h = {'c', 'a', 'f', 'f', 0, 1, 0, 0};
        put(h, four("desc"), 4);
        put(h, 32, 8);
        put(h, std::bit_cast<std::uint64_t>(format.rate), 8);
        put(h, format.format_id, 4);
        put(h, format.flags, 4);
        put(h, 0, 4);                                 // mBytesPerPacket: variable
        put(h, w.format_.frames_per_packet, 4);
        put(h, format.channels, 4);
        put(h, 0, 4);                                 // mBitsPerChannel: 0 for compressed data
        put(h, four("data"), 4);
        w.size_field_ = h.size();
        put(h, 0xffffffffffffffffULL, 8);             // -1 until finish()
        put(h, 0, 4);                                 // mEditCount
    } else {
        Bytes brands;
        const bool m4a = format.file_type == four("m4af");
        put(brands, m4a ? four("M4A ") : four("mp42"), 4);
        put(brands, 0, 4);
        for (const auto brand : {four("M4A "), four("mp42"), four("isom")}) put(brands, brand, 4);
        h = box(four("ftyp"), brands);
        put(h, 1, 4);                                 // size 1: a 64-bit size follows
        put(h, four("mdat"), 4);
        w.size_field_ = h.size();
        put(h, 16, 8);
    }
    return w;
}

void Writer::add_packet(std::uint32_t bytes, std::uint32_t frames) {
    if (!frames) frames = format_.frames_per_packet;
    sizes_.push_back(bytes);
    durations_.push_back(frames);
    data_bytes_ += bytes;
    frames_ += frames;
    maximum_ = std::max(maximum_, bytes);
}

std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> Writer::finish() const {
    return format_.file_type == four("caff") ? finish_caf() : finish_mp4();
}

std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> Writer::finish_caf() const {
    std::vector<std::pair<std::uint64_t, Bytes>> writes;
    Bytes size;
    put(size, 4 + data_bytes_, 8);   // the edit count and the packets
    writes.emplace_back(size_field_, std::move(size));
    Bytes tail;
    if (!cookie_.empty()) {
        put(tail, four("kuki"), 4);
        put(tail, cookie_.size(), 8);
        append(tail, cookie_);
    }
    // Constant frames per packet unless a packet before the last differs; a short last packet
    // counts in the remainder.
    const auto per_packet = format_.frames_per_packet;
    const bool variable = std::any_of(durations_.begin(), durations_.empty() ? durations_.end() : durations_.end() - 1,
                                      [&](std::uint32_t d) { return d != per_packet; }) ||
                          (!durations_.empty() && durations_.back() > per_packet);
    std::uint64_t remainder = remainder_;
    if (!variable && !durations_.empty()) remainder += per_packet - durations_.back();
    const std::uint64_t slots = variable ? frames_ : sizes_.size() * std::uint64_t{per_packet};
    const std::uint64_t valid = slots > priming_ + remainder ? slots - priming_ - remainder : 0;
    Bytes table;
    put(table, sizes_.size(), 8);
    put(table, valid, 8);
    put(table, priming_, 4);
    put(table, remainder, 4);
    for (std::size_t i = 0; i < sizes_.size(); ++i) {
        put_ber(table, sizes_[i]);
        if (variable) put_ber(table, durations_[i]);
    }
    put(tail, four("pakt"), 4);
    put(tail, table.size(), 8);
    append(tail, table);
    if (variable) {   // the description says so: mFramesPerPacket 0
        Bytes zero(4, 0);
        writes.emplace_back(8 + 12 + 20, std::move(zero));
    }
    writes.emplace_back(data_end(), std::move(tail));
    return writes;
}

std::vector<std::pair<std::uint64_t, std::vector<std::uint8_t>>> Writer::finish_mp4() const {
    std::vector<std::pair<std::uint64_t, Bytes>> writes;
    Bytes size;
    put(size, 16 + data_bytes_, 8);
    writes.emplace_back(size_field_, std::move(size));
    const auto rate = static_cast<std::uint32_t>(format_.rate);
    const std::uint64_t duration = frames_;
    const int version = duration > 0xffffffffULL ? 1 : 0;
    const auto put_times = [&](Bytes& out, bool with_timescale) {   // creation, modification, [timescale,] duration
        put(out, 0, version ? 8 : 4);
        put(out, 0, version ? 8 : 4);
        if (with_timescale) put(out, rate, 4);
        put(out, duration, version ? 8 : 4);
    };
    // mvhd
    Bytes mvhd;
    put_times(mvhd, true);
    put(mvhd, 0x00010000, 4);   // rate 1.0
    put(mvhd, 0x0100, 2);       // volume 1.0
    put(mvhd, 0, 2 + 8);
    put_matrix(mvhd);
    put(mvhd, 0, 24);           // pre_defined
    put(mvhd, 2, 4);            // next_track_ID
    // tkhd: enabled, in the movie and its preview; track 1.
    Bytes tkhd;
    put(tkhd, 0, version ? 8 : 4);
    put(tkhd, 0, version ? 8 : 4);
    put(tkhd, 1, 4);
    put(tkhd, 0, 4);
    put(tkhd, duration, version ? 8 : 4);
    put(tkhd, 0, 8 + 2 + 2);    // reserved, layer, alternate_group
    put(tkhd, 0x0100, 2);       // volume
    put(tkhd, 0, 2);
    put_matrix(tkhd);
    put(tkhd, 0, 8);            // width, height
    // mdhd: the sample rate as the timescale, language "und".
    Bytes mdhd;
    put_times(mdhd, true);
    put(mdhd, 0x55c4, 2);
    put(mdhd, 0, 2);
    Bytes hdlr;
    put(hdlr, 0, 4);
    put(hdlr, four("soun"), 4);
    put(hdlr, 0, 12);
    for (const char c : std::string("SoundHandler")) hdlr.push_back(static_cast<std::uint8_t>(c));
    hdlr.push_back(0);
    // The sample entry: an AudioSampleEntry with the format's configuration box.
    Bytes entry;
    put(entry, 0, 6);
    put(entry, 1, 2);                                   // data_reference_index
    put(entry, 0, 8);
    put(entry, format_.channels, 2);
    const std::uint32_t bits = format_.flags == 1 ? 16 : format_.flags == 2 ? 20 : format_.flags == 3 ? 24 : 32;
    put(entry, bits, 2);
    put(entry, 0, 4);
    put(entry, rate <= 0xffff ? std::uint64_t{rate} << 16 : 0, 4);   // 16.16; the ALAC config has the rate above that
    append(entry, full(four("alac"), 0, 0, cookie_));
    Bytes stsd;
    put(stsd, 1, 4);
    append(stsd, box(format_.format_id, entry));
    // stts: runs of equal durations.
    Bytes stts_runs;
    std::uint32_t runs = 0;
    for (std::size_t i = 0; i < durations_.size();) {
        std::size_t j = i;
        while (j < durations_.size() && durations_[j] == durations_[i]) ++j;
        put(stts_runs, j - i, 4);
        put(stts_runs, durations_[i], 4);
        ++runs;
        i = j;
    }
    Bytes stts;
    put(stts, runs, 4);
    append(stts, stts_runs);
    // One chunk holds every sample.
    Bytes stsc;
    put(stsc, sizes_.empty() ? 0 : 1, 4);
    if (!sizes_.empty()) { put(stsc, 1, 4); put(stsc, sizes_.size(), 4); put(stsc, 1, 4); }
    Bytes stsz;
    put(stsz, 0, 4);
    put(stsz, sizes_.size(), 4);
    for (const auto s : sizes_) put(stsz, s, 4);
    // The one chunk starts right after the header, well inside 32 bits.
    Bytes chunk_offsets;
    put(chunk_offsets, sizes_.empty() ? 0 : 1, 4);
    if (!sizes_.empty()) put(chunk_offsets, data_offset(), 4);
    Bytes stbl;
    append(stbl, full(four("stsd"), 0, 0, stsd));
    append(stbl, full(four("stts"), 0, 0, stts));
    append(stbl, full(four("stsc"), 0, 0, stsc));
    append(stbl, full(four("stsz"), 0, 0, stsz));
    append(stbl, full(four("stco"), 0, 0, chunk_offsets));
    Bytes dref;
    put(dref, 1, 4);
    append(dref, full(four("url "), 0, 1, {}));   // the data is in this file
    Bytes minf = full(four("smhd"), 0, 0, Bytes(4, 0));
    append(minf, box(four("dinf"), full(four("dref"), 0, 0, dref)));
    append(minf, box(four("stbl"), stbl));
    Bytes mdia = full(four("mdhd"), version, 0, mdhd);
    append(mdia, full(four("hdlr"), 0, 0, hdlr));
    append(mdia, box(four("minf"), minf));
    Bytes trak = full(four("tkhd"), version, 7, tkhd);
    append(trak, box(four("mdia"), mdia));
    Bytes moov = full(four("mvhd"), version, 0, mvhd);
    append(moov, box(four("trak"), trak));
    writes.emplace_back(data_end(), box(four("moov"), moov));
    return writes;
}

} // namespace alchemy::gui::completion_kit::media::packet_file
