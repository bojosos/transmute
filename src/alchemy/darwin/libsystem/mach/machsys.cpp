#include "alchemy/gui/fill_provider.hpp"

#include "alchemy/call_regs.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/process_tree.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/system.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <mutex>
#include <thread>
#include <vector>

// libSystem, libbsm and power-metrics gaps of the images Qt WebEngine links
// ("strict mode"). Fill-only: every symbol here is
// installed only when no other provider exports it.
//
// Behaviour by area:
// - Mach IPC: ports, rights, mach_msg, bootstrap, MIG reply ports, memory
//   entries, notify(3) and task policies are the in-process port namespace
//   of mach/mach_exports.cpp and mach/notify.cpp (installed earlier), and
//   kevent64 is DarwinServices'. Queries that ask "which exception ports are
//   registered" answer truthfully that none are.
// - launchd: there is no launchd. launch_msg fails with ENOTSUP.
//   launch_data_t objects are real (they are plain data containers).
// - sandbox: the process is never sandboxed. sandbox_check answers
//   "allowed" (0) as for an unsandboxed process; entering a sandbox or
//   issuing extensions fails with an error.
// - Task and thread control: only the calling task is known. task_for_pid(self)
//   and task_self_trap are real; suspending threads or tasks and reading
//   another thread's registers fail with KERN_NOT_SUPPORTED.
// - libc: termios speed accessors, creat, fgetln, getgrgid/getgrnam (the
//   user's own group and wheel), getpeereid, guarded descriptors,
//   getsect*fromheader_64, the generic
//   __atomic_load/__atomic_store, ilogbf and the arm64 page constants are real.
// - Power metrics (libpmsample, libpmenergy): pm_sample_task fails with
//   KERN_NOT_SUPPORTED; pm_energy_impact has no failure value and stops
//   the process if called.
// - terminfo (libncurses): no terminfo database exists, so setupterm fails
//   and no terminal is ever current (install_terminfo).
namespace alchemy::gui::fill {
namespace {

// <mach/kern_return.h>, <mach/message.h>
constexpr std::int32_t kern_success = 0, kern_invalid_address = 1, kern_invalid_argument = 4, kern_failure = 5,
    kern_not_supported = 46;
constexpr std::int32_t mach_send_invalid_dest = 0x10000003, mach_rcv_invalid_name = 0x10004002;
// <sys/errno.h>
constexpr std::uint32_t e_perm = 1, e_srch = 3, e_badf = 9, e_inval = 22, e_notty = 25, e_notsock = 38, e_notsup = 45;

struct LaunchNode {
    std::uint32_t type{};
    std::int64_t integer{};
    double real{};
    bool boolean{};
    std::string text;                                   // string or opaque bytes
    std::vector<std::pair<std::string, Address>> dict;   // owned children
    std::vector<Address> array;                         // owned children (0 = empty slot)
};
// <launch.h> launch_data_type_t
constexpr std::uint32_t launch_dictionary = 1, launch_array = 2, launch_integer = 4, launch_real = 5, launch_bool = 6,
    launch_string = 7, launch_opaque = 8, launch_errno = 9;

class Machsys final : public Provider {
public:
    using Provider::Provider;

    std::mutex mutex;
    std::map<Address, LaunchNode> launch;               // handle -> node
    std::map<std::int64_t, std::uint64_t> guards;       // fd -> guard id
    std::map<Address, std::pair<Address, Address>> lines; // FILE* -> (buffer slot, capacity slot) for fgetln
    std::map<Address, std::uint32_t> mutex_protocols;   // pthread_mutexattr_t* -> PTHREAD_PRIO_*
    std::mutex atomic_mutex;

    std::uint32_t task_self() {
        const auto found = process.resolve(libsystem, "_mach_task_self_");
        return found ? memory().read32(found->address) : 0;
    }
    std::int64_t guest_int(std::uint64_t value) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(value)); }
    std::uint32_t guest_errno() {
        const auto thread = process.current_thread();
        return thread && thread->errno_address ? process.calling_engine().read32(thread->errno_address) : 0;
    }

    // ---- launch_data_t ------------------------------------------------------------------
    Address new_node(LaunchNode node) {
        const auto handle = malloc(16);
        memory().write64(handle, node.type);
        memory().write64(handle + 8, 0);
        std::lock_guard lock(mutex);
        launch[handle] = std::move(node);
        return handle;
    }
    void free_node(Address handle) {
        if (!handle) return;
        LaunchNode node;
        {
            std::lock_guard lock(mutex);
            const auto found = launch.find(handle);
            if (found == launch.end()) throw std::runtime_error("Alchemy: launch_data_free of an unknown launch_data_t");
            node = std::move(found->second);
            launch.erase(found);
        }
        for (const auto& [key, child] : node.dict) free_node(child);
        for (const auto child : node.array) free_node(child);
        free(handle);
    }
    template <class F>
    auto with_node(Address handle, F f) {
        std::lock_guard lock(mutex);
        const auto found = launch.find(handle);
        if (found == launch.end()) throw std::runtime_error("Alchemy: unknown launch_data_t");
        return f(found->second);
    }

    // struct group {char *gr_name; char *gr_passwd; gid_t gr_gid; char **gr_mem;}
    std::map<std::uint32_t, Address> groups;
    Address group(std::optional<std::uint32_t> gid, const std::string& name) {
        auto& m = memory();
        const auto user_gid = static_cast<std::uint32_t>(call(libsystem, "_getgid", {}));
        const auto record = call(libsystem, "_getpwuid", {call(libsystem, "_getuid", {})});
        const std::string user = record ? read_cstring(m, m.read64(record)) : std::string("user");
        struct Entry { std::uint32_t gid; std::string name; bool member; };
        const std::array<Entry, 2> entries{{{user_gid, user, true}, {0, "wheel", false}}};
        for (const auto& entry : entries) {
            if (gid ? *gid != entry.gid : name != entry.name) continue;
            std::lock_guard lock(mutex);
            auto& slot = groups[entry.gid];
            if (!slot) {
                const auto members = process.allocate_guest(16);
                process.main_engine().write64(members, entry.member ? cstring(user) : 0);
                process.main_engine().write64(members + 8, 0);
                slot = process.allocate_guest(32);
                process.main_engine().write64(slot, cstring(entry.name));
                process.main_engine().write64(slot + 8, cstring("*"));
                process.main_engine().write64(slot + 16, entry.gid);
                process.main_engine().write64(slot + 24, members);
            }
            return slot;
        }
        return 0;   // not found (errno unchanged, as getgrgid reports a missing group)
    }

    void install();
    void install_mach();
    void install_launchd();
    void install_sandbox();
    void install_libc();
    void install_private();
    void install_terminfo();
};

void Machsys::install_mach() {
    // Exception ports: none is registered, so reads report an empty set;
    // registering one needs a Mach port namespace Alchemy does not have.
    for (const auto* name : {"_host_get_exception_ports", "_thread_get_exception_ports"}) {
        // (target, mask, masks[], *masksCnt, old_handlers[], old_behaviors[], old_flavors[])
        raw(libsystem, name, [](CpuState& s, GuestMemory& m) {
            if (s.x[3]) m.write32(s.x[3], 0);
            s.x[0] = kern_success;
        });
    }
    for (const auto* name : {"_host_set_exception_ports", "_thread_set_exception_ports"}) failure(libsystem, name, kern_not_supported);
    for (const auto* name : {"_host_swap_exception_ports", "_task_swap_exception_ports", "_thread_swap_exception_ports"}) {
        // (target, mask, new_port, behavior, flavor, masks[], *masksCnt, ...)
        raw(libsystem, name, [](CpuState& s, GuestMemory& m) {
            if (s.x[6]) m.write32(s.x[6], 0);
            s.x[0] = kern_not_supported;
        });
    }
    // Ports and rights, file ports, mach_msg, mach_msg_destroy and MIG reply
    // ports: mach/mach_exports.cpp (gui::install_chrome_support, before this).
    // NDR_record_t: little-endian integers, ASCII, IEEE floats.
    const std::array<std::uint8_t, 8> ndr{0, 0, 0, 0, 1, 0, 0, 0};
    data(libsystem, "_NDR_record", ndr, 8);

    // mach_error_string
    raw(libsystem, "_mach_error_string", [this](CpuState& s, GuestMemory&) {
        static const std::map<std::int32_t, std::string_view> texts{
            {0, "(os/kern) successful"}, {1, "(os/kern) invalid address"}, {2, "(os/kern) protection failure"},
            {3, "(os/kern) no space available"}, {4, "(os/kern) invalid argument"}, {5, "(os/kern) failure"},
            {6, "(os/kern) resource shortage"}, {7, "(os/kern) not receiver"}, {8, "(os/kern) no access"},
            {15, "(os/kern) invalid name"}, {16, "(os/kern) invalid task"}, {17, "(os/kern) invalid right"},
            {18, "(os/kern) invalid value"}, {46, "(os/kern) not supported"},
            {mach_send_invalid_dest, "(ipc/send) invalid destination port"},
            {mach_rcv_invalid_name, "(ipc/rcv) invalid name"}, {1102, "(bootstrap) unknown service name"}};
        const auto found = texts.find(static_cast<std::int32_t>(s.x[0]));
        const auto text = found == texts.end() ? std::string_view("unknown error code") : found->second;
        std::lock_guard lock(mutex);
        static std::map<std::string_view, Address> cache;
        auto& slot = cache[text];
        if (!slot) slot = cstring(text);
        s.x[0] = slot;
    });

    // Tasks: only the calling task is known.
    raw(libsystem, "_task_self_trap", [this](CpuState& s, GuestMemory&) { s.x[0] = task_self(); });
    // pid_for_task: mach/mach_exports.cpp.
    // task_name_for_pid(target_tport, pid, *name): the same rule as
    // task_for_pid (a name port is a task port with fewer rights).
    raw(libsystem, "_task_name_for_pid", [this](CpuState& s, GuestMemory& m) {
        const auto pid = static_cast<std::uint32_t>(s.x[1]);
        const bool own = pid == static_cast<std::uint32_t>(call(libsystem, "_getpid", {}));
        if (s.x[2]) m.write32(s.x[2], own ? task_self() : 0);
        s.x[0] = own ? kern_success : kern_failure;
    });
    raw(libsystem, "_task_for_pid", [this](CpuState& s, GuestMemory& m) {
        // (target_tport, pid, *task): another process's task port needs
        // privileges and Mach ports; the own pid answers mach_task_self.
        const auto pid = static_cast<std::uint32_t>(s.x[1]);
        if (pid != static_cast<std::uint32_t>(call(libsystem, "_getpid", {}))) {
            if (s.x[2]) m.write32(s.x[2], 0);
            s.x[0] = kern_failure;
            return;
        }
        if (s.x[2]) m.write32(s.x[2], task_self());
        s.x[0] = kern_success;
    });
    // task_policy_get and task_policy_set: mach/mach_exports.cpp.
    for (const auto* name : {"_task_suspend", "_task_resume", "_thread_suspend", "_thread_resume"})
        failure(libsystem, name, kern_not_supported);
    raw(libsystem, "_thread_get_state", [](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write32(s.x[3], 0);
        s.x[0] = kern_not_supported;
    });
    // thread_switch(port, option, time_ms): SWITCH_OPTION_DEPRESS/WAIT block
    // for `time` milliseconds, SWITCH_OPTION_NONE yields.
    raw(libsystem, "_thread_switch", [](CpuState& s, GuestMemory&) {
        const auto option = static_cast<std::uint32_t>(s.x[1]);
        const auto time = static_cast<std::uint32_t>(s.x[2]);
        if (option == 1 || option == 2) std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::uint32_t>(time, 60000)));
        else std::this_thread::yield();
        s.x[0] = kern_success;
    });

    // VM: the mach_vm_* names share the vm_* implementations (identical ABI on arm64).
    for (const auto& [alias, target] : std::array<std::pair<const char*, const char*>, 3>{{
             {"_mach_vm_map", "_vm_map"}, {"_mach_vm_remap", "_vm_remap"}, {"_vm_region_64", "_mach_vm_region"}}}) {
        if (has(libsystem, alias)) continue;
        if (const auto found = process.resolve(libsystem, target))
            process.install_export(std::string(libsystem), alias, {found->address, ExportKind::function});
    }
    // mach_vm_read(task, address, size, *data, *count): a copy in new memory.
    raw(libsystem, "_mach_vm_read", [this](CpuState& s, GuestMemory& m) {
        if (static_cast<std::uint32_t>(s.x[0]) != task_self()) { s.x[0] = kern_invalid_argument; return; }
        const auto source = s.x[1], size = s.x[2];
        if (!s.x[3] || !s.x[4] || size > 0xffffffffULL) { s.x[0] = kern_invalid_argument; return; }
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        try { if (size) m.read_bytes(source, bytes); } catch (const std::exception&) { s.x[0] = kern_invalid_address; return; }
        const auto slot = process.allocate_guest(8);
        const auto result = call(libsystem, "_vm_allocate", {task_self(), slot, size ? size : 1, 1});
        if (result != 0) { s.x[0] = result; return; }
        const auto copy = m.read64(slot);
        if (size) m.write_bytes(copy, bytes);
        m.write64(s.x[3], copy);
        m.write32(s.x[4], static_cast<std::uint32_t>(size));
        s.x[0] = kern_success;
    });
    // vm_page_mask / vm_page_shift: arm64 Darwin pages are 16 KiB (vm_page_size).
    const std::array<std::uint8_t, 8> mask{0xff, 0x3f, 0, 0, 0, 0, 0, 0};
    data(libsystem, "_vm_page_mask", mask, 8);
    const std::array<std::uint8_t, 4> shift{14, 0, 0, 0};
    data(libsystem, "_vm_page_shift", shift, 4);
    // host_statistics64(host, flavor, info, *count): HOST_VM_INFO64 from the host's memory status.
    raw(libsystem, "_host_statistics64", [](CpuState& s, GuestMemory& m) {
        constexpr std::uint32_t vm_info64_count = 38;
        if (static_cast<std::uint32_t>(s.x[1]) != 4) { s.x[0] = kern_invalid_argument; return; }
        if (!s.x[2] || !s.x[3]) { s.x[0] = kern_invalid_argument; return; }
        if (m.read32(s.x[3]) < vm_info64_count) { s.x[0] = kern_failure; return; }
        std::uint64_t total = 0, available = 0;
        if (const auto memory = host::system::memory()) { total = memory->physical; available = memory->available; }
        constexpr std::uint64_t page = 16384;
        std::vector<std::uint8_t> block(vm_info64_count * 4, 0);
        const auto put32 = [&](std::size_t offset, std::uint64_t value) {
            const auto word = static_cast<std::uint32_t>(std::min<std::uint64_t>(value, 0xffffffffULL));
            std::memcpy(block.data() + offset, &word, 4);
        };
        put32(0, available / page);                       // free_count
        put32(4, (total - std::min(total, available)) / page);   // active_count (in use)
        put32(140, (total - std::min(total, available)) / page); // internal_page_count
        m.write_bytes(s.x[2], block);
        m.write32(s.x[3], vm_info64_count);
        s.x[0] = kern_success;
    });
    // Malloc zones: malloc/malloc_zones.cpp (core).
    // libbsm: audit_token_t (8 x uint32, passed by reference); val[5] is the pid.
    raw(libbsm, "_audit_token_to_pid", [](CpuState& s, GuestMemory& m) { s.x[0] = m.read32(s.x[0] + 20); });
}

void Machsys::install_launchd() {
    // bootstrap_port, bootstrap_look_up and bootstrap_check_in: mach/mach_exports.cpp
    // (gui::install_chrome_support, before this).
    raw(libsystem, "_bootstrap_strerror", [this](CpuState& s, GuestMemory&) {
        static const std::map<std::int32_t, std::string_view> texts{
            {1100, "Permission denied"}, {1101, "Service name already exists"}, {1102, "Unknown service name"},
            {1103, "Service already active"}, {1104, "Too many lookups were requested in one request"},
            {1105, "Out of memory"}, {1106, "No children"}};
        const auto code = static_cast<std::int32_t>(s.x[0]);
        const auto found = texts.find(code);
        std::string text = found != texts.end() ? std::string(found->second) : code == 0 ? "Success" : "Unknown error";
        std::lock_guard lock(mutex);
        static std::map<std::string, Address> cache;
        auto& slot = cache[text];
        if (!slot) slot = cstring(text);
        s.x[0] = slot;
    });
    // launch_msg: launchd is absent.
    raw(libsystem, "_launch_msg", [this](CpuState& s, GuestMemory&) { set_errno(e_notsup); s.x[0] = 0; });
    // launch_data_t containers (real).
    raw(libsystem, "_launch_data_alloc", [this](CpuState& s, GuestMemory&) {
        LaunchNode node;
        node.type = static_cast<std::uint32_t>(s.x[0]);
        s.x[0] = new_node(std::move(node));
    });
    raw(libsystem, "_launch_data_free", [this](CpuState& s, GuestMemory&) { free_node(s.x[0]); });
    raw(libsystem, "_launch_data_get_type", [this](CpuState& s, GuestMemory&) {
        s.x[0] = with_node(s.x[0], [](LaunchNode& n) { return std::uint64_t{n.type}; });
    });
    raw(libsystem, "_launch_data_new_bool", [this](CpuState& s, GuestMemory&) {
        LaunchNode node; node.type = launch_bool; node.boolean = (s.x[0] & 0xff) != 0; s.x[0] = new_node(std::move(node));
    });
    raw(libsystem, "_launch_data_new_integer", [this](CpuState& s, GuestMemory&) {
        LaunchNode node; node.type = launch_integer; node.integer = static_cast<std::int64_t>(s.x[0]); s.x[0] = new_node(std::move(node));
    });
    raw(libsystem, "_launch_data_new_real", [this](CpuState& s, GuestMemory&) {
        LaunchNode node; node.type = launch_real; node.real = fill::real(s, 0); s.x[0] = new_node(std::move(node));
    });
    raw(libsystem, "_launch_data_new_string", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = 0; return; }
        LaunchNode node; node.type = launch_string; node.text = read_cstring(m, s.x[0]); s.x[0] = new_node(std::move(node));
    });
    raw(libsystem, "_launch_data_new_opaque", [this](CpuState& s, GuestMemory& m) {
        LaunchNode node; node.type = launch_opaque;
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(s.x[1], 1 << 24)));
        if (!bytes.empty()) m.read_bytes(s.x[0], bytes);
        node.text.assign(bytes.begin(), bytes.end());
        s.x[0] = new_node(std::move(node));
    });
    raw(libsystem, "_launch_data_get_integer", [this](CpuState& s, GuestMemory&) {
        s.x[0] = with_node(s.x[0], [](LaunchNode& n) { return static_cast<std::uint64_t>(n.integer); });
    });
    raw(libsystem, "_launch_data_get_errno", [this](CpuState& s, GuestMemory&) {
        s.x[0] = with_node(s.x[0], [](LaunchNode& n) { return static_cast<std::uint64_t>(n.type == launch_errno ? n.integer : 0); });
    });
    raw(libsystem, "_launch_data_dict_insert", [this](CpuState& s, GuestMemory& m) {
        // (dict, obj, key): the dictionary takes ownership of obj.
        const auto key = read_cstring(m, s.x[2]);
        Address replaced{};
        const bool ok = with_node(s.x[0], [&](LaunchNode& n) {
            if (n.type != launch_dictionary || !s.x[1]) return false;
            for (auto& [name, child] : n.dict) if (name == key) { replaced = child; child = s.x[1]; return true; }
            n.dict.emplace_back(key, s.x[1]);
            return true;
        });
        if (replaced) free_node(replaced);
        s.x[0] = ok;
    });
    raw(libsystem, "_launch_data_dict_lookup", [this](CpuState& s, GuestMemory& m) {
        const auto key = read_cstring(m, s.x[1]);
        s.x[0] = with_node(s.x[0], [&](LaunchNode& n) -> Address {
            if (n.type != launch_dictionary) return 0;
            for (const auto& [name, child] : n.dict) if (name == key) return child;
            return 0;
        });
    });
    raw(libsystem, "_launch_data_array_get_count", [this](CpuState& s, GuestMemory&) {
        s.x[0] = with_node(s.x[0], [](LaunchNode& n) { return n.type == launch_array ? std::uint64_t{n.array.size()} : 0; });
    });
    raw(libsystem, "_launch_data_array_get_index", [this](CpuState& s, GuestMemory&) {
        const auto index = s.x[1];
        s.x[0] = with_node(s.x[0], [&](LaunchNode& n) -> Address {
            return n.type == launch_array && index < n.array.size() ? n.array[static_cast<std::size_t>(index)] : 0;
        });
    });
    raw(libsystem, "_launch_data_array_set_index", [this](CpuState& s, GuestMemory&) {
        // (array, obj, index): the array takes ownership of obj.
        const auto index = s.x[2];
        Address replaced{};
        const bool ok = with_node(s.x[0], [&](LaunchNode& n) {
            if (n.type != launch_array || !s.x[1] || index > 1'000'000) return false;
            if (n.array.size() <= index) n.array.resize(static_cast<std::size_t>(index) + 1, 0);
            replaced = n.array[static_cast<std::size_t>(index)];
            n.array[static_cast<std::size_t>(index)] = s.x[1];
            return true;
        });
        if (replaced) free_node(replaced);
        s.x[0] = ok;
    });

    // notify_register_dispatch, notify_register_file_descriptor and notify_cancel:
    // mach/notify.cpp (gui::install_chrome_support, before this).
}

void Machsys::install_sandbox() {
    // <sandbox.h> named profiles (character arrays).
    for (const auto& [name, text] : std::array<std::pair<const char*, std::string_view>, 5>{{
             {"_kSBXProfileNoInternet", "no-internet"}, {"_kSBXProfileNoNetwork", "no-network"},
             {"_kSBXProfileNoWrite", "no-write"}, {"_kSBXProfileNoWriteExceptTemporary", "no-write-except-temporary"},
             {"_kSBXProfilePureComputation", "pure-computation"}}}) {
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        bytes.push_back(0);
        data(libsystem, name, bytes, 1);
    }
    // Extension classes (`extern const char *const`).
    if (!has(libsystem, "_APP_SANDBOX_READ")) value(libsystem, "_APP_SANDBOX_READ", cstring("com.apple.app-sandbox.read"));
    if (!has(libsystem, "_APP_SANDBOX_READ_WRITE"))
        value(libsystem, "_APP_SANDBOX_READ_WRITE", cstring("com.apple.app-sandbox.read-write"));
    // The process is not sandboxed: every operation is allowed.
    constant(libsystem, "_sandbox_check", 0);
    const auto refuse = [this](CpuState& s, GuestMemory& m, unsigned error_out) {
        if (error_out != ~0U) {
            const auto out = s.x[error_out];
            if (out) m.write64(out, strdup("Alchemy does not support the macOS sandbox"));
        }
        set_errno(e_notsup);
        s.x[0] = status(-1);
    };
    // sandbox_init(profile, flags, char **errorbuf), sandbox_init_with_parameters(profile, flags, params, errorbuf)
    raw(libsystem, "_sandbox_init", [refuse](CpuState& s, GuestMemory& m) { refuse(s, m, 2); });
    raw(libsystem, "_sandbox_init_with_parameters", [refuse](CpuState& s, GuestMemory& m) { refuse(s, m, 3); });
    raw(libsystem, "_sandbox_free_error", [this](CpuState& s, GuestMemory&) { free(s.x[0]); });
    raw(libsystem, "_sandbox_extension_issue_file", [this](CpuState& s, GuestMemory&) { set_errno(e_notsup); s.x[0] = 0; });
    raw(libsystem, "_sandbox_extension_consume", [this](CpuState& s, GuestMemory&) { set_errno(e_notsup); s.x[0] = status(-1); });
    raw(libsystem, "_sandbox_extension_release", [this](CpuState& s, GuestMemory&) { set_errno(e_inval); s.x[0] = status(-1); });
    // __sandbox_ms(policy, call, arg): the MAC policy syscall.
    raw(libsystem, "___sandbox_ms", [this](CpuState& s, GuestMemory&) { set_errno(78 /* ENOSYS */); s.x[0] = status(-1); });
}

void Machsys::install_libc() {
    // termios: speed_t c_ispeed at 56, c_ospeed at 64 (tcflag_t is unsigned long, cc_t c_cc[20]).
    raw(libsystem, "_cfgetispeed", [](CpuState& s, GuestMemory& m) { s.x[0] = m.read64(s.x[0] + 56); });
    raw(libsystem, "_cfgetospeed", [](CpuState& s, GuestMemory& m) { s.x[0] = m.read64(s.x[0] + 64); });
    raw(libsystem, "_cfsetispeed", [](CpuState& s, GuestMemory& m) { m.write64(s.x[0] + 56, s.x[1]); s.x[0] = 0; });
    raw(libsystem, "_cfsetospeed", [](CpuState& s, GuestMemory& m) { m.write64(s.x[0] + 64, s.x[1]); s.x[0] = 0; });
    // ttyname: libsystem/fs/terminal.cpp.
    // tcdrain: console output is written synchronously, so nothing is pending.
    raw(libsystem, "_tcdrain", [this](CpuState& s, GuestMemory&) {
        if (call(libsystem, "_isatty", {s.x[0]}) == 0) { set_errno(e_notty); s.x[0] = status(-1); return; }
        s.x[0] = 0;
    });
    // tcflush: output is never pending (TCOFLUSH succeeds); discarding unread
    // console input is not supported.
    raw(libsystem, "_tcflush", [this](CpuState& s, GuestMemory&) {
        if (call(libsystem, "_isatty", {s.x[0]}) == 0) { set_errno(e_notty); s.x[0] = status(-1); return; }
        const auto queue = static_cast<std::uint32_t>(s.x[1]);
        if (queue == 2) { s.x[0] = 0; return; }
        set_errno(queue == 1 || queue == 3 ? e_notsup : e_inval);
        s.x[0] = status(-1);
    });
    // creat(path, mode) = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode) (mode is variadic).
    raw(libsystem, "_creat", [this](CpuState& s, GuestMemory&) {
        CallRegs registers{};
        registers.x[0] = s.x[0];
        registers.x[1] = 0x1 | 0x200 | 0x400;
        registers.push_variadic(static_cast<std::uint64_t>(static_cast<std::uint16_t>(s.x[1])));
        s.x[0] = call_regs(libsystem, "_open", registers).x0;
    });
    // devname: no /dev entries are modelled.
    constant(libsystem, "_devname", 0);
    // fgetln(FILE*, size_t *len): a line (with its newline, not terminated)
    // in a per-stream buffer valid until the next call on that stream.
    raw(libsystem, "_fgetln", [this](CpuState& s, GuestMemory& m) {
        const auto stream = s.x[0];
        std::pair<Address, Address> slots;
        {
            std::lock_guard lock(mutex);
            auto& entry = lines[stream];
            if (!entry.first) {
                entry.first = process.allocate_guest(8);
                entry.second = process.allocate_guest(8);
                process.main_engine().write64(entry.first, 0);
                process.main_engine().write64(entry.second, 0);
            }
            slots = entry;
        }
        const auto length = static_cast<std::int64_t>(call(libsystem, "_getline", {slots.first, slots.second, stream}));
        if (length <= 0) { if (s.x[1]) m.write64(s.x[1], 0); s.x[0] = 0; return; }
        if (s.x[1]) m.write64(s.x[1], static_cast<std::uint64_t>(length));
        s.x[0] = m.read64(slots.first);
    });
    raw(libsystem, "_getdtablesize", [this](CpuState& s, GuestMemory&) {
        s.x[0] = call(libsystem, "_sysconf", {5});   // _SC_OPEN_MAX
    });
    // Groups: the user's own primary group (named after the user) and wheel.
    raw(libsystem, "_getgrgid", [this](CpuState& s, GuestMemory&) { s.x[0] = group(static_cast<std::uint32_t>(s.x[0]), {}); });
    raw(libsystem, "_getgrnam", [this](CpuState& s, GuestMemory& m) { s.x[0] = group(std::nullopt, read_cstring(m, s.x[0])); });
    // getpeereid(fd, uid*, gid*): every Alchemy process runs as the same user.
    raw(libsystem, "_getpeereid", [this](CpuState& s, GuestMemory& m) {
        const auto info = process.allocate_guest(144);
        if (static_cast<std::int32_t>(call(libsystem, "_fstat", {s.x[0], info})) != 0) { s.x[0] = status(-1); return; }
        if ((read16(m, info + 4) & 0170000) != 0140000) { set_errno(e_notsock); s.x[0] = status(-1); return; }
        if (s.x[1]) m.write32(s.x[1], static_cast<std::uint32_t>(call(libsystem, "_getuid", {})));
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(call(libsystem, "_getgid", {})));
        s.x[0] = 0;
    });
    // Guarded descriptors (<sys/guarded.h>): guards are tracked here; a
    // guarded close checks the guard before closing.
    raw(libsystem, "_change_fdguard_np", [this](CpuState& s, GuestMemory& m) {
        // (fd, const guardid_t *guard, guardflags, const guardid_t *nguard, nguardflags, int *fdflagsp)
        const auto fd = guest_int(s.x[0]);
        if (fd < 0) { set_errno(e_badf); s.x[0] = status(-1); return; }
        std::lock_guard lock(mutex);
        const auto found = guards.find(fd);
        if (found != guards.end()) {
            if (!s.x[1] || m.read64(s.x[1]) != found->second) { set_errno(e_perm); s.x[0] = status(-1); return; }
        } else if (s.x[1]) {
            set_errno(e_inval); s.x[0] = status(-1); return;
        }
        if (s.x[3]) guards[fd] = m.read64(s.x[3]);
        else guards.erase(fd);
        s.x[0] = 0;
    });
    raw(libsystem, "_guarded_close_np", [this](CpuState& s, GuestMemory& m) {
        const auto fd = guest_int(s.x[0]);
        {
            std::lock_guard lock(mutex);
            const auto found = guards.find(fd);
            if (found == guards.end()) { set_errno(e_inval); s.x[0] = status(-1); return; }
            if (!s.x[1] || m.read64(s.x[1]) != found->second) { set_errno(e_perm); s.x[0] = status(-1); return; }
            guards.erase(found);
        }
        s.x[0] = call(libsystem, "_close", {s.x[0]});
    });
    // Mach-O section lookups in a mapped image header.
    const auto find_section = [](GuestMemory& m, Address header, const std::string& segment, const std::string& section) -> Address {
        if (!header || m.read32(header) != 0xfeedfacfU) return 0;
        const auto commands = m.read32(header + 16);
        auto cursor = header + 32;
        for (std::uint32_t index = 0; index < commands && index < 4096; ++index) {
            const auto command = m.read32(cursor), size = m.read32(cursor + 4);
            if (size < 8) return 0;
            if (command == 0x19) {
                std::array<std::uint8_t, 16> name{};
                m.read_bytes(cursor + 8, name);
                if (std::string(reinterpret_cast<const char*>(name.data()), strnlen(reinterpret_cast<const char*>(name.data()), 16)) == segment) {
                    const auto sections = m.read32(cursor + 64);
                    for (std::uint32_t at = 0; at < sections; ++at) {
                        const auto entry = cursor + 72 + at * 80ULL;
                        m.read_bytes(entry, name);
                        if (std::string(reinterpret_cast<const char*>(name.data()), strnlen(reinterpret_cast<const char*>(name.data()), 16)) == section)
                            return entry;
                    }
                }
            }
            cursor += size;
        }
        return 0;
    };
    raw(libsystem, "_getsectbynamefromheader_64", [this, find_section](CpuState& s, GuestMemory& m) {
        s.x[0] = find_section(m, s.x[0], read_cstring(m, s.x[1], 17), read_cstring(m, s.x[2], 17));
    });
    // getsectdata(segment, section, size*): the main executable's section
    // (dyld's image 0), unslid as Darwin returns it.
    raw(libsystem, "_getsectdata", [this, find_section](CpuState& s, GuestMemory& m) {
        const auto header = call(libsystem, "__dyld_get_image_header", {0});
        const auto entry = find_section(m, header, read_cstring(m, s.x[0], 17), read_cstring(m, s.x[1], 17));
        if (!entry) { if (s.x[2]) m.write64(s.x[2], 0); s.x[0] = 0; return; }
        if (s.x[2]) m.write64(s.x[2], m.read64(entry + 40));
        s.x[0] = m.read64(entry + 32);
    });
    raw(libsystem, "_getsectdatafromheader_64", [this, find_section](CpuState& s, GuestMemory& m) {
        const auto entry = find_section(m, s.x[0], read_cstring(m, s.x[1], 17), read_cstring(m, s.x[2], 17));
        if (!entry) { if (s.x[3]) m.write64(s.x[3], 0); s.x[0] = 0; return; }
        if (s.x[3]) m.write64(s.x[3], m.read64(entry + 40));
        s.x[0] = m.read64(entry + 32);   // the unslid section address, as Darwin returns it
    });
    // kevent64: DarwinServices (net/sockets.cpp).
    // Generic (libatomic) __atomic_load(size, src, dest, order) and
    // __atomic_store(size, dest, src, order): one lock serializes them, which
    // is the libatomic contract for objects without lock-free access.
    raw(libsystem, "___atomic_load", [this](CpuState& s, GuestMemory& m) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(s.x[0], 1 << 20)));
        std::lock_guard lock(atomic_mutex);
        if (!bytes.empty()) { m.read_bytes(s.x[1], bytes); m.write_bytes(s.x[2], bytes); }
    });
    raw(libsystem, "___atomic_store", [this](CpuState& s, GuestMemory& m) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(std::min<std::uint64_t>(s.x[0], 1 << 20)));
        std::lock_guard lock(atomic_mutex);
        if (!bytes.empty()) { m.read_bytes(s.x[2], bytes); m.write_bytes(s.x[1], bytes); }
    });
    raw(libsystem, "_ilogbf", [](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(std::ilogb(real32(s, 0))));
    });
    // in6addr_loopback (::1).
    std::array<std::uint8_t, 16> loopback{};
    loopback[15] = 1;
    data(libsystem, "_in6addr_loopback", loopback, 4);
    // pthread_stack_frame_decode_np(frame, *return_address): arm64 frame
    // records are {previous fp, lr}; pointer authentication bits are stripped.
    raw(libsystem, "_pthread_stack_frame_decode_np", [](CpuState& s, GuestMemory& m) {
        const auto frame = s.x[0];
        if (!frame) { if (s.x[1]) m.write64(s.x[1], 0); s.x[0] = 0; return; }
        if (s.x[1]) m.write64(s.x[1], m.read64(frame + 8) & 0x0000ffffffffffffULL);
        s.x[0] = m.read64(frame);
    });
    // pthread_mutexattr_setpolicy_np: both policies are scheduling hints;
    // Alchemy mutexes wake waiters in arrival order under either.
    raw(libsystem, "_pthread_mutexattr_setpolicy_np", [](CpuState& s, GuestMemory&) {
        const auto policy = static_cast<std::uint32_t>(s.x[1]);
        s.x[0] = s.x[0] && (policy == 1 || policy == 3) ? 0 : e_inval;
    });
    // PTHREAD_PRIO_NONE (0), _INHERIT (1) and _PROTECT (2) are stored on the
    // attribute (pthread_mutexattr_getprotocol reads them back). Alchemy
    // mutexes have no priority inheritance or ceiling: the host scheduler
    // sees plain waits, as for the QoS hints. base::Lock asks for PRIO_INHERIT.
    raw(libsystem, "_pthread_mutexattr_setprotocol", [this](CpuState& s, GuestMemory&) {
        const auto protocol = static_cast<std::uint32_t>(s.x[1]);
        if (!s.x[0] || protocol > 2) { s.x[0] = e_inval; return; }
        std::lock_guard lock(mutex);
        mutex_protocols[s.x[0]] = protocol;
        s.x[0] = 0;
    });
    raw(libsystem, "_pthread_mutexattr_getprotocol", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = e_inval; return; }
        std::lock_guard lock(mutex);
        const auto found = mutex_protocols.find(s.x[0]);
        m.write32(s.x[1], found == mutex_protocols.end() ? 0 : found->second);
        s.x[0] = 0;
    });
    // MAP_JIT memory is mapped read-write-execute for every thread, so the
    // per-thread write-protect toggle changes no protection: after (0) the
    // thread can write the memory, after (1) it can execute it, as the
    // contract says. V8 toggles it around every code write. The engine hears
    // of it: inside a write window (0) a store to translated JIT code need not
    // invalidate at once (the contract makes it visible only after
    // sys_icache_invalidate), and closing the window (1) reconciles.
    raw(libsystem, "_pthread_jit_write_protect_np", [this](CpuState& s, GuestMemory&) {
        engine_jit_write_window(process.calling_engine(), s.x[0] == 0);
    });
    // So the toggle is supported (as on Apple silicon): 1.
    raw(libsystem, "_pthread_jit_write_protect_supported_np", [](CpuState& s, GuestMemory&) { s.x[0] = 1; });
    // posix_spawn attributes with no Alchemy meaning are refused.
    failure(libsystem, "_posix_spawnattr_set_csm_np", static_cast<std::int32_t>(e_notsup));
    failure(libsystem, "_responsibility_spawnattrs_setdisclaim", static_cast<std::int32_t>(e_notsup));
    // No responsibility (TCC attribution) exists: each process is its own.
    raw(libsystem, "_responsibility_get_pid_responsible_for_pid", [](CpuState& s, GuestMemory&) { s.x[0] = s.x[0]; });
    raw(libsystem, "_proc_get_wakemon_params", [this](CpuState& s, GuestMemory&) { set_errno(e_notsup); s.x[0] = status(-1); });
    // proc_pidinfo(pid, flavor, arg, buffer, size) for this process and any
    // other process of the Alchemy tree (process_tree.hpp): PROC_PIDTASKINFO
    // (memory and CPU times of the host process), PROC_PIDTBSDINFO and
    // PROC_PIDT_SHORTBSDINFO (identity). Other flavors are not available.
    raw(libsystem, "_proc_pidinfo", [this](CpuState& s, GuestMemory& m) {
        const auto pid = static_cast<std::uint32_t>(s.x[0]);
        const auto flavor = static_cast<std::int32_t>(s.x[1]);
        const auto buffer = s.x[3];
        const auto size = static_cast<std::int32_t>(s.x[4]);
        const auto own = static_cast<std::uint32_t>(call(libsystem, "_getpid", {}));
        auto record = process_tree::lookup(pid);
        if (!record && pid == own) { record = process_tree::Record{}; record->pid = own; record->host_pid = host::process::current_pid(); }
        if (!record) { set_errno(e_srch); s.x[0] = 0; return; }
        const std::int32_t needed = flavor == 4 ? 96 : flavor == 3 ? 136 : flavor == 13 ? 64 : 0;
        if (!needed) { set_errno(e_notsup); s.x[0] = 0; return; }
        if (!buffer || size < needed) { set_errno(e_inval); s.x[0] = 0; return; }
        std::vector<std::uint8_t> info(static_cast<std::size_t>(needed), 0);
        const auto put64 = [&](std::size_t offset, std::uint64_t value) { std::memcpy(info.data() + offset, &value, 8); };
        const auto put32 = [&](std::size_t offset, std::uint32_t value) { std::memcpy(info.data() + offset, &value, 4); };
        const auto put_text = [&](std::size_t offset, std::size_t capacity, const std::string& value) {
            std::memcpy(info.data() + offset, value.data(), std::min(value.size(), capacity - 1));
        };
        if (flavor == 4) {
            const auto host = record->host_pid ? record->host_pid : record->pid;
            const auto usage = host::process::usage(host);
            if (!usage) { set_errno(e_srch); s.x[0] = 0; return; }
            put64(0, usage->private_bytes);                       // pti_virtual_size
            put64(8, usage->resident);                            // pti_resident_size
            put32(52, static_cast<std::uint32_t>(usage->page_faults));   // pti_faults
            put64(16, usage->user_ns);    // pti_total_user (mach time units are ns: timebase 1/1)
            put64(24, usage->system_ns);  // pti_total_system
        } else if (flavor == 3) {   // struct proc_bsdinfo
            put32(0, 0x10);                     // pbi_flags: PROC_FLAG_LP64
            put32(4, 2);                        // pbi_status: SRUN
            put32(12, record->pid); put32(16, record->ppid);
            for (const auto offset : {20, 28, 36}) put32(static_cast<std::size_t>(offset), record->uid);   // uid, ruid, svuid
            for (const auto offset : {24, 32, 40}) put32(static_cast<std::size_t>(offset), record->gid);   // gid, rgid, svgid
            put_text(48, 16, record->name);     // pbi_comm (MAXCOMLEN)
            put_text(64, 32, record->name);     // pbi_name
            put32(100, record->pgid);
            put32(108, 0xffffffffU);            // e_tdev: NODEV
            put64(120, record->start_seconds); put64(128, record->start_microseconds);
        } else {   // flavor 13: struct proc_bsdshortinfo
            put32(0, record->pid); put32(4, record->ppid); put32(8, record->pgid);
            put32(12, 2);                       // pbsi_status: SRUN
            put_text(16, 16, record->name);
            put32(32, 0x10);                    // pbsi_flags: PROC_FLAG_LP64
            for (const auto offset : {36, 44, 52}) put32(static_cast<std::size_t>(offset), record->uid);
            for (const auto offset : {40, 48, 56}) put32(static_cast<std::size_t>(offset), record->gid);
        }
        m.write_bytes(buffer, info);
        s.x[0] = static_cast<std::uint64_t>(needed);
    });
}

void Machsys::install_private() {
    // os_signpost: no tracing session ever enables signposts, so emission
    // (which the os_signpost macros only reach when enabled) has nothing to record.
    constant(libsystem, "_os_signpost_enabled", 0);
    raw(libsystem, "__os_signpost_emit_with_name_impl", [](CpuState&, GuestMemory&) {});
    // abort_report_np(fmt, ...): prints the formatted message and aborts.
    raw(libsystem, "_abort_report_np", [this](CpuState& s, GuestMemory& m) {
        const auto buffer = process.allocate_guest(2048);
        static_cast<void>(call(libsystem, "_vsnprintf", {buffer, 2047, s.x[0], s.sp}));
        const auto text = read_cstring(m, buffer, 2048);
        std::cerr << text << std::endl;
        static_cast<void>(call(libsystem, "_abort", {}));
    });
    // asl_vlog(client, msg, level, format, va_list): ASL writes to stderr
    // (there is no syslogd to receive it).
    raw(libsystem, "_asl_vlog", [this](CpuState& s, GuestMemory& m) {
        const auto buffer = process.allocate_guest(4096);
        static_cast<void>(call(libsystem, "_vsnprintf", {buffer, 4095, s.x[3], s.x[4]}));
        std::cerr << read_cstring(m, buffer, 4096) << std::endl;
        s.x[0] = 0;
    });
    // Power metrics (private libpmsample / libpmenergy).
    failure(libpmsample, "_pm_sample_task", kern_not_supported);
    unsupported(libpmenergy, "_pm_energy_impact", "no energy model; pm_sample_task never succeeds");
    install_terminfo();
}

// libncurses terminfo (a shader compiler, through LLVM's terminal colour
// probe). Alchemy ships no terminfo database, so no terminal is ever set up:
// setupterm returns ERR with *errret = -1 ("the terminfo database could not
// be found"), cur_term stays NULL, tigetnum answers CANCELLED_NUMERIC (-2)
// and del_curterm has nothing to delete (ERR). set_curterm swaps the
// cur_term pointer as documented and returns the previous one.
void Machsys::install_terminfo() {
    constexpr std::int32_t err = -1;
    const auto cur_term = data(libncurses, "_cur_term", std::array<std::uint8_t, 8>{}, 8);
    raw(libncurses, "_setupterm", [](CpuState& s, GuestMemory& m) {
        if (s.x[2]) m.write32(s.x[2], static_cast<std::uint32_t>(-1));
        s.x[0] = status(err);
    });
    raw(libncurses, "_set_curterm", [cur_term](CpuState& s, GuestMemory& m) {
        const auto previous = m.read64(cur_term);
        m.write64(cur_term, s.x[0]);
        s.x[0] = previous;
    });
    raw(libncurses, "_del_curterm", [cur_term](CpuState& s, GuestMemory& m) {
        if (s.x[0] && m.read64(cur_term) == s.x[0]) m.write64(cur_term, 0);
        s.x[0] = status(err);
    });
    constant(libncurses, "_tigetnum", status(-2));
    constant(libncurses, "_tigetflag", status(-1));   // not a boolean capability: no terminal
    constant(libncurses, "_tigetstr", static_cast<Address>(-1));   // (char *)-1: no terminal
}

void Machsys::install() {
    install_mach();
    install_launchd();
    install_sandbox();
    install_libc();
    install_private();
}

} // namespace

void install_machsys(InstallContext& context) {
    const auto provider = std::make_shared<Machsys>(context);
    provider->install();
}

} // namespace alchemy::gui::fill
