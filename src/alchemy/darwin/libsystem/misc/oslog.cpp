#include "alchemy/darwin/kit/completion/completion_kit.hpp"
#include "alchemy/darwin/libsystem/misc/log_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

// os_log objects as Objective-C objects. On macOS an os_log_t is an
// OS_os_log instance (an OS_object): Swift stores it as a class reference
// (os.Logger.logObject: OSLog) and retains and releases it through the
// Objective-C runtime. These objects live in guest memory with an OS_os_log
// isa; the classes implement -retain/-release themselves, so the runtime
// dispatches to them without the object store.
//   os_log_create, os_log_type_enabled, _os_log_impl, os_retain/os_release
//   (for these objects; other os objects keep their earlier provider), and the
//   OS_LOG_DEFAULT object (_os_log_default), immortal.
// Messages are recorded in the process's log store (log_store.cpp,
// read through OSLogStore with the current-process scope), composed from
// their format and arguments; there is no system-wide log. With
// ALCHEMY_OS_LOG=1 each message is also written to stderr. The older
// _os_log_create(dso, subsystem, category) and the variadic
// _os_log_internal(dso, log, type, format, ...) are provided too.
namespace alchemy::gui::completion_kit {
namespace {

constexpr std::string_view libsystem = "/usr/lib/libSystem.B.dylib";

struct Log { std::string subsystem, category; std::uint64_t references{1}; bool immortal{}; };

struct Logs {
    std::mutex mutex;
    std::map<Address, Log> objects;
    Address log_class{}, object_class{};
};

bool os_log_diagnostics() {
    static const bool enabled = [] {
        char* value = nullptr;
        std::size_t length = 0;
        if (_dupenv_s(&value, &length, "ALCHEMY_OS_LOG") != 0 || !value) return false;
        const bool on = *value && std::string_view(value) != "0";
        std::free(value);
        return on;
    }();
    return enabled;
}

std::string guest_text(GuestMemory& memory, Address address, std::size_t limit = 4096) {
    std::string text;
    if (!address) return text;
    for (std::size_t index = 0; index < limit; ++index) {
        const auto c = memory.read8(address + index);
        if (!c) break;
        text.push_back(static_cast<char>(c));
    }
    return text;
}

} // namespace

void install_os_log(Support& support) {
    auto logs = std::make_shared<Logs>();
    logs->object_class = support.declare_class("OS_object", "NSObject", libsystem);
    logs->log_class = support.declare_class("OS_os_log", "OS_object", libsystem);
    const auto make = [&support, logs](std::string subsystem, std::string category, bool immortal) {
        const auto address = support.process.allocate_guest(32, 16);
        support.memory().fill_bytes(address, 0, 32);
        support.memory().write64(address, logs->log_class);
        std::lock_guard lock(logs->mutex);
        logs->objects.emplace(address, Log{std::move(subsystem), std::move(category), 1, immortal});
        return address;
    };
    const auto retain = [logs](Address object) {
        std::lock_guard lock(logs->mutex);
        const auto found = logs->objects.find(object);
        if (found == logs->objects.end()) return false;
        if (!found->second.immortal) ++found->second.references;
        return true;
    };
    const auto release = [&support, logs](Address object) {
        {
            std::lock_guard lock(logs->mutex);
            const auto found = logs->objects.find(object);
            if (found == logs->objects.end()) return false;
            if (found->second.immortal || --found->second.references) return true;
            logs->objects.erase(found);
        }
        support.process.release_guest(object);
        return true;
    };
    // OS_LOG_DEFAULT is the address of _os_log_default itself.
    const auto default_log = make("", "", true);
    support.process.install_export(std::string(libsystem), "__os_log_default", {default_log, ExportKind::data});
    support.method(logs->object_class, "retain", [retain](CpuState& s, GuestMemory&) {
        if (!retain(s.x[0])) fail("-[OS_object retain] on an unknown os object");
    }, "@@:");
    support.method(logs->object_class, "release", [release](CpuState& s, GuestMemory&) {
        if (!release(s.x[0])) fail("-[OS_object release] on an unknown os object");
    }, "v@:");
    support.method(logs->object_class, "retainCount", [logs](CpuState& s, GuestMemory&) {
        std::lock_guard lock(logs->mutex);
        const auto found = logs->objects.find(s.x[0]);
        s.x[0] = found == logs->objects.end() ? 0 : found->second.immortal ? UINT64_MAX : found->second.references;
    }, "Q@:");

    support.function(libsystem, "_os_log_create", [make](CpuState& s, GuestMemory& m) {
        s.x[0] = make(guest_text(m, s.x[0], 1024), guest_text(m, s.x[1], 1024), false);
    });
    // os_log_t _os_log_create(void* dso, const char* subsystem, const char* category): the
    // first argument is the caller's image header (a Mach-O magic); without
    // one the arguments are os_log_create's.
    support.function(libsystem, "__os_log_create", [make](CpuState& s, GuestMemory& m) {
        bool image = false;
        try { image = s.x[0] && m.read32(s.x[0]) == 0xfeedfacfU; } catch (const std::exception&) {}
        s.x[0] = image ? make(guest_text(m, s.x[1], 1024), guest_text(m, s.x[2], 1024), false)
                       : make(guest_text(m, s.x[0], 1024), guest_text(m, s.x[1], 1024), false);
    });
    // Darwin's default configuration records default (0x00), error (0x10)
    // and fault (0x11) messages; info (0x01) and debug (0x02) are off.
    support.function(libsystem, "_os_log_type_enabled", [](CpuState& s, GuestMemory&) {
        const auto type = static_cast<std::uint8_t>(s.x[1]);
        s.x[0] = type == 0x00 || type == 0x10 || type == 0x11;
    });
    // Records one message (and echoes it with ALCHEMY_OS_LOG=1).
    const auto record = [&support, logs](Address dso, Address log, std::uint8_t type, std::string format, std::string message) {
        logs::Entry entry;
        entry.date = CoreFoundation::absolute_time_now();
        entry.type = type;
        entry.format = std::move(format);
        entry.message = std::move(message);
        entry.thread = support.process.current_thread()->identity;
        {
            std::lock_guard lock(logs->mutex);
            const auto found = logs->objects.find(log);
            if (found != logs->objects.end()) { entry.subsystem = found->second.subsystem; entry.category = found->second.category; }
        }
        for (const auto& image : support.process.images())
            if (image.header == dso) { entry.sender = image.name.substr(image.name.find_last_of('/') + 1); break; }
        if (os_log_diagnostics()) {
            std::string prefix = "[os_log";
            if (!entry.subsystem.empty()) prefix += " " + entry.subsystem + ":" + entry.category;
            const auto line = prefix + "] " + entry.message + "\n";
            std::fwrite(line.data(), 1, line.size(), stderr);
        }
        logs::record(std::move(entry));
    };
    // void _os_log_impl(void* dso, os_log_t, os_log_type_t, const char* format, uint8_t* buffer, uint32_t size)
    support.function(libsystem, "__os_log_impl", [&support, record](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) return;
        auto format = guest_text(m, s.x[3]);
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(static_cast<std::uint32_t>(s.x[5]), 65536)));
        if (s.x[4] && !buffer.empty()) m.read_bytes(s.x[4], buffer);
        auto message = logs::compose(format, buffer,
            [&m](std::uint64_t pointer) { return guest_text(m, pointer, 65536); },
            [&support](std::uint64_t object) {
                const auto description = support.send_id(object, "description");
                return description ? support.cf.utf8(description) : std::string("(null)");
            });
        record(s.x[0], s.x[1], static_cast<std::uint8_t>(s.x[2]), std::move(format), std::move(message));
    });
    // void _os_log_internal(void* dso, os_log_t, os_log_type_t, const char* format, ...): the
    // arguments are C variadic arguments; Foundation's formatter composes them.
    support.function(libsystem, "__os_log_internal", [&support, record](CpuState& s, GuestMemory& m) {
        if (!s.x[3]) return;
        const auto format = guest_text(m, s.x[3]);
        std::string plain;   // without os_log's {public}/{private} annotations
        for (std::size_t i = 0; i < format.size(); ++i) {
            plain.push_back(format[i]);
            if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == '{') {
                const auto close = format.find('}', i + 1);
                if (close != std::string::npos) i = close;
            }
        }
        const auto format_object = support.cf.string(plain);
        const auto string_class = support.objc.find_class("NSString");
        const auto text = support.send_id(support.send_id(string_class, "alloc"), "initWithFormat:arguments:", {format_object, s.sp});
        support.release(format_object);
        auto message = text ? support.cf.utf8(text) : plain;
        support.release(text);
        record(s.x[0], s.x[1], static_cast<std::uint8_t>(s.x[2]), format, std::move(message));
    });
    // os_retain/os_release: these log objects here, os_activity objects
    // through libsystem_misc.cpp's internal retain/release.
    const auto previous_retain = support.process.resolve("alchemy.internal", "__alchemy_os_activity_retain");
    const auto previous_release = support.process.resolve("alchemy.internal", "__alchemy_os_activity_release");
    support.function(libsystem, "_os_retain", [&support, retain, previous_retain](CpuState& s, GuestMemory&) {
        if (!s.x[0] || retain(s.x[0])) return;
        if (!previous_retain) fail("os_retain on an unknown os object");
        const std::array<Address, 1> arguments{s.x[0]};
        s.x[0] = support.process.calling_engine().invoke_callback(previous_retain->address, arguments);
    });
    support.function(libsystem, "_os_release", [&support, release, previous_release](CpuState& s, GuestMemory&) {
        if (!s.x[0] || release(s.x[0])) return;
        if (!previous_release) fail("os_release on an unknown os object");
        const std::array<Address, 1> arguments{s.x[0]};
        static_cast<void>(support.process.calling_engine().invoke_callback(previous_release->address, arguments));
    });
}

} // namespace alchemy::gui::completion_kit
