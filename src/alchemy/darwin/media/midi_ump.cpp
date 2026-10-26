#include "alchemy/darwin/media/midi_ump.hpp"

#include <algorithm>

namespace alchemy::gui::completion_kit::media::midi::ump {
namespace {

// Data bytes after a status byte (0x80-0xFF; system exclusive excluded).
unsigned data_bytes(std::uint8_t status) {
    if (status < 0xf0) {
        const auto kind = status & 0xf0;
        return kind == 0xc0 || kind == 0xd0 ? 1 : 2;
    }
    switch (status) {
    case 0xf1: case 0xf3: return 1;
    case 0xf2: return 2;
    default: return 0;   // F6 and the real-time messages; F4, F5, F9, FD are undefined (no data)
    }
}

std::uint32_t word(std::uint8_t a, std::uint8_t b, std::uint8_t c, std::uint8_t d) {
    return (std::uint32_t(a) << 24) | (std::uint32_t(b) << 16) | (std::uint32_t(c) << 8) | d;
}

void channel_voice(std::vector<std::uint32_t>& out, std::uint8_t status, std::uint8_t d1, std::uint8_t d2, std::uint32_t protocol) {
    if (protocol != protocol_2_0) {
        out.push_back(word(0x20, status, d1, d2));
        return;
    }
    const auto channel = static_cast<std::uint8_t>(status & 0x0f);
    switch (status & 0xf0) {
    case 0x80:
        out.push_back(word(0x40, 0x80 | channel, d1, 0));
        out.push_back(scale(d2, 7, 16) << 16);
        break;
    case 0x90:
        if (d2 == 0) {   // a note on of velocity 0 is a note off (velocity 0x8000, the 7-bit 64)
            out.push_back(word(0x40, 0x80 | channel, d1, 0));
            out.push_back(0x8000U << 16);
        } else {
            out.push_back(word(0x40, 0x90 | channel, d1, 0));
            out.push_back(scale(d2, 7, 16) << 16);
        }
        break;
    case 0xa0:
        out.push_back(word(0x40, 0xa0 | channel, d1, 0));
        out.push_back(scale(d2, 7, 32));
        break;
    case 0xb0:
        out.push_back(word(0x40, 0xb0 | channel, d1, 0));
        out.push_back(scale(d2, 7, 32));
        break;
    case 0xc0:
        out.push_back(word(0x40, 0xc0 | channel, 0, 0));   // no bank
        out.push_back(std::uint32_t(d1) << 24);
        break;
    case 0xd0:
        out.push_back(word(0x40, 0xd0 | channel, 0, 0));
        out.push_back(scale(d1, 7, 32));
        break;
    default:   // 0xe0, pitch bend: 14 bits, LSB first
        out.push_back(word(0x40, 0xe0 | channel, 0, 0));
        out.push_back(scale(std::uint32_t(d1) | (std::uint32_t(d2) << 7), 14, 32));
        break;
    }
}

// System exclusive payload (without F0/F7) as 7-bit data packets.
void sysex(std::vector<std::uint32_t>& out, std::span<const std::uint8_t> payload) {
    if (payload.empty()) {
        out.push_back(word(0x30, 0x00, 0, 0));   // complete, no bytes
        out.push_back(0);
        return;
    }
    for (std::size_t at = 0; at < payload.size(); at += 6) {
        const auto count = std::min<std::size_t>(6, payload.size() - at);
        const bool first = at == 0, last = at + count == payload.size();
        const std::uint8_t kind = first && last ? 0 : first ? 1 : last ? 3 : 2;
        std::uint8_t bytes[6]{};
        std::copy_n(payload.begin() + static_cast<std::ptrdiff_t>(at), count, bytes);
        out.push_back(word(0x30, static_cast<std::uint8_t>((kind << 4) | count), bytes[0], bytes[1]));
        out.push_back(word(bytes[2], bytes[3], bytes[4], bytes[5]));
    }
}

} // namespace

std::size_t word_count(std::uint32_t first) {
    switch (first >> 28) {
    case 0x0: case 0x1: case 0x2: case 0x6: case 0x7: return 1;
    case 0x3: case 0x4: case 0x8: case 0x9: case 0xa: return 2;
    case 0xb: case 0xc: return 3;
    default: return 4;   // 0x5, 0xd, 0xe, 0xf
    }
}

std::uint32_t scale(std::uint32_t value, unsigned from_bits, unsigned to_bits) {
    if (from_bits >= to_bits) return value >> (from_bits - to_bits);
    const unsigned shift = to_bits - from_bits;
    std::uint64_t shifted = std::uint64_t(value) << shift;
    const std::uint32_t center = 1U << (from_bits - 1);
    if (value <= center) return static_cast<std::uint32_t>(shifted);
    // Above the center, the bits below the top one repeat down the new low bits.
    const unsigned repeat_bits = from_bits - 1;
    std::uint64_t repeat = value & ((1U << repeat_bits) - 1);
    repeat = shift > repeat_bits ? repeat << (shift - repeat_bits) : repeat >> (repeat_bits - shift);
    while (repeat) {
        shifted |= repeat;
        repeat >>= repeat_bits;
    }
    return static_cast<std::uint32_t>(shifted);
}

std::vector<std::uint32_t> from_bytes(std::span<const std::uint8_t> bytes, std::uint32_t protocol) {
    std::vector<std::uint32_t> out;
    std::uint8_t running = 0;
    for (std::size_t i = 0; i < bytes.size();) {
        auto status = bytes[i];
        if (status >= 0xf8) {   // real time, anywhere
            out.push_back(word(0x10, status, 0, 0));
            ++i;
            continue;
        }
        if (status == 0xf0) {
            std::vector<std::uint8_t> payload;
            std::size_t j = i + 1;
            for (; j < bytes.size() && bytes[j] != 0xf7; ++j) {
                if (bytes[j] >= 0xf8) out.push_back(word(0x10, bytes[j], 0, 0));
                else if (bytes[j] < 0x80) payload.push_back(bytes[j]);
                else break;   // another status ends it
            }
            sysex(out, payload);
            i = j < bytes.size() && bytes[j] == 0xf7 ? j + 1 : j;
            running = 0;
            continue;
        }
        if (status < 0x80) {   // running status
            if (!running) { ++i; continue; }
            status = running;
        } else {
            ++i;
            if (status < 0xf0) running = status;
            else running = 0;
        }
        const auto need = data_bytes(status);
        std::uint8_t data[2]{};
        unsigned have = 0;
        while (have < need && i < bytes.size() && bytes[i] < 0x80) data[have++] = bytes[i++];
        if (have < need) continue;   // truncated
        if (status < 0xf0) channel_voice(out, status, data[0], data[1], protocol);
        else if (status != 0xf7) out.push_back(word(0x10, status, data[0], data[1]));
    }
    return out;
}

std::vector<std::uint8_t> to_bytes(std::span<const std::uint32_t> words) {
    std::vector<std::uint8_t> out;
    for (std::size_t i = 0; i < words.size();) {
        const auto first = words[i];
        const auto count = word_count(first);
        if (i + count > words.size()) break;
        const auto type = first >> 28;
        const auto b1 = static_cast<std::uint8_t>(first >> 16), b2 = static_cast<std::uint8_t>(first >> 8), b3 = static_cast<std::uint8_t>(first);
        if (type == 0x1) {
            out.push_back(b1);
            for (unsigned k = 0; k < data_bytes(b1); ++k) out.push_back(k == 0 ? b2 : b3);
        } else if (type == 0x2) {
            out.push_back(b1);
            out.push_back(b2 & 0x7f);
            if (data_bytes(b1) == 2) out.push_back(b3 & 0x7f);
        } else if (type == 0x3) {
            const auto kind = b1 >> 4;
            const auto n = std::min<unsigned>(b1 & 0x0f, 6);
            const std::uint8_t payload[6]{b2, b3, static_cast<std::uint8_t>(words[i + 1] >> 24), static_cast<std::uint8_t>(words[i + 1] >> 16),
                                          static_cast<std::uint8_t>(words[i + 1] >> 8), static_cast<std::uint8_t>(words[i + 1])};
            if (kind == 0 || kind == 1) out.push_back(0xf0);
            for (unsigned k = 0; k < n; ++k) out.push_back(payload[k] & 0x7f);
            if (kind == 0 || kind == 3) out.push_back(0xf7);
        } else if (type == 0x4) {
            const auto opcode = b1 & 0xf0, channel = b1 & 0x0f;
            const auto data = words[i + 1];
            switch (opcode) {
            case 0x80: case 0x90: {
                auto velocity = static_cast<std::uint8_t>(scale(data >> 16, 16, 7));
                if (opcode == 0x90 && velocity == 0) velocity = 1;   // a 7-bit velocity of 0 would end the note
                out.insert(out.end(), {static_cast<std::uint8_t>(opcode | channel), static_cast<std::uint8_t>(b2 & 0x7f), velocity});
                break;
            }
            case 0xa0: case 0xb0:
                out.insert(out.end(), {static_cast<std::uint8_t>(opcode | channel), static_cast<std::uint8_t>(b2 & 0x7f),
                                       static_cast<std::uint8_t>(scale(data, 32, 7))});
                break;
            case 0xc0:
                if (b3 & 1) {   // bank valid: bank select MSB and LSB first
                    out.insert(out.end(), {static_cast<std::uint8_t>(0xb0 | channel), 0, static_cast<std::uint8_t>((data >> 8) & 0x7f)});
                    out.insert(out.end(), {static_cast<std::uint8_t>(0xb0 | channel), 32, static_cast<std::uint8_t>(data & 0x7f)});
                }
                out.insert(out.end(), {static_cast<std::uint8_t>(0xc0 | channel), static_cast<std::uint8_t>((data >> 24) & 0x7f)});
                break;
            case 0xd0:
                out.insert(out.end(), {static_cast<std::uint8_t>(0xd0 | channel), static_cast<std::uint8_t>(scale(data, 32, 7))});
                break;
            case 0xe0: {
                const auto bend = scale(data, 32, 14);
                out.insert(out.end(), {static_cast<std::uint8_t>(0xe0 | channel), static_cast<std::uint8_t>(bend & 0x7f),
                                       static_cast<std::uint8_t>((bend >> 7) & 0x7f)});
                break;
            }
            default: break;   // per-note and registered/assignable controllers: no MIDI 1.0 form
            }
        }
        i += count;
    }
    return out;
}

} // namespace alchemy::gui::completion_kit::media::midi::ump
