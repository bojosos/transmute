// Null host backend, area "http": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/http.hpp"

namespace alchemy::host::http {

Result<UserProxy> user_proxy() { return Error::unsupported; }
Result<std::string> auto_proxy(std::string_view, std::string_view, std::uint32_t) { return Error::unsupported; }

Result<Session> Session::open(std::string_view, const Proxy&) { return Error::unsupported; }
Session& Session::operator=(Session&& other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
}
Session::~Session() = default;

struct Request::State {
    Failure failure{Failure::Kind::other, "the null host backend has no HTTP stack"};
};
Request::Request(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
Request::Request(Request&&) noexcept = default;
Request& Request::operator=(Request&&) noexcept = default;
Request::~Request() = default;
Request Request::open(const Session&, const Target&, const Headers&, std::uint32_t, Trust) { return Request(std::make_unique<State>()); }
Result<Head> Request::send(ConstBytes) { return Error::unsupported; }
Result<std::size_t> Request::read(Bytes) { return Error::unsupported; }
Result<Challenge> Request::challenge(int) { return Error::unsupported; }
Status Request::set_credentials(bool, Scheme, std::string_view, std::string_view) { return Error::unsupported; }
void Request::cancel() noexcept {}
Failure Request::failure() const { return state_->failure; }

} // namespace alchemy::host::http
