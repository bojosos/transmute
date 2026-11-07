#include "alchemy/darwin/media/video_config.hpp"

#include <algorithm>
#include <array>

namespace alchemy::gui::completion_kit::media::video_config {

namespace {

// A NAL unit's payload after its header, without emulation prevention bytes, read MSB first (exp-Golomb too).
struct Bits {
    std::vector<std::uint8_t> bytes;
    std::size_t at{};
    bool failed{};
    Bits(std::span<const std::uint8_t> nal, std::size_t header) {
        for (std::size_t i = header, zeros = 0; i < nal.size(); ++i) {
            if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
            bytes.push_back(nal[i]);
            zeros = nal[i] == 0 ? zeros + 1 : 0;
        }
    }
    std::uint32_t bit() {
        if (at >= bytes.size() * 8) { failed = true; return 0; }
        const auto v = (bytes[at / 8] >> (7 - at % 8)) & 1;
        ++at;
        return v;
    }
    std::uint64_t read(unsigned n) {
        std::uint64_t v = 0;
        for (unsigned i = 0; i < n; ++i) v = (v << 1) | bit();
        return v;
    }
    std::uint32_t ue() {
        unsigned zeros = 0;
        while (!bit()) { if (failed || ++zeros > 31) { failed = true; return 0; } }
        return zeros ? static_cast<std::uint32_t>((1ULL << zeros) - 1 + read(zeros)) : 0;
    }
};

void put16(std::vector<std::uint8_t>& out, std::size_t v) {
    out.push_back(static_cast<std::uint8_t>(v >> 8));
    out.push_back(static_cast<std::uint8_t>(v));
}

bool length_size_ok(unsigned n) { return n == 1 || n == 2 || n == 4; }

// The chroma format and bit depths an H.264 SPS of a high profile names (4:2:0, 8 bits otherwise).
struct AvcFormat { std::uint32_t chroma{1}, luma_depth{8}, chroma_depth{8}; };
AvcFormat avc_format(std::span<const std::uint8_t> sps) {
    AvcFormat f;
    Bits b(sps, 1);
    const auto profile = b.read(8);
    b.read(16);   // constraint flags, level
    b.ue();       // seq_parameter_set_id
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 ||
        profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        f.chroma = b.ue();
        if (f.chroma == 3) b.bit();   // separate_colour_plane_flag
        f.luma_depth = b.ue() + 8;
        f.chroma_depth = b.ue() + 8;
    }
    if (b.failed || f.chroma > 3 || f.luma_depth > 14 || f.chroma_depth > 14) return {};
    return f;
}

// What an HEVC SPS gives the record: its profile_tier_level general part (12 bytes), the sub-layer count,
// temporal id nesting, chroma format and bit depths.
struct HevcFormat {
    std::array<std::uint8_t, 12> general{};
    std::uint32_t sub_layers{1}, nested{}, chroma{1}, luma_depth{8}, chroma_depth{8};
};
std::optional<HevcFormat> hevc_format(std::span<const std::uint8_t> sps) {
    HevcFormat f;
    Bits b(sps, 2);
    b.read(4);                                    // sps_video_parameter_set_id
    const auto sub_layers = static_cast<std::uint32_t>(b.read(3));   // sps_max_sub_layers_minus1
    f.sub_layers = sub_layers + 1;
    f.nested = b.bit();
    for (auto& byte : f.general) byte = static_cast<std::uint8_t>(b.read(8));
    std::vector<std::pair<bool, bool>> present;
    for (std::uint32_t i = 0; i < sub_layers; ++i) {
        const bool profile = b.bit(), level = b.bit();
        present.emplace_back(profile, level);
    }
    if (sub_layers > 0)
        for (std::uint32_t i = sub_layers; i < 8; ++i) b.read(2);
    for (const auto& [profile, level] : present) {
        if (profile) b.read(88);
        if (level) b.read(8);
    }
    b.ue();                                       // sps_seq_parameter_set_id
    f.chroma = b.ue();
    if (f.chroma == 3) b.bit();                   // separate_colour_plane_flag
    b.ue(); b.ue();                               // pic_width / pic_height_in_luma_samples
    if (b.bit()) { b.ue(); b.ue(); b.ue(); b.ue(); }   // conformance window
    f.luma_depth = b.ue() + 8;
    f.chroma_depth = b.ue() + 8;
    if (b.failed || f.chroma > 3 || f.luma_depth > 16 || f.chroma_depth > 16) return std::nullopt;
    return f;
}

unsigned avc_type(const std::vector<std::uint8_t>& nal) { return nal.empty() ? 0 : nal[0] & 0x1f; }
unsigned hevc_type(const std::vector<std::uint8_t>& nal) { return nal.size() < 2 ? 0 : (nal[0] >> 1) & 0x3f; }

} // namespace

std::vector<std::uint8_t> avc_configuration(const NalUnits& sets, unsigned nal_length_size) {
    NalUnits sps, pps, ext;
    for (const auto& set : sets) {
        if (set.size() < 4 && avc_type(set) == 7) return {};
        if (set.size() > 0xffff) return {};
        switch (avc_type(set)) {
        case 7: sps.push_back(set); break;
        case 8: pps.push_back(set); break;
        case 13: ext.push_back(set); break;
        default: break;
        }
    }
    if (sps.empty() || pps.empty() || sps.size() > 31 || pps.size() > 255 || ext.size() > 255 || !length_size_ok(nal_length_size)) return {};
    const auto& first = sps.front();
    std::vector<std::uint8_t> out{1, first[1], first[2], first[3], static_cast<std::uint8_t>(0xfc | (nal_length_size - 1)),
                                  static_cast<std::uint8_t>(0xe0 | sps.size())};
    for (const auto& s : sps) { put16(out, s.size()); out.insert(out.end(), s.begin(), s.end()); }
    out.push_back(static_cast<std::uint8_t>(pps.size()));
    for (const auto& p : pps) { put16(out, p.size()); out.insert(out.end(), p.begin(), p.end()); }
    // The high profiles add the chroma format, bit depths and SPS extensions.
    if (first[1] == 100 || first[1] == 110 || first[1] == 122 || first[1] == 144) {
        const auto f = avc_format(first);
        out.push_back(static_cast<std::uint8_t>(0xfc | f.chroma));
        out.push_back(static_cast<std::uint8_t>(0xf8 | (f.luma_depth - 8)));
        out.push_back(static_cast<std::uint8_t>(0xf8 | (f.chroma_depth - 8)));
        out.push_back(static_cast<std::uint8_t>(ext.size()));
        for (const auto& e : ext) { put16(out, e.size()); out.insert(out.end(), e.begin(), e.end()); }
    }
    return out;
}

std::vector<std::uint8_t> hevc_configuration(const NalUnits& sets, unsigned nal_length_size) {
    // The arrays in the record's order: VPS, SPS, PPS, then SEI.
    constexpr std::array<unsigned, 4> order{32, 33, 34, 39};
    std::array<NalUnits, 4> arrays;
    for (const auto& set : sets) {
        if (set.size() > 0xffff) return {};
        const auto at = std::find(order.begin(), order.end(), hevc_type(set));
        if (at != order.end()) arrays[static_cast<std::size_t>(at - order.begin())].push_back(set);
    }
    if (arrays[1].empty() || arrays[2].empty() || !length_size_ok(nal_length_size)) return {};
    const auto f = hevc_format(arrays[1].front());
    if (!f) return {};
    std::vector<std::uint8_t> out{1};
    out.insert(out.end(), f->general.begin(), f->general.end());   // profile space .. general_level_idc
    out.push_back(0xf0); out.push_back(0x00);                       // min_spatial_segmentation_idc 0
    out.push_back(0xfc);                                            // parallelismType 0 (unknown)
    out.push_back(static_cast<std::uint8_t>(0xfc | f->chroma));
    out.push_back(static_cast<std::uint8_t>(0xf8 | (f->luma_depth - 8)));
    out.push_back(static_cast<std::uint8_t>(0xf8 | (f->chroma_depth - 8)));
    put16(out, 0);                                                  // avgFrameRate: unspecified
    out.push_back(static_cast<std::uint8_t>((std::min<std::uint32_t>(f->sub_layers, 7) << 3) | (f->nested << 2) | (nal_length_size - 1)));
    std::uint8_t count = 0;
    for (const auto& a : arrays) count += a.empty() ? 0 : 1;
    out.push_back(count);
    for (std::size_t i = 0; i < arrays.size(); ++i) {
        if (arrays[i].empty()) continue;
        out.push_back(static_cast<std::uint8_t>(0x80 | order[i]));   // array_completeness 1
        put16(out, arrays[i].size());
        for (const auto& n : arrays[i]) { put16(out, n.size()); out.insert(out.end(), n.begin(), n.end()); }
    }
    return out;
}

std::optional<Configuration> read_avc_configuration(std::span<const std::uint8_t> r) {
    if (r.size() < 7 || r[0] != 1) return std::nullopt;
    Configuration c;
    c.nal_length_size = (r[4] & 3) + 1;
    if (c.nal_length_size == 3) return std::nullopt;
    std::size_t at = 5;
    const auto take = [&](std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            if (at + 2 > r.size()) return false;
            const std::size_t length = (std::size_t(r[at]) << 8) | r[at + 1];
            at += 2;
            if (!length || at + length > r.size()) return false;
            c.sets.emplace_back(r.begin() + static_cast<std::ptrdiff_t>(at), r.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += length;
        }
        return true;
    };
    if (!take(r[at++] & 0x1f) || at >= r.size() || !take(r[at++])) return std::nullopt;
    return c;
}

std::optional<Configuration> read_hevc_configuration(std::span<const std::uint8_t> r) {
    if (r.size() < 23 || r[0] != 1) return std::nullopt;
    Configuration c;
    c.nal_length_size = (r[21] & 3) + 1;
    if (c.nal_length_size == 3) return std::nullopt;
    std::size_t at = 23;
    for (unsigned array = 0; array < r[22]; ++array) {
        if (at + 3 > r.size()) return std::nullopt;
        const std::size_t count = (std::size_t(r[at + 1]) << 8) | r[at + 2];
        at += 3;
        for (std::size_t i = 0; i < count; ++i) {
            if (at + 2 > r.size()) return std::nullopt;
            const std::size_t length = (std::size_t(r[at]) << 8) | r[at + 1];
            at += 2;
            if (!length || at + length > r.size()) return std::nullopt;
            c.sets.emplace_back(r.begin() + static_cast<std::ptrdiff_t>(at), r.begin() + static_cast<std::ptrdiff_t>(at + length));
            at += length;
        }
    }
    return c;
}

} // namespace alchemy::gui::completion_kit::media::video_config
