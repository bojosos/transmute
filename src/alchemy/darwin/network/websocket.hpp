#pragma once

// The WebSocket protocol's framing and opening-handshake key (RFC 6455), host
// only: no guest calls. Used by NSURLSessionWebSocketTask (loading.cpp).

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::network::websocket {

using Bytes = std::vector<std::uint8_t>;

enum class Opcode : std::uint8_t { continuation = 0, text = 1, binary = 2, close = 8, ping = 9, pong = 10 };

struct Frame {
    bool fin{true};
    Opcode opcode{Opcode::binary};
    bool masked{};
    Bytes payload;   // unmasked
};

// A malformed frame (reserved bits, unknown opcode, a fragmented or oversized control frame, a payload over the limit).
struct ProtocolError : std::runtime_error {
    ProtocolError(std::uint16_t close_code, const std::string& what) : std::runtime_error(what), code(close_code) {}
    std::uint16_t code;   // the close code to answer with (1002 protocol error, 1009 too big)
};

// One frame; client frames are masked with `mask` (RFC 6455 5.3), server frames are not (no mask).
[[nodiscard]] Bytes encode(const Frame& frame, const std::optional<std::array<std::uint8_t, 4>>& mask);

// Frames from a byte stream, in order.
class Decoder {
public:
    void feed(const std::uint8_t* data, std::size_t size);
    // The next complete frame, or nullopt when more bytes are needed. Throws ProtocolError.
    std::optional<Frame> next(std::uint64_t max_payload);
    [[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - offset_; }

private:
    Bytes buffer_;
    std::size_t offset_{};
};

// Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA-1(key + the RFC 6455 GUID)); empty when the host has no SHA-1.
[[nodiscard]] std::string accept_for(std::string_view key);

// A close frame's payload: the code (network order) and the UTF-8 reason.
[[nodiscard]] Bytes close_payload(std::uint16_t code, std::string_view reason);

} // namespace alchemy::gui::network::websocket
