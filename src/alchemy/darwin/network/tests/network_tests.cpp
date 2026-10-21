// Networking provider tests (owner: v-netsec): network NSURLs and CFURL
// access, NSOperationQueue blocks, CFHTTPMessage, and NSURLSession against a
// local HTTPS server started in-test (SChannel, self-signed certificate):
// trust rejection without a delegate (no request bytes reach the server),
// trust override through the server-trust challenge, redirects, uploads,
// downloads, delegate streaming, HTTP Basic authentication challenges and
// cancellation. When the network is reachable, a real HTTPS request to
// https://example.com is evaluated against the Windows root store.
// CFSocket: accept, data, write and end-of-stream callbacks through the
// run loop against host Winsock peers. Bonjour: dns_sd and NSNetService over
// the Windows DNS-SD client (see test_bonjour).
#include "http.hpp"
#include "mdns.hpp"
#include "tls.hpp"

#include "alchemy/darwin_dispatch.hpp"
#include "alchemy/darwin_frameworks.hpp"
#include "alchemy/darwin_services.hpp"
#include "alchemy/darwin_threads.hpp"
#include "alchemy/foundation_bridge.hpp"
#include "alchemy/gui/install.hpp"
#include "alchemy/gui/runloop.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace {
using Address = std::uint64_t;
using Bytes = std::vector<std::uint8_t>;
namespace sec = alchemy::gui::security;
namespace net = alchemy::gui::network;
using transmute::CpuState;
using transmute::GuestMemory;

void require(bool value, std::string_view message) {
    if (!value) throw std::runtime_error(std::string(message));
}

template <class Predicate>
void eventually(Predicate done, std::string_view message, int seconds = 15) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!done()) {
        require(std::chrono::steady_clock::now() < until, std::string("timed out: ") + std::string(message));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// ---- the local HTTPS server -------------------------------------------------------------------------

class HttpsServer {
public:
    explicit HttpsServer(const alchemy::host::crypto::Identity* certificate) : certificate_(certificate) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener_, 16) == 0, "listen failed");
        int size = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &size);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] { accept_loop(); });
    }
    ~HttpsServer() {
        stop_ = true;
        if (thread_.joinable()) thread_.join();
        for (auto& connection : connections_) if (connection.joinable()) connection.join();
        closesocket(listener_);
    }
    [[nodiscard]] std::uint16_t port() const { return port_; }
    [[nodiscard]] std::string base() const { return "https://localhost:" + std::to_string(port_); }
    std::vector<std::string> requests() { std::lock_guard lock(mutex_); return requests_; }
    std::size_t handshakes() { std::lock_guard lock(mutex_); return handshakes_; }
    std::string error() { std::lock_guard lock(mutex_); return error_; }

private:
    void accept_loop() {
        while (!stop_) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listener_, &set);
            timeval timeout{0, 50'000};
            if (select(0, &set, nullptr, nullptr, &timeout) <= 0) continue;
            const auto client = accept(listener_, nullptr, nullptr);
            if (client == INVALID_SOCKET) continue;
            std::lock_guard lock(mutex_);
            connections_.emplace_back([this, client] { serve(client); });
        }
    }
    void serve(SOCKET client) {
        struct Close { SOCKET s; ~Close() { closesocket(s); } } close{client};
        try {
            sec::TlsOptions options;
            options.server = true;
            options.certificate = certificate_;
            sec::TlsSession tls(options);
            const sec::TlsRead read = [client](std::uint8_t* buffer, std::size_t size) {
                const auto got = recv(client, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0);
                if (got < 0) return sec::IoResult{sec::IoResult::Kind::error, 0};
                return sec::IoResult{got == 0 ? sec::IoResult::Kind::closed : sec::IoResult::Kind::ok, static_cast<std::size_t>(got)};
            };
            const sec::TlsWrite write = [client](const std::uint8_t* data, std::size_t size) {
                const auto sent = send(client, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
                if (sent < 0) return sec::IoResult{sec::IoResult::Kind::error, 0};
                return sec::IoResult{sec::IoResult::Kind::ok, static_cast<std::size_t>(sent)};
            };
            if (tls.handshake(read, write) != sec::TlsSession::Status::ok) return;
            { std::lock_guard lock(mutex_); ++handshakes_; }
            // One request per connection (responses carry Connection: close).
            std::string data;
            std::uint8_t buffer[16384];
            std::size_t header_end;
            while ((header_end = data.find("\r\n\r\n")) == std::string::npos) {
                std::size_t got{};
                if (tls.read(read, write, buffer, sizeof(buffer), got) != sec::TlsSession::Status::ok || !got) return;
                data.append(reinterpret_cast<char*>(buffer), got);
            }
            const auto head = data.substr(0, header_end);
            std::string body = data.substr(header_end + 4);
            const auto line_end = head.find("\r\n");
            const auto request_line = head.substr(0, line_end);
            const auto method = request_line.substr(0, request_line.find(' '));
            const auto path = request_line.substr(method.size() + 1, request_line.rfind(' ') - method.size() - 1);
            std::map<std::string, std::string> headers;
            for (std::size_t at = line_end + 2; at < head.size();) {
                auto next = head.find("\r\n", at);
                if (next == std::string::npos) next = head.size();
                const auto line = head.substr(at, next - at);
                const auto colon = line.find(':');
                if (colon != std::string::npos) headers[sec::lower(line.substr(0, colon))] = line.substr(line.find_first_not_of(' ', colon + 1));
                at = next + 2;
            }
            const auto length = headers.contains("content-length") ? std::stoul(headers["content-length"]) : 0UL;
            while (body.size() < length) {
                std::size_t got{};
                if (tls.read(read, write, buffer, sizeof(buffer), got) != sec::TlsSession::Status::ok || !got) return;
                body.append(reinterpret_cast<char*>(buffer), got);
            }
            {
                std::lock_guard lock(mutex_);
                requests_.push_back(method + " " + path + (body.empty() ? "" : " " + body));
            }
            int status = 200;
            std::string reason = "OK", content_type = "text/plain; charset=utf-8", payload, extra;
            if (path == "/hello") payload = "hello world";
            else if (path == "/redirect") { status = 302; reason = "Found"; extra = "Location: /hello\r\n"; }
            else if (path == "/echo") { content_type = "application/octet-stream"; payload = body; }
            else if (path == "/download") {
                content_type = "application/octet-stream";
                extra = "Content-Disposition: attachment; filename=\"payload.bin\"\r\n";
                payload.resize(100000);
                for (std::size_t index = 0; index < payload.size(); ++index) payload[index] = static_cast<char>(index & 0xff);
            } else if (path == "/auth") {
                if (headers["authorization"] == "Basic " + sec::base64_encode(Bytes{'a', 'r', 't', 'i', 's', 't', ':', 's', 'e', 'c', 'r', 'e', 't'}))
                    payload = "authorized";
                else { status = 401; reason = "Unauthorized"; extra = "WWW-Authenticate: Basic realm=\"alchemy\"\r\n"; payload = "denied"; }
            } else if (path == "/slow") {
                for (int index = 0; index < 60 && !stop_; ++index) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                payload = "slow";
            } else { status = 404; reason = "Not Found"; payload = "missing"; }
            const auto response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: " + content_type +
                "\r\nContent-Length: " + std::to_string(payload.size()) + "\r\nConnection: close\r\n" + extra + "\r\n" + payload;
            std::size_t accepted{};
            static_cast<void>(tls.write(write, reinterpret_cast<const std::uint8_t*>(response.data()), response.size(), accepted));
            static_cast<void>(tls.shutdown(write));
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            error_ = e.what();
        }
    }

    const alchemy::host::crypto::Identity* certificate_;
    SOCKET listener_{INVALID_SOCKET};
    std::uint16_t port_{};
    std::thread thread_;
    std::vector<std::thread> connections_;
    std::atomic<bool> stop_{};
    std::mutex mutex_;
    std::vector<std::string> requests_;
    std::size_t handshakes_{};
    std::string error_;
};

// ---- the guest side -----------------------------------------------------------------------------------

struct Fixture {
    transmute::ReferenceEngine engine{transmute::ExecutionStart{0}};
    alchemy::ProcessContext process{engine, [](unsigned, std::span<const std::uint8_t> bytes) {
        return alchemy::OutputResult{bytes.size(), 0}; }};
    alchemy::DarwinServices services{process};
    alchemy::DarwinThreads threads{(alchemy::install_darwin_dispatch(process), process)};
    alchemy::ObjcRuntime objc{process};
    alchemy::CoreFoundation cf{process};
    alchemy::FoundationBridge foundation{process, objc, cf};

    Fixture() {
        alchemy::install_darwin_frameworks(process, objc, cf);
        alchemy::gui::InstallContext context{process, objc, cf};
        alchemy::gui::install_runloop(context);
        alchemy::gui::install_system_configuration(context);
        alchemy::gui::install_security(context);
        alchemy::gui::install_network(context);
    }
    ~Fixture() {
        try { process.request_exit(0); threads.join_workers(); } catch (...) {}
    }
    Address symbol(std::string_view library, std::string_view name) {
        const auto found = process.resolve(library, name);
        require(found.has_value(), "missing symbol " + std::string(name));
        return found->address;
    }
    Address call(std::string_view library, std::string_view name, std::initializer_list<Address> arguments = {}) {
        return engine.invoke_callback(symbol(library, name), std::span(arguments.begin(), arguments.size()));
    }
    Address core(std::string_view name, std::initializer_list<Address> arguments = {}) { return call(alchemy::core_foundation_library, name, arguments); }
    Address cls(std::string_view name) { const auto value = objc.find_class(name); require(value != 0, "missing class " + std::string(name)); return value; }
    Address send(Address receiver, std::string_view selector, std::initializer_list<Address> arguments = {}) {
        alchemy::CallRegs registers;
        std::size_t index = 2;
        for (const auto argument : arguments) registers.x[index++] = argument;
        return objc.send(receiver, selector, registers).x0;
    }
    Address string(std::string_view text) { return cf.string(text); }
    // A guest call with a double in d0 (the reference engine keeps vector registers across invoke_callback, fx takes a full register image).
    template <class Engine>
    static Address call_d0(Engine& engine, Address function, std::initializer_list<Address> arguments, double d0) {
        if constexpr (requires { engine.set_initial_vector(0, {}); }) {
            engine.set_initial_vector(0, {std::bit_cast<std::uint64_t>(d0), 0});
            return engine.invoke_callback(function, std::span(arguments.begin(), arguments.size()));
        } else {
            alchemy::CallRegs registers;
            std::copy(arguments.begin(), arguments.end(), registers.x.begin());
            registers.set_d(0, d0);
            return engine.invoke_callback_regs(function, registers).x0;
        }
    }
    Address call_d0(std::string_view library, std::string_view name, std::initializer_list<Address> arguments, double d0) {
        return call_d0(engine, symbol(library, name), arguments, d0);
    }
    std::string text(Address object) { return object ? cf.utf8(object) : std::string("(nil)"); }
    Address url(std::string_view text) { const auto value = string(text); const auto result = send(cls("NSURL"), "URLWithString:", {value}); return result; }
    // A compiler-emitted (global) block whose invoke is `invoke`.
    Address block(Address invoke) {
        const auto value = process.allocate_guest(32), descriptor = process.allocate_guest(16);
        engine.write64(descriptor, 0); engine.write64(descriptor + 8, 32);
        engine.write64(value, symbol("/usr/lib/libSystem.B.dylib", "__NSConcreteGlobalBlock"));
        engine.write32(value + 8, 1U << 28); engine.write32(value + 12, 0);
        engine.write64(value + 16, invoke); engine.write64(value + 24, descriptor);
        return value;
    }
};

std::filesystem::path host_path(const std::string& guest) {
    if (guest.starts_with("/Volumes/") && guest.size() > 10) return std::filesystem::path(guest.substr(9, 1) + ":" + guest.substr(10));
    return std::filesystem::path(guest);
}

// Results recorded by completion handlers and the test delegate.
struct Record {
    std::mutex mutex;
    bool done{};
    std::int64_t status{}, error{}, challenges{}, responses{}, invalidated{};
    Bytes data;
    std::string location_name, response_name;
    std::uintmax_t file_size{};
    void reset() { std::lock_guard lock(mutex); done = false; status = error = challenges = responses = invalidated = 0; data.clear(); file_size = 0; }
};

struct Harness {
    Fixture& f;
    Record record;
    bool accept_trust{true};
    Address completion{}, download_completion{}, delegate_class{};

    explicit Harness(Fixture& fixture) : f(fixture) {
        const auto b = sec::bridge();
        // (data, response, error)
        completion = f.block(f.process.install_function("test_completion", [this, b](CpuState& s, GuestMemory&) {
            std::lock_guard lock(record.mutex);
            if (s.x[1]) record.data = b->bytes(s.x[1]);
            if (s.x[2]) record.status = static_cast<std::int64_t>(f.objc.send(s.x[2], "statusCode").x0);
            if (s.x[3]) record.error = static_cast<std::int64_t>(f.objc.send(s.x[3], "code").x0);
            record.done = true;
            s.x[0] = 0;
        }, "alchemy.test"));
        // (location, response, error)
        download_completion = f.block(f.process.install_function("test_download_completion", [this](CpuState& s, GuestMemory&) {
            std::lock_guard lock(record.mutex);
            if (s.x[1]) {
                std::error_code error;
                record.file_size = std::filesystem::file_size(host_path(f.cf.url_path(s.x[1])), error);
            }
            if (s.x[2]) {
                record.status = static_cast<std::int64_t>(f.objc.send(s.x[2], "statusCode").x0);
                record.response_name = f.text(f.objc.send(s.x[2], "suggestedFilename").x0);
            }
            if (s.x[3]) record.error = static_cast<std::int64_t>(f.objc.send(s.x[3], "code").x0);
            record.done = true;
            s.x[0] = 0;
        }, "alchemy.test"));
        // The delegate: a guest-visible class with host methods.
        delegate_class = f.objc.register_native_class("AlchemyTestURLDelegate", f.cls("NSObject"), 8);
        const auto method = [this](std::string_view selector, transmute::GuestCallHandler handler) {
            static_cast<void>(f.objc.add_native_method(delegate_class, std::string(selector), std::move(handler), false, "v@:@@@@"));
        };
        method("URLSession:didReceiveChallenge:completionHandler:", [this, b](CpuState& s, GuestMemory&) {
            const auto challenge = s.x[3], handler = s.x[4];
            { std::lock_guard lock(record.mutex); ++record.challenges; }
            const auto space = f.objc.send(challenge, "protectionSpace").x0;
            const auto method_name = f.text(f.objc.send(space, "authenticationMethod").x0);
            if (method_name == "NSURLAuthenticationMethodServerTrust" && accept_trust) {
                alchemy::CallRegs registers;
                registers.x[2] = f.objc.send(space, "serverTrust").x0;
                const auto credential = f.objc.send(f.cls("NSURLCredential"), "credentialForTrust:", registers).x0;
                b->invoke_block(handler, {0, credential});
            } else {
                b->invoke_block(handler, {1, 0});   // default handling
            }
            s.x[0] = 0;
        });
        method("URLSession:task:didReceiveChallenge:completionHandler:", [this, b](CpuState& s, GuestMemory&) {
            const auto challenge = s.x[4], handler = s.x[5];
            { std::lock_guard lock(record.mutex); ++record.challenges; }
            const auto space = f.objc.send(challenge, "protectionSpace").x0;
            if (f.text(f.objc.send(space, "authenticationMethod").x0) == "NSURLAuthenticationMethodHTTPBasic" &&
                f.text(f.objc.send(space, "realm").x0) == "alchemy") {
                alchemy::CallRegs registers;
                registers.x[2] = f.string("artist"); registers.x[3] = f.string("secret"); registers.x[4] = 1;
                const auto credential = f.objc.send(f.cls("NSURLCredential"), "credentialWithUser:password:persistence:", registers).x0;
                b->invoke_block(handler, {0, credential});
            } else {
                b->invoke_block(handler, {1, 0});
            }
            s.x[0] = 0;
        });
        method("URLSession:dataTask:didReceiveResponse:completionHandler:", [this, b](CpuState& s, GuestMemory&) {
            {
                std::lock_guard lock(record.mutex);
                ++record.responses;
                record.status = static_cast<std::int64_t>(f.objc.send(s.x[4], "statusCode").x0);
            }
            // NSURLSessionResponseAllow; the handler takes one argument, so x2
            // is junk the caller left behind (a guest leaves anything there).
            b->invoke_block(s.x[5], {1, 0x18});
            s.x[0] = 0;
        });
        method("URLSession:dataTask:didReceiveData:", [this, b](CpuState& s, GuestMemory&) {
            const auto bytes = b->bytes(s.x[4]);
            std::lock_guard lock(record.mutex);
            record.data.insert(record.data.end(), bytes.begin(), bytes.end());
            s.x[0] = 0;
        });
        method("URLSession:downloadTask:didFinishDownloadingToURL:", [this](CpuState& s, GuestMemory&) {
            std::error_code error;
            std::lock_guard lock(record.mutex);
            record.file_size = std::filesystem::file_size(host_path(f.cf.url_path(s.x[4])), error);
            s.x[0] = 0;
        });
        method("URLSession:task:didCompleteWithError:", [this](CpuState& s, GuestMemory&) {
            std::lock_guard lock(record.mutex);
            record.error = s.x[4] ? static_cast<std::int64_t>(f.objc.send(s.x[4], "code").x0) : 0;
            record.done = true;
            s.x[0] = 0;
        });
        method("URLSession:didBecomeInvalidWithError:", [this](CpuState& s, GuestMemory&) {
            std::lock_guard lock(record.mutex);
            ++record.invalidated;
            s.x[0] = 0;
        });
    }
    void wait(std::string_view what, int seconds = 15) {
        eventually([&] { std::lock_guard lock(record.mutex); return record.done; }, what, seconds);
    }
    Address delegate() { return f.send(f.send(delegate_class, "alloc"), "init"); }
    Address session(Address delegate) {
        const auto configuration = f.send(f.cls("NSURLSessionConfiguration"), "ephemeralSessionConfiguration");
        return f.send(f.cls("NSURLSession"), "sessionWithConfiguration:delegate:delegateQueue:", {configuration, delegate, 0});
    }
};

void test_urls(Fixture& f) {
    const auto url = f.url("https://user:pw@example.com:8443/a/b%20c.txt?q=1&r=2#frag");
    require(url != 0, "URLWithString: returned nil for an https URL");
    require(f.text(f.send(url, "scheme")) == "https" && f.text(f.send(url, "host")) == "example.com", "scheme / host");
    require(f.core("_CFNumberGetTypeID") && f.send(f.send(url, "port"), "integerValue") == 8443, "port");
    require(f.text(f.send(url, "path")) == "/a/b c.txt" && f.text(f.send(url, "query")) == "q=1&r=2" && f.text(f.send(url, "fragment")) == "frag",
            "path / query / fragment");
    require(f.text(f.send(url, "user")) == "user" && f.text(f.send(url, "password")) == "pw", "user / password");
    require(f.text(f.send(url, "lastPathComponent")) == "b c.txt" && f.text(f.send(url, "pathExtension")) == "txt", "path components");
    require((f.send(url, "isFileURL") & 0xff) == 0, "network URL claims to be a file URL");
    const auto relative = f.string("../d/e?x=1");
    const auto resolved = f.send(f.cls("NSURL"), "URLWithString:relativeToURL:", {relative, url});
    require(f.text(f.send(resolved, "absoluteString")) == "https://user:pw@example.com:8443/d/e?x=1", "relative resolution: " +
            f.text(f.send(resolved, "absoluteString")));
    const auto spaced = f.url("http://example.com/a b");
    require(f.text(f.send(spaced, "absoluteString")) == "http://example.com/a%20b", "invalid characters are percent-encoded");
    const auto appended = f.send(f.url("https://example.com/api"), "URLByAppendingPathComponent:", {f.string("v1 x")});
    require(f.text(f.send(appended, "absoluteString")) == "https://example.com/api/v1%20x", "URLByAppendingPathComponent:");
    // File URLs stay Foundation's.
    const auto file = f.url("file:///tmp/alchemy.txt");
    require(file != 0, "file URL is nil");
    require((f.send(file, "isFileURL") & 0xff) == 1 && f.text(f.send(file, "path")).ends_with("/alchemy.txt"),
            "file URL: " + f.text(f.send(file, "path")));
    // CFURL access.
    const auto text = f.string("http://[fd00::1]:8080/x");
    const auto cfurl = f.core("_CFURLCreateWithString", {0, text, 0});
    const auto host = f.core("_CFURLCopyHostName", {cfurl});
    require(f.text(host) == "fd00::1" && f.core("_CFURLGetPortNumber", {cfurl}) == 8080, "CFURL host / port");
    require(f.text(f.core("_CFURLGetString", {cfurl})) == "http://[fd00::1]:8080/x", "CFURLGetString");
    require(f.core("_CFURLCreateWithString", {0, f.string("no scheme here"), 0}) == 0, "a string without a scheme is not a URL");
    for (const auto object : {host, cfurl, text}) static_cast<void>(f.core("_CFRelease", {object}));
}

void test_operation_queue(Fixture& f) {
    std::atomic<int> ran{};
    const auto block = f.block(f.process.install_function("test_operation", [&](CpuState& s, GuestMemory&) { ++ran; s.x[0] = 0; }, "alchemy.test"));
    const auto queue = f.send(f.send(f.cls("NSOperationQueue"), "alloc"), "init");
    alchemy::CallRegs one;
    one.x[2] = 1;
    static_cast<void>(f.objc.send(queue, "setMaxConcurrentOperationCount:", one));
    for (int index = 0; index < 3; ++index) static_cast<void>(f.send(queue, "addOperationWithBlock:", {block}));
    static_cast<void>(f.send(queue, "waitUntilAllOperationsAreFinished"));
    require(ran == 3, "NSOperationQueue blocks did not run");
    require(f.send(f.cls("NSOperationQueue"), "mainQueue") != 0, "+[NSOperationQueue mainQueue]");
}

// The NSURLError codes of an exchange that never sent (the old WinHTTP values).
void test_unsent_exchange() {
    using namespace alchemy::gui::network;
    const auto client = HttpClient::create("Alchemy test", ProxyConfiguration{});
    HttpRequest request;
    request.url = *parse_url("https://example.invalid/");
    std::uint8_t buffer[16];
    std::optional<HttpFailure> failure;
    HttpExchange unsent(client, request, [](const auto&) { return true; });
    require(unsent.read(buffer, sizeof(buffer), failure) == 0 && failure && failure->code == url_error::unknown,
            "reading an unsent exchange: NSURLErrorUnknown");
    HttpExchange cancelled(client, request, [](const auto&) { return true; });
    cancelled.cancel();
    failure.reset();
    require(cancelled.read(buffer, sizeof(buffer), failure) == 0 && failure && failure->code == url_error::cancelled,
            "reading a cancelled exchange: NSURLErrorCancelled");
    const auto outcome = cancelled.send();
    require(std::holds_alternative<HttpFailure>(outcome) && std::get<HttpFailure>(outcome).code == url_error::cancelled,
            "sending a cancelled exchange: NSURLErrorCancelled");
}

void test_http_message(Fixture& f) {
    const auto lib = sec::cfnetwork_library;
    const auto method = f.string("POST");
    const auto url = f.url("https://example.com/upload?x=1");
    const auto version = f.engine.read64(f.symbol(lib, "_kCFHTTPVersion1_1"));
    const auto message = f.call(lib, "_CFHTTPMessageCreateRequest", {0, method, url, version});
    static_cast<void>(f.call(lib, "_CFHTTPMessageSetHeaderFieldValue", {message, f.string("Content-Type"), f.string("text/plain")}));
    static_cast<void>(f.call(lib, "_CFHTTPMessageSetBody", {message, sec::bridge()->data(Bytes{'h', 'i'})}));
    const auto serialized = sec::bridge()->bytes(f.call(lib, "_CFHTTPMessageCopySerializedMessage", {message}));
    const std::string text(serialized.begin(), serialized.end());
    require(text.starts_with("POST /upload?x=1 HTTP/1.1\r\n") && text.find("Host: example.com\r\n") != std::string::npos &&
            text.find("Content-Type: text/plain\r\n") != std::string::npos && text.ends_with("\r\n\r\nhi"), "serialized request: " + text);
    const auto response = f.call(lib, "_CFHTTPMessageCreateEmpty", {0, 0});
    const std::string raw = "HTTP/1.1 404 Not Found\r\nX-Test: yes\r\n\r\nbody";
    const auto bytes = f.process.allocate_guest(raw.size());
    f.engine.write_bytes(bytes, {reinterpret_cast<const std::uint8_t*>(raw.data()), raw.size()});
    require((f.call(lib, "_CFHTTPMessageAppendBytes", {response, bytes, 10}) & 0xff) == 1 &&
            (f.call(lib, "_CFHTTPMessageIsHeaderComplete", {response}) & 0xff) == 0, "partial header");
    require((f.call(lib, "_CFHTTPMessageAppendBytes", {response, bytes + 10, raw.size() - 10}) & 0xff) == 1 &&
            (f.call(lib, "_CFHTTPMessageIsHeaderComplete", {response}) & 0xff) == 1, "complete header");
    require(f.call(lib, "_CFHTTPMessageGetResponseStatusCode", {response}) == 404, "parsed status");
    require(f.text(f.call(lib, "_CFHTTPMessageCopyHeaderFieldValue", {response, f.string("x-test")})) == "yes", "parsed header");
    require(sec::bridge()->bytes(f.call(lib, "_CFHTTPMessageCopyBody", {response})) == Bytes{'b', 'o', 'd', 'y'}, "parsed body");
    // Proxies for a URL from an explicit settings dictionary.
    const auto key = [&](std::string_view name) { return f.engine.read64(f.symbol(lib, "_kCFNetworkProxies" + std::string(name))); };
    const auto settings = f.cf.dictionary(std::vector<std::pair<Address, Address>>{
        {key("HTTPSEnable"), f.cf.integer(1, 4)}, {key("HTTPSProxy"), f.string("proxy.example")}, {key("HTTPSPort"), f.cf.integer(3128, 4)}});
    const auto proxies = f.call(lib, "_CFNetworkCopyProxiesForURL", {url, settings});
    const auto first = f.core("_CFArrayGetValueAtIndex", {proxies, 0});
    require(f.text(f.core("_CFDictionaryGetValue", {first, f.engine.read64(f.symbol(lib, "_kCFProxyTypeKey"))})) == "kCFProxyTypeHTTPS" &&
            f.text(f.core("_CFDictionaryGetValue", {first, f.engine.read64(f.symbol(lib, "_kCFProxyHostNameKey"))})) == "proxy.example",
            "CFNetworkCopyProxiesForURL");
    const auto system = f.call(lib, "_CFNetworkCopySystemProxySettings");
    require(system != 0, "CFNetworkCopySystemProxySettings");
}

void test_sockets(Fixture& f) {
    struct Event { std::uint64_t type; Bytes data; std::int32_t fd{-1}; };
    std::vector<Event> events;
    const auto b = sec::bridge();
    const auto callout = f.process.install_function("test_socket_callout", [&](CpuState& s, GuestMemory& m) {
        // (socket, type, address, data, info)
        Event event{s.x[1], {}, -1};
        if (s.x[1] == 2 && s.x[3]) event.fd = static_cast<std::int32_t>(m.read32(s.x[3]));
        if (s.x[1] == 3 && s.x[3]) event.data = b->bytes(s.x[3]);
        events.push_back(std::move(event));
        s.x[0] = 0;
    }, "alchemy.test");
    const auto core = alchemy::core_foundation_library;
    const auto mode = f.engine.read64(f.symbol(core, "_kCFRunLoopDefaultMode"));
    const auto loop = f.core("_CFRunLoopGetCurrent");
    const auto run_until = [&](auto done, std::string_view what) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            require(std::chrono::steady_clock::now() < until, "timed out: " + std::string(what));
            static_cast<void>(f.call_d0(core, "_CFRunLoopRunInMode", {mode, 1}, 0.1));
        }
    };
    const auto count = [&](std::uint64_t type) { return std::count_if(events.begin(), events.end(), [&](const Event& e) { return e.type == type; }); };

    // Listener: CFSocketCreate + CFSocketSetAddress(127.0.0.1:0) with an accept callback.
    const auto listener = f.core("_CFSocketCreate", {0, 2, 1, 6, 2, callout, 0});
    require(listener != 0, "CFSocketCreate");
    require(f.core("_CFSocketGetTypeID") == f.core("_CFGetTypeID", {listener}), "CFSocket type id");
    require(f.core("_CFSocketGetSocketFlags", {listener}) == 131, "default CFSocket flags");
    const Bytes any{16, 2, 0, 0, 127, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};   // sockaddr_in 127.0.0.1:0
    require(f.core("_CFSocketSetAddress", {listener, b->data(any)}) == 0, "CFSocketSetAddress");
    const auto bound = b->bytes(f.core("_CFSocketCopyAddress", {listener}));
    require(bound.size() >= 8 && bound[1] == 2, "CFSocketCopyAddress");
    const auto port = static_cast<std::uint16_t>((bound[2] << 8) | bound[3]);
    const auto listen_source = f.core("_CFSocketCreateRunLoopSource", {0, listener, 0});
    require(listen_source != 0 && f.core("_CFSocketCreateRunLoopSource", {0, listener, 0}) == listen_source, "one run-loop source per socket");
    static_cast<void>(f.core("_CFRunLoopAddSource", {loop, listen_source, mode}));

    // A host peer connects: the accept callback carries the new descriptor.
    const auto peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in target{};
    target.sin_family = AF_INET; target.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &target.sin_addr);
    require(connect(peer, reinterpret_cast<sockaddr*>(&target), sizeof(target)) == 0, "host connect to the CFSocket listener");
    run_until([&] { return count(2) == 1; }, "accept callback");
    const auto accepted = events.back().fd;
    require(accepted >= 0, "accepted descriptor");

    // The accepted descriptor with data + write callbacks.
    const auto connection = f.core("_CFSocketCreateWithNative", {0, static_cast<Address>(accepted), 3 | 8, callout, 0});
    require(f.core("_CFSocketGetNative", {connection}) == static_cast<Address>(accepted), "CFSocketGetNative");
    const auto source = f.core("_CFSocketCreateRunLoopSource", {0, connection, 0});
    static_cast<void>(f.core("_CFRunLoopAddSource", {loop, source, mode}));
    run_until([&] { return count(8) == 1; }, "write callback");
    require(send(peer, "ping", 4, 0) == 4, "host send");
    run_until([&] { Bytes all; for (const auto& e : events) if (e.type == 3) all.insert(all.end(), e.data.begin(), e.data.end()); return all.size() >= 4; },
              "data callback");
    {
        Bytes all;
        for (const auto& e : events) if (e.type == 3) all.insert(all.end(), e.data.begin(), e.data.end());
        require(all == Bytes{'p', 'i', 'n', 'g'}, "data callback bytes");
    }
    require(count(8) == 1, "the write callback is not re-enabled by default");
    // End of stream: a zero-length CFData.
    closesocket(peer);
    run_until([&] { return std::any_of(events.begin(), events.end(), [](const Event& e) { return e.type == 3 && e.data.empty(); }); }, "end of stream");

    for (const auto object : {connection, listener}) {
        static_cast<void>(f.core("_CFSocketInvalidate", {object}));
        require(f.core("_CFSocketIsValid", {object}) == 0, "CFSocketInvalidate");
    }
    require(f.core("_CFRunLoopSourceIsValid", {source}) == 0, "invalidating the socket invalidates its source");
    for (const auto object : {source, listen_source, listen_source, connection, listener}) static_cast<void>(f.core("_CFRelease", {object}));
}

// A plain HTTP server for a proxy auto-configuration script.
class PacServer {
public:
    explicit PacServer(std::string script) : script_(std::move(script)) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        require(bind(listener_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 && listen(listener_, 16) == 0, "PAC listen failed");
        int length = sizeof(address);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        thread_ = std::thread([this] {
            for (;;) {
                const auto client = accept(listener_, nullptr, nullptr);
                if (client == INVALID_SOCKET) return;
                std::string request;
                char buffer[2048];
                while (request.find("\r\n\r\n") == std::string::npos) {
                    const auto got = recv(client, buffer, sizeof(buffer), 0);
                    if (got <= 0) break;
                    request.append(buffer, static_cast<std::size_t>(got));
                }
                ++requests_;
                const auto response = "HTTP/1.1 200 OK\r\nContent-Type: application/x-ns-proxy-autoconfig\r\nContent-Length: " +
                    std::to_string(script_.size()) + "\r\nConnection: close\r\n\r\n" + script_;
                send(client, response.data(), static_cast<int>(response.size()), 0);
                shutdown(client, SD_SEND);
                closesocket(client);
            }
        });
    }
    ~PacServer() { closesocket(listener_); if (thread_.joinable()) thread_.join(); }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port_) + "/proxy.pac"; }
    int requests() const { return requests_; }
private:
    std::string script_;
    SOCKET listener_{INVALID_SOCKET};
    std::uint16_t port_{};
    std::thread thread_;
    std::atomic<int> requests_{};
};

void test_proxy_auto_configuration(Fixture& f) {
    PacServer server("function FindProxyForURL(url, host) {\n"
                     "  if (host == \"direct.example\") return \"DIRECT\";\n"
                     "  return \"PROXY proxy.example:3128; PROXY backup.example:8080\";\n}\n");
    const auto lib = sec::cfnetwork_library;
    const auto core = alchemy::core_foundation_library;
    struct Result { bool done{}; std::vector<std::string> entries; std::int64_t error{}; };
    Result result;
    const auto b = sec::bridge();
    const auto key = [&](std::string_view name) { return f.engine.read64(f.symbol(lib, "_" + std::string(name))); };
    const auto callback = f.process.install_function("test_pac_callback", [&](CpuState& s, GuestMemory&) {
        // (client, proxyList, error)
        result.done = true;
        if (s.x[2]) result.error = static_cast<std::int64_t>(f.core("_CFErrorGetCode", {s.x[2]}));
        if (s.x[1]) for (const auto entry : f.cf.array_values(s.x[1])) {
            std::string text = f.text(f.core("_CFDictionaryGetValue", {entry, key("kCFProxyTypeKey")}));
            if (const auto host = f.core("_CFDictionaryGetValue", {entry, key("kCFProxyHostNameKey")})) {
                text += " " + f.text(host) + ":" + std::to_string(f.send(f.core("_CFDictionaryGetValue", {entry, key("kCFProxyPortNumberKey")}), "integerValue"));
            }
            result.entries.push_back(text);
        }
        s.x[0] = 0;
    }, "alchemy.test");
    const auto mode = f.engine.read64(f.symbol(core, "_kCFRunLoopDefaultMode"));
    const auto loop = f.core("_CFRunLoopGetCurrent");
    const auto run = [&](std::string_view script, std::string_view target) {
        result = {};
        const auto source = f.call(lib, "_CFNetworkExecuteProxyAutoConfigurationURL", {f.url(script), f.url(target), callback, 0});
        require(source != 0, "CFNetworkExecuteProxyAutoConfigurationURL");
        static_cast<void>(f.core("_CFRunLoopAddSource", {loop, source, mode}));
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!result.done) {
            require(std::chrono::steady_clock::now() < until, "timed out waiting for the PAC callback");
            static_cast<void>(f.call_d0(core, "_CFRunLoopRunInMode", {mode, 1}, 0.2));
        }
        static_cast<void>(f.core("_CFRunLoopRemoveSource", {loop, source, mode}));
        static_cast<void>(f.core("_CFRelease", {source}));
    };
    run(server.url(), "https://www.example.com/x");
    const std::vector<std::string> expected{"kCFProxyTypeHTTPS proxy.example:3128", "kCFProxyTypeHTTPS backup.example:8080"};
    if (result.error != 0 && result.entries.empty()) {
        // WinHTTP's autoproxy service may be disabled by policy on this host.
        std::cout << "PAC evaluation unavailable (CFNetwork error " << result.error << "), skipped\n";
        return;
    }
    require(result.entries == expected, "PAC proxies: " + (result.entries.empty() ? std::string("(none)") : result.entries.front()));
    require(server.requests() >= 1, "the PAC script was not fetched");
    run(server.url(), "http://direct.example/");
    require(result.entries == std::vector<std::string>{"kCFProxyTypeNone"}, "PAC DIRECT");
    run("http://127.0.0.1:1/missing.pac", "http://example.com/");
    require(result.error == 2 && result.entries.empty(), "an unreachable PAC script reports kCFErrorPACFileError");
}

void test_system_configuration(Fixture& f) {
    const std::string lib = "/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration";
    const auto store = f.call(lib, "_SCDynamicStoreCreate", {0, f.string("alchemy.test"), 0, 0});
    require(store != 0, "SCDynamicStoreCreate");
    wchar_t host[256]{};
    DWORD host_size = 256;
    require(GetComputerNameExW(ComputerNamePhysicalDnsHostname, host, &host_size) != 0, "host computer name");
    const auto encoding = f.process.allocate_guest(8);
    const auto name = f.call(lib, "_SCDynamicStoreCopyComputerName", {store, encoding});
    require(sec::wide(f.text(name)) == std::wstring(host, host_size) && f.engine.read32(encoding) == 0x08000100, "SCDynamicStoreCopyComputerName");
    const auto ids = f.process.allocate_guest(8);
    require(!f.text(f.call(lib, "_SCDynamicStoreCopyConsoleUser", {store, ids, ids + 4})).empty() && f.engine.read32(ids) == 1000,
            "SCDynamicStoreCopyConsoleUser");
    const auto system = f.call(lib, "_SCDynamicStoreCopyValue", {store, f.string("Setup:/System")});
    require(system && f.text(f.core("_CFDictionaryGetValue", {system, f.string("ComputerName")})) == f.text(name), "Setup:/System");
    const auto interfaces = f.call(lib, "_SCDynamicStoreCopyValue", {store, f.string("State:/Network/Interface")});
    require(interfaces && f.core("_CFDictionaryGetValue", {interfaces, f.string("Interfaces")}) != 0, "State:/Network/Interface");
    require(f.call(lib, "_SCDynamicStoreCopyValue", {store, f.string("State:/No/Such/Key")}) == 0, "an unknown key is absent");
    const auto keys = f.call(lib, "_SCDynamicStoreCopyKeyList", {store, f.string("State:/Network/Interface/[^/]+/IPv4")});
    require(f.core("_CFArrayGetCount", {keys}) >= 1, "SCDynamicStoreCopyKeyList (loopback IPv4)");
    const auto global = f.call(lib, "_SCDynamicStoreKeyCreateNetworkGlobalEntity",
                               {0, f.engine.read64(f.symbol(lib, "_kSCDynamicStoreDomainState")), f.engine.read64(f.symbol(lib, "_kSCEntNetIPv4"))});
    require(f.text(global) == "State:/Network/Global/IPv4", "SCDynamicStoreKeyCreateNetworkGlobalEntity");
    // Notifications: registration on a queue and its removal.
    require(f.call(lib, "_SCDynamicStoreSetNotificationKeys", {store, f.cf.array(std::vector<Address>{global}), 0}) == 1,
            "SCDynamicStoreSetNotificationKeys");
    const auto queue = sec::bridge()->create_serial_queue("alchemy.test.sc");
    require(f.call(lib, "_SCDynamicStoreSetDispatchQueue", {store, queue}) == 1, "SCDynamicStoreSetDispatchQueue");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    require(f.call(lib, "_SCDynamicStoreSetDispatchQueue", {store, 0}) == 1, "SCDynamicStoreSetDispatchQueue(NULL)");

    // Reachability.
    const auto cstring = [&](std::string_view text) {
        const auto address = f.process.allocate_guest(text.size() + 1);
        f.engine.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        f.engine.fill_bytes(address + text.size(), 0, 1);
        return address;
    };
    const auto flags = f.process.allocate_guest(4);
    const auto local = f.call(lib, "_SCNetworkReachabilityCreateWithName", {0, cstring("localhost")});
    require(f.call(lib, "_SCNetworkReachabilityGetFlags", {local, flags}) == 1 && (f.engine.read32(flags) & ((1U << 1) | (1U << 16))) == ((1U << 1) | (1U << 16)),
            "localhost is reachable and local");
    const auto missing = f.call(lib, "_SCNetworkReachabilityCreateWithName", {0, cstring("alchemy-no-such-host.invalid")});
    require(f.call(lib, "_SCNetworkReachabilityGetFlags", {missing, flags}) == 1 && f.engine.read32(flags) == 0, "an unresolvable name is not reachable");
    const auto address = f.process.allocate_guest(16);
    const std::array<std::uint8_t, 16> loopback{16, 2, 0, 0, 127, 0, 0, 1};
    f.engine.write_bytes(address, loopback);
    const auto by_address = f.call(lib, "_SCNetworkReachabilityCreateWithAddress", {0, address});
    require(f.call(lib, "_SCNetworkReachabilityGetFlags", {by_address, flags}) == 1 && (f.engine.read32(flags) & (1U << 1)), "127.0.0.1 is reachable");
    // Change callbacks on a dispatch queue and through a run loop (the change
    // comes from the test hook: the host's network does not change here).
    std::atomic<std::uint64_t> seen_target{}, seen_flags{}, seen_info{};
    std::atomic<int> callbacks{};
    const auto callout = f.process.install_function("test_reachability_callout", [&](CpuState& s, GuestMemory&) {
        seen_target = s.x[0]; seen_flags = s.x[1] & 0xffffffff; seen_info = s.x[2];
        ++callbacks;
    }, "alchemy.test");
    const auto context = f.process.allocate_guest(40);
    f.engine.fill_bytes(context, 0, 40);
    f.engine.write64(context + 8, 0x5ca1ab1e);
    require(f.call(lib, "_SCNetworkReachabilitySetCallback", {by_address, callout, context}) == 1, "SCNetworkReachabilitySetCallback");
    const auto inject = f.process.resolve("alchemy.internal", "__alchemy_sc_reachability_inject")->address;
    require(f.call(lib, "_SCNetworkReachabilitySetDispatchQueue", {by_address, queue}) == 1, "SCNetworkReachabilitySetDispatchQueue");
    static_cast<void>(f.engine.invoke_callback(inject, std::array<Address, 2>{by_address, (1U << 1) | (1U << 18)}));
    eventually([&] { return callbacks.load() == 1; }, "reachability callback on the queue");
    require(seen_target == by_address && seen_flags == ((1U << 1) | (1U << 18)) && seen_info == 0x5ca1ab1e, "reachability callback arguments");
    require(f.call(lib, "_SCNetworkReachabilitySetDispatchQueue", {by_address, 0}) == 1, "SCNetworkReachabilitySetDispatchQueue(NULL)");
    const auto core = alchemy::core_foundation_library;
    const auto mode = f.engine.read64(f.symbol(core, "_kCFRunLoopDefaultMode"));
    const auto loop = f.core("_CFRunLoopGetCurrent");
    require(f.call(lib, "_SCNetworkReachabilityScheduleWithRunLoop", {by_address, loop, mode}) == 1, "SCNetworkReachabilityScheduleWithRunLoop");
    static_cast<void>(f.engine.invoke_callback(inject, std::array<Address, 2>{by_address, 1U << 1}));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (callbacks.load() < 2) {
        require(std::chrono::steady_clock::now() < until, "timed out: reachability callback through the run loop");
        static_cast<void>(f.call_d0(core, "_CFRunLoopRunInMode", {mode, 1}, 0.1));
    }
    require(seen_flags == (1U << 1) && callbacks.load() == 2, "reachability callback through the run loop");
    require(f.call(lib, "_SCNetworkReachabilityUnscheduleFromRunLoop", {by_address, loop, mode}) == 1, "SCNetworkReachabilityUnscheduleFromRunLoop");
    require(f.call(lib, "_SCNetworkReachabilityUnscheduleFromRunLoop", {by_address, loop, mode}) == 0, "not scheduled any more");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // the watcher ends and drops its reference

    // A watcher holding the last reference deallocates the object when it
    // ends (its release took the object's lock while holding it: "resource
    // deadlock would occur" before). The contexts' release callbacks count.
    static std::atomic<int> released_infos{};
    const auto release_info = f.process.install_function("test_sc_release_info", [](CpuState&, GuestMemory&) { ++released_infos; }, "alchemy.test");
    const auto info_context = f.process.allocate_guest(40);
    f.engine.fill_bytes(info_context, 0, 40);
    f.engine.write64(info_context + 8, 0x1234);
    f.engine.write64(info_context + 24, release_info);
    const auto last_store = f.call(lib, "_SCDynamicStoreCreate", {0, f.string("alchemy.test.last"), callout, info_context});
    require(f.call(lib, "_SCDynamicStoreSetDispatchQueue", {last_store, queue}) == 1, "store watcher started");
    require(f.call(lib, "_SCDynamicStoreSetDispatchQueue", {last_store, 0}) == 1, "store watcher stopped");
    static_cast<void>(f.core("_CFRelease", {last_store}));   // the watcher now holds the last reference
    const auto last_target = f.call(lib, "_SCNetworkReachabilityCreateWithAddress", {0, address});
    require(f.call(lib, "_SCNetworkReachabilitySetCallback", {last_target, callout, info_context}) == 1, "reachability callback");
    require(f.call(lib, "_SCNetworkReachabilitySetDispatchQueue", {last_target, queue}) == 1, "reachability watcher started");
    require(f.call(lib, "_SCNetworkReachabilitySetDispatchQueue", {last_target, 0}) == 1, "reachability watcher stopped");
    static_cast<void>(f.core("_CFRelease", {last_target}));
    eventually([&] { return released_infos.load() == 2; }, "watchers deallocate the store and the target they held last");
    for (const auto object : {local, missing, by_address, store}) static_cast<void>(f.core("_CFRelease", {object}));
}

// libresolv: res_ninit / res_nsearch / res_getservers against the Windows DNS servers.
void test_resolver(Fixture& f, bool online) {
    const std::string lib = "/usr/lib/libresolv.9.dylib";
    const auto state = f.process.allocate_guest(552);
    require(f.call(lib, "_res_9_ninit", {state}) == 0 && (f.engine.read32(state + 8) & 1), "res_ninit");
    const auto servers = f.process.allocate_guest(128 * 3);
    const auto count = static_cast<std::int32_t>(f.call(lib, "_res_9_getservers", {state, servers, 3}));
    require(count >= 0 && count <= 3, "res_getservers");
    const auto cstring = [&](std::string_view text) {
        const auto address = f.process.allocate_guest(text.size() + 1);
        f.engine.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        f.engine.fill_bytes(address + text.size(), 0, 1);
        return address;
    };
    const auto answer = f.process.allocate_guest(4096);
    // An NXDOMAIN (or, offline, a failure) sets h_errno in the res_state.
    const auto missing = static_cast<std::int64_t>(f.call(lib, "_res_9_nsearch", {state, cstring("alchemy-no-such-host.invalid."), 1, 1, answer, 4096}));
    const auto h_errno_value = f.engine.read32(state + 496);
    require(missing == -1 && (h_errno_value == 1 || h_errno_value == 2), "res_nsearch for an invalid name: h_errno " + std::to_string(h_errno_value));
    if (online && count > 0) {
        const auto length = static_cast<std::int64_t>(f.call(lib, "_res_9_nsearch", {state, cstring("example.com"), 1, 1, answer, 4096}));
        if (length < 0) {
            std::cout << "res_nsearch example.com: DNS unavailable (h_errno " << f.engine.read32(state + 496) << "), skipped\n";
        } else {
            Bytes header(12);
            f.engine.read_bytes(answer, header);
            require(length >= 12 && (header[2] & 0x80) && ((header[6] << 8) | header[7]) >= 1, "res_nsearch answer message");
            std::cout << "res_nsearch example.com: " << length << "-byte answer, " << ((header[6] << 8) | header[7]) << " records\n";
        }
    }
    static_cast<void>(f.call(lib, "_res_9_ndestroy", {state}));
}

void test_sessions(Fixture& f, Harness& h, HttpsServer& server) {
    const auto data_task = [&](Address session, const std::string& path, Address handler) {
        const auto task = f.send(session, "dataTaskWithURL:completionHandler:", {f.url(server.base() + path), handler});
        require(task != 0, "dataTaskWithURL:completionHandler:");
        static_cast<void>(f.send(task, "resume"));
        return task;
    };

    // 1. No delegate: the self-signed server is rejected before any request byte is sent.
    const auto shared = f.send(f.cls("NSURLSession"), "sharedSession");
    h.record.reset();
    static_cast<void>(data_task(shared, "/hello-untrusted", h.completion));
    h.wait("untrusted request");
    require(h.record.error == net::url_error::certificate_untrusted, "untrusted server gave error " + std::to_string(h.record.error));
    for (const auto& request : server.requests())
        require(request.find("/hello-untrusted") == std::string::npos, "the untrusted server received the request");

    // 2. A delegate accepts the server trust: completion handler with data.
    const auto delegate = h.delegate();
    const auto session = h.session(delegate);
    h.record.reset();
    const auto task = data_task(session, "/hello", h.completion);
    h.wait("trusted request");
    {
        std::lock_guard lock(h.record.mutex);
        require(h.record.error == 0 && h.record.status == 200 && std::string(h.record.data.begin(), h.record.data.end()) == "hello world",
                "trusted request: error " + std::to_string(h.record.error) + " status " + std::to_string(h.record.status));
        require(h.record.challenges >= 1, "the server-trust challenge was not delivered");
    }
    const auto response = f.send(task, "response");
    require(f.text(f.send(response, "MIMEType")) == "text/plain" && f.text(f.send(response, "textEncodingName")) == "utf-8", "MIME type / encoding");
    require(f.send(task, "state") == 3, "NSURLSessionTaskStateCompleted");

    // 3. Redirects are followed.
    h.record.reset();
    const auto redirected = data_task(session, "/redirect", h.completion);
    h.wait("redirect");
    require(h.record.status == 200 && std::string(h.record.data.begin(), h.record.data.end()) == "hello world", "redirect not followed");
    require(f.text(f.send(f.send(f.send(redirected, "currentRequest"), "URL"), "path")) == "/hello", "currentRequest after the redirect");

    // 4. Upload with a body.
    h.record.reset();
    const auto request = f.send(f.cls("NSMutableURLRequest"), "requestWithURL:", {f.url(server.base() + "/echo")});
    static_cast<void>(f.send(request, "setHTTPMethod:", {f.string("POST")}));
    static_cast<void>(f.send(request, "setValue:forHTTPHeaderField:", {f.string("application/json"), f.string("Content-Type")}));
    require(f.text(f.send(request, "valueForHTTPHeaderField:", {f.string("content-type")})) == "application/json", "header lookup is case-insensitive");
    const auto body = sec::bridge()->data(Bytes{'{', '"', 'a', '"', ':', '1', '}'});
    const auto upload = f.send(session, "uploadTaskWithRequest:fromData:completionHandler:", {request, body, h.completion});
    static_cast<void>(f.send(upload, "resume"));
    h.wait("upload");
    require(std::string(h.record.data.begin(), h.record.data.end()) == "{\"a\":1}", "echoed upload body");

    // 5. Download to a file.
    h.record.reset();
    const auto download = f.send(session, "downloadTaskWithURL:completionHandler:", {f.url(server.base() + "/download"), h.download_completion});
    static_cast<void>(f.send(download, "resume"));
    h.wait("download");
    require(h.record.file_size == 100000 && h.record.response_name == "payload.bin", "download size " + std::to_string(h.record.file_size));

    // 6. Delegate-driven data task (response disposition, streamed data, completion).
    h.record.reset();
    const auto streamed = f.send(session, "dataTaskWithURL:", {f.url(server.base() + "/download")});
    static_cast<void>(f.send(streamed, "resume"));
    h.wait("delegate data task");
    require(h.record.responses == 1 && h.record.status == 200 && h.record.data.size() == 100000 && h.record.error == 0, "delegate data task");
    require(f.send(streamed, "countOfBytesReceived") == 100000, "countOfBytesReceived");

    // 7. HTTP Basic authentication through the task challenge.
    h.record.reset();
    static_cast<void>(data_task(session, "/auth", h.completion));
    h.wait("authentication");
    require(h.record.status == 200 && std::string(h.record.data.begin(), h.record.data.end()) == "authorized", "Basic authentication");

    // 8. Cancellation.
    h.record.reset();
    const auto slow = data_task(session, "/slow", h.completion);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    static_cast<void>(f.send(slow, "cancel"));
    h.wait("cancellation");
    require(h.record.error == net::url_error::cancelled, "cancelled task error " + std::to_string(h.record.error));

    // 9. Invalidation reaches the delegate.
    static_cast<void>(f.send(session, "finishTasksAndInvalidate"));
    eventually([&] { std::lock_guard lock(h.record.mutex); return h.record.invalidated == 1; }, "didBecomeInvalidWithError:");
    require(server.error().empty(), "server error: " + server.error());
}

void test_internet(Fixture& f, Harness& h) {
    h.record.reset();
    const auto shared = f.send(f.cls("NSURLSession"), "sharedSession");
    const auto task = f.send(shared, "dataTaskWithURL:completionHandler:", {f.url("https://example.com/"), h.completion});
    static_cast<void>(f.send(task, "resume"));
    h.wait("https://example.com", 60);
    std::lock_guard lock(h.record.mutex);
    const auto offline = {net::url_error::cannot_find_host, net::url_error::cannot_connect, net::url_error::not_connected,
                          net::url_error::timed_out, net::url_error::connection_lost};
    if (std::find(offline.begin(), offline.end(), h.record.error) != offline.end()) {
        std::cout << "https://example.com: network unavailable (NSURLError " << h.record.error << "), skipped\n";
        return;
    }
    require(h.record.error == 0, "https://example.com failed with NSURLError " + std::to_string(h.record.error));
    require(h.record.status >= 200 && h.record.status < 500, "https://example.com status " + std::to_string(h.record.status));
    std::cout << "https://example.com: HTTP " << h.record.status << ", " << h.record.data.size() << " bytes, trusted by the Windows roots\n";
}

} // namespace

// Bonjour: dns_sd TXT helpers and full names, NSNetService TXT dictionaries,
// and (when the Windows DNS-SD client is available) an NSNetService
// publication found by NSNetServiceBrowser and by dns_sd's DNSServiceBrowse
// on a shared connection, resolved by NSNetService, looked up with
// DNSServiceGetAddrInfo, and withdrawn (browse removals on both APIs); a
// dns_sd DNSServiceRegister / DNSServiceResolve pair with stack-passed
// arguments. ALCHEMY_TEST_NO_MDNS=1 skips the discovery part.
void test_bonjour(Fixture& f) {
    const std::string lib = "/usr/lib/libSystem.B.dylib";
    const auto core = alchemy::core_foundation_library;
    const auto b = sec::bridge();
    auto& guest = static_cast<GuestMemory&>(f.engine);
    const auto cstring = [&](std::string_view text) {
        const auto address = f.process.allocate_guest(text.size() + 1);
        f.engine.write_bytes(address, std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        f.engine.fill_bytes(address + text.size(), 0, 1);
        return address;
    };
    const auto read_text = [&](Address at) {
        std::string text;
        for (; at; ++at) { const auto c = guest.read8(at); if (!c) break; text.push_back(static_cast<char>(c)); }
        return text;
    };
    const auto read = [&](Address at, std::size_t size) { Bytes bytes(size); if (size) f.engine.read_bytes(at, bytes); return bytes; };

    // ---- DNSServiceConstructFullName and TXTRecord* ------------------------------------------------
    const auto full = f.process.allocate_guest(1024);
    require(f.call(lib, "_DNSServiceConstructFullName", {full, cstring("My.Show\\1"), cstring("_qlab._tcp"), cstring("local")}) == 0 &&
            read_text(full) == "My\\.Show\\\\1._qlab._tcp.local.", "DNSServiceConstructFullName: " + read_text(full));
    const auto txt = f.process.allocate_guest(16);
    static_cast<void>(f.call(lib, "_TXTRecordCreate", {txt, 0, 0}));
    require(f.call(lib, "_TXTRecordSetValue", {txt, cstring("txtvers"), 1, cstring("1")}) == 0, "TXTRecordSetValue txtvers");
    require(f.call(lib, "_TXTRecordSetValue", {txt, cstring("k"), 2, cstring("vv")}) == 0, "TXTRecordSetValue k");
    require(f.call(lib, "_TXTRecordSetValue", {txt, cstring("flag"), 0, 0}) == 0, "TXTRecordSetValue flag (no value)");
    require(f.call(lib, "_TXTRecordSetValue", {txt, cstring("K"), 3, cstring("new")}) == 0, "TXTRecordSetValue replaces a key case-insensitively");
    require(static_cast<std::int32_t>(f.call(lib, "_TXTRecordSetValue", {txt, cstring("a=b"), 0, 0})) == -65549, "a key with '=' is invalid");
    const auto length = f.call(lib, "_TXTRecordGetLength", {txt});
    const auto bytes = f.call(lib, "_TXTRecordGetBytesPtr", {txt});
    const Bytes expected_txt{9, 't', 'x', 't', 'v', 'e', 'r', 's', '=', '1', 4, 'f', 'l', 'a', 'g', 5, 'K', '=', 'n', 'e', 'w'};
    require(read(bytes, length) == expected_txt, "TXT record wire format");
    require(f.call(lib, "_TXTRecordContainsKey", {length, bytes, cstring("FLAG")}) == 1 &&
            f.call(lib, "_TXTRecordContainsKey", {length, bytes, cstring("absent")}) == 0, "TXTRecordContainsKey");
    const auto value_length = f.process.allocate_guest(8);
    const auto value = f.call(lib, "_TXTRecordGetValuePtr", {length, bytes, cstring("k"), value_length});
    require(value && guest.read8(value_length) == 3 && read(value, 3) == Bytes{'n', 'e', 'w'}, "TXTRecordGetValuePtr");
    require(f.call(lib, "_TXTRecordGetValuePtr", {length, bytes, cstring("flag"), value_length}) == 0, "a key without a value has no value pointer");
    require(f.call(lib, "_TXTRecordGetCount", {length, bytes}) == 3, "TXTRecordGetCount");
    const auto key = f.process.allocate_guest(256), value_pointer = f.process.allocate_guest(8);
    require(f.call(lib, "_TXTRecordGetItemAtIndex", {length, bytes, 2, 256, key, value_length, value_pointer}) == 0 && read_text(key) == "K" &&
            guest.read8(value_length) == 3, "TXTRecordGetItemAtIndex");
    require(static_cast<std::int32_t>(f.call(lib, "_TXTRecordGetItemAtIndex", {length, bytes, 3, 256, key, value_length, value_pointer})) == -65549,
            "TXTRecordGetItemAtIndex out of range");
    require(f.call(lib, "_TXTRecordRemoveValue", {txt, cstring("flag")}) == 0 && f.call(lib, "_TXTRecordGetLength", {txt}) == 16 &&
            static_cast<std::int32_t>(f.call(lib, "_TXTRecordRemoveValue", {txt, cstring("flag")})) == -65556, "TXTRecordRemoveValue");
    // A caller buffer that is too small reports kDNSServiceErr_NoMemory.
    const auto small = f.process.allocate_guest(16), small_buffer = f.process.allocate_guest(4);
    static_cast<void>(f.call(lib, "_TXTRecordCreate", {small, 4, small_buffer}));
    require(static_cast<std::int32_t>(f.call(lib, "_TXTRecordSetValue", {small, cstring("long"), 4, cstring("text")})) == -65539, "TXT buffer full");
    static_cast<void>(f.call(lib, "_TXTRecordDeallocate", {txt}));

    // ---- NSNetService TXT dictionaries -------------------------------------------------------------
    const auto service_class = f.cls("NSNetService"), browser_class = f.cls("NSNetServiceBrowser");
    {
        const auto dictionary = b->dictionary({{f.string("b"), b->data(Bytes{'2'})}, {f.string("a"), f.string("one")}});
        const auto data = f.send(service_class, "dataFromTXTRecordDictionary:", {dictionary});
        require(data && b->bytes(data) == Bytes{5, 'a', '=', 'o', 'n', 'e', 3, 'b', '=', '2'}, "+dataFromTXTRecordDictionary:");
        const auto back = f.send(service_class, "dictionaryFromTXTRecordData:", {data});
        require(f.core("_CFDictionaryGetCount", {back}) == 2 && b->bytes(f.core("_CFDictionaryGetValue", {back, f.string("a")})) == Bytes{'o', 'n', 'e'},
                "+dictionaryFromTXTRecordData:");
    }

    if (const auto status = net::mdns::available(); status != 0) {
        std::cout << "Windows DNS-SD client unavailable (" << status << "): Bonjour discovery skipped\n";
        return;
    }
    if (!sec::host_environment("ALCHEMY_TEST_NO_MDNS").empty()) {
        std::cout << "ALCHEMY_TEST_NO_MDNS set: Bonjour discovery skipped\n";
        return;
    }

    // ---- NSNetService / NSNetServiceBrowser through a guest-visible delegate ----------------------
    struct Seen {
        std::vector<std::string> log;
        Address found{};
        std::int64_t port{-1};
        std::size_t addresses{};
        Bytes txt;
        std::string host;
        bool has(std::string_view entry) const { return std::find(log.begin(), log.end(), entry) != log.end(); }
    };
    Seen seen;
    const auto delegate_class = f.objc.register_native_class("AlchemyTestNetServiceDelegate", f.cls("NSObject"), 8);
    const auto on = [&](std::string_view selector, transmute::GuestCallHandler handler) {
        static_cast<void>(f.objc.add_native_method(delegate_class, std::string(selector), std::move(handler), false, "v@:@@B"));
    };
    const auto name_of = [&](Address service) { return f.text(f.objc.send(service, "name").x0); };
    const auto code_of = [&](Address dictionary) {
        return std::to_string(static_cast<std::int64_t>(f.send(f.core("_CFDictionaryGetValue", {dictionary, f.string("NSNetServicesErrorCode")}), "integerValue")));
    };
    on("netServiceWillPublish:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("willpublish"); s.x[0] = 0; });
    on("netServiceDidPublish:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("publish:" + name_of(s.x[2])); s.x[0] = 0; });
    on("netService:didNotPublish:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("notpublish:" + code_of(s.x[3])); s.x[0] = 0; });
    on("netServiceWillResolve:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("willresolve"); s.x[0] = 0; });
    on("netServiceDidResolveAddress:", [&](CpuState& s, GuestMemory&) {
        seen.log.push_back("resolved");
        seen.port = static_cast<std::int64_t>(f.objc.send(s.x[2], "port").x0);
        seen.addresses = f.cf.array_values(f.objc.send(s.x[2], "addresses").x0).size();
        seen.txt = b->bytes(f.objc.send(s.x[2], "TXTRecordData").x0);
        seen.host = f.text(f.objc.send(s.x[2], "hostName").x0);
        s.x[0] = 0;
    });
    on("netService:didNotResolve:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("notresolve:" + code_of(s.x[3])); s.x[0] = 0; });
    on("netServiceDidStop:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("stop"); s.x[0] = 0; });
    on("netServiceBrowserWillSearch:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("willsearch"); s.x[0] = 0; });
    on("netServiceBrowserDidStopSearch:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("stopsearch"); s.x[0] = 0; });
    on("netServiceBrowser:didNotSearch:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("notsearch:" + code_of(s.x[3])); s.x[0] = 0; });
    on("netServiceBrowser:didFindDomain:moreComing:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("domain:" + f.text(s.x[3])); s.x[0] = 0; });
    on("netServiceBrowser:didFindService:moreComing:", [&](CpuState& s, GuestMemory&) {
        seen.log.push_back("found:" + name_of(s.x[3]));
        if (seen.log.back().find("Alchemy Test ") != std::string::npos && !seen.found) seen.found = f.objc.send(s.x[3], "retain").x0;
        s.x[0] = 0;
    });
    on("netServiceBrowser:didRemoveService:moreComing:", [&](CpuState& s, GuestMemory&) { seen.log.push_back("removed:" + name_of(s.x[3])); s.x[0] = 0; });
    const auto delegate = f.send(f.send(delegate_class, "alloc"), "init");

    const auto mode = f.engine.read64(f.symbol(core, "_kCFRunLoopDefaultMode"));
    const auto run_until = [&](auto done, std::string_view what, int seconds = 20) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!done()) {
            require(std::chrono::steady_clock::now() < until, "timed out: " + std::string(what));
            static_cast<void>(f.call_d0(core, "_CFRunLoopRunInMode", {mode, 1}, 0.1));
        }
    };

    // Domains and an invalid type.
    const auto browser = f.send(f.send(browser_class, "alloc"), "init");
    static_cast<void>(f.send(browser, "setDelegate:", {delegate}));
    static_cast<void>(f.send(browser, "searchForBrowsableDomains"));
    run_until([&] { return seen.has("domain:local."); }, "didFindDomain local.");
    static_cast<void>(f.send(browser, "stop"));
    run_until([&] { return seen.has("stopsearch"); }, "netServiceBrowserDidStopSearch:");
    static_cast<void>(f.send(browser, "searchForServicesOfType:inDomain:", {f.string("not a type"), f.string("local.")}));
    run_until([&] { return seen.has("notsearch:-72004"); }, "didNotSearch for an invalid type");

    // Publish a service with a TXT record.
    const auto name = "Alchemy Test " + std::to_string(GetCurrentProcessId());
    const std::string type = "_alchemytest._tcp.";
    const auto published = f.send(f.send(service_class, "alloc"), "initWithDomain:type:name:port:",
                                  {f.string("local."), f.string(type), f.string(name), 47123});
    static_cast<void>(f.send(published, "setDelegate:", {delegate}));
    require(f.send(published, "setTXTRecordData:", {b->data(Bytes{6, 'm', 'o', 'd', 'e', '=', 'x'})}) == 1, "setTXTRecordData:");
    seen.log.clear();
    static_cast<void>(f.send(published, "publish"));
    require(seen.has("willpublish"), "netServiceWillPublish: is sent by -publish");
    run_until([&] { return seen.has("publish:" + name); }, "netServiceDidPublish:");

    // NSNetServiceBrowser and dns_sd's DNSServiceBrowse (shared connection) both find it.
    std::vector<std::string> dns_added, dns_removed;
    const auto browse_reply = f.process.install_function("test_dnssd_browse", [&](CpuState& s, GuestMemory&) {
        // (sdRef, flags, interfaceIndex, errorCode, serviceName, regtype, replyDomain, context)
        require(s.x[3] == 0, "DNSServiceBrowseReply error");
        require(read_text(s.x[5]) == type && read_text(s.x[6]) == "local.", "DNSServiceBrowseReply type/domain");
        ((s.x[1] & 2) ? dns_added : dns_removed).push_back(read_text(s.x[4]));
        s.x[0] = 0;
    }, "alchemy.test");
    std::vector<int> families;
    std::int64_t address_error{1};
    const auto address_reply = f.process.install_function("test_dnssd_address", [&](CpuState& s, GuestMemory& m) {
        // (sdRef, flags, interfaceIndex, errorCode, hostname, address, ttl, context)
        address_error = static_cast<std::int32_t>(s.x[3]);
        if (s.x[3] == 0 && s.x[5]) families.push_back(m.read8(s.x[5] + 1));
        s.x[0] = 0;
    }, "alchemy.test");
    const auto connection_cell = f.process.allocate_guest(8), browse_cell = f.process.allocate_guest(8), address_cell = f.process.allocate_guest(8);
    require(f.call(lib, "_DNSServiceCreateConnection", {connection_cell}) == 0, "DNSServiceCreateConnection");
    const auto connection = f.engine.read64(connection_cell);
    const auto fd = static_cast<std::int32_t>(f.call(lib, "_DNSServiceRefSockFD", {connection}));
    require(fd >= 0, "DNSServiceRefSockFD");
    f.engine.write64(browse_cell, connection);
    require(f.call(lib, "_DNSServiceBrowse", {browse_cell, 0x4000, 0, cstring("_alchemytest._tcp"), 0, browse_reply, 0}) == 0 &&
            f.engine.read64(browse_cell) != connection, "DNSServiceBrowse (kDNSServiceFlagsShareConnection)");
    require(static_cast<std::int32_t>(f.call(lib, "_DNSServiceBrowse", {address_cell, 0, 0, cstring("_alchemytest._tcp"), cstring("example.com."), browse_reply, 0})) == -65544,
            "a wide-area browse domain is unsupported");
    require(static_cast<std::int32_t>(f.call(lib, "_DNSServiceProcessResult", {f.engine.read64(browse_cell)})) == -65541,
            "DNSServiceProcessResult needs the primary ref");
    const auto pollfd = f.process.allocate_guest(8);
    const auto pump_dns = [&](auto done, std::string_view what) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!done()) {
            require(std::chrono::steady_clock::now() < until, "timed out: " + std::string(what));
            f.engine.write32(pollfd, static_cast<std::uint32_t>(fd));
            f.engine.write32(pollfd + 4, 1);   // POLLIN
            if (static_cast<std::int32_t>(f.call(lib, "_poll", {pollfd, 1, 100})) == 1)
                require(f.call(lib, "_DNSServiceProcessResult", {connection}) == 0, "DNSServiceProcessResult");
            static_cast<void>(f.call_d0(core, "_CFRunLoopRunInMode", {mode, 1}, 0.0));
        }
    };
    pump_dns([&] { return std::find(dns_added.begin(), dns_added.end(), name) != dns_added.end(); }, "DNSServiceBrowse finds the published service");

    static_cast<void>(f.send(browser, "searchForServicesOfType:inDomain:", {f.string(type), f.string("")}));
    run_until([&] { return seen.found != 0; }, "netServiceBrowser:didFindService: for the published service");
    require(seen.has("willsearch"), "netServiceBrowserWillSearch:");
    require(f.text(f.objc.send(seen.found, "type").x0) == type && f.text(f.objc.send(seen.found, "domain").x0) == "local." &&
            static_cast<std::int64_t>(f.objc.send(seen.found, "port").x0) == -1, "a found service has its type and domain, no port");
    require(f.send(seen.found, "isEqual:", {published}) == 1, "the found service equals the published one");

    // Resolve the found service.
    static_cast<void>(f.send(seen.found, "setDelegate:", {delegate}));
    {
        alchemy::CallRegs registers;
        registers.set_d(0, 10.0);
        static_cast<void>(f.objc.send(seen.found, "resolveWithTimeout:", registers));
    }
    require(seen.has("willresolve"), "netServiceWillResolve:");
    run_until([&] { return seen.has("resolved") || seen.has("notresolve:-72007") || seen.has("notresolve:-72002"); }, "resolve");
    require(seen.has("resolved"), "netServiceDidResolveAddress:");
    require(seen.port == 47123 && seen.addresses >= 1 && !seen.host.empty() && seen.host.back() == '.', "resolved port, addresses and host " + seen.host);
    require(seen.txt == Bytes{6, 'm', 'o', 'd', 'e', '=', 'x'}, "resolved TXT record");

    // DNSServiceGetAddrInfo for the resolved host.
    f.engine.write64(address_cell, connection);
    require(f.call(lib, "_DNSServiceGetAddrInfo", {address_cell, 0x4000, 0, 0, cstring(seen.host), address_reply, 0}) == 0, "DNSServiceGetAddrInfo");
    pump_dns([&] { return !families.empty() || address_error != 1; }, "DNSServiceGetAddrInfoReply");
    require(!families.empty() && std::all_of(families.begin(), families.end(), [](int family) { return family == 2 || family == 30; }),
            "GetAddrInfo answers with Darwin sockaddrs");
    static_cast<void>(f.call(lib, "_DNSServiceRefDeallocate", {f.engine.read64(address_cell)}));

    // dns_sd registration and resolve (stack-passed arguments, as compiled code passes them).
    const auto dns_name = "Alchemy DNS-SD " + std::to_string(GetCurrentProcessId());
    std::string registered;
    std::int64_t register_error{1};
    const auto register_reply = f.process.install_function("test_dnssd_register", [&](CpuState& s, GuestMemory&) {
        // (sdRef, flags, errorCode, name, regtype, domain, context)
        register_error = static_cast<std::int32_t>(s.x[2]);
        if (s.x[2] == 0 && (s.x[1] & 2) && s.x[6] == 0x5eed) registered = read_text(s.x[3]);
        s.x[0] = 0;
    }, "alchemy.test");
    struct Resolved { bool done{}; std::int64_t error{}; std::string full, host; std::uint16_t port{}; Bytes txt; Address context{}; };
    Resolved resolved;
    const auto resolve_reply = f.process.install_function("test_dnssd_resolve", [&](CpuState& s, GuestMemory& m) {
        // (sdRef, flags, interfaceIndex, errorCode, fullname, hosttarget, port, txtLen, [sp] txtRecord, [sp+8] context)
        resolved.done = true;
        resolved.error = static_cast<std::int32_t>(s.x[3]);
        resolved.full = read_text(s.x[4]);
        resolved.host = read_text(s.x[5]);
        resolved.port = static_cast<std::uint16_t>(((s.x[6] & 0xff) << 8) | ((s.x[6] >> 8) & 0xff));
        resolved.txt = read(m.read64(s.sp), static_cast<std::uint16_t>(s.x[7]));
        resolved.context = m.read64(s.sp + 8);
        s.x[0] = 0;
    }, "alchemy.test");
    const auto register_cell = f.process.allocate_guest(8), resolve_cell = f.process.allocate_guest(8);
    const Bytes dns_txt{7, 'k', 'i', 'n', 'd', '=', 'd', 'n'};
    const auto dns_txt_bytes = f.process.allocate_guest(dns_txt.size());
    f.engine.write_bytes(dns_txt_bytes, dns_txt);
    {
        // DNSServiceRegister(sdRef*, flags, interfaceIndex, name, regtype, domain, host, port (network order),
        //                    [sp] txtLen (uint16), [sp+8] txtRecord, [sp+16] callBack, [sp+24] context)
        f.engine.write64(register_cell, connection);
        alchemy::CallRegs registers;
        registers.x = {register_cell, 0x4000, 0, cstring(dns_name), cstring("_alchemytest._tcp"), 0, 0, (47124 >> 8) | ((47124 & 0xff) << 8)};
        registers.stack.resize(32);
        const std::uint16_t txt_length = static_cast<std::uint16_t>(dns_txt.size());
        const Address callback_context = 0x5eed;
        std::memcpy(registers.stack.data(), &txt_length, 2);
        std::memcpy(registers.stack.data() + 8, &dns_txt_bytes, 8);
        std::memcpy(registers.stack.data() + 16, &register_reply, 8);
        std::memcpy(registers.stack.data() + 24, &callback_context, 8);
        require(f.engine.invoke_callback_regs(f.symbol(lib, "_DNSServiceRegister"), registers).x0 == 0, "DNSServiceRegister");
    }
    pump_dns([&] { return !registered.empty() || register_error != 1 && register_error != 0; }, "DNSServiceRegisterReply");
    require(registered == dns_name, "DNSServiceRegisterReply name: " + registered);
    pump_dns([&] { return std::find(dns_added.begin(), dns_added.end(), dns_name) != dns_added.end(); }, "browse finds the dns_sd registration");
    f.engine.write64(resolve_cell, connection);
    require(f.call(lib, "_DNSServiceResolve", {resolve_cell, 0x4000, 0, cstring(dns_name), cstring("_alchemytest._tcp."), cstring("local."),
                                               resolve_reply, 0xc0de}) == 0, "DNSServiceResolve");
    pump_dns([&] { return resolved.done; }, "DNSServiceResolveReply");
    require(resolved.error == 0 && resolved.port == 47124 && resolved.txt == dns_txt && resolved.context == 0xc0de &&
            resolved.full == "Alchemy DNS-SD " + std::to_string(GetCurrentProcessId()) + "._alchemytest._tcp.local." && !resolved.host.empty(),
            "DNSServiceResolveReply: " + resolved.full + " " + resolved.host + ":" + std::to_string(resolved.port));
    static_cast<void>(f.call(lib, "_DNSServiceRefDeallocate", {f.engine.read64(resolve_cell)}));
    static_cast<void>(f.call(lib, "_DNSServiceRefDeallocate", {f.engine.read64(register_cell)}));
    pump_dns([&] { return std::find(dns_removed.begin(), dns_removed.end(), dns_name) != dns_removed.end(); }, "dns_sd registration withdrawn");

    // Withdraw: both browsers report the removal; the publisher reports the stop.
    static_cast<void>(f.send(published, "stop"));
    run_until([&] { return seen.has("stop"); }, "netServiceDidStop:");
    run_until([&] { return seen.has("removed:" + name); }, "netServiceBrowser:didRemoveService:");
    pump_dns([&] { return std::find(dns_removed.begin(), dns_removed.end(), name) != dns_removed.end(); }, "DNSServiceBrowse removal");
    static_cast<void>(f.call(lib, "_DNSServiceRefDeallocate", {connection}));   // ends the subordinate browse too
    require(static_cast<std::int32_t>(f.call(lib, "_DNSServiceProcessResult", {connection})) == -65541, "a deallocated ref is invalid");

    static_cast<void>(f.send(browser, "stop"));
    for (const auto object : {seen.found, published, browser, delegate}) b->release(object);
    std::cout << "Bonjour: published, browsed (NSNetServiceBrowser and dns_sd), resolved and withdrew " << name << '\n';
}

int main() {
    try {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        Fixture fixture;
        test_urls(fixture);
        test_operation_queue(fixture);
        test_http_message(fixture);
        test_unsent_exchange();
        test_sockets(fixture);
        test_proxy_auto_configuration(fixture);
        test_system_configuration(fixture);
        test_resolver(fixture, sec::host_environment("ALCHEMY_TEST_OFFLINE").empty());
        test_bonjour(fixture);
        Harness harness(fixture);   // registers the delegate class once
        const auto identity = sec::SelfSignedIdentity::create("localhost", {"localhost"});
        {
            HttpsServer server(identity->context());
            test_sessions(fixture, harness, server);
        }
        if (sec::host_environment("ALCHEMY_TEST_OFFLINE").empty()) test_internet(fixture, harness);
        std::cout << "Alchemy network tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n';
        return 1;
    }
}
