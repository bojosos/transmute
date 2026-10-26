// Network.framework over the full provider set: nw_path_monitor over the host's
// adapters (network_framework.cpp), Bonjour browsing, and loopback TCP
// connections and listeners (nw_connections.cpp).
#include "alchemy/gui/tests/fill_fixture.hpp"
#include "alchemy/darwin/security/bridge.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

namespace {
using namespace fill_test;

#define FRAMEWORK(name, version) "/System/Library/Frameworks/" name ".framework/Versions/" version "/" name
constexpr std::string_view appkit = FRAMEWORK("AppKit", "C"), cf_lib = FRAMEWORK("CoreFoundation", "A"),
    foundation = FRAMEWORK("Foundation", "C"), as = FRAMEWORK("ApplicationServices", "A"), cs = FRAMEWORK("CoreServices", "A"),
    sec = FRAMEWORK("Security", "A"), network = FRAMEWORK("Network", "A"), cv = FRAMEWORK("CoreVideo", "A"),
    cm = FRAMEWORK("CoreMedia", "A"), midi = FRAMEWORK("CoreMIDI", "A"), ct = FRAMEWORK("CoreText", "A"),
    sc = FRAMEWORK("SystemConfiguration", "A"), iosurface = FRAMEWORK("IOSurface", "A"), avf_audio = FRAMEWORK("AVFAudio", "A"),
    vt = FRAMEWORK("VideoToolbox", "A"), un = FRAMEWORK("UserNotifications", "A"), cmio = FRAMEWORK("CoreMediaIO", "A");
#undef FRAMEWORK
constexpr std::string_view sandbox = "/usr/lib/libsandbox.1.dylib", objc_lib = "/usr/lib/libobjc.A.dylib",
    bz2 = "/usr/lib/libbz2.1.0.dylib", cups = "/usr/lib/libcups.2.dylib", sys = "/usr/lib/libSystem.B.dylib",
    bsm = "/usr/lib/libbsm.0.dylib";

std::shared_ptr<alchemy::gui::security::Bridge> bridge() { return alchemy::gui::security::bridge(); }

void network_monitor(Fixture& f) {
    auto b = bridge();
    const auto queue = b->create_serial_queue("alchemy.test.nw");
    std::atomic<int> updates{0};
    std::atomic<std::uint64_t> status{0}, interfaces{0};
    const auto enumerate = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        const auto type = f.call(network, "_nw_interface_get_type", {s.x[1]});
        require(type <= 4, "interface type");
        ++interfaces;
        s.x[0] = 1;
    });
    const auto update = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        status = f.call(network, "_nw_path_get_status", {s.x[1]});
        static_cast<void>(f.call(network, "_nw_path_enumerate_interfaces", {s.x[1], enumerate}));
        ++updates;
    });
    const auto monitor = f.call(network, "_nw_path_monitor_create");
    static_cast<void>(f.call(network, "_nw_path_monitor_set_queue", {monitor, queue}));
    static_cast<void>(f.call(network, "_nw_path_monitor_set_update_handler", {monitor, update}));
    static_cast<void>(f.call(network, "_nw_path_monitor_start", {monitor}));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!updates && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(updates >= 1, "first path delivered");
    require(status == 1 || status == 2, "satisfied or unsatisfied");
    require(status == 2 || interfaces > 0, "a satisfied path has interfaces");
    static_cast<void>(f.call(network, "_nw_path_monitor_cancel", {monitor}));
    // Bonjour browsing over the host mDNS engine: ready when the host has a DNS-SD client and the network policy
    // allows multicast DNS, else failed with that DNS-SD error; cancelling reports cancelled.
    std::atomic<std::uint64_t> state{0};
    std::atomic<std::int64_t> code{0};
    const auto state_handler = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        if (s.x[2]) code = static_cast<std::int32_t>(f.call(network, "_nw_error_get_error_code", {s.x[2]}));
        state = s.x[1];
    });
    const auto descriptor = f.call(network, "_nw_browse_descriptor_create_bonjour_service", {f.text("_googlecast._tcp"), 0});
    const auto parameters = f.call(network, "_nw_parameters_create_secure_tcp", {
        f.engine.read64(f.symbol(network, "__nw_parameters_configure_protocol_disable")),
        f.engine.read64(f.symbol(network, "__nw_parameters_configure_protocol_default_configuration"))});
    const auto browser = f.call(network, "_nw_browser_create", {descriptor, parameters});
    static_cast<void>(f.call(network, "_nw_browser_set_queue", {browser, queue}));
    static_cast<void>(f.call(network, "_nw_browser_set_state_changed_handler", {browser, state_handler}));
    static_cast<void>(f.call(network, "_nw_browser_start", {browser}));
    const auto until2 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!state && std::chrono::steady_clock::now() < until2) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(state == 1 || (state == 2 && code != 0), "the browser is ready, or failed with a DNS-SD error (no client, or the network policy)");
    static_cast<void>(f.call(network, "_nw_browser_cancel", {browser}));
    const auto until3 = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (state != 3 && std::chrono::steady_clock::now() < until3) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    require(state == 3, "the browser is cancelled");
}

// nw_listener and nw_connection over loopback TCP (nw_connections.cpp): a listener on an ephemeral port accepts a
// connection to 127.0.0.1; the client sends, the server reads it and answers with a final message, and the client
// reads the answer with is_complete. Endpoints, contexts and TLS's explicit failure are checked on the way.
void connections(Fixture& f) {
    auto b = bridge();
    const auto queue = b->create_serial_queue("alchemy.test.nw.connections");
    const auto wait_for = [](auto condition, const char* what) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!condition() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        require(condition(), what);
    };
    const auto read64 = [&](std::string_view name) { return f.engine.read64(f.symbol(network, name)); };
    const auto disable = read64("__nw_parameters_configure_protocol_disable");
    const auto defaults = read64("__nw_parameters_configure_protocol_default_configuration");
    const auto final_context = read64("__nw_content_context_final_send");
    const auto message_context = read64("__nw_content_context_default_message");
    require(f.call(network, "_nw_content_context_get_is_final", {final_context}) == 1 &&
                f.call(network, "_nw_content_context_get_is_final", {message_context}) == 0, "the content contexts");
    // Endpoints.
    const auto host = f.call(network, "_nw_endpoint_create_host", {f.text("127.0.0.1"), f.text("8080")});
    require(f.call(network, "_nw_endpoint_get_type", {host}) == 2 && f.call(network, "_nw_endpoint_get_port", {host}) == 8080 &&
                f.read_text(f.call(network, "_nw_endpoint_get_hostname", {host})) == "127.0.0.1", "a host endpoint");
    const auto url = f.call(network, "_nw_endpoint_create_url", {f.text("https://example.com/path")});
    require(f.call(network, "_nw_endpoint_get_type", {url}) == 4 && f.call(network, "_nw_endpoint_get_port", {url}) == 443 &&
                f.read_text(f.call(network, "_nw_endpoint_get_hostname", {url})) == "example.com", "a URL endpoint");

    // The listener.
    std::atomic<std::uint64_t> listener_state{0};
    std::atomic<Address> accepted{0};
    const auto listener_handler = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) { listener_state = s.x[1]; });
    const auto accept_handler = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        accepted = f.call(objc_lib, "_objc_retain", {s.x[1]});
    });
    const auto tcp = f.call(network, "_nw_parameters_create_secure_tcp", {disable, defaults});
    const auto listener = f.call(network, "_nw_listener_create", {tcp});
    static_cast<void>(f.call(network, "_nw_listener_set_queue", {listener, queue}));
    static_cast<void>(f.call(network, "_nw_listener_set_state_changed_handler", {listener, listener_handler}));
    static_cast<void>(f.call(network, "_nw_listener_set_new_connection_handler", {listener, accept_handler}));
    static_cast<void>(f.call(network, "_nw_listener_start", {listener}));
    wait_for([&] { return listener_state.load() == 2; }, "the listener is ready");
    const auto port = f.call(network, "_nw_listener_get_port", {listener});
    require(port > 0 && port < 65536, "the listener's port");

    // The client connection.
    std::atomic<std::uint64_t> client_state{0};
    const auto client_handler = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) { client_state = s.x[1]; });
    const auto server = f.call(network, "_nw_endpoint_create_host", {f.text("127.0.0.1"), f.text(std::to_string(port))});
    const auto client = f.call(network, "_nw_connection_create", {server, tcp});
    static_cast<void>(f.call(network, "_nw_connection_set_queue", {client, queue}));
    static_cast<void>(f.call(network, "_nw_connection_set_state_changed_handler", {client, client_handler}));
    static_cast<void>(f.call(network, "_nw_connection_start", {client}));
    wait_for([&] { return client_state.load() == 3; }, "the client is ready");
    wait_for([&] { return accepted.load() != 0; }, "the listener accepted the connection");
    std::atomic<std::uint64_t> server_state{0};
    const auto server_handler = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) { server_state = s.x[1]; });
    static_cast<void>(f.call(network, "_nw_connection_set_queue", {accepted, queue}));
    static_cast<void>(f.call(network, "_nw_connection_set_state_changed_handler", {accepted, server_handler}));
    static_cast<void>(f.call(network, "_nw_connection_start", {accepted}));
    wait_for([&] { return server_state.load() == 3; }, "the accepted connection is ready");
    const auto peer = f.call(network, "_nw_connection_copy_endpoint", {accepted});
    require(f.call(network, "_nw_endpoint_get_type", {peer}) == 1, "the peer is an address endpoint");

    const auto data_of = [&](std::string_view text) {
        const auto bytes = f.text(text);
        return f.call(sys, "_dispatch_data_create", {bytes, text.size(), 0, 0});
    };
    const auto text_of = [&](Address data) {
        const auto slots = f.block(16);
        const auto map = f.call(sys, "_dispatch_data_create_map", {data, slots, slots + 8});
        const auto size = f.engine.read64(slots + 8);
        std::string value(static_cast<std::size_t>(size), '\0');
        for (std::size_t index = 0; index < value.size(); ++index) value[index] = static_cast<char>(f.memory().read8(f.engine.read64(slots) + index));
        static_cast<void>(f.call(sys, "_dispatch_release", {map}));
        return value;
    };
    // Client -> server.
    std::atomic<int> sent{0};
    std::string received;
    std::mutex received_mutex;
    std::atomic<int> server_reads{0};
    const auto send_done = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) { sent = s.x[1] ? -1 : 1; });
    static_cast<void>(f.call(network, "_nw_connection_send", {client, data_of("ping"), message_context, 1, send_done}));
    wait_for([&] { return sent.load() != 0; }, "the send completes");
    require(sent == 1, "the send succeeds");
    const auto server_receive = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        std::lock_guard lock(received_mutex);
        if (s.x[1]) received = text_of(s.x[1]);
        ++server_reads;
    });
    static_cast<void>(f.call(network, "_nw_connection_receive", {accepted, 4, 64, server_receive}));
    wait_for([&] { return server_reads.load() != 0; }, "the server receives");
    { std::lock_guard lock(received_mutex); require(received == "ping", "the server reads what the client sent"); }
    // Server -> client, final: the client sees is_complete.
    std::atomic<int> answered{0};
    const auto answer_done = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) { answered = s.x[1] ? -1 : 1; });
    static_cast<void>(f.call(network, "_nw_connection_send", {accepted, data_of("pong"), final_context, 1, answer_done}));
    wait_for([&] { return answered.load() != 0; }, "the answer is sent");
    std::atomic<int> client_reads{0};
    std::atomic<bool> complete{false};
    std::string reply;
    const auto client_receive = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        std::lock_guard lock(received_mutex);
        if (s.x[1]) reply += text_of(s.x[1]);
        if (s.x[3] & 0xff) complete = true;
        ++client_reads;
    });
    for (int round = 0; round < 3 && !complete; ++round) {
        const auto before = client_reads.load();
        static_cast<void>(f.call(network, "_nw_connection_receive", {client, 1, 64, client_receive}));
        wait_for([&] { return client_reads.load() > before; }, "the client receives");
    }
    { std::lock_guard lock(received_mutex); require(reply == "pong" && complete, "the client reads the final answer"); }

    // Cancelling.
    static_cast<void>(f.call(network, "_nw_connection_cancel", {client}));
    wait_for([&] { return client_state.load() == 5; }, "the client is cancelled");
    static_cast<void>(f.call(network, "_nw_connection_cancel", {accepted}));
    static_cast<void>(f.call(network, "_nw_listener_cancel", {listener}));
    wait_for([&] { return listener_state.load() == 4; }, "the listener is cancelled");

    // TLS is unavailable: the connection fails with a TLS-domain error.
    std::atomic<std::uint64_t> tls_state{0}, tls_domain{0};
    const auto tls_handler = b->make_block([&](transmute::CpuState& s, transmute::GuestMemory&) {
        if (s.x[2]) tls_domain = f.call(network, "_nw_error_get_error_domain", {s.x[2]});
        tls_state = s.x[1];
    });
    const auto secure = f.call(network, "_nw_parameters_create_secure_tcp", {defaults, defaults});
    const auto tls_connection = f.call(network, "_nw_connection_create", {server, secure});
    static_cast<void>(f.call(network, "_nw_connection_set_queue", {tls_connection, queue}));
    static_cast<void>(f.call(network, "_nw_connection_set_state_changed_handler", {tls_connection, tls_handler}));
    static_cast<void>(f.call(network, "_nw_connection_start", {tls_connection}));
    wait_for([&] { return tls_state.load() == 4; }, "a TLS connection fails");
    require(tls_domain == 3, "with an nw_error_domain_tls error");
}

} // namespace

int main() {
    return run("network framework", {{"network", network_monitor}, {"connections", connections}});
}
