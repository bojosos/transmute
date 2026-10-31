#include "alchemy/darwin/media/avaudio_sampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <numbers>

namespace alchemy::gui::completion_kit::media::avaudio {
namespace {

constexpr std::size_t maximum_voices = 64;

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | (static_cast<std::uint32_t>(b[at + 1]) << 8) | (static_cast<std::uint32_t>(b[at + 2]) << 16) |
           (static_cast<std::uint32_t>(b[at + 3]) << 24);
}
std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8)); }

// RIFF chunks of a LIST (or the file): id -> body.
std::multimap<std::string, std::span<const std::uint8_t>> chunks(std::span<const std::uint8_t> body) {
    std::multimap<std::string, std::span<const std::uint8_t>> out;
    for (std::size_t at = 0; at + 8 <= body.size();) {
        const std::string id(reinterpret_cast<const char*>(body.data() + at), 4);
        const auto size = le32(body, at + 4);
        if (at + 8 + size > body.size()) break;
        auto data = body.subspan(at + 8, size);
        if (id == "LIST" && size >= 4) out.emplace("LIST:" + std::string(reinterpret_cast<const char*>(data.data()), 4), data.subspan(4));
        else out.emplace(id, data);
        at += 8 + size + (size & 1);
    }
    return out;
}

// SoundFont 2 generator operators (the 2.04 specification, section 8.1.2).
enum Gen : std::uint16_t {
    start_offset = 0, end_offset = 1, loop_start_offset = 2, loop_end_offset = 3, start_coarse = 4, pan = 17,
    env_delay = 33, env_attack = 34, env_hold = 35, env_decay = 36, env_sustain = 37, env_release = 38, instrument = 41,
    key_range = 43, velocity_range = 44, end_coarse = 12, attenuation = 48, loop_start_coarse = 45, loop_end_coarse = 50,
    coarse_tune = 51, fine_tune = 52, sample_id = 53, sample_modes = 54, scale_tuning = 56, root_key = 58,
};

struct Generators {
    std::map<std::uint16_t, std::int16_t> values;
    std::array<std::uint8_t, 2> keys{0, 127}, velocities{0, 127};
    bool has(std::uint16_t op) const { return values.contains(op); }
    int get(std::uint16_t op, int fallback) const {
        const auto found = values.find(op);
        return found != values.end() ? found->second : fallback;
    }
};

// The generators of bag `b` (one zone): bags are (gen index, mod index) pairs.
Generators zone_generators(std::span<const std::uint8_t> bags, std::span<const std::uint8_t> gens, std::size_t b) {
    Generators g;
    const auto first = le16(bags, b * 4), last = le16(bags, (b + 1) * 4);
    for (std::size_t i = first; i < last && (i + 1) * 4 <= gens.size(); ++i) {
        const auto op = le16(gens, i * 4);
        if (op == key_range) g.keys = {gens[i * 4 + 2], gens[i * 4 + 3]};
        else if (op == velocity_range) g.velocities = {gens[i * 4 + 2], gens[i * 4 + 3]};
        else g.values[op] = static_cast<std::int16_t>(le16(gens, i * 4 + 2));
    }
    return g;
}

double timecents(int value) { return std::pow(2.0, value / 1200.0); }

} // namespace

std::optional<Instrument> load_sound_font(std::span<const std::uint8_t> file, int bank, int program, std::string& why) {
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "sfbk", 4) != 0) {
        why = "not a SoundFont 2 file";
        return std::nullopt;
    }
    const auto top = chunks(file.subspan(12, std::min<std::size_t>(le32(file, 4) - 4, file.size() - 12)));
    const auto sdta = top.find("LIST:sdta"), pdta = top.find("LIST:pdta");
    if (sdta == top.end() || pdta == top.end()) { why = "a SoundFont without sample or preset data"; return std::nullopt; }
    const auto samples_chunk = chunks(sdta->second), presets = chunks(pdta->second);
    const auto smpl = samples_chunk.find("smpl");
    const auto get = [&](const char* id) { const auto found = presets.find(id); return found != presets.end() ? found->second : std::span<const std::uint8_t>{}; };
    const auto phdr = get("phdr"), pbag = get("pbag"), pgen = get("pgen"), inst = get("inst"), ibag = get("ibag"), igen = get("igen"), shdr = get("shdr");
    if (smpl == samples_chunk.end() || phdr.size() < 76 || inst.size() < 44 || shdr.size() < 92) { why = "an incomplete SoundFont"; return std::nullopt; }
    // All samples as floats, shared by the zones.
    auto all = std::make_shared<std::vector<float>>(smpl->second.size() / 2);
    for (std::size_t i = 0; i < all->size(); ++i) (*all)[i] = static_cast<std::int16_t>(le16(smpl->second, i * 2)) / 32768.0f;
    const std::shared_ptr<const std::vector<float>> data = all;

    const auto presets_count = phdr.size() / 38 - 1;   // the last is the terminal record
    std::optional<std::size_t> chosen;
    for (std::size_t p = 0; p < presets_count; ++p)
        if (le16(phdr, p * 38 + 20) == program && le16(phdr, p * 38 + 22) == bank) { chosen = p; break; }
    if (!chosen) { why = "no preset " + std::to_string(program) + " in bank " + std::to_string(bank); return std::nullopt; }

    Instrument out;
    const auto preset_first = le16(phdr, *chosen * 38 + 24), preset_last = le16(phdr, (*chosen + 1) * 38 + 24);
    Generators preset_global;
    for (std::size_t b = preset_first; b < preset_last; ++b) {
        const auto pz = zone_generators(pbag, pgen, b);
        if (!pz.has(instrument)) {
            if (b == preset_first) preset_global = pz;
            continue;
        }
        // Preset zone values: the zone's over the preset's global zone; added to the instrument's.
        Generators pzone = preset_global;
        for (const auto& [op, v] : pz.values) pzone.values[op] = v;
        if (pz.keys != std::array<std::uint8_t, 2>{0, 127}) pzone.keys = pz.keys;
        if (pz.velocities != std::array<std::uint8_t, 2>{0, 127}) pzone.velocities = pz.velocities;
        const auto i = static_cast<std::size_t>(pz.get(instrument, 0));
        if ((i + 1) * 22 + 22 > inst.size()) continue;
        const auto inst_first = le16(inst, i * 22 + 20), inst_last = le16(inst, (i + 1) * 22 + 20);
        Generators inst_global;
        for (std::size_t ib = inst_first; ib < inst_last; ++ib) {
            const auto iz = zone_generators(ibag, igen, ib);
            if (!iz.has(sample_id)) {
                if (ib == inst_first) inst_global = iz;
                continue;
            }
            Generators g = inst_global;
            for (const auto& [op, v] : iz.values) g.values[op] = v;
            if (iz.keys != std::array<std::uint8_t, 2>{0, 127}) g.keys = iz.keys;
            if (iz.velocities != std::array<std::uint8_t, 2>{0, 127}) g.velocities = iz.velocities;
            const auto s = static_cast<std::size_t>(g.get(sample_id, 0));
            if ((s + 1) * 46 > shdr.size()) continue;
            const auto sh = s * 46;
            Zone z;
            z.data = data;
            z.key_low = std::max(g.keys[0], pzone.keys[0]);
            z.key_high = std::min(g.keys[1], pzone.keys[1]);
            z.velocity_low = std::max(g.velocities[0], pzone.velocities[0]);
            z.velocity_high = std::min(g.velocities[1], pzone.velocities[1]);
            if (z.key_low > z.key_high || z.velocity_low > z.velocity_high) continue;
            const auto additive = [&](std::uint16_t op, int fallback) { return g.get(op, fallback) + pzone.get(op, 0); };
            const auto offset = [&](std::uint16_t fine, std::uint16_t coarse) { return static_cast<std::int64_t>(g.get(fine, 0)) + 32768LL * g.get(coarse, 0); };
            const auto clamp_at = [&](std::int64_t v) { return static_cast<std::size_t>(std::clamp<std::int64_t>(v, 0, static_cast<std::int64_t>(data->size()))); };
            z.start = clamp_at(le32(shdr, sh + 20) + offset(start_offset, start_coarse));
            z.end = clamp_at(le32(shdr, sh + 24) + offset(end_offset, end_coarse));
            z.loop_start = clamp_at(le32(shdr, sh + 28) + offset(loop_start_offset, loop_start_coarse));
            z.loop_end = clamp_at(le32(shdr, sh + 32) + offset(loop_end_offset, loop_end_coarse));
            if (z.end <= z.start) continue;
            z.sample_rate = std::max<std::uint32_t>(le32(shdr, sh + 36), 1);
            const auto original = shdr[sh + 40];
            const auto correction = static_cast<std::int8_t>(shdr[sh + 41]);
            z.root_key = g.get(root_key, -1) >= 0 ? g.get(root_key, 60) : original;
            z.tune = additive(coarse_tune, 0) * 100.0 + additive(fine_tune, 0) + correction;
            z.scale_tuning = g.get(scale_tuning, 100);
            z.loop_mode = g.get(sample_modes, 0) & 3;
            if (z.loop_mode == 2) z.loop_mode = 0;   // reserved: no loop
            if (z.loop_end <= z.loop_start) z.loop_mode = 0;
            z.attenuation = std::max(0, additive(attenuation, 0)) / 10.0;
            z.pan = std::clamp(additive(pan, 0) / 500.0, -1.0, 1.0);
            z.delay = timecents(additive(env_delay, -12000));
            z.attack = timecents(additive(env_attack, -12000));
            z.hold = timecents(additive(env_hold, -12000));
            z.decay = timecents(additive(env_decay, -12000));
            z.sustain = std::pow(10.0, -std::clamp(additive(env_sustain, 0), 0, 1440) / 200.0);
            z.release = timecents(additive(env_release, -12000));
            out.zones.push_back(std::move(z));
        }
    }
    if (out.zones.empty()) { why = "the preset has no playable zone"; return std::nullopt; }
    return out;
}

Instrument from_samples(std::vector<std::pair<std::vector<float>, double>> files) {
    Instrument out;
    const bool spread = files.size() == 1;
    for (std::size_t i = 0; i < files.size() && i < 68; ++i) {
        Zone z;
        auto& [samples, rate] = files[i];
        z.end = samples.size();
        z.sample_rate = rate;
        z.data = std::make_shared<const std::vector<float>>(std::move(samples));
        z.root_key = 60 + static_cast<int>(i);
        z.key_low = spread ? 0 : static_cast<std::uint8_t>(z.root_key);
        z.key_high = spread ? 127 : static_cast<std::uint8_t>(z.root_key);
        out.zones.push_back(std::move(z));
    }
    return out;
}

void Sampler::set_instrument(Instrument instrument) {
    voices_.clear();
    instrument_ = std::move(instrument);
}

void Sampler::note_on(int channel, int key, int velocity) {
    if (velocity <= 0) return note_off(channel, key);
    channel &= 15;
    for (const auto& z : instrument_.zones) {
        if (key < z.key_low || key > z.key_high || velocity < z.velocity_low || velocity > z.velocity_high) continue;
        if (voices_.size() >= maximum_voices) voices_.erase(voices_.begin());   // the oldest goes
        Voice v;
        v.zone = &z;
        v.channel = channel;
        v.key = key;
        const double level = velocity / 127.0;
        v.velocity_gain = level * level;
        v.position = static_cast<double>(z.start);
        voices_.push_back(v);
    }
}

void Sampler::note_off(int channel, int key) {
    channel &= 15;
    for (auto& v : voices_)
        if (v.channel == channel && v.key == key && v.release_time < 0) {
            if (channels_[channel].sustain) v.sustained = true;
            else {
                v.level_at_release = envelope(v);
                v.release_time = 0;
            }
        }
}

void Sampler::controller(int channel, int number, int value) {
    auto& c = channels_[channel & 15];
    switch (number) {
    case 7: c.volume = value / 127.0; break;
    case 10: c.pan = (value - 64) / 64.0; break;
    case 11: c.expression = value / 127.0; break;
    case 64:
        c.sustain = value >= 64;
        if (!c.sustain)
            for (auto& v : voices_)
                if (v.channel == (channel & 15) && v.sustained) {
                    v.sustained = false;
                    v.level_at_release = envelope(v);
                    v.release_time = 0;
                }
        break;
    case 120: std::erase_if(voices_, [&](const Voice& v) { return v.channel == (channel & 15); }); break;
    case 123:
        for (auto& v : voices_)
            if (v.channel == (channel & 15) && v.release_time < 0) {
                v.level_at_release = envelope(v);
                v.release_time = 0;
            }
        break;
    default: break;
    }
}

void Sampler::pitch_bend(int channel, int value) { channels_[channel & 15].bend = (std::clamp(value, 0, 16383) - 8192) / 8192.0 * 2.0; }

void Sampler::all_off() { voices_.clear(); }

// The volume envelope's level now (delay, linear attack, hold, decay in dB to sustain; release
// from the level at release to silence, in dB).
double Sampler::envelope(Voice& v) const {
    const auto& z = *v.zone;
    double level{};
    double t = v.time;
    if (t < z.delay) level = 0;
    else if ((t -= z.delay) < z.attack) level = t / z.attack;
    else if ((t -= z.attack) < z.hold) level = 1;
    else if ((t -= z.hold) < z.decay) level = std::pow(10.0, std::log10(std::max(z.sustain, 1e-5)) * t / z.decay);
    else level = z.sustain;
    if (v.release_time >= 0) {
        if (v.release_time >= z.release) { v.done = true; return 0; }
        level = v.level_at_release * std::pow(10.0, -5.0 * v.release_time / z.release);   // -100 dB over the release
    }
    return level;
}

void Sampler::render(double* out, std::uint32_t frames, std::uint32_t channels, double rate) {
    const double master = std::pow(10.0, master_gain / 20.0);
    for (auto& v : voices_) {
        const auto& z = *v.zone;
        const auto& c = channels_[v.channel];
        const double semitones = (v.key - z.root_key) * z.scale_tuning / 100.0 + (z.tune + global_tuning) / 100.0 + c.bend;
        const double step = std::pow(2.0, semitones / 12.0) * z.sample_rate / rate;
        const double p = std::clamp(z.pan + c.pan + stereo_pan, -1.0, 1.0);
        const double theta = (p + 1) * std::numbers::pi / 4;
        const double gain = master * v.velocity_gain * c.volume * c.expression * std::pow(10.0, -z.attenuation / 20.0) * std::numbers::sqrt2;
        const double left = std::cos(theta) * gain, right = std::sin(theta) * gain;
        const auto& samples = *z.data;
        for (std::uint32_t f = 0; f < frames && !v.done; ++f) {
            const bool looping = z.loop_mode == 1 || (z.loop_mode == 3 && v.release_time < 0);
            if (looping && v.position >= static_cast<double>(z.loop_end)) v.position -= static_cast<double>(z.loop_end - z.loop_start);
            if (v.position >= static_cast<double>(z.end) - 1) { v.done = true; break; }
            const auto i = static_cast<std::size_t>(v.position);
            const double frac = v.position - static_cast<double>(i);
            const double next = i + 1 < samples.size() ? samples[i + 1] : samples[i];
            const double s = (samples[i] + (next - samples[i]) * frac) * envelope(v);
            if (channels == 1) out[f] += s * (left + right) / 2;
            else {
                out[static_cast<std::size_t>(f) * channels] += s * left;
                out[static_cast<std::size_t>(f) * channels + 1] += s * right;
            }
            v.position += step;
            v.time += 1.0 / rate;
            if (v.release_time >= 0) v.release_time += 1.0 / rate;
        }
    }
    std::erase_if(voices_, [](const Voice& v) { return v.done; });
}

} // namespace alchemy::gui::completion_kit::media::avaudio
