#include "alchemy/darwin/kit/completion/runtime/common.hpp"

#include <bit>
#include <cmath>
#include <map>
#include <random>

// C interfaces of libSystem, CoreFoundation, Foundation and ServiceManagement
// that QLab and its frameworks import (QLab group G2):
//   OSAtomicIncrement32/Decrement32(Barrier), OSAtomicTestAndSet/ClearBarrier,
//   OSAtomicEnqueue/Dequeue, OSSpinLockLock/Unlock/Try: atomic on guest memory
//   with respect to each other (one host lock serializes them; the emulated
//   threads are host threads).
//   asl_*: an in-process Apple System Log store: asl_send records messages,
//   asl_search matches them against queries (equal/not equal/ordering/true,
//   with casefold, prefix, suffix, substring and numeric modifiers; regular
//   expressions fail explicitly). Messages of other processes are not visible.
//   cfmakeraw, cfsetspeed (Darwin termios layout), mach_wait_until,
//   memset_pattern4, arc4random_uniform (rejection sampling over
//   arc4random), matrix_identity_float4x4.
//   CFAllocatorAllocate/Deallocate (the default, system and malloc allocators;
//   others fail explicitly), CFArrayExchangeValuesAtIndices,
//   CFStringCreateWithFormat (Foundation's format engine),
//   CFMessagePort*: ports local to this process (a remote port reaches a local
//   port of this process by name; ports of other processes are not visible,
//   so CFMessagePortCreateRemote answers NULL for them, as for a missing port);
//   a request runs the local port's callout on its dispatch queue (sync) or on
//   the sending thread when it has none.
//   NSAllMapTableKeys/Values, NSHomeDirectoryForUser, NSPoint/Size/RectFromString,
//   NSEdgeInsetsZero and the NSAppleScript/NSProgress string constants.
//   SMJobSubmit / SMJobRemove fail explicitly (no launchd); kSMDomain* constants.
namespace alchemy::gui::completion_kit {
namespace {

using namespace rt;

constexpr std::string_view service_management = "/System/Library/Frameworks/ServiceManagement.framework/Versions/A/ServiceManagement";

struct AslMessage { std::uint32_t type{}; std::vector<std::tuple<std::string, std::string, std::uint32_t>> fields; std::map<std::string, Address> strings; };
struct AslResponse { std::vector<Address> messages; std::size_t next{}; };

struct MessagePort final : ObjectPayload {
    std::string name;
    Address callout{}, info{}, queue{};   // queue: retained
    Address local{};                      // remote ports: the local port (retained)
    bool valid{true}, is_local{};
};

struct System {
    explicit System(Support& value) : support(value) {}
    Support& support;
    std::mutex atomic_mutex;
    std::mutex asl_mutex;
    std::map<Address, AslMessage> asl_messages;      // guest handle -> message (also queries)
    std::map<Address, AslResponse> asl_responses;
    std::vector<std::vector<std::tuple<std::string, std::string, std::uint32_t>>> asl_store;   // sent messages
    std::mutex port_mutex;
    std::map<std::string, Address> ports;            // name -> local port
    Address port_class{};

    // ---- ASL ----
    Address asl_handle() {
        const auto address = support.process.allocate_guest(16, 16);
        support.memory().fill_bytes(address, 0, 16);
        return address;
    }
    void asl_free_message(Address handle) {
        AslMessage message;
        {
            std::lock_guard lock(asl_mutex);
            const auto found = asl_messages.find(handle);
            if (found == asl_messages.end()) return;
            message = std::move(found->second);
            asl_messages.erase(found);
        }
        for (const auto& [key, value] : message.strings) { static_cast<void>(key); support.process.release_guest(value); }
        support.process.release_guest(handle);
    }
    static std::string lower(std::string value) {
        for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return value;
    }
    bool asl_match(const std::vector<std::tuple<std::string, std::string, std::uint32_t>>& message,
                   const std::vector<std::tuple<std::string, std::string, std::uint32_t>>& query) {
        for (const auto& [key, wanted, op] : query) {
            const std::uint32_t base = op & 0x7, modifiers = op & 0x1f0;
            if (base == 7) continue;   // ASL_QUERY_OP_TRUE: the key need only exist (checked below)
            if (modifiers & 0x100) unsupported("asl_search with regular expressions");
            const std::string* value = nullptr;
            for (const auto& [name, text_value, ignored] : message) { static_cast<void>(ignored); if (name == key) value = &text_value; }
            if (!value) return false;
            std::string a = *value, b = wanted;
            if (modifiers & 0x10) { a = lower(a); b = lower(b); }
            int compare;
            if (modifiers & 0x80) {
                const auto x = std::strtoll(a.c_str(), nullptr, 10), y = std::strtoll(b.c_str(), nullptr, 10);
                compare = x < y ? -1 : x > y ? 1 : 0;
            } else if ((modifiers & 0x60) == 0x60) compare = a.find(b) != std::string::npos ? 0 : 1;
            else if (modifiers & 0x20) compare = a.starts_with(b) ? 0 : 1;
            else if (modifiers & 0x40) compare = a.ends_with(b) ? 0 : 1;
            else compare = a < b ? -1 : a > b ? 1 : 0;
            const bool ok = base == 1 ? compare == 0 : base == 2 ? compare > 0 : base == 3 ? compare >= 0 : base == 4 ? compare < 0 :
                            base == 5 ? compare <= 0 : base == 6 ? compare != 0 : false;
            if (!ok) return false;
        }
        for (const auto& [key, wanted, op] : query) {
            static_cast<void>(wanted);
            if ((op & 0x7) != 7) continue;
            bool present = false;
            for (const auto& [name, value, ignored] : message) { static_cast<void>(value); static_cast<void>(ignored); if (name == key) present = true; }
            if (!present) return false;
        }
        return true;
    }

    // ---- message ports ----
    std::shared_ptr<MessagePort> port_of(Address object) {
        auto state = object ? support.objc.find_host_state<MessagePort>(object) : nullptr;
        if (!state) fail("not a CFMessagePort");
        return state;
    }
};

// Parses the numbers of an NSStringFrom* text in order (missing ones are 0).
std::array<double, 4> numbers_of(const std::string& text) {
    std::array<double, 4> values{};
    std::size_t index = 0;
    const char* at = text.c_str();
    while (*at && index < values.size()) {
        if ((*at >= '0' && *at <= '9') || *at == '-' || *at == '+' || *at == '.') {
            char* end = nullptr;
            const auto value = std::strtod(at, &end);
            if (end == at) { ++at; continue; }
            values[index++] = value;
            at = end;
        } else ++at;
    }
    return values;
}

} // namespace

void install_system_functions(Support& support) {
    auto self = std::make_shared<System>(support);

    // ---- OSAtomic / OSSpinLock ----
    for (const auto& [name, delta] : std::array<std::pair<const char*, std::int32_t>, 4>{{
             {"_OSAtomicIncrement32", 1}, {"_OSAtomicIncrement32Barrier", 1}, {"_OSAtomicDecrement32", -1}, {"_OSAtomicDecrement32Barrier", -1}}})
        support.function(libsystem, name, [self, delta = delta](CpuState& s, GuestMemory& m) {
            std::lock_guard lock(self->atomic_mutex);
            const auto value = static_cast<std::int32_t>(m.read32(s.x[0])) + delta;
            m.write32(s.x[0], static_cast<std::uint32_t>(value));
            s.x[0] = static_cast<Address>(static_cast<std::uint32_t>(value));
        });
    for (const auto& [name, set] : std::array<std::pair<const char*, bool>, 4>{{
             {"_OSAtomicTestAndSetBarrier", true}, {"_OSAtomicTestAndSet", true}, {"_OSAtomicTestAndClearBarrier", false}, {"_OSAtomicTestAndClear", false}}})
        support.function(libsystem, name, [self, set = set](CpuState& s, GuestMemory& m) {
            // Bit n is (0x80 >> (n & 7)) of byte (n >> 3).
            const auto n = static_cast<std::uint32_t>(s.x[0]);
            const auto address = s.x[1] + (n >> 3);
            const auto mask = static_cast<std::uint8_t>(0x80u >> (n & 7));
            std::lock_guard lock(self->atomic_mutex);
            const auto old = m.read8(address);
            m.write8(address, set ? static_cast<std::uint8_t>(old | mask) : static_cast<std::uint8_t>(old & ~mask));
            s.x[0] = (old & mask) != 0;
        });
    // OSQueueHead { void* opaque1; long opaque2; }: a LIFO linked through `offset`.
    support.function(libsystem, "_OSAtomicEnqueue", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->atomic_mutex);
        m.write64(s.x[1] + s.x[2], m.read64(s.x[0]));
        m.write64(s.x[0], s.x[1]);
        m.write64(s.x[0] + 8, m.read64(s.x[0] + 8) + 1);
    });
    support.function(libsystem, "_OSAtomicDequeue", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->atomic_mutex);
        const auto item = m.read64(s.x[0]);
        if (item) {
            m.write64(s.x[0], m.read64(item + s.x[1]));
            m.write64(s.x[0] + 8, m.read64(s.x[0] + 8) + 1);
        }
        s.x[0] = item;
    });
    const auto spin_try = [self](GuestMemory& m, Address lock_address) {
        std::lock_guard lock(self->atomic_mutex);
        if (m.read32(lock_address) != 0) return false;
        m.write32(lock_address, 1);
        return true;
    };
    support.function(libsystem, "_OSSpinLockTry", [spin_try](CpuState& s, GuestMemory& m) { s.x[0] = spin_try(m, s.x[0]); });
    support.function(libsystem, "_OSSpinLockLock", [&support, spin_try](CpuState& s, GuestMemory& m) {
        while (!spin_try(m, s.x[0])) {
            if (support.process.requested_exit()) fail("process exit while waiting for an OSSpinLock");
            static_cast<void>(support.call(libsystem, "_sched_yield", {}));
        }
    });
    support.function(libsystem, "_OSSpinLockUnlock", [self](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(self->atomic_mutex);
        m.write32(s.x[0], 0);
    });

    // ---- ASL ----
    support.function(libsystem, "_asl_new", [self](CpuState& s, GuestMemory&) {
        // ASL_TYPE_MSG 0, ASL_TYPE_QUERY 1
        if (s.x[0] > 1) { s.x[0] = 0; return; }
        const auto handle = self->asl_handle();
        std::lock_guard lock(self->asl_mutex);
        self->asl_messages[handle].type = static_cast<std::uint32_t>(s.x[0]);
        s.x[0] = handle;
    });
    const auto set_field = [self](Address handle, const std::string& key, const std::string& value, std::uint32_t op) {
        std::lock_guard lock(self->asl_mutex);
        const auto found = self->asl_messages.find(handle);
        if (found == self->asl_messages.end()) return false;
        auto& fields = found->second.fields;
        for (auto& field : fields) if (std::get<0>(field) == key && found->second.type == 0) { std::get<1>(field) = value; std::get<2>(field) = op; return true; }
        fields.emplace_back(key, value, op);
        return true;
    };
    support.function(libsystem, "_asl_set", [set_field](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = static_cast<Address>(-1); return; }
        s.x[0] = set_field(s.x[0], c_string(m, s.x[1], 4096), c_string(m, s.x[2]), 1) ? 0 : static_cast<Address>(-1);
    });
    support.function(libsystem, "_asl_set_query", [set_field](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = static_cast<Address>(-1); return; }
        s.x[0] = set_field(s.x[0], c_string(m, s.x[1], 4096), s.x[2] ? c_string(m, s.x[2]) : std::string{}, static_cast<std::uint32_t>(s.x[3])) ? 0
                                                                                                                                                  : static_cast<Address>(-1);
    });
    // asl_get: a C string the message owns until it is freed.
    support.function(libsystem, "_asl_get", [self](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = 0; return; }
        const auto key = c_string(m, s.x[1], 4096);
        std::lock_guard lock(self->asl_mutex);
        const auto found = self->asl_messages.find(s.x[0]);
        if (found == self->asl_messages.end()) { s.x[0] = 0; return; }
        for (const auto& [name, value, op] : found->second.fields) {
            static_cast<void>(op);
            if (name != key) continue;
            auto& slot = found->second.strings[key];
            if (slot) self->support.process.release_guest(slot);
            slot = self->support.process.allocate_guest(value.size() + 1, 1);
            if (!value.empty()) m.write_bytes(slot, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
            m.write8(slot + value.size(), 0);
            s.x[0] = slot;
            return;
        }
        s.x[0] = 0;
    });
    support.function(libsystem, "_asl_open", [self](CpuState& s, GuestMemory& m) {
        // A client: Sender and Facility defaults for the messages it sends.
        const auto handle = self->asl_handle();
        std::lock_guard lock(self->asl_mutex);
        auto& client = self->asl_messages[handle];
        client.type = 2;
        if (s.x[0]) client.fields.emplace_back("Sender", c_string(m, s.x[0], 4096), 1);
        if (s.x[1]) client.fields.emplace_back("Facility", c_string(m, s.x[1], 4096), 1);
        s.x[0] = handle;
    });
    support.function(libsystem, "_asl_send", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->asl_mutex);
        const auto found = self->asl_messages.find(s.x[1]);
        if (found == self->asl_messages.end()) { s.x[0] = static_cast<Address>(-1); return; }
        auto fields = found->second.fields;
        if (const auto client = self->asl_messages.find(s.x[0]); s.x[0] && client != self->asl_messages.end())
            for (const auto& field : client->second.fields) {
                bool present = false;
                for (const auto& own : fields) if (std::get<0>(own) == std::get<0>(field)) present = true;
                if (!present) fields.push_back(field);
            }
        const auto time = std::to_string(static_cast<long long>(std::floor(CoreFoundation::absolute_time_now() + 978307200.0)));
        bool has_time = false;
        for (const auto& own : fields) if (std::get<0>(own) == "Time") has_time = true;
        if (!has_time) fields.emplace_back("Time", time, 1);
        self->asl_store.push_back(std::move(fields));
        s.x[0] = 0;
    });
    support.function(libsystem, "_asl_search", [self](CpuState& s, GuestMemory&) {
        std::vector<std::vector<std::tuple<std::string, std::string, std::uint32_t>>> matches;
        {
            std::lock_guard lock(self->asl_mutex);
            const auto query = self->asl_messages.find(s.x[1]);
            const auto fields = query == self->asl_messages.end() ? decltype(query->second.fields){} : query->second.fields;
            for (const auto& message : self->asl_store) if (self->asl_match(message, fields)) matches.push_back(message);
        }
        AslResponse response;
        for (auto& message : matches) {
            const auto handle = self->asl_handle();
            std::lock_guard lock(self->asl_mutex);
            self->asl_messages[handle].fields = std::move(message);
            response.messages.push_back(handle);
        }
        const auto handle = self->asl_handle();
        std::lock_guard lock(self->asl_mutex);
        self->asl_responses[handle] = std::move(response);
        s.x[0] = handle;
    });
    support.function(libsystem, "_asl_next", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->asl_mutex);
        const auto found = self->asl_responses.find(s.x[0]);
        if (found == self->asl_responses.end() || found->second.next >= found->second.messages.size()) { s.x[0] = 0; return; }
        s.x[0] = found->second.messages[found->second.next++];
    });
    const auto release_asl = [self](Address handle) {
        AslResponse response;
        bool is_response = false;
        {
            std::lock_guard lock(self->asl_mutex);
            if (const auto found = self->asl_responses.find(handle); found != self->asl_responses.end()) {
                response = std::move(found->second);
                self->asl_responses.erase(found);
                is_response = true;
            }
        }
        if (!is_response) { self->asl_free_message(handle); return; }
        for (const auto message : response.messages) self->asl_free_message(message);
        self->support.process.release_guest(handle);
    };
    support.function(libsystem, "_asl_free", [release_asl](CpuState& s, GuestMemory&) { if (s.x[0]) release_asl(s.x[0]); });
    support.function(libsystem, "_asl_release", [release_asl](CpuState& s, GuestMemory&) { if (s.x[0]) release_asl(s.x[0]); });
    support.function(libsystem, "_asl_close", [release_asl](CpuState& s, GuestMemory&) { if (s.x[0]) release_asl(s.x[0]); });

    // ---- termios ----
    // struct termios { tcflag_t c_iflag, c_oflag, c_cflag, c_lflag (8 bytes each); cc_t c_cc[20]; speed_t c_ispeed, c_ospeed; }
    support.function(libsystem, "_cfmakeraw", [](CpuState& s, GuestMemory& m) {
        const auto t = s.x[0];
        auto iflag = m.read64(t), oflag = m.read64(t + 8), cflag = m.read64(t + 16), lflag = m.read64(t + 24);
        iflag &= ~std::uint64_t{0x2000 | 0x400 | 0x10 | 0x2 | 0x8 | 0x20 | 0x40 | 0x80 | 0x100 | 0x200 | 0x4};   // IMAXBEL IXOFF INPCK BRKINT PARMRK ISTRIP INLCR IGNCR ICRNL IXON IGNPAR
        iflag |= 0x1;   // IGNBRK
        oflag &= ~std::uint64_t{0x1};   // OPOST
        lflag &= ~std::uint64_t{0x8 | 0x2 | 0x4 | 0x10 | 0x100 | 0x80 | 0x400 | 0x80000000ULL | 0x400000 | 0x20000000};   // ECHO ECHOE ECHOK ECHONL ICANON ISIG IEXTEN NOFLSH TOSTOP PENDIN
        cflag &= ~std::uint64_t{0x300 | 0x1000};   // CSIZE PARENB
        cflag |= 0x300 | 0x800;   // CS8 CREAD
        m.write64(t, iflag);
        m.write64(t + 8, oflag);
        m.write64(t + 16, cflag);
        m.write64(t + 24, lflag);
        m.write8(t + 32 + 16, 1);   // c_cc[VMIN]
        m.write8(t + 32 + 17, 0);   // c_cc[VTIME]
    });
    support.function(libsystem, "_cfsetspeed", [](CpuState& s, GuestMemory& m) {
        m.write64(s.x[0] + 56, s.x[1]);
        m.write64(s.x[0] + 64, s.x[1]);
        s.x[0] = 0;
    });

    // ---- miscellany ----
    // mach_absolute_time counts nanoseconds here (timebase 1/1).
    support.function(libsystem, "_mach_wait_until", [&support](CpuState& s, GuestMemory&) {
        const auto deadline = s.x[0];
        const auto now = support.call(libsystem, "_mach_absolute_time", {});
        if (deadline > now) {
            GuestBlock spec(support, 16);
            const auto delta = deadline - now;
            support.memory().write64(spec, delta / 1000000000ULL);
            support.memory().write64(spec + 8, delta % 1000000000ULL);
            static_cast<void>(support.call(libsystem, "_nanosleep", {spec, 0}));
        }
        s.x[0] = 0;   // KERN_SUCCESS
    });
    support.function(libsystem, "_memset_pattern4", [](CpuState& s, GuestMemory& m) {
        std::array<std::uint8_t, 4> pattern{};
        m.read_bytes(s.x[1], pattern);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<Address>(s.x[2], 1u << 20)));
        for (Address done = 0; done < s.x[2];) {
            const auto count = std::min<Address>(bytes.size(), s.x[2] - done);
            for (std::size_t i = 0; i < count; ++i) bytes[i] = pattern[(done + i) % 4];
            m.write_bytes(s.x[0] + done, std::span(bytes.data(), static_cast<std::size_t>(count)));
            done += count;
        }
    });
    // reallocf: malloc/malloc_zones.cpp.
    support.function(libsystem, "_arc4random_uniform", [&support](CpuState& s, GuestMemory&) {
        const auto upper = static_cast<std::uint32_t>(s.x[0]);
        if (upper < 2) { s.x[0] = 0; return; }
        const auto minimum = (~upper + 1u) % upper;   // 2^32 mod upper
        for (;;) {
            const auto value = static_cast<std::uint32_t>(support.call(libsystem, "_arc4random", {}));
            if (value >= minimum) { s.x[0] = value % upper; return; }
        }
    });
    if (!support.process.resolve(libsystem, "_matrix_identity_float4x4")) {
        const auto matrix = support.process.allocate_guest(64, 16);
        auto& memory = support.process.main_engine();
        memory.fill_bytes(matrix, 0, 64);
        for (int i = 0; i < 4; ++i) memory.write32(matrix + 16 * i + 4 * i, std::bit_cast<std::uint32_t>(1.0f));
        support.process.install_export(std::string(libsystem), "_matrix_identity_float4x4", {matrix, ExportKind::data});
    }

    // ---- CoreFoundation ----
    const auto standard_allocator = [&support](Address allocator) {
        if (!allocator) return true;
        for (const auto* name : {"_kCFAllocatorDefault", "_kCFAllocatorSystemDefault", "_kCFAllocatorMalloc", "_kCFAllocatorMallocZone"})
            if (const auto found = support.process.resolve(core_foundation, name); found && support.memory().read64(found->address) == allocator) return true;
        return false;
    };
    const auto null_allocator = [&support](Address allocator) {
        const auto found = support.process.resolve(core_foundation, "_kCFAllocatorNull");
        return allocator && found && support.memory().read64(found->address) == allocator;
    };
    support.function(core_foundation, "_CFAllocatorAllocate", [&support, standard_allocator, null_allocator](CpuState& s, GuestMemory&) {
        if (null_allocator(s.x[0])) { s.x[0] = 0; return; }
        if (!standard_allocator(s.x[0])) unsupported("CFAllocatorAllocate with a custom allocator");
        s.x[0] = s.x[1] ? support.call(libsystem, "_malloc", {s.x[1]}) : 0;
    });
    support.function(core_foundation, "_CFAllocatorDeallocate", [&support, standard_allocator, null_allocator](CpuState& s, GuestMemory&) {
        if (null_allocator(s.x[0]) || !s.x[1]) return;
        if (!standard_allocator(s.x[0])) unsupported("CFAllocatorDeallocate with a custom allocator");
        static_cast<void>(support.call(libsystem, "_free", {s.x[1]}));
    });
    support.function(core_foundation, "_CFArrayExchangeValuesAtIndices", [&support](CpuState& s, GuestMemory&) {
        const auto array_object = s.x[0], first = s.x[1], second = s.x[2];
        const auto count = support.send_id(array_object, "count");
        if (first >= count || second >= count) fail("CFArrayExchangeValuesAtIndices: index out of bounds");
        if (first == second) return;
        CallRegs at;
        at.x[2] = first;
        const auto a = objc_retain(support, support.send(array_object, "objectAtIndex:", at).x0);
        at.x[2] = second;
        const auto b = objc_retain(support, support.send(array_object, "objectAtIndex:", at).x0);
        static_cast<void>(support.send_id(array_object, "replaceObjectAtIndex:withObject:", {first, b}));
        static_cast<void>(support.send_id(array_object, "replaceObjectAtIndex:withObject:", {second, a}));
        objc_release(support, a);
        objc_release(support, b);
    });
    // CFStringRef CFStringCreateWithFormat(CFAllocatorRef, CFDictionaryRef formatOptions, CFStringRef format, ...)
    support.function(core_foundation, "_CFStringCreateWithFormat", [&support](CpuState& s, GuestMemory&) {
        if (!s.x[2]) fail("CFStringCreateWithFormat with a NULL format");
        const auto string_class = class_named(support, "NSString");
        s.x[0] = support.send_id(support.send_id(string_class, "alloc"), "initWithFormat:arguments:", {s.x[2], s.sp});
    });
    if (!support.process.resolve(core_foundation, "_CFStringCreateWithFormatAndArguments"))
        support.function(core_foundation, "_CFStringCreateWithFormatAndArguments", [&support](CpuState& s, GuestMemory&) {
            const auto string_class = class_named(support, "NSString");
            s.x[0] = support.send_id(support.send_id(string_class, "alloc"), "initWithFormat:arguments:", {s.x[2], s.x[3]});
        });

    // ---- CFMessagePort (in-process) ----
    auto port_class = support.objc.find_class("__CFMessagePort");
    if (!port_class) port_class = support.objc.register_native_class("__CFMessagePort", class_named(support, "NSObject"), 16);
    self->port_class = port_class;
    support.method(port_class, "dealloc", [self, port_class](CpuState& s, GuestMemory&) {
        if (const auto state = self->support.objc.find_host_state<MessagePort>(s.x[0])) {
            if (state->is_local && state->valid) {
                std::lock_guard lock(self->port_mutex);
                if (const auto found = self->ports.find(state->name); found != self->ports.end() && found->second == s.x[0]) self->ports.erase(found);
            }
            if (state->queue) static_cast<void>(self->support.call(libsystem, "_dispatch_release", {std::exchange(state->queue, 0)}));
            objc_release(self->support, std::exchange(state->local, 0));
        }
        static_cast<void>(self->support.objc.send_super(s.x[0], port_class, self->support.objc.selector("dealloc")));
    }, "v@:");
    // CFMessagePortCreateLocal(allocator, name, callout, CFMessagePortContext*, Boolean* shouldFreeInfo)
    support.function(core_foundation, "_CFMessagePortCreateLocal", [self](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write8(s.x[4], 0);
        const auto name = s.x[1] ? self->support.cf.utf8(s.x[1]) : std::string{};
        auto state = std::make_shared<MessagePort>();
        state->name = name;
        state->callout = s.x[2];
        state->info = s.x[3] ? m.read64(s.x[3] + 8) : 0;   // context: { version, info, retain, release, copyDescription }
        state->is_local = true;
        std::lock_guard lock(self->port_mutex);
        if (!name.empty() && self->ports.contains(name)) { if (s.x[4]) m.write8(s.x[4], 1); s.x[0] = 0; return; }
        const auto object = make_object(self->support, self->port_class, state);
        if (!name.empty()) self->ports[name] = object;
        s.x[0] = object;
    });
    support.function(core_foundation, "_CFMessagePortCreateRemote", [self](CpuState& s, GuestMemory&) {
        if (!s.x[1]) { s.x[0] = 0; return; }
        const auto name = self->support.cf.utf8(s.x[1]);
        Address local;
        {
            std::lock_guard lock(self->port_mutex);
            const auto found = self->ports.find(name);
            if (found == self->ports.end()) { s.x[0] = 0; return; }   // no such port (ports of other processes are not visible)
            local = found->second;
        }
        auto state = std::make_shared<MessagePort>();
        state->name = name;
        state->local = objc_retain(self->support, local);
        s.x[0] = make_object(self->support, self->port_class, state);
    });
    support.function(core_foundation, "_CFMessagePortIsValid", [self](CpuState& s, GuestMemory&) {
        const auto state = self->port_of(s.x[0]);
        bool valid = state->valid;
        if (valid && state->local) valid = self->port_of(state->local)->valid;
        s.x[0] = valid;
    });
    support.function(core_foundation, "_CFMessagePortInvalidate", [self](CpuState& s, GuestMemory&) {
        const auto state = self->port_of(s.x[0]);
        if (!state->valid) return;
        state->valid = false;
        if (state->is_local) {
            std::lock_guard lock(self->port_mutex);
            if (const auto found = self->ports.find(state->name); found != self->ports.end() && found->second == s.x[0]) self->ports.erase(found);
        }
    });
    support.function(core_foundation, "_CFMessagePortSetDispatchQueue", [self](CpuState& s, GuestMemory&) {
        const auto state = self->port_of(s.x[0]);
        if (s.x[1]) static_cast<void>(self->support.call(libsystem, "_dispatch_retain", {s.x[1]}));
        if (const auto old = std::exchange(state->queue, s.x[1])) static_cast<void>(self->support.call(libsystem, "_dispatch_release", {old}));
    });
    // SInt32 CFMessagePortSendRequest(remote, msgid, data, sendTimeout, rcvTimeout, replyMode, CFDataRef* returnData):
    // the timeouts are doubles (d0, d1), so replyMode and returnData are x3 and x4.
    const auto callout_entry = support.process.install_function("___alchemy_completion_message_port_callout", [&support](CpuState& s, GuestMemory& m) {
        // context: { local, msgid, data, info, callout, result }
        const auto context = s.x[0];
        const std::array<Address, 4> arguments{m.read64(context), m.read64(context + 8), m.read64(context + 16), m.read64(context + 24)};
        m.write64(context + 40, EngineAdapter(support.process.calling_engine()).call(m.read64(context + 32), arguments));
        s.x[0] = 0;
    }, "alchemy.internal");
    support.function(core_foundation, "_CFMessagePortSendRequest", [self, callout_entry](CpuState& s, GuestMemory& m) {
        constexpr Address success = 0, invalid = static_cast<Address>(static_cast<std::uint32_t>(-5));
        const auto remote = self->port_of(s.x[0]);
        const auto local_object = remote->local ? remote->local : s.x[0];
        const auto local = self->port_of(local_object);
        if (!remote->valid || !local->valid || !local->callout) { s.x[0] = invalid; return; }
        GuestBlock context(self->support, 48);
        m.write64(context, local_object);
        m.write64(context + 8, static_cast<std::uint32_t>(s.x[1]));
        m.write64(context + 16, s.x[2]);
        m.write64(context + 24, local->info);
        m.write64(context + 32, local->callout);
        if (local->queue) static_cast<void>(self->support.call(libsystem, "_dispatch_sync_f", {local->queue, context, callout_entry}));
        else static_cast<void>(call_address(self->support, callout_entry, {context}));
        const auto reply = m.read64(context + 40);
        if (s.x[3] && s.x[4]) {
            // The reply becomes the caller's (+1) data.
            m.write64(s.x[4], reply ? self->support.cf.copy(reply) : 0);
        }
        if (reply) self->support.release(reply);
        s.x[0] = success;
    });

    // ---- Foundation ----
    for (const auto* name : {"NSAppleScriptErrorMessage", "NSAppleScriptErrorNumber", "NSAppleScriptErrorRange", "NSAppleScriptErrorBriefMessage",
                             "NSAppleScriptErrorAppName", "NSProgressFileOperationKindCopying", "NSProgressKindFile",
                             "NSProgressFileOperationKindKey", "NSProgressFileURLKey", "NSProgressFileTotalCountKey", "NSProgressFileCompletedCountKey"})
        if (!support.process.resolve(foundation_library, "_" + std::string(name))) support.string_constant(foundation_library, "_" + std::string(name), name);
    if (!support.process.resolve(foundation_library, "_NSEdgeInsetsZero")) {
        const auto insets = support.process.allocate_guest(32, 16);
        support.process.main_engine().fill_bytes(insets, 0, 32);
        support.process.install_export(std::string(foundation_library), "_NSEdgeInsetsZero", {insets, ExportKind::data});
    }
    support.function(foundation_library, "_NSAllMapTableKeys", [&support](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? support.send_id(support.send_id(s.x[0], "keyEnumerator"), "allObjects") : 0;
    });
    support.function(foundation_library, "_NSAllMapTableValues", [&support](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? support.send_id(support.send_id(s.x[0], "objectEnumerator"), "allObjects") : 0;
    });
    // The current user's home for nil or the current user's name; /Users/<name> when it exists; else nil.
    support.function(foundation_library, "_NSHomeDirectoryForUser", [&support](CpuState& s, GuestMemory&) {
        const auto home = support.call(foundation_library, "_NSHomeDirectory", {});
        if (!s.x[0]) { s.x[0] = home; return; }
        const auto name = support.cf.utf8(s.x[0]);
        const auto current = support.process.resolve(foundation_library, "_NSUserName") ? support.call(foundation_library, "_NSUserName", {}) : 0;
        if (current && support.cf.utf8(current) == name) { s.x[0] = home; return; }
        const auto path = "/Users/" + name;
        GuestBlock text_path(support, path), buffer(support, 144);
        s.x[0] = support.call(libsystem, "_stat", {text_path, buffer}) == 0 ? string(support, path) : 0;
    });
    support.function(foundation_library, "_NSPointFromString", [&support](CpuState& s, GuestMemory&) {
        const auto values = numbers_of(s.x[0] ? support.cf.utf8(s.x[0]) : std::string{});
        s.v[0] = {std::bit_cast<std::uint64_t>(values[0]), 0};
        s.v[1] = {std::bit_cast<std::uint64_t>(values[1]), 0};
    });
    support.function(foundation_library, "_NSSizeFromString", [&support](CpuState& s, GuestMemory&) {
        const auto values = numbers_of(s.x[0] ? support.cf.utf8(s.x[0]) : std::string{});
        s.v[0] = {std::bit_cast<std::uint64_t>(values[0]), 0};
        s.v[1] = {std::bit_cast<std::uint64_t>(values[1]), 0};
    });
    support.function(foundation_library, "_NSRectFromString", [&support](CpuState& s, GuestMemory&) {
        const auto values = numbers_of(s.x[0] ? support.cf.utf8(s.x[0]) : std::string{});
        for (unsigned i = 0; i < 4; ++i) s.v[i] = {std::bit_cast<std::uint64_t>(values[i]), 0};
    });

    // ---- ServiceManagement ----
    support.string_constant(service_management, "_kSMDomainSystemLaunchd", "kSMDomainSystemLaunchd");
    support.string_constant(service_management, "_kSMDomainUserLaunchd", "kSMDomainUserLaunchd");
    support.function(service_management, "_SMJobSubmit", [](CpuState&, GuestMemory&) { unsupported("SMJobSubmit (no launchd under Alchemy)"); });
    support.function(service_management, "_SMJobRemove", [](CpuState&, GuestMemory&) { unsupported("SMJobRemove (no launchd under Alchemy)"); });
    ProcessContext::add_provider_library(std::string(service_management));
}

} // namespace alchemy::gui::completion_kit
