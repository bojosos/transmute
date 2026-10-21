#include "alchemy/darwin/libsystem/install.hpp"
#include "alchemy/darwin/libsystem/mach/mach_ipc.hpp"

#include "alchemy/gui/fill_provider.hpp"
#include "alchemy/mach_port_events.hpp"

#include <map>
#include <mutex>
#include <string>
#include <vector>

// libnotify (<notify.h>) inside one process: names, tokens, posts and the
// four delivery kinds (check, dispatch block, Mach port over the in-process
// namespace, file descriptor), plus per-name state. There is no notifyd, so
// posts from other processes (and the system's own names, such as the
// thermal pressure level) are never delivered; their state stays 0.
// Independently written from the public notify(3) documentation.
namespace alchemy::libsystem {
namespace {

using namespace mach;
using gui::fill::Provider;
using gui::fill::libsystem;
using transmute::CpuState;
using transmute::GuestMemory;

// NOTIFY_STATUS_* (<notify.h>)
constexpr std::uint32_t status_ok = 0, status_invalid_name = 1, status_invalid_token = 2, status_invalid_port = 3,
    status_invalid_file = 4, status_invalid_request = 6, status_null_input = 10, status_failed = 1000000;
constexpr std::uint32_t notify_reuse = 0x1;   // NOTIFY_REUSE

class NotifyProvider final : public Provider {
public:
    using Provider::Provider;

    enum class Kind { check, dispatch, mach_port, file };
    struct Registration {
        std::string name;
        Kind kind{};
        bool posted{true};                 // notify_check: true on its first call
        Address queue{}, block{};          // dispatch
        Right port;                        // mach_port: a send right to the caller's port
        std::int32_t write_fd{-1};         // file: the pipe's write end
        std::int32_t read_fd{-1};
    };
    std::shared_ptr<MachIpc> ipc;
    std::mutex mutex;
    std::map<std::int32_t, Registration> tokens;
    std::map<std::string, std::uint64_t> states;
    std::int32_t next_token{1};
    Address trampoline{};

    std::int32_t add(Registration registration) {
        std::lock_guard lock(mutex);
        const auto token = next_token++;
        tokens.emplace(token, std::move(registration));
        return token;
    }

    // dispatch_async_f context: {block (copied), token}.
    void deliver_dispatch(Address queue, Address block, std::int32_t token) {
        const auto cell = malloc(16);
        memory().write64(cell, call(libsystem, "__Block_copy", {block}));
        memory().write64(cell + 8, static_cast<std::uint32_t>(token));
        static_cast<void>(call(libsystem, "_dispatch_async_f", {queue, cell, trampoline}));
    }

    std::uint32_t post(const std::string& name) {
        std::vector<std::pair<std::int32_t, Registration>> targets;
        {
            std::lock_guard lock(mutex);
            for (auto& [token, registration] : tokens) {
                if (registration.name != name) continue;
                registration.posted = true;
                if (registration.kind != Kind::check) targets.emplace_back(token, registration);
            }
        }
        for (const auto& [token, registration] : targets) {
            switch (registration.kind) {
            case Kind::dispatch: deliver_dispatch(registration.queue, registration.block, token); break;
            case Kind::mach_port: static_cast<void>(ipc->post(registration.port, token)); break;   // a full queue drops it, as notifyd does
            case Kind::file: {
                // The token in network byte order (notify(3)).
                const auto cell = malloc(4);
                const auto value = static_cast<std::uint32_t>(token);
                memory().write32(cell, (value >> 24) | ((value >> 8) & 0xff00) | ((value << 8) & 0xff0000) | (value << 24));
                static_cast<void>(call(libsystem, "_write", {static_cast<std::uint32_t>(registration.write_fd), cell, 4}));
                free(cell);
                break;
            }
            default: break;
            }
        }
        return status_ok;
    }

    void install();
};

void NotifyProvider::install() {
    ipc = std::dynamic_pointer_cast<MachIpc>(mach_port_events(process));
    // dispatch_async_f trampoline: runs the registration's block with its token.
    trampoline = process.install_function("__alchemy_notify_dispatch", [weak = std::weak_ptr<Provider>(shared_from_this())](CpuState& s, GuestMemory& m) {
        const auto self = weak.lock();
        if (!self) return;
        const auto cell = s.x[0];
        const auto block = m.read64(cell), token = m.read64(cell + 8);
        self->free(cell);
        CallRegs registers{};
        registers.x[0] = block;
        registers.x[1] = token;
        static_cast<void>(EngineAdapter(self->process.calling_engine()).call_regs(m.read64(block + 16), registers));
        static_cast<void>(self->call(libsystem, "__Block_release", {block}));
    });
    // notify_post(name)
    raw(libsystem, "_notify_post", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0]) { s.x[0] = status_invalid_name; return; }
        s.x[0] = post(read_cstring(m, s.x[0]));
    });
    // notify_register_check(name, int *token)
    raw(libsystem, "_notify_register_check", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1]) { s.x[0] = status_null_input; return; }
        m.write32(s.x[1], static_cast<std::uint32_t>(add({read_cstring(m, s.x[0]), Kind::check})));
        s.x[0] = status_ok;
    });
    // notify_register_dispatch(name, int *token, dispatch_queue_t queue, notify_handler_t handler)
    static_cast<void>(process.install_function("_notify_register_dispatch", [owner = shared_from_this(), this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[2] || !s.x[3]) { s.x[0] = status_null_input; return; }
        Registration registration{read_cstring(m, s.x[0]), Kind::dispatch};
        registration.posted = false;
        registration.queue = s.x[2];
        registration.block = call(libsystem, "__Block_copy", {s.x[3]});
        static_cast<void>(call(libsystem, "_dispatch_retain", {s.x[2]}));
        m.write32(s.x[1], static_cast<std::uint32_t>(add(std::move(registration))));
        s.x[0] = status_ok;
    }));
    // notify_register_mach_port(name, mach_port_t *notify_port, int flags, int *token):
    // a new receive right, or the caller's (NOTIFY_REUSE); each post sends a
    // header-only message whose msgh_id is the token.
    raw(libsystem, "_notify_register_mach_port", [this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[3]) { s.x[0] = status_null_input; return; }
        if (!ipc) { s.x[0] = status_failed; return; }
        Name port = 0;
        if (static_cast<std::uint32_t>(s.x[2]) & notify_reuse) port = m.read32(s.x[1]);
        else if (ipc->allocate(right::receive, port) != kern::success) { s.x[0] = status_failed; return; }
        Registration registration{read_cstring(m, s.x[0]), Kind::mach_port};
        registration.posted = false;
        if (ipc->take(port, disp::make_send, registration.port) != kern::success) { s.x[0] = status_invalid_port; return; }
        m.write32(s.x[1], port);
        m.write32(s.x[3], static_cast<std::uint32_t>(add(std::move(registration))));
        s.x[0] = status_ok;
    });
    // notify_register_file_descriptor(name, int *notify_fd, int flags, int *token):
    // the read end of a pipe that receives each post's token (4 bytes).
    static_cast<void>(process.install_function("_notify_register_file_descriptor", [owner = shared_from_this(), this](CpuState& s, GuestMemory& m) {
        if (!s.x[0] || !s.x[1] || !s.x[3]) { s.x[0] = status_null_input; return; }
        Registration registration{read_cstring(m, s.x[0]), Kind::file};
        registration.posted = false;
        if (static_cast<std::uint32_t>(s.x[2]) & notify_reuse) {
            // Reusing a descriptor needs its write end, which only this
            // provider has for descriptors it made.
            const auto fd = static_cast<std::int32_t>(m.read32(s.x[1]));
            std::lock_guard lock(mutex);
            for (const auto& [token, other] : tokens)
                if (other.kind == Kind::file && other.read_fd == fd) { registration.read_fd = fd; registration.write_fd = other.write_fd; }
            if (registration.read_fd < 0) { s.x[0] = status_invalid_file; return; }
        } else {
            const auto fds = process.allocate_guest(8);
            const auto result = static_cast<std::int32_t>(call(libsystem, "_pipe", {fds}));
            registration.read_fd = static_cast<std::int32_t>(m.read32(fds));
            registration.write_fd = static_cast<std::int32_t>(m.read32(fds + 4));
            process.release_guest(fds);
            if (result != 0) { s.x[0] = status_failed; return; }
        }
        m.write32(s.x[1], static_cast<std::uint32_t>(registration.read_fd));
        m.write32(s.x[3], static_cast<std::uint32_t>(add(std::move(registration))));
        s.x[0] = status_ok;
    }));
    // notify_check(token, int *check): posted since the last check (true on the first call).
    raw(libsystem, "_notify_check", [this](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(mutex);
        const auto found = tokens.find(static_cast<std::int32_t>(s.x[0]));
        if (found == tokens.end()) { s.x[0] = status_invalid_token; return; }
        if (!s.x[1]) { s.x[0] = status_null_input; return; }
        m.write32(s.x[1], std::exchange(found->second.posted, false) ? 1 : 0);
        s.x[0] = status_ok;
    });
    // notify_get_state(token, uint64_t *state) / notify_set_state(token, uint64_t state): per name.
    raw(libsystem, "_notify_get_state", [this](CpuState& s, GuestMemory& m) {
        std::lock_guard lock(mutex);
        const auto found = tokens.find(static_cast<std::int32_t>(s.x[0]));
        if (found == tokens.end()) { s.x[0] = status_invalid_token; return; }
        if (!s.x[1]) { s.x[0] = status_null_input; return; }
        const auto state = states.find(found->second.name);
        m.write64(s.x[1], state == states.end() ? 0 : state->second);
        s.x[0] = status_ok;
    });
    raw(libsystem, "_notify_set_state", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = tokens.find(static_cast<std::int32_t>(s.x[0]));
        if (found == tokens.end()) { s.x[0] = status_invalid_token; return; }
        states[found->second.name] = s.x[1];
        s.x[0] = status_ok;
    });
    // notify_cancel(token): the registration's resources go away (a Mach
    // port's receive right stays with its owner; a pipe closes with its last token).
    static_cast<void>(process.install_function("_notify_cancel", [owner = shared_from_this(), this](CpuState& s, GuestMemory&) {
        Registration registration;
        bool last_user = true;
        {
            std::lock_guard lock(mutex);
            const auto found = tokens.find(static_cast<std::int32_t>(s.x[0]));
            if (found == tokens.end()) { s.x[0] = status_invalid_token; return; }
            registration = std::move(found->second);
            tokens.erase(found);
            for (const auto& [token, other] : tokens)
                if (other.kind == Kind::file && other.read_fd == registration.read_fd) last_user = false;
        }
        if (registration.kind == Kind::dispatch) {
            static_cast<void>(call(libsystem, "__Block_release", {registration.block}));
            static_cast<void>(call(libsystem, "_dispatch_release", {registration.queue}));
        } else if (registration.kind == Kind::mach_port) {
            ipc->drop(registration.port);
        } else if (registration.kind == Kind::file && last_user) {
            static_cast<void>(call(libsystem, "_close", {static_cast<std::uint32_t>(registration.write_fd)}));
        }
        s.x[0] = status_ok;
    }));
    // notify_is_valid_token(token) (replaces the export-fill kit's always-false stub).
    static_cast<void>(process.install_function("_notify_is_valid_token", [owner = shared_from_this(), this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        s.x[0] = tokens.contains(static_cast<std::int32_t>(s.x[0])) ? 1 : 0;
    }));
    static_cast<void>(status_invalid_request);
}

} // namespace

void install_notify(gui::InstallContext& context) {
    std::make_shared<NotifyProvider>(context)->install();
}

} // namespace alchemy::libsystem
