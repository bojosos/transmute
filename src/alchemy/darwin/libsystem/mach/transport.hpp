#pragma once

// Mach IPC between Alchemy processes (docs/ALCHEMY_PROCESS.md, "Mach ports
// between processes"). Every Alchemy process that exports a port right runs
// one endpoint: a named pipe \\.\pipe\alchemy-mach-<pid> (pid = the Windows
// pid, which is the guest pid). A peer connects once and keeps the
// connection: one-way frames (messages, reference releases, death notices)
// are written in order by a writer thread; requests (reference grants,
// bootstrap, special ports) wait for their response. Peer death comes from a
// wait on the peer's process handle. All callbacks run on transport threads,
// never on guest threads: handlers must not call guest code.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace alchemy::libsystem::mach {

// Frame payload encoding (little-endian).
struct Writer {
    std::vector<std::uint8_t> bytes;
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) { append(&value, 4); }
    void u64(std::uint64_t value) { append(&value, 8); }
    void blob(std::span<const std::uint8_t> data) {
        u32(static_cast<std::uint32_t>(data.size()));
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
    void text(const std::string& value) { blob({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}); }
    void append(const void* data, std::size_t size) {
        const auto* first = static_cast<const std::uint8_t*>(data);
        bytes.insert(bytes.end(), first, first + size);
    }
};

struct Reader {
    std::span<const std::uint8_t> data;
    std::size_t at{};
    void need(std::size_t size) const {
        if (data.size() - at < size) throw std::runtime_error("Alchemy Mach transport: truncated frame");
    }
    std::uint8_t u8() { need(1); return data[at++]; }
    std::uint32_t u32() { need(4); std::uint32_t value{}; std::memcpy(&value, data.data() + at, 4); at += 4; return value; }
    std::uint64_t u64() { need(8); std::uint64_t value{}; std::memcpy(&value, data.data() + at, 8); at += 8; return value; }
    std::vector<std::uint8_t> blob() {
        const auto size = u32();
        need(size);
        std::vector<std::uint8_t> value(data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += size;
        return value;
    }
    std::string text() { const auto value = blob(); return {value.begin(), value.end()}; }
};

class Transport {
public:
    using Payload = std::vector<std::uint8_t>;
    struct Handler {
        virtual ~Handler() = default;
        virtual void frame(std::uint32_t from, std::uint32_t type, Reader& payload) = 0;
        virtual Payload request(std::uint32_t from, std::uint32_t type, Reader& payload) = 0;
        virtual void peer_died(std::uint32_t pid) = 0;
    };
    virtual ~Transport() = default;
    // The Windows transport (a stub that refuses everything elsewhere).
    static std::shared_ptr<Transport> create(std::weak_ptr<Handler> handler);

    virtual std::uint32_t self() const = 0;
    // Starts the endpoint (idempotent). False when it cannot listen.
    virtual bool start() = 0;
    // A one-way frame to `pid`, written after every frame posted earlier to it.
    // False when `pid` cannot be reached.
    virtual bool post(std::uint32_t pid, std::uint32_t type, Payload payload) = 0;
    // A request to `pid` and its response; empty when `pid` is unreachable or
    // died, or after `timeout`.
    virtual std::optional<Payload> call(std::uint32_t pid, std::uint32_t type, Payload payload,
                                        std::chrono::milliseconds timeout) = 0;
    // Reports `pid`'s death to the handler once (peer_died).
    virtual void watch(std::uint32_t pid) = 0;
    // Ends the endpoint and every connection (process teardown).
    virtual void stop() = 0;
};

} // namespace alchemy::libsystem::mach
