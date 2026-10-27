// In-process Mach IPC tests (src/alchemy/darwin/libsystem/mach/).
// - ipc_*: the space itself (MachIpc) over a host-backed test memory, so
//   several host threads can send and receive concurrently.
// - exports_*: the libSystem exports through their guest addresses (the
//   fill fixture): mach_msg, kevent64 EVFILT_MACHPORT, bootstrap, task ports,
//   memory entries and mach_vm_map aliases.
#include "alchemy/darwin/libsystem/mach/mach_ipc.hpp"
#include "alchemy/gui/tests/fill_fixture.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

namespace {

using fill_test::Fixture;
using fill_test::require;
using namespace alchemy::libsystem::mach;
using Address = std::uint64_t;
std::uint16_t read16(transmute::GuestMemory& m, Address at) { return static_cast<std::uint16_t>(m.read8(at) | (m.read8(at + 1) << 8)); }
void write16(transmute::GuestMemory& m, Address at, std::uint16_t value) {
    m.write8(at, static_cast<std::uint8_t>(value));
    m.write8(at + 1, static_cast<std::uint8_t>(value >> 8));
}
constexpr std::string_view sys = "/usr/lib/libSystem.B.dylib";

// ---- host-level space -----------------------------------------------------------

// Flat test memory at [base, base + size); thread-safe for disjoint bytes.
class HostMemory final : public transmute::GuestMemory {
public:
    static constexpr std::uint64_t base = 0x10000000, size = 64ULL << 20;
    HostMemory() : bytes_(size) {}
    std::uint8_t read8(std::uint64_t address) const override { return bytes_.at(check(address)); }
    void write8(std::uint64_t address, std::uint8_t value) override { bytes_.at(check(address)) = value; }
    void map_memory(std::uint64_t, std::size_t) override {}
    void map_external_memory(std::uint64_t, std::uint8_t*, std::size_t, bool) override {}
    void unmap_memory(std::uint64_t) override {}
    bool is_range_available(std::uint64_t, std::uint64_t) const override { return false; }
    std::optional<RegionInfo> query_region(std::uint64_t address) const override {
        if (address < base || address >= base + size) return std::nullopt;
        return RegionInfo{base, size, true, true, false, false, false};
    }
    void set_memory_access(std::uint64_t, bool, bool, bool) override {}
    std::uint64_t allocate(std::uint64_t length) {
        const auto rounded = (std::max<std::uint64_t>(length, 1) + 15) & ~15ULL;
        const auto at = next_.fetch_add(rounded);
        if (at + rounded > base + size) throw std::runtime_error("test memory exhausted");
        return at;
    }
    std::atomic<int> deallocations{0};

private:
    std::size_t check(std::uint64_t address) const {
        if (address < base || address >= base + size) throw std::runtime_error("test memory fault");
        return static_cast<std::size_t>(address - base);
    }
    std::vector<std::uint8_t> bytes_;
    std::atomic<std::uint64_t> next_{base};
};

struct Space {
    HostMemory memory;
    std::shared_ptr<MachIpc> ipc;
    Space() {
        MachIpc::Guest guest;
        guest.allocate = [this](std::uint64_t size) { return memory.allocate(size); };
        guest.deallocate = [this](Address, std::uint64_t) { ++memory.deallocations; };
        guest.kernel_name = [](Name name) { return name == 0xa1c1; };
        guest.interrupted = [] { return false; };
        guest.audit_token = [] { return std::array<std::uint32_t, 8>{501, 501, 20, 501, 20, 4242, 0, 0}; };
        ipc = std::make_shared<MachIpc>(std::move(guest));
    }
    Name receive_right() {
        Name name{};
        require(ipc->allocate(right::receive, name) == kern::success && MachIpc::allocated_name(name), "allocate receive right");
        return name;
    }
    std::uint32_t refs(Name name, std::uint32_t kind) {
        std::uint32_t value{};
        require(ipc->get_refs(name, kind, value) == kern::success, "get_refs");
        return value;
    }
    std::uint32_t type(Name name) {
        std::uint32_t value{};
        return ipc->type(name, value) == kern::success ? value : 0;
    }
    // A simple message: header (bits, remote, local, id) and `body` words.
    std::uint64_t message(std::uint32_t bits, Name remote, Name local, std::int32_t id, std::vector<std::uint32_t> body = {}) {
        const auto size = 24 + static_cast<std::uint32_t>(body.size()) * 4;
        const auto at = memory.allocate(size);
        memory.write32(at, bits);
        memory.write32(at + 4, size);
        memory.write32(at + 8, remote);
        memory.write32(at + 12, local);
        memory.write32(at + 16, 0);
        memory.write32(at + 20, static_cast<std::uint32_t>(id));
        for (std::size_t index = 0; index < body.size(); ++index) memory.write32(at + 24 + index * 4, body[index]);
        return at;
    }
    std::uint32_t send(std::uint64_t at, std::uint32_t options = option::send_msg, std::uint32_t timeout = 0) {
        return ipc->send(memory, at, options, memory.read32(at + 4), timeout);
    }
    struct Received { std::uint32_t result{}; std::uint64_t at{}; std::uint32_t bits{}, size{}; Name remote{}, local{}; std::int32_t id{}; };
    Received receive(Name name, std::uint32_t options = option::rcv_msg, std::uint32_t timeout = 0, std::uint32_t capacity = 512) {
        Received out;
        out.at = memory.allocate(capacity);
        out.result = ipc->receive(memory, out.at, options, capacity, name, timeout);
        if (out.result == kern::success) {
            out.bits = memory.read32(out.at);
            out.size = memory.read32(out.at + 4);
            out.remote = memory.read32(out.at + 8);
            out.local = memory.read32(out.at + 12);
            out.id = static_cast<std::int32_t>(memory.read32(out.at + 20));
        }
        return out;
    }
};

constexpr std::uint32_t bits(std::uint8_t remote, std::uint8_t local = 0) { return remote | (static_cast<std::uint32_t>(local) << 8); }
constexpr std::uint32_t type_send = 1U << 16, type_receive = 1U << 17, type_send_once = 1U << 18, type_port_set = 1U << 19,
    type_dead_name = 1U << 20;

void ipc_rights() {
    Space space;
    auto& ipc = *space.ipc;
    const auto port = space.receive_right();
    require(space.type(port) == type_receive, "a new receive right");
    require(ipc.insert_right(port, port, disp::make_send) == kern::success, "insert_right MAKE_SEND");
    require(space.type(port) == (type_receive | type_send) && space.refs(port, right::send) == 1, "send and receive share the name");
    require(ipc.mod_refs(port, right::send, 2) == kern::success && space.refs(port, right::send) == 3, "mod_refs +2");
    require(ipc.deallocate(port) == kern::success && space.refs(port, right::send) == 2, "deallocate drops one reference");
    require(ipc.mod_refs(port, right::send, -3) == kern::invalid_value, "mod_refs below zero");
    require(ipc.mod_refs(port, right::send, -2) == kern::success && space.type(port) == type_receive, "mod_refs -2 removes the right");
    require(ipc.deallocate(port) == kern::invalid_right, "deallocate of a receive right");
    require(ipc.mod_refs(port, right::receive, -2) == kern::invalid_value, "receive delta -2");
    std::uint8_t kind{};
    Name once{};
    require(ipc.extract_right(port, disp::make_send_once, once, kind) == kern::success && kind == disp::port_send_once && once != port,
            "extract_right MAKE_SEND_ONCE names a new send-once right");
    require(space.type(once) == type_send_once, "send-once type");
    Name set{}, dead{};
    require(ipc.allocate(right::port_set, set) == kern::success && space.type(set) == type_port_set, "port set");
    require(ipc.allocate(right::dead_name, dead) == kern::success && space.type(dead) == type_dead_name, "dead name");
    require(ipc.allocate(right::send, dead) == kern::invalid_value, "a send right cannot be allocated");
    require(ipc.mod_refs(port, right::receive, -1) == kern::success, "destroy the receive right");
    std::uint32_t value{};
    require(ipc.type(port, value) == kern::invalid_name, "the receive-only name is gone");
    require(space.type(once) == type_dead_name, "the send-once right became a dead name");
    require(ipc.deallocate(once) == kern::success && ipc.type(once, value) == kern::invalid_name, "dead name deallocated");
    // Kernel names are immortal send rights.
    require(ipc.deallocate(0xa1c1) == kern::success && space.type(0xa1c1) == type_send, "task self is a kernel send right");
    require(ipc.deallocate(0x12345678) == kern::invalid_name, "unknown name");
    // Names are reused with a new generation.
    Name fresh{};
    require(ipc.allocate(right::receive, fresh) == kern::success && fresh != port, "a recycled index gets a new generation");
    // allocate_name and construct.
    require(ipc.allocate_name(right::receive, 0x777700) == kern::success && space.type(0x777700) == type_receive, "allocate_name");
    require(ipc.allocate_name(right::receive, 0x777700) == kern::name_exists, "allocate_name in use");
    MachIpc::Options options{0x01 | 0x02 | 0x10, 16};   // CONTEXT_AS_GUARD | QLIMIT | INSERT_SEND_RIGHT
    Name guarded{};
    require(ipc.construct(options, 0xfeed, guarded) == kern::success && space.type(guarded) == (type_receive | type_send), "construct");
    std::vector<std::uint32_t> status;
    require(ipc.get_attributes(guarded, 2, 10, status) == kern::success && status.size() == 10 && status[3] == 16 && status[6] == 1 &&
            (status[9] & 0x02), "receive status: qlimit, srights, guarded");
    require(ipc.get_attributes(guarded, 2, 9, status) == kern::failure, "status needs ten words");
    bool stopped = false;
    try { static_cast<void>(ipc.mod_refs(guarded, right::receive, -1)); } catch (const std::runtime_error&) { stopped = true; }
    require(stopped, "destroying a guarded port without its guard is a guard violation");
    require(ipc.destruct(guarded, -1, 0xfeed) == kern::success && ipc.type(guarded, value) == kern::invalid_name, "destruct with the guard");
}

void ipc_messages() {
    Space space;
    auto& ipc = *space.ipc;
    const auto port = space.receive_right();
    const auto reply = space.receive_right();
    // MAKE_SEND to the destination, MAKE_SEND_ONCE reply.
    const auto at = space.message(bits(disp::make_send, disp::make_send_once), port, reply, 1234, {0xabcdef, 7});
    require(space.send(at) == kern::success, "send");
    const auto got = space.receive(port, option::rcv_msg | (3U << 24));   // MACH_RCV_TRAILER_AUDIT
    require(got.result == kern::success, "receive");
    require(got.bits == bits(disp::port_send_once, disp::port_send), "received bits: reply send-once, destination send");
    require(got.local == port && got.id == 1234 && got.size == 32, "local port, id, size");
    require(space.type(got.remote) == type_send_once, "the reply right arrived as a send-once name");
    require(space.memory.read32(got.at + 24) == 0xabcdef && space.memory.read32(got.at + 28) == 7, "inline body");
    require(space.memory.read32(got.at + 32) == 0 && space.memory.read32(got.at + 36) == 52, "audit trailer type and size");
    require(space.memory.read32(got.at + 40) == 0, "first sequence number");
    require(space.memory.read32(got.at + 44) == 501 && space.memory.read32(got.at + 52 + 20) == 4242, "sender and audit token (pid)");
    // The make-send right was consumed by the receive: no send right left.
    std::vector<std::uint32_t> status;
    require(ipc.get_attributes(port, 2, 10, status) == kern::success && status[1] == 1 && status[2] == 1 && status[6] == 0,
            "seqno 1, mscount 1, no senders");
    // Reply through the send-once right.
    const auto answer = space.message(bits(disp::move_send_once), got.remote, 0, 1235);
    require(space.send(answer) == kern::success, "reply on the send-once right");
    std::uint32_t value{};
    require(ipc.type(got.remote, value) == kern::invalid_name, "the send-once right was consumed");
    const auto replied = space.receive(reply);
    require(replied.result == kern::success && replied.bits == bits(0, disp::port_send_once) && replied.id == 1235, "reply received");
    // Invalid destinations and headers.
    require(space.send(space.message(bits(disp::copy_send), port, 0, 1)) == mr::send_invalid_dest, "COPY_SEND without a send right");
    require(space.send(space.message(bits(disp::make_send), 0, 0, 1)) == mr::send_invalid_dest, "MACH_PORT_NULL destination");
    require(space.send(space.message(bits(disp::move_receive), port, 0, 1)) == mr::send_invalid_header, "receive disposition as destination");
    require(space.send(space.message(bits(disp::make_send), 0xa1c1, 0, 1)) == mr::send_invalid_dest, "kernel ports have no MIG server");
    require(space.receive(0x12345678).result == mr::rcv_invalid_name, "receive on an unknown name");
}

void ipc_timeouts_and_limits() {
    Space space;
    auto& ipc = *space.ipc;
    const auto port = space.receive_right();
    const auto start = std::chrono::steady_clock::now();
    require(space.receive(port, option::rcv_msg | option::rcv_timeout, 20).result == mr::rcv_timed_out, "receive times out");
    require(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(15), "the timeout was waited for");
    require(space.receive(port, option::rcv_msg | option::rcv_timeout, 0).result == mr::rcv_timed_out, "zero timeout polls");
    require(ipc.set_attributes(port, 1, {1}) == kern::success, "MACH_PORT_LIMITS_INFO qlimit 1");
    require(ipc.set_attributes(port, 1, {2000}) == kern::invalid_value, "qlimit above MACH_PORT_QLIMIT_MAX");
    require(ipc.insert_right(port, port, disp::make_send) == kern::success, "send right");
    require(space.send(space.message(bits(disp::copy_send), port, 0, 1)) == kern::success, "first message fits");
    const auto blocked = space.message(bits(disp::move_send), port, 0, 2);
    require(space.send(blocked, option::send_msg | option::send_timeout, 20) == mr::send_timed_out, "the full queue times out");
    require(space.refs(port, right::send) == 1, "a timed-out send keeps its rights");
    // Send-once messages ignore the queue limit.
    require(space.send(space.message(bits(disp::make_send_once), port, 0, 3)) == kern::success, "send-once ignores qlimit");
    // MACH_RCV_LARGE keeps a message that does not fit and reports its size.
    require(ipc.set_attributes(port, 1, {5}) == kern::success, "qlimit 5");
    const auto large = space.message(bits(disp::copy_send), port, 0, 4, std::vector<std::uint32_t>(16, 9));
    require(space.receive(port).result == kern::success && space.receive(port).result == kern::success, "drain");
    require(space.send(large) == kern::success, "large message");
    auto small = space.receive(port, option::rcv_msg | option::rcv_large, 0, 40);
    require(small.result == mr::rcv_too_large && space.memory.read32(small.at + 4) == 24 + 64 + 8, "too large: needed size with trailer");
    require(space.receive(port, option::rcv_msg, 0, 96).result == kern::success, "then received whole");
    require(space.send(large) == kern::success, "large message again");
    require(space.receive(port, option::rcv_msg, 0, 40).result == mr::rcv_too_large, "too large without MACH_RCV_LARGE");
    require(space.receive(port, option::rcv_msg | option::rcv_timeout, 0).result == mr::rcv_timed_out, "and destroyed");
    require(space.receive(port, option::rcv_msg | (9U << 24)).result == mr::rcv_invalid_trailer, "unknown trailer elements");
}

void ipc_descriptors() {
    Space space;
    auto& ipc = *space.ipc;
    auto& m = space.memory;
    const auto port = space.receive_right();
    const auto carried = space.receive_right();
    const auto shared = space.receive_right();
    require(ipc.insert_right(shared, shared, disp::make_send) == kern::success, "send right to share");
    // Complex message: port (MOVE_RECEIVE carried), port (COPY_SEND shared),
    // OOL memory (deallocate), OOL ports [shared COPY_SEND, NULL].
    const auto ool = m.allocate(100);
    for (int index = 0; index < 100; ++index) m.write8(ool + index, static_cast<std::uint8_t>(index));
    const auto names = m.allocate(8);
    m.write32(names, shared);
    m.write32(names + 4, 0);
    const std::uint32_t size = 24 + 4 + 12 + 12 + 16 + 16;
    const auto at = m.allocate(size);
    m.write32(at, 0x80000000U | bits(disp::make_send));
    m.write32(at + 4, size);
    m.write32(at + 8, port);
    m.write32(at + 12, 0);
    m.write32(at + 16, 0);
    m.write32(at + 20, 77);
    m.write32(at + 24, 4);
    auto cursor = at + 28;
    m.write32(cursor, carried); m.write8(cursor + 10, disp::move_receive); m.write8(cursor + 11, 0); cursor += 12;
    m.write32(cursor, shared); m.write8(cursor + 10, disp::copy_send); m.write8(cursor + 11, 0); cursor += 12;
    m.write64(cursor, ool); m.write8(cursor + 8, 1); m.write8(cursor + 9, 0); m.write8(cursor + 11, 1); m.write32(cursor + 12, 100); cursor += 16;
    m.write64(cursor, names); m.write8(cursor + 8, 0); m.write8(cursor + 10, disp::copy_send); m.write8(cursor + 11, 2); m.write32(cursor + 12, 2);
    require(space.send(at) == kern::success, "complex send");
    std::uint32_t value{};
    require(ipc.type(carried, value) == kern::invalid_name, "the moved receive right left the space");
    require(m.deallocations.load() == 1, "deallocate = TRUE released the sender's OOL memory");
    const auto got = space.receive(port);
    require(got.result == kern::success && (got.bits & 0x80000000U) && got.size == size, "complex receive");
    require(m.read32(got.at + 24) == 4, "descriptor count");
    auto in = got.at + 28;
    const auto received_receive = m.read32(in);
    require(m.read8(in + 10) == disp::port_receive && space.type(received_receive) == type_receive, "receive right arrives under a new name");
    in += 12;
    require(m.read32(in) == shared && m.read8(in + 10) == disp::port_send, "a send right merges into the existing name");
    require(space.refs(shared, right::send) == 3, "both carried send rights merged into the name");
    in += 12;
    const auto region = m.read64(in);
    require(m.read32(in + 12) == 100 && region && m.read8(in + 11) == 1, "OOL descriptor");
    for (int index = 0; index < 100; ++index) require(m.read8(region + index) == static_cast<std::uint8_t>(index), "OOL bytes");
    in += 16;
    const auto array = m.read64(in);
    require(m.read32(in + 12) == 2 && m.read8(in + 10) == disp::port_send && m.read32(array) == shared && m.read32(array + 4) == 0,
            "OOL ports: a send right and MACH_PORT_NULL");
    // A message whose body names an invalid right is not sent, and nothing moves.
    const auto bad = m.allocate(24 + 4 + 12);
    m.write32(bad, 0x80000000U | bits(disp::move_send));
    m.write32(bad + 4, 40);
    m.write32(bad + 8, shared);
    m.write32(bad + 12, 0); m.write32(bad + 16, 0); m.write32(bad + 20, 1);
    m.write32(bad + 24, 1);
    m.write32(bad + 28, 0x99999900); m.write8(bad + 38, disp::copy_send); m.write8(bad + 39, 0);
    require(space.send(bad) == mr::send_invalid_right, "invalid body right");
    require(space.refs(shared, right::send) == 3, "the destination right was restored");
    // mach_msg_destroy on a received complex message releases its rights.
    const auto again = space.message(bits(disp::copy_send), shared, 0, 5);
    require(space.send(again) == kern::success, "plain message");
    const auto plain = space.receive(shared);
    require(plain.result == kern::success, "received");
    ipc.destroy_message(m, got.at);
    require(ipc.type(received_receive, value) == kern::invalid_name, "mach_msg_destroy destroyed the receive right");
    require(space.refs(shared, right::send) == 1, "and released both send rights");
}

void ipc_notifications() {
    Space space;
    auto& ipc = *space.ipc;
    const auto notify = space.receive_right();
    const auto port = space.receive_right();
    Name previous{};
    // No-senders: immediate (no send right exists and mscount 0 >= sync 0).
    require(ipc.request_notification(port, notify::no_senders, 0, notify, disp::make_send_once, previous) == kern::success, "no-senders");
    auto got = space.receive(notify);
    require(got.result == kern::success && got.id == notify::no_senders && space.memory.read32(got.at + 32) == 0, "immediate no-senders");
    // Armed at mscount 1: fires when the last send right goes away.
    require(ipc.insert_right(port, port, disp::make_send) == kern::success, "make send (mscount 1)");
    require(ipc.request_notification(port, notify::no_senders, 1, notify, disp::make_send_once, previous) == kern::success, "armed");
    require(space.receive(notify, option::rcv_msg | option::rcv_timeout, 0).result == mr::rcv_timed_out, "not yet");
    // A send right in a queued message still counts.
    require(space.send(space.message(bits(disp::move_send), port, 0, 9)) == kern::success, "the send right travels");
    require(space.receive(notify, option::rcv_msg | option::rcv_timeout, 0).result == mr::rcv_timed_out, "in transit: still a sender");
    require(space.receive(port).result == kern::success, "received (consumes the right)");
    got = space.receive(notify, option::rcv_msg | option::rcv_timeout, 0);
    require(got.result == kern::success && got.id == notify::no_senders && space.memory.read32(got.at + 32) == 1, "no-senders, count 1");
    // Send-once: destroying the right sends a notification to its port.
    Name once{};
    std::uint8_t kind{};
    require(ipc.extract_right(port, disp::make_send_once, once, kind) == kern::success, "send-once right");
    // Dead-name request on the send-once name.
    require(ipc.request_notification(once, notify::dead_name, 0, notify, disp::make_send_once, previous) == kern::success && !previous,
            "dead-name request");
    std::uint32_t bits_out{};
    require(ipc.type(once, bits_out) == kern::success && (bits_out & 0x80000000U), "MACH_PORT_TYPE_DNREQUEST");
    Name once2{};
    require(ipc.extract_right(port, disp::make_send_once, once2, kind) == kern::success, "another send-once right");
    require(ipc.deallocate(once2) == kern::success, "destroy it");
    got = space.receive(port);
    require(got.result == kern::success && got.id == notify::send_once && got.size == 24 && got.bits == bits(0, disp::port_send_once),
            "send-once notification");
    // Port death: the send-once name becomes a dead name (+1 reference for the notification).
    require(ipc.mod_refs(port, right::receive, -1) == kern::success, "destroy the port");
    got = space.receive(notify);
    require(got.result == kern::success && got.id == notify::dead_name && space.memory.read32(got.at + 32) == once, "dead-name notification");
    require(space.type(once) == type_dead_name && space.refs(once, right::dead_name) == 2, "dead name with two references");
    // Port-destroyed: the receive right is sent to the notify port.
    const auto keeper = space.receive_right();
    require(space.send(space.message(bits(disp::make_send), keeper, 0, 31)) == kern::success, "a queued message");
    require(ipc.request_notification(keeper, notify::port_destroyed, 0, notify, disp::make_send_once, previous) == kern::success, "pd");
    require(ipc.mod_refs(keeper, right::receive, -1) == kern::success, "destroy the receive right");
    got = space.receive(notify);
    require(got.result == kern::success && got.id == notify::port_destroyed && (got.bits & 0x80000000U), "port-destroyed notification");
    const auto revived = space.memory.read32(got.at + 28);
    require(space.memory.read8(got.at + 28 + 10) == disp::port_receive && space.type(revived) == type_receive, "the receive right came back");
    const auto queued = space.receive(revived);
    require(queued.result == kern::success && queued.id == 31, "with its queued message");
    // Send-possible: immediate when the queue has room (sync).
    require(ipc.insert_right(revived, revived, disp::make_send) == kern::success, "send right");
    require(ipc.request_notification(revived, notify::send_possible, 1, notify, disp::make_send_once, previous) == kern::success, "sp");
    got = space.receive(notify);
    require(got.result == kern::success && got.id == notify::send_possible && space.memory.read32(got.at + 32) == revived, "send-possible");
}

void ipc_port_sets() {
    Space space;
    auto& ipc = *space.ipc;
    const auto a = space.receive_right(), b = space.receive_right();
    Name set{};
    require(ipc.allocate(right::port_set, set) == kern::success, "port set");
    require(ipc.insert_member(a, set) == kern::success && ipc.insert_member(b, set) == kern::success, "members");
    require(ipc.insert_member(a, set) == kern::already_in_set, "already a member");
    std::vector<Name> members;
    require(ipc.set_status(set, members) == kern::success && members.size() == 2, "set status");
    require(space.send(space.message(bits(disp::make_send), b, 0, 2)) == kern::success, "to b");
    require(space.send(space.message(bits(disp::make_send), a, 0, 1)) == kern::success, "to a");
    auto got = space.receive(set);
    require(got.result == kern::success && got.id == 2 && got.local == b, "the oldest message first, local port = member");
    got = space.receive(set);
    require(got.result == kern::success && got.id == 1 && got.local == a, "then a");
    require(ipc.move_member(a, 0) == kern::success && ipc.set_status(set, members) == kern::success && members == std::vector<Name>{b}, "move out");
    require(ipc.extract_member(a, set) == kern::not_in_set, "not in the set");
    require(space.send(space.message(bits(disp::make_send), a, 0, 3)) == kern::success, "to a (not a member)");
    require(space.receive(set, option::rcv_msg | option::rcv_timeout, 0).result == mr::rcv_timed_out, "the set does not see it");
    require(ipc.move_member(a, set) == kern::success, "move back");
    require(space.receive(set).id == 3, "now it does");
    // EVFILT_MACHPORT view.
    require(ipc.attach(set) == 0 && ipc.attach(a) == 0, "attach to a set and a receive right");
    require(ipc.insert_right(a, a, disp::make_send) == kern::success, "send right");
    Name once{};
    std::uint8_t kind{};
    require(ipc.extract_right(a, disp::make_send_once, once, kind) == kern::success && ipc.attach(once) == 45, "a send-once right: ENOTSUP");
    require(ipc.attach(0x55555500) == 2, "unknown: ENOENT");
    require(!ipc.poll(space.memory, set, 0, 0, 0), "nothing queued");
    require(space.send(space.message(bits(disp::copy_send), a, 0, 4, {1, 2})) == kern::success, "queue one");
    const auto seen = ipc.poll(space.memory, set, 0, 0, 0);
    require(seen && seen->data == a, "detection names the member");
    const auto buffer = space.memory.allocate(256);
    const auto small = ipc.poll(space.memory, set, option::rcv_msg, buffer, 16);
    require(small && small->fflags == mr::rcv_too_large, "direct receive into a small buffer");
    const auto direct = ipc.poll(space.memory, set, option::rcv_msg, buffer, 256);
    require(direct && direct->fflags == 0 && direct->ext1 == 32 + 8 && space.memory.read32(buffer + 20) == 4, "direct receive");
    require(!ipc.poll(space.memory, set, 0, 0, 0), "consumed");
    require(ipc.mod_refs(set, right::port_set, -1) == kern::success, "destroy the set");
    require(space.receive(a, option::rcv_msg | option::rcv_timeout, 0).result == mr::rcv_timed_out, "members survive");
}

void ipc_threads() {
    Space space;
    auto& ipc = *space.ipc;
    const auto port = space.receive_right();
    require(ipc.insert_right(port, port, disp::make_send) == kern::success, "send right");
    constexpr int senders = 4, per_sender = 250;
    std::atomic<bool> failed{false};
    std::vector<std::thread> threads;
    for (int sender = 0; sender < senders; ++sender) {
        threads.emplace_back([&, sender] {
            for (int index = 0; index < per_sender; ++index) {
                const auto at = space.message(bits(disp::copy_send), port, 0, sender * 1000 + index, {static_cast<std::uint32_t>(index)});
                if (space.send(at) != kern::success) failed = true;   // blocks while the queue (limit 5) is full
            }
        });
    }
    std::set<std::int32_t> ids;
    std::map<int, int> last;
    for (int count = 0; count < senders * per_sender; ++count) {
        const auto got = space.receive(port, option::rcv_msg | option::rcv_timeout, 10000);
        require(got.result == kern::success, "blocking receive");
        ids.insert(got.id);
        const auto sender = got.id / 1000, index = got.id % 1000;
        require(!last.contains(sender) || last[sender] < index, "per-sender order is kept");
        last[sender] = index;
    }
    for (auto& thread : threads) thread.join();
    require(!failed && ids.size() == senders * per_sender, "every message arrived once");
    require(space.refs(port, right::send) == 1, "the copied send rights were all consumed");
    // A receiver blocked without a timeout wakes when a message comes...
    std::atomic<std::uint32_t> woke{1};
    std::thread waiter([&] { woke = space.receive(port).result; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(space.send(space.message(bits(disp::copy_send), port, 0, 1)) == kern::success, "wake-up message");
    waiter.join();
    require(woke == kern::success, "the blocked receive got it");
    // ... a kevent-style waiter wakes on the epoch ...
    const auto epoch = ipc.epoch();
    std::thread kq([&] { ipc.wait(epoch, std::chrono::seconds(10)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto started = std::chrono::steady_clock::now();
    require(space.send(space.message(bits(disp::copy_send), port, 0, 2)) == kern::success, "enqueue");
    kq.join();
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5), "the wait ended on enqueue");
    require(space.receive(port).result == kern::success, "drain");
    // ... and a blocked receive reports a destroyed port.
    std::atomic<bool> receiving{false};
    std::thread dying([&] { receiving = true; woke = space.receive(port).result; });
    while (!receiving) std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(ipc.mod_refs(port, right::receive, -1) == kern::success, "destroy while blocked");
    dying.join();
    require(woke == mr::rcv_port_died, "MACH_RCV_PORT_DIED");
}

// ---- guest exports --------------------------------------------------------------------

alchemy::CallRegs registers(std::initializer_list<Address> values) {
    alchemy::CallRegs out{};
    unsigned index = 0;
    for (const auto value : values) out.x[index++] = value;
    return out;
}

void exports_ports_and_messages(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto name = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0, "mach_port_allocate");
    const auto port = f.engine.read32(name);
    require(f.status(sys, "_mach_port_allocate", {0, 1, name}) == static_cast<std::int32_t>(mr::send_invalid_dest), "null task");
    require(f.status(sys, "_mach_port_insert_right", {self, port, port, disp::make_send}) == 0, "insert_right");
    const auto type = f.block(4), refs = f.block(4);
    require(f.status(sys, "_mach_port_type", {self, port, type}) == 0 && f.engine.read32(type) == 0x30000, "mach_port_type");
    require(f.status(sys, "_mach_port_get_refs", {self, port, 0, refs}) == 0 && f.engine.read32(refs) == 1, "mach_port_get_refs");
    // mach_msg send then receive (with an audit trailer).
    const auto message = f.block(64);
    f.engine.write32(message, disp::copy_send);
    f.engine.write32(message + 4, 32);
    f.engine.write32(message + 8, port);
    f.engine.write32(message + 20, 4321);
    f.engine.write64(message + 24, 0x1122334455667788ULL);
    require(f.status(sys, "_mach_msg", {message, option::send_msg, 32, 0, 0, 0, 0}) == 0, "mach_msg send");
    const auto buffer = f.block(256);
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | (3U << 24), 0, 256, port, 0, 0}) == 0, "mach_msg receive");
    require(f.engine.read32(buffer + 20) == 4321 && f.engine.read64(buffer + 24) == 0x1122334455667788ULL, "message body");
    require(f.engine.read32(buffer + 52 + 20) == static_cast<std::uint32_t>(f.call(sys, "_getpid")), "audit token pid");
    require(f.call("/usr/lib/libbsm.0.dylib", "_audit_token_to_pid", {buffer + 52}) == f.call(sys, "_getpid"), "audit_token_to_pid");
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | option::rcv_timeout, 0, 256, port, 5, 0}) ==
            static_cast<std::int32_t>(mr::rcv_timed_out), "receive timeout");
    // mach_msg_send / mach_msg_receive use msgh_size and msgh_local_port.
    require(f.status(sys, "_mach_msg_send", {message}) == 0, "mach_msg_send");
    f.engine.write32(buffer + 4, 256);
    f.engine.write32(buffer + 12, port);
    require(f.status(sys, "_mach_msg_receive", {buffer}) == 0 && f.engine.read32(buffer + 20) == 4321, "mach_msg_receive");
    // mach_port_deallocate (replaces the kernel-names-only version).
    require(f.status(sys, "_mach_port_deallocate", {self, port}) == 0, "deallocate the send right");
    require(f.status(sys, "_mach_port_deallocate", {self, port}) == static_cast<std::int32_t>(kern::invalid_right), "receive only now");
    require(f.status(sys, "_mach_port_deallocate", {self, f.call(sys, "_mach_thread_self")}) == 0, "thread port");
    require(f.status(sys, "_mach_port_deallocate", {self, 0xffffffffU}) == 0 && f.status(sys, "_mach_port_deallocate", {self, 0}) == 0,
            "MACH_PORT_DEAD and MACH_PORT_NULL: nothing to release (a right whose port died arrives as MACH_PORT_DEAD)");
    // MIG reply ports are per thread and stable.
    const auto reply = f.call(sys, "_mig_get_reply_port");
    require(reply && f.call(sys, "_mig_get_reply_port") == reply, "mig_get_reply_port");
    f.call(sys, "_mig_dealloc_reply_port", {reply});
    require(f.call(sys, "_mig_get_reply_port") != reply, "a new reply port after mig_dealloc_reply_port");
    require(f.call(sys, "_mach_reply_port") != 0, "mach_reply_port");
    require(f.call(sys, "_voucher_mach_msg_set", {message}) == 0, "no voucher is attached");
    // Attributes through guest memory.
    const auto info = f.block(68), count = f.block(4);
    f.engine.write32(count, 10);
    require(f.status(sys, "_mach_port_get_attributes", {self, port, 2, info, count}) == 0 && f.engine.read32(count) == 10 &&
            f.engine.read32(info + 12) == 5, "MACH_PORT_RECEIVE_STATUS (default qlimit 5)");
    f.engine.write32(info, 1);
    require(f.status(sys, "_mach_port_set_attributes", {self, port, 1, info, 1}) == 0, "MACH_PORT_LIMITS_INFO");
    // mach_port_construct with options.
    const auto options = f.block(24);
    f.engine.write32(options, 0x10 | 0x02);
    f.engine.write32(options + 4, 1024);
    require(f.status(sys, "_mach_port_construct", {self, options, 0, name}) == 0, "mach_port_construct");
    require(f.status(sys, "_mach_port_get_refs", {self, f.engine.read32(name), 0, refs}) == 0 && f.engine.read32(refs) == 1,
            "MPO_INSERT_SEND_RIGHT");
    require(f.status(sys, "_mach_port_destruct", {self, f.engine.read32(name), static_cast<Address>(-1), 0}) == 0, "mach_port_destruct");
    require(f.status(sys, "_mach_port_mod_refs", {self, 0x1234, 1, 1}) == static_cast<std::int32_t>(kern::invalid_name), "unknown name");
}

// mach_msg_server_once / mach_msg_server (mach/mach_extra.cpp): a MIG-style round trip.
void exports_msg_server(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto name = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0, "service port");
    const auto service = f.engine.read32(name);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0, "reply port");
    const auto reply_port = f.engine.read32(name);
    require(f.status(sys, "_mach_port_insert_right", {self, service, service, disp::make_send}) == 0, "send right");
    const auto request = [&](std::uint32_t id) {
        const auto message = f.block(64);
        f.engine.write32(message, disp::copy_send | (static_cast<std::uint32_t>(disp::make_send_once) << 8));
        f.engine.write32(message + 4, 24);
        f.engine.write32(message + 8, service);
        f.engine.write32(message + 12, reply_port);
        f.engine.write32(message + 20, id);
        require(f.status(sys, "_mach_msg", {message, option::send_msg, 24, 0, 0, 0, 0}) == 0, "request sent");
    };
    static std::uint32_t seen = 0;
    static std::int32_t answer = 0;
    const auto demux = f.process.install_function("_mach_test_demux", [](transmute::CpuState& s, transmute::GuestMemory& m) {
        const auto in = s.x[0], out = s.x[1];
        seen = m.read32(in + 20);
        m.write32(out, m.read32(in) & 0x1f);   // the reply right the request carried
        m.write32(out + 4, 36);
        m.write32(out + 8, m.read32(in + 8));
        m.write32(out + 12, 0);
        m.write32(out + 20, seen + 100);
        m.write32(out + 32, static_cast<std::uint32_t>(answer));   // mig_reply_error_t.RetCode
        s.x[0] = 1;
    });
    request(500);
    require(f.status(sys, "_mach_msg_server_once", {demux, 64, service, 0}) == 0 && seen == 500, "the demux routine ran");
    const auto buffer = f.block(256);
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | option::rcv_timeout, 0, 256, reply_port, 100, 0}) == 0 &&
            f.engine.read32(buffer + 20) == 600, "the reply arrives");
    answer = -305;   // MIG_NO_REPLY
    request(501);
    require(f.status(sys, "_mach_msg_server_once", {demux, 64, service, 0}) == 0 && seen == 501, "a second round");
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | option::rcv_timeout, 0, 256, reply_port, 5, 0}) ==
            static_cast<std::int32_t>(mr::rcv_timed_out), "MIG_NO_REPLY sends nothing");
    // The loop ends on a receive error (here: a timeout on the empty port).
    require(f.status(sys, "_mach_msg_server", {demux, 64, service, option::rcv_timeout}) == static_cast<std::int32_t>(mr::rcv_timed_out),
            "mach_msg_server returns on a receive error");
}

void exports_kevent(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto name = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0, "port");
    const auto port = f.engine.read32(name);
    require(f.status(sys, "_mach_port_allocate", {self, 3, name}) == 0, "port set");
    const auto set = f.engine.read32(name);
    const auto other = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, other}) == 0, "member");
    const auto member = f.engine.read32(other);
    require(f.status(sys, "_mach_port_insert_member", {self, member, set}) == 0, "insert member");
    const auto kq = f.call(sys, "_kqueue");
    // EV_ADD: the port with MACH_RCV_MSG (direct receive into ext[0]); the set for detection.
    const auto receive_buffer = f.block(128);
    const auto changes = f.block(96), events = f.block(96 * 2);
    f.engine.write64(changes, port);
    write16(f.engine, changes + 8, static_cast<std::uint16_t>(-8));
    write16(f.engine, changes + 10, 1);
    f.engine.write32(changes + 12, option::rcv_msg);
    f.engine.write64(changes + 32, receive_buffer);
    f.engine.write64(changes + 40, 128);
    f.engine.write64(changes + 48, set);
    write16(f.engine, changes + 56, static_cast<std::uint16_t>(-8));
    write16(f.engine, changes + 58, 1);
    require(f.status(sys, "_kevent64", {kq, changes, 2, 0, 0, 0, 0}) == 0, "kevent64 EV_ADD EVFILT_MACHPORT");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 2, 1, 0}) == 0, "nothing pending (KEVENT_FLAG_IMMEDIATE)");
    const auto message = f.block(32);
    f.engine.write32(message, disp::make_send_once);
    f.engine.write32(message + 4, 24);
    f.engine.write32(message + 8, port);
    f.engine.write32(message + 20, 99);
    require(f.status(sys, "_mach_msg", {message, option::send_msg, 24, 0, 0, 0, 0}) == 0, "send to the port");
    f.engine.write32(message + 8, member);
    require(f.status(sys, "_mach_msg", {message, option::send_msg, 24, 0, 0, 0, 0}) == 0, "send to the member");
    const auto timeout = f.block(16);
    f.engine.write64(timeout, 1);
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 2, 0, timeout}) == 2, "two events");
    bool direct = false, detected = false;
    for (int index = 0; index < 2; ++index) {
        const auto at = events + index * 48ULL;
        require(static_cast<std::int16_t>(read16(f.engine, at + 8)) == -8, "EVFILT_MACHPORT");
        if (f.engine.read64(at) == port) {
            direct = f.engine.read32(at + 12) == 0 && f.engine.read64(at + 40) == 24 + 8 && f.engine.read32(receive_buffer + 20) == 99;
        } else if (f.engine.read64(at) == set) {
            detected = f.engine.read64(at + 16) == member;
        }
    }
    require(direct, "the wake-up message was received into ext[0]");
    require(detected, "the set event names the member in data");
    // The detected message is still queued; the received one is not.
    const auto buffer = f.block(128);
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | option::rcv_timeout, 0, 128, member, 0, 0}) == 0, "member message");
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | option::rcv_timeout, 0, 128, port, 0, 0}) ==
            static_cast<std::int32_t>(mr::rcv_timed_out), "port message consumed by kevent64");
    // kevent (32-byte events) still rejects EV_ADD of a send-only name.
    require(f.status(sys, "_mach_port_insert_right", {self, port, port, disp::make_send}) == 0, "send right");
    const auto send_only = f.block(4);
    require(f.status(sys, "_mach_port_extract_right", {self, port, disp::make_send_once, send_only, f.block(4)}) == 0, "send-once name");
    f.engine.write64(changes, f.engine.read32(send_only));
    write16(f.engine, changes + 10, 1 | 0x40);   // EV_ADD | EV_RECEIPT
    f.engine.write32(changes + 12, 0);
    require(f.status(sys, "_kevent64", {kq, changes, 1, events, 1, 0, 0}) == 1 && read16(f.engine, events + 10) == 0x4000 &&
            f.engine.read64(events + 16) == 45, "EV_ERROR ENOTSUP for a send-once right");
    static_cast<void>(f.call(sys, "_close", {kq}));
}

void exports_tasks_and_bootstrap(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto bootstrap = f.engine.read32(f.symbol(sys, "_bootstrap_port"));
    require(bootstrap != 0, "bootstrap_port is a send right");
    const auto port = f.block(4);
    require(f.status(sys, "_task_get_special_port", {self, 4, port}) == 0 && f.engine.read32(port) == bootstrap, "TASK_BOOTSTRAP_PORT");
    require(f.status(sys, "_task_get_special_port", {self, 1, port}) == 0 && f.engine.read32(port) == self, "TASK_KERNEL_PORT");
    // In-process bootstrap registry.
    const auto service = f.text("org.alchemy.mach-test");
    require(f.status(sys, "_bootstrap_look_up", {bootstrap, service, port}) == 1102, "unknown before check-in");
    const auto receive = f.block(4);
    require(f.status(sys, "_bootstrap_check_in", {bootstrap, service, receive}) == 0, "bootstrap_check_in");
    require(f.status(sys, "_bootstrap_check_in", {bootstrap, service, port}) == 1103, "BOOTSTRAP_SERVICE_ACTIVE");
    require(f.status(sys, "_bootstrap_look_up", {bootstrap, service, port}) == 0 && f.engine.read32(port) == f.engine.read32(receive),
            "look-up returns a send right to the service (same name in this task)");
    require(f.status(sys, "_bootstrap_look_up", {0, service, port}) == static_cast<std::int32_t>(mr::send_invalid_dest), "null bootstrap port");
    require(f.status(sys, "_bootstrap_parent", {bootstrap, port}) == 1100, "bootstrap_parent: not privileged");
    // Special ports keep send rights.
    const auto name = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0, "port");
    const auto special = f.engine.read32(name);
    require(f.status(sys, "_mach_port_insert_right", {self, special, special, disp::make_send}) == 0, "send right");
    require(f.status(sys, "_task_set_special_port", {self, 9, special}) == 0, "TASK_ACCESS_PORT");
    require(f.status(sys, "_task_get_special_port", {self, 9, port}) == 0 && f.engine.read32(port) == special, "read back");
    // task_threads lists this thread.
    const auto list = f.block(8), count = f.block(4);
    require(f.status(sys, "_task_threads", {self, list, count}) == 0 && f.engine.read32(count) >= 1, "task_threads");
    bool found = false;
    for (std::uint32_t index = 0; index < f.engine.read32(count); ++index)
        found = found || f.engine.read32(f.engine.read64(list) + index * 4ULL) == static_cast<std::uint32_t>(f.call(sys, "_mach_thread_self"));
    require(found, "mach_thread_self is in task_threads");
    require(f.status(sys, "_vm_deallocate", {self, f.engine.read64(list), std::max<Address>(f.engine.read32(count), 1) * 4}) == 0,
            "the thread list is vm_allocate memory");
    // Identity tokens.
    const auto token = f.block(4);
    require(f.status(sys, "_task_create_identity_token", {self, token}) == 0, "task_create_identity_token");
    require(f.status(sys, "_task_identity_token_get_task_port", {f.engine.read32(token), 0, port}) == 0 && f.engine.read32(port) == self,
            "identity token -> task port");
    require(f.status(sys, "_task_identity_token_get_task_port", {special, 0, port}) == static_cast<std::int32_t>(kern::invalid_argument),
            "not a token");
}

// task_policy_set/get, thread_policy_set/get and task_info on this task.
void exports_task_policies(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto info = f.block(64 * 4), count = f.block(4), fallback = f.block(4);
    const auto get = [&](std::uint32_t flavor, std::uint32_t capacity, bool want_default) {
        f.engine.write32(count, capacity);
        f.engine.write32(fallback, want_default ? 1 : 0);
        return f.status(sys, "_task_policy_get", {self, flavor, info, count, fallback});
    };
    // TASK_CATEGORY_POLICY: unspecified (the default) until set.
    require(get(1, 1, false) == 0 && f.engine.read32(info) == 0 && f.engine.read32(fallback) == 1, "category: TASK_UNSPECIFIED by default");
    f.engine.write32(info, 2);   // TASK_BACKGROUND_APPLICATION
    require(f.status(sys, "_task_policy_set", {self, 1, info, 1}) == 0, "set TASK_BACKGROUND_APPLICATION");
    require(get(1, 1, false) == 0 && f.engine.read32(info) == 2 && f.engine.read32(fallback) == 0 && f.engine.read32(count) == 1, "read back");
    require(get(1, 1, true) == 0 && f.engine.read32(info) == 0, "get_default asks for the default");
    f.engine.write32(info, 4);   // TASK_GRAPHICS_SERVER
    require(f.status(sys, "_task_policy_set", {self, 1, info, 1}) == static_cast<std::int32_t>(kern::invalid_argument), "privileged role refused");
    // TASK_SUPPRESSION_POLICY (16 integers), TASK_BASE_QOS_POLICY (latency, throughput tiers).
    for (std::uint32_t index = 0; index < 16; ++index) f.engine.write32(info + index * 4, index + 1);
    require(f.status(sys, "_task_policy_set", {self, 3, info, 16}) == 0, "suppression");
    require(f.status(sys, "_task_policy_set", {self, 3, info, 15}) == static_cast<std::int32_t>(kern::invalid_argument), "short suppression");
    require(get(3, 16, false) == 0 && f.engine.read32(info + 15 * 4) == 16 && f.engine.read32(count) == 16, "suppression read back");
    f.engine.write32(info, 0xff0006);   // LATENCY_QOS_TIER_5
    f.engine.write32(info + 4, 0xfe0006);   // THROUGHPUT_QOS_TIER_5
    require(f.status(sys, "_task_policy_set", {self, 8, info, 2}) == 0, "base QoS");
    require(get(8, 2, false) == 0 && f.engine.read32(info) == 0xff0006 && f.engine.read32(info + 4) == 0xfe0006, "base QoS read back");
    f.engine.write32(info, 7);
    require(f.status(sys, "_task_policy_set", {self, 8, info, 2}) == static_cast<std::int32_t>(kern::invalid_argument), "bad tier");
    require(f.status(sys, "_task_policy_set", {self, 77, info, 1}) == static_cast<std::int32_t>(kern::invalid_argument), "unknown flavor");
    require(f.status(sys, "_task_policy_set", {0, 1, info, 1}) == static_cast<std::int32_t>(mr::send_invalid_dest), "null task");
    // thread_policy: stored per thread, defaults otherwise.
    const auto thread = static_cast<std::uint32_t>(f.call(sys, "_mach_thread_self"));
    const auto thread_get = [&](std::uint32_t flavor, std::uint32_t capacity) {
        f.engine.write32(count, capacity);
        f.engine.write32(fallback, 0);
        return f.status(sys, "_thread_policy_get", {thread, flavor, info, count, fallback});
    };
    require(thread_get(3, 1) == 0 && f.engine.read32(info) == 0 && f.engine.read32(fallback) == 1, "precedence: default 0");
    f.engine.write32(info, 37);
    require(f.status(sys, "_thread_policy_set", {thread, 3, info, 1}) == 0, "set precedence");
    require(thread_get(3, 1) == 0 && f.engine.read32(info) == 37 && f.engine.read32(fallback) == 0, "precedence read back");
    f.engine.write32(info, 1000000); f.engine.write32(info + 4, 500000); f.engine.write32(info + 8, 1000000); f.engine.write32(info + 12, 1);
    require(f.status(sys, "_thread_policy_set", {thread, 2, info, 4}) == 0, "time constraint");
    require(thread_get(2, 4) == 0 && f.engine.read32(info + 4) == 500000 && f.engine.read32(count) == 4, "time constraint read back");
    require(thread_get(1, 1) == 0 && f.engine.read32(info) == 0, "real time is not timeshare");
    require(f.status(sys, "_thread_policy_set", {thread, 0, info, 0}) == 0 && thread_get(3, 1) == 0 && f.engine.read32(info) == 0,
            "THREAD_STANDARD_POLICY resets");
    require(f.status(sys, "_thread_policy_set", {thread, 4, info, 1}) == static_cast<std::int32_t>(kern::not_supported), "affinity: not supported");
    require(f.status(sys, "_thread_policy_set", {0x7777, 3, info, 1}) == static_cast<std::int32_t>(kern::invalid_argument), "unknown thread");
    // task_info: TASK_POWER_INFO and TASK_BASIC_INFO_64.
    f.engine.write32(count, 12);
    require(f.status(sys, "_task_info", {self, 21, info, count}) == 0 && f.engine.read32(count) == 12, "TASK_POWER_INFO");
    f.engine.write32(count, 10);
    require(f.status(sys, "_task_info", {self, 5, info, count}) == 0 && f.engine.read64(info + 12) > 0, "TASK_BASIC_INFO_64 resident size");
    f.engine.write32(count, 11);
    require(f.status(sys, "_task_info", {self, 21, info, count}) == static_cast<std::int32_t>(kern::invalid_argument), "short TASK_POWER_INFO");
}

void exports_memory_entries(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto size = f.block(8), handle = f.block(4);
    f.engine.write64(size, 5000);
    const auto made = f.status(sys, "_mach_make_memory_entry_64", {self, size, 0, 0x020000 | 3, handle, 0});
    if (made == static_cast<std::int32_t>(kern::not_supported)) {
        std::cout << "  (engine without shared guest memory: memory entries fail explicitly)" << std::endl;
        return;
    }
    require(made == 0 && f.engine.read64(size) == 16384 && f.engine.read32(handle) != 0, "MAP_MEM_NAMED_CREATE");
    const auto entry = f.engine.read32(handle);
    // Two shared mappings of the entry.
    const auto map = [&](Address address_slot, std::uint32_t current, std::uint32_t maximum) {
        auto regs = registers({self, address_slot, 16384, 0, 1, entry, 0, 0});
        regs.push_stack(current);
        regs.push_stack(maximum);
        regs.push_stack(std::uint32_t{2});   // VM_INHERIT_NONE
        return static_cast<std::int32_t>(f.regs(sys, "_mach_vm_map", regs).x0);
    };
    const auto first = f.block(8), second = f.block(8), readonly = f.block(8);
    require(map(first, 3, 3) == 0 && map(second, 3, 3) == 0, "mach_vm_map twice");
    const auto a = f.engine.read64(first), b = f.engine.read64(second);
    require(a && b && a != b, "two addresses");
    f.engine.write32(a + 100, 0x5eed);
    require(f.engine.read32(b + 100) == 0x5eed, "a write through one mapping is seen through the other");
    // A read-only entry from the mapping (base::PlatformSharedMemoryRegion's ConvertToReadOnly).
    f.engine.write64(size, 16384);
    require(f.status(sys, "_mach_make_memory_entry_64", {self, size, a, 1, handle, 0}) == 0, "entry over a mapping");
    const auto read_entry = f.engine.read32(handle);
    auto regs = registers({self, readonly, 16384, 0, 1, read_entry, 0, 0});
    regs.push_stack(std::uint32_t{3});
    regs.push_stack(std::uint32_t{3});
    regs.push_stack(std::uint32_t{2});
    require(static_cast<std::uint32_t>(f.regs(sys, "_mach_vm_map", regs).x0) == kern::invalid_right, "write access beyond the entry");
    regs = registers({self, readonly, 16384, 0, 1, read_entry, 0, 0});
    regs.push_stack(std::uint32_t{1});
    regs.push_stack(std::uint32_t{1});
    regs.push_stack(std::uint32_t{2});
    require(f.regs(sys, "_mach_vm_map", regs).x0 == 0, "read-only mapping");
    const auto c = f.engine.read64(readonly);
    f.engine.write32(b + 200, 0xc0ffee);
    require(f.engine.read32(c + 200) == 0xc0ffee, "the read-only view shares the pages");
    const auto region = f.engine.query_region(c);
    require(region && !region->writable, "and is read-only");
    // Unmapping one view and releasing the entries keeps the others.
    require(f.status(sys, "_vm_deallocate", {self, a, 16384}) == 0, "vm_deallocate one view");
    require(f.engine.read32(b + 100) == 0x5eed, "the other view still has the data");
    require(f.status(sys, "_mach_port_deallocate", {self, entry}) == 0 && f.status(sys, "_mach_port_deallocate", {self, read_entry}) == 0,
            "release the entries");
    require(f.engine.read32(b + 200) == 0xc0ffee, "mappings outlive their entries");
    // Private memory (a whole page, so the range is mapped) cannot become a memory entry.
    // mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0)
    const auto page = f.call(sys, "_mmap", {0, 16384, 3, 0x1002, static_cast<Address>(-1), 0});
    require(page && page != static_cast<Address>(-1), "private anonymous mmap");
    f.engine.write64(size, 16384);
    const auto private_result = f.status(sys, "_mach_make_memory_entry_64", {self, size, page, 1, handle, 0});
    if (private_result != static_cast<std::int32_t>(kern::not_supported))
        throw std::runtime_error("private memory: KERN_NOT_SUPPORTED expected, got " + std::to_string(private_result));
    // Unmap what the test mapped (teardown with live memory: teardown-* cases).
    require(f.status(sys, "_vm_deallocate", {self, b, 16384}) == 0 && f.status(sys, "_vm_deallocate", {self, c, 16384}) == 0,
            "release the remaining views");
    static_cast<void>(f.call(sys, "_munmap", {page, 16384}));
}


void exports_kevent_clear(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto name = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0, "port");
    const auto port = f.engine.read32(name);
    const auto kq = f.call(sys, "_kqueue");
    const auto changes = f.block(48), events = f.block(48);
    // EVFILT_MACHPORT detection with EV_CLEAR (crashpad's child port handshake).
    f.engine.write64(changes, port);
    write16(f.engine, changes + 8, static_cast<std::uint16_t>(-8));
    write16(f.engine, changes + 10, 1 | 0x20);   // EV_ADD | EV_CLEAR
    require(f.status(sys, "_kevent64", {kq, changes, 1, 0, 0, 0, 0}) == 0, "EV_ADD | EV_CLEAR accepted");
    const auto message = f.block(32);
    const auto send = [&] {
        f.engine.write32(message, disp::make_send);
        f.engine.write32(message + 4, 24);
        f.engine.write32(message + 8, port);
        f.engine.write32(message + 20, 5);
        return f.status(sys, "_mach_msg", {message, option::send_msg, 24, 0, 0, 0, 0});
    };
    require(send() == 0, "first message");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 1, 1, 0}) == 1 && f.engine.read64(events + 16) == port &&
            (read16(f.engine, events + 10) & 0x20), "fires once, EV_CLEAR reported");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 1, 1, 0}) == 0, "the same queued message does not fire again");
    require(send() == 0, "second message");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 1, 1, 0}) == 1, "a newer message fires");
    // EVFILT_READ on a pipe with EV_CLEAR: more data or EOF fire again.
    const auto fds = f.block(8);
    require(f.status(sys, "_pipe", {fds}) == 0, "pipe");
    const auto reader = f.engine.read32(fds), writer = f.engine.read32(fds + 4);
    f.engine.write64(changes, reader);
    write16(f.engine, changes + 8, static_cast<std::uint16_t>(-1));
    write16(f.engine, changes + 10, 1 | 0x20);
    require(f.status(sys, "_kevent64", {kq, changes, 1, 0, 0, 0, 0}) == 0, "EVFILT_READ EV_CLEAR");
    const auto bytes = f.text("ab");
    const auto read_event = [&]() -> std::int64_t {
        const auto count = f.status(sys, "_kevent64", {kq, 0, 0, events, 1, 1, 0});
        if (count != 1) return -1;
        return f.engine.read64(events) == reader ? static_cast<std::int64_t>(f.engine.read64(events + 16)) : -2;
    };
    require(read_event() == -1, "empty pipe: nothing");
    require(f.call(sys, "_write", {writer, bytes, 1}) == 1 && read_event() == 1, "one byte: fires with data 1");
    require(read_event() == -1, "no change: quiet");
    require(f.call(sys, "_write", {writer, bytes, 2}) == 2 && read_event() == 3, "more data: fires again");
    static_cast<void>(f.call(sys, "_close", {writer}));
    // Windows pipes report the closed writer once drained (EOF after the data).
    require(f.call(sys, "_read", {reader, f.block(8), 8}) == 3, "drain");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 1, 1, 0}) == 1 && (read16(f.engine, events + 10) & 0x8000), "EOF fires (EV_EOF)");
    require(read_event() == -1, "EOF reported once");
    static_cast<void>(f.call(sys, "_close", {reader}));
    // EVFILT_WRITE on a socketpair used one way (crashpad's handshake): the
    // peer's SHUT_WR ends this side's reading, not its writing; a local
    // SHUT_WR is EOF for writing.
    const auto pair = f.block(8);
    require(f.status(sys, "_socketpair", {1, 1, 0, pair}) == 0, "socketpair");
    const auto writer_end = f.engine.read32(pair), reader_end = f.engine.read32(pair + 4);
    require(f.status(sys, "_shutdown", {reader_end, 1}) == 0 && f.status(sys, "_shutdown", {writer_end, 0}) == 0, "one-way shutdowns");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // the FIN arrives
    f.engine.write64(changes, writer_end);
    write16(f.engine, changes + 8, static_cast<std::uint16_t>(-2));
    write16(f.engine, changes + 10, 1 | 0x20);
    require(f.status(sys, "_kevent64", {kq, changes, 1, 0, 0, 0, 0}) == 0, "EVFILT_WRITE EV_CLEAR");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 1, 1, 0}) == 1 && f.engine.read64(events) == writer_end &&
            !(read16(f.engine, events + 10) & 0x8000), "writable, no EV_EOF after the peer's SHUT_WR");
    require(f.status(sys, "_shutdown", {writer_end, 1}) == 0, "local SHUT_WR");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 1, 1, 0}) == 1 && (read16(f.engine, events + 10) & 0x8000),
            "local SHUT_WR: EV_EOF");
    static_cast<void>(f.call(sys, "_close", {writer_end}));
    static_cast<void>(f.call(sys, "_close", {reader_end}));
    static_cast<void>(f.call(sys, "_close", {kq}));
}

std::atomic<int> receive_events{0}, dead_events{0};

void exports_dispatch_sources(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto receive_handler = f.process.install_function("_alchemy_test_mach_recv",
        [](transmute::CpuState&, transmute::GuestMemory&) { ++receive_events; });
    const auto dead_handler = f.process.install_function("_alchemy_test_mach_dead",
        [](transmute::CpuState&, transmute::GuestMemory&) { ++dead_events; });
    const auto name = f.block(4);
    require(f.status(sys, "_mach_port_allocate", {self, 1, name}) == 0, "port");
    const auto port = f.engine.read32(name);
    const auto queue = f.call(sys, "_dispatch_get_global_queue", {0, 0});
    const auto recv_type = f.symbol(sys, "__dispatch_source_type_mach_recv");
    const auto send_type = f.symbol(sys, "__dispatch_source_type_mach_send");
    require(f.call(sys, "_dispatch_source_create", {recv_type, 0, 0, queue}) == 0, "MACH_RECV needs a port");
    require(f.call(sys, "_dispatch_source_create", {recv_type, port, 1, queue}) == 0, "MACH_RECV takes no mask");
    require(f.call(sys, "_dispatch_source_create", {send_type, port, 2, queue}) == 0, "MACH_SEND: DISPATCH_MACH_SEND_DEAD only");
    const auto source = f.call(sys, "_dispatch_source_create", {recv_type, port, 0, queue});
    require(source != 0 && f.call(sys, "_dispatch_source_get_handle", {source}) == port, "MACH_RECV source");
    static_cast<void>(f.call(sys, "_dispatch_source_set_event_handler_f", {source, receive_handler}));
    static_cast<void>(f.call(sys, "_dispatch_resume", {source}));
    const auto wait_for = [](std::atomic<int>& counter, int at_least) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < at_least && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return counter.load() >= at_least;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(receive_events == 0, "no message: no event");
    const auto message = f.block(64);
    f.engine.write32(message, disp::make_send);
    f.engine.write32(message + 4, 24);
    f.engine.write32(message + 8, port);
    f.engine.write32(message + 20, 9);
    require(f.status(sys, "_mach_msg", {message, option::send_msg, 24, 0, 0, 0, 0}) == 0, "send");
    require(wait_for(receive_events, 1), "the handler ran for the queued message");
    require(wait_for(receive_events, 3), "level-triggered: the unreceived message keeps the source pending");
    const auto buffer = f.block(128);
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | option::rcv_timeout, 0, 128, port, 0, 0}) == 0, "receive it");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto settled = receive_events.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(receive_events.load() == settled, "quiet once the queue is empty");
    static_cast<void>(f.call(sys, "_dispatch_source_cancel", {source}));
    // MACH_SEND with DISPATCH_MACH_SEND_DEAD: fires once when the name dies.
    require(f.status(sys, "_mach_port_insert_right", {self, port, port, disp::make_send}) == 0, "send right");
    const auto watcher = f.call(sys, "_dispatch_source_create", {send_type, port, 1, queue});
    require(watcher != 0, "MACH_SEND source");
    static_cast<void>(f.call(sys, "_dispatch_source_set_event_handler_f", {watcher, dead_handler}));
    static_cast<void>(f.call(sys, "_dispatch_resume", {watcher}));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(dead_events == 0, "alive: no event");
    require(f.status(sys, "_mach_port_mod_refs", {self, port, 1, static_cast<Address>(-1)}) == 0, "destroy the receive right");
    require(wait_for(dead_events, 1), "DISPATCH_MACH_SEND_DEAD delivered");
    require(f.call(sys, "_dispatch_source_get_data", {watcher}) == 1, "data is DISPATCH_MACH_SEND_DEAD");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    require(dead_events == 1, "once");
    static_cast<void>(f.call(sys, "_dispatch_source_cancel", {watcher}));
    require(f.status(sys, "_mach_port_deallocate", {self, port}) == 0, "release the dead name");
}


std::atomic<int> pressure_events{0};
std::atomic<int> notify_events{0};
std::atomic<std::uint64_t> notify_last_token{0};

void set_pressure(const char* level) {
#ifdef _WIN32
    _putenv_s("ALCHEMY_MEMORY_PRESSURE", level);
#else
    setenv("ALCHEMY_MEMORY_PRESSURE", level, 1);
#endif
}

void exports_memory_pressure_and_proc(Fixture& f) {
    const auto handler = f.process.install_function("_alchemy_test_pressure",
        [](transmute::CpuState&, transmute::GuestMemory&) { ++pressure_events; });
    const auto queue = f.call(sys, "_dispatch_get_global_queue", {0, 0});
    const auto type = f.symbol(sys, "__dispatch_source_type_memorypressure");
    require(f.call(sys, "_dispatch_source_create", {type, 0, 0x100, queue}) == 0, "unknown memory-pressure mask: NULL");
    require(f.call(sys, "_dispatch_source_create", {type, 1, 0x7, queue}) == 0, "memory pressure takes no handle");
    set_pressure("normal");
    const auto source = f.call(sys, "_dispatch_source_create", {type, 0, 0x2 | 0x4, queue});   // WARN | CRITICAL
    require(source != 0, "DISPATCH_SOURCE_TYPE_MEMORYPRESSURE");
    static_cast<void>(f.call(sys, "_dispatch_source_set_event_handler_f", {source, handler}));
    static_cast<void>(f.call(sys, "_dispatch_resume", {source}));
    const auto wait_for = [](int at_least) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (pressure_events.load() < at_least && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return pressure_events.load() >= at_least;
    };
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    require(pressure_events == 0, "the starting level is not an event");
    set_pressure("critical");
    require(wait_for(1) && f.call(sys, "_dispatch_source_get_data", {source}) == 4, "CRITICAL delivered with its level");
    set_pressure("warn");
    require(wait_for(2) && f.call(sys, "_dispatch_source_get_data", {source}) == 2, "WARN delivered");
    set_pressure("normal");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    require(pressure_events == 2, "NORMAL is outside the mask");
    static_cast<void>(f.call(sys, "_dispatch_source_cancel", {source}));
    set_pressure("");
    // PROC: masks are checked; a pid that is not a child is cancelled at once.
    const auto proc = f.symbol(sys, "__dispatch_source_type_proc");
    require(f.call(sys, "_dispatch_source_create", {proc, 0, 0x80000000ULL, queue}) == 0, "PROC needs a pid");
    require(f.call(sys, "_dispatch_source_create", {proc, 4242, 0x1, queue}) == 0, "unknown PROC mask: NULL");
    const auto stranger = f.call(sys, "_dispatch_source_create", {proc, 999999, 0x80000000ULL, queue});
    require(stranger != 0, "PROC source for another pid");
    const auto canceled = f.process.install_function("_alchemy_test_proc_cancel",
        [](transmute::CpuState&, transmute::GuestMemory&) { pressure_events += 100; });
    static_cast<void>(f.call(sys, "_dispatch_source_set_cancel_handler_f", {stranger, canceled}));
    static_cast<void>(f.call(sys, "_dispatch_resume", {stranger}));
    require(wait_for(102) && f.call(sys, "_dispatch_source_testcancel", {stranger}) != 0, "not a child: cancelled");
}

void exports_notify(Fixture& f) {
    const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
    const auto token = f.block(4), check = f.block(4), state = f.block(8);
    const auto name = f.text("org.alchemy.test.notify");
    // Check delivery: true on the first check, then only after a post.
    require(f.status(sys, "_notify_register_check", {name, token}) == 0, "notify_register_check");
    const auto checked = f.engine.read32(token);
    require(f.call(sys, "_notify_is_valid_token", {checked}) == 1, "valid token");
    require(f.status(sys, "_notify_check", {checked, check}) == 0 && f.engine.read32(check) == 1, "first check is true");
    require(f.status(sys, "_notify_check", {checked, check}) == 0 && f.engine.read32(check) == 0, "then false");
    // State is per name.
    require(f.status(sys, "_notify_set_state", {checked, 0x1234567890ULL}) == 0, "notify_set_state");
    // Dispatch delivery through a global block whose invoke records the token.
    const auto invoke = f.process.install_function("_alchemy_test_notify_block",
        [](transmute::CpuState& s, transmute::GuestMemory&) { notify_last_token = s.x[1]; ++notify_events; });
    const auto descriptor = f.block(16);
    f.engine.write64(descriptor + 8, 32);
    const auto block = f.block(32);
    f.engine.write64(block, f.symbol(sys, "__NSConcreteGlobalBlock"));
    f.engine.write32(block + 8, 1U << 28);   // BLOCK_IS_GLOBAL
    f.engine.write64(block + 16, invoke);
    f.engine.write64(block + 24, descriptor);
    const auto queue = f.call(sys, "_dispatch_get_global_queue", {0, 0});
    const auto dispatch_token = f.block(4);
    require(f.status(sys, "_notify_register_dispatch", {name, dispatch_token, queue, block}) == 0, "notify_register_dispatch");
    // Mach port delivery: a new receive right; each post is a message with msgh_id = token.
    const auto port = f.block(4), port_token = f.block(4);
    require(f.status(sys, "_notify_register_mach_port", {name, port, 0, port_token}) == 0, "notify_register_mach_port");
    // File descriptor delivery: the token in network byte order.
    const auto fd = f.block(4), fd_token = f.block(4);
    require(f.status(sys, "_notify_register_file_descriptor", {name, fd, 0, fd_token}) == 0, "notify_register_file_descriptor");
    require(f.status(sys, "_notify_post", {name}) == 0, "notify_post");
    require(f.status(sys, "_notify_check", {checked, check}) == 0 && f.engine.read32(check) == 1, "posted: check is true");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!notify_events && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    require(notify_events == 1 && notify_last_token == f.engine.read32(dispatch_token), "the dispatch handler got its token");
    const auto buffer = f.block(64);
    require(f.status(sys, "_mach_msg", {buffer, option::rcv_msg | option::rcv_timeout, 0, 64, f.engine.read32(port), 1000, 0}) == 0 &&
            f.engine.read32(buffer + 20) == f.engine.read32(port_token), "the Mach message carries the token");
    const auto bytes = f.block(4);
    require(f.call(sys, "_read", {f.engine.read32(fd), bytes, 4}) == 4, "the descriptor got 4 bytes");
    const auto expected = f.engine.read32(fd_token);
    require(f.memory().read8(bytes + 3) == (expected & 0xff) && f.memory().read8(bytes) == (expected >> 24), "network byte order");
    require(f.status(sys, "_notify_get_state", {f.engine.read32(port_token), state}) == 0 && f.engine.read64(state) == 0x1234567890ULL,
            "state shared by the name's tokens");
    // Cancel.
    for (const auto cancelled : {checked, f.engine.read32(dispatch_token), f.engine.read32(port_token), f.engine.read32(fd_token)})
        require(f.status(sys, "_notify_cancel", {cancelled}) == 0, "notify_cancel");
    require(f.call(sys, "_notify_is_valid_token", {checked}) == 0, "cancelled token is invalid");
    require(f.status(sys, "_notify_cancel", {checked}) == 2, "NOTIFY_STATUS_INVALID_TOKEN");
    require(f.status(sys, "_notify_post", {name}) == 0, "posting with no registration");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(notify_events == 1, "no delivery after cancel");
    require(f.status(sys, "_mach_port_mod_refs", {self, f.engine.read32(port), 1, static_cast<Address>(-1)}) == 0, "the port stays the caller's");
    static_cast<void>(f.call(sys, "_close", {f.engine.read32(fd)}));
}

// notify_suspend holds a token's posts and notify_resume delivers them once; signal registrations.
std::atomic<int> suspended_events{0};
void exports_notify_suspend(Fixture& f) {
    const auto name = f.text("org.alchemy.test.notify.suspend");
    const auto invoke = f.process.install_function("_alchemy_test_notify_suspend_block",
        [](transmute::CpuState&, transmute::GuestMemory&) { ++suspended_events; });
    const auto descriptor = f.block(16);
    f.engine.write64(descriptor + 8, 32);
    const auto block = f.block(32);
    f.engine.write64(block, f.symbol(sys, "__NSConcreteGlobalBlock"));
    f.engine.write32(block + 8, 1U << 28);   // BLOCK_IS_GLOBAL
    f.engine.write64(block + 16, invoke);
    f.engine.write64(block + 24, descriptor);
    const auto queue = f.call(sys, "_dispatch_get_global_queue", {0, 0});
    const auto token = f.block(4);
    require(f.status(sys, "_notify_register_dispatch", {name, token, queue, block}) == 0, "notify_register_dispatch");
    const auto value = f.engine.read32(token);
    require(f.status(sys, "_notify_suspend", {value}) == 0, "notify_suspend");
    require(f.status(sys, "_notify_post", {name}) == 0 && f.status(sys, "_notify_post", {name}) == 0, "two posts");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(suspended_events == 0, "nothing is delivered while suspended");
    require(f.status(sys, "_notify_resume", {value}) == 0, "notify_resume");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!suspended_events && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    require(suspended_events == 1, "the held posts arrive once");
    require(f.status(sys, "_notify_suspend", {0x7fffffff}) == 2, "an unknown token");
    require(f.status(sys, "_notify_cancel", {value}) == 0, "notify_cancel");
    // Signal registrations: a valid signal gives a token; 0 is an invalid request.
    const auto signal_token = f.block(4);
    require(f.status(sys, "_notify_register_signal", {name, 0, signal_token}) == 6, "signal 0 is refused");
    require(f.status(sys, "_notify_register_signal", {name, 30, signal_token}) == 0 &&
            f.call(sys, "_notify_is_valid_token", {f.engine.read32(signal_token)}) == 1, "notify_register_signal");
    require(f.status(sys, "_notify_cancel", {f.engine.read32(signal_token)}) == 0, "cancel the signal registration");
    // A workgroup's port names it: a workgroup made from the port shares its cancellation.
    const auto group = f.call(sys, "_os_workgroup_interval_create", {f.text("render"), 32, 0});
    const auto port = f.block(4);
    require(group && f.status(sys, "_os_workgroup_copy_port", {group, port}) == 0 && f.engine.read32(port) != 0, "os_workgroup_copy_port");
    const auto twin = f.call(sys, "_os_workgroup_create_with_port", {f.text("twin"), f.engine.read32(port)});
    require(twin != 0 && f.call(sys, "_os_workgroup_create_with_port", {f.text("none"), 0x7777}) == 0, "os_workgroup_create_with_port");
    f.call(sys, "_os_workgroup_cancel", {group});
    require(f.call(sys, "_os_workgroup_testcancel", {twin}) == 1, "the port's workgroups are one group");
}

// A fresh guest directory under $TMPDIR.
std::string temporary_directory(Fixture& f) {
    const auto base = f.read_text(f.call(sys, "_getenv", {f.text("TMPDIR")}));
    const auto path = f.text((base.empty() ? std::string("/tmp") : base) + (base.ends_with("/") ? "" : "/") + "alchemy-vnode-XXXXXX");
    require(f.call(sys, "_mkdtemp", {path}) == path, "mkdtemp");
    return f.read_text(path);
}

void exports_vnode(Fixture& f) {
    constexpr std::uint32_t note_delete = 1, note_write = 2, note_extend = 4, note_attrib = 8, note_link = 0x10, note_rename = 0x20;
    const auto directory = temporary_directory(f);
    const auto file = directory + "/watched.txt";
    const auto writer = f.status(sys, "_open", {f.text(file), 0x1 | 0x200, 0644});
    require(writer >= 0, "create the file");
    const auto watched = f.status(sys, "_open", {f.text(file), 0x8000});   // O_EVTONLY
    require(watched >= 0, "O_EVTONLY is accepted");
    const auto folder = f.status(sys, "_open", {f.text(directory), 0x8000});
    require(folder >= 0, "O_EVTONLY opens a directory");
    const auto kq = f.call(sys, "_kqueue");
    const auto changes = f.block(96), events = f.block(96), timeout = f.block(16);
    const auto add = [&](std::int32_t fd, std::uint32_t notes, std::uint64_t at) {
        f.engine.write64(changes + at, static_cast<std::uint32_t>(fd));
        write16(f.engine, changes + at + 8, static_cast<std::uint16_t>(-4));
        write16(f.engine, changes + at + 10, 1 | 0x20);   // EV_ADD | EV_CLEAR
        f.engine.write32(changes + at + 12, notes);
        f.engine.write64(changes + at + 24, static_cast<std::uint32_t>(fd));
    };
    add(watched, note_delete | note_write | note_extend | note_attrib | note_link | note_rename, 0);
    add(folder, note_write | note_link | note_delete, 48);
    require(f.status(sys, "_kevent64", {kq, changes, 2, 0, 0, 0, 0}) == 0, "EVFILT_VNODE registration");
    // Collects the notes of `fd` for up to two seconds.
    const auto notes_of = [&](std::int32_t fd, std::uint32_t wanted) {
        std::uint32_t seen = 0;
        f.engine.write64(timeout, 0);
        f.engine.write64(timeout + 8, 50'000'000);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while ((seen & wanted) != wanted && std::chrono::steady_clock::now() < until) {
            const auto count = f.status(sys, "_kevent64", {kq, 0, 0, events, 2, 0, timeout});
            for (std::int32_t index = 0; index < count; ++index)
                if (f.engine.read64(events + index * 48ULL) == static_cast<std::uint32_t>(fd)) seen |= f.engine.read32(events + index * 48ULL + 12);
        }
        return seen;
    };
    f.engine.write64(timeout, 0);
    f.engine.write64(timeout + 8, 100'000'000);
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 2, 0, timeout}) == 0, "nothing changed yet");
    require(f.call(sys, "_write", {static_cast<std::uint32_t>(writer), f.text("hello"), 5}) == 5, "write");
    auto seen = notes_of(watched, note_write | note_extend);
    require((seen & (note_write | note_extend)) == (note_write | note_extend), "NOTE_WRITE | NOTE_EXTEND");
    require(f.status(sys, "_kevent64", {kq, 0, 0, events, 2, 0, timeout}) == 0, "EV_CLEAR: reported once");
    // A new entry in the directory, then a subdirectory.
    const auto other = f.status(sys, "_open", {f.text(directory + "/other.txt"), 0x1 | 0x200, 0644});
    require(other >= 0 && f.status(sys, "_close", {static_cast<std::uint32_t>(other)}) == 0, "create another file");
    require(notes_of(folder, note_write) & note_write, "the directory reports NOTE_WRITE");
    require(f.status(sys, "_mkdir", {f.text(directory + "/sub"), 0755}) == 0, "mkdir");
    seen = notes_of(folder, note_write | note_link);
    require((seen & (note_write | note_link)) == (note_write | note_link), "a subdirectory: NOTE_WRITE | NOTE_LINK");
    // Rename, then delete the watched file.
    require(f.status(sys, "_rename", {f.text(file), f.text(directory + "/renamed.txt")}) == 0, "rename");
    require(notes_of(watched, note_rename) & note_rename, "NOTE_RENAME");
    require(f.status(sys, "_close", {static_cast<std::uint32_t>(writer)}) == 0, "close the writer");
    require(f.status(sys, "_unlink", {f.text(directory + "/renamed.txt")}) == 0, "unlink");
    require(notes_of(watched, note_delete) & note_delete, "NOTE_DELETE");
    // A pipe cannot be watched as a vnode.
    const auto fds = f.block(8);
    require(f.status(sys, "_pipe", {fds}) == 0, "pipe");
    add(static_cast<std::int32_t>(f.engine.read32(fds)), note_write, 0);
    write16(f.engine, changes + 10, 1 | 0x40);   // EV_ADD | EV_RECEIPT
    require(f.status(sys, "_kevent64", {kq, changes, 1, events, 1, 0, 0}) == 1 && f.engine.read64(events + 16) == 22, "a pipe: EINVAL");
    for (const auto fd : {watched, folder, static_cast<std::int32_t>(f.engine.read32(fds)), static_cast<std::int32_t>(f.engine.read32(fds + 4)),
                          static_cast<std::int32_t>(kq)})
        static_cast<void>(f.call(sys, "_close", {static_cast<std::uint32_t>(fd)}));
    static_cast<void>(f.call(sys, "_unlink", {f.text(directory + "/other.txt")}));
    static_cast<void>(f.call(sys, "_rmdir", {f.text(directory + "/sub")}));
    static_cast<void>(f.call(sys, "_rmdir", {f.text(directory)}));
}

std::atomic<int> vnode_events{0}, read_events{0};

void exports_dispatch_file_sources(Fixture& f) {
    const auto directory = temporary_directory(f);
    const auto file = directory + "/source.txt";
    const auto writer = f.status(sys, "_open", {f.text(file), 0x1 | 0x200, 0644});
    const auto watched = f.status(sys, "_open", {f.text(file), 0x8000});
    require(writer >= 0 && watched >= 0, "files");
    const auto queue = f.call(sys, "_dispatch_get_global_queue", {0, 0});
    const auto vnode_type = f.symbol(sys, "__dispatch_source_type_vnode");
    const auto read_type = f.symbol(sys, "__dispatch_source_type_read");
    require(f.call(sys, "_dispatch_source_create", {vnode_type, static_cast<std::uint32_t>(watched), 0x400, queue}) == 0, "unknown VNODE mask: NULL");
    require(f.call(sys, "_dispatch_source_create", {read_type, static_cast<std::uint32_t>(watched), 1, queue}) == 0, "READ takes no mask");
    const auto vnode_handler = f.process.install_function("_alchemy_test_vnode", [](transmute::CpuState&, transmute::GuestMemory&) { ++vnode_events; });
    const auto read_handler = f.process.install_function("_alchemy_test_read", [](transmute::CpuState&, transmute::GuestMemory&) { ++read_events; });
    const auto vnode = f.call(sys, "_dispatch_source_create", {vnode_type, static_cast<std::uint32_t>(watched), 0x2 | 0x4, queue});
    require(vnode != 0, "DISPATCH_SOURCE_TYPE_VNODE");
    static_cast<void>(f.call(sys, "_dispatch_source_set_event_handler_f", {vnode, vnode_handler}));
    static_cast<void>(f.call(sys, "_dispatch_resume", {vnode}));
    const auto wait_for = [](std::atomic<int>& counter, int at_least) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (counter.load() < at_least && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return counter.load() >= at_least;
    };
    require(f.call(sys, "_write", {static_cast<std::uint32_t>(writer), f.text("abc"), 3}) == 3, "write");
    require(wait_for(vnode_events, 1), "the VNODE handler ran");
    require((f.call(sys, "_dispatch_source_get_data", {vnode}) & 0x2) != 0, "data holds DISPATCH_VNODE_WRITE");
    static_cast<void>(f.call(sys, "_dispatch_source_cancel", {vnode}));
    // READ on a pipe: level-triggered until the data is read.
    const auto fds = f.block(8);
    require(f.status(sys, "_pipe", {fds}) == 0, "pipe");
    const auto reader = f.engine.read32(fds), pipe_writer = f.engine.read32(fds + 4);
    const auto source = f.call(sys, "_dispatch_source_create", {read_type, reader, 0, queue});
    require(source != 0, "DISPATCH_SOURCE_TYPE_READ");
    static_cast<void>(f.call(sys, "_dispatch_source_set_event_handler_f", {source, read_handler}));
    static_cast<void>(f.call(sys, "_dispatch_resume", {source}));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    require(read_events == 0, "empty pipe: no event");
    require(f.call(sys, "_write", {pipe_writer, f.text("xy"), 2}) == 2, "write to the pipe");
    require(wait_for(read_events, 1) && f.call(sys, "_dispatch_source_get_data", {source}) == 2, "READ with the readable byte count");
    require(wait_for(read_events, 3), "still readable: the handler runs again");
    require(f.call(sys, "_read", {reader, f.block(8), 8}) == 2, "drain");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto settled = read_events.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    require(read_events.load() == settled, "quiet once drained");
    static_cast<void>(f.call(sys, "_dispatch_source_cancel", {source}));
    // A closed descriptor: the source is cancelled at creation.
    const auto bad = f.call(sys, "_dispatch_source_create", {read_type, 999, 0, queue});
    require(bad != 0 && f.call(sys, "_dispatch_source_testcancel", {bad}) != 0, "bad descriptor: cancelled");
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    for (const auto fd : {static_cast<std::uint32_t>(writer), static_cast<std::uint32_t>(watched), reader, pipe_writer})
        static_cast<void>(f.call(sys, "_close", {fd}));
    static_cast<void>(f.call(sys, "_unlink", {f.text(file)}));
    static_cast<void>(f.call(sys, "_rmdir", {f.text(directory)}));
}


// ---- process teardown with live Mach VM state -------------------------------------
// `alchemy_gui_mach_ipc teardown-<case>`: the fixture is destroyed while
// memory stays mapped or a memory entry stays alive, as a guest process that
// exits without cleaning up (Chrome's Mojo shared buffers). It must end
// without a host fault.
#ifdef _WIN32
// Debug aid: a host fault prints its symbolized stack (dbghelp; names only
// when the binary has a PDB).
LONG WINAPI report_teardown_fault(EXCEPTION_POINTERS* pointers) {
    static std::atomic<bool> reported{false};
    if (reported.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
    const auto process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    auto context = *pointers->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp; frame.AddrStack.Mode = AddrModeFlat;
    std::fprintf(stderr, "teardown fault 0x%lx at 0x%llx\n", pointers->ExceptionRecord->ExceptionCode,
                 static_cast<unsigned long long>(context.Rip));
    for (int depth = 0; depth < 40 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                                                   SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++depth) {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512]{};
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement{};
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement{};
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol);
        const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line);
        std::fprintf(stderr, "  #%d 0x%llx %s %s:%lu\n", depth, static_cast<unsigned long long>(frame.AddrPC.Offset),
                     named ? symbol->Name : "?", lined ? line.FileName : "", lined ? line.LineNumber : 0UL);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

int teardown(std::string_view which) {
#ifdef _WIN32
    AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* pointers) -> LONG {
        const auto code = pointers->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION && std::getenv("ALCHEMY_TEARDOWN_TRACE")) return report_teardown_fault(pointers);
        return EXCEPTION_CONTINUE_SEARCH;
    });
#endif
    try {
        Fixture f;
        const auto self = f.engine.read32(f.symbol(sys, "_mach_task_self_"));
        if (which == "vm") {
            // Plain vm_allocate memory left mapped.
            const auto slot = f.block(8);
            require(f.status(sys, "_vm_allocate", {self, slot, 3 * 16384, 1}) == 0, "vm_allocate");
            f.engine.write32(f.engine.read64(slot), 7);
        } else if (which == "entry") {
            // A named entry whose mappings are gone but whose port is still alive.
            const auto size = f.block(8), handle = f.block(4), slot = f.block(8);
            f.engine.write64(size, 65536);
            require(f.status(sys, "_mach_make_memory_entry_64", {self, size, 0, 0x020000 | 3, handle, 0}) == 0, "named entry");
            auto regs = registers({self, slot, 65536, 0, 1, f.engine.read32(handle), 0, 0});
            regs.push_stack(std::uint32_t{3});
            regs.push_stack(std::uint32_t{3});
            regs.push_stack(std::uint32_t{2});
            require(f.regs(sys, "_mach_vm_map", regs).x0 == 0, "map it");
            f.engine.write32(f.engine.read64(slot), 9);
            require(f.status(sys, "_vm_deallocate", {self, f.engine.read64(slot), 65536}) == 0, "unmap it");
        } else if (which == "mapped-entry") {
            // A named entry and a live mapping of it.
            const auto size = f.block(8), handle = f.block(4), slot = f.block(8);
            f.engine.write64(size, 65536);
            require(f.status(sys, "_mach_make_memory_entry_64", {self, size, 0, 0x020000 | 3, handle, 0}) == 0, "named entry");
            auto regs = registers({self, slot, 65536, 0, 1, f.engine.read32(handle), 0, 0});
            regs.push_stack(std::uint32_t{3});
            regs.push_stack(std::uint32_t{3});
            regs.push_stack(std::uint32_t{2});
            require(f.regs(sys, "_mach_vm_map", regs).x0 == 0, "map it");
            f.engine.write32(f.engine.read64(slot), 9);
        } else {
            throw std::runtime_error("unknown teardown case " + std::string(which));
        }
        std::cout << "teardown " << which << ": state left alive, destroying the process" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "teardown " << which << ": ok" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]).starts_with("teardown-")) return teardown(std::string_view(argv[1]).substr(9));
    const std::vector<std::pair<const char*, std::function<void()>>> host{
        {"ipc_rights", ipc_rights}, {"ipc_messages", ipc_messages}, {"ipc_timeouts_and_limits", ipc_timeouts_and_limits},
        {"ipc_descriptors", ipc_descriptors}, {"ipc_notifications", ipc_notifications}, {"ipc_port_sets", ipc_port_sets},
        {"ipc_threads", ipc_threads}};
    std::string only;
    if (const char* value = std::getenv("FILL_TEST_ONLY")) only = std::string(",") + value + ",";
    try {
        for (const auto& [name, test] : host) {
            if (!only.empty() && only.find(std::string(",") + name + ",") == std::string::npos) continue;
            std::cout << "[chrome_mach] " << name << std::endl;
            test();
        }
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    return fill_test::run("chrome_mach", {
        {"exports_ports_and_messages", exports_ports_and_messages},
        {"exports_kevent", exports_kevent},
        {"exports_msg_server", exports_msg_server},
        {"exports_tasks_and_bootstrap", exports_tasks_and_bootstrap},
        {"exports_task_policies", exports_task_policies},
        {"exports_memory_entries", exports_memory_entries},
        {"exports_kevent_clear", exports_kevent_clear},
        {"exports_dispatch_sources", exports_dispatch_sources},
        {"exports_memory_pressure_and_proc", exports_memory_pressure_and_proc},
        {"exports_notify", exports_notify},
        {"exports_notify_suspend", exports_notify_suspend},
        {"exports_vnode", exports_vnode},
        {"exports_dispatch_file_sources", exports_dispatch_file_sources},
    });
}
