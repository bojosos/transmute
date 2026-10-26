#include "alchemy/darwin/media/media_container.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>

namespace alchemy::gui::completion_kit::media::container {
namespace {

constexpr std::uint64_t maximum_box = 64ULL << 20;   // moov/moof/header payloads read into memory
constexpr std::size_t maximum_samples = 1U << 24;

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t be24(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 16) | (p[1] << 8) | p[2]; }
std::uint32_t be32(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (p[2] << 8) | p[3]; }
std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t(be32(p)) << 32) | be32(p + 4); }
std::uint32_t le32(const std::uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24); }
std::uint16_t le16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

bool read_exact(const ReadAt& read, std::uint64_t offset, std::span<std::uint8_t> out) { return read(offset, out) == out.size(); }
std::vector<std::uint8_t> read_bytes(const ReadAt& read, std::uint64_t offset, std::uint64_t size) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    out.resize(read(offset, out));
    return out;
}

// ---- ISO base media -------------------------------------------------------------------------------

// A box within an in-memory payload: its type and payload span.
struct Box { std::uint32_t type{}; std::span<const std::uint8_t> payload; };

// The boxes of `data`, in order (a truncated last box ends the list).
std::vector<Box> boxes(std::span<const std::uint8_t> data) {
    std::vector<Box> out;
    std::size_t at = 0;
    while (at + 8 <= data.size()) {
        std::uint64_t size = be32(data.data() + at);
        const auto type = be32(data.data() + at + 4);
        std::size_t header = 8;
        if (size == 1) {
            if (at + 16 > data.size()) break;
            size = be64(data.data() + at + 8);
            header = 16;
        } else if (size == 0) {
            size = data.size() - at;
        }
        if (size < header || at + size > data.size()) break;
        out.push_back({type, data.subspan(at + header, static_cast<std::size_t>(size - header))});
        at += static_cast<std::size_t>(size);
    }
    return out;
}
std::optional<Box> child(std::span<const std::uint8_t> data, std::uint32_t type) {
    for (const auto& b : boxes(data)) if (b.type == type) return b;
    return std::nullopt;
}

struct Trex { std::uint32_t duration{}, size{}, flags{}; };

struct IsoTrack {
    Track track;
    std::uint32_t handler{};
    // Sample table
    std::vector<std::pair<std::uint32_t, std::uint32_t>> stts, ctts;   // (count, delta/offset)
    std::vector<std::array<std::uint32_t, 3>> stsc;                    // (first chunk, samples per chunk, description)
    std::vector<std::uint32_t> sizes;
    std::uint32_t constant_size{};
    std::uint32_t sample_count{};
    std::vector<std::uint64_t> chunks;
    std::vector<std::uint32_t> sync;
    bool has_sync{};
    std::int64_t edit_media_time{}, edit_empty{};   // edit list: media start, and an initial empty edit (movie timescale)
    Trex trex;
};

std::uint32_t codec_of_entry(std::uint32_t type) {
    switch (type) {
    case fourcc("avc3"): return fourcc("avc1");
    case fourcc("hev1"): return fourcc("hvc1");
    case fourcc("Opus"): return fourcc("opus");
    case fourcc("fLaC"): return fourcc("flac");
    case fourcc("sowt"): case fourcc("twos"): case fourcc("in24"): case fourcc("in32"): case fourcc("fl32"): case fourcc("fl64"):
    case fourcc("raw "): case fourcc("NONE"): return fourcc("lpcm");
    default: return type;
    }
}

// The objectTypeIndication of an esds payload (0: none found).
std::uint8_t esds_object_type(std::span<const std::uint8_t> esds) {
    // version/flags, then ES_Descriptor (tag 3) holding DecoderConfigDescriptor (tag 4).
    std::size_t at = 4;
    const auto descriptor = [&](std::uint8_t& tag, std::size_t& length) {
        if (at >= esds.size()) return false;
        tag = esds[at++];
        length = 0;
        for (int i = 0; i < 4 && at < esds.size(); ++i) {
            const auto b = esds[at++];
            length = (length << 7) | (b & 0x7f);
            if (!(b & 0x80)) break;
        }
        return true;
    };
    std::uint8_t tag{};
    std::size_t length{};
    if (!descriptor(tag, length) || tag != 3) return 0;
    if (at + 3 > esds.size()) return 0;
    const auto flags = esds[at + 2];
    at += 3;
    if (flags & 0x80) at += 2;                                   // dependsOn_ES_ID
    if (flags & 0x40) { if (at >= esds.size()) return 0; at += 1 + esds[at]; }   // URL
    if (flags & 0x20) at += 2;                                   // OCR_ES_Id
    if (!descriptor(tag, length) || tag != 4 || at >= esds.size()) return 0;
    return esds[at];
}

void parse_stsd(IsoTrack& t, std::span<const std::uint8_t> stsd) {
    if (stsd.size() < 8) return;
    const auto entries = boxes(stsd.subspan(8));
    if (entries.empty()) return;
    const auto& entry = entries.front();
    auto& track = t.track;
    const auto p = entry.payload;
    track.codec_name = std::string{char(entry.type >> 24), char(entry.type >> 16), char(entry.type >> 8), char(entry.type)};
    track.codec = codec_of_entry(entry.type);
    std::span<const std::uint8_t> children;
    if (t.handler == fourcc("vide")) {
        if (p.size() < 78) return;
        track.kind = Track::Kind::video;
        track.width = be16(p.data() + 24);
        track.height = be16(p.data() + 26);
        children = p.subspan(78);
    } else if (t.handler == fourcc("soun")) {
        if (p.size() < 28) return;
        track.kind = Track::Kind::audio;
        const auto version = be16(p.data() + 8);
        track.channels = be16(p.data() + 16);
        track.bits = be16(p.data() + 18);
        track.sample_rate = be32(p.data() + 24) / 65536.0;
        std::size_t header = 28;
        if (version == 1) header += 16;
        else if (version == 2 && p.size() >= 64) {
            header += 36;
            track.sample_rate = std::bit_cast<double>(be64(p.data() + 32));
            track.channels = be32(p.data() + 40);
            track.bits = be32(p.data() + 48);
        }
        if (p.size() < header) return;
        children = p.subspan(header);
        if (track.sample_rate == 0 && t.track.timescale) track.sample_rate = t.track.timescale;
    } else {
        return;
    }
    // The codec configuration (QuickTime may wrap it in a 'wave' box).
    auto found = boxes(children);
    if (const auto wave = child(children, fourcc("wave"))) {
        const auto inner = boxes(wave->payload);
        found.insert(found.end(), inner.begin(), inner.end());
    }
    for (const auto& b : found) {
        switch (b.type) {
        case fourcc("avcC"): case fourcc("hvcC"): case fourcc("av1C"): case fourcc("vpcC"): case fourcc("esds"):
        case fourcc("dOps"): case fourcc("dfLa"): case fourcc("alac"):
            if (track.config.empty()) track.config.assign(b.payload.begin(), b.payload.end());
            break;
        default: break;
        }
        if (b.type == fourcc("esds")) {
            const auto object = esds_object_type(b.payload);
            if (object == 0x69 || object == 0x6b) track.codec = fourcc(".mp3");   // MPEG-1/2 audio
            else if (object == 0xad) track.codec = fourcc("opus");
        }
    }
    if (track.codec == fourcc("opus") && track.config.size() >= 11) {
        track.priming = be16(track.config.data() + 2);   // dOps PreSkip
        track.channels = track.config[1];
    }
}

void parse_trak(std::span<const std::uint8_t> trak, std::uint32_t movie_timescale, std::vector<IsoTrack>& out) {
    IsoTrack t;
    if (const auto tkhd = child(trak, fourcc("tkhd")); tkhd && tkhd->payload.size() >= 24)
        t.track.id = tkhd->payload[0] == 1 ? be32(tkhd->payload.data() + 20) : be32(tkhd->payload.data() + 12);
    if (const auto edts = child(trak, fourcc("edts")))
        if (const auto elst = child(edts->payload, fourcc("elst")); elst && elst->payload.size() >= 8) {
            const auto version = elst->payload[0];
            const auto count = be32(elst->payload.data() + 4);
            const std::size_t entry = version == 1 ? 20 : 12;
            for (std::uint32_t i = 0; i < count && 8 + (i + 1) * entry <= elst->payload.size(); ++i) {
                const auto* e = elst->payload.data() + 8 + i * entry;
                const std::int64_t duration = version == 1 ? static_cast<std::int64_t>(be64(e)) : be32(e);
                const std::int64_t media = version == 1 ? static_cast<std::int64_t>(be64(e + 8)) : static_cast<std::int32_t>(be32(e + 4));
                if (media == -1) { t.edit_empty += duration; continue; }
                t.edit_media_time = media;
                break;
            }
        }
    const auto mdia = child(trak, fourcc("mdia"));
    if (!mdia) return;
    if (const auto mdhd = child(mdia->payload, fourcc("mdhd")); mdhd && mdhd->payload.size() >= 24) {
        const auto* p = mdhd->payload.data();
        if (p[0] == 1 && mdhd->payload.size() >= 32) { t.track.timescale = be32(p + 20); t.track.duration = static_cast<std::int64_t>(be64(p + 24)); }
        else { t.track.timescale = be32(p + 12); t.track.duration = be32(p + 16); }
        if (!t.track.timescale) t.track.timescale = 1;
    }
    if (const auto hdlr = child(mdia->payload, fourcc("hdlr")); hdlr && hdlr->payload.size() >= 12) t.handler = be32(hdlr->payload.data() + 8);
    const auto minf = child(mdia->payload, fourcc("minf"));
    const auto stbl = minf ? child(minf->payload, fourcc("stbl")) : std::nullopt;
    if (!stbl) return;
    for (const auto& b : boxes(stbl->payload)) {
        const auto p = b.payload;
        if (p.size() < 8) continue;
        const auto count = be32(p.data() + 4);
        switch (b.type) {
        case fourcc("stsd"): parse_stsd(t, p); break;
        case fourcc("stts"):
            for (std::uint32_t i = 0; i < count && 8 + (i + 1) * 8 <= p.size(); ++i) t.stts.emplace_back(be32(p.data() + 8 + i * 8), be32(p.data() + 12 + i * 8));
            break;
        case fourcc("ctts"):
            for (std::uint32_t i = 0; i < count && 8 + (i + 1) * 8 <= p.size(); ++i) t.ctts.emplace_back(be32(p.data() + 8 + i * 8), be32(p.data() + 12 + i * 8));
            break;
        case fourcc("stsc"):
            for (std::uint32_t i = 0; i < count && 8 + (i + 1) * 12 <= p.size(); ++i)
                t.stsc.push_back({be32(p.data() + 8 + i * 12), be32(p.data() + 12 + i * 12), be32(p.data() + 16 + i * 12)});
            break;
        case fourcc("stsz"):
            if (p.size() < 12) break;
            t.constant_size = be32(p.data() + 4);
            t.sample_count = be32(p.data() + 8);
            if (!t.constant_size)
                for (std::uint32_t i = 0; i < t.sample_count && 12 + (i + 1) * 4 <= p.size(); ++i) t.sizes.push_back(be32(p.data() + 12 + i * 4));
            break;
        case fourcc("stz2"): {
            if (p.size() < 12) break;
            const auto field = p[7];
            t.sample_count = be32(p.data() + 8);
            for (std::uint32_t i = 0; i < t.sample_count; ++i) {
                const auto bit = static_cast<std::size_t>(i) * field;
                if (12 + (bit + field + 7) / 8 > p.size()) break;
                const auto* q = p.data() + 12 + bit / 8;
                t.sizes.push_back(field == 4 ? ((bit % 8) ? (*q & 0x0f) : (*q >> 4)) : field == 8 ? *q : be16(q));
            }
            break;
        }
        case fourcc("stco"):
            for (std::uint32_t i = 0; i < count && 8 + (i + 1) * 4 <= p.size(); ++i) t.chunks.push_back(be32(p.data() + 8 + i * 4));
            break;
        case fourcc("co64"):
            for (std::uint32_t i = 0; i < count && 8 + (i + 1) * 8 <= p.size(); ++i) t.chunks.push_back(be64(p.data() + 8 + i * 8));
            break;
        case fourcc("stss"):
            t.has_sync = true;
            for (std::uint32_t i = 0; i < count && 8 + (i + 1) * 4 <= p.size(); ++i) t.sync.push_back(be32(p.data() + 8 + i * 4));
            break;
        default: break;
        }
    }
    static_cast<void>(movie_timescale);
    out.push_back(std::move(t));
}

// The sample table of a moov track: offsets from the chunks, times from stts/ctts.
void build_samples(IsoTrack& t) {
    auto& samples = t.track.samples;
    const auto count = std::min<std::size_t>(t.sample_count, maximum_samples);
    samples.resize(count);
    // Sizes
    for (std::size_t i = 0; i < count; ++i) samples[i].size = t.constant_size ? t.constant_size : (i < t.sizes.size() ? t.sizes[i] : 0);
    // Offsets: stsc runs over chunks.
    std::size_t sample = 0;
    for (std::size_t run = 0; run < t.stsc.size() && sample < count; ++run) {
        const auto first = t.stsc[run][0], per_chunk = t.stsc[run][1];
        const auto last = run + 1 < t.stsc.size() ? t.stsc[run + 1][0] : static_cast<std::uint32_t>(t.chunks.size() + 1);
        for (auto chunk = first; chunk < last && chunk <= t.chunks.size() && sample < count; ++chunk) {
            auto offset = t.chunks[chunk - 1];
            for (std::uint32_t k = 0; k < per_chunk && sample < count; ++k, ++sample) {
                samples[sample].offset = offset;
                offset += samples[sample].size;
            }
        }
    }
    samples.resize(sample);
    // Times
    std::int64_t dts = 0;
    std::size_t i = 0;
    for (const auto& [n, delta] : t.stts)
        for (std::uint32_t k = 0; k < n && i < samples.size(); ++k, ++i) {
            samples[i].dts = dts;
            samples[i].duration = delta;
            dts += delta;
        }
    for (; i < samples.size(); ++i) samples[i].dts = dts;
    i = 0;
    for (const auto& [n, offset] : t.ctts)
        for (std::uint32_t k = 0; k < n && i < samples.size(); ++k, ++i) samples[i].pts = samples[i].dts + static_cast<std::int32_t>(offset);
    for (; i < samples.size(); ++i) samples[i].pts = samples[i].dts;
    // Sync samples (no table: all are)
    if (!t.has_sync) for (auto& s : samples) s.keyframe = true;
    for (const auto number : t.sync) if (number >= 1 && number <= samples.size()) samples[number - 1].keyframe = true;
}

void parse_moof(std::span<const std::uint8_t> moof, std::uint64_t moof_offset, std::map<std::uint32_t, IsoTrack*>& by_id) {
    for (const auto& traf : boxes(moof)) {
        if (traf.type != fourcc("traf")) continue;
        const auto tfhd = child(traf.payload, fourcc("tfhd"));
        if (!tfhd || tfhd->payload.size() < 8) continue;
        const auto* p = tfhd->payload.data();
        const auto flags = be24(p + 1);
        const auto found = by_id.find(be32(p + 4));
        if (found == by_id.end()) continue;
        auto& t = *found->second;
        std::size_t at = 8;
        std::uint64_t base = moof_offset;
        Trex defaults = t.trex;
        const auto field = [&](std::size_t bytes) -> std::uint64_t {
            if (at + bytes > tfhd->payload.size()) return 0;
            const auto value = bytes == 8 ? be64(p + at) : be32(p + at);
            at += bytes;
            return value;
        };
        if (flags & 0x1) base = field(8);
        if (flags & 0x2) static_cast<void>(field(4));
        if (flags & 0x8) defaults.duration = static_cast<std::uint32_t>(field(4));
        if (flags & 0x10) defaults.size = static_cast<std::uint32_t>(field(4));
        if (flags & 0x20) defaults.flags = static_cast<std::uint32_t>(field(4));
        std::int64_t decode_time = t.track.samples.empty() ? 0 : t.track.samples.back().dts + t.track.samples.back().duration;
        if (const auto tfdt = child(traf.payload, fourcc("tfdt")); tfdt && tfdt->payload.size() >= 8)
            decode_time = tfdt->payload[0] == 1 && tfdt->payload.size() >= 12 ? static_cast<std::int64_t>(be64(tfdt->payload.data() + 4))
                                                                            : be32(tfdt->payload.data() + 4);
        std::uint64_t data_end = base;
        for (const auto& trun : boxes(traf.payload)) {
            if (trun.type != fourcc("trun") || trun.payload.size() < 8) continue;
            const auto* r = trun.payload.data();
            const auto version = r[0];
            const auto rflags = be24(r + 1);
            const auto count = be32(r + 4);
            std::size_t ra = 8;
            const auto rfield = [&]() -> std::uint32_t {
                if (ra + 4 > trun.payload.size()) return 0;
                const auto v = be32(r + ra);
                ra += 4;
                return v;
            };
            std::uint64_t offset = data_end;
            if (rflags & 0x1) offset = base + static_cast<std::int32_t>(rfield());
            std::optional<std::uint32_t> first_flags;
            if (rflags & 0x4) first_flags = rfield();
            for (std::uint32_t i = 0; i < count && t.track.samples.size() < maximum_samples; ++i) {
                Sample s;
                s.duration = (rflags & 0x100) ? rfield() : defaults.duration;
                s.size = (rflags & 0x200) ? rfield() : defaults.size;
                auto sample_flags = (rflags & 0x400) ? rfield() : (i == 0 && first_flags ? *first_flags : defaults.flags);
                if (i == 0 && first_flags && !(rflags & 0x400)) sample_flags = *first_flags;
                std::int64_t composition = 0;
                if (rflags & 0x800) composition = version == 0 ? static_cast<std::int64_t>(rfield()) : static_cast<std::int32_t>(rfield());
                s.offset = offset;
                offset += s.size;
                s.dts = decode_time;
                s.pts = decode_time + composition;
                decode_time += s.duration;
                s.keyframe = !(sample_flags & 0x10000);   // sample_is_non_sync_sample
                t.track.samples.push_back(s);
            }
            data_end = offset;
        }
    }
}

} // namespace

bool parse_iso(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why) {
    out = Movie{};
    std::vector<IsoTrack> tracks;
    std::map<std::uint32_t, IsoTrack*> by_id;
    bool have_moov = false;
    std::uint32_t movie_timescale = 1000;
    std::int64_t movie_duration = 0;
    for (std::uint64_t at = 0; at + 8 <= size;) {
        std::array<std::uint8_t, 16> header{};
        if (!read_exact(read, at, std::span(header).first(8))) break;
        std::uint64_t box_size = be32(header.data());
        const auto type = be32(header.data() + 4);
        std::uint64_t header_size = 8;
        if (box_size == 1) {
            if (!read_exact(read, at + 8, std::span(header).subspan(8, 8))) break;
            box_size = be64(header.data() + 8);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = size - at;
        }
        if (box_size < header_size) { why = "a malformed box"; return false; }
        if (type == fourcc("ftyp") && box_size >= 12) {
            std::array<std::uint8_t, 4> brand{};
            if (read_exact(read, at + header_size, brand)) out.brand = be32(brand.data());
        } else if (type == fourcc("moov") || type == fourcc("moof")) {
            if (box_size - header_size > maximum_box) { why = "a moov or moof box over 64 MB"; return false; }
            const auto payload = read_bytes(read, at + header_size, box_size - header_size);
            if (type == fourcc("moov")) {
                have_moov = true;
                if (const auto mvhd = child(payload, fourcc("mvhd")); mvhd && mvhd->payload.size() >= 20) {
                    const auto* p = mvhd->payload.data();
                    if (p[0] == 1 && mvhd->payload.size() >= 32) { movie_timescale = be32(p + 20); movie_duration = static_cast<std::int64_t>(be64(p + 24)); }
                    else { movie_timescale = be32(p + 12); movie_duration = be32(p + 16); }
                    if (!movie_timescale) movie_timescale = 1000;
                }
                for (const auto& b : boxes(payload))
                    if (b.type == fourcc("trak")) parse_trak(b.payload, movie_timescale, tracks);
                for (auto& t : tracks) {
                    build_samples(t);
                    by_id[t.track.id] = &t;
                }
                if (const auto mvex = child(payload, fourcc("mvex")))
                    for (const auto& b : boxes(mvex->payload))
                        if (b.type == fourcc("trex") && b.payload.size() >= 24) {
                            const auto found = by_id.find(be32(b.payload.data() + 4));
                            if (found != by_id.end())
                                found->second->trex = {be32(b.payload.data() + 12), be32(b.payload.data() + 16), be32(b.payload.data() + 20)};
                        }
            } else if (have_moov) {
                parse_moof(payload, at, by_id);
            }
        }
        at += box_size;
    }
    if (!have_moov) { why = "no moov box"; return false; }
    out.timescale = movie_timescale;
    out.duration = movie_duration;
    if (!out.brand) out.brand = fourcc("isom");
    for (auto& t : tracks) {
        if (t.track.kind == Track::Kind::other) continue;
        // The edit list: the media start (audio priming, composition offsets), an initial empty edit.
        t.track.start = t.edit_media_time - (movie_timescale ? t.edit_empty * t.track.timescale / movie_timescale : 0);
        if (t.track.kind == Track::Kind::audio && t.edit_media_time > 0 && !t.track.priming) t.track.priming = static_cast<std::uint32_t>(t.edit_media_time);
        if (!t.track.duration && !t.track.samples.empty()) t.track.duration = t.track.samples.back().dts + t.track.samples.back().duration;
        out.tracks.push_back(std::move(t.track));
    }
    return true;
}

// ---- Matroska / WebM --------------------------------------------------------------------------------

namespace {

constexpr std::uint64_t unknown_size = ~0ULL;

// An EBML element header at `at`: id (with its marker bits), payload size (unknown_size when all
// ones), header length. False at the end or on garbage.
bool element(const ReadAt& read, std::uint64_t at, std::uint64_t end, std::uint32_t& id, std::uint64_t& size, std::uint64_t& header) {
    std::array<std::uint8_t, 12> b{};
    const auto got = read(at, std::span(b).first(static_cast<std::size_t>(std::min<std::uint64_t>(12, end - at))));
    if (got < 2) return false;
    const auto length = [](std::uint8_t first) { return first ? std::countl_zero(first) + 1 : 9; };
    const auto id_length = static_cast<std::size_t>(length(b[0]));
    if (id_length > 4 || id_length >= got) return false;
    id = 0;
    for (std::size_t i = 0; i < id_length; ++i) id = (id << 8) | b[i];
    const auto size_length = static_cast<std::size_t>(length(b[id_length]));
    if (size_length > 8 || id_length + size_length > got) return false;
    size = b[id_length] & (0xff >> size_length);
    bool all_ones = size == (0xffU >> size_length);
    for (std::size_t i = 1; i < size_length; ++i) {
        size = (size << 8) | b[id_length + i];
        all_ones = all_ones && b[id_length + i] == 0xff;
    }
    if (all_ones) size = unknown_size;
    header = id_length + size_length;
    return true;
}

std::uint64_t uint_value(std::span<const std::uint8_t> p) { std::uint64_t v = 0; for (const auto b : p) v = (v << 8) | b; return v; }
double float_value(std::span<const std::uint8_t> p) {
    if (p.size() == 4) return std::bit_cast<float>(be32(p.data()));
    if (p.size() == 8) return std::bit_cast<double>(be64(p.data()));
    return 0;
}

// A variable-length integer within a block (size or track number): the value and its length.
bool vint(std::span<const std::uint8_t> p, std::size_t& at, std::uint64_t& value) {
    if (at >= p.size()) return false;
    const auto first = p[at];
    const auto length = static_cast<std::size_t>(first ? std::countl_zero(first) + 1 : 9);
    if (length > 8 || at + length > p.size()) return false;
    value = first & (0xff >> length);
    for (std::size_t i = 1; i < length; ++i) value = (value << 8) | p[at + i];
    at += length;
    return true;
}
bool signed_vint(std::span<const std::uint8_t> p, std::size_t& at, std::int64_t& value) {
    const auto start = at;
    std::uint64_t raw{};
    if (!vint(p, at, raw)) return false;
    const auto length = at - start;
    value = static_cast<std::int64_t>(raw) - ((std::int64_t{1} << (7 * length - 1)) - 1);
    return true;
}

struct MkvTrack {
    Track track;
    std::uint64_t default_duration_ns{};
    std::uint64_t codec_delay_ns{};
};

std::uint32_t codec_of(const std::string& id, Track& t) {
    if (id == "V_VP8") return fourcc("vp08");
    if (id == "V_VP9") return fourcc("vp09");
    if (id == "V_AV1") return fourcc("av01");
    if (id == "V_MPEG4/ISO/AVC") return fourcc("avc1");
    if (id == "V_MPEGH/ISO/HEVC") return fourcc("hvc1");
    if (id == "A_OPUS") return fourcc("opus");
    if (id == "A_FLAC") return fourcc("flac");
    if (id == "A_MPEG/L3" || id == "A_MPEG/L2") return fourcc(".mp3");
    if (id.starts_with("A_AAC")) return fourcc("mp4a");
    if (id == "A_VORBIS") return fourcc("vorb");
    if (id == "A_PCM/INT/LIT" || id == "A_PCM/INT/BIG" || id == "A_PCM/FLOAT/IEEE") { if (t.bits == 0) t.bits = 16; return fourcc("lpcm"); }
    if (id == "A_ALAC") return fourcc("alac");
    return 0;
}

} // namespace

bool parse_matroska(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why) {
    out = Movie{};
    constexpr std::uint32_t id_ebml = 0x1A45DFA3, id_doctype = 0x4282, id_segment = 0x18538067, id_info = 0x1549A966, id_timecode_scale = 0x2AD7B1,
        id_duration = 0x4489, id_tracks = 0x1654AE6B, id_track_entry = 0xAE, id_track_number = 0xD7, id_track_type = 0x83, id_codec_id = 0x86,
        id_codec_private = 0x63A2, id_codec_delay = 0x56AA, id_default_duration = 0x23E383, id_video = 0xE0, id_pixel_width = 0xB0,
        id_pixel_height = 0xBA, id_audio = 0xE1, id_sampling_frequency = 0xB5, id_channels = 0x9F, id_bit_depth = 0x6264, id_cluster = 0x1F43B675,
        id_cluster_timecode = 0xE7, id_simple_block = 0xA3, id_block_group = 0xA0, id_block = 0xA1, id_block_duration = 0x9B, id_reference_block = 0xFB;
    std::uint32_t id{};
    std::uint64_t length{}, header{};
    if (!element(read, 0, size, id, length, header) || id != id_ebml || length == unknown_size) { why = "not an EBML file"; return false; }
    // The doctype: webm or matroska.
    {
        const auto ebml = read_bytes(read, header, std::min<std::uint64_t>(length, 4096));
        for (std::uint64_t at = 0; at < ebml.size();) {
            std::uint32_t cid{};
            std::uint64_t clen{}, chead{};
            if (!element([&](std::uint64_t o, std::span<std::uint8_t> to) {
                    if (o >= ebml.size()) return std::size_t{0};
                    const auto n = std::min<std::size_t>(to.size(), ebml.size() - static_cast<std::size_t>(o));
                    std::memcpy(to.data(), ebml.data() + o, n);
                    return n;
                }, at, ebml.size(), cid, clen, chead) || clen == unknown_size)
                break;
            if (cid == id_doctype && at + chead + clen <= ebml.size())
                out.brand = std::string(ebml.begin() + static_cast<std::ptrdiff_t>(at + chead), ebml.begin() + static_cast<std::ptrdiff_t>(at + chead + clen)) == "webm"
                                ? fourcc("webm") : fourcc("matr");
            at += chead + clen;
        }
    }
    std::uint64_t at = header + length;
    if (!element(read, at, size, id, length, header) || id != id_segment) { why = "no Matroska segment"; return false; }
    const auto segment_end = length == unknown_size ? size : std::min(size, at + header + length);
    at += header;
    std::uint64_t timecode_scale = 1'000'000;
    double duration = 0;
    std::map<std::uint64_t, MkvTrack> tracks;
    // Reads a small element's payload.
    const auto payload = [&](std::uint64_t offset, std::uint64_t bytes) { return read_bytes(read, offset, std::min<std::uint64_t>(bytes, maximum_box)); };
    // Iterates the children of [begin, end): f(id, payload offset, payload size).
    const auto children = [&](std::uint64_t begin, std::uint64_t end, const auto& f) {
        for (std::uint64_t c = begin; c < end;) {
            std::uint32_t cid{};
            std::uint64_t clen{}, chead{};
            if (!element(read, c, end, cid, clen, chead) || clen == unknown_size) break;
            f(cid, c + chead, std::min(clen, end - c - chead));
            c += chead + clen;
        }
    };
    const auto add_block = [&](std::uint64_t offset, std::uint64_t bytes, std::int64_t cluster_time, bool simple, bool keyframe_hint, std::optional<std::uint64_t> block_duration) {
        const auto head = payload(offset, std::min<std::uint64_t>(bytes, 65536));
        std::size_t p = 0;
        std::uint64_t number{};
        if (!vint(head, p, number) || p + 3 > head.size()) return;
        const auto found = tracks.find(number);
        if (found == tracks.end()) return;
        auto& t = found->second;
        const auto relative = static_cast<std::int16_t>(be16(head.data() + p));
        const auto flags = head[p + 2];
        p += 3;
        const bool keyframe = simple ? (flags & 0x80) != 0 : keyframe_hint;
        const auto lacing = (flags >> 1) & 3;
        std::vector<std::uint64_t> sizes;
        if (lacing == 0) {
            sizes.push_back(bytes - p);
        } else {
            if (p >= head.size()) return;
            const std::size_t frames = head[p++] + 1U;
            if (lacing == 1) {   // Xiph
                std::uint64_t total = 0;
                for (std::size_t i = 0; i + 1 < frames; ++i) {
                    std::uint64_t value = 0;
                    while (p < head.size()) { const auto b = head[p++]; value += b; if (b != 255) break; }
                    sizes.push_back(value);
                    total += value;
                }
                if (p > bytes || total > bytes - p) return;
                sizes.push_back(bytes - p - total);
            } else if (lacing == 2) {   // fixed
                const auto each = (bytes - p) / frames;
                sizes.assign(frames, each);
            } else {   // EBML
                std::uint64_t first{};
                if (!vint(head, p, first)) return;
                sizes.push_back(first);
                std::uint64_t total = first, previous = first;
                for (std::size_t i = 1; i + 1 < frames; ++i) {
                    std::int64_t delta{};
                    if (!signed_vint(head, p, delta)) return;
                    previous = static_cast<std::uint64_t>(static_cast<std::int64_t>(previous) + delta);
                    sizes.push_back(previous);
                    total += previous;
                }
                if (p > bytes || total > bytes - p) return;
                sizes.push_back(bytes - p - total);
            }
        }
        // Times in the track's timescale (timecode units).
        const std::int64_t time = cluster_time + relative;
        const auto frame_duration = t.default_duration_ns ? static_cast<std::uint32_t>(t.default_duration_ns / timecode_scale) : 0;
        auto data = offset + p;
        for (std::size_t i = 0; i < sizes.size() && t.track.samples.size() < maximum_samples; ++i) {
            Sample s;
            s.offset = data;
            s.size = sizes[i];
            s.pts = s.dts = time + static_cast<std::int64_t>(i) * frame_duration;
            s.duration = block_duration && sizes.size() == 1 ? static_cast<std::uint32_t>(*block_duration) : frame_duration;
            s.keyframe = keyframe || t.track.kind == Track::Kind::audio;
            t.track.samples.push_back(s);
            data += sizes[i];
        }
    };
    while (at < segment_end) {
        if (!element(read, at, segment_end, id, length, header)) break;
        const auto body = at + header;
        const auto end = length == unknown_size ? segment_end : std::min(segment_end, body + length);
        if (id == id_info) {
            children(body, end, [&](std::uint32_t cid, std::uint64_t o, std::uint64_t n) {
                const auto v = payload(o, n);
                if (cid == id_timecode_scale) timecode_scale = std::max<std::uint64_t>(1, uint_value(v));
                else if (cid == id_duration) duration = float_value(v);
            });
        } else if (id == id_tracks) {
            children(body, end, [&](std::uint32_t cid, std::uint64_t o, std::uint64_t n) {
                if (cid != id_track_entry) return;
                MkvTrack t;
                std::uint64_t number = 0;
                std::string codec;
                children(o, o + n, [&](std::uint32_t tid, std::uint64_t to, std::uint64_t tn) {
                    const auto v = payload(to, tn);
                    switch (tid) {
                    case id_track_number: number = uint_value(v); break;
                    case id_track_type: {
                        const auto type = uint_value(v);
                        t.track.kind = type == 1 ? Track::Kind::video : type == 2 ? Track::Kind::audio : Track::Kind::other;
                        break;
                    }
                    case id_codec_id: codec.assign(v.begin(), v.end()); while (!codec.empty() && codec.back() == 0) codec.pop_back(); break;
                    case id_codec_private: t.track.config = v; break;
                    case id_codec_delay: t.codec_delay_ns = uint_value(v); break;
                    case id_default_duration: t.default_duration_ns = uint_value(v); break;
                    case id_video:
                        children(to, to + tn, [&](std::uint32_t vid, std::uint64_t vo, std::uint64_t vn) {
                            const auto w = payload(vo, vn);
                            if (vid == id_pixel_width) t.track.width = static_cast<std::uint32_t>(uint_value(w));
                            else if (vid == id_pixel_height) t.track.height = static_cast<std::uint32_t>(uint_value(w));
                        });
                        break;
                    case id_audio:
                        children(to, to + tn, [&](std::uint32_t aid, std::uint64_t ao, std::uint64_t an) {
                            const auto w = payload(ao, an);
                            if (aid == id_sampling_frequency) t.track.sample_rate = float_value(w);
                            else if (aid == id_channels) t.track.channels = static_cast<std::uint32_t>(uint_value(w));
                            else if (aid == id_bit_depth) t.track.bits = static_cast<std::uint32_t>(uint_value(w));
                        });
                        break;
                    default: break;
                    }
                });
                t.track.id = static_cast<std::uint32_t>(number);
                t.track.codec_name = codec;
                t.track.codec = codec_of(codec, t.track);
                if (!t.track.sample_rate && t.track.kind == Track::Kind::audio) t.track.sample_rate = 8000;
                if (!t.track.channels && t.track.kind == Track::Kind::audio) t.track.channels = 1;
                if (t.track.codec == fourcc("opus") && t.track.config.size() >= 19)
                    t.track.priming = le16(t.track.config.data() + 10);   // OpusHead pre-skip
                tracks[number] = std::move(t);
            });
        } else if (id == id_cluster) {
            std::int64_t cluster_time = 0;
            for (std::uint64_t c = body; c < end;) {
                std::uint32_t cid{};
                std::uint64_t clen{}, chead{};
                if (!element(read, c, end, cid, clen, chead)) break;
                if (length == unknown_size && (cid == id_cluster || cid == id_tracks || cid == id_info || cid == 0x1C53BB6B || cid == 0x1254C367))
                    break;   // an unknown-size cluster ends at the next top-level element
                if (clen == unknown_size) break;
                const auto co = c + chead;
                if (cid == id_cluster_timecode) cluster_time = static_cast<std::int64_t>(uint_value(payload(co, clen)));
                else if (cid == id_simple_block) add_block(co, clen, cluster_time, true, false, std::nullopt);
                else if (cid == id_block_group) {
                    std::uint64_t block_offset = 0, block_size = 0;
                    std::optional<std::uint64_t> block_duration;
                    bool referenced = false;
                    children(co, co + clen, [&](std::uint32_t gid, std::uint64_t go, std::uint64_t gn) {
                        if (gid == id_block) { block_offset = go; block_size = gn; }
                        else if (gid == id_block_duration) block_duration = uint_value(payload(go, gn));
                        else if (gid == id_reference_block) referenced = true;
                    });
                    if (block_size) add_block(block_offset, block_size, cluster_time, false, !referenced, block_duration);
                }
                c = co + clen;
                if (length == unknown_size) at = c;   // continue after the unknown-size cluster's last child
            }
            if (length == unknown_size) { if (at <= body) break; continue; }
        }
        if (length == unknown_size) break;   // an unknown-size element other than a cluster: nothing follows it reliably
        at = body + length;
    }
    if (tracks.empty()) { why = "no Matroska tracks"; return false; }
    // Track times are in timecode units: timescale = 1e9 / TimecodeScale (ns per unit).
    const auto timescale = static_cast<std::uint32_t>(std::max<std::uint64_t>(1, 1'000'000'000ULL / timecode_scale));
    out.timescale = timescale;
    out.duration = static_cast<std::int64_t>(duration);
    for (auto& [number, t] : tracks) {
        if (t.track.kind == Track::Kind::other || !t.track.codec) continue;
        t.track.timescale = timescale;
        if (!t.track.samples.empty()) {
            const auto& last = t.track.samples.back();
            t.track.duration = last.pts + last.duration;
        } else {
            t.track.duration = out.duration;
        }
        out.tracks.push_back(std::move(t.track));
    }
    if (out.tracks.empty()) { why = "no supported Matroska tracks"; return false; }
    if (!out.brand) out.brand = fourcc("matr");
    return true;
}

// ---- Ogg ------------------------------------------------------------------------------------------

namespace {

// Samples (at 48 kHz) of an Opus packet, from its TOC byte.
std::uint32_t opus_samples(std::span<const std::uint8_t> packet) {
    if (packet.empty()) return 0;
    const auto toc = packet[0];
    const auto config = toc >> 3;
    std::uint32_t frame;
    if (config < 12) frame = std::array<std::uint32_t, 4>{480, 960, 1920, 2880}[config & 3];         // SILK
    else if (config < 16) frame = (config & 1) ? 960 : 480;                                         // hybrid
    else frame = std::array<std::uint32_t, 4>{120, 240, 480, 960}[config & 3];                      // CELT
    const auto code = toc & 3;
    std::uint32_t frames = code == 0 ? 1 : code < 3 ? 2 : (packet.size() > 1 ? packet[1] & 0x3f : 0);
    return frame * frames;
}

// Block size of a FLAC frame header (0: not a frame header).
std::uint32_t flac_block_size(std::span<const std::uint8_t> frame, std::uint32_t& sample_number_out, bool& variable);

} // namespace

bool parse_ogg(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why) {
    out = Movie{};
    out.brand = fourcc("OggS");
    struct Stream {
        Track track;
        std::vector<std::pair<std::uint64_t, std::uint64_t>> current;   // pieces of the packet being assembled
        std::uint64_t current_size{};
        int headers_seen{};
        bool flac{}, opus{};
        std::int64_t time{};
        std::uint32_t flac_rate{};
    };
    std::map<std::uint32_t, Stream> streams;
    for (std::uint64_t at = 0; at + 27 <= size;) {
        std::array<std::uint8_t, 27> h{};
        if (!read_exact(read, at, h) || std::memcmp(h.data(), "OggS", 4) != 0) {
            if (streams.empty()) { why = "not an Ogg stream"; return false; }
            break;
        }
        const auto serial = le32(h.data() + 14);
        const auto segments = h[26];
        std::vector<std::uint8_t> lacing(segments);
        if (!read_exact(read, at + 27, lacing)) break;
        auto data = at + 27 + segments;
        auto& stream = streams[serial];
        for (std::size_t i = 0; i < segments; ++i) {
            const auto bytes = lacing[i];
            if (bytes) stream.current.emplace_back(data, bytes);
            stream.current_size += bytes;
            data += bytes;
            if (bytes == 255) continue;
            // A packet ends here.
            auto pieces = std::move(stream.current);
            const auto packet_size = std::exchange(stream.current_size, 0);
            stream.current.clear();
            const auto head_size = static_cast<std::size_t>(std::min<std::uint64_t>(packet_size, 64));
            std::vector<std::uint8_t> head;
            for (const auto& [o, n] : pieces) {
                const auto more = read_bytes(read, o, std::min<std::uint64_t>(n, head_size - head.size()));
                head.insert(head.end(), more.begin(), more.end());
                if (head.size() >= head_size) break;
            }
            auto& t = stream.track;
            if (stream.headers_seen == 0) {
                if (head.size() >= 19 && std::memcmp(head.data(), "OpusHead", 8) == 0) {
                    stream.opus = true;
                    t.kind = Track::Kind::audio;
                    t.codec = fourcc("opus");
                    t.codec_name = "OpusHead";
                    t.channels = head[9];
                    t.priming = le16(head.data() + 10);
                    t.sample_rate = 48000;
                    t.timescale = 48000;
                    t.config = read_bytes(read, pieces.front().first, std::min<std::uint64_t>(pieces.front().second, packet_size));
                } else if (head.size() >= 51 && head[0] == 0x7f && std::memcmp(head.data() + 1, "FLAC", 4) == 0) {
                    stream.flac = true;
                    t.kind = Track::Kind::audio;
                    t.codec = fourcc("flac");
                    t.codec_name = "FLAC";
                    // "fLaC" + the STREAMINFO block follow the 9-byte Ogg mapping header.
                    t.config.assign(head.begin() + 13, head.begin() + 51);
                    const auto* info = head.data() + 17;
                    stream.flac_rate = (std::uint32_t(info[10]) << 12) | (info[11] << 4) | (info[12] >> 4);
                    t.channels = ((info[12] >> 1) & 7) + 1U;
                    t.bits = (((info[12] & 1) << 4) | (info[13] >> 4)) + 1U;
                    t.sample_rate = stream.flac_rate;
                    t.timescale = stream.flac_rate ? stream.flac_rate : 1;
                }
                t.id = serial;
                ++stream.headers_seen;
                continue;
            }
            if (stream.opus && stream.headers_seen == 1) { ++stream.headers_seen; continue; }   // OpusTags
            if (stream.flac && !head.empty() && (head[0] & 0x7f) != 0x7f && head[0] != 0xff) { continue; }   // metadata packets
            if (!stream.opus && !stream.flac) continue;
            Sample sample;
            sample.size = packet_size;
            if (pieces.size() == 1) {
                sample.offset = pieces.front().first;
            } else {
                sample.offset = t.pieces.size();
                sample.pieces = static_cast<std::uint32_t>(pieces.size());
                t.pieces.insert(t.pieces.end(), pieces.begin(), pieces.end());
            }
            if (stream.opus) sample.duration = opus_samples(head);
            else {
                std::uint32_t number{};
                bool variable{};
                sample.duration = flac_block_size(head, number, variable);
            }
            sample.pts = sample.dts = stream.time;
            sample.keyframe = true;
            stream.time += sample.duration;
            if (t.samples.size() < maximum_samples) t.samples.push_back(sample);
        }
        at = data;
    }
    for (auto& [serial, stream] : streams) {
        if (!stream.opus && !stream.flac) continue;
        stream.track.duration = stream.time;
        out.tracks.push_back(std::move(stream.track));
    }
    if (out.tracks.empty()) { why = "no Opus or FLAC stream in the Ogg file"; return false; }
    out.timescale = out.tracks.front().timescale;
    out.duration = out.tracks.front().duration;
    return true;
}

// ---- FLAC -----------------------------------------------------------------------------------------

namespace {

std::uint8_t crc8(const std::uint8_t* p, std::size_t n) {
    std::uint8_t crc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = static_cast<std::uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

std::uint32_t flac_block_size(std::span<const std::uint8_t> f, std::uint32_t& sample_number_out, bool& variable) {
    if (f.size() < 6 || f[0] != 0xff || (f[1] & 0xfe) != 0xf8) return 0;
    variable = f[1] & 1;
    const auto block_code = f[2] >> 4, rate_code = f[2] & 0xf;
    if ((f[3] >> 4) > 10 || ((f[3] >> 1) & 7) == 3 || ((f[3] >> 1) & 7) == 7 || rate_code == 15) return 0;
    // The coded number (UTF-8 style, up to 7 bytes).
    std::size_t at = 4;
    std::uint64_t number = f[at];
    std::size_t extra = 0;
    if (number & 0x80) {
        if ((number & 0xe0) == 0xc0) { number &= 0x1f; extra = 1; }
        else if ((number & 0xf0) == 0xe0) { number &= 0x0f; extra = 2; }
        else if ((number & 0xf8) == 0xf0) { number &= 0x07; extra = 3; }
        else if ((number & 0xfc) == 0xf8) { number &= 0x03; extra = 4; }
        else if ((number & 0xfe) == 0xfc) { number &= 0x01; extra = 5; }
        else if (number == 0xfe) { number = 0; extra = 6; }
        else return 0;
    }
    ++at;
    for (std::size_t i = 0; i < extra; ++i, ++at) {
        if (at >= f.size() || (f[at] & 0xc0) != 0x80) return 0;
        number = (number << 6) | (f[at] & 0x3f);
    }
    std::uint32_t block = 0;
    switch (block_code) {
    case 0: return 0;
    case 1: block = 192; break;
    case 2: case 3: case 4: case 5: block = 576U << (block_code - 2); break;
    case 6: if (at >= f.size()) return 0; block = f[at++] + 1U; break;
    case 7: if (at + 1 >= f.size()) return 0; block = be16(f.data() + at) + 1U; at += 2; break;
    default: block = 256U << (block_code - 8); break;
    }
    if (rate_code == 12) ++at;
    else if (rate_code == 13 || rate_code == 14) at += 2;
    if (at >= f.size() || crc8(f.data(), at) != f[at]) return 0;
    sample_number_out = static_cast<std::uint32_t>(number);
    return block;
}

} // namespace

bool parse_flac(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why) {
    out = Movie{};
    out.brand = fourcc("fLaC");
    std::array<std::uint8_t, 4> magic{};
    if (!read_exact(read, 0, magic) || std::memcmp(magic.data(), "fLaC", 4) != 0) { why = "not a FLAC stream"; return false; }
    Track t;
    t.kind = Track::Kind::audio;
    t.codec = fourcc("flac");
    t.codec_name = "fLaC";
    t.id = 1;
    std::uint64_t at = 4;
    bool last = false, have_info = false;
    std::uint64_t total_samples = 0;
    while (!last && at + 4 <= size) {
        std::array<std::uint8_t, 4> h{};
        if (!read_exact(read, at, h)) break;
        last = h[0] & 0x80;
        const auto type = h[0] & 0x7f;
        const auto length = be24(h.data() + 1);
        if (type == 0 && length >= 34) {
            const auto info = read_bytes(read, at + 4, 34);
            if (info.size() < 34) break;
            // The configuration as MP4's dfLa carries it: the STREAMINFO block with its header.
            t.config.assign(h.begin(), h.end());
            t.config.insert(t.config.end(), info.begin(), info.end());
            t.config[0] |= 0x80;
            t.sample_rate = (std::uint32_t(info[10]) << 12) | (info[11] << 4) | (info[12] >> 4);
            t.channels = ((info[12] >> 1) & 7) + 1U;
            t.bits = (((info[12] & 1) << 4) | (info[13] >> 4)) + 1U;
            total_samples = (std::uint64_t(info[13] & 0x0f) << 32) | be32(info.data() + 14);
            have_info = true;
        }
        at += 4 + length;
    }
    if (!have_info || !t.sample_rate) { why = "FLAC without STREAMINFO"; return false; }
    t.timescale = static_cast<std::uint32_t>(t.sample_rate);
    // Frames: each starts at a valid frame header (sync code and header CRC-8).
    constexpr std::size_t chunk = 1 << 20;
    std::vector<std::uint64_t> starts;
    std::vector<std::uint32_t> blocks, numbers;
    std::vector<bool> variable_flags;
    std::uint64_t next_number = 0;   // the frame number (fixed block size) or sample number (variable) expected next
    for (std::uint64_t base = at; base < size;) {
        const auto data = read_bytes(read, base, std::min<std::uint64_t>(chunk + 32, size - base));
        if (data.size() < 2) break;
        const auto limit = std::min(data.size(), chunk);
        for (std::size_t i = 0; i + 1 < limit; ++i) {
            if (data[i] != 0xff || (data[i + 1] & 0xfe) != 0xf8) continue;
            std::uint32_t number{};
            bool variable{};
            const auto block = flac_block_size(std::span(data).subspan(i, std::min<std::size_t>(16, data.size() - i)), number, variable);
            // A sync pattern inside frame data passes the CRC-8 now and then: the numbers must follow on.
            if (!block || number != next_number) continue;
            next_number = variable ? std::uint64_t{number} + block : std::uint64_t{number} + 1;
            starts.push_back(base + i);
            blocks.push_back(block);
            numbers.push_back(number);
            variable_flags.push_back(variable);
            if (starts.size() >= maximum_samples) break;
        }
        if (starts.size() >= maximum_samples || data.size() <= chunk) break;
        base += chunk;
    }
    std::int64_t time = 0;
    for (std::size_t i = 0; i < starts.size(); ++i) {
        Sample s;
        s.offset = starts[i];
        s.size = (i + 1 < starts.size() ? starts[i + 1] : size) - starts[i];
        s.duration = blocks[i];
        s.pts = s.dts = variable_flags[i] ? static_cast<std::int64_t>(numbers[i]) : time;
        s.keyframe = true;
        time = s.pts + blocks[i];
        t.samples.push_back(s);
    }
    t.duration = total_samples ? static_cast<std::int64_t>(total_samples) : time;
    out.timescale = t.timescale;
    out.duration = t.duration;
    out.tracks.push_back(std::move(t));
    return true;
}

// ---- MPEG audio -----------------------------------------------------------------------------------

MpegFrame mpeg_frame(const std::uint8_t* p) {
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0) return {};
    const auto version = (p[1] >> 3) & 3;   // 0: 2.5, 2: 2, 3: 1
    const auto layer = (p[1] >> 1) & 3;     // 1: III, 2: II, 3: I
    const auto bitrate_index = p[2] >> 4, rate_index = (p[2] >> 2) & 3, padding = (p[2] >> 1) & 1;
    if (version == 1 || layer == 0 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return {};
    static constexpr std::uint16_t v1[3][15] = {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},   // layer I
                                                 {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},     // II
                                                 {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}};     // III
    static constexpr std::uint16_t v2[2][15] = {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},     // I
                                                 {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}};         // II, III
    static constexpr std::uint32_t rates[3] = {44100, 48000, 32000};
    const auto layer_index = 3 - layer;   // 0: I, 1: II, 2: III
    const std::uint32_t bitrate = 1000U * (version == 3 ? v1[layer_index][bitrate_index] : v2[layer_index == 0 ? 0 : 1][bitrate_index]);
    std::uint32_t rate = rates[rate_index];
    if (version == 2) rate /= 2;
    else if (version == 0) rate /= 4;
    MpegFrame f;
    f.rate = rate;
    f.channels = ((p[3] >> 6) & 3) == 3 ? 1 : 2;
    if (layer_index == 0) {
        f.samples = 384;
        f.bytes = (12 * bitrate / rate + padding) * 4;
    } else {
        f.samples = (layer_index == 2 && version != 3) ? 576 : 1152;
        f.bytes = (f.samples / 8) * bitrate / rate + padding;
    }
    return f;
}

bool parse_mpeg_audio(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why) {
    out = Movie{};
    out.brand = fourcc("mp3 ");
    std::uint64_t at = 0;
    std::array<std::uint8_t, 10> id3{};
    while (read_exact(read, at, id3) && std::memcmp(id3.data(), "ID3", 3) == 0)
        at += 10 + ((std::uint64_t(id3[6] & 0x7f) << 21) | ((id3[7] & 0x7f) << 14) | ((id3[8] & 0x7f) << 7) | (id3[9] & 0x7f)) + ((id3[5] & 0x10) ? 10 : 0);
    Track t;
    t.kind = Track::Kind::audio;
    t.codec = fourcc(".mp3");
    t.codec_name = "MPEG audio";
    t.id = 1;
    std::int64_t time = 0;
    std::uint32_t encoder_delay = 0, padding = 0;
    bool first = true;
    constexpr std::size_t chunk = 1 << 20;
    std::vector<std::uint8_t> data;
    std::uint64_t data_base = 0;
    const auto bytes_at = [&](std::uint64_t offset, std::size_t n) -> const std::uint8_t* {
        if (offset < data_base || offset + n > data_base + data.size()) {
            data_base = offset;
            data = read_bytes(read, offset, std::min<std::uint64_t>(chunk, size - offset));
        }
        return offset + n <= data_base + data.size() ? data.data() + (offset - data_base) : nullptr;
    };
    std::size_t resync = 0;
    while (at + 4 <= size && t.samples.size() < maximum_samples) {
        const auto* h = bytes_at(at, 4);
        if (!h) break;
        const auto f = mpeg_frame(h);
        if (!f.bytes || (!t.samples.empty() && (f.rate != t.sample_rate))) {
            if (++resync > 65536) break;
            ++at;   // resynchronise byte by byte
            continue;
        }
        resync = 0;
        if (first) {
            first = false;
            t.sample_rate = f.rate;
            t.channels = f.channels;
            t.timescale = f.rate;
            // A Xing/Info frame (no audio): its LAME tag gives the encoder delay and padding.
            if (const auto* frame = bytes_at(at, std::min<std::uint64_t>(f.bytes, size - at))) {
                const std::size_t side = (h[1] & 0x08) ? (f.channels == 1 ? 17 : 32) : (f.channels == 1 ? 9 : 17);
                if (f.bytes >= 4 + side + 8 && (std::memcmp(frame + 4 + side, "Xing", 4) == 0 || std::memcmp(frame + 4 + side, "Info", 4) == 0)) {
                    const auto flags = be32(frame + 4 + side + 4);
                    std::size_t tag = 4 + side + 8 + ((flags & 1) ? 4 : 0) + ((flags & 2) ? 4 : 0) + ((flags & 4) ? 100 : 0) + ((flags & 8) ? 4 : 0);
                    if (tag + 24 <= f.bytes) {
                        encoder_delay = (std::uint32_t(frame[tag + 21]) << 4) | (frame[tag + 22] >> 4);
                        padding = ((frame[tag + 22] & 0x0f) << 8) | frame[tag + 23];
                    }
                    at += f.bytes;
                    continue;
                }
            }
        }
        Sample s;
        s.offset = at;
        s.size = std::min<std::uint64_t>(f.bytes, size - at);
        s.pts = s.dts = time;
        s.duration = f.samples;
        s.keyframe = true;
        time += f.samples;
        t.samples.push_back(s);
        at += f.bytes;
    }
    if (t.samples.empty()) { why = "no MPEG audio frames"; return false; }
    // The decoder's own delay (529 samples) follows the encoder's.
    t.priming = encoder_delay ? encoder_delay + 529 : 0;
    t.duration = time - (encoder_delay ? encoder_delay + 529 + padding : 0);
    if (t.duration < 0) t.duration = 0;
    out.timescale = t.timescale;
    out.duration = t.duration;
    out.tracks.push_back(std::move(t));
    return true;
}

// ---- dispatch -------------------------------------------------------------------------------------

bool parse(const ReadAt& read, std::uint64_t size, Movie& out, std::string& why) {
    std::array<std::uint8_t, 12> head{};
    const auto got = read(0, head);
    if (got < 4) { why = "file too short"; return false; }
    const auto at4 = got >= 8 ? be32(head.data() + 4) : 0;
    if (at4 == fourcc("ftyp") || at4 == fourcc("moov") || at4 == fourcc("mdat") || at4 == fourcc("wide") || at4 == fourcc("free") || at4 == fourcc("skip"))
        return parse_iso(read, size, out, why);
    if (be32(head.data()) == 0x1A45DFA3) return parse_matroska(read, size, out, why);
    if (std::memcmp(head.data(), "OggS", 4) == 0) return parse_ogg(read, size, out, why);
    if (std::memcmp(head.data(), "fLaC", 4) == 0) return parse_flac(read, size, out, why);
    if (std::memcmp(head.data(), "ID3", 3) == 0 || mpeg_frame(head.data()).bytes) return parse_mpeg_audio(read, size, out, why);
    why = "not an MP4, QuickTime, Matroska, WebM, Ogg, FLAC or MP3 file";
    return false;
}

std::vector<std::uint8_t> read_sample(const ReadAt& read, const Track& track, const Sample& sample) {
    if (!sample.pieces) {
        auto bytes = read_bytes(read, sample.offset, sample.size);
        if (bytes.size() != sample.size) bytes.clear();
        return bytes;
    }
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(sample.size));
    for (std::uint32_t i = 0; i < sample.pieces; ++i) {
        const auto index = static_cast<std::size_t>(sample.offset) + i;
        if (index >= track.pieces.size()) return {};
        const auto [offset, n] = track.pieces[index];
        const auto bytes = read_bytes(read, offset, n);
        if (bytes.size() != n) return {};
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

} // namespace alchemy::gui::completion_kit::media::container
