#include "alchemy/darwin/media/hal_core.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace alchemy::gui::completion_kit::media::hal {
namespace {

constexpr std::uint32_t first_device = 0x40, stride = 2;   // device n: 0x40 + 2n, its stream: + 1

void put32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
void put_real(std::vector<std::uint8_t>& out, double value) { put64(out, std::bit_cast<std::uint64_t>(value)); }
void put_float(std::vector<std::uint8_t>& out, float value) { put32(out, std::bit_cast<std::uint32_t>(value)); }

Value u32(std::uint32_t value) { Value v; put32(v.bytes, value); return v; }
Value array32(const std::vector<std::uint32_t>& values) {
    Value v;
    for (const auto value : values) put32(v.bytes, value);
    v.element_size = 4;
    return v;
}
Value text(const std::string& value) { Value v; v.string = value; return v; }
Value range(double low, double high) {
    Value v;
    put_real(v.bytes, low);
    put_real(v.bytes, high);
    v.element_size = 16;
    return v;
}

// kAudioChannelLabel_* for the WAVEFORMATEXTENSIBLE speaker bits, in bit order.
constexpr std::array<std::uint32_t, 11> speaker_labels{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

} // namespace

std::vector<std::uint8_t> float_format(double sample_rate, std::uint32_t channels) {
    std::vector<std::uint8_t> out;
    put_real(out, sample_rate);
    put32(out, four("lpcm"));
    put32(out, 1U | 8U);          // kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
    put32(out, 4 * channels);     // mBytesPerPacket
    put32(out, 1);                // mFramesPerPacket
    put32(out, 4 * channels);     // mBytesPerFrame
    put32(out, channels);
    put32(out, 32);               // mBitsPerChannel
    put32(out, 0);                // mReserved
    return out;
}

Model::Model(Snapshot snapshot) : snapshot_(std::move(snapshot)) {
    for (std::size_t i = 0; i < snapshot_.devices.size(); ++i) {
        Entry entry;
        entry.id = first_device + static_cast<std::uint32_t>(i) * stride;
        entry.stream = entry.id + 1;
        entry.device = snapshot_.devices[i];
        if (entry.device.channels == 0) entry.device.channels = 2;
        if (!(entry.device.sample_rate > 0)) entry.device.sample_rate = 48000;
        entries_.push_back(entry);
        device_ids_.push_back(entry.id);
    }
}

const Model::Entry* Model::entry_for_device(std::uint32_t object) const {
    for (const auto& entry : entries_) if (entry.id == object) return &entry;
    return nullptr;
}
const Model::Entry* Model::entry_for_stream(std::uint32_t object) const {
    for (const auto& entry : entries_) if (entry.stream == object) return &entry;
    return nullptr;
}

std::optional<Kind> Model::kind(std::uint32_t object) const {
    if (object == system_object) return Kind::system;
    if (entry_for_device(object)) return Kind::device;
    if (entry_for_stream(object)) return Kind::stream;
    return std::nullopt;
}

std::optional<std::uint32_t> Model::device_for_uid(const std::string& uid) const {
    for (const auto& entry : entries_) if (entry.device.uid == uid) return entry.id;
    return std::nullopt;
}

const Device* Model::device(std::uint32_t object) const {
    const auto* entry = entry_for_device(object);
    return entry ? &entry->device : nullptr;
}

std::int32_t Model::system_get(const Address& address, const std::optional<std::string>& qualifier, Value& out) const {
    const auto default_of = [this](const std::string& uid) { return uid.empty() ? 0U : device_for_uid(uid).value_or(0U); };
    switch (address.selector) {
    case four("dev#"): case four("ownd"): out = array32(device_ids_); return 0;
    case four("dIn "): out = u32(default_of(snapshot_.default_input)); return 0;
    case four("dOut"): case four("sOut"): out = u32(default_of(snapshot_.default_output)); return 0;
    case four("uidd"):   // kAudioHardwarePropertyTranslateUIDToDevice: qualifier = CFStringRef UID
        if (!qualifier) return error::bad_size;
        out = u32(device_for_uid(*qualifier).value_or(0U));
        return 0;
    case four("plg#"): case four("box#"): case four("clk#"): case four("tps#"): out = array32({}); return 0;
    case four("clas"): out = u32(four("asys")); return 0;
    case four("bcls"): out = u32(four("aobj")); return 0;
    case four("stdv"): out = u32(0); return 0;
    case four("rnlp"): { Value v; put64(v.bytes, run_loop_); out = v; return 0; }
    case four("hwbz"): out = u32(0); return 0;                            // not initing or exiting
    case four("slep"): out = u32(sleeping_allowed_ ? 1 : 0); return 0;    // kAudioHardwarePropertySleepingIsAllowed
    default: return error::unknown_property;
    }
}

std::int32_t Model::device_get(const Entry& entry, const Address& address, Value& out) const {
    const auto& d = entry.device;
    const auto direction = d.input ? scope_input : scope_output;
    const bool in_scope = address.scope == direction || address.scope == scope_global;
    switch (address.selector) {
    case four("clas"): out = u32(four("adev")); return 0;
    case four("bcls"): out = u32(four("aobj")); return 0;
    case four("stdv"): out = u32(system_object); return 0;
    case four("lnam"): out = text(d.name); return 0;
    case four("lmak"):
        if (d.manufacturer.empty()) return error::unknown_property;   // Windows endpoints rarely name one
        out = text(d.manufacturer);
        return 0;
    case four("uid "): out = text(d.uid); return 0;
    case four("muid"): out = text(d.name); return 0;
    case four("name"): {   // kAudioDevicePropertyDeviceName: a C string
        Value v;
        v.bytes.assign(d.name.begin(), d.name.end());
        v.bytes.push_back(0);
        v.element_size = 1;
        out = v;
        return 0;
    }
    case four("tran"): out = u32(d.form_factor == 9 ? four("hdmi") : 0U); return 0;   // DigitalAudioDisplayDevice
    case four("livn"): out = u32(1); return 0;
    case four("goin"): case four("gone"): out = u32(0); return 0;   // no I/O runs
    case four("hidn"): out = u32(0); return 0;
    case four("dflt"): out = u32(address.scope == direction ? 1 : 0); return 0;
    case four("sflt"): out = u32(!d.input && address.scope == scope_output ? 1 : 0); return 0;
    case four("nsrt"): { Value v; put_real(v.bytes, d.sample_rate); out = v; return 0; }
    case four("nsr#"): out = range(d.sample_rate, d.sample_rate); return 0;
    case four("fsiz"): out = u32(entry.buffer_frames); return 0;
    case four("fsz#"): out = range(32, 4096); return 0;
    case four("clkd"): out = u32(0); return 0;
    case four("akin"): out = array32({entry.id}); return 0;
    case four("oink"): out = u32(0xffffffffU); return 0;   // pid_t -1: nobody hogs the device
    case four("ownd"): out = array32({entry.stream}); return 0;
    case four("stm#"): out = array32(in_scope ? std::vector<std::uint32_t>{entry.stream} : std::vector<std::uint32_t>{}); return 0;
    case four("slay"): {   // AudioBufferList {UInt32 mNumberBuffers; AudioBuffer mBuffers[]}
        Value v;
        const bool present = address.scope == direction;
        put32(v.bytes, present ? 1 : 0);
        put32(v.bytes, 0);
        if (present) { put32(v.bytes, d.channels); put32(v.bytes, 0); put64(v.bytes, 0); }
        out = v;
        return 0;
    }
    case four("dch2"):   // kAudioDevicePropertyPreferredChannelsForStereo: UInt32[2]
        if (!in_scope) return error::unknown_property;
        { Value v; put32(v.bytes, 1); put32(v.bytes, d.channels > 1 ? 2 : 1); out = v; }
        return 0;
    case four("srnd"): {   // kAudioDevicePropertyPreferredChannelLayout: AudioChannelLayout with descriptions
        if (!in_scope) return error::unknown_property;
        Value v;
        put32(v.bytes, 0);   // kAudioChannelLayoutTag_UseChannelDescriptions
        put32(v.bytes, 0);   // mChannelBitmap
        put32(v.bytes, d.channels);
        std::vector<std::uint32_t> labels;
        for (std::size_t bit = 0; bit < speaker_labels.size() && labels.size() < d.channels; ++bit)
            if (d.channel_mask & (1U << bit)) labels.push_back(speaker_labels[bit]);
        if (labels.size() != d.channels) {
            labels.clear();
            if (d.channels == 2) labels = {1, 2};                          // Left, Right
            else for (std::uint32_t i = 0; i < d.channels; ++i) labels.push_back((1U << 16) | i);   // Discrete_i
        }
        for (const auto label : labels) {
            put32(v.bytes, label);
            put32(v.bytes, 0);   // mChannelFlags
            for (int i = 0; i < 3; ++i) put_float(v.bytes, 0);
        }
        out = v;
        return 0;
    }
    default: return error::unknown_property;
    }
}

std::int32_t Model::stream_get(const Entry& entry, const Address& address, Value& out) const {
    const auto& d = entry.device;
    switch (address.selector) {
    case four("clas"): out = u32(four("astr")); return 0;
    case four("bcls"): out = u32(four("aobj")); return 0;
    case four("stdv"): out = u32(entry.id); return 0;
    case four("sdir"): out = u32(d.input ? 1 : 0); return 0;
    case four("schn"): out = u32(1); return 0;
    case four("sact"): out = u32(1); return 0;
    case four("term"): {
        std::uint32_t terminal = 0;   // kAudioStreamTerminalTypeUnknown
        switch (d.form_factor) {
        case 1: terminal = four("spkr"); break;   // Speakers
        case 2: terminal = four("line"); break;   // LineLevel
        case 3: terminal = four("hdph"); break;   // Headphones
        case 4: terminal = four("micr"); break;   // Microphone
        case 5: terminal = four("hdst"); break;   // Headset
        case 8: terminal = four("spdf"); break;   // SPDIF
        case 9: terminal = four("hdmi"); break;   // DigitalAudioDisplayDevice
        default: break;
        }
        out = u32(terminal);
        return 0;
    }
    case four("sfmt"): case four("pft "): { Value v; v.bytes = float_format(d.sample_rate, d.channels); out = v; return 0; }
    case four("sfma"): case four("pfta"): {   // AudioStreamRangedDescription {ASBD; AudioValueRange}
        Value v;
        v.bytes = float_format(d.sample_rate, d.channels);
        put_real(v.bytes, d.sample_rate);
        put_real(v.bytes, d.sample_rate);
        v.element_size = 56;
        out = v;
        return 0;
    }
    default: return error::unknown_property;
    }
}

bool Model::has(std::uint32_t object, const Address& address) const {
    Value ignored;
    return get(object, address, std::string(), ignored) == 0;
}

std::int32_t Model::settable(std::uint32_t object, const Address& address, bool& out) const {
    Value ignored;
    if (const auto result = get(object, address, std::string(), ignored)) return result;
    const auto kind = this->kind(object);
    const auto s = address.selector;
    out = (kind == Kind::system && (s == four("rnlp") || s == four("slep"))) || (kind == Kind::device && s == four("fsiz"));
    return 0;
}

std::int32_t Model::get(std::uint32_t object, const Address& address, const std::optional<std::string>& qualifier, Value& out) const {
    if (object == system_object) return system_get(address, qualifier, out);
    if (const auto* entry = entry_for_device(object)) return device_get(*entry, address, out);
    if (const auto* entry = entry_for_stream(object)) return stream_get(*entry, address, out);
    return error::bad_object;
}

std::int32_t Model::set(std::uint32_t object, const Address& address, const std::vector<std::uint8_t>& data) {
    Value current;
    if (const auto result = get(object, address, std::string(), current)) return result;
    const auto s = address.selector;
    const auto read32 = [&] { std::uint32_t v{}; std::memcpy(&v, data.data(), 4); return v; };
    if (object == system_object) {
        if (s == four("dIn ") || s == four("dOut") || s == four("sOut")) {
            if (data.size() != 4) return error::bad_size;
            const auto wanted = read32();
            if (wanted == 0 || !entry_for_device(wanted)) return error::bad_device;
            // Windows' default endpoint is the user's choice; only the current one can be "set".
            return current.bytes == data ? 0 : error::unsupported;
        }
        if (s == four("rnlp")) {
            if (data.size() != 8) return error::bad_size;
            std::memcpy(&run_loop_, data.data(), 8);
            return 0;
        }
        if (s == four("slep")) {
            if (data.size() != 4) return error::bad_size;
            sleeping_allowed_ = read32() != 0;
            return 0;
        }
        return error::illegal_operation;
    }
    if (auto* entry = const_cast<Entry*>(entry_for_device(object))) {
        if (s == four("fsiz")) {
            if (data.size() != 4) return error::bad_size;
            const auto frames = read32();
            if (frames < 32 || frames > 4096) return error::illegal_operation;
            entry->buffer_frames = frames;
            return 0;
        }
        if (s == four("nsrt")) {
            if (data.size() != 8) return error::bad_size;
            return current.bytes == data ? 0 : error::unsupported;   // the shared-mode rate is Windows'
        }
        if (s == four("oink")) return error::unsupported;           // no exclusive (hog) mode
        return error::illegal_operation;
    }
    // Streams: only the current format can be "set".
    if (s == four("sfmt") || s == four("pft ")) return current.bytes == data ? 0 : error::unsupported;
    return error::illegal_operation;
}

} // namespace alchemy::gui::completion_kit::media::hal
