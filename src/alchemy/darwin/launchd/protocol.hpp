#pragma once

// The session launchd's request protocol (private to src/alchemy/darwin/launchd/).
// One host::ipc channel per client: a hello, then requests, each answered in
// order. Frames are a 32-bit little-endian length and the body.

#include "alchemy/host/host.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::launchd::wire {

enum class Op : std::uint32_t {
    hello = 1,       // u32 caller pid, u32 tree root pid, u32 tree root pidversion
    load = 2,        // text plist XML, text guest path ("" for a submitted job), u8 flags (load_*) -> status
    remove = 3,      // text label -> status
    list = 4,        // -> u32 count, then (i64 pid or -1, i32 last status, text label)
    info = 5,        // text label -> status, text dictionary XML, text state XML
    kickstart = 6,   // text label, u8 kill first -> status, u32 pid
    signal = 7,      // text label, u32 signal -> status
    enable = 8,      // text label, u8 enabled -> status
    overrides = 9,   // -> u32 count, then (text label, u8 disabled)
    setenv = 10,     // text name, text value
    unsetenv = 11,   // text name
    getenv = 12,     // text name -> status, text value
    start = 13,      // text label -> status
    stop = 14,       // text label -> status
};

inline constexpr std::uint8_t load_submit = 1;       // SMJobSubmit / launchctl submit: no on-disk path
inline constexpr std::uint8_t load_write = 2;        // launchctl load -w: clear a disable override
inline constexpr std::uint8_t load_force = 4;        // launchctl load -F: ignore Disabled and overrides
inline constexpr std::uint8_t load_legacy = 8;       // launchctl load (not bootstrap): legacy messages

// launchctl's own status values (beside errno values).
inline constexpr std::int32_t status_no_such_service = 113;   // "Could not find specified service"
inline constexpr std::int32_t status_service_disabled = 119;  // "Service is disabled"
inline constexpr std::int32_t status_already_loaded = 37;     // EALREADY: "Operation already in progress"
inline constexpr std::int32_t status_invalid_plist = 22;      // EINVAL
inline constexpr std::int32_t status_not_permitted = 1;       // EPERM (the system domain)
inline constexpr std::int32_t status_unsupported = 45;        // ENOTSUP
inline constexpr std::int32_t status_io = 5;                  // EIO

struct WireError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Writer {
public:
    Writer& u8(std::uint8_t value) { bytes.push_back(value); return *this; }
    Writer& u32(std::uint32_t value) { put(&value, 4); return *this; }
    Writer& i32(std::int32_t value) { put(&value, 4); return *this; }
    Writer& i64(std::int64_t value) { put(&value, 8); return *this; }
    Writer& text(std::string_view value) { u32(static_cast<std::uint32_t>(value.size())); put(value.data(), value.size()); return *this; }
    std::vector<std::uint8_t> bytes;
private:
    void put(const void* data, std::size_t size) {
        const auto* p = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
};

class Reader {
public:
    explicit Reader(std::vector<std::uint8_t> bytes) : bytes_(std::move(bytes)) {}
    std::uint8_t u8() { std::uint8_t v; take(&v, 1); return v; }
    std::uint32_t u32() { std::uint32_t v; take(&v, 4); return v; }
    std::int32_t i32() { std::int32_t v; take(&v, 4); return v; }
    std::int64_t i64() { std::int64_t v; take(&v, 8); return v; }
    std::string text() {
        const auto size = u32();
        if (size > bytes_.size() - at_) throw WireError("launchd: truncated message");
        std::string value(reinterpret_cast<const char*>(bytes_.data() + at_), size);
        at_ += size;
        return value;
    }
private:
    void take(void* out, std::size_t size) {
        if (size > bytes_.size() - at_) throw WireError("launchd: truncated message");
        std::memcpy(out, bytes_.data() + at_, size);
        at_ += size;
    }
    std::vector<std::uint8_t> bytes_;
    std::size_t at_{};
};

// Frames over a host::ipc channel. receive returns nullopt when the peer closed it.
bool send(host::Handle channel, const std::vector<std::uint8_t>& body);
std::optional<std::vector<std::uint8_t>> receive(host::Handle channel);

} // namespace alchemy::launchd::wire
