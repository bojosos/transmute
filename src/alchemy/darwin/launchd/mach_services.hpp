#pragma once

// The session launchd's Mach side: the ports of its jobs' MachServices. The
// server (server.cpp) is headless code; the ports need the cross-process Mach
// layer, which only alchemy-fx has, so main() passes make_mach_services there
// and the server runs without MachServices elsewhere.

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alchemy::launchd {

// Where a service's port is: its host process and its key there (mach_ipc.hpp).
struct ServiceLocation {
    std::uint32_t host{};
    std::uint32_t key_pid{};
    std::uint64_t key_id{};
};

class MachServices {
public:
    virtual ~MachServices() = default;
    // This process's pid on the Mach transport.
    [[nodiscard]] virtual std::uint32_t self() const = 0;
    // A port for `name`, its receive right held here until a job checks in.
    virtual bool create(const std::string& name) = 0;
    // The port of `name` dies (when still held here) and is forgotten.
    virtual void destroy(const std::string& name) = 0;
    [[nodiscard]] virtual std::optional<ServiceLocation> locate(const std::string& name) = 0;
    // Offers the receive right of `name` to process `pid` (its check-in);
    // false when it is no longer held here.
    virtual bool offer(const std::string& name, std::uint32_t pid) = 0;
    // The names held here with a message queued (a job to launch on demand).
    [[nodiscard]] virtual std::vector<std::string> pending() = 0;
    // Changes of the port space (a message queued): epoch, and a wait for a
    // newer one up to `limit`.
    [[nodiscard]] virtual std::uint64_t epoch() = 0;
    virtual void wait(std::uint64_t seen, std::chrono::milliseconds limit) = 0;
};

using MachServicesFactory = std::unique_ptr<MachServices> (*)();

// alchemy-fx only (mach_services.cpp).
[[nodiscard]] std::unique_ptr<MachServices> make_mach_services();

} // namespace alchemy::launchd
