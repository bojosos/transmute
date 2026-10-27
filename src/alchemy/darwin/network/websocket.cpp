#include "alchemy/darwin/network/websocket.hpp"

#include "alchemy/darwin/security/bridge.hpp"
#include "alchemy/host/crypto.hpp"

#include <span>

namespace alchemy::gui::network::websocket {

Bytes encode(const Frame& frame, const std::optional<std::array<std::uint8_t, 4>>& mask) {
    Bytes out;
    out.reserve(frame.payload.size() + 14);
    out.push_back(static_cast<std::uint8_t>((frame.fin ? 0x80 : 0) | static_cast<std::uint8_t>(frame.opcode)));
    const auto size = frame.payload.size();
    const std::uint8_t mask_bit = mask ? 0x80 : 0;
    if (size < 126) {
        out.push_back(static_cast<std::uint8_t>(mask_bit | size));
    } else if (size <= 0xffff) {
        out.push_back(static_cast<std::uint8_t>(mask_bit | 126));
        out.push_back(static_cast<std::uint8_t>(size >> 8));
        out.push_back(static_cast<std::uint8_t>(size));
    } else {
        out.push_back(static_cast<std::uint8_t>(mask_bit | 127));
        for (int shift = 56; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(size) >> shift));
    }
    if (mask) out.insert(out.end(), mask->begin(), mask->end());
    const auto start = out.size();
    out.insert(out.end(), frame.payload.begin(), frame.payload.end());
    if (mask)
        for (std::size_t index = 0; index < size; ++index) out[start + index] ^= (*mask)[index & 3];
    return out;
}

void Decoder::feed(const std::uint8_t* data, std::size_t size) {
    if (offset_ > 0 && offset_ == buffer_.size()) { buffer_.clear(); offset_ = 0; }
    buffer_.insert(buffer_.end(), data, data + size);
}

std::optional<Frame> Decoder::next(std::uint64_t max_payload) {
    const auto available = buffer_.size() - offset_;
    if (available < 2) return std::nullopt;
    const auto* at = buffer_.data() + offset_;
    const auto first = at[0], second = at[1];
    if (first & 0x70) throw ProtocolError(1002, "a WebSocket frame used reserved bits");
    const auto opcode = static_cast<Opcode>(first & 0x0f);
    const bool control = (first & 0x08) != 0;
    switch (opcode) {
    case Opcode::continuation: case Opcode::text: case Opcode::binary: case Opcode::close: case Opcode::ping: case Opcode::pong: break;
    default: throw ProtocolError(1002, "an unknown WebSocket opcode");
    }
    const bool fin = (first & 0x80) != 0;
    if (control && !fin) throw ProtocolError(1002, "a fragmented WebSocket control frame");
    std::size_t header = 2;
    std::uint64_t length = second & 0x7f;
    if (length == 126) {
        if (available < 4) return std::nullopt;
        length = (static_cast<std::uint64_t>(at[2]) << 8) | at[3];
        header = 4;
    } else if (length == 127) {
        if (available < 10) return std::nullopt;
        length = 0;
        for (int index = 0; index < 8; ++index) length = (length << 8) | at[2 + index];
        header = 10;
    }
    if (control && length > 125) throw ProtocolError(1002, "a WebSocket control frame over 125 bytes");
    if (length > max_payload) throw ProtocolError(1009, "a WebSocket message over the maximum message size");
    const bool masked = (second & 0x80) != 0;
    const std::size_t mask_size = masked ? 4 : 0;
    if (available < header + mask_size + length) return std::nullopt;
    Frame frame;
    frame.fin = fin;
    frame.opcode = opcode;
    frame.masked = masked;
    const auto* payload = at + header + mask_size;
    frame.payload.assign(payload, payload + length);
    if (masked)
        for (std::size_t index = 0; index < frame.payload.size(); ++index) frame.payload[index] ^= at[header + (index & 3)];
    offset_ += header + mask_size + static_cast<std::size_t>(length);
    return frame;
}

std::string accept_for(std::string_view key) {
    std::string input(key);
    input += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest = host::crypto::hash(host::crypto::Digest::sha1, std::as_bytes(std::span(input.data(), input.size())));
    if (!digest) return {};
    return security::base64_encode(*digest);
}

Bytes close_payload(std::uint16_t code, std::string_view reason) {
    Bytes out{static_cast<std::uint8_t>(code >> 8), static_cast<std::uint8_t>(code)};
    out.insert(out.end(), reason.begin(), reason.end());
    return out;
}

} // namespace alchemy::gui::network::websocket
