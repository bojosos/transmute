#include "alchemy/host/http.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <winhttp.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <mutex>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace alchemy::host::http {
namespace {

std::string utf8(const wchar_t* text) { return text ? win32::narrow(text) : std::string{}; }

DWORD scheme_flag(Scheme scheme) {
    switch (scheme) {
    case Scheme::basic: return WINHTTP_AUTH_SCHEME_BASIC;
    case Scheme::digest: return WINHTTP_AUTH_SCHEME_DIGEST;
    case Scheme::ntlm: return WINHTTP_AUTH_SCHEME_NTLM;
    case Scheme::negotiate: return WINHTTP_AUTH_SCHEME_NEGOTIATE;
    }
    return 0;
}

Failure failure_of(DWORD error) {
    Failure failure;
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT: failure.kind = Failure::Kind::timed_out; break;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: failure.kind = Failure::Kind::name_not_resolved; break;
    case ERROR_WINHTTP_CANNOT_CONNECT: failure.kind = Failure::Kind::cannot_connect; break;
    case ERROR_WINHTTP_CONNECTION_ERROR: failure.kind = Failure::Kind::connection_lost; break;
    case ERROR_WINHTTP_SECURE_FAILURE: case ERROR_WINHTTP_SECURE_CHANNEL_ERROR: case ERROR_WINHTTP_SECURE_INVALID_CERT:
        failure.kind = Failure::Kind::secure_channel; break;
    case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: failure.kind = Failure::Kind::client_certificate_required; break;
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: case ERROR_WINHTTP_HEADER_NOT_FOUND: failure.kind = Failure::Kind::bad_response; break;
    case ERROR_WINHTTP_INVALID_URL: failure.kind = Failure::Kind::bad_url; break;
    case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: failure.kind = Failure::Kind::unsupported_scheme; break;
    case ERROR_WINHTTP_OPERATION_CANCELLED: failure.kind = Failure::Kind::cancelled; break;
    default:
        failure.kind = Failure::Kind::other;
        failure.detail = "WinHTTP error " + std::to_string(error);
        break;
    }
    return failure;
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

} // namespace

// ---- Proxies ----

Result<UserProxy> user_proxy() {
    using Query = BOOL(WINAPI*)(WINHTTP_CURRENT_USER_IE_PROXY_CONFIG*);
    // winhttp.dll from System32 only, loaded the first time a guest asks.
    static const Query query = [] {
        const auto module = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return Query{};
        return reinterpret_cast<Query>(GetProcAddress(module, "WinHttpGetIEProxyConfigForCurrentUser"));
    }();
    if (!query) return Error::unsupported;
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG config{};
    if (!query(&config)) return win32::last_error();
    UserProxy result;
    result.proxy = utf8(config.lpszProxy);
    result.bypass = utf8(config.lpszProxyBypass);
    result.auto_config_url = utf8(config.lpszAutoConfigUrl);
    result.auto_discovery = config.fAutoDetect != FALSE;
    for (const auto text : {config.lpszAutoConfigUrl, config.lpszProxy, config.lpszProxyBypass})
        if (text) GlobalFree(text);
    return result;
}

Result<std::string> auto_proxy(std::string_view script_url, std::string_view target_url, std::uint32_t timeout_ms) {
    const auto session = WinHttpOpen(L"Alchemy CFNetwork PAC", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return win32::last_error();
    struct Close { HINTERNET h; ~Close() { WinHttpCloseHandle(h); } } close{session};
    const auto timeout = static_cast<int>(std::min<std::uint32_t>(timeout_ms, 0x7fffffff));
    WinHttpSetTimeouts(session, timeout, timeout, timeout, timeout);
    const auto url = win32::widen(script_url), destination = win32::widen(target_url);
    WINHTTP_AUTOPROXY_OPTIONS options{};
    options.dwFlags = WINHTTP_AUTOPROXY_CONFIG_URL;
    options.lpszAutoConfigUrl = url.c_str();
    options.fAutoLogonIfChallenged = TRUE;
    WINHTTP_PROXY_INFO info{};
    if (!WinHttpGetProxyForUrl(session, destination.c_str(), &options, &info)) return win32::last_error();
    const auto list = utf8(info.lpszProxy);
    if (info.lpszProxy) GlobalFree(info.lpszProxy);
    if (info.lpszProxyBypass) GlobalFree(info.lpszProxyBypass);
    if (info.dwAccessType != WINHTTP_ACCESS_TYPE_NAMED_PROXY) return std::string{};   // DIRECT
    return list;
}

// ---- Sessions ----

Result<Session> Session::open(std::string_view user_agent, const Proxy& proxy) {
    const auto agent = win32::widen(user_agent);
    const auto list = win32::widen(proxy.list), bypass = win32::widen(proxy.bypass);
    DWORD access = WINHTTP_ACCESS_TYPE_NO_PROXY;
    if (proxy.kind == Proxy::Kind::named) access = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    else if (proxy.kind == Proxy::Kind::automatic) access = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    const auto handle = WinHttpOpen(agent.c_str(), access,
        access == WINHTTP_ACCESS_TYPE_NAMED_PROXY ? list.c_str() : WINHTTP_NO_PROXY_NAME,
        access == WINHTTP_ACCESS_TYPE_NAMED_PROXY && !bypass.empty() ? bypass.c_str() : WINHTTP_NO_PROXY_BYPASS, 0);
    if (!handle) return win32::last_error();
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(handle, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));
    DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
    WinHttpSetOption(handle, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));
    return Session(handle);
}

Session& Session::operator=(Session&& other) noexcept {
    if (this != &other) {
        if (handle_) WinHttpCloseHandle(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Session::~Session() { if (handle_) WinHttpCloseHandle(handle_); }

// ---- Requests ----

struct Request::State {
    std::mutex mutex;
    HINTERNET connect{}, handle{};
    bool closed{};
    std::atomic<bool> cancelled{};
    bool secure{};
    bool trust_rejected{};
    Trust trust;
    std::exception_ptr callback_error;
    Failure failure;

    ~State() {
        close_request();
        if (connect) WinHttpCloseHandle(connect);
    }
    void close_request() {
        std::lock_guard lock(mutex);
        if (handle && !closed) {
            WinHttpSetStatusCallback(handle, nullptr, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
            WinHttpCloseHandle(handle);
        }
        closed = true;
    }
    // Records why a call failed (cancellation and a refused chain win over the stack's error).
    template <class T>
    Result<T> fail(DWORD error) {
        if (trust_rejected) failure = {Failure::Kind::trust_rejected, {}};
        else if (cancelled) failure = {Failure::Kind::cancelled, {}};
        else failure = failure_of(error);
        return error_of(failure);
    }
    // The handle, or nothing once closed.
    HINTERNET current() {
        std::lock_guard lock(mutex);
        return closed ? nullptr : handle;
    }

    static void CALLBACK status_callback(HINTERNET, DWORD_PTR context, DWORD status, LPVOID, DWORD) {
        if (status != WINHTTP_CALLBACK_STATUS_SENDING_REQUEST || !context) return;
        reinterpret_cast<State*>(context)->on_sending();
    }
    void on_sending() {
        if (!secure || trust_rejected) return;
        std::vector<std::vector<std::uint8_t>> chain;
        {
            std::lock_guard lock(mutex);
            if (closed || !handle) return;
            PCCERT_CONTEXT leaf{};
            DWORD size = sizeof(leaf);
            if (WinHttpQueryOption(handle, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &leaf, &size) && leaf) {
                const auto der = [](PCCERT_CONTEXT context) {
                    return std::vector<std::uint8_t>(context->pbCertEncoded, context->pbCertEncoded + context->cbCertEncoded);
                };
                chain.push_back(der(leaf));
                if (leaf->hCertStore)
                    for (PCCERT_CONTEXT context = nullptr; (context = CertEnumCertificatesInStore(leaf->hCertStore, context)) != nullptr;) {
                        if (CertCompareCertificate(X509_ASN_ENCODING, context->pCertInfo, leaf->pCertInfo)) continue;
                        chain.push_back(der(context));
                    }
                CertFreeCertificateContext(leaf);
            }
        }
        bool accepted = false;
        try {
            accepted = !chain.empty() && trust && trust(chain);
        } catch (...) {
            callback_error = std::current_exception();
            accepted = false;
        }
        if (accepted) return;
        // Close before WinHTTP writes the request: the refused server sees no request bytes.
        trust_rejected = true;
        close_request();
    }
};

Request::Request(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&&) noexcept = default;
Request::~Request() = default;

Request Request::open(const Session& session, const Target& target, const Headers& headers, std::uint32_t timeout_ms, Trust trust) {
    auto state = std::make_unique<State>();
    state->secure = target.secure;
    state->trust = std::move(trust);
    const auto failed = [&]() -> Request {
        // The first send() reports it.
        state->failure = failure_of(GetLastError());
        state->closed = true;
        return Request(std::move(state));
    };
    state->connect = WinHttpConnect(session.handle_, win32::widen(target.host).c_str(), static_cast<INTERNET_PORT>(target.port), 0);
    if (!state->connect) return failed();
    const auto method = win32::widen(target.method);
    const auto path = win32::widen(target.path);
    DWORD flags = WINHTTP_FLAG_ESCAPE_DISABLE | WINHTTP_FLAG_ESCAPE_DISABLE_QUERY;
    if (target.secure) flags |= WINHTTP_FLAG_SECURE;
    state->handle = WinHttpOpenRequest(state->connect, method.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!state->handle) return failed();
    const auto handle = state->handle;
    DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
    // Certificates are judged by the Trust callback, not by WinHTTP.
    DWORD security = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
        SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    if (target.secure) WinHttpSetOption(handle, WINHTTP_OPTION_SECURITY_FLAGS, &security, sizeof(security));
    DWORD logon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;   // never send Windows credentials unasked
    WinHttpSetOption(handle, WINHTTP_OPTION_AUTOLOGON_POLICY, &logon, sizeof(logon));
    DWORD feature = WINHTTP_DISABLE_COOKIES;   // cookies belong to the caller
    WinHttpSetOption(handle, WINHTTP_OPTION_DISABLE_FEATURE, &feature, sizeof(feature));
    const auto timeout = static_cast<int>(std::min<std::uint32_t>(timeout_ms, 0x7fffffff));
    WinHttpSetTimeouts(handle, timeout, timeout, timeout, timeout);
    std::wstring lines;
    for (const auto& [name, value] : headers) lines += win32::widen(name) + L": " + win32::widen(value) + L"\r\n";
    if (!lines.empty() &&
        !WinHttpAddRequestHeaders(handle, lines.c_str(), static_cast<DWORD>(-1L), WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
        return failed();
    WinHttpSetStatusCallback(handle, &State::status_callback, WINHTTP_CALLBACK_FLAG_SEND_REQUEST, 0);
    return Request(std::move(state));
}

Result<Head> Request::send(ConstBytes body) {
    auto& state = *state_;
    if (state.cancelled) return state.fail<Head>(ERROR_WINHTTP_OPERATION_CANCELLED);
    const auto handle = state.current();
    if (!handle) {
        if (state.failure.kind != Failure::Kind::none && !state.cancelled) return error_of(state.failure);   // opening failed
        return state.fail<Head>(ERROR_WINHTTP_OPERATION_CANCELLED);
    }
    const auto* data = body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<std::byte*>(body.data());
    const auto length = static_cast<DWORD>(body.size());
    const BOOL ok = WinHttpSendRequest(handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<void*>(static_cast<const void*>(data)), length,
                                       length, reinterpret_cast<DWORD_PTR>(&state));
    const auto error = GetLastError();
    if (state.callback_error) std::rethrow_exception(std::exchange(state.callback_error, nullptr));
    if (!ok || state.trust_rejected) return state.fail<Head>(error);
    if (!WinHttpReceiveResponse(handle, nullptr)) return state.fail<Head>(GetLastError());
    Head head;
    DWORD status{}, size = sizeof(status);
    if (!WinHttpQueryHeaders(handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                             WINHTTP_NO_HEADER_INDEX))
        return state.fail<Head>(GetLastError());
    head.status = static_cast<int>(status);
    size = 0;
    WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    std::wstring raw(size / sizeof(wchar_t) + 1, L'\0');
    if (size && WinHttpQueryHeaders(handle, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        raw.resize(size / sizeof(wchar_t));
        const auto text = win32::narrow(raw);
        std::size_t at = 0;
        bool first = true;
        while (at < text.size()) {
            auto end = text.find("\r\n", at);
            if (end == std::string::npos) end = text.size();
            const auto line = text.substr(at, end - at);
            at = end + 2;
            if (line.empty()) continue;
            if (first) {
                first = false;
                head.version = line.substr(0, line.find(' '));
                continue;
            }
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            auto value = line.substr(colon + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            head.headers.emplace_back(line.substr(0, colon), value);
        }
    }
    return head;
}

Result<std::size_t> Request::read(Bytes buffer) {
    auto& state = *state_;
    const auto handle = state.current();
    if (!handle) return state.fail<std::size_t>(ERROR_WINHTTP_OPERATION_CANCELLED);
    DWORD read = 0;
    if (!WinHttpReadData(handle, buffer.data(), static_cast<DWORD>(std::min<std::size_t>(buffer.size(), 1U << 20)), &read))
        return state.fail<std::size_t>(GetLastError());
    return static_cast<std::size_t>(read);
}

Result<Challenge> Request::challenge(int status) {
    const auto handle = state_->current();
    if (!handle) return Error::bad_handle;
    DWORD supported{}, first{}, target{};
    if (!WinHttpQueryAuthSchemes(handle, &supported, &first, &target)) return win32::last_error();
    Challenge result;
    result.proxy = target == WINHTTP_AUTH_TARGET_PROXY;
    for (const DWORD scheme : {first, static_cast<DWORD>(WINHTTP_AUTH_SCHEME_NEGOTIATE), static_cast<DWORD>(WINHTTP_AUTH_SCHEME_NTLM),
                               static_cast<DWORD>(WINHTTP_AUTH_SCHEME_DIGEST), static_cast<DWORD>(WINHTTP_AUTH_SCHEME_BASIC)}) {
        if (!(supported & scheme)) continue;
        Scheme value;
        if (scheme == WINHTTP_AUTH_SCHEME_BASIC) value = Scheme::basic;
        else if (scheme == WINHTTP_AUTH_SCHEME_DIGEST) value = Scheme::digest;
        else if (scheme == WINHTTP_AUTH_SCHEME_NTLM) value = Scheme::ntlm;
        else if (scheme == WINHTTP_AUTH_SCHEME_NEGOTIATE) value = Scheme::negotiate;
        else continue;   // Passport and other schemes the caller cannot answer
        if (std::find(result.schemes.begin(), result.schemes.end(), value) == result.schemes.end()) result.schemes.push_back(value);
    }
    // realm="..." from the authenticate header.
    const auto query = status == 407 ? WINHTTP_QUERY_PROXY_AUTHENTICATE : WINHTTP_QUERY_WWW_AUTHENTICATE;
    std::array<wchar_t, 1024> buffer{};
    DWORD size = sizeof(buffer) - sizeof(wchar_t);
    if (WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        const auto text = win32::narrow(std::wstring_view(buffer.data(), size / sizeof(wchar_t)));
        auto lowered = text;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        if (const auto at = lowered.find("realm=\""); at != std::string::npos) {
            const auto end = text.find('"', at + 7);
            result.realm = text.substr(at + 7, end == std::string::npos ? std::string::npos : end - at - 7);
        }
    }
    return result;
}

Status Request::set_credentials(bool proxy, Scheme scheme, std::string_view user, std::string_view password) {
    const auto handle = state_->current();
    if (!handle) return Error::bad_handle;
    const auto name = win32::widen(user), secret = win32::widen(password);
    if (!WinHttpSetCredentials(handle, proxy ? WINHTTP_AUTH_TARGET_PROXY : WINHTTP_AUTH_TARGET_SERVER, scheme_flag(scheme), name.c_str(),
                               secret.c_str(), nullptr))
        return win32::last_error();
    return {};
}

void Request::cancel() noexcept {
    state_->cancelled = true;
    state_->close_request();
}

Failure Request::failure() const { return state_->failure; }

} // namespace alchemy::host::http
