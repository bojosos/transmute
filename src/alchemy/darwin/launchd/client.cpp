#include "alchemy/darwin/launchd/client.hpp"

#include "alchemy/darwin/launchd/protocol.hpp"

#include "alchemy/alchemy_root.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/ipc.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/path.hpp"
#include "alchemy/process_tree.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace alchemy::launchd {
namespace wire {

bool send(host::Handle channel, const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> frame(4 + body.size());
    const auto size = static_cast<std::uint32_t>(body.size());
    std::memcpy(frame.data(), &size, 4);
    std::memcpy(frame.data() + 4, body.data(), body.size());
    std::size_t done = 0;
    while (done < frame.size()) {
        const auto wrote = host::ipc::write(channel, std::as_bytes(std::span(frame)).subspan(done));
        if (!wrote || !*wrote) return false;
        done += *wrote;
    }
    return true;
}

namespace {
bool read_exactly(host::Handle channel, std::uint8_t* out, std::size_t size) {
    std::size_t done = 0;
    while (done < size) {
        const auto got = host::ipc::read(channel, std::as_writable_bytes(std::span(out + done, size - done)));
        if (!got || !*got) return false;
        done += *got;
    }
    return true;
}
} // namespace

std::optional<std::vector<std::uint8_t>> receive(host::Handle channel) {
    std::uint32_t size = 0;
    if (!read_exactly(channel, reinterpret_cast<std::uint8_t*>(&size), 4)) return std::nullopt;
    if (size > 64U * 1024 * 1024) return std::nullopt;
    std::vector<std::uint8_t> body(size);
    if (size && !read_exactly(channel, body.data(), size)) return std::nullopt;
    return body;
}

} // namespace wire

namespace {

std::string lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

std::uint64_t fnv1a(std::string_view text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const auto c : text) { hash ^= static_cast<unsigned char>(c); hash *= 1099511628211ULL; }
    return hash;
}

// The root as one spelling: lower case, '/' separators, no trailing separator.
std::string root_text() {
    const auto root = configured_alchemy_root();
    if (!root) return "(no root)";
    auto text = lower(path_utf8(root->lexically_normal()));
    for (auto& c : text) if (c == '\\') c = '/';
    while (text.size() > 3 && text.back() == '/') text.pop_back();
    return text;
}

} // namespace

std::string pipe_name() {
    const auto user = host::process::environment("USERNAME").value_or(host::process::environment("USER").value_or("user"));
    const auto domain = host::process::environment("USERDOMAIN").value_or("");
    // One session per user, root and Alchemy executable: two builds never share one.
    const auto program = host::process::image_path(host::process::current_pid());
    char hash[17];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(
        fnv1a(lower(domain + "\\" + user) + "|" + root_text() + "|" + lower(program ? *program : std::string()))));
    return std::string("alchemy-launchd-") + hash;
}

Caller current_caller() {
    Caller caller;
    caller.pid = host::process::current_pid();
    if (const auto self = process_tree::self()) caller.pid = self->pid;
    // Under alchemy-fx the tree's root names itself in ALCHEMY_MACH_BROKER;
    // otherwise walk the published records up to a process the host started.
    if (const auto broker = host::process::environment("ALCHEMY_MACH_BROKER")) {
        caller.tree_root = static_cast<std::uint32_t>(std::strtoul(broker->c_str(), nullptr, 10));
    }
    if (!caller.tree_root) {
        auto pid = caller.pid;
        for (int depth = 0; depth < 64; ++depth) {
            const auto record = process_tree::lookup(pid);
            if (!record || record->ppid <= 1 || !process_tree::alive(record->ppid)) break;
            pid = record->ppid;
        }
        caller.tree_root = pid;
    }
    caller.tree_version = process_tree::pidversion(caller.tree_root);
    return caller;
}

bool Session::running() {
    auto channel = host::ipc::connect(pipe_name());
    if (!channel) return false;
    host::close(*channel);
    return true;
}

std::unique_ptr<Session> Session::open(bool start, const std::vector<std::string>& launch_options, std::string* error) {
    const auto name = pipe_name();
    const auto say = [&](std::string text) { if (error) *error = std::move(text); };
    auto channel = host::ipc::connect(name);
    if (!channel && channel.error() == host::Error::not_found && start) {
        // Start the server: the running Alchemy executable, detached from this
        // process (it outlives the application), with no console of its own.
        const auto self = host::process::image_path(host::process::current_pid());
        if (!self) { say("cannot read the Alchemy executable path"); return nullptr; }
        host::process::Spawn spawn;
        spawn.program = *self;
        spawn.arguments = {"launchd"};
        if (const auto root = configured_alchemy_root()) {
            spawn.arguments.push_back("--root");
            spawn.arguments.push_back(path_utf8(*root));
        }
        spawn.arguments.insert(spawn.arguments.end(), launch_options.begin(), launch_options.end());
        spawn.detached = true;
        // ALCHEMY_LAUNCHD_LOG=<host path>: the server's log (appended); none by default.
        std::optional<host::OwnedHandle> log;
        if (const auto path = host::process::environment("ALCHEMY_LAUNCHD_LOG"); path && !path->empty()) {
            if (auto file = host::files::open(*path, host::files::Access::write, host::files::Create::open_or_create, host::files::open_append)) {
                host::OwnedHandle opened(*file);
                if (auto inheritable = host::process::inheritable(opened.get())) {
                    log.emplace(*inheritable);
                    spawn.standard_output = spawn.standard_error = log->get();
                }
            }
        }
        auto started = host::process::spawn(spawn);
        if (!started) { say("cannot start the session launchd (" + std::string(host::name(started.error())) + ")"); return nullptr; }
        host::close(started->thread);
        // Wait for it to listen (or for another client's server to win the race).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        for (;;) {
            channel = host::ipc::connect(name);
            if (channel || channel.error() != host::Error::not_found) break;
            if (const auto code = host::process::exit_code(started->process); code) {
                // It ended: another server won the race, or it failed.
                channel = host::ipc::connect(name);
                if (!channel) say("the session launchd exited with status " + std::to_string(*code) + " before listening");
                break;
            }
            if (std::chrono::steady_clock::now() > deadline) { say("the session launchd did not start listening"); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        host::close(started->process);
    }
    if (!channel) {
        if (error && error->empty()) say(channel.error() == host::Error::not_found ? "no session launchd runs" : "cannot reach the session launchd");
        return nullptr;
    }
    std::unique_ptr<Session> session(new Session(*channel));
    const auto caller = current_caller();
    wire::Writer hello;
    hello.u32(static_cast<std::uint32_t>(wire::Op::hello)).u32(caller.pid).u32(caller.tree_root).u32(caller.tree_version);
    try { static_cast<void>(session->call(hello.bytes)); }
    catch (const std::exception& failure) { say(failure.what()); return nullptr; }
    return session;
}

Session::~Session() { host::close(channel_); }

std::vector<std::uint8_t> Session::call(const std::vector<std::uint8_t>& request) {
    if (!wire::send(channel_, request)) throw wire::WireError("launchd: the session launchd closed the connection");
    auto answer = wire::receive(channel_);
    if (!answer) throw wire::WireError("launchd: the session launchd closed the connection");
    return std::move(*answer);
}

namespace {
wire::Writer op(wire::Op which) { wire::Writer w; w.u32(static_cast<std::uint32_t>(which)); return w; }
}

std::int32_t Session::load(const Value& job, const std::string& guest_path, std::uint8_t flags, std::string& message) {
    auto request = op(wire::Op::load);
    request.text(to_xml(job)).text(guest_path).u8(flags);
    wire::Reader in(call(request.bytes));
    const auto status = in.i32();
    message = in.text();
    return status;
}

std::int32_t Session::remove(const std::string& label, std::string& message) {
    auto request = op(wire::Op::remove);
    request.text(label);
    wire::Reader in(call(request.bytes));
    const auto status = in.i32();
    message = in.text();
    return status;
}

std::vector<ListEntry> Session::list() {
    wire::Reader in(call(op(wire::Op::list).bytes));
    std::vector<ListEntry> out(in.u32());
    for (auto& entry : out) { entry.pid = in.i64(); entry.last_status = in.i32(); entry.label = in.text(); }
    return out;
}

std::optional<JobInfo> Session::info(const std::string& label) {
    auto request = op(wire::Op::info);
    request.text(label);
    wire::Reader in(call(request.bytes));
    if (in.i32() != 0) return std::nullopt;
    const auto dictionary = in.text(), state = in.text();
    const auto bytes = [](const std::string& text) { return std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()); };
    return JobInfo{parse_plist(bytes(dictionary)), parse_plist(bytes(state))};
}

std::int32_t Session::kickstart(const std::string& label, bool kill_first, std::uint32_t& pid, std::string& message) {
    auto request = op(wire::Op::kickstart);
    request.text(label).u8(kill_first ? 1 : 0);
    wire::Reader in(call(request.bytes));
    const auto status = in.i32();
    message = in.text();
    pid = in.u32();
    return status;
}

std::int32_t Session::signal(const std::string& label, std::uint32_t signal, std::string& message) {
    auto request = op(wire::Op::signal);
    request.text(label).u32(signal);
    wire::Reader in(call(request.bytes));
    const auto status = in.i32();
    message = in.text();
    return status;
}

std::int32_t Session::set_enabled(const std::string& label, bool enabled, std::string& message) {
    auto request = op(wire::Op::enable);
    request.text(label).u8(enabled ? 1 : 0);
    wire::Reader in(call(request.bytes));
    const auto status = in.i32();
    message = in.text();
    return status;
}

std::vector<std::pair<std::string, bool>> Session::overrides() {
    wire::Reader in(call(op(wire::Op::overrides).bytes));
    std::vector<std::pair<std::string, bool>> out(in.u32());
    for (auto& [label, disabled] : out) { label = in.text(); disabled = in.u8() != 0; }
    return out;
}

void Session::set_variable(const std::string& name, const std::optional<std::string>& value) {
    auto request = op(value ? wire::Op::setenv : wire::Op::unsetenv);
    request.text(name);
    if (value) request.text(*value);
    static_cast<void>(call(request.bytes));
}

std::optional<std::string> Session::variable(const std::string& name) {
    auto request = op(wire::Op::getenv);
    request.text(name);
    wire::Reader in(call(request.bytes));
    if (in.i32() != 0) return std::nullopt;
    return in.text();
}

std::int32_t Session::start(const std::string& label, std::string& message) {
    auto request = op(wire::Op::start);
    request.text(label);
    wire::Reader in(call(request.bytes));
    const auto status = in.i32();
    message = in.text();
    return status;
}

std::int32_t Session::stop(const std::string& label, std::string& message) {
    auto request = op(wire::Op::stop);
    request.text(label);
    wire::Reader in(call(request.bytes));
    const auto status = in.i32();
    message = in.text();
    return status;
}

} // namespace alchemy::launchd
