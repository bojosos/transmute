// Null host backend, area "crypto": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/crypto.hpp"

namespace alchemy::host::crypto {

Status random(Bytes) { return Error::unsupported; }

void secure_zero(Bytes buffer) noexcept {
    volatile std::byte* at = buffer.data();
    for (std::size_t index = 0; index < buffer.size(); ++index) at[index] = std::byte{};
}
Result<std::vector<std::uint8_t>> protect(ConstBytes, ConstBytes, std::string_view) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> unprotect(ConstBytes, ConstBytes) { return Error::unsupported; }
Result<BlockCipher> BlockCipher::create(BlockAlgorithm, ConstBytes) { return Error::unsupported; }
BlockCipher& BlockCipher::operator=(BlockCipher&& other) noexcept {
    algorithm_ = std::exchange(other.algorithm_, nullptr);
    key_ = std::exchange(other.key_, nullptr);
    block_ = other.block_;
    return *this;
}
BlockCipher::~BlockCipher() = default;
Status BlockCipher::encrypt(ConstBytes, Bytes) const { return Error::unsupported; }
Status BlockCipher::decrypt(ConstBytes, Bytes) const { return Error::unsupported; }
Status pbkdf2(Digest, ConstBytes, ConstBytes, std::uint32_t, Bytes) { return Error::unsupported; }
bool supports(Digest) noexcept { return false; }
Result<std::vector<std::uint8_t>> hash(Digest, ConstBytes) { return Error::unsupported; }
Status check(const RsaKey&, bool) { return Error::unsupported; }
Status check(const EcKey&, bool) { return Error::unsupported; }
Result<RsaKey> generate_rsa(unsigned) { return Error::unsupported; }
Result<EcKey> generate_ec(std::size_t) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> rsa_sign(const RsaKey&, RsaPadding, std::optional<Digest>, ConstBytes) { return Error::unsupported; }
Status rsa_verify(const RsaKey&, RsaPadding, std::optional<Digest>, ConstBytes, ConstBytes) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> rsa_encrypt(const RsaKey&, RsaPadding, std::optional<Digest>, ConstBytes) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> rsa_decrypt(const RsaKey&, RsaPadding, std::optional<Digest>, ConstBytes) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> ecdsa_sign(const EcKey&, ConstBytes) { return Error::unsupported; }
Status ecdsa_verify(const EcKey&, ConstBytes, ConstBytes) { return Error::unsupported; }
Result<std::vector<std::uint8_t>> ecdh(const EcKey&, const EcKey&) { return Error::unsupported; }

Result<Certificate> Certificate::parse(ConstBytes) { return Error::unsupported; }
Certificate::Certificate(const Certificate& other) : handle_(other.handle_) {}
Certificate& Certificate::operator=(const Certificate& other) {
    handle_ = other.handle_;
    return *this;
}
Certificate& Certificate::operator=(Certificate&& other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
}
Certificate::~Certificate() = default;
std::vector<std::uint8_t> Certificate::der() const { return {}; }
std::string Certificate::common_name() const { return {}; }
std::string Certificate::display_name() const { return {}; }
std::string Certificate::email() const { return {}; }
std::vector<std::string> Certificate::dns_names() const { return {}; }
std::vector<std::uint8_t> Certificate::serial_number() const { return {}; }
std::vector<std::uint8_t> Certificate::issuer() const { return {}; }
std::vector<std::uint8_t> Certificate::subject() const { return {}; }
std::int64_t Certificate::not_before_ns() const { return 0; }
std::int64_t Certificate::not_after_ns() const { return 0; }
bool Certificate::self_issued() const { return false; }
Result<std::vector<Certificate>> root_certificates(RootStore) { return Error::unsupported; }
Result<ChainResult> evaluate_chain(const ChainRequest&) { return Error::unsupported; }

Result<Identity> Identity::self_signed(std::string_view, const std::vector<std::string>&) { return Error::unsupported; }
Result<Identity::Imported> Identity::import_pkcs12(ConstBytes, std::string_view) { return Error::unsupported; }
bool Identity::is_pkcs12(ConstBytes) { return false; }
Identity& Identity::operator=(Identity&& other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    key_ = std::move(other.key_);
    return *this;
}
Identity::~Identity() = default;
Certificate Identity::certificate() const { return {}; }

struct TlsSession::State {
    std::string error{"the null host backend has no TLS"};
};
Result<TlsSession> TlsSession::create(const TlsOptions&, std::string* error_text) {
    if (error_text) *error_text = "the null host backend has no TLS";
    return Error::unsupported;
}
TlsSession::TlsSession(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
TlsSession::TlsSession() = default;
TlsSession::TlsSession(TlsSession&&) noexcept = default;
TlsSession& TlsSession::operator=(TlsSession&&) noexcept = default;
TlsSession::~TlsSession() = default;
TlsSession::Status TlsSession::handshake(const TlsRead&, const TlsWrite&) { return Status::error; }
TlsSession::Status TlsSession::read(const TlsRead&, const TlsWrite&, std::uint8_t*, std::size_t, std::size_t& got) {
    got = 0;
    return Status::error;
}
TlsSession::Status TlsSession::write(const TlsWrite&, const std::uint8_t*, std::size_t, std::size_t& accepted) {
    accepted = 0;
    return Status::error;
}
TlsSession::Status TlsSession::flush(const TlsWrite&) { return Status::error; }
TlsSession::Status TlsSession::shutdown(const TlsWrite&) { return Status::ok; }
bool TlsSession::complete() const noexcept { return false; }
std::size_t TlsSession::buffered() const noexcept { return 0; }
std::vector<Certificate> TlsSession::peer_certificates() const { return {}; }
std::string TlsSession::negotiated_alpn() const { return {}; }
TlsVersion TlsSession::negotiated_version() const { return TlsVersion::unknown; }
const std::string& TlsSession::error() const noexcept {
    static const std::string none{"the null host backend has no TLS"};
    return state_ ? state_->error : none;
}
bool TlsSession::close_notified() const noexcept { return false; }

} // namespace alchemy::host::crypto
