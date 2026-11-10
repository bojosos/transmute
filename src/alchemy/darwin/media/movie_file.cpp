#include "alchemy/darwin/media/movie_file.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace alchemy::gui::completion_kit::media::movie_file {

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
Bytes full(std::uint32_t type, int version, std::uint32_t flags, const Bytes& payload) {
    Bytes body;
    put(body, static_cast<std::uint64_t>(version), 1);
    put(body, flags, 3);
    append(body, payload);
    return box(type, body);
}

// Fixed point: 16.16 for a, b, c, d, tx, ty; 2.30 for u, v, w (0, 0, 1).
std::uint32_t fixed(double value, int fraction_bits) {
    return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::lround(value * double(1 << fraction_bits))));
}
void put_matrix(Bytes& out, const std::array<double, 6>& t) {
    put(out, fixed(t[0], 16), 4);
    put(out, fixed(t[1], 16), 4);
    put(out, 0, 4);
    put(out, fixed(t[2], 16), 4);
    put(out, fixed(t[3], 16), 4);
    put(out, 0, 4);
    put(out, fixed(t[4], 16), 4);
    put(out, fixed(t[5], 16), 4);
    put(out, 0x40000000, 4);
}

std::uint64_t rescale(std::uint64_t value, std::uint32_t from, std::uint32_t to) {
    if (!from) return 0;
    return static_cast<std::uint64_t>((static_cast<long double>(value) * to) / from + 0.5L);
}

} // namespace

Writer::Writer(Brand brand) : brand_(brand) {
    Bytes ftyp;
    switch (brand) {
    case Brand::mp4:
        put(ftyp, four("mp42"), 4);
        put(ftyp, 0, 4);
        for (const auto b : {four("mp42"), four("isom")}) put(ftyp, b, 4);
        break;
    case Brand::m4v:
        put(ftyp, four("M4V "), 4);
        put(ftyp, 1, 4);
        for (const auto b : {four("M4V "), four("M4A "), four("mp42"), four("isom")}) put(ftyp, b, 4);
        break;
    case Brand::quicktime:
        put(ftyp, four("qt  "), 4);
        put(ftyp, 0, 4);
        put(ftyp, four("qt  "), 4);
        break;
    }
    header_ = box(four("ftyp"), ftyp);
    // mdat with a 64-bit size (set by finish).
    put(header_, 1, 4);
    put(header_, four("mdat"), 4);
    put(header_, 16, 8);
    data_end_ = header_.size();
}

std::size_t Writer::add_track(const TrackFormat& format) {
    Track t;
    t.format = format;
    if (!t.format.timescale) t.format.timescale = format.video ? 600 : static_cast<std::uint32_t>(format.sample_rate);
    if (!t.format.timescale) t.format.timescale = 1;
    tracks_.push_back(std::move(t));
    return tracks_.size() - 1;
}

void Writer::set_format(std::size_t track, const TrackFormat& format) {
    auto& t = tracks_.at(track);
    t.format = format;
    if (!t.format.timescale) t.format.timescale = format.video ? 600 : static_cast<std::uint32_t>(format.sample_rate);
    if (!t.format.timescale) t.format.timescale = 1;
}

void Writer::set_config(std::size_t track, std::vector<std::uint8_t> config) { tracks_.at(track).config = std::move(config); }

void Writer::set_trim(std::size_t track, std::uint64_t priming, std::uint64_t presented) {
    tracks_.at(track).priming = priming;
    tracks_.at(track).presented = presented;
}

Writes Writer::add_sample(std::size_t track, std::vector<std::uint8_t> bytes, std::int64_t dts, std::int64_t pts, std::uint32_t duration, bool sync) {
    auto& t = tracks_.at(track);
    Sample s{static_cast<std::uint32_t>(bytes.size()), dts, pts, duration, sync};
    t.pending_duration += duration;
    t.pending.emplace_back(s, std::move(bytes));
    // Half a second in one track: every pending chunk goes out.
    if (t.pending_duration * 2 >= t.format.timescale) return flush();
    return {};
}

Writes Writer::flush() {
    Writes writes;
    // The tracks' pending chunks, the earliest first.
    std::vector<std::size_t> order;
    for (std::size_t i = 0; i < tracks_.size(); ++i)
        if (!tracks_[i].pending.empty()) order.push_back(i);
    const auto start = [this](std::size_t i) {
        const auto& t = tracks_[i];
        return static_cast<long double>(t.pending.front().first.dts) / t.format.timescale;
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return start(a) < start(b); });
    for (const auto i : order) {
        auto& t = tracks_[i];
        Bytes chunk;
        for (auto& [sample, bytes] : t.pending) {
            append(chunk, bytes);
            t.samples.push_back(sample);
        }
        t.chunks.push_back({data_end_, static_cast<std::uint32_t>(t.pending.size())});
        t.pending.clear();
        t.pending_duration = 0;
        data_end_ += chunk.size();
        writes.emplace_back(t.chunks.back().offset, std::move(chunk));
    }
    return writes;
}

Writer::Finish Writer::finish(bool moov_first) {
    Finish out;
    out.writes = flush();
    const std::uint64_t ftyp_size = header_.size() - 16;
    const std::uint64_t mdat_size = data_end_ - ftyp_size;
    if (!moov_first) {
        Bytes size;
        put(size, mdat_size, 8);
        out.writes.emplace_back(ftyp_size + 8, std::move(size));
        out.writes.emplace_back(data_end_, moov(0));
        return out;
    }
    // The moov goes between ftyp and mdat: the data moves up by its size (which may itself grow when the offsets
    // pass 4 GiB and stco becomes co64).
    Bytes m = moov(0);
    for (int i = 0; i < 3; ++i) {
        Bytes again = moov(m.size());
        const bool stable = again.size() == m.size();
        m = std::move(again);
        if (stable) break;
    }
    out.move_from = ftyp_size;
    out.move_by = m.size();
    // The chunks flushed just now are not in the file yet: they go to their places after the move.
    for (auto& [offset, bytes] : out.writes) offset += m.size();
    Bytes size;
    put(size, mdat_size, 8);
    out.writes.emplace_back(ftyp_size + m.size() + 8, std::move(size));
    out.writes.emplace_back(ftyp_size, std::move(m));
    return out;
}

std::vector<std::uint8_t> Writer::moov(std::uint64_t offset_shift) const {
    const auto scale = movie_timescale();
    // The longest track's presentation, in the movie's timescale.
    std::uint64_t longest = 0;
    Bytes traks;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        std::uint64_t length = 0;
        append(traks, trak(i, offset_shift, length));
        longest = std::max(longest, length);
    }
    const int version = longest > 0xffffffffULL ? 1 : 0;
    Bytes mvhd;
    put(mvhd, 0, version ? 8 : 4);
    put(mvhd, 0, version ? 8 : 4);
    put(mvhd, scale, 4);
    put(mvhd, longest, version ? 8 : 4);
    put(mvhd, 0x00010000, 4);   // rate 1.0
    put(mvhd, 0x0100, 2);       // volume 1.0
    put(mvhd, 0, 2 + 8);
    put_matrix(mvhd, {1, 0, 0, 1, 0, 0});
    put(mvhd, 0, 24);           // pre_defined (QuickTime: preview, poster, selection and current times)
    put(mvhd, tracks_.size() + 1, 4);
    Bytes body = full(four("mvhd"), version, 0, mvhd);
    append(body, traks);
    return box(four("moov"), body);
}

std::vector<std::uint8_t> Writer::trak(std::size_t index, std::uint64_t offset_shift, std::uint64_t& movie_length) const {
    const auto& t = tracks_[index];
    const auto& f = t.format;
    const bool qt = brand_ == Brand::quicktime;
    const auto scale = movie_timescale();
    const auto& samples = t.samples;
    movie_length = 0;
    if (samples.empty()) return {};
    // Decode times from the first sample's; durations as decode-time steps (the last its own duration).
    const std::int64_t first_dts = samples.front().dts;
    std::vector<std::uint32_t> durations(samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::int64_t step = i + 1 < samples.size() ? samples[i + 1].dts - samples[i].dts : samples[i].duration;
        durations[i] = step > 0 ? static_cast<std::uint32_t>(step) : samples[i].duration;
    }
    std::uint64_t media = 0;
    for (const auto d : durations) media += d;
    // Composition offsets (pts - dts), the presentation's start and end in media time.
    bool offsets = false, negative = false;
    std::int64_t first_pts = std::numeric_limits<std::int64_t>::max(), last_end = std::numeric_limits<std::int64_t>::min();
    for (const auto& s : samples) {
        offsets = offsets || s.pts != s.dts;
        negative = negative || s.pts < s.dts;
        first_pts = std::min(first_pts, s.pts);
        last_end = std::max(last_end, s.pts + std::int64_t{s.duration});
    }
    // The edits: an empty one while the track has not started (video after the session's start), then the media
    // from its first presented time.
    std::uint64_t empty = 0, media_time = 0, presented = 0;
    if (f.video) {
        empty = first_pts > 0 ? static_cast<std::uint64_t>(first_pts) : 0;
        media_time = static_cast<std::uint64_t>(first_pts - first_dts);
        presented = static_cast<std::uint64_t>(std::max<std::int64_t>(0, last_end - std::max<std::int64_t>(first_pts, 0)));
        if (first_pts < 0) media_time += static_cast<std::uint64_t>(-first_pts);
    } else {
        media_time = t.priming;
        presented = t.presented ? t.presented : (media > t.priming ? media - t.priming : 0);
    }
    const std::uint64_t movie_empty = rescale(empty, f.timescale, scale), movie_presented = rescale(presented, f.timescale, scale);
    const bool edited = movie_empty != 0 || media_time != 0;
    const std::uint64_t track_length = movie_empty + movie_presented;
    movie_length = track_length;
    const int version = std::max({track_length, media}) > 0xffffffffULL ? 1 : 0;

    // tkhd: enabled, in the movie and its preview.
    Bytes tkhd;
    put(tkhd, 0, version ? 8 : 4);
    put(tkhd, 0, version ? 8 : 4);
    put(tkhd, index + 1, 4);
    put(tkhd, 0, 4);
    put(tkhd, track_length, version ? 8 : 4);
    put(tkhd, 0, 8);            // reserved
    put(tkhd, 0, 2);            // layer
    put(tkhd, f.video ? 0 : 1, 2);   // alternate group
    put(tkhd, f.video ? 0 : 0x0100, 2);
    put(tkhd, 0, 2);
    put_matrix(tkhd, f.transform);
    put(tkhd, f.video ? std::uint64_t{f.width} << 16 : 0, 4);
    put(tkhd, f.video ? std::uint64_t{f.height} << 16 : 0, 4);
    Bytes body = full(four("tkhd"), version, 3, tkhd);
    if (edited) {
        const bool wide = std::max({movie_empty, movie_presented, media_time}) > 0x7fffffffULL;
        Bytes elst;
        put(elst, (movie_empty ? 1 : 0) + 1, 4);
        const auto edit = [&](std::uint64_t duration, std::int64_t time) {
            put(elst, duration, wide ? 8 : 4);
            put(elst, static_cast<std::uint64_t>(time), wide ? 8 : 4);
            put(elst, 0x00010000, 4);   // rate 1.0
        };
        if (movie_empty) edit(movie_empty, -1);
        edit(movie_presented, static_cast<std::int64_t>(media_time));
        append(body, box(four("edts"), full(four("elst"), wide ? 1 : 0, 0, elst)));
    }

    // mdia
    Bytes mdhd;
    put(mdhd, 0, version ? 8 : 4);
    put(mdhd, 0, version ? 8 : 4);
    put(mdhd, f.timescale, 4);
    put(mdhd, media, version ? 8 : 4);
    put(mdhd, 0x55c4, 2);       // "und"
    put(mdhd, 0, 2);
    const std::string handler_name = f.video ? "Core Media Video" : "Core Media Audio";
    Bytes hdlr;
    put(hdlr, qt ? four("mhlr") : 0, 4);   // QuickTime: the component type
    put(hdlr, f.video ? four("vide") : four("soun"), 4);
    put(hdlr, 0, 12);
    if (qt) hdlr.push_back(static_cast<std::uint8_t>(handler_name.size()));   // a counted string
    for (const char c : handler_name) hdlr.push_back(static_cast<std::uint8_t>(c));
    if (!qt) hdlr.push_back(0);

    // The sample entry.
    Bytes entry;
    put(entry, 0, 6);
    put(entry, 1, 2);           // data_reference_index
    if (f.video) {
        put(entry, 0, 2 + 2 + 12);
        put(entry, f.width, 2);
        put(entry, f.height, 2);
        put(entry, 0x00480000, 4);   // 72 dpi
        put(entry, 0x00480000, 4);
        put(entry, 0, 4);
        put(entry, 1, 2);            // frame_count
        const std::string compressor = f.codec == four("hvc1") ? "HEVC" : "H.264";
        Bytes name(32, 0);
        name[0] = static_cast<std::uint8_t>(compressor.size());
        std::copy(compressor.begin(), compressor.end(), name.begin() + 1);
        append(entry, name);
        put(entry, 0x0018, 2);       // depth
        put(entry, 0xffff, 2);       // pre_defined -1
        append(entry, box(f.codec == four("hvc1") ? four("hvcC") : four("avcC"), t.config));
    } else {
        const bool lossless = f.codec == four("alac");
        const auto rate = static_cast<std::uint32_t>(f.sample_rate);
        put(entry, qt ? 1 : 0, 2);   // QuickTime: sound description version 1
        put(entry, 0, 2 + 4);        // revision, vendor
        put(entry, f.channels, 2);
        put(entry, lossless ? f.bits : 16, 2);
        put(entry, qt ? 0xfffe : 0, 2);   // QuickTime: compression ID -2 (compressed)
        put(entry, 0, 2);
        put(entry, rate <= 0xffff ? std::uint64_t{rate} << 16 : 0, 4);
        // The configuration box: ALAC's ALACSpecificConfig, AAC's ES descriptor.
        Bytes config = full(lossless ? four("alac") : four("esds"), 0, 0, t.config);
        if (qt) {
            // Version 1 fields: samples per packet, bytes per packet, bytes per frame, bytes per sample.
            put(entry, lossless ? 4096 : 1024, 4);
            put(entry, 0, 4);
            put(entry, 0, 4);
            put(entry, 2, 4);
            Bytes wave;
            Bytes frma;
            put(frma, f.codec, 4);
            append(wave, box(four("frma"), frma));
            if (!lossless) append(wave, box(four("mp4a"), Bytes(4, 0)));
            append(wave, config);
            append(wave, Bytes(8, 0));   // the terminator atom
            append(entry, box(four("wave"), wave));
        } else {
            append(entry, config);
        }
    }
    Bytes stsd;
    put(stsd, 1, 4);
    append(stsd, box(f.codec, entry));

    // stts: runs of equal durations.
    Bytes stts;
    {
        Bytes runs;
        std::uint32_t count = 0;
        for (std::size_t i = 0; i < durations.size();) {
            std::size_t j = i;
            while (j < durations.size() && durations[j] == durations[i]) ++j;
            put(runs, j - i, 4);
            put(runs, durations[i], 4);
            ++count;
            i = j;
        }
        put(stts, count, 4);
        append(stts, runs);
    }
    Bytes stbl;
    append(stbl, full(four("stsd"), 0, 0, stsd));
    append(stbl, full(four("stts"), 0, 0, stts));
    // ctts: runs of equal offsets (version 1: signed, when a presentation precedes its decode).
    if (offsets) {
        Bytes runs;
        std::uint32_t count = 0;
        for (std::size_t i = 0; i < samples.size();) {
            const auto offset = samples[i].pts - samples[i].dts;
            std::size_t j = i;
            while (j < samples.size() && samples[j].pts - samples[j].dts == offset) ++j;
            put(runs, j - i, 4);
            put(runs, static_cast<std::uint32_t>(static_cast<std::int32_t>(offset)), 4);
            ++count;
            i = j;
        }
        Bytes ctts;
        put(ctts, count, 4);
        append(ctts, runs);
        append(stbl, full(four("ctts"), negative ? 1 : 0, 0, ctts));
    }
    // stss: the sync samples (1-based), unless every sample is one.
    if (std::any_of(samples.begin(), samples.end(), [](const Sample& s) { return !s.sync; })) {
        Bytes stss;
        std::vector<std::uint32_t> sync;
        for (std::size_t i = 0; i < samples.size(); ++i)
            if (samples[i].sync) sync.push_back(static_cast<std::uint32_t>(i + 1));
        put(stss, sync.size(), 4);
        for (const auto n : sync) put(stss, n, 4);
        append(stbl, full(four("stss"), 0, 0, stss));
    }
    // stsc: runs of chunks with the same sample count.
    {
        Bytes runs;
        std::uint32_t count = 0;
        for (std::size_t i = 0; i < t.chunks.size();) {
            std::size_t j = i;
            while (j < t.chunks.size() && t.chunks[j].count == t.chunks[i].count) ++j;
            put(runs, i + 1, 4);
            put(runs, t.chunks[i].count, 4);
            put(runs, 1, 4);
            ++count;
            i = j;
        }
        Bytes stsc;
        put(stsc, count, 4);
        append(stsc, runs);
        append(stbl, full(four("stsc"), 0, 0, stsc));
    }
    // stsz: one size for all, or each.
    {
        Bytes stsz;
        const bool same = std::all_of(samples.begin(), samples.end(), [&](const Sample& s) { return s.size == samples.front().size; });
        put(stsz, same ? samples.front().size : 0, 4);
        put(stsz, samples.size(), 4);
        if (!same)
            for (const auto& s : samples) put(stsz, s.size, 4);
        append(stbl, full(four("stsz"), 0, 0, stsz));
    }
    // stco, or co64 when an offset needs 64 bits.
    {
        const bool wide = std::any_of(t.chunks.begin(), t.chunks.end(), [&](const Chunk& c) { return c.offset + offset_shift > 0xffffffffULL; });
        Bytes table;
        put(table, t.chunks.size(), 4);
        for (const auto& c : t.chunks) put(table, c.offset + offset_shift, wide ? 8 : 4);
        append(stbl, full(wide ? four("co64") : four("stco"), 0, 0, table));
    }

    Bytes minf;
    if (f.video) {
        Bytes vmhd;
        put(vmhd, 0, 2 + 6);   // graphicsmode, opcolor
        append(minf, full(four("vmhd"), 0, 1, vmhd));
    } else {
        Bytes smhd;
        put(smhd, 0, 4);       // balance, reserved
        append(minf, full(four("smhd"), 0, 0, smhd));
    }
    Bytes dref;
    put(dref, 1, 4);
    append(dref, full(four("url "), 0, 1, {}));   // self-contained
    append(minf, box(four("dinf"), full(four("dref"), 0, 0, dref)));
    append(minf, box(four("stbl"), stbl));

    Bytes mdia = full(four("mdhd"), version, 0, mdhd);
    append(mdia, full(four("hdlr"), 0, 0, hdlr));
    append(mdia, box(four("minf"), minf));
    append(body, box(four("mdia"), mdia));
    return box(four("trak"), body);
}

} // namespace alchemy::gui::completion_kit::media::movie_file
