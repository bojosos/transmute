#include "alchemy/darwin/network/http.hpp"
#include "alchemy/darwin/network/cookies.hpp"

#include "alchemy/darwin_threads.hpp"
#include "alchemy/host/files.hpp"
#include "alchemy/path.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

// Foundation URL loading: NSURLRequest / NSMutableURLRequest, NSURLResponse
// / NSHTTPURLResponse, NSURLSessionConfiguration, NSURLSession and its data,
// upload and download tasks, NSURLAuthenticationChallenge,
// NSURLProtectionSpace, NSURLCredential and the shared NSHTTPCookieStorage
// object. Transfers run over WinHTTP (http.hpp) on a guest dispatch worker;
// delegate messages and completion handlers run on the session's delegate
// queue (an NSOperationQueue; a private serial one when none is given), in
// order, as on macOS. Server trust goes through SecTrust and, when the
// delegate implements it, the NSURLAuthenticationMethodServerTrust
// challenge.
//
// Not provided (explicit failures): background sessions, stream / WebSocket
// tasks, custom NSURLProtocol classes, HTTPBodyStream uploads, client
// certificate credentials, response dispositions other than allow / cancel,
// and resume data. Cookies: NSHTTPCookieStorage's jar (cookies.cpp) sets
// Cookie headers and stores Set-Cookie on every hop; default sessions share
// it, ephemeral ones keep their own; nothing is persisted.
namespace alchemy::gui::network {
namespace {

using security::Security;
using security::TrustOutcome;
using security::TrustPolicy;

double d_arg(const CpuState& s, unsigned index) { return std::bit_cast<double>(s.v[index][0]); }
void return_double(CpuState& s, double value) { s.v[0] = {std::bit_cast<std::uint64_t>(value), 0}; }

[[noreturn]] void stop_now(ProcessContext& process) {
    if (guest_worker_must_stop(process)) throw GuestThreadStop{};
    EngineAdapter::require("cooperative guest stop during URL loading");
}

// ---- states --------------------------------------------------------------------------------------

struct RequestState final : ObjectPayload {
    std::mutex mutex;
    Address url{};                // +1
    Address main_document{};      // +1
    std::string method{"GET"};
    Headers headers;
    std::optional<Bytes> body;
    double timeout{60};
    std::uint64_t cache_policy{}, service_type{};
    bool cookies{true}, cellular{true}, pipelining{}, expensive{true}, constrained{true};
};

struct ResponseState final : ObjectPayload {
    Address url{};                // +1
    std::string mime, encoding, version;
    std::int64_t expected{-1};
    bool http{};
    std::int64_t status{};
    Headers headers;
};

struct Properties {
    std::mutex mutex;
    std::map<std::string, Address> objects;   // +1
    std::map<std::string, std::uint64_t> integers;
    std::map<std::string, double> reals;
};
struct ConfigurationState final : ObjectPayload { Properties properties; bool ephemeral{}; };

struct Decision {
    std::mutex mutex;
    std::condition_variable cv;
    bool ready{};
    std::int64_t value{};
    Address object{};             // +1 (credential / request), released by the waiter
};

struct Transfer {
    std::mutex mutex;
    std::condition_variable cv;
    bool cancelled{}, suspended{};
    std::shared_ptr<HttpExchange> exchange;
};

enum class TaskKind { data, upload, download };
enum class TaskStatus : std::int64_t { running = 0, suspended = 1, canceling = 2, completed = 3 };

struct SessionState;
struct TaskState final : ObjectPayload {
    std::mutex mutex;
    std::weak_ptr<SessionState> session_state;
    Address session{};            // +1
    Address original{}, current{}, response{}, error{}, completion{}, description{};   // +1
    TaskKind kind{TaskKind::data};
    std::uint64_t identifier{};
    std::atomic<std::int64_t> status{static_cast<std::int64_t>(TaskStatus::suspended)};
    bool started{};
    std::atomic<std::int64_t> received{}, sent{}, expected_receive{-1}, expected_send{};
    double priority{0.5};
    std::optional<Bytes> upload;
    std::shared_ptr<Transfer> transfer = std::make_shared<Transfer>();
};

struct SessionState final : ObjectPayload {
    std::mutex mutex;
    Address configuration{};      // +1 private copy
    Address delegate{};           // +1 until invalidated
    Address delegate_queue{};     // +1 NSOperationQueue
    Address dispatch{};           // its dispatch queue
    Address description{};        // +1
    std::shared_ptr<HttpClient> client;
    bool shared{}, invalidated{}, finishing{}, ephemeral{}, self_retained{};
    std::set<Address> tasks;      // +1 while not completed
    std::uint64_t next_identifier{1};
    double request_timeout{60}, resource_timeout{604800};
    Headers additional;
    bool cookies{true};
    // Ephemeral sessions keep their own cookies; others use the shared storage's jar.
    std::shared_ptr<CookieJar> cookie_jar;
    std::set<std::pair<std::string, Bytes>> trusted;   // (host, leaf SHA-256) accepted by the delegate
};

struct ChallengeState final : ObjectPayload {
    Address space{}, credential{}, failure{}, error{};   // +1
    std::int64_t failures{};
};
struct SpaceState final : ObjectPayload {
    std::string host, protocol, realm, method, proxy_type;
    std::int64_t port{};
    Address trust{};              // +1
    bool proxy{};
};
struct CredentialState final : ObjectPayload {
    Address trust{};              // +1
    std::string user, password;
    bool has_password{};
    std::uint64_t persistence{};
};

// ---- the provider ----------------------------------------------------------------------------------

struct Loading : std::enable_shared_from_this<Loading> {
    std::shared_ptr<Urls> urls;
    std::shared_ptr<Bridge> b;
    std::shared_ptr<Security> sec;
    Address request_class{}, mutable_request_class{}, response_class{}, http_response_class{}, configuration_class{};
    Address session_class{}, task_class{}, data_task_class{}, upload_task_class{}, download_task_class{};
    Address challenge_class{}, space_class{}, credential_class{}, cookie_storage_class{};
    Address shared_session{}, shared_cookie_storage{};
    std::uint64_t cookie_policy{2};           // NSHTTPCookieAcceptPolicyOnlyFromMainDocumentDomain
    std::map<std::string, Address, std::less<>> constants;
    std::mutex mutex;
    std::shared_ptr<HttpClient> default_client;
    std::string user_agent;

    Address constant(std::string_view name) {
        std::lock_guard lock(mutex);
        const auto found = constants.find(name);
        if (found == constants.end()) security::fail("URL loading constant missing: " + std::string(name));
        return found->second;
    }
    Address export_string(std::string_view name, std::string_view value) {
        const auto object = b->export_string(foundation, "_" + std::string(name), value);
        std::lock_guard lock(mutex);
        constants.emplace(std::string(name), object);
        return object;
    }
    Address autoreleased_string(const std::string& value) { return b->autorelease(b->string(value)); }

    template <class T> std::shared_ptr<T> require(Address object, const char* what) {
        auto value = object ? b->objc.find_host_state<T>(object) : nullptr;
        if (!value) security::fail(std::string("expected an initialized ") + what);
        return value;
    }

    // ---- waiting ------------------------------------------------------------------------------------
    // Waits for `ready` while polling cancellation and process stop.
    template <class Ready>
    bool wait(std::unique_lock<std::mutex>& lock, std::condition_variable& cv, Ready ready, const std::shared_ptr<Transfer>& transfer) {
        while (!ready()) {
            cv.wait_for(lock, std::chrono::milliseconds(50));
            if (transfer) {
                std::lock_guard guard(transfer->mutex);
                if (transfer->cancelled) return false;
            }
            if (guest_worker_must_stop(b->process) || engine_stop_requested(b->process.calling_engine())) {
                lock.unlock();
                stop_now(b->process);
            }
        }
        return true;
    }

    // ---- headers ------------------------------------------------------------------------------------
    Headers headers_from(Address dictionary) {
        Headers result;
        if (!dictionary) return result;
        for (const auto& [key, value] : b->cf.dictionary_entries(dictionary)) {
            const auto name = b->maybe_text(key);
            if (!name) continue;
            if (const auto text = b->maybe_text(value)) result.emplace_back(*name, *text);
            else if (const auto number = b->integer(value)) result.emplace_back(*name, std::to_string(*number));
        }
        return result;
    }
    Address dictionary_from(const Headers& headers) {
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        for (const auto& [name, value] : headers) {
            const auto key = b->string(name), text = b->string(value);
            owned.push_back(key); owned.push_back(text);
            entries.emplace_back(key, text);
        }
        const auto result = b->dictionary(entries);
        b->release_all(owned);
        return result;
    }
    static void set_header(Headers& headers, const std::string& name, const std::string& value, bool append) {
        for (auto& [key, existing] : headers)
            if (security::iequals(key, name)) {
                existing = append ? existing + "," + value : value;
                return;
            }
        headers.emplace_back(name, value);
    }

    // ---- requests -------------------------------------------------------------------------------------
    Address make_request(Address cls, Address url, std::uint64_t policy, double timeout) {
        const auto object = b->objc.allocate(cls);
        auto state = std::make_shared<RequestState>();
        state->url = b->retain(url);
        state->cache_policy = policy;
        state->timeout = timeout;
        static_cast<void>(b->objc.host_state<RequestState>(object, [&] { return state; }));
        return object;
    }
    void init_request(Address object, Address url, std::uint64_t policy, double timeout) {
        auto state = std::make_shared<RequestState>();
        state->url = b->retain(url);
        state->cache_policy = policy;
        state->timeout = timeout;
        const auto created = b->objc.host_state<RequestState>(object, [&] { return state; });
        if (created != state) {
            b->release(state->url);
            security::fail("NSURLRequest initialized twice");
        }
    }
    Address copy_request(Address source, Address cls) {
        const auto from = require<RequestState>(source, "NSURLRequest");
        const auto object = b->objc.allocate(cls);
        auto state = std::make_shared<RequestState>();
        {
            std::lock_guard lock(from->mutex);
            state->url = b->retain(from->url);
            state->main_document = b->retain(from->main_document);
            state->method = from->method; state->headers = from->headers; state->body = from->body;
            state->timeout = from->timeout; state->cache_policy = from->cache_policy; state->service_type = from->service_type;
            state->cookies = from->cookies; state->cellular = from->cellular; state->pipelining = from->pipelining;
            state->expensive = from->expensive; state->constrained = from->constrained;
        }
        static_cast<void>(b->objc.host_state<RequestState>(object, [&] { return state; }));
        return object;
    }
    bool is_mutable_request(Address object) {
        for (auto cls = b->objc.object_class(object); cls; ) {
            if (cls == mutable_request_class) return true;
            if (cls == request_class) return false;
            const auto super = b->objc.send(cls, "superclass").x0;
            if (super == cls) break;
            cls = super;
        }
        return false;
    }

    // ---- responses --------------------------------------------------------------------------------------
    Address make_response(Address url, const HttpResponseHead* head, const std::string& mime, std::int64_t expected, const std::string& encoding) {
        const auto object = b->objc.allocate(head ? http_response_class : response_class);
        auto state = std::make_shared<ResponseState>();
        state->url = b->retain(url);
        state->mime = mime;
        state->expected = expected;
        state->encoding = encoding;
        if (head) {
            state->http = true;
            state->status = head->status;
            state->headers = head->headers;
            state->version = head->version;
        }
        static_cast<void>(b->objc.host_state<ResponseState>(object, [&] { return state; }));
        return object;
    }
    static void content_type(const Headers& headers, std::string& mime, std::string& encoding) {
        const auto type = header_value(headers, "Content-Type");
        if (!type) return;
        const auto semicolon = type->find(';');
        mime = security::lower(type->substr(0, semicolon));
        mime.erase(mime.find_last_not_of(" \t") + 1);
        if (semicolon == std::string::npos) return;
        const auto lowered = security::lower(*type);
        if (const auto at = lowered.find("charset=", semicolon); at != std::string::npos) {
            auto value = type->substr(at + 8);
            value = value.substr(0, value.find(';'));
            value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
            encoding = security::lower(value);
        }
    }

    // ---- errors -----------------------------------------------------------------------------------------
    Address url_error(std::int64_t code, const std::string& description, Address url, Address trust = 0, Address underlying = 0) {
        std::vector<std::pair<std::string, Address>> info;
        std::vector<Address> owned;
        if (url) {
            info.emplace_back("NSErrorFailingURLKey", url);
            owned.push_back(b->string(urls->text(url)));
            info.emplace_back("NSErrorFailingURLStringKey", owned.back());
        }
        if (trust) info.emplace_back("NSURLErrorFailingURLPeerTrustErrorKey", trust);
        if (underlying) info.emplace_back("NSUnderlyingError", underlying);
        const auto error = b->error("NSURLErrorDomain", code, description, info);
        b->release_all(owned);
        return error;
    }

    // ---- delegate delivery --------------------------------------------------------------------------------
    // Runs `body` on the session's delegate queue.
    void deliver(const std::shared_ptr<SessionState>& session, std::function<void()> body) {
        Address queue;
        { std::lock_guard lock(session->mutex); queue = session->dispatch; }
        b->dispatch_async(queue, std::move(body));
    }
    Address delegate_of(const std::shared_ptr<SessionState>& session) {
        std::lock_guard lock(session->mutex);
        return session->delegate;
    }
    bool responds(Address delegate, std::string_view selector) {
        return delegate && b->objc.responds(delegate, b->objc.selector(selector));
    }
    // The arguments of a delegate's completion handler: an object
    // (willPerformHTTPRedirection: the request), a value and an object
    // (challenges: disposition, credential) or a value only
    // (didReceiveResponse: the disposition; x2 is then not an argument and
    // holds whatever the caller left there).
    enum class Handler { object, value_and_object, value };
    // A completion-handler block whose first call stores its arguments in `decision`.
    Address decision_block(const std::shared_ptr<Decision>& decision, Handler shape) {
        auto bridge = b;
        auto block = std::make_shared<Address>(0);
        *block = b->make_block([bridge, decision, shape, block](CpuState& s, GuestMemory&) {
            {
                std::lock_guard lock(decision->mutex);
                if (decision->ready) security::fail("URL loading completion handler called twice");
                if (shape == Handler::object) decision->object = bridge->retain(s.x[1]);
                else decision->value = static_cast<std::int64_t>(s.x[1]);
                if (shape == Handler::value_and_object) decision->object = bridge->retain(s.x[2]);
                decision->ready = true;
            }
            decision->cv.notify_all();
            bridge->forget_block(*block);
            s.x[0] = 0;
        });
        return *block;
    }
    // Waits for a delegate decision; false when the task was cancelled meanwhile.
    bool await(const std::shared_ptr<Decision>& decision, const std::shared_ptr<Transfer>& transfer) {
        std::unique_lock lock(decision->mutex);
        return wait(lock, decision->cv, [&] { return decision->ready; }, transfer);
    }

    // ---- challenges ---------------------------------------------------------------------------------------
    Address make_space(const std::string& host, std::int64_t port, const std::string& protocol, const std::string& realm,
                       const std::string& method, Address trust, bool proxy) {
        const auto object = b->objc.allocate(space_class);
        auto state = std::make_shared<SpaceState>();
        state->host = host; state->port = port; state->protocol = protocol; state->realm = realm; state->method = method;
        state->trust = b->retain(trust); state->proxy = proxy;
        static_cast<void>(b->objc.host_state<SpaceState>(object, [&] { return state; }));
        return object;
    }
    Address make_challenge(Address space, std::int64_t failures, Address failure_response) {
        const auto object = b->objc.allocate(challenge_class);
        auto state = std::make_shared<ChallengeState>();
        state->space = b->retain(space);
        state->failures = failures;
        state->failure = b->retain(failure_response);
        static_cast<void>(b->objc.host_state<ChallengeState>(object, [&] { return state; }));
        return object;
    }
    // Sends the challenge to the delegate (session-level for server trust when
    // implemented, else task-level) and waits. nullopt: no delegate method.
    std::optional<std::pair<std::int64_t, Address>> ask_challenge(const std::shared_ptr<SessionState>& session, Address session_object,
                                                                  Address task, Address challenge, bool connection_level,
                                                                  const std::shared_ptr<Transfer>& transfer) {
        const auto delegate = delegate_of(session);
        const bool session_level = connection_level && responds(delegate, "URLSession:didReceiveChallenge:completionHandler:");
        const bool task_level = responds(delegate, "URLSession:task:didReceiveChallenge:completionHandler:");
        if (!session_level && !task_level) return std::nullopt;
        auto decision = std::make_shared<Decision>();
        const auto block = decision_block(decision, Handler::value_and_object);
        b->retain(challenge);
        deliver(session, [this, delegate, session_level, session_object, task, challenge, block] {
            CallRegs registers;
            registers.x[2] = session_object;
            if (session_level) { registers.x[3] = challenge; registers.x[4] = block; }
            else { registers.x[3] = task; registers.x[4] = challenge; registers.x[5] = block; }
            static_cast<void>(b->objc.send(delegate, session_level ? "URLSession:didReceiveChallenge:completionHandler:"
                                                                   : "URLSession:task:didReceiveChallenge:completionHandler:", registers));
            b->release(challenge);
        });
        if (!await(decision, transfer)) return std::make_pair(std::int64_t{2}, Address{0});
        return std::make_pair(decision->value, decision->object);
    }

    // Server trust for one TLS connection of `task`.
    bool server_trust(const std::shared_ptr<SessionState>& session, Address session_object, Address task, const UrlParts& url,
                      const std::vector<security::CertificatePtr>& chain, const std::shared_ptr<Transfer>& transfer,
                      std::optional<TrustOutcome>& rejected) {
        const auto host = url.host_name();
        const auto leaf = chain[0]->sha256();
        {
            std::lock_guard lock(session->mutex);
            if (session->trusted.contains({host, leaf})) return true;
        }
        const auto trust = sec->make_trust(chain, {{TrustPolicy::Kind::ssl, true, host, 0}});
        struct Release { Bridge& b; Address object; ~Release() { b.release(object); } } release_trust{*b, trust};
        const auto space = make_space(host, url.effective_port(), "https", {}, "NSURLAuthenticationMethodServerTrust", trust, false);
        const auto challenge = make_challenge(space, 0, 0);
        b->release(space);
        const auto answer = ask_challenge(session, session_object, task, challenge, true, transfer);
        b->release(challenge);
        bool default_handling = !answer;
        if (answer) {
            const auto [disposition, credential] = *answer;
            b->release(credential);
            switch (disposition) {
            case 0:   // NSURLSessionAuthChallengeUseCredential
                if (credential) {
                    std::lock_guard lock(session->mutex);
                    session->trusted.insert({host, leaf});
                    return true;
                }
                default_handling = true;
                break;
            case 2: return false;   // cancel
            default: default_handling = true; break;   // default handling / reject protection space
            }
        }
        if (default_handling) {
            const auto outcome = sec->evaluate(trust);
            if (outcome.trusted) return true;
            rejected = outcome;
        }
        return false;
    }

    // ---- task completion ------------------------------------------------------------------------------------
    void finish(const std::shared_ptr<SessionState>& session, Address session_object, Address task, const std::shared_ptr<TaskState>& state,
                Address error, Bytes data, std::filesystem::path file) {
        {
            std::lock_guard lock(state->mutex);
            if (state->error) b->release(state->error);
            state->error = error;   // +1 moved in
        }
        state->status = static_cast<std::int64_t>(TaskStatus::completed);
        deliver(session, [this, session, session_object, task, state, error, data = std::move(data), file] {
            const auto delegate = delegate_of(session);
            Address completion, response;
            {
                std::lock_guard lock(state->mutex);
                completion = std::exchange(state->completion, 0);
                response = state->response;
            }
            const auto remove_file = [&] { std::error_code ignored; if (!file.empty()) std::filesystem::remove(file, ignored); };
            if (completion) {
                if (state->kind == TaskKind::download) {
                    const auto location = !error && !file.empty() ? b->cf.file_url(guest_path_string(file), false) : 0;
                    b->invoke_block(completion, {location, response, error});
                    b->release(location);
                } else {
                    const auto body = error ? 0 : b->data(data);
                    b->invoke_block(completion, {body, response, error});
                    b->release(body);
                }
                b->block_release(completion);
                remove_file();
            } else {
                if (state->kind == TaskKind::download && !error && responds(delegate, "URLSession:downloadTask:didFinishDownloadingToURL:")) {
                    const auto location = b->cf.file_url(guest_path_string(file), false);
                    CallRegs registers;
                    registers.x[2] = session_object; registers.x[3] = task; registers.x[4] = location;
                    static_cast<void>(b->objc.send(delegate, "URLSession:downloadTask:didFinishDownloadingToURL:", registers));
                    b->release(location);
                }
                remove_file();
                if (responds(delegate, "URLSession:task:didCompleteWithError:")) {
                    CallRegs registers;
                    registers.x[2] = session_object; registers.x[3] = task; registers.x[4] = error;
                    static_cast<void>(b->objc.send(delegate, "URLSession:task:didCompleteWithError:", registers));
                }
            }
            bool invalidate = false;
            {
                std::lock_guard lock(session->mutex);
                session->tasks.erase(task);
                invalidate = session->finishing && session->tasks.empty() && !session->invalidated;
            }
            b->release(task);
            if (invalidate) become_invalid(session, session_object);
        });
    }

    // Delivers URLSession:didBecomeInvalidWithError: and drops the delegate.
    // Runs on the delegate queue.
    void become_invalid(const std::shared_ptr<SessionState>& session, Address session_object) {
        Address delegate;
        bool self_retained;
        {
            std::lock_guard lock(session->mutex);
            if (session->invalidated) return;
            session->invalidated = true;
            delegate = std::exchange(session->delegate, 0);
            self_retained = std::exchange(session->self_retained, false);
        }
        if (responds(delegate, "URLSession:didBecomeInvalidWithError:")) {
            CallRegs registers;
            registers.x[2] = session_object;
            static_cast<void>(b->objc.send(delegate, "URLSession:didBecomeInvalidWithError:", registers));
        }
        b->release(delegate);
        if (self_retained) b->release(session_object);   // the reference the delegate cycle held
    }

    // ---- the transfer ------------------------------------------------------------------------------------------
    void run(Address task);
    std::shared_ptr<HttpClient> client_for(const std::shared_ptr<SessionState>& session);
    std::string default_user_agent();
    Address create_task(Address session_object, TaskKind kind, Address request, Address completion, std::optional<Bytes> upload);
};

std::string Loading::default_user_agent() {
    {
        std::lock_guard lock(mutex);
        if (!user_agent.empty()) return user_agent;
    }
    // "<CFBundleName>/<CFBundleShortVersionString> CFNetwork/1494.0.7 Darwin/23.0.0" (macOS 14).
    std::string name = "Alchemy", version;
    if (const auto bundle_class = b->objc.find_class("NSBundle")) {
        if (const auto bundle = b->objc.send(bundle_class, "mainBundle").x0) {
            const auto value = [&](std::string_view key) -> std::optional<std::string> {
                const auto key_object = b->string(key);
                CallRegs registers;
                registers.x[2] = key_object;
                const auto result = b->objc.send(bundle, "objectForInfoDictionaryKey:", registers).x0;
                b->release(key_object);
                return b->maybe_text(result);
            };
            if (const auto bundle_name = value("CFBundleName")) name = *bundle_name;
            if (const auto bundle_version = value("CFBundleShortVersionString")) version = *bundle_version;
        }
    }
    std::string agent = name;
    std::replace(agent.begin(), agent.end(), ' ', '-');
    if (!version.empty()) agent += "/" + version;
    agent += " CFNetwork/1494.0.7 Darwin/23.0.0";
    std::lock_guard lock(mutex);
    user_agent = agent;
    return agent;
}

std::shared_ptr<HttpClient> Loading::client_for(const std::shared_ptr<SessionState>& session) {
    {
        std::lock_guard lock(session->mutex);
        if (session->client) return session->client;
    }
    const auto agent = default_user_agent();
    std::shared_ptr<HttpClient> client;
    if (session->ephemeral) client = HttpClient::create(agent, system_proxy());
    else {
        std::lock_guard lock(mutex);
        if (!default_client) default_client = HttpClient::create(agent, system_proxy());
        client = default_client;
    }
    std::lock_guard lock(session->mutex);
    if (!session->client) session->client = client;
    return session->client;
}

Address Loading::create_task(Address session_object, TaskKind kind, Address request, Address completion, std::optional<Bytes> upload) {
    const auto session = require<SessionState>(session_object, "NSURLSession");
    {
        std::lock_guard lock(session->mutex);
        if (session->invalidated || session->finishing) security::fail("task created on an invalidated NSURLSession");
    }
    if (!request || !b->objc.find_host_state<RequestState>(request)) security::fail("NSURLSession task without an NSURLRequest");
    const auto cls = kind == TaskKind::download ? download_task_class : kind == TaskKind::upload ? upload_task_class : data_task_class;
    const auto task = b->objc.allocate(cls);
    auto state = std::make_shared<TaskState>();
    state->session_state = session;
    state->session = b->retain(session_object);
    state->kind = kind;
    state->original = copy_request(request, request_class);
    state->current = b->retain(state->original);
    state->completion = b->block_copy(completion);
    state->upload = std::move(upload);
    if (state->upload) state->expected_send = static_cast<std::int64_t>(state->upload->size());
    {
        std::lock_guard lock(session->mutex);
        state->identifier = session->next_identifier++;
    }
    static_cast<void>(b->objc.host_state<TaskState>(task, [&] { return state; }));
    return task;
}

void Loading::run(Address task) {
    const auto state = require<TaskState>(task, "NSURLSessionTask");
    const auto session = state->session_state.lock();
    if (!session) security::fail("NSURLSessionTask outlived its session");
    const auto session_object = state->session;
    const auto transfer = state->transfer;
    const auto fail_task = [&](std::int64_t code, const std::string& description, Address trust = 0, Address underlying = 0) {
        Address url;
        { std::lock_guard lock(state->mutex); url = b->retain(require<RequestState>(state->current, "NSURLRequest")->url); }
        const auto error = url_error(code, description, url, trust, underlying);
        b->release(url);
        finish(session, session_object, task, state, error, {}, {});
    };

    // Snapshot of the current request.
    Address current;
    { std::lock_guard lock(state->mutex); current = b->retain(state->current); }
    struct Holder { Bridge& b; Address& object; ~Holder() { b.release(object); } } hold_current{*b, current};
    HttpRequest request;
    std::shared_ptr<CookieJar> jar;
    {
        const auto source = require<RequestState>(current, "NSURLRequest");
        std::lock_guard lock(source->mutex);
        const auto parts = source->url ? urls->parts(source->url) : std::nullopt;
        if (!parts) { fail_task(url_error::bad_url, "unsupported URL"); return; }
        request.url = *parts;
        request.method = source->method;
        std::lock_guard session_lock(session->mutex);
        request.headers = session->additional;
        for (const auto& [name, value] : source->headers) set_header(request.headers, name, value, false);
        request.body = source->body.value_or(Bytes{});
        request.timeout = source->timeout == 60 ? session->request_timeout : source->timeout;
        request.cookies = source->cookies && session->cookies && cookie_policy != 1;   // NSHTTPCookieAcceptPolicyNever
        if (!session->cookie_jar) session->cookie_jar = session->ephemeral ? std::make_shared<CookieJar>() : shared_cookie_jar();
        jar = session->cookie_jar;
    }
    // The request's own Cookie header wins; otherwise the jar's cookies for each hop's URL.
    const bool own_cookie_header = header_value(request.headers, "Cookie").has_value();
    if (state->upload) request.body = *state->upload;
    if (request.url.scheme != "http" && request.url.scheme != "https") { fail_task(url_error::unsupported_url, "unsupported URL"); return; }
    if (request.url.host.empty()) { fail_task(url_error::bad_url, "unsupported URL"); return; }
    if (!header_value(request.headers, "Accept")) request.headers.emplace_back("Accept", "*/*");
    if (!header_value(request.headers, "Accept-Language")) request.headers.emplace_back("Accept-Language", "en-US,en;q=0.9");

    const auto client = client_for(session);
    std::optional<TrustOutcome> rejected;
    std::vector<security::CertificatePtr> rejected_chain;
    int redirects = 0, auth_attempts = 0;
    std::shared_ptr<HttpExchange> exchange;
    HttpResponseHead head;
    for (;;) {
        rejected.reset();
        if (request.cookies && !own_cookie_header) {
            std::erase_if(request.headers, [](const auto& header) { return security::iequals(header.first, "Cookie"); });
            if (const auto cookie = jar->request_header(request.url.scheme, request.url.host_name(), request.url.path.empty() ? "/" : request.url.path, unix_now()))
                request.headers.emplace_back("Cookie", *cookie);
        }
        exchange = std::make_shared<HttpExchange>(client, request, [&, url = request.url](const std::vector<security::CertificatePtr>& chain) {
            rejected_chain = chain;
            return server_trust(session, session_object, task, url, chain, transfer, rejected);
        });
        {
            std::lock_guard lock(transfer->mutex);
            if (transfer->cancelled) { fail_task(url_error::cancelled, "cancelled"); return; }
            transfer->exchange = exchange;
        }
        auto result = exchange->send();
        // Authentication challenges re-send on the same exchange.
        while (std::holds_alternative<HttpResponseHead>(result)) {
            const auto& response = std::get<HttpResponseHead>(result);
            if ((response.status != 401 && response.status != 407) || auth_attempts >= 5) break;
            const auto challenge = exchange->challenge(response.status);
            if (!challenge || challenge->schemes.empty()) break;
            std::optional<std::pair<std::string, std::string>> credentials;
            if (challenge->target == AuthTarget::server && request.url.user && auth_attempts == 0)
                credentials = std::make_pair(percent_decode(*request.url.user), percent_decode(request.url.password.value_or("")));
            if (!credentials) {
                const auto scheme = challenge->schemes[0];
                using Scheme = host::http::Scheme;
                const char* method = scheme == Scheme::basic ? "NSURLAuthenticationMethodHTTPBasic"
                    : scheme == Scheme::digest ? "NSURLAuthenticationMethodHTTPDigest"
                    : scheme == Scheme::ntlm ? "NSURLAuthenticationMethodNTLM" : "NSURLAuthenticationMethodNegotiate";
                Address url;
                { std::lock_guard lock(state->mutex); url = b->retain(require<RequestState>(current, "NSURLRequest")->url); }
                const auto failure = make_response(url, &response, {}, -1, {});
                b->release(url);
                const auto space = make_space(request.url.host_name(), request.url.effective_port(), request.url.scheme, challenge->realm,
                                              method, 0, challenge->target == AuthTarget::proxy);
                const auto object = make_challenge(space, auth_attempts, failure);
                b->release_all({space, failure});
                const auto answer = ask_challenge(session, session_object, task, object, false, transfer);
                b->release(object);
                if (!answer) break;
                const auto [disposition, credential] = *answer;
                struct Drop { Bridge& b; Address object; ~Drop() { b.release(object); } } drop{*b, credential};
                if (disposition == 2) { fail_task(url_error::cancelled, "cancelled"); return; }
                if (disposition != 0 || !credential) break;
                const auto value = b->objc.find_host_state<CredentialState>(credential);
                if (!value || !value->has_password) break;
                credentials = std::make_pair(value->user, value->password);
            }
            ++auth_attempts;
            if (!exchange->set_credentials(challenge->target, challenge->schemes[0], credentials->first, credentials->second)) break;
            result = exchange->send();
        }
        if (std::holds_alternative<HttpFailure>(result)) {
            const auto failure = std::get<HttpFailure>(result);
            if (failure.trust && rejected) {
                const auto trust = sec->make_trust(rejected_chain, {{TrustPolicy::Kind::ssl, true, request.url.host_name(), 0}});
                const auto underlying = b->os_status_error(rejected->status, rejected->description);
                fail_task(failure.code, failure.description, trust, underlying);
                b->release_all({trust, underlying});
            } else {
                fail_task(failure.code, failure.description);
            }
            return;
        }
        head = std::get<HttpResponseHead>(result);
        state->sent = static_cast<std::int64_t>(exchange->bytes_sent());
        // Set-Cookie into the jar (every hop, as Foundation stores them during redirects).
        if (request.cookies) {
            std::vector<std::string> set_cookies;
            for (const auto& [name, value] : head.headers) if (security::iequals(name, "Set-Cookie")) set_cookies.push_back(value);
            if (!set_cookies.empty())
                jar->store_response(set_cookies, request.url.host_name(), request.url.path.empty() ? "/" : request.url.path, unix_now());
        }
        // Redirects.
        const auto location = header_value(head.headers, "Location");
        if (location && (head.status == 301 || head.status == 302 || head.status == 303 || head.status == 307 || head.status == 308)) {
            if (++redirects > 16) { fail_task(url_error::too_many_redirects, "too many HTTP redirects"); return; }
            const auto target = resolve_url(request.url, *location);
            if (!target) break;
            HttpRequest next = request;
            next.url = *target;
            next.url.fragment.reset();
            if (head.status == 303 || ((head.status == 301 || head.status == 302) && request.method == "POST")) {
                if (next.method != "HEAD") next.method = "GET";
                next.body.clear();
                std::erase_if(next.headers, [](const auto& header) {
                    return security::iequals(header.first, "Content-Type") || security::iequals(header.first, "Content-Length");
                });
            }
            // The new NSURLRequest (a copy of the current one with the new URL / method).
            const auto new_url = urls->make(next.url);
            const auto proposed = copy_request(current, mutable_request_class);
            {
                const auto value = require<RequestState>(proposed, "NSURLRequest");
                std::lock_guard lock(value->mutex);
                b->release(std::exchange(value->url, new_url));
                value->method = next.method;
                if (next.body.empty()) value->body.reset();
            }
            const auto delegate = delegate_of(session);
            Address follow = proposed;
            if (!state->completion && responds(delegate, "URLSession:task:willPerformHTTPRedirection:newRequest:completionHandler:")) {
                std::string mime, encoding;
                content_type(head.headers, mime, encoding);
                Address url;
                { std::lock_guard lock(state->mutex); url = b->retain(require<RequestState>(current, "NSURLRequest")->url); }
                const auto redirect_response = make_response(url, &head, mime, -1, encoding);
                b->release(url);
                auto decision = std::make_shared<Decision>();
                const auto block = decision_block(decision, Handler::object);
                deliver(session, [this, delegate, session_object, task, redirect_response, proposed, block] {
                    CallRegs registers;
                    registers.x[2] = session_object; registers.x[3] = task; registers.x[4] = redirect_response;
                    registers.x[5] = proposed; registers.x[6] = block;
                    static_cast<void>(b->objc.send(delegate, "URLSession:task:willPerformHTTPRedirection:newRequest:completionHandler:", registers));
                    b->release(redirect_response);
                });
                if (!await(decision, transfer)) { b->release(proposed); fail_task(url_error::cancelled, "cancelled"); return; }
                follow = decision->object;   // +1 or 0
                b->release(proposed);
                if (follow) {
                    const auto value = b->objc.find_host_state<RequestState>(follow);
                    const auto parts = value && value->url ? urls->parts(value->url) : std::nullopt;
                    if (!parts) { b->release(follow); fail_task(url_error::bad_url, "unsupported URL"); return; }
                    std::lock_guard lock(value->mutex);
                    next.url = *parts;
                    next.method = value->method;
                    next.body = value->body.value_or(Bytes{});
                }
            }
            if (follow) {
                {
                    std::lock_guard lock(state->mutex);
                    b->release(std::exchange(state->current, follow));
                }
                b->release(std::exchange(current, b->retain(follow)));
                request = std::move(next);
                continue;
            }
        }
        break;
    }

    // The response.
    std::string mime, encoding;
    content_type(head.headers, mime, encoding);
    std::int64_t expected = -1;
    if (const auto length = header_value(head.headers, "Content-Length"); length && !header_value(head.headers, "Content-Encoding")) {
        try { expected = std::stoll(*length); } catch (...) {}
    }
    if (mime.empty()) mime = "application/octet-stream";
    Address response_url;
    { std::lock_guard lock(state->mutex); response_url = b->retain(require<RequestState>(state->current, "NSURLRequest")->url); }
    const auto response = make_response(response_url, &head, mime, expected, encoding);
    b->release(response_url);
    {
        std::lock_guard lock(state->mutex);
        b->release(std::exchange(state->response, response));
    }
    state->expected_receive = expected;
    const auto delegate = delegate_of(session);
    if (state->kind != TaskKind::download && !state->completion && responds(delegate, "URLSession:dataTask:didReceiveResponse:completionHandler:")) {
        auto decision = std::make_shared<Decision>();
        const auto block = decision_block(decision, Handler::value);
        b->retain(response);
        deliver(session, [this, delegate, session_object, task, response, block] {
            CallRegs registers;
            registers.x[2] = session_object; registers.x[3] = task; registers.x[4] = response; registers.x[5] = block;
            static_cast<void>(b->objc.send(delegate, "URLSession:dataTask:didReceiveResponse:completionHandler:", registers));
            b->release(response);
        });
        if (!await(decision, transfer)) { fail_task(url_error::cancelled, "cancelled"); return; }
        b->release(decision->object);
        if (decision->value == 0) { fail_task(url_error::cancelled, "cancelled"); return; }   // NSURLSessionResponseCancel
        if (decision->value != 1) {
            fail_task(url_error::unknown, "unsupported NSURLSessionResponseDisposition " + std::to_string(decision->value) +
                      " (only allow and cancel are provided)");
            return;
        }
    }
    if (state->upload && responds(delegate, "URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:") && !state->completion) {
        const auto total = state->sent.load();
        deliver(session, [this, delegate, session_object, task, total] {
            CallRegs registers;
            registers.x[2] = session_object; registers.x[3] = task;
            registers.x[4] = static_cast<Address>(total); registers.x[5] = static_cast<Address>(total); registers.x[6] = static_cast<Address>(total);
            static_cast<void>(b->objc.send(delegate, "URLSession:task:didSendBodyData:totalBytesSent:totalBytesExpectedToSend:", registers));
        });
    }

    // The body.
    Bytes body;
    std::filesystem::path file;
    std::ofstream output;
    if (state->kind == TaskKind::download) {
        const auto name = host::files::create_temporary("CFN");
        if (!name) { fail_task(url_error::cannot_create_file, "cannot create the download file"); return; }
        file = utf8_path(*name);
        output.open(file, std::ios::binary | std::ios::trunc);
        if (!output) { fail_task(url_error::cannot_create_file, "cannot create the download file"); return; }
    }
    const bool stream_data = state->kind != TaskKind::download && !state->completion &&
        responds(delegate, "URLSession:dataTask:didReceiveData:");
    const bool progress = state->kind == TaskKind::download && !state->completion &&
        responds(delegate, "URLSession:downloadTask:didWriteData:totalBytesWritten:totalBytesExpectedToWrite:");
    std::vector<std::uint8_t> buffer(64 * 1024);
    for (;;) {
        {
            std::unique_lock lock(transfer->mutex);
            wait(lock, transfer->cv, [&] { return !transfer->suspended || transfer->cancelled; }, nullptr);
            if (transfer->cancelled) {
                lock.unlock();
                output.close();
                std::error_code ignored;
                if (!file.empty()) std::filesystem::remove(file, ignored);
                fail_task(url_error::cancelled, "cancelled");
                return;
            }
        }
        std::optional<HttpFailure> failure;
        const auto count = exchange->read(buffer.data(), buffer.size(), failure);
        if (failure) {
            output.close();
            std::error_code ignored;
            if (!file.empty()) std::filesystem::remove(file, ignored);
            fail_task(failure->code, failure->description);
            return;
        }
        if (count == 0) break;
        state->received += static_cast<std::int64_t>(count);
        if (state->kind == TaskKind::download) {
            output.write(reinterpret_cast<const char*>(buffer.data()), static_cast<std::streamsize>(count));
            if (!output) { fail_task(url_error::cannot_write_file, "cannot write the download file"); return; }
            if (progress) {
                const auto written = static_cast<Address>(state->received.load()), expected_total = static_cast<Address>(state->expected_receive.load());
                deliver(session, [this, delegate, session_object, task, count, written, expected_total] {
                    CallRegs registers;
                    registers.x[2] = session_object; registers.x[3] = task; registers.x[4] = count;
                    registers.x[5] = written; registers.x[6] = expected_total;
                    static_cast<void>(b->objc.send(delegate, "URLSession:downloadTask:didWriteData:totalBytesWritten:totalBytesExpectedToWrite:", registers));
                });
            }
        } else if (stream_data) {
            Bytes chunk(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
            deliver(session, [this, delegate, session_object, task, chunk = std::move(chunk)] {
                const auto data = b->data(chunk);
                CallRegs registers;
                registers.x[2] = session_object; registers.x[3] = task; registers.x[4] = data;
                static_cast<void>(b->objc.send(delegate, "URLSession:dataTask:didReceiveData:", registers));
                b->release(data);
            });
        } else {
            body.insert(body.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }
    output.close();
    {
        std::lock_guard lock(transfer->mutex);
        transfer->exchange.reset();
    }
    finish(session, session_object, task, state, 0, std::move(body), file);
}

} // namespace

// ---- installation --------------------------------------------------------------------------------------

void install_url_loading(const std::shared_ptr<Urls>& urls) {
    auto self = std::make_shared<Loading>();
    self->urls = urls;
    self->b = urls->b;
    self->sec = security::security();
    auto& b = *self->b;
    const auto object = b.object_class;
    const auto define = [&](const char* name, Address super) { return b.define_class(name, super, foundation); };
    self->request_class = define("NSURLRequest", object);
    self->mutable_request_class = define("NSMutableURLRequest", self->request_class);
    self->response_class = define("NSURLResponse", object);
    self->http_response_class = define("NSHTTPURLResponse", self->response_class);
    self->configuration_class = define("NSURLSessionConfiguration", object);
    self->session_class = define("NSURLSession", object);
    self->task_class = define("NSURLSessionTask", object);
    self->data_task_class = define("NSURLSessionDataTask", self->task_class);
    self->upload_task_class = define("NSURLSessionUploadTask", self->data_task_class);
    self->download_task_class = define("NSURLSessionDownloadTask", self->task_class);
    self->challenge_class = define("NSURLAuthenticationChallenge", object);
    self->space_class = define("NSURLProtectionSpace", object);
    self->credential_class = define("NSURLCredential", object);
    self->cookie_storage_class = define("NSHTTPCookieStorage", object);

    // Constants.
    const std::pair<const char*, const char*> strings[] = {
        {"NSURLErrorDomain", "NSURLErrorDomain"}, {"NSURLErrorFailingURLErrorKey", "NSErrorFailingURLKey"},
        {"NSURLErrorFailingURLStringErrorKey", "NSErrorFailingURLStringKey"}, {"NSErrorFailingURLStringKey", "NSErrorFailingURLStringKey"},
        {"NSURLErrorFailingURLPeerTrustErrorKey", "NSURLErrorFailingURLPeerTrustErrorKey"},
        {"NSURLErrorBackgroundTaskCancelledReasonKey", "NSURLErrorBackgroundTaskCancelledReasonKey"},
        {"NSURLSessionDownloadTaskResumeData", "NSURLSessionDownloadTaskResumeData"},
        {"NSURLAuthenticationMethodDefault", "NSURLAuthenticationMethodDefault"},
        {"NSURLAuthenticationMethodHTTPBasic", "NSURLAuthenticationMethodHTTPBasic"},
        {"NSURLAuthenticationMethodHTTPDigest", "NSURLAuthenticationMethodHTTPDigest"},
        {"NSURLAuthenticationMethodHTMLForm", "NSURLAuthenticationMethodHTMLForm"},
        {"NSURLAuthenticationMethodNTLM", "NSURLAuthenticationMethodNTLM"},
        {"NSURLAuthenticationMethodNegotiate", "NSURLAuthenticationMethodNegotiate"},
        {"NSURLAuthenticationMethodClientCertificate", "NSURLAuthenticationMethodClientCertificate"},
        {"NSURLAuthenticationMethodServerTrust", "NSURLAuthenticationMethodServerTrust"},
        {"NSURLProtectionSpaceHTTP", "http"}, {"NSURLProtectionSpaceHTTPS", "https"}, {"NSURLProtectionSpaceFTP", "ftp"},
        {"NSURLProtectionSpaceHTTPProxy", "http"}, {"NSURLProtectionSpaceHTTPSProxy", "https"},
        {"NSURLProtectionSpaceFTPProxy", "ftp"}, {"NSURLProtectionSpaceSOCKSProxy", "SOCKS"}};
    for (const auto& [name, value] : strings) self->export_string(name, value);
    b.export_value(foundation, "_NSURLSessionTransferSizeUnknown", static_cast<std::uint64_t>(-1));
    for (const auto& [name, value] : {std::pair{"_NSURLSessionTaskPriorityDefault", 0.5f}, std::pair{"_NSURLSessionTaskPriorityLow", 0.0f},
                                      std::pair{"_NSURLSessionTaskPriorityHigh", 1.0f}})
        b.export_value(foundation, name, std::bit_cast<std::uint32_t>(value));

    // ---- NSURLRequest / NSMutableURLRequest -------------------------------------------------------------
    const auto rq = self->request_class, mrq = self->mutable_request_class;
    const auto request = [self](Address object) { return self->require<RequestState>(object, "NSURLRequest"); };
    b.method(rq, "requestWithURL:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(self->make_request(s.x[0], s.x[2], 0, 60));
    }, true);
    b.method(rq, "requestWithURL:cachePolicy:timeoutInterval:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(self->make_request(s.x[0], s.x[2], s.x[3], d_arg(s, 0)));
    }, true, "@@:@Qd");
    b.method(rq, "init", [self](CpuState& s, GuestMemory&) { self->init_request(s.x[0], 0, 0, 60); });
    b.method(rq, "initWithURL:", [self](CpuState& s, GuestMemory&) { self->init_request(s.x[0], s.x[2], 0, 60); });
    b.method(rq, "initWithURL:cachePolicy:timeoutInterval:", [self](CpuState& s, GuestMemory&) {
        self->init_request(s.x[0], s.x[2], s.x[3], d_arg(s, 0));
    }, false, "@@:@Qd");
    b.method(rq, "dealloc", [self, rq](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<RequestState>(s.x[0])) {
            self->b->release(std::exchange(state->url, 0));
            self->b->release(std::exchange(state->main_document, 0));
        }
        static_cast<void>(self->b->objc.send_super(s.x[0], rq, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(rq, "URL", [request](CpuState& s, GuestMemory&) { const auto r = request(s.x[0]); std::lock_guard lock(r->mutex); s.x[0] = r->url; });
    b.method(rq, "mainDocumentURL", [request](CpuState& s, GuestMemory&) { const auto r = request(s.x[0]); std::lock_guard lock(r->mutex); s.x[0] = r->main_document; });
    b.method(rq, "HTTPMethod", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        std::lock_guard lock(r->mutex);
        s.x[0] = self->autoreleased_string(r->method);
    });
    b.method(rq, "allHTTPHeaderFields", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        Headers headers;
        { std::lock_guard lock(r->mutex); headers = r->headers; }
        s.x[0] = headers.empty() ? 0 : self->b->autorelease(self->dictionary_from(headers));
    });
    b.method(rq, "valueForHTTPHeaderField:", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        const auto name = self->b->text(s.x[2]);
        std::lock_guard lock(r->mutex);
        const auto value = header_value(r->headers, name);
        s.x[0] = value ? self->autoreleased_string(*value) : 0;
    });
    b.method(rq, "HTTPBody", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        std::optional<Bytes> body;
        { std::lock_guard lock(r->mutex); body = r->body; }
        s.x[0] = body ? self->b->autorelease(self->b->data(*body)) : 0;
    });
    b.method(rq, "HTTPBodyStream", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    b.method(rq, "timeoutInterval", [request](CpuState& s, GuestMemory&) { return_double(s, request(s.x[0])->timeout); }, false, "d@:");
    b.method(rq, "cachePolicy", [request](CpuState& s, GuestMemory&) { s.x[0] = request(s.x[0])->cache_policy; }, false, "Q@:");
    b.method(rq, "networkServiceType", [request](CpuState& s, GuestMemory&) { s.x[0] = request(s.x[0])->service_type; }, false, "Q@:");
    const std::pair<const char*, bool RequestState::*> flags[] = {
        {"HTTPShouldHandleCookies", &RequestState::cookies}, {"allowsCellularAccess", &RequestState::cellular},
        {"HTTPShouldUsePipelining", &RequestState::pipelining}, {"allowsExpensiveNetworkAccess", &RequestState::expensive},
        {"allowsConstrainedNetworkAccess", &RequestState::constrained}};
    for (const auto& [name, member] : flags) {
        b.method(rq, name, [request, member](CpuState& s, GuestMemory&) { s.x[0] = (*request(s.x[0])).*member; }, false, "B@:");
        std::string setter = std::string("set") + static_cast<char>(std::toupper(name[0])) + (name + 1) + ":";
        b.method(mrq, setter, [request, member](CpuState& s, GuestMemory&) {
            const auto r = request(s.x[0]);
            std::lock_guard lock(r->mutex);
            (*r).*member = (s.x[2] & 0xff) != 0;
        }, false, "v@:B");
    }
    b.method(rq, "copyWithZone:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->is_mutable_request(s.x[0]) ? self->copy_request(s.x[0], self->request_class) : self->b->retain(s.x[0]);
    }, false, "@@:^v");
    b.method(rq, "mutableCopyWithZone:", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->copy_request(s.x[0], self->mutable_request_class);
    }, false, "@@:^v");
    b.method(rq, "description", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        Address url;
        { std::lock_guard lock(r->mutex); url = r->url; }
        s.x[0] = self->autoreleased_string("<NSURLRequest> { URL: " + (url ? self->urls->text(url) : std::string("(null)")) + " }");
    });
    b.method(mrq, "setURL:", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        const auto value = self->b->retain(s.x[2]);
        std::lock_guard lock(r->mutex);
        self->b->release(std::exchange(r->url, value));
    }, false, "v@:@");
    b.method(mrq, "setMainDocumentURL:", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        const auto value = self->b->retain(s.x[2]);
        std::lock_guard lock(r->mutex);
        self->b->release(std::exchange(r->main_document, value));
    }, false, "v@:@");
    b.method(mrq, "setHTTPMethod:", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        auto method = s.x[2] ? self->b->text(s.x[2]) : std::string("GET");
        std::lock_guard lock(r->mutex);
        r->method = method.empty() ? "GET" : method;
    }, false, "v@:@");
    b.method(mrq, "setAllHTTPHeaderFields:", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        auto headers = self->headers_from(s.x[2]);
        std::lock_guard lock(r->mutex);
        r->headers.clear();
        for (const auto& [name, value] : headers) Loading::set_header(r->headers, name, value, false);
    }, false, "v@:@");
    for (const auto append : {false, true})
        b.method(mrq, append ? "addValue:forHTTPHeaderField:" : "setValue:forHTTPHeaderField:", [self, request, append](CpuState& s, GuestMemory&) {
            const auto r = request(s.x[0]);
            const auto name = self->b->text(s.x[3]);
            const auto value = self->b->maybe_text(s.x[2]);
            std::lock_guard lock(r->mutex);
            if (!value) {
                if (!append) std::erase_if(r->headers, [&](const auto& header) { return security::iequals(header.first, name); });
                return;
            }
            Loading::set_header(r->headers, name, *value, append);
        }, false, "v@:@@");
    b.method(mrq, "setHTTPBody:", [self, request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        std::optional<Bytes> body;
        if (s.x[2]) body = self->b->bytes(s.x[2]);
        std::lock_guard lock(r->mutex);
        r->body = std::move(body);
    }, false, "v@:@");
    b.method(mrq, "setHTTPBodyStream:", [](CpuState& s, GuestMemory&) {
        if (s.x[2]) security::unsupported("NSURLRequest HTTPBodyStream uploads");
    }, false, "v@:@");
    b.method(mrq, "setTimeoutInterval:", [request](CpuState& s, GuestMemory&) {
        const auto r = request(s.x[0]);
        std::lock_guard lock(r->mutex);
        r->timeout = d_arg(s, 0);
    }, false, "v@:d");
    b.method(mrq, "setCachePolicy:", [request](CpuState& s, GuestMemory&) { request(s.x[0])->cache_policy = s.x[2]; }, false, "v@:Q");
    b.method(mrq, "setNetworkServiceType:", [request](CpuState& s, GuestMemory&) { request(s.x[0])->service_type = s.x[2]; }, false, "v@:Q");

    // ---- NSURLResponse / NSHTTPURLResponse -----------------------------------------------------------------
    const auto rs = self->response_class, hrs = self->http_response_class;
    const auto response = [self](Address object) { return self->require<ResponseState>(object, "NSURLResponse"); };
    b.method(rs, "initWithURL:MIMEType:expectedContentLength:textEncodingName:", [self](CpuState& s, GuestMemory&) {
        auto state = std::make_shared<ResponseState>();
        state->url = self->b->retain(s.x[2]);
        state->mime = s.x[3] ? self->b->text(s.x[3]) : std::string{};
        state->expected = static_cast<std::int64_t>(s.x[4]);
        state->encoding = s.x[5] ? self->b->text(s.x[5]) : std::string{};
        static_cast<void>(self->b->objc.host_state<ResponseState>(s.x[0], [&] { return state; }));
    }, false, "@@:@@q@");
    b.method(hrs, "initWithURL:statusCode:HTTPVersion:headerFields:", [self](CpuState& s, GuestMemory&) {
        auto state = std::make_shared<ResponseState>();
        state->url = self->b->retain(s.x[2]);
        state->http = true;
        state->status = static_cast<std::int64_t>(s.x[3]);
        state->version = s.x[4] ? self->b->text(s.x[4]) : std::string("HTTP/1.1");
        state->headers = self->headers_from(s.x[5]);
        Loading::content_type(state->headers, state->mime, state->encoding);
        if (const auto length = header_value(state->headers, "Content-Length")) try { state->expected = std::stoll(*length); } catch (...) {}
        static_cast<void>(self->b->objc.host_state<ResponseState>(s.x[0], [&] { return state; }));
    }, false, "@@:@q@@");
    b.method(rs, "dealloc", [self, rs](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<ResponseState>(s.x[0])) self->b->release(std::exchange(state->url, 0));
        static_cast<void>(self->b->objc.send_super(s.x[0], rs, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(rs, "URL", [response](CpuState& s, GuestMemory&) { s.x[0] = response(s.x[0])->url; });
    b.method(rs, "MIMEType", [self, response](CpuState& s, GuestMemory&) {
        const auto r = response(s.x[0]);
        s.x[0] = r->mime.empty() ? 0 : self->autoreleased_string(r->mime);
    });
    b.method(rs, "textEncodingName", [self, response](CpuState& s, GuestMemory&) {
        const auto r = response(s.x[0]);
        s.x[0] = r->encoding.empty() ? 0 : self->autoreleased_string(r->encoding);
    });
    b.method(rs, "expectedContentLength", [response](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(response(s.x[0])->expected); }, false, "q@:");
    b.method(rs, "suggestedFilename", [self, response](CpuState& s, GuestMemory&) {
        const auto r = response(s.x[0]);
        std::string name;
        if (const auto disposition = header_value(r->headers, "Content-Disposition")) {
            const auto lowered = security::lower(*disposition);
            if (const auto at = lowered.find("filename="); at != std::string::npos) {
                name = disposition->substr(at + 9);
                name = name.substr(0, name.find(';'));
                name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
            }
        }
        if (name.empty() && r->url) {
            if (const auto parts = self->urls->parts(r->url)) {
                auto path = percent_decode(parts->path);
                while (!path.empty() && path.back() == '/') path.pop_back();
                name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
            }
        }
        if (name.empty()) name = "Unknown";
        s.x[0] = self->autoreleased_string(name);
    });
    b.method(rs, "copyWithZone:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->retain(s.x[0]); }, false, "@@:^v");
    b.method(hrs, "statusCode", [response](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(response(s.x[0])->status); }, false, "q@:");
    b.method(hrs, "allHeaderFields", [self, response](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(self->dictionary_from(response(s.x[0])->headers));
    });
    b.method(hrs, "valueForHTTPHeaderField:", [self, response](CpuState& s, GuestMemory&) {
        const auto value = header_value(response(s.x[0])->headers, self->b->text(s.x[2]));
        s.x[0] = value ? self->autoreleased_string(*value) : 0;
    });
    b.method(hrs, "localizedStringForStatusCode:", [self](CpuState& s, GuestMemory&) {
        static const std::map<std::int64_t, const char*> names{
            {200, "no error"}, {201, "created"}, {202, "accepted"}, {204, "no content"}, {206, "partial content"},
            {301, "moved permanently"}, {302, "found"}, {303, "see other"}, {304, "not modified"}, {307, "temporarily redirected"},
            {308, "permanent redirect"}, {400, "bad request"}, {401, "unauthorized"}, {403, "forbidden"}, {404, "not found"},
            {405, "method not allowed"}, {408, "request timed out"}, {409, "conflict"}, {410, "no longer exists"},
            {429, "too many requests"}, {500, "internal server error"}, {501, "unimplemented"}, {502, "bad gateway"},
            {503, "service unavailable"}, {504, "gateway timed out"}};
        const auto code = static_cast<std::int64_t>(s.x[2]);
        const auto found = names.find(code);
        const char* text = found != names.end() ? found->second : code < 200 ? "informational" : code < 300 ? "success"
            : code < 400 ? "redirected" : code < 500 ? "client error" : "server error";
        s.x[0] = self->autoreleased_string(text);
    }, true, "@@:q");

    // ---- NSURLSessionConfiguration ---------------------------------------------------------------------------
    const auto cfg = self->configuration_class;
    const auto configuration = [self](Address object) { return self->require<ConfigurationState>(object, "NSURLSessionConfiguration"); };
    const auto make_configuration = [self](bool ephemeral) {
        const auto object = self->b->objc.allocate(self->configuration_class);
        auto state = std::make_shared<ConfigurationState>();
        state->ephemeral = ephemeral;
        auto& p = state->properties;
        p.reals["timeoutIntervalForRequest"] = 60;
        p.reals["timeoutIntervalForResource"] = 604800;
        p.integers["HTTPShouldSetCookies"] = 1;
        p.integers["HTTPCookieAcceptPolicy"] = 2;
        p.integers["allowsCellularAccess"] = 1;
        p.integers["allowsExpensiveNetworkAccess"] = 1;
        p.integers["allowsConstrainedNetworkAccess"] = 1;
        p.integers["HTTPMaximumConnectionsPerHost"] = 6;
        p.integers["TLSMinimumSupportedProtocolVersion"] = 769;   // tls_protocol_version_TLSv10
        p.integers["TLSMaximumSupportedProtocolVersion"] = 772;   // TLSv13
        static_cast<void>(self->b->objc.host_state<ConfigurationState>(object, [&] { return state; }));
        return object;
    };
    b.method(cfg, "defaultSessionConfiguration", [self, make_configuration](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(make_configuration(false));
    }, true);
    b.method(cfg, "ephemeralSessionConfiguration", [self, make_configuration](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(make_configuration(true));
    }, true);
    for (const auto* name : {"backgroundSessionConfigurationWithIdentifier:", "backgroundSessionConfiguration:"})
        b.method(cfg, name, [](CpuState&, GuestMemory&) { security::unsupported("background NSURLSession configurations"); }, true);
    b.method(cfg, "dealloc", [self, cfg](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<ConfigurationState>(s.x[0])) {
            std::vector<Address> objects;
            { std::lock_guard lock(state->properties.mutex); for (auto& [name, value] : state->properties.objects) objects.push_back(value); state->properties.objects.clear(); }
            self->b->release_all(objects);
        }
        static_cast<void>(self->b->objc.send_super(s.x[0], cfg, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(cfg, "copyWithZone:", [self, configuration](CpuState& s, GuestMemory&) {
        const auto from = configuration(s.x[0]);
        const auto object = self->b->objc.allocate(self->configuration_class);
        auto state = std::make_shared<ConfigurationState>();
        {
            std::lock_guard lock(from->properties.mutex);
            state->ephemeral = from->ephemeral;
            state->properties.integers = from->properties.integers;
            state->properties.reals = from->properties.reals;
            for (const auto& [name, value] : from->properties.objects) state->properties.objects[name] = self->b->retain(value);
        }
        static_cast<void>(self->b->objc.host_state<ConfigurationState>(object, [&] { return state; }));
        s.x[0] = object;
    }, false, "@@:^v");
    const auto setter_name = [](std::string_view name) {
        return "set" + std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])))) + std::string(name.substr(1)) + ":";
    };
    for (const auto* name : {"HTTPAdditionalHeaders", "HTTPCookieStorage", "URLCache", "URLCredentialStorage", "connectionProxyDictionary",
                             "identifier", "sharedContainerIdentifier", "protocolClasses"}) {
        const std::string key(name);
        b.method(cfg, name, [configuration, key](CpuState& s, GuestMemory&) {
            const auto state = configuration(s.x[0]);
            std::lock_guard lock(state->properties.mutex);
            const auto found = state->properties.objects.find(key);
            s.x[0] = found == state->properties.objects.end() ? 0 : found->second;
        });
        b.method(cfg, setter_name(name), [self, configuration, key](CpuState& s, GuestMemory&) {
            if (key == "protocolClasses" && s.x[2] && self->b->cf.length(s.x[2]) > 0) security::unsupported("custom NSURLProtocol classes");
            const auto state = configuration(s.x[0]);
            const auto value = s.x[2] ? self->b->retain(s.x[2]) : 0;
            std::lock_guard lock(state->properties.mutex);
            auto& slot = state->properties.objects[key];
            self->b->release(std::exchange(slot, value));
        }, false, "v@:@");
    }
    for (const auto* name : {"HTTPShouldSetCookies", "allowsCellularAccess", "allowsExpensiveNetworkAccess", "allowsConstrainedNetworkAccess",
                             "waitsForConnectivity", "HTTPShouldUsePipelining", "sessionSendsLaunchEvents", "isDiscretionary"}) {
        const std::string key(name);
        b.method(cfg, name, [configuration, key](CpuState& s, GuestMemory&) {
            const auto state = configuration(s.x[0]);
            std::lock_guard lock(state->properties.mutex);
            s.x[0] = state->properties.integers[key] != 0;
        }, false, "B@:");
        const auto setter = key == "isDiscretionary" ? std::string("setDiscretionary:") : setter_name(name);
        b.method(cfg, setter, [configuration, key](CpuState& s, GuestMemory&) {
            const auto state = configuration(s.x[0]);
            std::lock_guard lock(state->properties.mutex);
            state->properties.integers[key] = s.x[2] & 0xff;
        }, false, "v@:B");
    }
    for (const auto* name : {"requestCachePolicy", "HTTPCookieAcceptPolicy", "HTTPMaximumConnectionsPerHost", "networkServiceType",
                             "TLSMinimumSupportedProtocolVersion", "TLSMaximumSupportedProtocolVersion", "TLSMinimumSupportedProtocol",
                             "TLSMaximumSupportedProtocol", "multipathServiceType"}) {
        const std::string key(name);
        b.method(cfg, name, [configuration, key](CpuState& s, GuestMemory&) {
            const auto state = configuration(s.x[0]);
            std::lock_guard lock(state->properties.mutex);
            s.x[0] = state->properties.integers[key];
        }, false, "Q@:");
        b.method(cfg, setter_name(name), [configuration, key](CpuState& s, GuestMemory&) {
            const auto state = configuration(s.x[0]);
            std::lock_guard lock(state->properties.mutex);
            state->properties.integers[key] = s.x[2];
        }, false, "v@:Q");
    }
    for (const auto* name : {"timeoutIntervalForRequest", "timeoutIntervalForResource"}) {
        const std::string key(name);
        b.method(cfg, name, [configuration, key](CpuState& s, GuestMemory&) {
            const auto state = configuration(s.x[0]);
            std::lock_guard lock(state->properties.mutex);
            return_double(s, state->properties.reals[key]);
        }, false, "d@:");
        b.method(cfg, setter_name(name), [configuration, key](CpuState& s, GuestMemory&) {
            const auto state = configuration(s.x[0]);
            std::lock_guard lock(state->properties.mutex);
            state->properties.reals[key] = d_arg(s, 0);
        }, false, "v@:d");
    }

    // ---- NSURLSession ----------------------------------------------------------------------------------------
    const auto ss = self->session_class;
    const auto make_session = [self, configuration](Address config, Address delegate, Address queue, bool shared) {
        const auto object = shared ? self->b->process.objects().create(16, self->session_class, ObjectKind::instance, {}, {}, true)
                                   : self->b->objc.allocate(self->session_class);
        auto state = std::make_shared<SessionState>();
        state->shared = shared;
        state->configuration = self->b->objc.send(config, "copy").x0;
        const auto cs = configuration(state->configuration);
        {
            std::lock_guard lock(cs->properties.mutex);
            state->ephemeral = cs->ephemeral;
            state->request_timeout = cs->properties.reals["timeoutIntervalForRequest"];
            state->resource_timeout = cs->properties.reals["timeoutIntervalForResource"];
            state->cookies = cs->properties.integers["HTTPShouldSetCookies"] != 0;
            if (const auto found = cs->properties.objects.find("HTTPAdditionalHeaders"); found != cs->properties.objects.end())
                state->additional = self->headers_from(found->second);
            if (const auto found = cs->properties.objects.find("connectionProxyDictionary"); found != cs->properties.objects.end() && found->second &&
                self->b->cf.length(found->second) > 0)
                security::unsupported("NSURLSessionConfiguration connectionProxyDictionary (the system proxy settings are used)");
        }
        state->delegate = self->b->retain(delegate);
        state->delegate_queue = queue ? self->b->retain(queue) : make_serial_operation_queue(*self->b, "NSURLSession delegate queue");
        state->dispatch = dispatch_queue_of(*self->b, state->delegate_queue);
        if (!state->dispatch) security::unsupported("NSURLSession delegate queues that are not NSOperationQueue / dispatch backed");
        static_cast<void>(self->b->objc.host_state<SessionState>(object, [&] { return state; }));
        // A session with a delegate keeps itself alive until it is invalidated (as on macOS).
        if (delegate && !shared) { self->b->retain(object); state->self_retained = true; }
        return object;
    };
    b.method(ss, "sharedSession", [self, make_session](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex);
        if (!self->shared_session) {
            const auto config = self->b->objc.send(self->configuration_class, "defaultSessionConfiguration").x0;
            self->shared_session = make_session(config, 0, 0, true);
        }
        s.x[0] = self->shared_session;
    }, true);
    b.method(ss, "sessionWithConfiguration:", [self, make_session](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(make_session(s.x[2], 0, 0, false));
    }, true);
    b.method(ss, "sessionWithConfiguration:delegate:delegateQueue:", [self, make_session](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(make_session(s.x[2], s.x[3], s.x[4], false));
    }, true, "@@:@@@");
    const auto session = [self](Address object) { return self->require<SessionState>(object, "NSURLSession"); };
    b.method(ss, "dealloc", [self, ss](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<SessionState>(s.x[0])) {
            std::vector<Address> objects;
            {
                std::lock_guard lock(state->mutex);
                objects = {std::exchange(state->configuration, 0), std::exchange(state->delegate, 0),
                           std::exchange(state->delegate_queue, 0), std::exchange(state->description, 0)};
            }
            self->b->release_all(objects);
        }
        static_cast<void>(self->b->objc.send_super(s.x[0], ss, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(ss, "configuration", [self, session](CpuState& s, GuestMemory&) {
        s.x[0] = self->b->autorelease(self->b->objc.send(session(s.x[0])->configuration, "copy").x0);
    });
    b.method(ss, "delegate", [session](CpuState& s, GuestMemory&) { const auto v = session(s.x[0]); std::lock_guard lock(v->mutex); s.x[0] = v->delegate; });
    b.method(ss, "delegateQueue", [session](CpuState& s, GuestMemory&) { s.x[0] = session(s.x[0])->delegate_queue; });
    b.method(ss, "sessionDescription", [session](CpuState& s, GuestMemory&) { s.x[0] = session(s.x[0])->description; });
    b.method(ss, "setSessionDescription:", [self, session](CpuState& s, GuestMemory&) {
        const auto v = session(s.x[0]);
        const auto copy = s.x[2] ? self->b->cf.copy(s.x[2]) : 0;
        std::lock_guard lock(v->mutex);
        self->b->release(std::exchange(v->description, copy));
    }, false, "v@:@");
    // Task factories: (url or request [, data / file] [, completionHandler]).
    struct Factory { const char* selector; TaskKind kind; bool url; int body; bool completion; const char* types; };
    const Factory factories[] = {
        {"dataTaskWithURL:", TaskKind::data, true, 0, false, "@@:@"},
        {"dataTaskWithRequest:", TaskKind::data, false, 0, false, "@@:@"},
        {"dataTaskWithURL:completionHandler:", TaskKind::data, true, 0, true, "@@:@@?"},
        {"dataTaskWithRequest:completionHandler:", TaskKind::data, false, 0, true, "@@:@@?"},
        {"uploadTaskWithRequest:fromData:", TaskKind::upload, false, 1, false, "@@:@@"},
        {"uploadTaskWithRequest:fromData:completionHandler:", TaskKind::upload, false, 1, true, "@@:@@@?"},
        {"uploadTaskWithRequest:fromFile:", TaskKind::upload, false, 2, false, "@@:@@"},
        {"uploadTaskWithRequest:fromFile:completionHandler:", TaskKind::upload, false, 2, true, "@@:@@@?"},
        {"downloadTaskWithURL:", TaskKind::download, true, 0, false, "@@:@"},
        {"downloadTaskWithRequest:", TaskKind::download, false, 0, false, "@@:@"},
        {"downloadTaskWithURL:completionHandler:", TaskKind::download, true, 0, true, "@@:@@?"},
        {"downloadTaskWithRequest:completionHandler:", TaskKind::download, false, 0, true, "@@:@@?"}};
    for (const auto& factory : factories) {
        b.method(ss, factory.selector, [self, factory](CpuState& s, GuestMemory&) {
            Address request = s.x[2];
            Address temporary = 0;
            if (factory.url) {
                if (!s.x[2]) security::fail("NSURLSession task with a nil URL");
                temporary = request = self->make_request(self->request_class, s.x[2], 0, 60);
            }
            std::optional<Bytes> upload;
            if (factory.body == 1) upload = s.x[3] ? self->b->bytes(s.x[3]) : Bytes{};
            if (factory.body == 2) {
                const auto path = self->b->cf.url_path(s.x[3]);
                const auto host = self->b->process.map_guest_path ? self->b->process.map_guest_path(path) : utf8_path(path);
                std::ifstream file(host, std::ios::binary);
                if (!file) security::fail("cannot read the upload file " + path);
                upload = Bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            }
            const auto completion = factory.completion ? s.x[factory.body ? 4 : 3] : 0;
            Address task;
            try { task = self->create_task(s.x[0], factory.kind, request, completion, std::move(upload)); }
            catch (...) { self->b->release(temporary); throw; }
            self->b->release(temporary);
            s.x[0] = self->b->autorelease(task);
        }, false, factory.types);
    }
    const auto cancel_task = [self](Address task) {
        const auto state = self->require<TaskState>(task, "NSURLSessionTask");
        std::shared_ptr<HttpExchange> exchange;
        bool started;
        {
            std::lock_guard lock(state->transfer->mutex);
            if (state->transfer->cancelled) return;
            state->transfer->cancelled = true;
            exchange = state->transfer->exchange;
        }
        state->transfer->cv.notify_all();
        { std::lock_guard lock(state->mutex); started = state->started; }
        if (state->status.load() == static_cast<std::int64_t>(TaskStatus::completed)) return;
        state->status = static_cast<std::int64_t>(TaskStatus::canceling);
        if (exchange) exchange->cancel();
        if (!started) {
            // Never resumed: complete right away with NSURLErrorCancelled.
            const auto session = state->session_state.lock();
            if (!session) return;
            {
                std::lock_guard lock(session->mutex);
                if (!session->tasks.insert(task).second) return;
            }
            self->b->retain(task);
            Address url;
            { std::lock_guard lock(state->mutex); url = self->b->retain(self->require<RequestState>(state->current, "NSURLRequest")->url); }
            const auto error = self->url_error(url_error::cancelled, "cancelled", url);
            self->b->release(url);
            self->finish(session, state->session, task, state, error, {}, {});
        }
    };
    b.method(ss, "invalidateAndCancel", [self, session, cancel_task](CpuState& s, GuestMemory&) {
        const auto state = session(s.x[0]);
        if (state->shared) return;   // the shared session cannot be invalidated
        std::vector<Address> tasks;
        bool idle;
        {
            std::lock_guard lock(state->mutex);
            if (state->finishing || state->invalidated) return;
            state->finishing = true;
            tasks.assign(state->tasks.begin(), state->tasks.end());
            idle = tasks.empty();
        }
        for (const auto task : tasks) cancel_task(task);
        if (idle) {
            const auto object = s.x[0];
            self->deliver(state, [self, state, object] { self->become_invalid(state, object); });
        }
    }, false, "v@:");
    b.method(ss, "finishTasksAndInvalidate", [self, session](CpuState& s, GuestMemory&) {
        const auto state = session(s.x[0]);
        if (state->shared) return;
        bool idle;
        {
            std::lock_guard lock(state->mutex);
            if (state->finishing || state->invalidated) return;
            state->finishing = true;
            idle = state->tasks.empty();
        }
        if (idle) {
            const auto object = s.x[0];
            self->deliver(state, [self, state, object] { self->become_invalid(state, object); });
        }
    }, false, "v@:");
    b.method(ss, "getTasksWithCompletionHandler:", [self, session](CpuState& s, GuestMemory&) {
        const auto state = session(s.x[0]);
        std::vector<Address> data, upload, download;
        {
            std::lock_guard lock(state->mutex);
            for (const auto task : state->tasks) {
                const auto value = self->b->objc.find_host_state<TaskState>(task);
                (value->kind == TaskKind::download ? download : value->kind == TaskKind::upload ? upload : data).push_back(task);
            }
        }
        const auto block = self->b->block_copy(s.x[2]);
        const auto arrays = std::vector<Address>{self->b->array(data), self->b->array(upload), self->b->array(download)};
        self->deliver(state, [self, block, arrays] {
            self->b->invoke_block(block, {arrays[0], arrays[1], arrays[2]});
            self->b->block_release(block);
            self->b->release_all(arrays);
        });
    }, false, "v@:@?");

    // ---- tasks -------------------------------------------------------------------------------------------------
    const auto tk = self->task_class;
    const auto task_state = [self](Address object) { return self->require<TaskState>(object, "NSURLSessionTask"); };
    b.method(tk, "dealloc", [self, tk](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<TaskState>(s.x[0])) {
            std::vector<Address> objects;
            {
                std::lock_guard lock(state->mutex);
                objects = {std::exchange(state->session, 0), std::exchange(state->original, 0), std::exchange(state->current, 0),
                           std::exchange(state->response, 0), std::exchange(state->error, 0), std::exchange(state->description, 0)};
            }
            if (const auto completion = std::exchange(state->completion, 0)) self->b->block_release(completion);
            self->b->release_all(objects);
        }
        static_cast<void>(self->b->objc.send_super(s.x[0], tk, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(tk, "resume", [self, task_state](CpuState& s, GuestMemory&) {
        const auto state = task_state(s.x[0]);
        if (state->status.load() >= static_cast<std::int64_t>(TaskStatus::canceling)) return;
        {
            std::lock_guard lock(state->transfer->mutex);
            state->transfer->suspended = false;
        }
        state->transfer->cv.notify_all();
        bool start = false;
        {
            std::lock_guard lock(state->mutex);
            if (!state->started) { state->started = true; start = true; }
        }
        state->status = static_cast<std::int64_t>(TaskStatus::running);
        if (!start) return;
        const auto session = state->session_state.lock();
        if (!session) security::fail("NSURLSessionTask outlived its session");
        {
            std::lock_guard lock(session->mutex);
            session->tasks.insert(s.x[0]);
        }
        const auto task = self->b->retain(s.x[0]);   // released when the completion has been delivered
        const auto worker = self->b->call(security::libsystem_library, "_dispatch_get_global_queue", {0, 0});
        self->b->dispatch_async(worker, [self, task] { self->run(task); });
    }, false, "v@:");
    b.method(tk, "suspend", [task_state](CpuState& s, GuestMemory&) {
        const auto state = task_state(s.x[0]);
        if (state->status.load() >= static_cast<std::int64_t>(TaskStatus::canceling)) return;
        {
            std::lock_guard lock(state->transfer->mutex);
            state->transfer->suspended = true;
        }
        state->status = static_cast<std::int64_t>(TaskStatus::suspended);
    }, false, "v@:");
    b.method(tk, "cancel", [cancel_task](CpuState& s, GuestMemory&) { cancel_task(s.x[0]); }, false, "v@:");
    b.method(tk, "state", [task_state](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(task_state(s.x[0])->status.load()); }, false, "q@:");
    b.method(tk, "taskIdentifier", [task_state](CpuState& s, GuestMemory&) { s.x[0] = task_state(s.x[0])->identifier; }, false, "Q@:");
    const std::pair<const char*, Address TaskState::*> objects[] = {
        {"originalRequest", &TaskState::original}, {"currentRequest", &TaskState::current}, {"response", &TaskState::response},
        {"error", &TaskState::error}, {"taskDescription", &TaskState::description}};
    for (const auto& [name, member] : objects)
        b.method(tk, name, [task_state, member](CpuState& s, GuestMemory&) {
            const auto state = task_state(s.x[0]);
            std::lock_guard lock(state->mutex);
            s.x[0] = (*state).*member;
        });
    b.method(tk, "setTaskDescription:", [self, task_state](CpuState& s, GuestMemory&) {
        const auto state = task_state(s.x[0]);
        const auto copy = s.x[2] ? self->b->cf.copy(s.x[2]) : 0;
        std::lock_guard lock(state->mutex);
        self->b->release(std::exchange(state->description, copy));
    }, false, "v@:@");
    const std::pair<const char*, std::atomic<std::int64_t> TaskState::*> counts[] = {
        {"countOfBytesReceived", &TaskState::received}, {"countOfBytesSent", &TaskState::sent},
        {"countOfBytesExpectedToReceive", &TaskState::expected_receive}, {"countOfBytesExpectedToSend", &TaskState::expected_send}};
    for (const auto& [name, member] : counts)
        b.method(tk, name, [task_state, member](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(((*task_state(s.x[0])).*member).load()); }, false, "q@:");
    b.method(tk, "priority", [task_state](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(task_state(s.x[0])->priority)), 0};
    }, false, "f@:");
    b.method(tk, "setPriority:", [task_state](CpuState& s, GuestMemory&) {
        task_state(s.x[0])->priority = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
    }, false, "v@:f");

    // ---- challenges, protection spaces and credentials --------------------------------------------------------------
    const auto ch = self->challenge_class, sp = self->space_class, cr = self->credential_class;
    const auto challenge = [self](Address object) { return self->require<ChallengeState>(object, "NSURLAuthenticationChallenge"); };
    b.method(ch, "dealloc", [self, ch](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<ChallengeState>(s.x[0]))
            self->b->release_all({std::exchange(state->space, 0), std::exchange(state->credential, 0), std::exchange(state->failure, 0),
                                  std::exchange(state->error, 0)});
        static_cast<void>(self->b->objc.send_super(s.x[0], ch, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(ch, "protectionSpace", [challenge](CpuState& s, GuestMemory&) { s.x[0] = challenge(s.x[0])->space; });
    b.method(ch, "proposedCredential", [challenge](CpuState& s, GuestMemory&) { s.x[0] = challenge(s.x[0])->credential; });
    b.method(ch, "previousFailureCount", [challenge](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(challenge(s.x[0])->failures); }, false, "q@:");
    b.method(ch, "failureResponse", [challenge](CpuState& s, GuestMemory&) { s.x[0] = challenge(s.x[0])->failure; });
    b.method(ch, "error", [challenge](CpuState& s, GuestMemory&) { s.x[0] = challenge(s.x[0])->error; });
    b.method(ch, "sender", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });

    const auto space = [self](Address object) { return self->require<SpaceState>(object, "NSURLProtectionSpace"); };
    b.method(sp, "initWithHost:port:protocol:realm:authenticationMethod:", [self](CpuState& s, GuestMemory&) {
        auto state = std::make_shared<SpaceState>();
        state->host = self->b->text(s.x[2]);
        state->port = static_cast<std::int64_t>(s.x[3]);
        state->protocol = s.x[4] ? self->b->text(s.x[4]) : std::string{};
        state->realm = s.x[5] ? self->b->text(s.x[5]) : std::string{};
        state->method = s.x[6] ? self->b->text(s.x[6]) : std::string("NSURLAuthenticationMethodDefault");
        static_cast<void>(self->b->objc.host_state<SpaceState>(s.x[0], [&] { return state; }));
    }, false, "@@:@q@@@");
    b.method(sp, "dealloc", [self, sp](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<SpaceState>(s.x[0])) self->b->release(std::exchange(state->trust, 0));
        static_cast<void>(self->b->objc.send_super(s.x[0], sp, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    const auto space_text = [self, space](const char* name, std::string SpaceState::*member, bool nil_when_empty) {
        self->b->method(self->space_class, name, [self, space, member, nil_when_empty](CpuState& s, GuestMemory&) {
            const auto value = (*space(s.x[0])).*member;
            s.x[0] = nil_when_empty && value.empty() ? 0 : self->autoreleased_string(value);
        });
    };
    space_text("host", &SpaceState::host, false);
    space_text("protocol", &SpaceState::protocol, true);
    space_text("realm", &SpaceState::realm, true);
    space_text("authenticationMethod", &SpaceState::method, false);
    space_text("proxyType", &SpaceState::proxy_type, true);
    b.method(sp, "port", [space](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(space(s.x[0])->port); }, false, "q@:");
    b.method(sp, "serverTrust", [space](CpuState& s, GuestMemory&) { s.x[0] = space(s.x[0])->trust; });
    b.method(sp, "isProxy", [space](CpuState& s, GuestMemory&) { s.x[0] = space(s.x[0])->proxy; }, false, "B@:");
    b.method(sp, "receivesCredentialSecurely", [space](CpuState& s, GuestMemory&) {
        const auto v = space(s.x[0]);
        s.x[0] = v->protocol == "https" || v->method == "NSURLAuthenticationMethodHTTPDigest" || v->method == "NSURLAuthenticationMethodNTLM" ||
            v->method == "NSURLAuthenticationMethodNegotiate" || v->method == "NSURLAuthenticationMethodServerTrust";
    }, false, "B@:");
    b.method(sp, "distinguishedNames", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });

    const auto credential = [self](Address object) { return self->require<CredentialState>(object, "NSURLCredential"); };
    const auto trust_credential = [self](Address object, Address trust) {
        auto state = std::make_shared<CredentialState>();
        state->trust = self->b->retain(trust);
        static_cast<void>(self->b->objc.host_state<CredentialState>(object, [&] { return state; }));
    };
    const auto user_credential = [self](Address object, Address user, Address password, std::uint64_t persistence) {
        auto state = std::make_shared<CredentialState>();
        state->user = user ? self->b->text(user) : std::string{};
        state->has_password = password != 0;
        state->password = password ? self->b->text(password) : std::string{};
        state->persistence = persistence;
        static_cast<void>(self->b->objc.host_state<CredentialState>(object, [&] { return state; }));
    };
    b.method(cr, "credentialForTrust:", [self, trust_credential](CpuState& s, GuestMemory&) {
        const auto object = self->b->objc.allocate(self->credential_class);
        trust_credential(object, s.x[2]);
        s.x[0] = self->b->autorelease(object);
    }, true);
    b.method(cr, "initWithTrust:", [trust_credential](CpuState& s, GuestMemory&) { trust_credential(s.x[0], s.x[2]); });
    b.method(cr, "credentialWithUser:password:persistence:", [self, user_credential](CpuState& s, GuestMemory&) {
        const auto object = self->b->objc.allocate(self->credential_class);
        user_credential(object, s.x[2], s.x[3], s.x[4]);
        s.x[0] = self->b->autorelease(object);
    }, true, "@@:@@Q");
    b.method(cr, "initWithUser:password:persistence:", [user_credential](CpuState& s, GuestMemory&) {
        user_credential(s.x[0], s.x[2], s.x[3], s.x[4]);
    }, false, "@@:@@Q");
    for (const auto* name : {"credentialWithIdentity:certificates:persistence:", "initWithIdentity:certificates:persistence:"})
        b.method(cr, name, [](CpuState&, GuestMemory&) { security::unsupported("client certificate (identity) URL credentials"); },
                 std::string_view(name).starts_with("credential"));
    b.method(cr, "dealloc", [self, cr](CpuState& s, GuestMemory&) {
        if (const auto state = self->b->objc.find_host_state<CredentialState>(s.x[0])) self->b->release(std::exchange(state->trust, 0));
        static_cast<void>(self->b->objc.send_super(s.x[0], cr, self->b->objc.selector("dealloc")));
    }, false, "v@:");
    b.method(cr, "user", [self, credential](CpuState& s, GuestMemory&) {
        const auto v = credential(s.x[0]);
        s.x[0] = v->trust ? 0 : self->autoreleased_string(v->user);
    });
    b.method(cr, "password", [self, credential](CpuState& s, GuestMemory&) {
        const auto v = credential(s.x[0]);
        s.x[0] = v->has_password ? self->autoreleased_string(v->password) : 0;
    });
    b.method(cr, "hasPassword", [credential](CpuState& s, GuestMemory&) { s.x[0] = credential(s.x[0])->has_password; }, false, "B@:");
    b.method(cr, "persistence", [credential](CpuState& s, GuestMemory&) { s.x[0] = credential(s.x[0])->persistence; }, false, "Q@:");
    b.method(cr, "identity", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });
    b.method(cr, "certificates", [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->autorelease(self->b->array({})); });
    b.method(cr, "copyWithZone:", [self](CpuState& s, GuestMemory&) { s.x[0] = self->b->retain(s.x[0]); }, false, "@@:^v");

    // ---- NSHTTPCookieStorage (the shared object and its accept policy) -----------------------------------------------
    b.method(self->cookie_storage_class, "sharedHTTPCookieStorage", [self](CpuState& s, GuestMemory&) {
        std::lock_guard lock(self->mutex);
        if (!self->shared_cookie_storage)
            self->shared_cookie_storage = self->b->process.objects().create(16, self->cookie_storage_class, ObjectKind::instance, {}, {}, true);
        s.x[0] = self->shared_cookie_storage;
    }, true);
    b.method(self->cookie_storage_class, "cookieAcceptPolicy", [self](CpuState& s, GuestMemory&) { s.x[0] = self->cookie_policy; }, false, "Q@:");
    b.method(self->cookie_storage_class, "setCookieAcceptPolicy:", [self](CpuState& s, GuestMemory&) {
        if (s.x[2] > 2) security::fail("invalid NSHTTPCookieAcceptPolicy");
        self->cookie_policy = s.x[2];
    }, false, "v@:Q");

    // The provider lives as long as the process.
    static std::shared_ptr<Loading> keep;
    keep = self;
}

} // namespace alchemy::gui::network
