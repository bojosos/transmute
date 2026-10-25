// Host layer tests (include/alchemy/host/): the compiled-in backend's calls
// behave as their headers say. On the null backend every call must fail with
// Error::unsupported (or return its documented neutral value).
#include "alchemy/host/crypto.hpp"
#include "alchemy/host/dns_sd.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/host/host.hpp"
#include "alchemy/host/ipc.hpp"
#include "alchemy/host/http.hpp"
#include "alchemy/host/icu.hpp"
#include "alchemy/host/locale.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/process.hpp"
#include "alchemy/host/sections.hpp"
#include "alchemy/host/sync.hpp"
#include "alchemy/host/system.hpp"
#include "alchemy/host/time.hpp"
#include "alchemy/host/vm.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <atomic>
#include <deque>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <string>

namespace {

namespace host = alchemy::host;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

// Socket addresses: plain C++, the same on every backend.
// Result<T>::operator-> reaches the value even when T overloads unary &,
// as WRL's ComPtr does.
struct OverloadsAddress {
    int value{7};
    OverloadsAddress* operator&() noexcept { return nullptr; }
    const OverloadsAddress* operator&() const noexcept { return nullptr; }
};
void result_type() {
    host::Result<OverloadsAddress> result{OverloadsAddress{}};
    const auto& constant = result;
    require(result->value == 7 && constant->value == 7, "Result::operator-> with an overloaded unary &");
}

void addresses() {
    namespace net = host::net;
    const auto text = [](std::string_view host) {
        const auto address = net::parse_numeric(host);
        return address ? net::host_text(*address) : std::string("-");
    };
    require(text("192.0.2.1") == "192.0.2.1" && text("[::1]") == "::1" && text("::") == "::", "numeric literals");
    require(text("2001:0DB8:0:0:1:0:0:1") == "2001:db8::1:0:0:1" && text("1:0:0:2:0:0:0:3") == "1:0:0:2::3" &&
                text("1:2:3:4:5:6:7::") == "1:2:3:4:5:6:7:0" && text("::ffff:192.0.2.1") == "::ffff:192.0.2.1" &&
                text("1::") == "1::" && text("::1:2:3:4:5:6:7") == "0:1:2:3:4:5:6:7",
        "RFC 5952 text");
    for (const auto bad : {"01.2.3.4", "1.2.3", "256.1.1.1", "1.2.3.4.", "1::2::3", "1:2:3:4:5:6:7:8:9", ":1::", "1:", "12345::",
                           "1:2:3:4:5:6:7:8::", "example.com", "", "[]", "fe80::1%"})
        require(!net::parse_numeric(bad), std::string("not numeric: ") + bad);
    const auto scoped = net::parse_numeric("fe80::1%7", 443);
    require(scoped && scoped->scope_id == 7 && scoped->port == 443 && net::to_string(*scoped) == "[fe80::1]:443", "a scoped IPv6 address");
    require(net::to_string(net::Address::ipv4({192, 0, 2, 1}, 80)) == "192.0.2.1:80", "IPv4 with its port");
    require(net::is_loopback(*net::parse_numeric("127.3.2.1")) && net::is_loopback(*net::parse_numeric("::1")) &&
                net::is_loopback(*net::parse_numeric("::ffff:127.0.0.2")) && net::is_loopback(net::Address::local("/tmp/s")) &&
                !net::is_loopback(*net::parse_numeric("10.0.0.1")) && !net::is_loopback(*net::parse_numeric("::2")) &&
                !net::is_loopback(net::Address{}),
        "loopback addresses");
    require(net::loopback_host("localhost.") && net::loopback_host("LocalHost") && net::loopback_host("a.localhost") &&
                net::loopback_host("[::1]") && net::loopback_host("127.0.0.1") && !net::loopback_host("localhost.example") &&
                !net::loopback_host("x.localhost.com") && !net::loopback_host(".localhost") && !net::loopback_host("192.0.2.1"),
        "loopback hosts");
    require(net::Address::ipv4({1, 2, 3, 4}, 5) == *net::parse_numeric("1.2.3.4", 5), "address equality");
}

// Sockets over loopback: a stream pair, a datagram pair, names and options.
void sockets() {
    namespace net = host::net;
    const auto loopback = net::Address::ipv4({127, 0, 0, 1});
    const auto wait_ready = [](host::Handle socket, std::uint16_t events) {
        // Winsock reports a peer's FIN as hang-up rather than readable.
        for (int attempt = 0; attempt < 500; ++attempt) {
            const auto ready = net::ready(socket, events);
            if (ready && (*ready & (events | net::ready_hang_up))) return true;
            host::time::sleep_ns(10'000'000);
        }
        return false;
    };
    struct Socket {
        host::Handle handle;
        ~Socket() { net::close_socket(handle); }
    };

    // stream: listen, connect, accept, send, receive, shutdown
    auto listener_handle = net::open_socket(net::Family::ipv4, net::SocketType::stream);
    require(static_cast<bool>(listener_handle), "open a stream socket");
    Socket listener{*listener_handle};
    require(net::bind(listener.handle, loopback) && net::listen(listener.handle, 4), "bind and listen");
    const auto bound = net::local_address(listener.handle);
    require(bound && bound->family == net::Family::ipv4 && bound->port != 0 && net::is_loopback(*bound), "local_address of a listener");
    require(net::accept(listener.handle).error() == host::Error::would_block, "accept without a connection would block");
    Socket client{*net::open_socket(net::Family::ipv4, net::SocketType::stream, 6)};
    const auto connecting = net::connect(client.handle, *bound);
    require(connecting || connecting.error() == host::Error::would_block || connecting.error() == host::Error::in_progress,
        "a non-blocking connect is under way");
    require(wait_ready(client.handle, net::ready_writable), "the connect completes");
    const auto ended = net::pending_error(client.handle);
    require(ended && ended->error == host::Error::none, "the connect succeeded");
    require(wait_ready(listener.handle, net::ready_readable), "the listener is readable");
    auto accepted = net::accept(listener.handle);
    require(accepted && net::is_loopback(accepted->peer), "accept");
    Socket server{accepted->socket};
    require(*net::peer_address(client.handle) == *net::local_address(server.handle), "the two ends agree on addresses");
    const char text[] = "hello";
    require(*net::send(client.handle, std::as_bytes(std::span(text, 5))) == 5, "send");
    require(wait_ready(server.handle, net::ready_readable), "the server end is readable");
    require(*net::readable_bytes(server.handle) == 5, "readable_bytes");
    std::array<std::byte, 16> buffer{};
    require(*net::receive(server.handle, buffer, net::message_peek) == 5, "a peek");
    require(*net::receive(server.handle, buffer) == 5 && std::memcmp(buffer.data(), text, 5) == 0, "receive");
    require(net::receive(server.handle, buffer).error() == host::Error::would_block, "an empty socket would block");
    require(static_cast<bool>(net::shutdown(client.handle, net::Shutdown::send)), "shutdown");
    require(wait_ready(server.handle, net::ready_readable) && *net::receive(server.handle, buffer) == 0, "end of stream after shutdown");

    // options
    require(net::set_option(server.handle, net::Option::tcp_no_delay, 1) && *net::get_option(server.handle, net::Option::tcp_no_delay) != 0,
        "TCP_NODELAY");
    require(net::set_linger(server.handle, {true, 0}) && net::get_linger(server.handle)->on, "SO_LINGER");
    require(net::set_option(listener.handle, net::Option::reuse_address, 1) && *net::get_option(listener.handle, net::Option::accepting) != 0,
        "SO_REUSEADDR and SO_ACCEPTCONN");

    // datagrams: a truncated receive returns the prefix
    Socket first{*net::open_socket(net::Family::ipv4, net::SocketType::datagram)};
    Socket second{*net::open_socket(net::Family::ipv4, net::SocketType::datagram, 17)};
    require(net::bind(first.handle, loopback) && net::bind(second.handle, loopback), "bind datagram sockets");
    const auto second_address = *net::local_address(second.handle);
    require(*net::send_to(first.handle, std::as_bytes(std::span(text, 5)), second_address) == 5, "send_to");
    require(wait_ready(second.handle, net::ready_readable), "a datagram arrives");
    std::array<std::byte, 3> small{};
    const auto datagram = net::receive_from(second.handle, small);
    require(datagram && datagram->size == 3 && datagram->from == *net::local_address(first.handle), "receive_from truncates");

    // a waiting ready: it times out on an idle socket, and wakes as soon as a
    // datagram arrives
    auto started = host::time::monotonic_ns();
    const auto idle = net::ready(second.handle, net::ready_readable, 120'000'000);
    auto waited = host::time::monotonic_ns() - started;
    require(idle && *idle == 0 && waited >= 100'000'000 && waited < 2'000'000'000, "ready waits out its timeout on an idle socket");
    std::thread sender([&] {
        host::time::sleep_ns(50'000'000);
        static_cast<void>(net::send_to(first.handle, std::as_bytes(std::span(text, 5)), second_address));
    });
    started = host::time::monotonic_ns();
    const auto woken = net::ready(second.handle, net::ready_readable, 10'000'000'000ULL);
    waited = host::time::monotonic_ns() - started;
    sender.join();
    require(woken && (*woken & net::ready_readable) && waited < 5'000'000'000ULL, "ready wakes when a datagram arrives");
    require(static_cast<bool>(net::receive_from(second.handle, buffer)) && *net::ready(second.handle, net::ready_readable, 0) == 0,
        "a zero timeout polls");

    // export and import within this process
    const auto blob = net::export_socket(first.handle, 0);
    require(blob && !blob->empty(), "export_socket");
    const auto imported = net::import_socket(*blob);
    require(static_cast<bool>(imported), "import_socket");
    Socket copy{*imported};
    require(*net::local_address(copy.handle) == *net::local_address(first.handle), "an imported socket is the same socket");

    // names
    const auto name = net::host_name();
    require(name && !name->empty(), "host_name");
    net::Hints hints;
    hints.family = net::Family::ipv4;
    hints.type = net::SocketType::stream;
    hints.flags = net::resolve_numeric_host | net::resolve_numeric_service;
    const auto resolved = net::resolve("127.0.0.1", "80", hints);
    require(resolved && !resolved->empty() && (*resolved)[0].address == net::Address::ipv4({127, 0, 0, 1}, 80) &&
                (*resolved)[0].type == net::SocketType::stream,
        "resolve a numeric host and service");
    require(net::resolve("not a host", std::nullopt, hints).error() != host::Error::none, "a bad numeric host fails");
    const auto numeric = net::name_info(net::Address::ipv4({127, 0, 0, 1}, 80), net::name_numeric_host | net::name_numeric_service);
    require(numeric && numeric->host == "127.0.0.1" && numeric->service == "80", "name_info numeric");
    const auto tcp = net::protocol_by_name("tcp");
    require(tcp && tcp->number == 6, "protocol_by_name");
    const auto http = net::service_by_name("http", "tcp");
    require(http && http->port == 80 && net::service_by_port(80, "tcp")->name == http->name, "the services database");
    require(net::open_socket(net::Family::local, net::SocketType::datagram).error() == host::Error::socket_type, "no local datagrams");
}

// What the libSystem descriptor table needs of an open file.
void file_table(const std::string& directory) {
    namespace files = host::files;
    const auto path = directory + "/alchemy-table-" + std::to_string(host::process::current_pid());
    std::filesystem::remove(path);
    auto opened = files::open(path, files::Access::read_write, files::Create::create_new, files::open_owner_only | files::open_deletable);
    require(static_cast<bool>(opened), "open owner-only and deletable");
    host::OwnedHandle file(*opened);
    require(files::open(path, files::Access::read, files::Create::create_new).error() == host::Error::exists, "create_new of an existing file");
    {
        auto sequential = files::open(path, files::Access::read, files::Create::open_existing, files::open_sequential);
        require(static_cast<bool>(sequential), "open_sequential");
        host::close(*sequential);
        auto appender = files::open(path, files::Access::write, files::Create::open_existing, files::open_append);
        require(static_cast<bool>(appender), "open_append");
        require(*files::write(*appender, std::as_bytes(std::span("zz", 2))) == 2, "an append-only write");
        host::close(*appender);
        require(*files::size(file.get()) == 2 && files::set_size(file.get(), 0), "the append landed at the end");
    }
    require(*files::write(file.get(), std::as_bytes(std::span("abcdef", 6))) == 6 && *files::seek(file.get(), 0, files::Whence::current) == 6,
        "write moves the position");
    require(*files::seek(file.get(), 2, files::Whence::set) == 2 && *files::seek(file.get(), -1, files::Whence::end) == 5, "seek");
    require(files::seek(file.get(), -10, files::Whence::set).error() == host::Error::invalid_argument, "a negative position");
    require(files::set_size(file.get(), 3) && *files::size(file.get()) == 3 && *files::seek(file.get(), 0, files::Whence::current) == 5,
        "set_size keeps the position");
    auto copy = host::duplicate(file.get());
    require(copy && *files::seek(*copy, 1, files::Whence::set) == 1 && *files::seek(file.get(), 0, files::Whence::current) == 1,
        "duplicates share the position");
    host::close(*copy);
    require(static_cast<bool>(files::flush(file.get())), "flush");
    const auto by_handle = files::metadata(file.get());
    const auto by_path = files::metadata(path);
    require(by_handle && by_path && by_handle->inode == by_path->inode && by_handle->device == by_path->device && by_path->size == 3 &&
                by_path->links == 1 && !by_path->directory && by_path->owner_only && by_path->modified_ns > 1'600'000'000'000'000'000LL,
        "metadata of an owner-only file");
    const auto first = files::identity(file.get());
    require(first && first->id != std::array<std::uint8_t, 16>{}, "identity");
    require(!files::is_character_device(file.get()), "a file is not a character device");
    // locks: a second handle of the same file conflicts
    auto other = files::open(path, files::Access::read, files::Create::open_existing);
    require(static_cast<bool>(other), "a second handle");
    host::OwnedHandle second(*other);
    require(files::identity(second.get())->id == first->id, "both handles have one identity");
    require(files::lock(file.get(), 0, 1, true) && files::lock(second.get(), 0, 1, false).error() == host::Error::would_block,
        "an exclusive lock blocks a shared one");
    require(files::unlock(file.get(), 0, 1) && files::lock(second.get(), 0, 1, false) && files::unlock(second.get(), 0, 1), "unlock");
    // read-only and directories
    require(files::set_read_only(path, true) && files::metadata(path)->read_only && files::set_read_only(path, false), "set_read_only");
    const auto folder = path + ".dir";
    require(files::create_directory(folder, true) && files::metadata(folder)->directory && files::metadata(folder)->owner_only,
        "create_directory owner-only");
    require(files::create_directory(folder).error() == host::Error::exists && files::sync_directory(folder), "sync_directory");
    require(files::sync_directory(folder + "/missing").error() == host::Error::not_found, "sync_directory of a missing directory");
    std::filesystem::remove(folder);
    require(files::error(std::make_error_code(std::errc::no_such_file_or_directory)) == host::Error::not_found, "files::error");
    // times, rename, access, attributes
    require(files::set_times(path, 1'000'000'000LL * 1'500'000'000, 1'000'000'000LL * 1'600'000'000) &&
                files::metadata(path)->modified_ns == 1'000'000'000LL * 1'600'000'000 &&
                files::metadata(path)->accessed_ns == 1'000'000'000LL * 1'500'000'000,
        "set_times");
    require(files::set_times(path, std::nullopt, 1'000'000'000LL * 1'700'000'000) &&
                files::metadata(path)->accessed_ns == 1'000'000'000LL * 1'500'000'000,
        "set_times leaves an absent time");
    require(files::access(path, true) && files::set_read_only(path, true) && files::access(path, false) &&
                files::access(path, true).error() == host::Error::access_denied && files::set_read_only(path, false),
        "access");
    require(files::access(path + ".missing", false).error() == host::Error::not_found, "access of a missing path");
    const char value[] = "attribute value";
    require(!*files::has_attribute(path, "com.apple.metadata:kMDItemWhereFroms") &&
                files::set_attribute(path, "com.apple.metadata:kMDItemWhereFroms", std::as_bytes(std::span(value, 15))) &&
                *files::has_attribute(path, "com.apple.metadata:kMDItemWhereFroms"),
        "set_attribute");
    const auto stored = files::get_attribute(path, "com.apple.metadata:kMDItemWhereFroms");
    require(stored && stored->size() == 15 && std::memcmp(stored->data(), value, 15) == 0, "get_attribute");
    const auto names = files::list_attributes(path);
    require(names && names->size() == 1 && (*names)[0] == "com.apple.metadata:kMDItemWhereFroms", "list_attributes maps ':' back");
    require(files::remove_attribute(path, "com.apple.metadata:kMDItemWhereFroms") &&
                files::get_attribute(path, "com.apple.metadata:kMDItemWhereFroms").error() == host::Error::not_found &&
                files::remove_attribute(path, "missing").error() == host::Error::not_found,
        "remove_attribute");
    const auto moved = path + ".moved";
    {
        auto target = files::open(moved, files::Access::write, files::Create::create_new);
        require(static_cast<bool>(target), "a rename target");
        host::close(*target);
    }
    require(files::rename(moved, moved + "2", false) && files::attributes(moved + "2"),
        "rename");
    {
        auto again = files::open(moved, files::Access::write, files::Create::create_new);
        host::close(*again);
    }
    require(files::rename(moved, moved + "2", false).error() == host::Error::exists && files::rename(moved, moved + "2", true), "rename replace");
    std::filesystem::remove(moved + "2");
    require(!files::is_console(file.get()), "a file is not a console");

    // links and trees
    {
        const auto root = path + ".tree";
        require(files::create_directory(root) && files::create_directory(root + "/inner"), "a tree");
        {
            auto leaf = files::open(root + "/inner/leaf", files::Access::write, files::Create::create_new);
            require(leaf && files::write(*leaf, std::as_bytes(std::span("leaf", 4))), "a leaf file");
            host::close(*leaf);
        }
        require(static_cast<bool>(files::set_read_only(root + "/inner/leaf", true)), "a read-only leaf");
        require(files::create_hard_link(root + "/hard", root + "/inner/leaf") && files::metadata(root + "/hard")->links == 2, "create_hard_link");
        {
            const auto junction = files::create_junction(root + "/junction", root + "/inner");
            require(static_cast<bool>(junction), std::string("create_junction: ") + std::string(host::name(junction.error())) + " " + root);
        }
        require(std::filesystem::exists(std::filesystem::path(root + "/junction/leaf")), "create_junction: the leaf through it");
        require(files::create_junction(root + "/relative", "inner").error() == host::Error::invalid_argument, "a junction needs an absolute target");
        // A symbolic link needs developer mode or a privilege on Windows: either it works or the host refuses it.
        const auto symlink = files::create_symbolic_link(root + "/symbolic", "inner/leaf", false);
        require(symlink || symlink.error() == host::Error::permission || symlink.error() == host::Error::access_denied, "create_symbolic_link");
        // A junction to the outside: remove_tree removes the junction, not what it points at.
        const auto outside = path + ".outside";
        require(files::create_directory(outside) && files::create_junction(root + "/out", outside), "a junction out of the tree");
        require(files::remove_tree(root) && !std::filesystem::exists(std::filesystem::path(root)), "remove_tree");
        require(std::filesystem::exists(std::filesystem::path(outside)), "remove_tree does not follow junctions");
        require(files::remove_tree(outside) && files::remove_tree(outside), "remove_tree of a missing path succeeds");
    }

    // unlink while open
    require(files::remove_open(file.get()) && files::open(path, files::Access::read, files::Create::open_existing).error() != host::Error::none,
        "remove_open: the name can no longer be opened");
    host::close(second.release());
    host::close(file.release());
    require(files::attributes(path).error() == host::Error::not_found, "remove_open: the name is gone once the handles close");
    // pipes
    const auto pipe = files::create_pipe();
    require(static_cast<bool>(pipe), "create_pipe");
    host::OwnedHandle reader(pipe->read), writer(pipe->write);
    require(*files::write(writer.get(), std::as_bytes(std::span("xyz", 3))) == 3 && *files::pipe_available(reader.get()) == 3, "pipe_available");
    std::array<std::byte, 8> buffer{};
    require(*files::read(reader.get(), buffer) == 3, "pipe read");
    host::close(writer.release());
    require(*files::read(reader.get(), buffer) == 0, "a pipe without writers reads end of file");
    // the null device
    auto null = files::open_null(files::Access::read_write);
    require(static_cast<bool>(null), "open_null");
    host::OwnedHandle device(*null);
    require(files::is_character_device(device.get()) && *files::read(device.get(), buffer) == 0 &&
                *files::write(device.get(), std::as_bytes(std::span("x", 1))) == 1,
        "the null device");
}

// This test program run as a pipe child: copies stdin to stdout, writes its
// arguments to stderr and exits with 3.
int pipe_child(int argc, char** argv) {
    const auto input = host::process::standard_handle(host::process::Standard::input);
    const auto output = host::process::standard_handle(host::process::Standard::output);
    const auto error = host::process::standard_handle(host::process::Standard::error);
    std::array<std::byte, 4096> buffer{};
    for (;;) {
        const auto count = host::files::read(input, buffer);
        if (!count || !*count) break;
        static_cast<void>(host::files::write(output, std::span(buffer.data(), *count)));
    }
    for (int index = 2; index < argc; ++index) {
        const std::string argument = std::string(argv[index]) + "|";
        static_cast<void>(host::files::write(error, std::as_bytes(std::span(argument.data(), argument.size()))));
    }
    return 3;
}

void spawn_piped(const std::string& self) {
    const std::vector<std::string> arguments{"--pipe-child", "plain", "with space", "quote\"inside", "trailing\\"};
    auto child = host::process::spawn_piped(self, arguments);
    require(child && child->pid != 0 && child->process && child->input && child->output && child->error, "spawn_piped");
    host::OwnedHandle process(child->process), input(child->input), output(child->output), error(child->error);
    require(host::process::exit_code(process.get()).error() == host::Error::busy, "a child blocked on stdin is running");
    const char text[] = "to the child";
    require(*host::files::write(input.get(), std::as_bytes(std::span(text, 12))) == 12, "write the child's stdin");
    host::close(input.release());
    const auto drain = [](host::Handle handle) {
        std::string all;
        std::array<std::byte, 256> buffer{};
        for (;;) {
            const auto count = host::files::read(handle, buffer);
            if (!count || !*count) break;
            all.append(reinterpret_cast<const char*>(buffer.data()), *count);
        }
        return all;
    };
    require(drain(output.get()) == "to the child", "read the child's stdout");
    require(drain(error.get()) == "plain|with space|quote\"inside|trailing\\|", "arguments arrive unchanged");
    const std::array processes{process.get()};
    require(static_cast<bool>(host::sync::wait_any(processes, 10'000'000'000ULL)) && *host::process::exit_code(process.get()) == 3,
        "the child's exit code");
    require(host::process::spawn_piped(self + ".missing", {}).error() == host::Error::not_found, "spawn_piped of a missing program");
}

// This test program run by spawn_general: reads the inheritable section
// whose handle value is its second argument and exits with its first byte.
int section_child(char** argv) {
    const auto value = std::stoull(argv[2], nullptr, 16);
    const auto view = host::sections::map(host::Handle{static_cast<std::uintptr_t>(value)}, 0, 4096, false);
    if (!view) return 100;
    return *reinterpret_cast<const unsigned char*>(*view);
}

void spawn_general(const std::string& self) {
    auto section = host::sections::create_section(4096, true);
    require(static_cast<bool>(section), "an inheritable section");
    host::OwnedHandle owned_section(*section);
    char text[32];
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(owned_section.get().value));
    host::process::Spawn options;
    options.program = self;
    options.arguments = {"--section-child", text};
    options.inherit = {owned_section.get()};
    options.suspended = true;
    options.end_with_parent = true;
    auto child = host::process::spawn(options);
    require(child && child->pid && child->process && child->thread, "spawn suspended");
    host::OwnedHandle process(child->process), thread(child->thread);
    // The child is suspended: the section is written before it reads it.
    const auto view = host::sections::map(owned_section.get(), 0, 4096, true);
    require(static_cast<bool>(view), "map the section");
    *reinterpret_cast<unsigned char*>(*view) = 42;
    static_cast<void>(host::sections::unmap(*view));
    require(host::process::exit_code(process.get()).error() == host::Error::busy, "a suspended child runs");
    require(static_cast<bool>(host::process::resume(thread.get())), "resume");
    const std::array processes{process.get()};
    require(static_cast<bool>(host::sync::wait_any(processes, 10'000'000'000ULL)) && *host::process::exit_code(process.get()) == 42,
        "the child read the inherited section");
    const auto copy = host::process::inheritable(owned_section.get());
    require(copy && copy->value != owned_section.get().value, "inheritable");
    host::close(*copy);
    host::process::Spawn missing;
    missing.program = self + ".missing";
    require(host::process::spawn(missing).error() == host::Error::not_found, "spawn of a missing program");
}

// A listener, a client, both directions, a second listener, cancel.
void channels() {
    namespace ipc = host::ipc;
    const auto name = "alchemy-host-test-" + std::to_string(host::process::current_pid());
    require(ipc::connect(name).error() == host::Error::not_found, "connect without a server");
    auto first = ipc::listen(name, true);
    require(static_cast<bool>(first), "listen");
    host::OwnedHandle listener(*first);
    require(ipc::listen(name, true).error() == host::Error::exists, "a second first listener");
    host::Handle accepted_channel;
    std::thread server([&] { if (ipc::accept(listener.get())) accepted_channel = listener.get(); });
    auto client = ipc::connect(name);
    require(static_cast<bool>(client), "connect");
    host::OwnedHandle channel(*client);
    server.join();
    require(static_cast<bool>(accepted_channel), "accept");
    const char hello[] = "hello";
    require(*ipc::write(channel.get(), std::as_bytes(std::span(hello, 5))) == 5, "write to the server");
    std::array<std::byte, 16> buffer{};
    require(*ipc::read(accepted_channel, buffer) == 5 && std::memcmp(buffer.data(), hello, 5) == 0, "the server reads");
    require(*ipc::write(accepted_channel, std::as_bytes(std::span(hello, 2))) == 2 && *ipc::read(channel.get(), buffer) == 2,
        "the client reads the answer");
    // A blocked read ends with 0 when cancelled.
    std::size_t cancelled_result = 99;
    std::thread reader([&] { cancelled_result = ipc::read(channel.get(), buffer).value_or(98); });
    host::time::sleep_ns(50'000'000);
    ipc::cancel(channel.get());
    reader.join();
    require(cancelled_result == 0, "cancel ends a blocked read");
    // The peer closing ends the other side's read with 0.
    host::close(listener.release());
    require(ipc::read(channel.get(), buffer).value_or(98) == 0, "a closed peer reads 0");
}

void null_backend() {
    require(host::name(host::Error::unsupported) == "unsupported", "error names");
    require(host::sync::create_event(true, false).error() == host::Error::unsupported, "null create_event fails");
    require(host::files::open("x", host::files::Access::read, host::files::Create::open_existing).error() == host::Error::unsupported,
        "null open fails");
    require(host::process::usage().error() == host::Error::unsupported && host::process::list().error() == host::Error::unsupported,
        "null process queries fail");
    require(host::time::process_cpu_ns().error() == host::Error::unsupported, "null CPU time fails");
    require(host::process::environment_block().empty() && host::process::image_path(1).error() == host::Error::unsupported,
        "null process queries are empty");
    require(host::net::interfaces().error() == host::Error::unsupported && host::net::dns_configuration().error() == host::Error::unsupported &&
                host::net::best_interface(host::net::Address::ipv4({127, 0, 0, 1})).error() == host::Error::unsupported,
        "null interfaces fail");
    require(host::net::open_socket(host::net::Family::ipv4, host::net::SocketType::stream).error() == host::Error::unsupported &&
                host::net::resolve("localhost", std::nullopt, {}).error() == host::Error::unsupported &&
                host::net::host_name().error() == host::Error::unsupported &&
                host::net::ready(host::Handle{}, host::net::ready_readable, 1).error() == host::Error::unsupported,
        "null sockets and names fail");
    require(host::files::attributes("x").error() == host::Error::unsupported && host::files::volume("x").error() == host::Error::unsupported &&
                host::files::times("x").error() == host::Error::unsupported && host::files::replace("x", "y").error() == host::Error::unsupported &&
                host::files::open_metadata("x").error() == host::Error::unsupported &&
                host::files::open_null(host::files::Access::read).error() == host::Error::unsupported &&
                host::files::metadata("x").error() == host::Error::unsupported && host::files::create_pipe().error() == host::Error::unsupported &&
                host::files::watch_directory("x").error() == host::Error::unsupported &&
                host::files::watch_tree("x").error() == host::Error::unsupported &&
                host::files::volume_roots().error() == host::Error::unsupported &&
                host::files::create_temporary("x").error() == host::Error::unsupported,
        "null file queries fail");
    require(host::sync::create_mutex("x").error() == host::Error::unsupported &&
                host::sync::create_semaphore("x", 0, 1).error() == host::Error::unsupported &&
                host::sections::open_named("x", 4096).error() == host::Error::unsupported,
        "null named objects fail");
    require(host::process::current_thread_id() != 0, "null thread id");
    require(host::process::thread_usage().error() == host::Error::unsupported, "null thread usage fails");
    require(host::process::memory_usage().error() == host::Error::unsupported, "null memory usage fails");
    require(host::system::cpu_times().error() == host::Error::unsupported &&
                host::system::processor_times().error() == host::Error::unsupported &&
                host::system::machine_uuid().error() == host::Error::unsupported &&
                host::system::host_name().error() == host::Error::unsupported &&
                host::system::user_name().error() == host::Error::unsupported &&
                host::system::PowerRequest::create("test", true, false).error() == host::Error::unsupported &&
                host::system::power_status().error() == host::Error::unsupported &&
                host::system::watch_sleep([](bool) {}).error() == host::Error::unsupported &&
                host::system::watch_power_source([] {}).error() == host::Error::unsupported,
        "null system queries fail");
    require(host::vm::lock(0, 1).error() == host::Error::unsupported, "null vm::lock fails");
    require(host::locale::normalize(u"a", host::locale::Form::d).error() == host::Error::unsupported, "null normalize fails");
    require(host::time::Timer::create([] {}).error() == host::Error::unsupported, "null timers fail");
    require(host::dns_sd::available() == host::dns_sd::Failure::not_running, "null has no DNS-SD client");
    require(host::crypto::protect({}, {}, "x").error() == host::Error::unsupported &&
                host::crypto::BlockCipher::create(host::crypto::BlockAlgorithm::aes, {}).error() == host::Error::unsupported &&
                host::crypto::generate_ec(32).error() == host::Error::unsupported && !host::crypto::supports(host::crypto::Digest::sha256) &&
                host::crypto::Identity::self_signed("x", {}).error() == host::Error::unsupported &&
                host::crypto::TlsSession::create({}).error() == host::Error::unsupported,
        "null crypto fails");
    require(!host::locale::valid("en-US") && host::locale::locales().error() == host::Error::unsupported, "null knows no locale");
    require(host::http::Session::open("test", {}).error() == host::Error::unsupported &&
                host::http::user_proxy().error() == host::Error::unsupported,
        "null HTTP fails");
}

void http_area() {
    namespace http = host::http;
    const auto proxy = http::user_proxy();
    require(proxy || proxy.error() == host::Error::unsupported, "user proxy settings");
    auto session = http::Session::open("Alchemy host test", {});
    require(static_cast<bool>(session), "open an HTTP session");
    // Port 1 on the loopback address: nothing listens, so the connection is refused.
    http::Target target{false, "127.0.0.1", 1, "GET", "/"};
    auto refused = http::Request::open(*session, target, {{"X-Test", "1"}}, 5000, {});
    const auto head = refused.send({});
    require(!head && refused.failure().kind == http::Failure::Kind::cannot_connect, "a refused connection is cannot_connect");
    auto cancelled = http::Request::open(*session, target, {}, 5000, {});
    cancelled.cancel();
    require(!cancelled.send({}) && cancelled.failure().kind == http::Failure::Kind::cancelled, "a cancelled request fails as cancelled");
    auto invalid = http::Request::open(*session, {false, "", 80, "GET", "/"}, {}, 5000, {});
    require(!invalid.send({}) && invalid.failure().kind != http::Failure::Kind::none, "a request without a host fails");
}

void crypto_area() {
    namespace crypto = host::crypto;
    // DPAPI round trip; the wrong entropy does not unseal.
    const std::string secret = "keychain contents";
    const std::string entropy = "entropy", other = "other";
    const auto sealed = crypto::protect(std::as_bytes(std::span(secret)), std::as_bytes(std::span(entropy)), "Alchemy host test");
    require(sealed && !sealed->empty(), "protect");
    const auto opened = crypto::unprotect(std::as_bytes(std::span(*sealed)), std::as_bytes(std::span(entropy)));
    require(opened && std::string(opened->begin(), opened->end()) == secret, "unprotect");
    require(!crypto::unprotect(std::as_bytes(std::span(*sealed)), std::as_bytes(std::span(other))), "unprotect with other entropy fails");
    std::array<std::byte, 4> wipe{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    crypto::secure_zero(wipe);
    require(wipe == std::array<std::byte, 4>{}, "secure_zero");
    // FIPS-197 appendix C.1: AES-128.
    const std::array<std::uint8_t, 16> key{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const std::array<std::uint8_t, 16> plain{0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    const std::array<std::uint8_t, 16> expected{0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    auto cipher = crypto::BlockCipher::create(crypto::BlockAlgorithm::aes, std::as_bytes(std::span(key)));
    require(cipher && cipher->block_size() == 16, "AES cipher");
    std::array<std::uint8_t, 16> out{}, back{};
    require(cipher->encrypt(std::as_bytes(std::span(plain)), std::as_writable_bytes(std::span(out))) && out == expected, "AES-128 encrypt");
    require(cipher->decrypt(std::as_bytes(std::span(out)), std::as_writable_bytes(std::span(back))) && back == plain, "AES-128 decrypt");
    require(crypto::BlockCipher::create(crypto::BlockAlgorithm::des, std::as_bytes(std::span(key))).error() == host::Error::invalid_argument,
        "a DES key of the wrong size");
    // RFC 6070 test 2: PBKDF2-HMAC-SHA1("password", "salt", 2, 20).
    const std::string password = "password", salt = "salt";
    std::array<std::uint8_t, 20> derived{};
    const std::array<std::uint8_t, 20> rfc6070{0xea, 0x6c, 0x01, 0x4d, 0xc7, 0x2d, 0x6f, 0x8c, 0xcd, 0x1e,
                                               0xd9, 0x2a, 0xce, 0x1d, 0x41, 0xf0, 0xd8, 0xde, 0x89, 0x57};
    require(crypto::pbkdf2(crypto::Digest::sha1, std::as_bytes(std::span(password)), std::as_bytes(std::span(salt)), 2,
                           std::as_writable_bytes(std::span(derived))) && derived == rfc6070, "PBKDF2-HMAC-SHA1");

    // Digests: SHA-256("abc") (FIPS 180-2 appendix B.1); SHA-224 is not provided by every host.
    const std::string abc = "abc";
    const auto sha256 = crypto::hash(crypto::Digest::sha256, std::as_bytes(std::span(abc)));
    require(sha256 && sha256->size() == 32 && (*sha256)[0] == 0xba && (*sha256)[31] == 0xad, "SHA-256");
    require(crypto::supports(crypto::Digest::sha1) && crypto::supports(crypto::Digest::md5), "SHA-1 and MD5 are provided");

    // EC keys: ECDSA round trip, and both sides of ECDH agree.
    const auto alice = crypto::generate_ec(32), bob = crypto::generate_ec(32);
    require(alice && bob && alice->x.size() == 32 && alice->k.size() == 32, "generate P-256 keys");
    require(static_cast<bool>(crypto::check(*alice, true)) && static_cast<bool>(crypto::check(*alice, false)), "check an EC key");
    const auto signature = crypto::ecdsa_sign(*alice, std::as_bytes(std::span(*sha256)));
    require(signature && signature->size() == 64, "ECDSA sign");
    require(static_cast<bool>(crypto::ecdsa_verify(*alice, std::as_bytes(std::span(*sha256)), std::as_bytes(std::span(*signature)))), "ECDSA verify");
    auto tampered = *signature;
    tampered[5] ^= 1;
    require(!crypto::ecdsa_verify(*alice, std::as_bytes(std::span(*sha256)), std::as_bytes(std::span(tampered))), "a tampered ECDSA signature fails");
    auto alice_public = *alice, bob_public = *bob;
    alice_public.k.clear();
    bob_public.k.clear();
    const auto ab = crypto::ecdh(*alice, bob_public), ba = crypto::ecdh(*bob, alice_public);
    require(ab && ba && ab->size() == 32 && *ab == *ba, "ECDH agreement");
    auto broken = alice_public;
    broken.coordinate = 20;
    require(crypto::check(broken, false).error() == host::Error::bad_handle, "a key the host refuses is bad_handle");

    // RSA: PKCS#1 v1.5 and PSS signatures, OAEP and raw encryption.
    const auto rsa = crypto::generate_rsa(2048);
    require(rsa && rsa->bits == 2048 && rsa->n.size() == 256 && !rsa->d.empty(), "generate an RSA key");
    using crypto::RsaPadding;
    for (const auto padding : {RsaPadding::pkcs1, RsaPadding::pss}) {
        const auto signed_hash = crypto::rsa_sign(*rsa, padding, crypto::Digest::sha256, std::as_bytes(std::span(*sha256)));
        require(signed_hash && signed_hash->size() == 256, "RSA sign");
        require(static_cast<bool>(crypto::rsa_verify(*rsa, padding, crypto::Digest::sha256, std::as_bytes(std::span(*sha256)),
                                                     std::as_bytes(std::span(*signed_hash)))), "RSA verify");
    }
    const auto sealed_key = crypto::rsa_encrypt(*rsa, RsaPadding::oaep, crypto::Digest::sha1, std::as_bytes(std::span(abc)));
    require(sealed_key && sealed_key->size() == 256, "RSA-OAEP encrypt");
    const auto unsealed_key = crypto::rsa_decrypt(*rsa, RsaPadding::oaep, crypto::Digest::sha1, std::as_bytes(std::span(*sealed_key)));
    require(unsealed_key && std::string(unsealed_key->begin(), unsealed_key->end()) == abc, "RSA-OAEP decrypt");
    std::vector<std::uint8_t> block(256, 0);
    block[255] = 2;
    const auto raw = crypto::rsa_decrypt(*rsa, RsaPadding::none, std::nullopt, std::as_bytes(std::span(block)));
    require(static_cast<bool>(raw), "raw RSA private operation");
    std::vector<std::uint8_t> raw_block(256 - raw->size(), 0);
    raw_block.insert(raw_block.end(), raw->begin(), raw->end());
    const auto back_again = crypto::rsa_encrypt(*rsa, RsaPadding::none, std::nullopt, std::as_bytes(std::span(raw_block)));
    require(back_again && !back_again->empty() && back_again->back() == 2, "raw RSA public operation inverts the private one");
}

void certificate_area() {
    namespace crypto = host::crypto;
    // A self-signed identity: its certificate parses back with its names.
    auto identity = crypto::Identity::self_signed("alchemy.host.test", {"alchemy.host.test", "localhost"});
    require(identity && *identity, "self-signed identity");
    const auto certificate = identity->certificate();
    require(certificate && certificate.common_name() == "alchemy.host.test" && certificate.self_issued(), "the identity's certificate");
    const auto names = certificate.dns_names();
    require(names.size() == 2 && names[0] == "alchemy.host.test" && names[1] == "localhost", "subjectAltName DNS names");
    const auto der = certificate.der();
    const auto parsed = crypto::Certificate::parse(std::as_bytes(std::span(der)));
    require(parsed && parsed->der() == der && parsed->issuer() == parsed->subject() && !parsed->serial_number().empty(), "parse a certificate");
    const auto now = host::time::realtime_ns();
    require(parsed->not_before_ns() < now && parsed->not_after_ns() > now, "validity period");
    const std::array<std::uint8_t, 3> junk{1, 2, 3};
    require(crypto::Certificate::parse(std::as_bytes(std::span(junk))).error() == host::Error::invalid_argument, "junk is not a certificate");
    require(!crypto::Identity::is_pkcs12(std::as_bytes(std::span(der))), "a certificate is not PKCS#12");
    const auto copy = *parsed;
    require(copy.der() == der, "copies share the certificate");

    // Chains: anchored at itself, the SSL policy passes for its name and not for another.
    crypto::ChainRequest request;
    request.leaf = *parsed;
    request.anchors.push_back(*parsed);
    request.policies.push_back({true, true, "localhost"});
    request.network_fetch = false;
    const auto good = crypto::evaluate_chain(request);
    require(good && good->failure == crypto::ChainFailure::none && good->chain.size() == 1, "a chain to its own anchor");
    request.policies[0].hostname = "example.invalid";
    const auto mismatch = crypto::evaluate_chain(request);
    require(mismatch && mismatch->failure == crypto::ChainFailure::name_mismatch, "the SSL policy checks the host name");
    request.anchors.clear();
    request.policies[0].hostname = "localhost";
    const auto untrusted = crypto::evaluate_chain(request);
    require(untrusted && untrusted->failure != crypto::ChainFailure::none, "a self-signed leaf is not trusted by the host's roots");
    const auto roots = crypto::root_certificates(crypto::RootStore::system);
    require(roots && !roots->empty(), "the system's root certificates");

    // TLS over an in-memory transport: handshake, ALPN, a record each way.
    std::deque<std::uint8_t> to_server, to_client;
    const auto reader = [](std::deque<std::uint8_t>& queue) {
        return [&queue](std::uint8_t* buffer, std::size_t size) {
            if (queue.empty()) return crypto::TlsIo{crypto::TlsIo::Kind::would_block, 0};
            const auto count = std::min(size, queue.size());
            std::copy_n(queue.begin(), count, buffer);
            queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(count));
            return crypto::TlsIo{crypto::TlsIo::Kind::ok, count};
        };
    };
    const auto writer = [](std::deque<std::uint8_t>& queue) {
        return [&queue](const std::uint8_t* data, std::size_t size) {
            queue.insert(queue.end(), data, data + size);
            return crypto::TlsIo{crypto::TlsIo::Kind::ok, size};
        };
    };
    const crypto::TlsRead server_read = reader(to_server), client_read = reader(to_client);
    const crypto::TlsWrite server_write = writer(to_client), client_write = writer(to_server);
    crypto::TlsOptions server_options;
    server_options.server = true;
    server_options.identity = &*identity;
    server_options.alpn = {"h2", "http/1.1"};
    crypto::TlsOptions client_options;
    client_options.peer_name = "localhost";
    client_options.alpn = {"http/1.1"};
    auto server = crypto::TlsSession::create(server_options);
    auto client = crypto::TlsSession::create(client_options);
    require(server && client, "create TLS sessions");
    using Status = crypto::TlsSession::Status;
    for (int round = 0; round < 64 && !(server->complete() && client->complete()); ++round) {
        const auto c = client->handshake(client_read, client_write);
        const auto s = server->handshake(server_read, server_write);
        require(c != Status::error && s != Status::error, "TLS handshake: " + client->error() + server->error());
    }
    require(server->complete() && client->complete(), "the TLS handshake completes");
    require(client->negotiated_alpn() == "http/1.1" && client->negotiated_version() >= crypto::TlsVersion::tls12, "ALPN and version");
    const auto peers = client->peer_certificates();
    require(!peers.empty() && peers[0].der() == der, "the client sees the server's certificate");
    std::size_t accepted{}, got{};
    require(client->write(client_write, reinterpret_cast<const std::uint8_t*>("ping"), 4, accepted) == Status::ok && accepted == 4, "TLS write");
    std::array<std::uint8_t, 16> buffer{};
    Status read_status = Status::would_block;
    for (int round = 0; round < 8 && read_status == Status::would_block; ++round) read_status = server->read(server_read, server_write, buffer.data(), buffer.size(), got);
    require(read_status == Status::ok && got == 4 && std::memcmp(buffer.data(), "ping", 4) == 0, "TLS read");
    crypto::TlsOptions bad = server_options;
    bad.identity = nullptr;
    require(crypto::TlsSession::create(bad).error() == host::Error::invalid_argument, "a TLS server needs an identity");
}

void dns_sd_area() {
    namespace dns_sd = host::dns_sd;
    // Address lookups go through the host resolver: localhost is loopback.
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<host::net::Address> found;
    bool answered = false, domain = false;
    dns_sd::Failure failure{};
    auto lookup = dns_sd::get_addr_info("localhost", 0, [&](const std::vector<host::net::Address>& addresses) {
        std::lock_guard lock(mutex);
        found = addresses;
        answered = true;
        changed.notify_all();
    }, failure);
    require(lookup && failure == dns_sd::Failure::none, "start an address lookup");
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(10), [&] { return answered; });
        require(answered && !found.empty() && host::net::is_loopback(found[0]), "localhost resolves to loopback");
    }
    lookup.cancel();
    if (dns_sd::available() != dns_sd::Failure::none) return;   // Windows before 1809
    auto domains = dns_sd::browse_domains([&] {
        std::lock_guard lock(mutex);
        domain = true;
        changed.notify_all();
    }, failure);
    require(domains && failure == dns_sd::Failure::none, "browse domains");
    std::unique_lock lock(mutex);
    changed.wait_for(lock, std::chrono::seconds(10), [&] { return domain; });
    require(domain, "the local. domain is reported");
}

void locale_area() {
    namespace locale = host::locale;
    const auto nfd = locale::normalize(u"\u00e9", locale::Form::d);
    require(nfd && *nfd == u"e\u0301", "normalize to NFD");
    const auto nfc = locale::normalize(u"e\u0301", locale::Form::c);
    require(nfc && *nfc == u"\u00e9", "normalize to NFC");
    require(*locale::compare("und", u"abc", u"ABC", locale::ignore_case) == 0, "case-insensitive comparison");
    const auto names = locale::locales();
    require(names && std::is_sorted(names->begin(), names->end()) && std::adjacent_find(names->begin(), names->end()) == names->end(),
            "locales: sorted and unique");
    for (const auto* expected : {"en", "en-US", "de-DE", "ja-JP", "zh-CN", "zh-Hans"})
        require(std::binary_search(names->begin(), names->end(), std::string(expected)), std::string("locales lists ") + expected);
    require(std::none_of(names->begin(), names->end(), [](const std::string& name) { return name.empty() || name.find('_') != std::string::npos; }),
            "locales: no root locale or alternate sort order");
    require(*locale::compare("en-US", u"a", u"b", 0) == -1 && *locale::compare("en-US", u"b", u"a", 0) == 1, "collation order");
    require(*locale::compare("en-US", u"x2", u"x10", locale::digits_as_numbers) == -1, "digits as numbers");
    const auto a = locale::sort_key("en-US", u"a", 0), b = locale::sort_key("en-US", u"b", 0);
    require(a && b && !a->empty() && *a < *b, "sort keys order like the text");
    require(*locale::map("und", u"ABC", locale::Mapping::lower) == u"abc", "lower-case mapping");
    require(*locale::map("tr-TR", u"I", locale::Mapping::linguistic_lower) == std::u16string(1, char16_t{0x0131}) &&
                *locale::map("tr-TR", u"i", locale::Mapping::linguistic_upper) == std::u16string(1, char16_t{0x0130}) &&
                *locale::map("en-US", u"I", locale::Mapping::linguistic_lower) == u"i",
        "linguistic case mapping");
    require(*locale::map("und", u"\uff21", locale::Mapping::half_width) == u"A", "half-width mapping");
    const auto found = locale::find("und", u"Hello World", u"WORLD", locale::Search::first, locale::ignore_case_linguistic);
    require(found && found->offset == 6 && found->length == 5, "linguistic search");
    require(locale::find("und", u"Hello", u"xyz", locale::Search::first, 0).error() == host::Error::not_found, "search without a match");
    std::array<std::uint16_t, 3> classes{};
    require(static_cast<bool>(locale::classify(u"A1 ", classes)), "classify");
    require((classes[0] & locale::upper) && (classes[0] & locale::alpha) && (classes[1] & locale::digit) && (classes[2] & locale::space),
        "character classes");
    require(*locale::decode_mac_roman("caf\x8e") == "caf\xc3\xa9", "Mac OS Roman text");
    require(locale::valid("en-US") && !locale::valid("not a locale!"), "valid locales");
    require(locale::user_locale() && !locale::user_locale()->empty(), "user locale");
    require(*locale::text("en-US", locale::Text::month_name, 0) == "January", "month names");
    require(*locale::text("en-US", locale::Text::day_name, 0) == "Sunday", "day names start on Sunday");
    require(locale::text("en-US", locale::Text::short_date_pattern)->find('y') != std::string::npos, "ICU date patterns");
    require(*locale::text("en-US", locale::Text::decimal_separator) == ".", "decimal separator");
    require(*locale::text("en-US", locale::Text::currency_code) == "USD", "currency code");
    require(*locale::number("en-US", locale::Number::first_weekday) == 1, "first weekday");
    require(*locale::number("en-US", locale::Number::currency_digits) == 2, "currency digits");
    require(*locale::format_number("en-US", "1234.5", {1, ".", ","}) == "1,234.5", "number formatting");
    const auto money = locale::format_currency("en-US", "1234.50", {2, ".", ","}, "$");
    require(money && money->find("1,234.50") != std::string::npos && money->find('$') != std::string::npos, "currency formatting");
    require(locale::region_name("DE") && !locale::region_name("DE")->empty(), "region names");
    require(locale::currency_name("EUR", true) && !locale::currency_name("EUR", true)->empty(), "currency names");
    if (const auto zone = locale::time_zone()) require(!zone->empty(), "system time zone");
}

// The host's ICU (win32: System32\icu.dll): converters, transliterators, break
// iterators and case mapping, the calls CoreFoundation's strings use.
void icu_area() {
    namespace icu = host::icu;
    const auto* api = icu::api();
    require(api != nullptr && api->version[0] >= 64, "the host has ICU 64 or later");
    icu::UErrorCode error = icu::zero_error;
    const auto converter = api->ucnv_open("macos-0_2-10.2", &error);
    require(converter && !icu::failure(error), "Mac Roman converter");
    std::array<icu::UChar, 8> units{};
    const char roman[] = "caf\x8e";
    const auto length = api->ucnv_toUChars(converter, units.data(), static_cast<std::int32_t>(units.size()), roman, 4, &error);
    require(!icu::failure(error) && length == 4 && units[3] == 0x00e9, "Mac Roman to UTF-16");
    api->ucnv_close(converter);
    error = icu::zero_error;
    const auto* sjis = api->ucnv_getAlias("windows-932", 0, &error);
    require(sjis && !icu::failure(error), "Windows code page aliases");
    error = icu::zero_error;
    const std::u16string id = u"Latin-ASCII";
    const auto transliterator = api->utrans_openU(id.data(), static_cast<std::int32_t>(id.size()), icu::trans_forward, nullptr, 0, nullptr, &error);
    require(transliterator && !icu::failure(error), "Latin-ASCII transliterator");
    std::array<icu::UChar, 16> text{u'c', u'a', u'f', 0x00e9};
    std::int32_t text_length = 4, limit = 4;
    api->utrans_transUChars(transliterator, text.data(), &text_length, static_cast<std::int32_t>(text.size()), 0, &limit, &error);
    require(!icu::failure(error) && text_length == 4 && text[3] == u'e', "transliteration");
    api->utrans_close(transliterator);
    error = icu::zero_error;
    const std::u16string words = u"Hello big world";
    const auto breaks = api->ubrk_open(icu::break_word, "en_US", words.data(), static_cast<std::int32_t>(words.size()), &error);
    require(breaks && !icu::failure(error), "word break iterator");
    require(api->ubrk_first(breaks) == 0 && api->ubrk_next(breaks) == 5 && api->ubrk_getRuleStatus(breaks) >= icu::word_number_limit &&
                api->ubrk_getRuleStatus(breaks) < icu::word_letter_limit,
        "word boundaries and rule status");
    api->ubrk_close(breaks);
    error = icu::zero_error;
    std::array<icu::UChar, 8> folded{};
    const std::u16string strasse = u"Stra\u00dfe";
    const auto folded_length = api->u_strFoldCase(folded.data(), static_cast<std::int32_t>(folded.size()), strasse.data(),
        static_cast<std::int32_t>(strasse.size()), icu::fold_case_default, &error);
    require(!icu::failure(error) && folded_length == 7 && folded[4] == u's' && folded[5] == u's', "case folding");
}

void real_backend() {
    require(host::name(host::Error::would_block) == "would block", "error names");
    require(host::name(host::Error::network_reset) == "network reset" && host::name(host::Error::loop) == "too many symbolic links",
        "error names of the socket and link kinds");

    // time
    const auto first = host::time::monotonic_ns();
    host::time::sleep_ns(2'000'000);
    require(host::time::monotonic_ns() > first, "monotonic time advances");
    require(host::time::realtime_ns() > 1'700'000'000LL * 1'000'000'000LL, "wall clock is after 2023");
    {
        std::atomic<int> fired{0};
        auto timer = host::time::Timer::create([&fired] { ++fired; });
        require(timer && static_cast<bool>(timer->schedule(1'000'000)), "a one-shot timer");
        for (int wait = 0; wait < 500 && fired == 0; ++wait) host::time::sleep_ns(2'000'000);
        require(fired == 1, "the one-shot timer fires once");
        require(static_cast<bool>(timer->schedule(1'000'000, 1'000'000)), "a periodic timer");
        for (int wait = 0; wait < 500 && fired < 4; ++wait) host::time::sleep_ns(2'000'000);
        timer->cancel();
        const int stopped = fired;
        host::time::sleep_ns(20'000'000);
        require(stopped >= 4 && fired == stopped, "a cancelled periodic timer stops");
    }
    // The clocks differ only by the time the machine has slept, zero on a machine
    // that never slept: read the unbiased one first, then the continuous one.
    const auto unbiased = host::time::monotonic_ns();
    const auto continuous = host::time::continuous_ns();
    require(continuous >= unbiased, "continuous time counts at least the unbiased time");
    const auto tick = host::time::tick_ms();
    host::time::sleep_ns(20'000'000);
    require(static_cast<std::uint32_t>(host::time::tick_ms() - tick) >= 10, "tick_ms advances");
    require(host::time::process_cpu_ns() && host::time::thread_cpu_ns(), "CPU-time clocks");

    // process
    require(host::process::current_thread_id() != 0, "a thread id");
    const auto usage = host::process::usage();
    require(usage && usage->resident > 0 && usage->virtual_size > 0, "this process's usage");
    const auto processes = host::process::list();
    bool listed = false;
    if (processes) for (const auto& entry : *processes) listed = listed || entry.pid == host::process::current_pid();
    require(listed, "the process list has this process");
    const auto block = host::process::environment_block();
    bool has_path = false;
    for (const auto& entry : block) has_path = has_path || entry.find('=') != std::string::npos;
    require(!block.empty() && has_path, "the environment block");
    const auto image = host::process::image_path(host::process::current_pid());
    require(image && image->find("alchemy_host_tests") != std::string::npos, "image_path of this process");
    require(host::process::image_path(0xfffffff0U).error() != host::Error::none, "image_path of no process");
    require(*host::process::is_running(host::process::current_pid()) && !host::process::is_running(0xfffffff0U), "is_running");
    const auto started = host::process::start_time();
    require(started && *started > 1'600'000'000'000'000'000ULL && *host::process::start_time(host::process::current_pid()) == *started &&
                host::process::start_time(0xfffffff0U).error() != host::Error::none,
        "start_time");
    spawn_piped(*image);
    spawn_general(*image);
    const auto pipe = host::files::create_pipe();
    require(static_cast<bool>(pipe), "a pipe for duplicate_into");
    host::OwnedHandle pipe_read(pipe->read), pipe_write(pipe->write);
    require(host::files::kind(pipe_read.get()) == host::files::Kind::pipe, "files::kind of a pipe");
    const auto copy = host::process::duplicate_into(pipe_write.get(), 0);
    require(copy && *copy != 0, "duplicate_into this process");
    host::OwnedHandle copied(host::Handle{static_cast<std::uintptr_t>(*copy)});
    require(*host::files::write(copied.get(), std::as_bytes(std::span("p", 1))) == 1 && *host::files::pipe_available(pipe_read.get()) == 1,
        "the duplicate writes the same pipe");
    const auto before_sleep = host::time::monotonic_ns();
    host::time::sleep_ns(1'000'000);
    require(host::time::monotonic_ns() - before_sleep >= 900'000, "sleep_ns sleeps at least about its duration");
    const auto thread = host::process::thread_usage();
    require(thread && thread->user_ns + thread->system_ns <= *host::time::process_cpu_ns(), "the thread's CPU time is within the process's");
    {
        const auto before = host::process::memory_usage();
        std::vector<char> block(8 << 20, 1);   // 8 MiB from malloc
        const auto after = host::process::memory_usage();
        require(before && after && after->heap_committed_bytes >= before->heap_committed_bytes &&
                    after->private_bytes >= after->heap_committed_bytes && after->private_bytes > before->private_bytes + (6 << 20),
                "memory_usage counts private bytes and the heap");
        static_cast<void>(block);
    }

    // environment
    require(host::process::set_environment("ALCHEMY_HOST_TEST_VALUE", "set") &&
                host::process::environment("ALCHEMY_HOST_TEST_VALUE") == std::optional<std::string>("set") &&
                host::process::set_environment("ALCHEMY_HOST_TEST_VALUE", std::nullopt) &&
                !host::process::environment("ALCHEMY_HOST_TEST_VALUE"),
        "set_environment");

    // ipc channels
    channels();

    // sync
    auto event = host::sync::create_event(true, false);
    require(static_cast<bool>(event), "create_event");
    host::OwnedHandle owned(*event);
    const std::array handles{owned.get()};
    require(host::sync::wait_any(handles, 1'000'000).error() == host::Error::timed_out, "an unset event times out");
    require(static_cast<bool>(host::sync::set_event(owned.get())), "set_event");
    const auto woken = host::sync::wait_any(handles, std::nullopt);
    require(woken && *woken == 0, "a set event wakes the wait");

    // one-shot watches
    {
        auto signalled = host::sync::create_event(true, false);
        require(static_cast<bool>(signalled), "an event to watch");
        host::OwnedHandle object(*signalled);
        auto done = host::sync::create_event(true, false);
        host::OwnedHandle finished(*done);
        auto finished_handle = finished.get();
        const auto watch = host::sync::watch_once(object.get(), [](void* context) {
            static_cast<void>(host::sync::set_event(*static_cast<host::Handle*>(context)));
        }, &finished_handle);
        require(static_cast<bool>(watch), "watch_once");
        require(static_cast<bool>(host::sync::set_event(object.get())), "signal the watched object");
        const std::array waits{finished.get()};
        require(static_cast<bool>(host::sync::wait_any(waits, 5'000'000'000ULL)), "the watch fired");
        host::sync::unwatch(*watch, true);
        const auto self = host::process::open(host::process::current_pid());
        require(self && host::sync::wait_any(std::array{*self}, 0).error() == host::Error::timed_out, "process::open: a running process is unsignalled");
        host::close(*self);
    }

    // named objects: a second create opens the same object
    const auto prefix = "alchemy.host-test." + std::to_string(host::process::current_pid());
    auto mutex = host::sync::create_mutex(prefix + ".mutex");
    require(static_cast<bool>(mutex), "create_mutex");
    host::OwnedHandle owned_mutex(*mutex);
    const std::array mutexes{owned_mutex.get()};
    require(host::sync::wait_any(mutexes, std::nullopt) && host::sync::unlock_mutex(owned_mutex.get()), "lock and unlock a mutex");
    require(host::sync::unlock_mutex(owned_mutex.get()).error() != host::Error::none, "unlocking an unowned mutex fails");
    auto semaphore = host::sync::create_semaphore(prefix + ".semaphore", 1, 2);
    auto same = host::sync::create_semaphore(prefix + ".semaphore", 0, 2);
    require(semaphore && same, "create_semaphore");
    host::OwnedHandle owned_semaphore(*semaphore), owned_same(*same);
    const std::array semaphores{owned_same.get()};
    require(static_cast<bool>(host::sync::wait_any(semaphores, 0)), "the named semaphore has its creator's count");
    require(host::sync::wait_any(semaphores, 0).error() == host::Error::timed_out, "an empty semaphore times out");
    require(static_cast<bool>(host::sync::release_semaphore(owned_semaphore.get(), 2)), "release_semaphore");
    require(host::sync::release_semaphore(owned_semaphore.get(), 1).error() != host::Error::none, "a release past the maximum fails");
    require(host::sync::create_semaphore(prefix + ".semaphore", 0, 2, true).error() == host::Error::exists, "an exclusive semaphore exists");
    require(host::sync::open_semaphore(prefix + ".missing").error() == host::Error::not_found, "open_semaphore of no semaphore");
    const auto reopened = host::sync::open_semaphore(prefix + ".semaphore");
    require(static_cast<bool>(reopened), "open_semaphore");
    host::close(*reopened);
    require(host::sync::wait_any(semaphores, 0) && host::sync::wait_any(semaphores, 0), "released counts");
    auto named = host::sections::open_named(prefix + ".section", 4096);
    auto again = host::sections::open_named(prefix + ".section", 4096);
    require(named && named->created && again && !again->created, "open_named creates once");
    host::OwnedHandle owned_named(named->handle), owned_again(again->handle);
    auto existing = host::sections::open_existing(prefix + ".section", false);
    require(static_cast<bool>(existing), "open_existing");
    host::OwnedHandle owned_existing(*existing);
    require(host::sections::open_existing(prefix + ".missing", false).error() == host::Error::not_found, "a missing section is not_found");
    const auto writer = host::sections::map(owned_named.get(), 0, 4096, true);
    const auto reader = host::sections::map(owned_existing.get(), 0, 4096, false);
    require(writer && reader, "map named views");
    *reinterpret_cast<volatile std::uint32_t*>(*writer) = 0x51ab;
    require(*reinterpret_cast<const volatile std::uint32_t*>(*reader) == 0x51ab, "views of a named section share memory");
    require(host::sections::unmap(*writer) && host::sections::unmap(*reader), "unmap named views");

    // files
    const auto path = (std::filesystem::temp_directory_path() / ("alchemy-host-test-" + std::to_string(host::process::current_pid()))).string();
    {
        auto file = host::files::open(path, host::files::Access::read_write, host::files::Create::create_or_truncate);
        require(static_cast<bool>(file), "open for writing");
        host::OwnedHandle handle(*file);
        const char text[] = "host layer";
        const auto written = host::files::write_at(handle.get(), std::as_bytes(std::span(text, 10)), 3);
        require(written && *written == 10, "write_at");
        require(*host::files::size(handle.get()) == 13, "size after a positional write");
        std::array<std::byte, 16> buffer{};
        const auto read = host::files::read_at(handle.get(), buffer, 3);
        require(read && *read == 10 && std::memcmp(buffer.data(), text, 10) == 0, "read_at");
        const auto end = host::files::read_at(handle.get(), buffer, 13);
        require(end && *end == 0, "read_at end of file reads 0 bytes");
    }
    const auto attributes = host::files::attributes(path);
    require(attributes && !attributes->directory && !attributes->symlink, "attributes of a file");
    const auto times = host::files::times(path);
    const auto now = host::time::realtime_ns();
    require(times && times->modified_ns > now - 600'000'000'000LL && times->modified_ns <= now + 1'000'000'000LL &&
                times->created_ns <= times->modified_ns + 1'000'000'000LL,
        "times of a new file");
    std::filesystem::remove(path);
    require(host::files::open(path, host::files::Access::read, host::files::Create::open_existing).error() == host::Error::not_found,
        "a missing file is not_found");
    require(host::files::attributes(path).error() == host::Error::not_found &&
                host::files::attributes(path + "/below").error() == host::Error::not_found,
        "attributes of a missing path are not_found");
    const auto temporary_directory = std::filesystem::temp_directory_path().string();
    require(host::files::attributes(temporary_directory) && host::files::attributes(temporary_directory)->directory, "attributes of a directory");

    // volumes and temporary files
    const auto volume = host::files::volume(temporary_directory);
    require(volume && !volume->root.empty() && !volume->filesystem.empty() && volume->total >= volume->free && volume->total > 0 &&
                volume->block_size > 0,
        "the volume of the temporary directory");
    const auto roots = host::files::volume_roots();
    const auto folded = [](std::string text) {
        for (auto& c : text) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return text;
    };
    bool rooted = false;
    if (roots) for (const auto& root : *roots) rooted = rooted || folded(root) == folded(volume->root);
    require(rooted, "volume_roots lists that volume");
    bool system = false;
    if (roots) for (const auto& root : *roots) if (const auto item = host::files::volume(root)) system = system || item->system;
    require(system, "one volume holds the host OS");
    require(host::files::times(volume->root) && host::files::times(volume->root)->modified_ns > 0, "times of a volume root");
    const auto temporary = host::files::create_temporary("alchemy");
    require(temporary && std::filesystem::is_regular_file(*temporary) && std::filesystem::file_size(*temporary) == 0, "create_temporary");
    const auto second = host::files::create_temporary("alchemy");
    require(second && *second != *temporary, "temporary names are unique");

    // replace, metadata handles, final paths and directory watches
    {
        auto file = host::files::open(*temporary, host::files::Access::write, host::files::Create::open_existing);
        require(file && host::files::write_at(*file, std::as_bytes(std::span("new", 3)), 0), "write the replacement");
        host::close(*file);
    }
    require(static_cast<bool>(host::files::replace(*temporary, *second)), "replace");
    require(!std::filesystem::exists(*temporary) && std::filesystem::file_size(*second) == 3, "replace moved the file over the target");
    const auto metadata = host::files::open_metadata(*second);
    require(static_cast<bool>(metadata), "open_metadata");
    host::OwnedHandle owned_metadata(*metadata);
    const auto before = host::files::info(owned_metadata.get());
    require(before && before->size == 3 && before->links == 1 && !before->directory && !before->delete_pending, "info");
    const auto renamed = *second + ".renamed";
    std::filesystem::rename(*second, renamed);
    const auto moved = host::files::final_path(owned_metadata.get());
    require(moved && moved->ends_with(std::filesystem::path(renamed).filename().string()), "final_path follows a rename");
    const auto directory = host::files::open_metadata(temporary_directory);
    require(directory && host::files::info(*directory)->directory, "open_metadata of a directory");
    host::close(*directory);
    const auto folder = std::filesystem::temp_directory_path() / ("alchemy-watch-" + std::to_string(host::process::current_pid()));
    std::filesystem::create_directory(folder);
    const auto watch = host::files::watch_directory(folder.string());
    require(watch && !*host::files::directory_changed(*watch), "a new directory watch has no change");
    std::filesystem::create_directory(folder / "entry");
    bool changed = false;
    for (int attempt = 0; attempt < 100 && !changed; ++attempt) {
        changed = *host::files::directory_changed(*watch);
        if (!changed) host::time::sleep_ns(10'000'000);
    }
    require(changed, "the watch reports a new entry");
    host::files::close_watch(*watch);
    {
        // A tree watch reports changes below the directory with their relative paths.
        auto tree = host::files::watch_tree(folder.string());
        require(static_cast<bool>(tree), "watch_tree");
        bool overflow{};
        require((*tree)->read(0, overflow) && !overflow, "a new tree watch has nothing to read");
        std::ofstream(folder / "entry" / "file.txt") << "x";
        std::filesystem::rename(folder / "entry" / "file.txt", folder / "entry" / "moved.txt");
        bool created{}, renamed_from{}, renamed_to{};
        for (int attempt = 0; attempt < 100 && !(created && renamed_from && renamed_to); ++attempt) {
            const auto changes = (*tree)->read(50, overflow);
            require(static_cast<bool>(changes), "tree watch read");
            for (const auto& change : *changes) {
                created |= change.kind == host::files::TreeChange::Kind::created && change.path == "entry/file.txt";
                renamed_from |= change.kind == host::files::TreeChange::Kind::renamed_from && change.path == "entry/file.txt";
                renamed_to |= change.kind == host::files::TreeChange::Kind::renamed_to && change.path == "entry/moved.txt";
            }
        }
        require(created && renamed_from && renamed_to, "the tree watch reports a nested create and rename");
        (*tree)->cancel();
        require((*tree)->read(1000, overflow).error() == host::Error::interrupted, "a cancelled tree watch read is interrupted");
    }
    {
        // rename with replace succeeds while the replaced file is open elsewhere (POSIX semantics).
        const auto target = folder / "target.txt", source = folder / "source.txt";
        std::ofstream(target) << "old";
        std::ofstream(source) << "new";
        auto held = host::files::open(target.string(), host::files::Access::read, host::files::Create::open_existing, 0);
        require(static_cast<bool>(held), "open the file to be replaced");
        require(static_cast<bool>(host::files::rename(source.string(), target.string(), true)), "rename over an open file");
        host::close(*held);
        std::ifstream check(target);
        std::string text;
        check >> text;
        require(text == "new" && !std::filesystem::exists(source), "the renamed file replaced the open one");
    }
    std::filesystem::remove_all(folder);
    std::filesystem::remove(renamed);

    // the descriptor-table calls: positions, sizes, metadata, locks, pipes, the null device
    file_table(temporary_directory);

    // sections and vm
    auto section = host::sections::create_section(1 << 16);
    require(static_cast<bool>(section), "create_section");
    host::OwnedHandle owned_section(*section);
    const auto view = host::sections::map(owned_section.get(), 0, 1 << 16, true);
    require(static_cast<bool>(view), "map");
    std::memset(reinterpret_cast<void*>(*view), 0x5a, 16);
    const auto region = host::vm::query(*view);
    require(region && region->committed && host::vm::has(region->protection, host::vm::Protection::write), "vm::query of a view");
    require(static_cast<bool>(host::sections::unmap(*view)), "unmap");
    {
        const auto copy = host::duplicate(owned_section.get());
        require(copy && host::sections::same(owned_section.get(), *copy), "sections::same of a duplicate");
        auto other = host::sections::create_section(1 << 16);
        require(other && !host::sections::same(owned_section.get(), *other), "sections::same of another section");
        host::close(*copy);
        host::close(*other);
        const auto remote = host::sections::duplicate_into(owned_section.get(), host::process::current_pid(), false);
        require(remote && *remote != 0, "sections::duplicate_into");
        host::OwnedHandle read_only(host::Handle{static_cast<std::uintptr_t>(*remote)});
        require(host::sections::map(read_only.get(), 0, 4096, true).error() != host::Error::none &&
                    static_cast<bool>(host::sections::map(read_only.get(), 0, 4096, false)),
            "a read-only duplicate maps read-only only");
    }
    require(host::vm::page_info().page_size >= 4096, "page size");
    std::array<std::byte, 4096> locked{};
    const auto locked_at = reinterpret_cast<std::uintptr_t>(locked.data());
    require(host::vm::lock(locked_at, locked.size()) && host::vm::unlock(locked_at, locked.size()), "vm lock and unlock");
    require(static_cast<bool>(host::vm::unlock(locked_at, locked.size())), "unlocking pages that are not locked succeeds");

    // crypto, system, net, process
    std::array<std::byte, 32> random{};
    require(static_cast<bool>(host::crypto::random(random)), "random");
    require(host::system::memory()->physical > 0, "physical memory");
    require(static_cast<bool>(host::system::memory_pressure()), "memory_pressure");
    const auto total = host::system::cpu_times();
    const auto processors = host::system::processor_times();
    require(total && total->user_ns + total->system_ns + total->idle_ns > 0, "system CPU times");
    require(processors && !processors->empty(), "per-processor CPU times");
    const auto uuid = host::system::machine_uuid();
    require(uuid && *uuid != std::array<std::uint8_t, 16>{} && *host::system::machine_uuid() == *uuid, "a stable machine uuid");
    require(static_cast<bool>(host::net::initialize()), "net::initialize");
    const auto interfaces = host::net::interfaces();
    bool loopback = false;
    if (interfaces)
        for (const auto& item : *interfaces)
            for (const auto& assigned : item.addresses)
                loopback = loopback || (item.kind == host::net::InterfaceKind::loopback && host::net::is_loopback(assigned.address) &&
                                        item.index != 0);
    require(loopback, "interfaces lists the loopback interface with a loopback address");
    const auto to_loopback = host::net::best_interface(host::net::Address::ipv4({127, 0, 0, 1}));
    require(to_loopback && *to_loopback != 0, "the best interface for a loopback destination");
    const auto dns = host::net::dns_configuration();
    require(static_cast<bool>(dns), "the DNS configuration");
    for (const auto& server : dns->servers)
        require(server.port == 53 && (server.family == host::net::Family::ipv4 || server.family == host::net::Family::ipv6), "DNS servers use port 53");
    if (interfaces)
        for (const auto& item : *interfaces)
            if (item.up && item.kind != host::net::InterfaceKind::loopback)
                require(static_cast<bool>(host::net::dns_configuration(item.index)), "the DNS configuration of one interface");
    sockets();
    require(host::process::current_pid() != 0, "pid");
    require(!host::process::environment("ALCHEMY_HOST_TEST_SURELY_UNSET").has_value(), "unset environment variable");

    locale_area();
    icu_area();
    http_area();
    crypto_area();
    certificate_area();
    dns_sd_area();

    // system names and power requests
    require(host::system::host_name() && !host::system::host_name()->empty() && host::system::host_name(false), "host names");
    require(host::system::user_name() && !host::system::user_name()->empty(), "user name");
    {
        auto awake = host::system::PowerRequest::create("Alchemy host test", true, false);
        require(static_cast<bool>(awake), "power request");
        auto moved = std::move(*awake);
        moved.release();
        moved.release();
    }
    const auto power = host::system::power_status();
    require(power && (!power->percent || *power->percent <= 100), "power status");
    require(host::system::watch_power_source([] {}) && host::system::watch_power_source([] {}).error() == host::Error::already,
        "power-source changes are watched once per process");
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--pipe-child") return pipe_child(argc, argv);
    if (argc > 2 && std::string_view(argv[1]) == "--section-child") return section_child(argv);
    try {
        result_type();
        addresses();
        if (host::backend() == "null") null_backend();
        else real_backend();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
    std::cout << "host tests passed (" << host::backend() << ")\n";
    return 0;
}
