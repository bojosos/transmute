#include "alchemy/darwin/media/media_support.hpp"
#include "alchemy/darwin/media/audio_file_core.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

// AudioFormatGetProperty / AudioFormatGetPropertyInfo (<AudioToolbox/AudioFormat.h>) for linear PCM
// and channel layouts:
// - kAudioFormatProperty_FormatInfo completes a linear PCM description (bytes per frame and
//   packet, frames per packet); kAudioFormatProperty_FormatName names it;
//   kAudioFormatProperty_FormatIsVBR / FormatIsExternallyFramed / FormatIsEncrypted answer 0;
//   kAudioFormatProperty_EncodeFormatIDs / DecodeFormatIDs list linear PCM only (no codec exists);
// - channel layouts: NumberOfChannelsForLayout, ChannelLayoutForTag (the mono, stereo, quad and
//   MPEG 5.1 / 6.1 / 7.1 tags), ChannelLayoutForBitmap, BitmapForLayoutTag, TagsForNumberOfChannels.
// Other properties answer kAudioFormatUnsupportedPropertyError, and other formats
// kAudioFormatUnsupportedDataFormatError.
namespace alchemy::gui::completion_kit::media {
namespace {

namespace af = audio_file;

constexpr std::string_view audio_toolbox = "/System/Library/Frameworks/AudioToolbox.framework/Versions/A/AudioToolbox";
constexpr std::uint32_t four(const char (&code)[5]) { return af::four(code); }

namespace error {
inline constexpr std::int32_t unsupported_property = static_cast<std::int32_t>(four("prop")), bad_property_size = static_cast<std::int32_t>(four("!siz")),
    bad_specifier_size = static_cast<std::int32_t>(four("!spc")), unsupported_data_format = static_cast<std::int32_t>(four("fmt?")),
    unknown_format = static_cast<std::int32_t>(four("!fmt")), param = -50;
}

// AudioChannelLabel values (CoreAudioBaseTypes.h).
enum Label : std::uint32_t {
    left = 1, right = 2, center = 3, lfe = 4, left_surround = 5, right_surround = 6, center_surround = 9, rear_left = 33, rear_right = 34, mono = 42,
};
constexpr std::uint32_t tag(std::uint32_t index, std::uint32_t channels) { return (index << 16) | channels; }

struct LayoutTag { std::uint32_t tag; std::vector<std::uint32_t> labels; };
// kAudioChannelLayoutTag_* with their channel order.
const std::vector<LayoutTag>& known_tags() {
    static const std::vector<LayoutTag> tags{
        {tag(100, 1), {mono}},                                                  // Mono
        {tag(101, 2), {left, right}},                                           // Stereo
        {tag(102, 2), {left, right}},                                           // StereoHeadphones
        {tag(108, 4), {left, right, left_surround, right_surround}},            // Quadraphonic
        {tag(113, 3), {left, right, center}},                                   // MPEG_3_0_A
        {tag(115, 4), {left, right, center, center_surround}},                  // MPEG_4_0_A
        {tag(117, 5), {left, right, center, left_surround, right_surround}},    // MPEG_5_0_A
        {tag(121, 6), {left, right, center, lfe, left_surround, right_surround}},   // MPEG_5_1_A
        {tag(122, 6), {left, right, left_surround, right_surround, center, lfe}},   // MPEG_5_1_B
        {tag(123, 6), {left, center, right, left_surround, right_surround, lfe}},   // MPEG_5_1_C
        {tag(124, 6), {center, left, right, left_surround, right_surround, lfe}},   // MPEG_5_1_D
        {tag(125, 7), {left, right, center, lfe, left_surround, right_surround, center_surround}},   // MPEG_6_1_A
        {tag(128, 8), {left, right, center, lfe, left_surround, right_surround, rear_left, rear_right}},   // MPEG_7_1_C
    };
    return tags;
}

// The labels of the speaker bits of a channel bitmap, in bit order (kAudioChannelBit_*).
std::vector<std::uint32_t> bitmap_labels(std::uint32_t bitmap) {
    std::vector<std::uint32_t> labels;
    for (std::uint32_t bit = 0; bit < 18; ++bit)
        if (bitmap & (1U << bit)) labels.push_back(bit + 1);
    return labels;
}

std::vector<std::uint8_t> layout_bytes(std::uint32_t layout_tag, std::uint32_t bitmap, const std::vector<std::uint32_t>& labels) {
    std::vector<std::uint8_t> out(12 + 20 * labels.size(), 0);
    std::memcpy(out.data(), &layout_tag, 4);
    std::memcpy(out.data() + 4, &bitmap, 4);
    const auto count = static_cast<std::uint32_t>(labels.size());
    std::memcpy(out.data() + 8, &count, 4);
    for (std::size_t i = 0; i < labels.size(); ++i) std::memcpy(out.data() + 12 + 20 * i, &labels[i], 4);
    return out;
}

std::string format_name(const af::Pcm& f) {
    std::string kind = f.is_float ? "floating point" : (f.is_signed ? "signed integer" : "unsigned integer");
    std::string text = "Linear PCM, " + std::to_string(f.bits) + " bit " + (f.big_endian ? "big-endian " : "little-endian ") + kind + ", " +
                       std::to_string(f.channels) + (f.channels == 1 ? " channel, " : " channels, ") +
                       std::to_string(static_cast<long long>(f.rate)) + " Hz";
    if (f.non_interleaved && f.channels > 1) text += ", deinterleaved";
    return text;
}

class Formats : public std::enable_shared_from_this<Formats> {
public:
    explicit Formats(Support& support) : support_(support) {}
    void install();

private:
    // The property value, or an OSStatus; `string` holds a CFString result (+1, made by the caller).
    std::int32_t value(GuestMemory& m, std::uint32_t id, std::uint64_t specifier_size, std::uint64_t specifier, std::vector<std::uint8_t>& out,
                       std::string* string);
    Support& support_;
};

std::int32_t Formats::value(GuestMemory& m, std::uint32_t id, std::uint64_t specifier_size, std::uint64_t specifier, std::vector<std::uint8_t>& out,
                            std::string* string) {
    const auto spec = [&](std::size_t size) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> bytes(std::min<std::uint64_t>(specifier_size, 1 << 16));
        if (!bytes.empty() && specifier) m.read_bytes(specifier, bytes);
        if (bytes.size() < size) bytes.clear();
        return bytes;
    };
    const auto u32 = [&](std::uint32_t v) { out.resize(4); std::memcpy(out.data(), &v, 4); };
    switch (id) {
    case four("fmti"): {   // kAudioFormatProperty_FormatInfo: specifier is the partial description itself
        auto asbd = spec(40);
        if (asbd.empty()) return error::bad_specifier_size;
        std::uint32_t format_id{};
        std::memcpy(&format_id, asbd.data() + 8, 4);
        if (format_id != four("lpcm")) return error::unsupported_data_format;
        std::uint32_t flags{}, channels{}, bits{};
        std::memcpy(&flags, asbd.data() + 12, 4);
        std::memcpy(&channels, asbd.data() + 28, 4);
        std::memcpy(&bits, asbd.data() + 32, 4);
        if (!channels || !bits) return error::unknown_format;
        const std::uint32_t container = (bits + 7) / 8;
        const std::uint32_t frame = (flags & af::flag::non_interleaved) ? container : container * channels;
        const std::uint32_t one = 1;
        std::memcpy(asbd.data() + 16, &frame, 4);   // mBytesPerPacket
        std::memcpy(asbd.data() + 20, &one, 4);     // mFramesPerPacket
        std::memcpy(asbd.data() + 24, &frame, 4);   // mBytesPerFrame
        out = asbd;
        return 0;
    }
    case four("fnam"): {   // kAudioFormatProperty_FormatName (CFString)
        const auto asbd = spec(40);
        if (asbd.empty()) return error::bad_specifier_size;
        std::string why;
        const auto pcm = af::pcm_from_asbd(asbd, why);
        if (!pcm) return error::unsupported_data_format;
        if (string) *string = format_name(*pcm);
        out.assign(8, 0);
        return 0;
    }
    case four("fvbr"): case four("fexf"): case four("cryp"):   // FormatIsVBR, FormatIsExternallyFramed, FormatIsEncrypted
        u32(0);
        return 0;
    case four("acof"): case four("acif"): case four("acfd"):   // EncodeFormatIDs, DecodeFormatIDs, FormatList-adjacent: linear PCM only
        u32(four("lpcm"));
        return 0;
    case four("nchm"): {   // kAudioFormatProperty_NumberOfChannelsForLayout
        const auto layout = spec(12);
        if (layout.empty()) return error::bad_specifier_size;
        std::uint32_t layout_tag{}, bitmap{}, count{};
        std::memcpy(&layout_tag, layout.data(), 4);
        std::memcpy(&bitmap, layout.data() + 4, 4);
        std::memcpy(&count, layout.data() + 8, 4);
        u32(layout_tag == 0 ? count : layout_tag == (1U << 16) ? static_cast<std::uint32_t>(std::popcount(bitmap)) : layout_tag & 0xFFFF);
        return 0;
    }
    case four("cmpl"): {   // kAudioFormatProperty_ChannelLayoutForTag
        const auto bytes = spec(4);
        if (bytes.empty()) return error::bad_specifier_size;
        std::uint32_t wanted{};
        std::memcpy(&wanted, bytes.data(), 4);
        for (const auto& t : known_tags())
            if (t.tag == wanted) {
                out = layout_bytes(0, 0, t.labels);
                return 0;
            }
        return error::unsupported_property;
    }
    case four("cmpb"): {   // kAudioFormatProperty_ChannelLayoutForBitmap
        const auto bytes = spec(4);
        if (bytes.empty()) return error::bad_specifier_size;
        std::uint32_t bitmap{};
        std::memcpy(&bitmap, bytes.data(), 4);
        out = layout_bytes(0, 0, bitmap_labels(bitmap));
        return 0;
    }
    case four("bmtg"): {   // kAudioFormatProperty_BitmapForLayoutTag
        const auto bytes = spec(4);
        if (bytes.empty()) return error::bad_specifier_size;
        std::uint32_t wanted{};
        std::memcpy(&wanted, bytes.data(), 4);
        for (const auto& t : known_tags())
            if (t.tag == wanted) {
                std::uint32_t bitmap = 0;
                for (const auto label : t.labels)
                    if (label >= 1 && label <= 18) bitmap |= 1U << (label - 1);
                    else if (label == mono) bitmap |= 1U << (center - 1);
                u32(bitmap);
                return 0;
            }
        return error::unsupported_property;
    }
    case four("tagc"): {   // kAudioFormatProperty_TagsForNumberOfChannels
        const auto bytes = spec(4);
        if (bytes.empty()) return error::bad_specifier_size;
        std::uint32_t channels{};
        std::memcpy(&channels, bytes.data(), 4);
        std::vector<std::uint32_t> tags;
        for (const auto& t : known_tags())
            if ((t.tag & 0xFFFF) == channels) tags.push_back(t.tag);
        out.resize(tags.size() * 4);
        if (!tags.empty()) std::memcpy(out.data(), tags.data(), out.size());
        return 0;
    }
    default:
        return error::unsupported_property;
    }
}

void Formats::install() {
    auto self = shared_from_this();
    const auto status = [](CpuState& s, std::int32_t v) { s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(v)); };
    // AudioFormatGetPropertyInfo(id, specifierSize, specifier, UInt32 *outPropertyDataSize)
    support_.function(audio_toolbox, "_AudioFormatGetPropertyInfo", [self, status](CpuState& s, GuestMemory& m) {
        std::vector<std::uint8_t> out;
        std::string text;
        const auto result = self->value(m, static_cast<std::uint32_t>(s.x[0]), static_cast<std::uint32_t>(s.x[1]), s.x[2], out, &text);
        if (result) return status(s, result);
        if (s.x[3]) m.write32(s.x[3], static_cast<std::uint32_t>(out.size()));
        status(s, 0);
    });
    // AudioFormatGetProperty(id, specifierSize, specifier, UInt32 *ioPropertyDataSize, void *outPropertyData)
    support_.function(audio_toolbox, "_AudioFormatGetProperty", [self, status](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) return status(s, error::param);
        std::vector<std::uint8_t> out;
        std::string text;
        const auto id = static_cast<std::uint32_t>(s.x[0]);
        const auto result = self->value(m, id, static_cast<std::uint32_t>(s.x[1]), s.x[2], out, &text);
        if (result) return status(s, result);
        const auto room = m.read32(s.x[3]);
        const bool list = id == four("tagc") || id == four("acof") || id == four("acif");
        if (room < out.size() && !list) return status(s, error::bad_property_size);
        const auto size = std::min<std::size_t>(room, out.size()) / (list ? 4 : 1) * (list ? 4 : 1);
        if (size && !s.x[4]) return status(s, error::param);
        if (id == four("fnam")) {
            m.write64(s.x[4], self->support_.cf.string(text));
        } else if (size) {
            m.write_bytes(s.x[4], std::span(out.data(), size));
        }
        m.write32(s.x[3], static_cast<std::uint32_t>(size));
        status(s, 0);
    });
}

} // namespace

void install_audio_format(Support& support) { std::make_shared<Formats>(support)->install(); }

} // namespace alchemy::gui::completion_kit::media
