// Linux host backend, area "http": the system HTTP stack over libcurl (with OpenSSL), the proxy
// settings from the environment.
//
// - A Session shares a connection cache and DNS cache (CURLSH) between its requests; HTTP/2 over
//   TLS when the server offers it; responses are decompressed. TLS sessions are not resumed, so a
//   new connection always carries the server's chain.
// - A Request drives one libcurl easy handle on its own multi handle: send() runs the transfer
//   until the response head is complete, read() until more body bytes arrive, and cancel() wakes
//   whichever is waiting. timeout_ms bounds connecting and each wait for the server.
// - Server certificates: libcurl checks neither the chain nor the host name. The Trust callback
//   runs for every request once its connection is ready (new or reused) and before any request
//   byte is written (CURLOPT_PREREQFUNCTION), with the chain of that connection's TLS session
//   (CURLINFO_TLS_SSL_PTR); a refusal ends the request there.
// - Redirects are not followed, there is no cookie engine, and credentials go out only after
//   set_credentials.
// - user_proxy() reports the environment's proxies (http_proxy, https_proxy, all_proxy, no_proxy):
//   the settings Linux programs share. auto_proxy() is Error::unsupported: no PAC interpreter.
#include "alchemy/host/http.hpp"

#include <curl/curl.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>

namespace alchemy::host::http {
namespace {

void global_init() {
    static const bool ready = [] { return curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK; }();
    static_cast<void>(ready);
}

Error error_of(const Failure& failure) {
    switch (failure.kind) {
    case Failure::Kind::none: return Error::none;
    case Failure::Kind::cancelled: return Error::interrupted;
    case Failure::Kind::trust_rejected: return Error::access_denied;
    case Failure::Kind::timed_out: return Error::timed_out;
    case Failure::Kind::name_not_resolved: return Error::host_unreachable;
    case Failure::Kind::cannot_connect: return Error::connection_refused;
    case Failure::Kind::connection_lost: return Error::connection_reset;
    case Failure::Kind::secure_channel: return Error::protocol_not_supported;
    case Failure::Kind::client_certificate_required: return Error::permission;
    case Failure::Kind::bad_response: return Error::io;
    case Failure::Kind::bad_url: return Error::invalid_argument;
    case Failure::Kind::unsupported_scheme: return Error::protocol_not_supported;
    case Failure::Kind::other: return Error::other;
    }
    return Error::other;
}

Failure failure_of(CURLcode code) {
    Failure failure;
    switch (code) {
    case CURLE_OK: break;
    case CURLE_COULDNT_RESOLVE_HOST: case CURLE_COULDNT_RESOLVE_PROXY: failure.kind = Failure::Kind::name_not_resolved; break;
    case CURLE_COULDNT_CONNECT: failure.kind = Failure::Kind::cannot_connect; break;
    case CURLE_OPERATION_TIMEDOUT: failure.kind = Failure::Kind::timed_out; break;
    case CURLE_SSL_CONNECT_ERROR: case CURLE_PEER_FAILED_VERIFICATION: case CURLE_SSL_CIPHER: case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_ENGINE_NOTFOUND: case CURLE_SSL_ENGINE_SETFAILED: case CURLE_SSL_ISSUER_ERROR: case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
        failure.kind = Failure::Kind::secure_channel; break;
    case CURLE_SSL_CLIENTCERT: failure.kind = Failure::Kind::client_certificate_required; break;
    case CURLE_SEND_ERROR: case CURLE_RECV_ERROR: case CURLE_GOT_NOTHING: case CURLE_PARTIAL_FILE: case CURLE_HTTP2: case CURLE_HTTP2_STREAM:
        failure.kind = Failure::Kind::connection_lost; break;
    case CURLE_WEIRD_SERVER_REPLY: case CURLE_BAD_CONTENT_ENCODING: failure.kind = Failure::Kind::bad_response; break;
    case CURLE_URL_MALFORMAT: failure.kind = Failure::Kind::bad_url; break;
    case CURLE_UNSUPPORTED_PROTOCOL: failure.kind = Failure::Kind::unsupported_scheme; break;
    case CURLE_ABORTED_BY_CALLBACK: failure.kind = Failure::Kind::cancelled; break;
    default:
        failure.kind = Failure::Kind::other;
        failure.detail = std::string("libcurl: ") + curl_easy_strerror(code);
        break;
    }
    return failure;
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return text;
}
std::string trimmed(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

// One proxy-list entry for `scheme` ("[scheme=]host[:port]"; an entry without a scheme serves every scheme).
std::string proxy_for(std::string_view list, std::string_view scheme) {
    std::string fallback;
    std::size_t at = 0;
    while (at < list.size()) {
        const auto end = std::min(list.find_first_of("; \t", at), list.size());
        const auto entry = trimmed(list.substr(at, end - at));
        at = end + 1;
        if (entry.empty()) continue;
        const auto equals = entry.find('=');
        if (equals == std::string::npos) { if (fallback.empty()) fallback = entry; continue; }
        if (lowered(entry.substr(0, equals)) == scheme) return entry.substr(equals + 1);
    }
    return fallback;
}

// The environment's proxy URL for one variable pair, as "host:port".
std::string environment_proxy(const char* lower, const char* upper) {
    const char* value = std::getenv(lower);
    if (!value || !*value) value = std::getenv(upper);
    if (!value || !*value) return {};
    std::string text = value;
    if (const auto scheme = text.find("://"); scheme != std::string::npos) text.erase(0, scheme + 3);
    if (const auto credentials = text.rfind('@'); credentials != std::string::npos) text.erase(0, credentials + 1);
    while (!text.empty() && text.back() == '/') text.pop_back();
    return text;
}

// A session's shared caches, locked per data kind (libcurl's share interface).
struct SessionState {
    std::string agent;
    Proxy proxy;
    CURLSH* share{};
    std::array<std::mutex, CURL_LOCK_DATA_LAST> locks;

    static void lock(CURL*, curl_lock_data data, curl_lock_access, void* self) {
        static_cast<SessionState*>(self)->locks[static_cast<std::size_t>(data)].lock();
    }
    static void unlock(CURL*, curl_lock_data data, void* self) {
        static_cast<SessionState*>(self)->locks[static_cast<std::size_t>(data)].unlock();
    }
    ~SessionState() { if (share) curl_share_cleanup(share); }
};

} // namespace

// ---- Proxies ----

Result<UserProxy> user_proxy() {
    UserProxy result;
    const auto http = environment_proxy("http_proxy", "HTTP_PROXY"), https = environment_proxy("https_proxy", "HTTPS_PROXY");
    const auto all = environment_proxy("all_proxy", "ALL_PROXY");
    if (!http.empty()) result.proxy += "http=" + http;
    if (!https.empty()) result.proxy += (result.proxy.empty() ? "" : ";") + std::string("https=") + https;
    if (!all.empty()) result.proxy += (result.proxy.empty() ? "" : ";") + all;
    const char* bypass = std::getenv("no_proxy");
    if (!bypass || !*bypass) bypass = std::getenv("NO_PROXY");
    if (bypass) {
        result.bypass = bypass;
        std::replace(result.bypass.begin(), result.bypass.end(), ',', ';');
    }
    return result;
}

// No proxy auto-configuration interpreter on this host.
Result<std::string> auto_proxy(std::string_view, std::string_view, std::uint32_t) { return Error::unsupported; }

// ---- Sessions ----

Result<Session> Session::open(std::string_view user_agent, const Proxy& proxy) {
    global_init();
    auto state = std::make_unique<SessionState>();
    state->agent = user_agent;
    state->proxy = proxy;
    state->share = curl_share_init();
    if (!state->share) return Error::no_memory;
    curl_share_setopt(state->share, CURLSHOPT_LOCKFUNC, &SessionState::lock);
    curl_share_setopt(state->share, CURLSHOPT_UNLOCKFUNC, &SessionState::unlock);
    curl_share_setopt(state->share, CURLSHOPT_USERDATA, state.get());
    for (const auto data : {CURL_LOCK_DATA_DNS, CURL_LOCK_DATA_CONNECT})
        curl_share_setopt(state->share, CURLSHOPT_SHARE, data);
    return Session(state.release());
}

Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        delete static_cast<SessionState*>(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Session::~Session() { delete static_cast<SessionState*>(handle_); }

// ---- Requests ----

struct Request::State {
    CURL* easy{};
    CURLM* multi{};
    curl_slist* headers{};
    std::uint32_t timeout_ms{};
    bool secure{}, attached{}, done{}, head_done{};
    CURLcode result{CURLE_OK};
    std::atomic<bool> cancelled{};
    std::atomic<bool> trust_rejected{};
    bool timed_out{};
    Trust trust;
    std::exception_ptr callback_error;
    Failure failure;
    Head head, received;           // the head send() returned; the one being received
    std::vector<std::uint8_t> body; // response bytes not read yet
    std::mutex wake;                // guards `multi` against cancel()'s wakeup during cleanup

    ~State() {
        if (multi && easy && attached) curl_multi_remove_handle(multi, easy);
        if (easy) curl_easy_cleanup(easy);
        {
            std::lock_guard lock(wake);
            if (multi) curl_multi_cleanup(multi);
            multi = nullptr;
        }
        if (headers) curl_slist_free_all(headers);
    }

    template <class T>
    Result<T> fail(Failure why) {
        if (trust_rejected) why = {Failure::Kind::trust_rejected, {}};
        else if (cancelled) why = {Failure::Kind::cancelled, {}};
        else if (timed_out) why = {Failure::Kind::timed_out, {}};
        failure = std::move(why);
        return error_of(failure);
    }

    // libcurl callbacks.
    static std::size_t on_body(char* data, std::size_t size, std::size_t count, void* self) {
        auto& state = *static_cast<State*>(self);
        state.body.insert(state.body.end(), data, data + size * count);
        return size * count;
    }
    static std::size_t on_header(char* data, std::size_t size, std::size_t count, void* self) {
        auto& state = *static_cast<State*>(self);
        const std::string_view line(data, size * count);
        const auto text = trimmed(line);
        if (text.rfind("HTTP/", 0) == 0) {   // a status line: a new response (after 1xx or authentication)
            state.received = {};
            const auto space = text.find(' ');
            state.received.version = text.substr(0, space);
            if (state.received.version == "HTTP/2.0") state.received.version = "HTTP/2";
            if (space != std::string::npos) state.received.status = std::atoi(text.c_str() + space + 1);
        } else if (text.empty()) {   // the end of a head
            if (state.received.status >= 200) {
                state.head = state.received;
                state.head_done = true;
            }
        } else if (const auto colon = text.find(':'); colon != std::string::npos) {
            state.received.headers.emplace_back(text.substr(0, colon), trimmed(std::string_view(text).substr(colon + 1)));
        }
        return size * count;
    }
    static int on_progress(void* self, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        return static_cast<State*>(self)->cancelled ? 1 : 0;
    }
    // The connection is ready and nothing is sent yet: for https, the server's chain (leaf first) from
    // the connection's TLS session to the Trust callback; a refusal aborts the request.
    static int on_ready(void* self, char*, char*, int, int) {
        auto& state = *static_cast<State*>(self);
        if (!state.secure) return CURL_PREREQFUNC_OK;
        std::vector<std::vector<std::uint8_t>> chain;
        curl_tlssessioninfo* session = nullptr;
        if (curl_easy_getinfo(state.easy, CURLINFO_TLS_SSL_PTR, &session) == CURLE_OK && session && session->backend == CURLSSLBACKEND_OPENSSL &&
            session->internals) {
            const auto ssl = static_cast<SSL*>(session->internals);
            const auto add = [&](X509* certificate) {
                unsigned char* der = nullptr;
                const auto size = i2d_X509(certificate, &der);
                if (size <= 0) return;
                chain.emplace_back(der, der + size);
                OPENSSL_free(der);
            };
            const auto leaf = SSL_get0_peer_certificate(ssl);
            if (leaf) add(leaf);
            if (const auto others = SSL_get_peer_cert_chain(ssl))
                for (int index = 0; index < sk_X509_num(others); ++index) {
                    const auto certificate = sk_X509_value(others, index);
                    if (!leaf || X509_cmp(certificate, leaf) != 0) add(certificate);
                }
        }
        bool accepted = false;
        try {
            accepted = !chain.empty() && state.trust && state.trust(chain);
        } catch (...) {
            state.callback_error = std::current_exception();
        }
        if (accepted) return CURL_PREREQFUNC_OK;
        state.trust_rejected = true;
        return CURL_PREREQFUNC_ABORT;
    }

    // Runs the transfer until `ready` holds or it ends; false when it ended with an error, was
    // cancelled, or the server stayed silent for timeout_ms.
    template <class Ready>
    bool pump(Ready ready) {
        auto quiet_since = std::chrono::steady_clock::now();
        std::size_t seen = body.size();
        bool seen_head = head_done;
        while (!ready() && !done) {
            if (cancelled) return false;
            int running = 0;
            if (curl_multi_perform(multi, &running) != CURLM_OK) return false;
            for (int left = 0; const auto message = curl_multi_info_read(multi, &left);)
                if (message->msg == CURLMSG_DONE && message->easy_handle == easy) {
                    done = true;
                    result = message->data.result;
                }
            if (ready() || done) break;
            if (body.size() != seen || head_done != seen_head) {
                seen = body.size();
                seen_head = head_done;
                quiet_since = std::chrono::steady_clock::now();
            }
            const auto quiet = std::chrono::steady_clock::now() - quiet_since;
            if (timeout_ms && quiet > std::chrono::milliseconds(timeout_ms)) {
                timed_out = true;
                return false;
            }
            curl_multi_poll(multi, nullptr, 0, 100, nullptr);
        }
        return !done || result == CURLE_OK || ready();
    }
};

Request::Request(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&&) noexcept = default;
Request::~Request() = default;

Request Request::open(const Session& session, const Target& target, const Headers& headers, std::uint32_t timeout_ms, Trust trust) {
    global_init();
    auto state = std::make_unique<State>();
    state->secure = target.secure;
    state->trust = std::move(trust);
    state->timeout_ms = timeout_ms;
    const auto* shared = static_cast<const SessionState*>(session.handle_);
    if (!shared) {
        state->failure = {Failure::Kind::other, "no HTTP session"};
        return Request(std::move(state));
    }
    if (target.host.empty() || target.host.find_first_of("/?# \t\r\n") != std::string::npos) {
        state->failure = {Failure::Kind::bad_url, {}};
        return Request(std::move(state));
    }
    state->easy = curl_easy_init();
    state->multi = curl_multi_init();
    if (!state->easy || !state->multi) {
        state->failure = {Failure::Kind::other, "libcurl could not start a transfer"};
        return Request(std::move(state));
    }
    const auto easy = state->easy;
    const bool literal6 = target.host.find(':') != std::string::npos;
    std::string url = std::string(target.secure ? "https" : "http") + "://" + (literal6 ? "[" + target.host + "]" : target.host);
    if (target.port) url += ":" + std::to_string(target.port);
    url += target.path.empty() || target.path[0] != '/' ? "/" + target.path : target.path;
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_SHARE, shared->share);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, shared->agent.c_str());
    curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");   // every encoding libcurl decodes
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_2TLS));
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    if (target.method == "HEAD") curl_easy_setopt(easy, CURLOPT_NOBODY, 1L);
    else if (target.method != "GET") curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, target.method.c_str());
    // The Trust callback judges the chain (State::on_ready): libcurl checks neither it nor the host name,
    // and resumes no TLS session, so every new connection carries its chain.
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(easy, CURLOPT_SSL_SESSIONID_CACHE, 0L);
    curl_easy_setopt(easy, CURLOPT_PREREQFUNCTION, &State::on_ready);
    curl_easy_setopt(easy, CURLOPT_PREREQDATA, state.get());
    if (timeout_ms) curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &State::on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, state.get());
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &State::on_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, state.get());
    curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &State::on_progress);
    curl_easy_setopt(easy, CURLOPT_XFERINFODATA, state.get());
    // Proxies: none, the session's named list, or the environment's (automatic).
    switch (shared->proxy.kind) {
    case Proxy::Kind::none: curl_easy_setopt(easy, CURLOPT_PROXY, ""); break;
    case Proxy::Kind::named: {
        const auto proxy = proxy_for(shared->proxy.list, target.secure ? "https" : "http");
        curl_easy_setopt(easy, CURLOPT_PROXY, proxy.c_str());
        auto bypass = shared->proxy.bypass;
        std::replace(bypass.begin(), bypass.end(), ';', ',');
        if (!bypass.empty()) curl_easy_setopt(easy, CURLOPT_NOPROXY, bypass.c_str());
        break;
    }
    case Proxy::Kind::automatic: break;   // libcurl reads the environment's proxies
    }
    bool expect = false;
    for (const auto& [name, value] : headers) {
        if (lowered(name) == "expect") expect = true;
        state->headers = curl_slist_append(state->headers, (name + ": " + value).c_str());
    }
    if (!expect) state->headers = curl_slist_append(state->headers, "Expect:");   // no 100-continue round trip
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, state->headers);
    return Request(std::move(state));
}

Result<Head> Request::send(ConstBytes body) {
    auto& state = *state_;
    if (state.cancelled) return state.fail<Head>({Failure::Kind::cancelled, {}});
    if (!state.easy) return state.fail<Head>(state.failure);   // opening failed
    if (state.attached) {   // again, after set_credentials
        curl_multi_remove_handle(state.multi, state.easy);
        state.attached = false;
    }
    state.done = state.head_done = state.timed_out = false;
    state.result = CURLE_OK;
    state.head = state.received = {};
    state.body.clear();
    if (!body.empty()) {
        curl_easy_setopt(state.easy, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
        curl_easy_setopt(state.easy, CURLOPT_COPYPOSTFIELDS, reinterpret_cast<const char*>(body.data()));
    }
    if (curl_multi_add_handle(state.multi, state.easy) != CURLM_OK) return state.fail<Head>({Failure::Kind::other, "libcurl refused the transfer"});
    state.attached = true;
    const bool ok = state.pump([&] { return state.head_done; });
    if (state.callback_error) std::rethrow_exception(std::exchange(state.callback_error, nullptr));
    if (!ok || !state.head_done) return state.fail<Head>(state.done && state.result != CURLE_OK ? failure_of(state.result) : Failure{Failure::Kind::bad_response, {}});
    state.failure = {};
    return state.head;
}

Result<std::size_t> Request::read(Bytes buffer) {
    auto& state = *state_;
    if (state.cancelled) return state.fail<std::size_t>({Failure::Kind::cancelled, {}});
    if (!state.attached) return state.fail<std::size_t>({Failure::Kind::other, "the request was not sent"});
    if (state.body.empty() && !state.done) {
        if (!state.pump([&] { return !state.body.empty(); }) && state.body.empty())
            return state.fail<std::size_t>(state.done ? failure_of(state.result) : Failure{Failure::Kind::other, {}});
    }
    if (state.body.empty()) {
        if (state.result != CURLE_OK) return state.fail<std::size_t>(failure_of(state.result));
        return std::size_t{0};
    }
    const auto count = std::min(buffer.size(), state.body.size());
    std::memcpy(buffer.data(), state.body.data(), count);
    state.body.erase(state.body.begin(), state.body.begin() + static_cast<std::ptrdiff_t>(count));
    return count;
}

// The schemes and realm of the last response's WWW-Authenticate (401) or Proxy-Authenticate (407) headers.
Result<Challenge> Request::challenge(int status) {
    const auto& state = *state_;
    if (!state.easy || !state.head_done) return Error::bad_handle;
    Challenge result;
    result.proxy = status == 407;
    const auto wanted = result.proxy ? "proxy-authenticate" : "www-authenticate";
    for (const auto& [name, value] : state.head.headers) {
        if (lowered(name) != wanted) continue;
        const auto scheme = lowered(value.substr(0, value.find(' ')));
        Scheme kind;
        if (scheme == "basic") kind = Scheme::basic;
        else if (scheme == "digest") kind = Scheme::digest;
        else if (scheme == "ntlm") kind = Scheme::ntlm;
        else if (scheme == "negotiate") kind = Scheme::negotiate;
        else continue;
        if (std::find(result.schemes.begin(), result.schemes.end(), kind) == result.schemes.end()) result.schemes.push_back(kind);
        const auto lower = lowered(value);
        if (const auto at = lower.find("realm=\""); at != std::string::npos && result.realm.empty()) {
            const auto end = value.find('"', at + 7);
            result.realm = value.substr(at + 7, end == std::string::npos ? std::string::npos : end - at - 7);
        }
    }
    return result;
}

Status Request::set_credentials(bool proxy, Scheme scheme, std::string_view user, std::string_view password) {
    auto& state = *state_;
    if (!state.easy) return Error::bad_handle;
    const auto features = curl_version_info(CURLVERSION_NOW)->features;
    unsigned long method = CURLAUTH_BASIC;
    switch (scheme) {
    case Scheme::basic: method = CURLAUTH_BASIC; break;
    case Scheme::digest: method = CURLAUTH_DIGEST; break;
    case Scheme::ntlm:
        if (!(features & CURL_VERSION_NTLM)) return Error::unsupported;
        method = CURLAUTH_NTLM;
        break;
    case Scheme::negotiate:
        if (!(features & CURL_VERSION_SPNEGO)) return Error::unsupported;
        method = CURLAUTH_NEGOTIATE;
        break;
    }
    const std::string name(user), secret(password);
    curl_easy_setopt(state.easy, proxy ? CURLOPT_PROXYAUTH : CURLOPT_HTTPAUTH, method);
    curl_easy_setopt(state.easy, proxy ? CURLOPT_PROXYUSERNAME : CURLOPT_USERNAME, name.c_str());
    curl_easy_setopt(state.easy, proxy ? CURLOPT_PROXYPASSWORD : CURLOPT_PASSWORD, secret.c_str());
    return {};
}

void Request::cancel() noexcept {
    state_->cancelled = true;
    std::lock_guard lock(state_->wake);
    if (state_->multi) curl_multi_wakeup(state_->multi);
}

Failure Request::failure() const { return state_->failure; }

} // namespace alchemy::host::http
