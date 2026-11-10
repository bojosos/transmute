// Linux host backend, area "crypto": random bytes from getrandom(2); digests, ciphers, keys,
// certificates, chains and TLS over OpenSSL 3 (libcrypto, libssl).
//
// - Keys are imported from their components for every call (EVP_PKEY_fromdata) and discarded.
// - Single DES is in OpenSSL's legacy provider (loaded on first use with the default provider;
//   without it, DES is Error::unsupported). PKCS#12 files with RC2 or 3DES key bags need it too.
// - Chains are built with an X509_STORE: the given anchors (any certificate, not only roots), else
//   the system trust store (OpenSSL's default paths, /etc/ssl/certs). There is no revocation data
//   source, so a chain that asks for revocation and has certificates below the root reports
//   revocation_unknown. Intermediates are never fetched.
// - The system root store is OpenSSL's default certificate file (SSL_CERT_FILE overrides it); the
//   user and admin stores are empty: Linux keeps one system-wide store.
// - TLS runs over memory BIOs between the caller's read and write callbacks. A client never judges
//   the server's chain (the caller does, from peer_certificates()).
// - protect / unprotect seal data for this user with a per-user key kept in the desktop's secret
//   store (the Secret Service: GNOME Keyring, KWallet), through libsecret loaded at run time. The key
//   (32 random bytes, item "org.alchemy.Protect" / purpose=protect in the default collection) is
//   made on the first protect. A sealed blob is AES-256-GCM: "ALPR", version 1, the description's
//   length and bytes (in the clear), a 12-byte nonce, the ciphertext and the 16-byte tag; the
//   header and the caller's entropy are authenticated, so other entropy does not unseal. They never
//   prompt: the key is read and stored only in the existing, unlocked collection the "default" alias
//   names. Without libsecret, a Secret Service that answers, a default collection, or with that
//   collection locked, both are Error::unsupported; no collection is created or unlocked, and the
//   key is never kept in a file.
#include "alchemy/host/crypto.hpp"
#include "alchemy/host/linux/linux.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/pkcs12.h>
#include <openssl/provider.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <dlfcn.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <span>

namespace alchemy::host::crypto {

// The backend's view of Certificate (friend of the class).
struct CertificateAccess {
    // Takes one reference of `x509`.
    static Certificate wrap(X509* x509) {
        Certificate certificate;
        certificate.handle_ = x509;
        return certificate;
    }
    static X509* x509(const Certificate& certificate) noexcept { return static_cast<X509*>(certificate.handle_); }
};

namespace {

template <auto Free>
struct Deleter {
    template <class T>
    void operator()(T* pointer) const noexcept { if (pointer) Free(pointer); }
};
template <class T, auto Free>
using Owned = std::unique_ptr<T, Deleter<Free>>;
using PKey = Owned<EVP_PKEY, EVP_PKEY_free>;
using PKeyContext = Owned<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
using Bignum = Owned<BIGNUM, BN_free>;
using Buffer = std::vector<std::uint8_t>;

const unsigned char* bytes(ConstBytes data) noexcept { return reinterpret_cast<const unsigned char*>(data.data()); }

// The legacy provider (single DES, RC2 PKCS#12 bags), loaded once with the default provider kept.
bool legacy_provider() {
    static const bool loaded = [] {
        const auto standard = OSSL_PROVIDER_load(nullptr, "default");
        const auto legacy = OSSL_PROVIDER_load(nullptr, "legacy");
        return standard != nullptr && legacy != nullptr;
    }();
    return loaded;
}

const EVP_MD* message_digest(Digest digest) noexcept {
    switch (digest) {
    case Digest::sha1: return EVP_sha1();
    case Digest::sha224: return EVP_sha224();
    case Digest::sha256: return EVP_sha256();
    case Digest::sha384: return EVP_sha384();
    case Digest::sha512: return EVP_sha512();
    case Digest::md5: return EVP_md5();
    }
    return nullptr;
}

Buffer to_bytes(const BIGNUM* number, std::size_t width = 0) {
    Buffer out(width ? width : static_cast<std::size_t>(BN_num_bytes(number)));
    if (width) BN_bn2binpad(number, out.data(), static_cast<int>(width));
    else BN_bn2bin(number, out.data());
    return out;
}
Buffer bn_param(const EVP_PKEY* key, const char* name, std::size_t width = 0) {
    BIGNUM* number = nullptr;
    if (!EVP_PKEY_get_bn_param(key, name, &number)) return {};
    Bignum owned(number);
    return to_bytes(number, width);
}

// EVP_PKEY_fromdata of OSSL_PARAMs pushed by `push` (BIGNUMs it creates stay alive until the params are built).
template <class Push>
PKey from_data(const char* type, int selection, Push push) {
    Owned<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free> builder(OSSL_PARAM_BLD_new());
    std::vector<Bignum> numbers;
    if (!builder || !push(builder.get(), numbers)) return {};
    Owned<OSSL_PARAM, OSSL_PARAM_free> params(OSSL_PARAM_BLD_to_param(builder.get()));
    PKeyContext context(EVP_PKEY_CTX_new_from_name(nullptr, type, nullptr));
    EVP_PKEY* key = nullptr;
    if (!params || !context || EVP_PKEY_fromdata_init(context.get()) <= 0 ||
        EVP_PKEY_fromdata(context.get(), &key, selection, params.get()) <= 0)
        return {};
    return PKey(key);
}
bool push_bn(OSSL_PARAM_BLD* builder, std::vector<Bignum>& numbers, const char* name, const Buffer& value) {
    if (value.empty()) return true;
    numbers.emplace_back(BN_bin2bn(value.data(), static_cast<int>(value.size()), nullptr));
    return numbers.back() && OSSL_PARAM_BLD_push_BN(builder, name, numbers.back().get());
}

PKey rsa_key(const RsaKey& key, bool use_private) {
    if (key.n.empty() || key.e.empty() || (use_private && key.d.empty())) return {};
    return from_data("RSA", use_private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, [&](OSSL_PARAM_BLD* builder, std::vector<Bignum>& numbers) {
        bool ok = push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_N, key.n) && push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_E, key.e);
        if (use_private) {
            ok = ok && push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_D, key.d);
            if (!key.p.empty() && !key.q.empty() && !key.dp.empty() && !key.dq.empty() && !key.qinv.empty())
                ok = ok && push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_FACTOR1, key.p) && push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_FACTOR2, key.q) &&
                     push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_EXPONENT1, key.dp) &&
                     push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_EXPONENT2, key.dq) &&
                     push_bn(builder, numbers, OSSL_PKEY_PARAM_RSA_COEFFICIENT1, key.qinv);
        }
        return ok;
    });
}

const char* curve_name(std::size_t coordinate) noexcept {
    switch (coordinate) {
    case 32: return "prime256v1";
    case 48: return "secp384r1";
    case 66: return "secp521r1";
    default: return nullptr;
    }
}
Buffer padded(const Buffer& value, std::size_t width) {
    if (value.size() >= width) return Buffer(value.end() - static_cast<std::ptrdiff_t>(width), value.end());
    Buffer out(width - value.size(), 0);
    out.insert(out.end(), value.begin(), value.end());
    return out;
}

PKey ec_key(const EcKey& key, bool use_private) {
    const auto curve = curve_name(key.coordinate);
    if (!curve || key.x.empty() || key.y.empty() || key.x.size() > key.coordinate || key.y.size() > key.coordinate ||
        (use_private && key.k.empty()))
        return {};
    Buffer point{0x04};
    for (const auto& part : {padded(key.x, key.coordinate), padded(key.y, key.coordinate)}) point.insert(point.end(), part.begin(), part.end());
    return from_data("EC", use_private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, [&](OSSL_PARAM_BLD* builder, std::vector<Bignum>& numbers) {
        return OSSL_PARAM_BLD_push_utf8_string(builder, OSSL_PKEY_PARAM_GROUP_NAME, curve, 0) &&
               OSSL_PARAM_BLD_push_octet_string(builder, OSSL_PKEY_PARAM_PUB_KEY, point.data(), point.size()) &&
               (!use_private || push_bn(builder, numbers, OSSL_PKEY_PARAM_PRIV_KEY, key.k));
    });
}

// One EVP_PKEY operation's output (sign, encrypt, decrypt, derive) through its two-call protocol.
template <class Operation>
Result<Buffer> two_call(Operation operation) {
    std::size_t size = 0;
    if (operation(nullptr, &size) <= 0) return Error::invalid_argument;
    Buffer out(size);
    if (operation(out.data(), &size) <= 0) return Error::invalid_argument;
    out.resize(size);
    return out;
}

// RSA padding parameters on an initialized context.
bool rsa_padding(EVP_PKEY_CTX* context, RsaPadding padding, std::optional<Digest> digest, bool signature) {
    const auto md = digest ? message_digest(*digest) : nullptr;
    switch (padding) {
    case RsaPadding::none: return EVP_PKEY_CTX_set_rsa_padding(context, RSA_NO_PADDING) > 0;
    case RsaPadding::pkcs1:
        if (EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PADDING) <= 0) return false;
        return !signature || !md || EVP_PKEY_CTX_set_signature_md(context, md) > 0;
    case RsaPadding::pss:
        return signature && md && EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_PSS_PADDING) > 0 &&
               EVP_PKEY_CTX_set_signature_md(context, md) > 0 && EVP_PKEY_CTX_set_rsa_mgf1_md(context, md) > 0 &&
               EVP_PKEY_CTX_set_rsa_pss_saltlen(context, RSA_PSS_SALTLEN_DIGEST) > 0;
    case RsaPadding::oaep: {
        if (signature) return false;
        const auto oaep = md ? md : EVP_sha1();
        return EVP_PKEY_CTX_set_rsa_padding(context, RSA_PKCS1_OAEP_PADDING) > 0 && EVP_PKEY_CTX_set_rsa_oaep_md(context, oaep) > 0 &&
               EVP_PKEY_CTX_set_rsa_mgf1_md(context, oaep) > 0;
    }
    }
    return false;
}

std::string openssl_error(std::string_view what) {
    std::string text(what);
    if (const auto code = ERR_get_error()) {
        char buffer[256];
        ERR_error_string_n(code, buffer, sizeof(buffer));
        text += ": ";
        text += buffer;
    }
    ERR_clear_error();
    return text;
}

} // namespace

// ---- Random, protect ----

Status random(Bytes buffer) {
    for (std::size_t done = 0; done < buffer.size();) {
        const auto got = getrandom(buffer.data() + done, buffer.size() - done, 0);
        if (got < 0) {
            if (errno == EINTR) continue;
            return linux_backend::last_error();
        }
        done += static_cast<std::size_t>(got);
    }
    return {};
}

void secure_zero(Bytes buffer) noexcept {
    volatile std::byte* at = buffer.data();
    for (std::size_t index = 0; index < buffer.size(); ++index) at[index] = std::byte{};
}

namespace {

// libsecret's simple password API (secret-schema.h, secret-password.h), loaded at run time; glib's
// g_error_free comes with it.
struct SecretSchemaAttribute { const char* name; int type; };   // SECRET_SCHEMA_ATTRIBUTE_STRING is 0
struct SecretSchema {
    const char* name;
    int flags;                                                   // SECRET_SCHEMA_NONE is 0
    SecretSchemaAttribute attributes[32];
    int reserved;
    void* reserved1; void* reserved2; void* reserved3; void* reserved4; void* reserved5; void* reserved6; void* reserved7;
};
struct GlibError;
struct SecretApi {
    using Lookup = char* (*)(const SecretSchema*, void* cancellable, GlibError** error, ...);
    using Store = int (*)(const SecretSchema*, const char* collection, const char* label, const char* password, void* cancellable,
                          GlibError** error, ...);
    using FreePassword = void (*)(char*);
    using FreeError = void (*)(GlibError*);
    using ServiceGet = void* (*)(int flags, void* cancellable, GlibError** error);   // SecretService*, SECRET_SERVICE_NONE is 0
    using ForAlias = void* (*)(void* service, const char* alias, int flags, void* cancellable, GlibError** error);   // SecretCollection*
    using GetLocked = int (*)(void* collection);
    using Unref = void (*)(void* object);
    Lookup lookup{};
    Store store{};
    FreePassword free_password{};
    FreeError free_error{};
    ServiceGet service_get{};
    ForAlias for_alias{};
    GetLocked get_locked{};
    Unref unref{};
};
const SecretApi* secret_api() {
    static const SecretApi api = [] {
        SecretApi loaded;
        const auto library = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!library) return loaded;
        loaded.lookup = reinterpret_cast<SecretApi::Lookup>(dlsym(library, "secret_password_lookup_sync"));
        loaded.store = reinterpret_cast<SecretApi::Store>(dlsym(library, "secret_password_store_sync"));
        loaded.free_password = reinterpret_cast<SecretApi::FreePassword>(dlsym(library, "secret_password_free"));
        loaded.free_error = reinterpret_cast<SecretApi::FreeError>(dlsym(library, "g_error_free"));   // glib, a dependency
        loaded.service_get = reinterpret_cast<SecretApi::ServiceGet>(dlsym(library, "secret_service_get_sync"));
        loaded.for_alias = reinterpret_cast<SecretApi::ForAlias>(dlsym(library, "secret_collection_for_alias_sync"));
        loaded.get_locked = reinterpret_cast<SecretApi::GetLocked>(dlsym(library, "secret_collection_get_locked"));
        loaded.unref = reinterpret_cast<SecretApi::Unref>(dlsym(library, "g_object_unref"));   // gobject, a dependency
        return loaded;
    }();
    return api.lookup && api.store && api.free_password && api.free_error && api.service_get && api.for_alias && api.get_locked && api.unref
               ? &api
               : nullptr;
}
// Whether the default collection exists and is unlocked, asked without creating or unlocking anything
// (SECRET_COLLECTION_NONE): only then may the password calls run, which would otherwise prompt.
bool default_collection_open(const SecretApi& api) {
    GlibError* error = nullptr;
    const auto service = api.service_get(0, nullptr, &error);
    if (error) api.free_error(error);
    if (!service) return false;
    error = nullptr;
    const auto collection = api.for_alias(service, "default", 0, nullptr, &error);
    if (error) api.free_error(error);
    const bool open = collection && !api.get_locked(collection);
    if (collection) api.unref(collection);
    api.unref(service);
    return open;
}
const SecretSchema protect_schema{"org.alchemy.Protect", 0, {{"purpose", 0}, {nullptr, 0}}, 0,
                                  nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
constexpr std::size_t key_size = 32, nonce_size = 12, tag_size = 16;
constexpr std::array<std::uint8_t, 4> seal_magic{'A', 'L', 'P', 'R'};

std::string hex(const Buffer& data) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (const auto byte : data) { text.push_back(digits[byte >> 4]); text.push_back(digits[byte & 15]); }
    return text;
}
std::optional<Buffer> unhex(std::string_view text) {
    if (text.size() % 2) return std::nullopt;
    Buffer out;
    for (std::size_t at = 0; at < text.size(); at += 2) {
        const auto nibble = [](char c) -> int {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        };
        const auto high = nibble(text[at]), low = nibble(text[at + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(high << 4 | low));
    }
    return out;
}

// The per-user sealing key: Error::unsupported without a Secret Service; Error::not_found when there is
// none yet and `create` is false.
Result<Buffer> sealing_key(bool create) {
    const auto api = secret_api();
    if (!api) return Error::unsupported;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    if (!default_collection_open(*api)) return Error::unsupported;   // never create or unlock a collection
    GlibError* error = nullptr;
    char* stored = api->lookup(&protect_schema, nullptr, &error, "purpose", "protect", static_cast<const char*>(nullptr));
    if (error) {
        api->free_error(error);
        if (stored) api->free_password(stored);
        return Error::unsupported;   // no Secret Service answered
    }
    if (stored) {
        auto key = unhex(stored);
        secure_zero(std::as_writable_bytes(std::span(stored, std::strlen(stored))));
        api->free_password(stored);
        if (!key || key->size() != key_size) return Error::invalid_argument;
        return std::move(*key);
    }
    if (!create) return Error::not_found;
    Buffer key(key_size);
    if (!random(std::as_writable_bytes(std::span(key)))) return Error::other;
    auto text = hex(key);
    const auto stored_ok = api->store(&protect_schema, "default", "Alchemy data protection key", text.c_str(), nullptr, &error, "purpose", "protect",
                                      static_cast<const char*>(nullptr));
    secure_zero(std::as_writable_bytes(std::span(text)));
    if (error) {
        api->free_error(error);
        return Error::unsupported;
    }
    if (!stored_ok) return Error::unsupported;
    return key;
}

// AES-256-GCM over `input` with `aad`: the output, and the tag (encrypting) or a check of `tag` (decrypting).
bool seal_cipher(bool encrypt, const Buffer& key, const std::uint8_t* nonce, const Buffer& aad, const std::uint8_t* input, std::size_t size,
                 std::uint8_t* output, std::uint8_t* tag) {
    Owned<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> context(EVP_CIPHER_CTX_new());
    int written = 0;
    if (!context || !EVP_CipherInit_ex2(context.get(), EVP_aes_256_gcm(), key.data(), nonce, encrypt ? 1 : 0, nullptr)) return false;
    if (!aad.empty() && !EVP_CipherUpdate(context.get(), nullptr, &written, aad.data(), static_cast<int>(aad.size()))) return false;
    if (size && !EVP_CipherUpdate(context.get(), output, &written, input, static_cast<int>(size))) return false;
    if (!encrypt && !EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(tag_size), tag)) return false;
    if (EVP_CipherFinal_ex(context.get(), output + written, &written) <= 0) return false;
    return !encrypt || EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(tag_size), tag) > 0;
}

} // namespace

Result<std::vector<std::uint8_t>> protect(ConstBytes data, ConstBytes entropy, std::string_view description) {
    if (description.size() > 0xffff || data.size() > 0x7fffffff) return Error::invalid_argument;
    auto key = sealing_key(true);
    if (!key) return key.error();
    Buffer sealed(seal_magic.begin(), seal_magic.end());
    sealed.push_back(1);   // version
    sealed.push_back(static_cast<std::uint8_t>(description.size()));
    sealed.push_back(static_cast<std::uint8_t>(description.size() >> 8));
    sealed.insert(sealed.end(), description.begin(), description.end());
    Buffer aad(sealed);
    aad.insert(aad.end(), bytes(entropy), bytes(entropy) + entropy.size());
    const auto nonce_at = sealed.size();
    sealed.resize(nonce_at + nonce_size + data.size() + tag_size);
    const auto ok = random(std::as_writable_bytes(std::span(sealed.data() + nonce_at, nonce_size))) &&
                    seal_cipher(true, *key, sealed.data() + nonce_at, aad, bytes(data), data.size(), sealed.data() + nonce_at + nonce_size,
                                sealed.data() + nonce_at + nonce_size + data.size());
    secure_zero(std::as_writable_bytes(std::span(*key)));
    if (!ok) return Error::other;
    return sealed;
}

Result<std::vector<std::uint8_t>> unprotect(ConstBytes data, ConstBytes entropy) {
    const auto blob = bytes(data);
    if (data.size() < seal_magic.size() + 3 || !std::equal(seal_magic.begin(), seal_magic.end(), blob) || blob[4] != 1) return Error::invalid_argument;
    const std::size_t description = blob[5] | (static_cast<std::size_t>(blob[6]) << 8);
    const auto nonce_at = seal_magic.size() + 3 + description;
    if (data.size() < nonce_at + nonce_size + tag_size) return Error::invalid_argument;
    auto key = sealing_key(false);
    if (!key) return key.error() == Error::not_found ? Error::access_denied : key.error();   // nothing was ever sealed for this user
    Buffer aad(blob, blob + nonce_at);
    aad.insert(aad.end(), bytes(entropy), bytes(entropy) + entropy.size());
    const auto size = data.size() - nonce_at - nonce_size - tag_size;
    Buffer opened(size), tag(blob + data.size() - tag_size, blob + data.size());
    const auto ok = seal_cipher(false, *key, blob + nonce_at, aad, blob + nonce_at + nonce_size, size, opened.data(), tag.data());
    secure_zero(std::as_writable_bytes(std::span(*key)));
    if (!ok) {
        secure_zero(std::as_writable_bytes(std::span(opened)));
        return Error::invalid_argument;   // other entropy, another user's key, or a damaged blob
    }
    return opened;
}

// ---- Block ciphers ----

namespace {
struct CipherKey { const EVP_CIPHER* cipher; Buffer key; };
} // namespace

Result<BlockCipher> BlockCipher::create(BlockAlgorithm algorithm, ConstBytes key) {
    const char* name = nullptr;
    std::size_t block = 16;
    switch (algorithm) {
    case BlockAlgorithm::aes:
        name = key.size() == 16 ? "AES-128-ECB" : key.size() == 24 ? "AES-192-ECB" : key.size() == 32 ? "AES-256-ECB" : nullptr;
        break;
    case BlockAlgorithm::des:
        name = key.size() == 8 ? "DES-ECB" : nullptr;
        block = 8;
        break;
    case BlockAlgorithm::triple_des:
        name = key.size() == 16 ? "DES-EDE-ECB" : key.size() == 24 ? "DES-EDE3-ECB" : nullptr;
        block = 8;
        break;
    }
    if (!name) return Error::invalid_argument;
    auto cipher = EVP_CIPHER_fetch(nullptr, name, nullptr);
    if (!cipher && algorithm == BlockAlgorithm::des && legacy_provider()) cipher = EVP_CIPHER_fetch(nullptr, name, nullptr);
    if (!cipher) return Error::unsupported;
    BlockCipher result;
    result.algorithm_ = cipher;
    result.key_ = new Buffer(bytes(key), bytes(key) + key.size());
    result.block_ = block;
    return result;
}

BlockCipher& BlockCipher::operator=(BlockCipher&& other) noexcept {
    if (this != &other) {
        this->~BlockCipher();
        algorithm_ = std::exchange(other.algorithm_, nullptr);
        key_ = std::exchange(other.key_, nullptr);
        block_ = other.block_;
    }
    return *this;
}

BlockCipher::~BlockCipher() {
    if (algorithm_) EVP_CIPHER_free(static_cast<EVP_CIPHER*>(algorithm_));
    if (key_) {
        auto* key = static_cast<Buffer*>(key_);
        secure_zero(std::as_writable_bytes(std::span(*key)));
        delete key;
    }
    algorithm_ = key_ = nullptr;
}

namespace {
Status ecb(const EVP_CIPHER* cipher, const Buffer& key, std::size_t block, ConstBytes input, Bytes output, bool encrypt) {
    if (!cipher) return Error::bad_handle;
    if (input.size() != output.size() || input.size() % block) return Error::invalid_argument;
    Owned<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free> context(EVP_CIPHER_CTX_new());
    int written = 0;
    if (!context || !EVP_CipherInit_ex2(context.get(), cipher, key.data(), nullptr, encrypt ? 1 : 0, nullptr) ||
        !EVP_CIPHER_CTX_set_padding(context.get(), 0) ||
        !EVP_CipherUpdate(context.get(), reinterpret_cast<unsigned char*>(output.data()), &written, bytes(input), static_cast<int>(input.size())))
        return Error::other;
    return {};
}
} // namespace

Status BlockCipher::encrypt(ConstBytes input, Bytes output) const {
    if (!key_) return Error::bad_handle;
    return ecb(static_cast<const EVP_CIPHER*>(algorithm_), *static_cast<const Buffer*>(key_), block_, input, output, true);
}
Status BlockCipher::decrypt(ConstBytes input, Bytes output) const {
    if (!key_) return Error::bad_handle;
    return ecb(static_cast<const EVP_CIPHER*>(algorithm_), *static_cast<const Buffer*>(key_), block_, input, output, false);
}

// ---- Digests ----

Status pbkdf2(Digest digest, ConstBytes password, ConstBytes salt, std::uint32_t rounds, Bytes output) {
    const auto md = message_digest(digest);
    if (!md || !supports(digest)) return Error::unsupported;
    if (!rounds) return Error::invalid_argument;
    if (!PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(password.data()), static_cast<int>(password.size()), bytes(salt),
                           static_cast<int>(salt.size()), static_cast<int>(std::min<std::uint32_t>(rounds, 0x7fffffff)), md,
                           static_cast<int>(output.size()), reinterpret_cast<unsigned char*>(output.data())))
        return Error::other;
    return {};
}

bool supports(Digest digest) noexcept {
    const auto md = message_digest(digest);
    if (!md) return false;
    // A FIPS-only configuration lacks MD5 (and may lack SHA-1).
    Owned<EVP_MD, EVP_MD_free> fetched(EVP_MD_fetch(nullptr, EVP_MD_get0_name(md), nullptr));
    return fetched != nullptr;
}

Result<std::vector<std::uint8_t>> hash(Digest digest, ConstBytes data) {
    const auto md = message_digest(digest);
    if (!md || !supports(digest)) return Error::unsupported;
    Buffer out(static_cast<std::size_t>(EVP_MD_get_size(md)));
    unsigned int size = 0;
    if (!EVP_Digest(data.data(), data.size(), out.data(), &size, md, nullptr)) return Error::other;
    out.resize(size);
    return out;
}

// ---- Asymmetric keys ----

Status check(const RsaKey& key, bool use_private) {
    const auto pkey = rsa_key(key, use_private);
    if (!pkey) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!context || (use_private ? EVP_PKEY_pairwise_check(context.get()) : EVP_PKEY_public_check(context.get())) <= 0) return Error::bad_handle;
    return {};
}

Status check(const EcKey& key, bool use_private) {
    const auto pkey = ec_key(key, use_private);
    if (!pkey) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!context || (use_private ? EVP_PKEY_check(context.get()) : EVP_PKEY_public_check(context.get())) <= 0) return Error::bad_handle;
    return {};
}

Result<RsaKey> generate_rsa(unsigned bits) {
    if (bits < 512 || bits > 16384) return Error::invalid_argument;
    PKey pkey(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", static_cast<std::size_t>(bits)));
    if (!pkey) return Error::other;
    RsaKey key;
    key.bits = static_cast<unsigned>(EVP_PKEY_get_bits(pkey.get()));
    key.n = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_N);
    key.e = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_E);
    key.d = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_D);
    key.p = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_FACTOR1);
    key.q = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_FACTOR2);
    key.dp = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_EXPONENT1);
    key.dq = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_EXPONENT2);
    key.qinv = bn_param(pkey.get(), OSSL_PKEY_PARAM_RSA_COEFFICIENT1);
    if (key.n.empty() || key.d.empty()) return Error::other;
    return key;
}

Result<EcKey> generate_ec(std::size_t coordinate) {
    const auto curve = curve_name(coordinate);
    if (!curve) return Error::invalid_argument;
    PKey pkey(EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", curve));
    if (!pkey) return Error::other;
    EcKey key;
    key.coordinate = coordinate;
    key.x = bn_param(pkey.get(), OSSL_PKEY_PARAM_EC_PUB_X, coordinate);
    key.y = bn_param(pkey.get(), OSSL_PKEY_PARAM_EC_PUB_Y, coordinate);
    key.k = bn_param(pkey.get(), OSSL_PKEY_PARAM_PRIV_KEY, coordinate);
    if (key.x.empty() || key.y.empty() || key.k.empty()) return Error::other;
    return key;
}

Result<std::vector<std::uint8_t>> rsa_sign(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes hash) {
    const auto pkey = rsa_key(key, true);
    if (!pkey) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!context || EVP_PKEY_sign_init(context.get()) <= 0 || !rsa_padding(context.get(), padding, digest, true)) return Error::invalid_argument;
    return two_call([&](unsigned char* out, std::size_t* size) { return EVP_PKEY_sign(context.get(), out, size, bytes(hash), hash.size()); });
}

Status rsa_verify(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes hash, ConstBytes signature) {
    const auto pkey = rsa_key(key, false);
    if (!pkey) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!context || EVP_PKEY_verify_init(context.get()) <= 0 || !rsa_padding(context.get(), padding, digest, true)) return Error::invalid_argument;
    if (EVP_PKEY_verify(context.get(), bytes(signature), signature.size(), bytes(hash), hash.size()) != 1) return Error::invalid_argument;
    return {};
}

Result<std::vector<std::uint8_t>> rsa_encrypt(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes data) {
    const auto pkey = rsa_key(key, false);
    if (!pkey) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!context || EVP_PKEY_encrypt_init(context.get()) <= 0 || !rsa_padding(context.get(), padding, digest, false)) return Error::invalid_argument;
    return two_call([&](unsigned char* out, std::size_t* size) { return EVP_PKEY_encrypt(context.get(), out, size, bytes(data), data.size()); });
}

Result<std::vector<std::uint8_t>> rsa_decrypt(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes data) {
    const auto pkey = rsa_key(key, true);
    if (!pkey) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!context || EVP_PKEY_decrypt_init(context.get()) <= 0 || !rsa_padding(context.get(), padding, digest, false)) return Error::invalid_argument;
    return two_call([&](unsigned char* out, std::size_t* size) { return EVP_PKEY_decrypt(context.get(), out, size, bytes(data), data.size()); });
}

Result<std::vector<std::uint8_t>> ecdsa_sign(const EcKey& key, ConstBytes hash) {
    const auto pkey = ec_key(key, true);
    if (!pkey) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    if (!context || EVP_PKEY_sign_init(context.get()) <= 0) return Error::invalid_argument;
    auto der = two_call([&](unsigned char* out, std::size_t* size) { return EVP_PKEY_sign(context.get(), out, size, bytes(hash), hash.size()); });
    if (!der) return der.error();
    const unsigned char* at = der->data();
    Owned<ECDSA_SIG, ECDSA_SIG_free> signature(d2i_ECDSA_SIG(nullptr, &at, static_cast<long>(der->size())));
    if (!signature) return Error::other;
    auto out = to_bytes(ECDSA_SIG_get0_r(signature.get()), key.coordinate);
    const auto s = to_bytes(ECDSA_SIG_get0_s(signature.get()), key.coordinate);
    out.insert(out.end(), s.begin(), s.end());
    return out;
}

Status ecdsa_verify(const EcKey& key, ConstBytes hash, ConstBytes signature) {
    const auto pkey = ec_key(key, false);
    if (!pkey) return Error::bad_handle;
    if (signature.size() != key.coordinate * 2) return Error::invalid_argument;
    Owned<ECDSA_SIG, ECDSA_SIG_free> parts(ECDSA_SIG_new());
    const auto r = BN_bin2bn(bytes(signature), static_cast<int>(key.coordinate), nullptr);
    const auto s = BN_bin2bn(bytes(signature) + key.coordinate, static_cast<int>(key.coordinate), nullptr);
    if (!parts || !r || !s || !ECDSA_SIG_set0(parts.get(), r, s)) {
        BN_free(r);
        BN_free(s);
        return Error::other;
    }
    unsigned char* der = nullptr;
    const auto size = i2d_ECDSA_SIG(parts.get(), &der);
    if (size <= 0) return Error::other;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, pkey.get(), nullptr));
    const bool verified = context && EVP_PKEY_verify_init(context.get()) > 0 &&
                          EVP_PKEY_verify(context.get(), der, static_cast<std::size_t>(size), bytes(hash), hash.size()) == 1;
    OPENSSL_free(der);
    if (!verified) return Error::invalid_argument;
    return {};
}

Result<std::vector<std::uint8_t>> ecdh(const EcKey& key, const EcKey& peer) {
    if (key.coordinate != peer.coordinate) return Error::invalid_argument;
    const auto mine = ec_key(key, true), theirs = ec_key(peer, false);
    if (!mine || !theirs) return Error::bad_handle;
    PKeyContext context(EVP_PKEY_CTX_new_from_pkey(nullptr, mine.get(), nullptr));
    if (!context || EVP_PKEY_derive_init(context.get()) <= 0 || EVP_PKEY_derive_set_peer(context.get(), theirs.get()) <= 0)
        return Error::invalid_argument;
    auto secret = two_call([&](unsigned char* out, std::size_t* size) { return EVP_PKEY_derive(context.get(), out, size); });
    if (secret) *secret = padded(*secret, key.coordinate);
    return secret;
}

// ---- Certificates ----

namespace {

X509* x509_of(const Certificate& certificate) noexcept { return CertificateAccess::x509(certificate); }

std::string utf8(const ASN1_STRING* value) {
    if (!value) return {};
    unsigned char* text = nullptr;
    const auto size = ASN1_STRING_to_UTF8(&text, value);
    if (size < 0) return {};
    std::string out(reinterpret_cast<const char*>(text), static_cast<std::size_t>(size));
    OPENSSL_free(text);
    return out;
}
std::string name_entry(const X509_NAME* name, int nid) {
    const auto index = X509_NAME_get_index_by_NID(name, nid, -1);
    return index < 0 ? std::string{} : utf8(X509_NAME_ENTRY_get_data(X509_NAME_get_entry(name, index)));
}
Buffer name_der(const X509_NAME* name) {
    unsigned char* der = nullptr;
    const auto size = i2d_X509_NAME(name, &der);
    if (size <= 0) return {};
    Buffer out(der, der + size);
    OPENSSL_free(der);
    return out;
}
// subjectAltName entries of one kind (GEN_DNS, GEN_EMAIL).
std::vector<std::string> alt_names(const X509* certificate, int kind) {
    std::vector<std::string> out;
    Owned<GENERAL_NAMES, GENERAL_NAMES_free> names(static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr)));
    if (!names) return out;
    for (int index = 0; index < sk_GENERAL_NAME_num(names.get()); ++index) {
        const auto entry = sk_GENERAL_NAME_value(names.get(), index);
        if (entry->type != kind) continue;
        const ASN1_STRING* value = kind == GEN_DNS ? entry->d.dNSName : entry->d.rfc822Name;
        out.emplace_back(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)), static_cast<std::size_t>(ASN1_STRING_length(value)));
    }
    return out;
}
std::int64_t time_ns(const ASN1_TIME* value) {
    std::tm parts{};
    if (!value || !ASN1_TIME_to_tm(value, &parts)) return 0;
    return static_cast<std::int64_t>(timegm(&parts)) * 1'000'000'000;
}

} // namespace

Result<Certificate> Certificate::parse(ConstBytes der) {
    const unsigned char* at = bytes(der);
    X509* parsed = d2i_X509(nullptr, &at, static_cast<long>(der.size()));
    if (!parsed || at != bytes(der) + der.size()) {
        X509_free(parsed);
        ERR_clear_error();
        return Error::invalid_argument;
    }
    return CertificateAccess::wrap(parsed);
}

Certificate::Certificate(const Certificate& other) : handle_(other.handle_) {
    if (handle_) X509_up_ref(static_cast<X509*>(handle_));
}
Certificate& Certificate::operator=(const Certificate& other) {
    if (this != &other) {
        if (other.handle_) X509_up_ref(static_cast<X509*>(other.handle_));
        if (handle_) X509_free(static_cast<X509*>(handle_));
        handle_ = other.handle_;
    }
    return *this;
}
Certificate& Certificate::operator=(Certificate&& other) noexcept {
    if (this != &other) {
        if (handle_) X509_free(static_cast<X509*>(handle_));
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}
Certificate::~Certificate() { if (handle_) X509_free(static_cast<X509*>(handle_)); }

std::vector<std::uint8_t> Certificate::der() const {
    if (!handle_) return {};
    unsigned char* der = nullptr;
    const auto size = i2d_X509(x509_of(*this), &der);
    if (size <= 0) return {};
    Buffer out(der, der + size);
    OPENSSL_free(der);
    return out;
}
std::string Certificate::common_name() const { return handle_ ? name_entry(X509_get_subject_name(x509_of(*this)), NID_commonName) : std::string{}; }
// The subject's commonName, else its organization, organizational unit or e-mail address.
std::string Certificate::display_name() const {
    if (!handle_) return {};
    const auto subject = X509_get_subject_name(x509_of(*this));
    for (const auto nid : {NID_commonName, NID_organizationName, NID_organizationalUnitName})
        if (auto value = name_entry(subject, nid); !value.empty()) return value;
    return email();
}
// A subjectAltName e-mail address, else the subject's emailAddress.
std::string Certificate::email() const {
    if (!handle_) return {};
    const auto names = alt_names(x509_of(*this), GEN_EMAIL);
    return names.empty() ? name_entry(X509_get_subject_name(x509_of(*this)), NID_pkcs9_emailAddress) : names.front();
}
std::vector<std::string> Certificate::dns_names() const { return handle_ ? alt_names(x509_of(*this), GEN_DNS) : std::vector<std::string>{}; }
// The INTEGER's content octets as DER encodes them (a positive value with its top bit set keeps its leading zero).
std::vector<std::uint8_t> Certificate::serial_number() const {
    if (!handle_) return {};
    Bignum number(ASN1_INTEGER_to_BN(X509_get0_serialNumber(x509_of(*this)), nullptr));
    if (!number) return {};
    auto out = to_bytes(number.get());
    if (out.empty()) out.push_back(0);
    else if (out.front() & 0x80) out.insert(out.begin(), 0);
    return out;
}
std::vector<std::uint8_t> Certificate::issuer() const { return handle_ ? name_der(X509_get_issuer_name(x509_of(*this))) : Buffer{}; }
std::vector<std::uint8_t> Certificate::subject() const { return handle_ ? name_der(X509_get_subject_name(x509_of(*this))) : Buffer{}; }
std::int64_t Certificate::not_before_ns() const { return handle_ ? time_ns(X509_get0_notBefore(x509_of(*this))) : 0; }
std::int64_t Certificate::not_after_ns() const { return handle_ ? time_ns(X509_get0_notAfter(x509_of(*this))) : 0; }
bool Certificate::self_issued() const {
    return handle_ && X509_NAME_cmp(X509_get_issuer_name(x509_of(*this)), X509_get_subject_name(x509_of(*this))) == 0;
}

// The system store: OpenSSL's default certificate file (SSL_CERT_FILE overrides it). Linux keeps no
// separate user or administrator stores.
Result<std::vector<Certificate>> root_certificates(RootStore store) {
    if (store != RootStore::system) return std::vector<Certificate>{};
    const char* path = std::getenv(X509_get_default_cert_file_env());
    if (!path || !*path) path = X509_get_default_cert_file();
    Owned<BIO, BIO_free_all> file(BIO_new_file(path, "r"));
    if (!file) {
        ERR_clear_error();
        return Error::not_found;
    }
    std::vector<Certificate> roots;
    while (X509* certificate = PEM_read_bio_X509(file.get(), nullptr, nullptr, nullptr)) roots.push_back(CertificateAccess::wrap(certificate));
    ERR_clear_error();   // the end of the file
    return roots;
}

namespace {

ChainFailure chain_failure(int error) noexcept {
    switch (error) {
    case X509_V_OK: return ChainFailure::none;
    case X509_V_ERR_CERT_HAS_EXPIRED: case X509_V_ERR_CERT_NOT_YET_VALID: case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD: return ChainFailure::expired;
    case X509_V_ERR_HOSTNAME_MISMATCH: case X509_V_ERR_IP_ADDRESS_MISMATCH: case X509_V_ERR_EMAIL_MISMATCH: return ChainFailure::name_mismatch;
    case X509_V_ERR_INVALID_PURPOSE: return ChainFailure::wrong_usage;
    case X509_V_ERR_CERT_REVOKED: return ChainFailure::revoked;
    case X509_V_ERR_UNABLE_TO_GET_CRL: case X509_V_ERR_CRL_HAS_EXPIRED: case X509_V_ERR_CRL_NOT_YET_VALID: return ChainFailure::revocation_unknown;
    case X509_V_ERR_CERT_SIGNATURE_FAILURE: case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE: case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        return ChainFailure::bad_signature;
    default: return ChainFailure::untrusted;
    }
}

void free_stack(STACK_OF(X509)* stack) { sk_X509_free(stack); }   // a macro in OpenSSL 3

bool numeric_host(const std::string& host) {
    Owned<ASN1_OCTET_STRING, ASN1_OCTET_STRING_free> address(a2i_IPADDRESS(host.c_str()));
    ERR_clear_error();
    return address != nullptr;
}

} // namespace

Result<ChainResult> evaluate_chain(const ChainRequest& request) {
    if (!request.leaf) return Error::invalid_argument;
    Owned<X509_STORE, X509_STORE_free> store(X509_STORE_new());
    Owned<X509_STORE_CTX, X509_STORE_CTX_free> context(X509_STORE_CTX_new());
    Owned<STACK_OF(X509), free_stack> untrusted(sk_X509_new_null());
    if (!store || !context || !untrusted) return Error::no_memory;
    if (request.anchors.empty()) {
        X509_STORE_set_default_paths(store.get());
    } else {
        for (const auto& anchor : request.anchors)
            if (anchor) X509_STORE_add_cert(store.get(), x509_of(anchor));
    }
    for (const auto& intermediate : request.intermediates)
        if (intermediate) sk_X509_push(untrusted.get(), x509_of(intermediate));
    if (!X509_STORE_CTX_init(context.get(), store.get(), x509_of(request.leaf), untrusted.get())) return Error::other;
    const auto parameters = X509_STORE_CTX_get0_param(context.get());
    // An anchor need not be a self-signed root (SecTrustSetAnchorCertificates allows any certificate).
    if (!request.anchors.empty()) X509_VERIFY_PARAM_set_flags(parameters, X509_V_FLAG_PARTIAL_CHAIN);
    if (request.time_ns) X509_VERIFY_PARAM_set_time(parameters, static_cast<time_t>(*request.time_ns / 1'000'000'000));
    for (const auto& policy : request.policies) {
        if (!policy.ssl) continue;
        X509_STORE_CTX_set_purpose(context.get(), policy.server ? X509_PURPOSE_SSL_SERVER : X509_PURPOSE_SSL_CLIENT);
        if (policy.hostname.empty()) continue;
        if (numeric_host(policy.hostname)) X509_VERIFY_PARAM_set1_ip_asc(parameters, policy.hostname.c_str());
        else X509_VERIFY_PARAM_set1_host(parameters, policy.hostname.c_str(), policy.hostname.size());
    }
    const bool verified = X509_verify_cert(context.get()) == 1;
    ChainResult result;
    result.failure = verified ? ChainFailure::none : chain_failure(X509_STORE_CTX_get_error(context.get()));
    if (STACK_OF(X509)* chain = X509_STORE_CTX_get1_chain(context.get())) {
        for (int index = 0; index < sk_X509_num(chain); ++index) result.chain.push_back(CertificateAccess::wrap(sk_X509_value(chain, index)));
        sk_X509_free(chain);   // the certificates now belong to result.chain
    } else {
        result.chain.push_back(request.leaf);
    }
    // No revocation data source: what lies below the root cannot be checked.
    if (verified && request.revocation && result.chain.size() > 1) result.failure = ChainFailure::revocation_unknown;
    ERR_clear_error();
    return result;
}

// ---- Identities ----

namespace {

struct IdentityPair {
    X509* certificate{};
    EVP_PKEY* key{};
    ~IdentityPair() {
        X509_free(certificate);
        EVP_PKEY_free(key);
    }
};
IdentityPair* pair_of(void* handle) noexcept { return static_cast<IdentityPair*>(handle); }

bool add_extension(X509* certificate, int nid, const std::string& value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    Owned<X509_EXTENSION, X509_EXTENSION_free> extension(X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str()));
    return extension && X509_add_ext(certificate, extension.get(), -1);
}

} // namespace

Result<Identity> Identity::self_signed(std::string_view common_name, const std::vector<std::string>& dns_names) {
    PKey key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048}));
    Owned<X509, X509_free> certificate(X509_new());
    if (!key || !certificate) return Error::other;
    std::array<unsigned char, 16> serial{};
    if (!random(std::as_writable_bytes(std::span(serial)))) return Error::other;
    serial[0] &= 0x7f;
    serial[0] |= 0x01;
    Bignum serial_number(BN_bin2bn(serial.data(), static_cast<int>(serial.size()), nullptr));
    const std::string name(common_name);
    bool ok = serial_number && X509_set_version(certificate.get(), X509_VERSION_3) &&
              BN_to_ASN1_INTEGER(serial_number.get(), X509_get_serialNumber(certificate.get())) &&
              X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -86400) &&
              X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 30L * 86400) && X509_set_pubkey(certificate.get(), key.get());
    const auto subject = X509_get_subject_name(certificate.get());
    ok = ok && X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(name.c_str()), -1, -1, 0) &&
         X509_set_issuer_name(certificate.get(), subject);
    std::string alternatives;
    for (const auto& dns : dns_names) alternatives += (alternatives.empty() ? "DNS:" : ",DNS:") + dns;
    ok = ok && add_extension(certificate.get(), NID_basic_constraints, "critical,CA:FALSE") &&
         add_extension(certificate.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment") &&
         add_extension(certificate.get(), NID_ext_key_usage, "serverAuth") &&
         add_extension(certificate.get(), NID_subject_key_identifier, "hash") &&
         (alternatives.empty() || add_extension(certificate.get(), NID_subject_alt_name, alternatives)) &&
         X509_sign(certificate.get(), key.get(), EVP_sha256()) > 0;
    if (!ok) {
        ERR_clear_error();
        return Error::other;
    }
    Identity identity;
    identity.handle_ = new IdentityPair{certificate.release(), key.release()};
    return identity;
}

Result<Identity::Imported> Identity::import_pkcs12(ConstBytes data, std::string_view password) {
    const unsigned char* at = bytes(data);
    Owned<PKCS12, PKCS12_free> blob(d2i_PKCS12(nullptr, &at, static_cast<long>(data.size())));
    if (!blob) {
        ERR_clear_error();
        return Error::invalid_argument;
    }
    legacy_provider();   // RC2 and 3DES key bags
    const std::string secret(password);
    if (PKCS12_mac_present(blob.get()) && !PKCS12_verify_mac(blob.get(), secret.c_str(), static_cast<int>(secret.size()))) {
        ERR_clear_error();
        return Error::access_denied;
    }
    EVP_PKEY* key = nullptr;
    X509* certificate = nullptr;
    STACK_OF(X509)* others = nullptr;
    if (!PKCS12_parse(blob.get(), secret.c_str(), &key, &certificate, &others)) {
        ERR_clear_error();
        return Error::unsupported;   // an encryption the providers lack
    }
    Imported imported;
    if (certificate && key) {
        Identity identity;
        identity.handle_ = new IdentityPair{certificate, key};
        imported.identities.push_back(std::move(identity));
    }
    else {
        if (certificate) imported.certificates.push_back(CertificateAccess::wrap(certificate));
        EVP_PKEY_free(key);
    }
    if (others) {
        while (sk_X509_num(others) > 0) imported.certificates.push_back(CertificateAccess::wrap(sk_X509_shift(others)));
        sk_X509_free(others);
    }
    return imported;
}

bool Identity::is_pkcs12(ConstBytes data) {
    const unsigned char* at = bytes(data);
    Owned<PKCS12, PKCS12_free> blob(d2i_PKCS12(nullptr, &at, static_cast<long>(data.size())));
    ERR_clear_error();
    return blob != nullptr && at == bytes(data) + data.size();
}

Identity& Identity::operator=(Identity&& other) noexcept {
    if (this != &other) {
        delete pair_of(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
        key_ = std::move(other.key_);
    }
    return *this;
}
Identity::~Identity() { delete pair_of(handle_); }
Certificate Identity::certificate() const {
    if (!handle_) return {};
    X509_up_ref(pair_of(handle_)->certificate);
    return CertificateAccess::wrap(pair_of(handle_)->certificate);
}

// ---- TLS ----

namespace {

int version_number(TlsVersion version) noexcept {
    switch (version) {
    case TlsVersion::tls10: return TLS1_VERSION;
    case TlsVersion::tls11: return TLS1_1_VERSION;
    case TlsVersion::tls12: return TLS1_2_VERSION;
    case TlsVersion::tls13: return TLS1_3_VERSION;
    case TlsVersion::unknown: break;
    }
    return 0;
}
Buffer alpn_wire(const std::vector<std::string>& protocols) {
    Buffer wire;
    for (const auto& protocol : protocols) {
        if (protocol.empty() || protocol.size() > 255) continue;
        wire.push_back(static_cast<std::uint8_t>(protocol.size()));
        wire.insert(wire.end(), protocol.begin(), protocol.end());
    }
    return wire;
}

} // namespace

struct TlsSession::State {
    Owned<SSL_CTX, SSL_CTX_free> context;
    Owned<SSL, SSL_free> ssl;   // owns the two memory BIOs
    BIO* incoming{};            // records from the peer, for OpenSSL to read
    BIO* outgoing{};            // records OpenSSL wrote, for the peer
    Buffer pending;             // outgoing records the transport has not taken yet
    Buffer alpn;                // the server's protocols, in its order of preference
    std::string error;
    bool complete{}, close_notified{};

    Status fail(std::string_view what) {
        error = openssl_error(what);
        return Status::error;
    }
    // Moves OpenSSL's output to the transport; would_block while some of it is still queued.
    Status flush_out(const TlsWrite& write) {
        std::array<std::uint8_t, 16384> chunk;
        for (int got; (got = BIO_read(outgoing, chunk.data(), static_cast<int>(chunk.size()))) > 0;)
            pending.insert(pending.end(), chunk.begin(), chunk.begin() + got);
        while (!pending.empty()) {
            const auto io = write(pending.data(), pending.size());
            if (io.kind == TlsIo::Kind::would_block) return Status::would_block;
            if (io.kind != TlsIo::Kind::ok) return fail("the TLS transport refused a write");
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(std::min(io.size, pending.size())));
            if (io.size == 0) return Status::would_block;
        }
        return Status::ok;
    }
    // Feeds bytes from the transport to OpenSSL.
    Status feed(const TlsRead& read) {
        std::array<std::uint8_t, 16384> chunk;
        const auto io = read(chunk.data(), chunk.size());
        if (io.kind == TlsIo::Kind::would_block) return Status::would_block;
        if (io.kind == TlsIo::Kind::closed || (io.kind == TlsIo::Kind::ok && io.size == 0)) return Status::closed;
        if (io.kind != TlsIo::Kind::ok) return fail("the TLS transport failed a read");
        BIO_write(incoming, chunk.data(), static_cast<int>(io.size));
        return Status::ok;
    }

    static int select_alpn(SSL*, const unsigned char** out, unsigned char* out_size, const unsigned char* offered, unsigned int offered_size,
                           void* argument) {
        auto& self = *static_cast<State*>(argument);
        unsigned char* chosen = nullptr;
        if (self.alpn.empty() ||
            SSL_select_next_proto(&chosen, out_size, self.alpn.data(), static_cast<unsigned int>(self.alpn.size()), offered, offered_size) !=
                OPENSSL_NPN_NEGOTIATED)
            return SSL_TLSEXT_ERR_NOACK;
        *out = chosen;
        return SSL_TLSEXT_ERR_OK;
    }
};

Result<TlsSession> TlsSession::create(const TlsOptions& options, std::string* error_text) {
    const auto refuse = [&](Error error, std::string_view why) -> Result<TlsSession> {
        if (error_text) *error_text = openssl_error(why);
        return error;
    };
    if (options.server && (!options.identity || !*options.identity)) return refuse(Error::invalid_argument, "a TLS server needs an identity");
    auto state = std::make_unique<State>();
    state->context.reset(SSL_CTX_new(options.server ? TLS_server_method() : TLS_client_method()));
    if (!state->context) return refuse(Error::other, "SSL_CTX_new");
    const auto context = state->context.get();
    if (const auto low = version_number(options.min_version)) SSL_CTX_set_min_proto_version(context, low);
    if (const auto high = version_number(options.max_version)) SSL_CTX_set_max_proto_version(context, high);
    if (options.identity && *options.identity) {
        const auto pair = pair_of(options.identity->native());
        if (!pair || SSL_CTX_use_certificate(context, pair->certificate) != 1 || SSL_CTX_use_PrivateKey(context, pair->key) != 1)
            return refuse(Error::invalid_argument, "the TLS identity");
    }
    // The caller judges the peer's chain: no verification here; a server asking for a client certificate accepts any.
    if (options.server && options.request_client_certificate)
        SSL_CTX_set_verify(context, SSL_VERIFY_PEER, [](int, X509_STORE_CTX*) { return 1; });
    else SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    state->ssl.reset(SSL_new(context));
    state->incoming = BIO_new(BIO_s_mem());
    state->outgoing = BIO_new(BIO_s_mem());
    if (!state->ssl || !state->incoming || !state->outgoing) {
        BIO_free(state->incoming);
        BIO_free(state->outgoing);
        return refuse(Error::other, "SSL_new");
    }
    SSL_set_bio(state->ssl.get(), state->incoming, state->outgoing);
    if (options.server) {
        state->alpn = alpn_wire(options.alpn);
        if (!state->alpn.empty()) SSL_CTX_set_alpn_select_cb(context, &State::select_alpn, state.get());
        SSL_set_accept_state(state->ssl.get());
    } else {
        const auto wire = alpn_wire(options.alpn);
        if (!wire.empty() && SSL_set_alpn_protos(state->ssl.get(), wire.data(), static_cast<unsigned int>(wire.size())) != 0)
            return refuse(Error::invalid_argument, "the ALPN protocols");
        if (!options.peer_name.empty() && !numeric_host(options.peer_name)) SSL_set_tlsext_host_name(state->ssl.get(), options.peer_name.c_str());
        SSL_set_connect_state(state->ssl.get());
    }
    return TlsSession(std::move(state));
}

TlsSession::TlsSession(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
TlsSession::TlsSession() = default;
TlsSession::TlsSession(TlsSession&&) noexcept = default;
TlsSession& TlsSession::operator=(TlsSession&&) noexcept = default;
TlsSession::~TlsSession() = default;

TlsSession::Status TlsSession::handshake(const TlsRead& read, const TlsWrite& write) {
    if (!state_) return Status::error;
    auto& state = *state_;
    for (;;) {
        const auto result = state.complete ? 1 : SSL_do_handshake(state.ssl.get());
        const auto flushed = state.flush_out(write);
        if (flushed == Status::error) return Status::error;
        if (result == 1) {
            state.complete = true;
            return flushed;
        }
        switch (SSL_get_error(state.ssl.get(), result)) {
        case SSL_ERROR_WANT_READ: {
            if (flushed == Status::would_block) return Status::would_block;
            const auto fed = state.feed(read);
            if (fed == Status::would_block || fed == Status::error) return fed;
            if (fed == Status::closed) return state.fail("the peer closed the connection during the TLS handshake");
            break;
        }
        case SSL_ERROR_WANT_WRITE: break;
        default: return state.fail("TLS handshake");
        }
    }
}

TlsSession::Status TlsSession::read(const TlsRead& read, const TlsWrite& write, std::uint8_t* buffer, std::size_t size, std::size_t& got) {
    got = 0;
    if (!state_) return Status::error;
    auto& state = *state_;
    if (state.close_notified) return Status::closed;
    for (;;) {
        const auto result = SSL_read(state.ssl.get(), buffer, static_cast<int>(std::min<std::size_t>(size, 0x7fffffff)));
        if (result > 0) {
            got = static_cast<std::size_t>(result);
            return state.flush_out(write) == Status::error ? Status::error : Status::ok;
        }
        switch (SSL_get_error(state.ssl.get(), result)) {
        case SSL_ERROR_ZERO_RETURN:
            state.close_notified = true;
            return Status::closed;
        case SSL_ERROR_WANT_READ: {
            if (state.flush_out(write) == Status::error) return Status::error;   // a key update or ticket answer
            const auto fed = state.feed(read);
            if (fed != Status::ok) return fed;
            break;
        }
        case SSL_ERROR_WANT_WRITE:
            if (state.flush_out(write) != Status::ok) return Status::would_block;
            break;
        default: return state.fail("TLS read");
        }
    }
}

TlsSession::Status TlsSession::write(const TlsWrite& write, const std::uint8_t* data, std::size_t size, std::size_t& accepted) {
    accepted = 0;
    if (!state_) return Status::error;
    auto& state = *state_;
    while (accepted < size) {
        const auto result = SSL_write(state.ssl.get(), data + accepted, static_cast<int>(std::min<std::size_t>(size - accepted, 0x7fffffff)));
        if (result <= 0) return state.fail("TLS write");
        accepted += static_cast<std::size_t>(result);
    }
    return state.flush_out(write);
}

TlsSession::Status TlsSession::flush(const TlsWrite& write) { return state_ ? state_->flush_out(write) : Status::error; }

TlsSession::Status TlsSession::shutdown(const TlsWrite& write) {
    if (!state_) return Status::ok;
    SSL_shutdown(state_->ssl.get());
    const auto flushed = state_->flush_out(write);
    ERR_clear_error();
    return flushed == Status::error ? Status::ok : flushed;   // best effort
}

bool TlsSession::complete() const noexcept { return state_ && state_->complete; }
std::size_t TlsSession::buffered() const noexcept { return state_ ? static_cast<std::size_t>(SSL_pending(state_->ssl.get())) : 0; }

std::vector<Certificate> TlsSession::peer_certificates() const {
    std::vector<Certificate> chain;
    if (!state_) return chain;
    const auto leaf = SSL_get0_peer_certificate(state_->ssl.get());
    if (leaf) {
        X509_up_ref(leaf);
        chain.push_back(CertificateAccess::wrap(leaf));
    }
    if (const auto others = SSL_get_peer_cert_chain(state_->ssl.get()))   // includes the leaf on a client
        for (int index = 0; index < sk_X509_num(others); ++index) {
            const auto certificate = sk_X509_value(others, index);
            if (leaf && X509_cmp(certificate, leaf) == 0) continue;
            X509_up_ref(certificate);
            chain.push_back(CertificateAccess::wrap(certificate));
        }
    return chain;
}

std::string TlsSession::negotiated_alpn() const {
    if (!state_) return {};
    const unsigned char* protocol = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(state_->ssl.get(), &protocol, &size);
    return protocol ? std::string(reinterpret_cast<const char*>(protocol), size) : std::string{};
}

TlsVersion TlsSession::negotiated_version() const {
    if (!complete()) return TlsVersion::unknown;
    switch (SSL_version(state_->ssl.get())) {
    case TLS1_VERSION: return TlsVersion::tls10;
    case TLS1_1_VERSION: return TlsVersion::tls11;
    case TLS1_2_VERSION: return TlsVersion::tls12;
    case TLS1_3_VERSION: return TlsVersion::tls13;
    default: return TlsVersion::unknown;
    }
}

std::uint16_t TlsSession::negotiated_cipher_suite() const {
    if (!complete()) return 0;
    const auto cipher = SSL_get_current_cipher(state_->ssl.get());
    return cipher ? SSL_CIPHER_get_protocol_id(cipher) : 0;
}

// The suites a default client offers, in OpenSSL's order of preference.
Result<std::vector<std::uint16_t>> tls_cipher_suites() {
    Owned<SSL_CTX, SSL_CTX_free> context(SSL_CTX_new(TLS_client_method()));
    Owned<SSL, SSL_free> ssl(context ? SSL_new(context.get()) : nullptr);
    if (!ssl) return Error::unsupported;
    STACK_OF(SSL_CIPHER)* ciphers = SSL_get1_supported_ciphers(ssl.get());
    if (!ciphers) return Error::unsupported;
    std::vector<std::uint16_t> suites;
    for (int index = 0; index < sk_SSL_CIPHER_num(ciphers); ++index) suites.push_back(SSL_CIPHER_get_protocol_id(sk_SSL_CIPHER_value(ciphers, index)));
    sk_SSL_CIPHER_free(ciphers);
    if (suites.empty()) return Error::unsupported;
    return suites;
}

const std::string& TlsSession::error() const noexcept {
    static const std::string none{"no TLS session"};
    return state_ ? state_->error : none;
}
bool TlsSession::close_notified() const noexcept { return state_ && state_->close_notified; }

} // namespace alchemy::host::crypto
