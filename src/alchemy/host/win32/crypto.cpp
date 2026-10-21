#include "alchemy/host/crypto.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <bcrypt.h>
#include <dpapi.h>
#include <ncrypt.h>
#include <wincrypt.h>
#define SECURITY_WIN32
#define SCHANNEL_USE_BLACKLISTS
#include <subauth.h>
#include <schannel.h>
#include <security.h>
#include <sspi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "secur32.lib")

namespace alchemy::host::crypto {

Status random(Bytes buffer) {
    while (!buffer.empty()) {
        const auto chunk = std::min<std::size_t>(buffer.size(), std::numeric_limits<ULONG>::max());
        const auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(chunk),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) return Error::io;
        buffer = buffer.subspan(chunk);
    }
    return {};
}

void secure_zero(Bytes buffer) noexcept {
    if (!buffer.empty()) SecureZeroMemory(buffer.data(), buffer.size());
}

namespace {

Result<DATA_BLOB> blob(ConstBytes bytes) {
    if (bytes.size() > std::numeric_limits<DWORD>::max()) return Error::out_of_range;
    return DATA_BLOB{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data()))};
}

// The DPAPI output, copied out (and wiped when it is plaintext).
std::vector<std::uint8_t> take(DATA_BLOB& output, bool wipe) {
    std::vector<std::uint8_t> result(output.pbData, output.pbData + output.cbData);
    if (wipe) SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
}

} // namespace

Result<std::vector<std::uint8_t>> protect(ConstBytes data, ConstBytes entropy, std::string_view description) {
    auto input = blob(data), salt = blob(entropy);
    if (!input || !salt) return Error::out_of_range;
    const auto text = win32::widen(description);
    DATA_BLOB output{};
    if (!CryptProtectData(&*input, text.empty() ? nullptr : text.c_str(), entropy.empty() ? nullptr : &*salt, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        return win32::last_error();
    return take(output, false);
}

Result<std::vector<std::uint8_t>> unprotect(ConstBytes data, ConstBytes entropy) {
    auto input = blob(data), salt = blob(entropy);
    if (!input || !salt) return Error::out_of_range;
    DATA_BLOB output{};
    if (!CryptUnprotectData(&*input, nullptr, entropy.empty() ? nullptr : &*salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        return win32::last_error();
    return take(output, true);
}

Result<BlockCipher> BlockCipher::create(BlockAlgorithm algorithm, ConstBytes key) {
    const wchar_t* name{};
    std::size_t block{};
    switch (algorithm) {
    case BlockAlgorithm::aes:
        if (key.size() != 16 && key.size() != 24 && key.size() != 32) return Error::invalid_argument;
        name = BCRYPT_AES_ALGORITHM; block = 16; break;
    case BlockAlgorithm::des:
        if (key.size() != 8) return Error::invalid_argument;
        name = BCRYPT_DES_ALGORITHM; block = 8; break;
    case BlockAlgorithm::triple_des:
        if (key.size() != 24) return Error::invalid_argument;
        name = BCRYPT_3DES_ALGORITHM; block = 8; break;
    default: return Error::unsupported;
    }
    BlockCipher cipher;
    cipher.block_ = block;
    BCRYPT_ALG_HANDLE provider{};
    if (BCryptOpenAlgorithmProvider(&provider, name, nullptr, 0) < 0) return Error::unsupported;
    cipher.algorithm_ = provider;
    static const wchar_t ecb[] = BCRYPT_CHAIN_MODE_ECB;
    if (BCryptSetProperty(provider, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(ecb)), sizeof(ecb), 0) < 0)
        return Error::unsupported;
    std::vector<std::uint8_t> secret(reinterpret_cast<const std::uint8_t*>(key.data()), reinterpret_cast<const std::uint8_t*>(key.data()) + key.size());
    BCRYPT_KEY_HANDLE handle{};
    const auto status = BCryptGenerateSymmetricKey(provider, &handle, nullptr, 0, secret.data(), static_cast<ULONG>(secret.size()), 0);
    SecureZeroMemory(secret.data(), secret.size());
    if (status < 0) return Error::invalid_argument;
    cipher.key_ = handle;
    return cipher;
}

namespace {

void destroy(void*& algorithm, void*& key) noexcept {
    if (key) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key));
    if (algorithm) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm), 0);
    key = algorithm = nullptr;
}

} // namespace

BlockCipher& BlockCipher::operator=(BlockCipher&& other) noexcept {
    if (this != &other) {
        destroy(algorithm_, key_);
        algorithm_ = std::exchange(other.algorithm_, nullptr);
        key_ = std::exchange(other.key_, nullptr);
        block_ = other.block_;
    }
    return *this;
}

BlockCipher::~BlockCipher() { destroy(algorithm_, key_); }

namespace {

Status blocks(void* key, std::size_t block, ConstBytes input, Bytes output, bool encrypt) {
    if (!key) return Error::bad_handle;
    if (input.size() != output.size() || !block || input.size() % block || input.size() > std::numeric_limits<ULONG>::max())
        return Error::invalid_argument;
    if (input.empty()) return {};
    ULONG written{};
    auto* in = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data()));
    auto* out = reinterpret_cast<PUCHAR>(output.data());
    const auto size = static_cast<ULONG>(input.size());
    const auto status = encrypt ? BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key), in, size, nullptr, nullptr, 0, out, size, &written, 0)
                                : BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key), in, size, nullptr, nullptr, 0, out, size, &written, 0);
    if (status < 0 || written != size) return Error::io;
    return {};
}

} // namespace

Status BlockCipher::encrypt(ConstBytes input, Bytes output) const { return blocks(key_, block_, input, output, true); }
Status BlockCipher::decrypt(ConstBytes input, Bytes output) const { return blocks(key_, block_, input, output, false); }

Status pbkdf2(Digest digest, ConstBytes password, ConstBytes salt, std::uint32_t rounds, Bytes output) {
    const wchar_t* name{};
    switch (digest) {
    case Digest::sha1: name = BCRYPT_SHA1_ALGORITHM; break;
    case Digest::sha256: name = BCRYPT_SHA256_ALGORITHM; break;
    case Digest::sha384: name = BCRYPT_SHA384_ALGORITHM; break;
    case Digest::sha512: name = BCRYPT_SHA512_ALGORITHM; break;
    case Digest::sha224: return Error::unsupported;   // no CNG provider
    case Digest::md5: return Error::unsupported;
    }
    if (!name) return Error::invalid_argument;
    if (password.size() > std::numeric_limits<ULONG>::max() || salt.size() > std::numeric_limits<ULONG>::max() ||
        output.size() > std::numeric_limits<ULONG>::max())
        return Error::out_of_range;
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, name, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) return Error::unsupported;
    std::vector<std::uint8_t> secret(reinterpret_cast<const std::uint8_t*>(password.data()), reinterpret_cast<const std::uint8_t*>(password.data()) + password.size());
    std::vector<std::uint8_t> seasoning(reinterpret_cast<const std::uint8_t*>(salt.data()), reinterpret_cast<const std::uint8_t*>(salt.data()) + salt.size());
    const auto result = BCryptDeriveKeyPBKDF2(algorithm, secret.empty() ? nullptr : secret.data(), static_cast<ULONG>(secret.size()),
        seasoning.empty() ? nullptr : seasoning.data(), static_cast<ULONG>(seasoning.size()), rounds,
        reinterpret_cast<PUCHAR>(output.data()), static_cast<ULONG>(output.size()), 0);
    SecureZeroMemory(secret.data(), secret.size());
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (result < 0) return Error::invalid_argument;
    return {};
}

// ---- Digests and asymmetric keys (CNG) ----

namespace {

using Buffer = std::vector<std::uint8_t>;

const wchar_t* digest_name(Digest digest) {
    switch (digest) {
    case Digest::sha1: return BCRYPT_SHA1_ALGORITHM;
    case Digest::sha256: return BCRYPT_SHA256_ALGORITHM;
    case Digest::sha384: return BCRYPT_SHA384_ALGORITHM;
    case Digest::sha512: return BCRYPT_SHA512_ALGORITHM;
    case Digest::md5: return BCRYPT_MD5_ALGORITHM;
    case Digest::sha224: return nullptr;   // no CNG implementation
    }
    return nullptr;
}

// Cached algorithm providers (BCryptOpenAlgorithmProvider is expensive).
BCRYPT_ALG_HANDLE provider(const wchar_t* name, ULONG flags = 0) {
    static std::mutex mutex;
    static std::map<std::pair<std::wstring, ULONG>, BCRYPT_ALG_HANDLE> providers;
    std::lock_guard lock(mutex);
    const auto key = std::make_pair(std::wstring(name), flags);
    if (const auto found = providers.find(key); found != providers.end()) return found->second;
    BCRYPT_ALG_HANDLE handle{};
    if (BCryptOpenAlgorithmProvider(&handle, name, nullptr, flags) != 0) return nullptr;
    providers.emplace(key, handle);
    return handle;
}

struct Curve { std::size_t coordinate; unsigned bits; const wchar_t* ecdsa; const wchar_t* ecdh;
               ULONG ecdsa_public, ecdsa_private, ecdh_public, ecdh_private; };
const Curve curves[] = {
    {32, 256, BCRYPT_ECDSA_P256_ALGORITHM, BCRYPT_ECDH_P256_ALGORITHM,
     BCRYPT_ECDSA_PUBLIC_P256_MAGIC, BCRYPT_ECDSA_PRIVATE_P256_MAGIC, BCRYPT_ECDH_PUBLIC_P256_MAGIC, BCRYPT_ECDH_PRIVATE_P256_MAGIC},
    {48, 384, BCRYPT_ECDSA_P384_ALGORITHM, BCRYPT_ECDH_P384_ALGORITHM,
     BCRYPT_ECDSA_PUBLIC_P384_MAGIC, BCRYPT_ECDSA_PRIVATE_P384_MAGIC, BCRYPT_ECDH_PUBLIC_P384_MAGIC, BCRYPT_ECDH_PRIVATE_P384_MAGIC},
    {66, 521, BCRYPT_ECDSA_P521_ALGORITHM, BCRYPT_ECDH_P521_ALGORITHM,
     BCRYPT_ECDSA_PUBLIC_P521_MAGIC, BCRYPT_ECDSA_PRIVATE_P521_MAGIC, BCRYPT_ECDH_PUBLIC_P521_MAGIC, BCRYPT_ECDH_PRIVATE_P521_MAGIC}};

const Curve* curve_by_coordinate(std::size_t coordinate) {
    for (const auto& curve : curves) if (curve.coordinate == coordinate) return &curve;
    return nullptr;
}

struct KeyHandle {
    BCRYPT_KEY_HANDLE handle{};
    KeyHandle() = default;
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;
    ~KeyHandle() { if (handle) BCryptDestroyKey(handle); }
};

Buffer padded(const Buffer& value, std::size_t size) {
    if (value.size() >= size) return Buffer(value.end() - static_cast<std::ptrdiff_t>(size), value.end());
    Buffer result(size - value.size(), 0);
    result.insert(result.end(), value.begin(), value.end());
    return result;
}
void append(Buffer& out, const Buffer& value) { out.insert(out.end(), value.begin(), value.end()); }

// The RSA public or full private blob; Error::bad_handle when CNG refuses it.
Status import(const RsaKey& key, bool use_private, KeyHandle& out) {
    Buffer blob;
    const auto modulus = key.n.size();
    BCRYPT_RSAKEY_BLOB header{};
    header.BitLength = key.bits;
    header.cbPublicExp = static_cast<ULONG>(key.e.size());
    header.cbModulus = static_cast<ULONG>(modulus);
    const wchar_t* type{};
    if (use_private) {
        const auto prime = std::max(key.p.size(), key.q.size());
        header.Magic = BCRYPT_RSAFULLPRIVATE_MAGIC;
        header.cbPrime1 = header.cbPrime2 = static_cast<ULONG>(prime);
        blob.resize(sizeof(header));
        std::memcpy(blob.data(), &header, sizeof(header));
        append(blob, key.e); append(blob, key.n);
        append(blob, padded(key.p, prime)); append(blob, padded(key.q, prime));
        append(blob, padded(key.dp, prime)); append(blob, padded(key.dq, prime));
        append(blob, padded(key.qinv, prime)); append(blob, padded(key.d, modulus));
        type = BCRYPT_RSAFULLPRIVATE_BLOB;
    } else {
        header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
        blob.resize(sizeof(header));
        std::memcpy(blob.data(), &header, sizeof(header));
        append(blob, key.e); append(blob, key.n);
        type = BCRYPT_RSAPUBLIC_BLOB;
    }
    const auto algorithm = provider(BCRYPT_RSA_ALGORITHM);
    const bool ok = algorithm &&
        BCryptImportKeyPair(algorithm, nullptr, type, &out.handle, blob.data(), static_cast<ULONG>(blob.size()), 0) == 0;
    SecureZeroMemory(blob.data(), blob.size());
    if (!ok) { out.handle = nullptr; return Error::bad_handle; }
    return {};
}

// The ECDSA or ECDH blob.
Status import(const EcKey& key, bool use_private, bool ecdh, KeyHandle& out) {
    const auto curve = curve_by_coordinate(key.coordinate);
    if (!curve) return Error::bad_handle;
    Buffer blob;
    BCRYPT_ECCKEY_BLOB header{};
    header.cbKey = static_cast<ULONG>(key.coordinate);
    header.dwMagic = ecdh ? (use_private ? curve->ecdh_private : curve->ecdh_public) : (use_private ? curve->ecdsa_private : curve->ecdsa_public);
    blob.resize(sizeof(header));
    std::memcpy(blob.data(), &header, sizeof(header));
    append(blob, padded(key.x, key.coordinate)); append(blob, padded(key.y, key.coordinate));
    if (use_private) append(blob, padded(key.k, key.coordinate));
    const auto algorithm = provider(ecdh ? curve->ecdh : curve->ecdsa);
    const bool ok = algorithm && BCryptImportKeyPair(algorithm, nullptr, use_private ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB,
                                                     &out.handle, blob.data(), static_cast<ULONG>(blob.size()), 0) == 0;
    SecureZeroMemory(blob.data(), blob.size());
    if (!ok) { out.handle = nullptr; return Error::bad_handle; }
    return {};
}

Buffer bytes_of(ConstBytes data) {
    return Buffer(reinterpret_cast<const std::uint8_t*>(data.data()), reinterpret_cast<const std::uint8_t*>(data.data()) + data.size());
}

// Leading zero bytes dropped (the minimal big-endian form of an integer).
Buffer unsigned_integer(Buffer value) {
    const auto first = std::find_if(value.begin(), value.end(), [](std::uint8_t byte) { return byte != 0; });
    value.erase(value.begin(), first);
    return value;
}

} // namespace

bool supports(Digest digest) noexcept { return digest_name(digest) != nullptr; }

Result<std::vector<std::uint8_t>> hash(Digest digest, ConstBytes data) {
    const auto name = digest_name(digest);
    if (!name) return Error::unsupported;
    const auto handle = provider(name);
    if (!handle) return Error::unsupported;
    BCRYPT_HASH_HANDLE object{};
    if (BCryptCreateHash(handle, &object, nullptr, 0, nullptr, 0, 0) != 0) return Error::io;
    ULONG length{}, ignored{};
    BCryptGetProperty(handle, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &ignored, 0);
    Buffer result(length);
    const bool ok = BCryptHashData(object, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())), static_cast<ULONG>(data.size()), 0) == 0 &&
        BCryptFinishHash(object, result.data(), length, 0) == 0;
    BCryptDestroyHash(object);
    if (!ok) return Error::io;
    return result;
}

Status check(const RsaKey& key, bool use_private) {
    KeyHandle handle;
    return import(key, use_private, handle);
}

Status check(const EcKey& key, bool use_private) {
    KeyHandle handle;
    return import(key, use_private, false, handle);
}

Result<RsaKey> generate_rsa(unsigned bits) {
    KeyHandle handle;
    const auto algorithm = provider(BCRYPT_RSA_ALGORITHM);
    if (!algorithm) return Error::unsupported;
    if (BCryptGenerateKeyPair(algorithm, &handle.handle, bits, 0) != 0 || BCryptFinalizeKeyPair(handle.handle, 0) != 0)
        return Error::invalid_argument;
    ULONG size{};
    if (BCryptExportKey(handle.handle, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, nullptr, 0, &size, 0) != 0) return Error::io;
    Buffer blob(size);
    if (BCryptExportKey(handle.handle, nullptr, BCRYPT_RSAFULLPRIVATE_BLOB, blob.data(), size, &size, 0) != 0) return Error::io;
    BCRYPT_RSAKEY_BLOB header;
    std::memcpy(&header, blob.data(), sizeof(header));
    RsaKey key;
    key.bits = header.BitLength;
    std::size_t at = sizeof(header);
    const auto take = [&](std::size_t count) {
        Buffer value(blob.begin() + static_cast<std::ptrdiff_t>(at), blob.begin() + static_cast<std::ptrdiff_t>(at + count));
        at += count;
        return value;
    };
    key.e = unsigned_integer(take(header.cbPublicExp));
    key.n = take(header.cbModulus);
    key.p = take(header.cbPrime1); key.q = take(header.cbPrime2);
    key.dp = take(header.cbPrime1); key.dq = take(header.cbPrime2);
    key.qinv = take(header.cbPrime1); key.d = take(header.cbModulus);
    SecureZeroMemory(blob.data(), blob.size());
    return key;
}

Result<EcKey> generate_ec(std::size_t coordinate) {
    const auto curve = curve_by_coordinate(coordinate);
    if (!curve) return Error::invalid_argument;
    const auto algorithm = provider(curve->ecdsa);
    if (!algorithm) return Error::unsupported;
    KeyHandle handle;
    if (BCryptGenerateKeyPair(algorithm, &handle.handle, curve->bits, 0) != 0 || BCryptFinalizeKeyPair(handle.handle, 0) != 0)
        return Error::io;
    ULONG size{};
    if (BCryptExportKey(handle.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, nullptr, 0, &size, 0) != 0) return Error::io;
    Buffer blob(size);
    if (BCryptExportKey(handle.handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), size, &size, 0) != 0) return Error::io;
    BCRYPT_ECCKEY_BLOB header;
    std::memcpy(&header, blob.data(), sizeof(header));
    const std::size_t c = header.cbKey;
    EcKey key;
    key.coordinate = c;
    const auto at = blob.begin() + sizeof(header);
    key.x.assign(at, at + static_cast<std::ptrdiff_t>(c));
    key.y.assign(at + static_cast<std::ptrdiff_t>(c), at + static_cast<std::ptrdiff_t>(2 * c));
    key.k.assign(at + static_cast<std::ptrdiff_t>(2 * c), at + static_cast<std::ptrdiff_t>(3 * c));
    SecureZeroMemory(blob.data(), blob.size());
    return key;
}

Result<std::vector<std::uint8_t>> rsa_sign(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes hash) {
    if (padding != RsaPadding::pkcs1 && padding != RsaPadding::pss) return Error::invalid_argument;
    if (digest && !digest_name(*digest)) return Error::unsupported;
    if (padding == RsaPadding::pss && !digest) return Error::invalid_argument;
    KeyHandle handle;
    if (const auto status = import(key, true, handle); !status) return status.error();
    auto input = bytes_of(hash);
    Buffer output(key.n.size());
    ULONG size{};
    NTSTATUS result;
    if (padding == RsaPadding::pss) {
        BCRYPT_PSS_PADDING_INFO info{digest_name(*digest), static_cast<ULONG>(input.size())};
        result = BCryptSignHash(handle.handle, &info, input.data(), static_cast<ULONG>(input.size()), output.data(),
                                static_cast<ULONG>(output.size()), &size, BCRYPT_PAD_PSS);
    } else {
        BCRYPT_PKCS1_PADDING_INFO info{digest ? digest_name(*digest) : nullptr};
        result = BCryptSignHash(handle.handle, &info, input.data(), static_cast<ULONG>(input.size()), output.data(),
                                static_cast<ULONG>(output.size()), &size, BCRYPT_PAD_PKCS1);
    }
    if (result != 0) return Error::io;
    output.resize(size);
    return output;
}

Status rsa_verify(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes hash, ConstBytes signature) {
    if (padding != RsaPadding::pkcs1 && padding != RsaPadding::pss) return Error::invalid_argument;
    if (digest && !digest_name(*digest)) return Error::unsupported;
    if (padding == RsaPadding::pss && !digest) return Error::invalid_argument;
    KeyHandle handle;
    if (const auto status = import(key, false, handle); !status) return status;
    auto input = bytes_of(hash), sig = bytes_of(signature);
    NTSTATUS result;
    if (padding == RsaPadding::pss) {
        BCRYPT_PSS_PADDING_INFO info{digest_name(*digest), static_cast<ULONG>(input.size())};
        result = BCryptVerifySignature(handle.handle, &info, input.data(), static_cast<ULONG>(input.size()), sig.data(),
                                       static_cast<ULONG>(sig.size()), BCRYPT_PAD_PSS);
    } else {
        BCRYPT_PKCS1_PADDING_INFO info{digest ? digest_name(*digest) : nullptr};
        result = BCryptVerifySignature(handle.handle, &info, input.data(), static_cast<ULONG>(input.size()), sig.data(),
                                       static_cast<ULONG>(sig.size()), BCRYPT_PAD_PKCS1);
    }
    if (result != 0) return Error::access_denied;
    return {};
}

namespace {

ULONG padding_flag(RsaPadding padding) {
    switch (padding) {
    case RsaPadding::none: return BCRYPT_PAD_NONE;
    case RsaPadding::pkcs1: return BCRYPT_PAD_PKCS1;
    case RsaPadding::oaep: return BCRYPT_PAD_OAEP;
    case RsaPadding::pss: return 0;
    }
    return 0;
}

Result<std::vector<std::uint8_t>> rsa_crypt(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes data, bool encrypt) {
    if (padding == RsaPadding::pss) return Error::invalid_argument;
    if (padding == RsaPadding::oaep && (!digest || !digest_name(*digest))) return digest ? Error::unsupported : Error::invalid_argument;
    KeyHandle handle;
    if (const auto status = import(key, !encrypt, handle); !status) return status.error();
    auto input = bytes_of(data);
    Buffer output(key.n.size());
    ULONG size{};
    BCRYPT_OAEP_PADDING_INFO oaep{padding == RsaPadding::oaep ? digest_name(*digest) : nullptr, nullptr, 0};
    void* info = padding == RsaPadding::oaep ? &oaep : nullptr;
    const auto result = encrypt
        ? BCryptEncrypt(handle.handle, input.data(), static_cast<ULONG>(input.size()), info, nullptr, 0, output.data(),
                        static_cast<ULONG>(output.size()), &size, padding_flag(padding))
        : BCryptDecrypt(handle.handle, input.data(), static_cast<ULONG>(input.size()), info, nullptr, 0, output.data(),
                        static_cast<ULONG>(output.size()), &size, padding_flag(padding));
    if (result != 0) return Error::io;
    output.resize(size);
    return output;
}

} // namespace

Result<std::vector<std::uint8_t>> rsa_encrypt(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes data) {
    return rsa_crypt(key, padding, digest, data, true);
}

Result<std::vector<std::uint8_t>> rsa_decrypt(const RsaKey& key, RsaPadding padding, std::optional<Digest> digest, ConstBytes data) {
    return rsa_crypt(key, padding, digest, data, false);
}

Result<std::vector<std::uint8_t>> ecdsa_sign(const EcKey& key, ConstBytes hash) {
    KeyHandle handle;
    if (const auto status = import(key, true, false, handle); !status) return status.error();
    auto input = bytes_of(hash);
    Buffer raw(key.coordinate * 2);
    ULONG size{};
    if (BCryptSignHash(handle.handle, nullptr, input.data(), static_cast<ULONG>(input.size()), raw.data(), static_cast<ULONG>(raw.size()), &size, 0) != 0)
        return Error::io;
    raw.resize(size);
    return raw;
}

Status ecdsa_verify(const EcKey& key, ConstBytes hash, ConstBytes signature) {
    KeyHandle handle;
    if (const auto status = import(key, false, false, handle); !status) return status;
    auto input = bytes_of(hash), raw = bytes_of(signature);
    if (BCryptVerifySignature(handle.handle, nullptr, input.data(), static_cast<ULONG>(input.size()), raw.data(), static_cast<ULONG>(raw.size()), 0) != 0)
        return Error::access_denied;
    return {};
}

Result<std::vector<std::uint8_t>> ecdh(const EcKey& key, const EcKey& peer) {
    if (peer.coordinate != key.coordinate) return Error::invalid_argument;
    KeyHandle mine, theirs;
    if (const auto status = import(key, true, true, mine); !status) return status.error();
    if (const auto status = import(peer, false, true, theirs); !status) return status.error();
    BCRYPT_SECRET_HANDLE secret{};
    if (BCryptSecretAgreement(mine.handle, theirs.handle, &secret, 0) != 0) return Error::io;
    ULONG size{};
    Buffer output(key.coordinate);
    const auto result = BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, output.data(), static_cast<ULONG>(output.size()), &size, 0);
    BCryptDestroySecret(secret);
    if (result != 0) return Error::io;
    output.resize(size);
    std::reverse(output.begin(), output.end());   // CNG returns the raw secret little-endian
    return output;
}

// ---- Certificates and trust (CryptoAPI) ----

struct CertificateAccess {
    static Certificate wrap(PCCERT_CONTEXT context) noexcept {
        Certificate result;
        result.handle_ = const_cast<CERT_CONTEXT*>(context);
        return result;
    }
    static PCCERT_CONTEXT context(const Certificate& certificate) noexcept { return static_cast<PCCERT_CONTEXT>(certificate.handle_); }
};

namespace {

constexpr DWORD certificate_encoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;
constexpr std::int64_t unix_epoch_ticks = 116444736000000000LL;   // 1970-01-01 in FILETIME ticks

PCCERT_CONTEXT context_of(const Certificate& certificate) { return CertificateAccess::context(certificate); }
// A Certificate holding its own reference to `context`.
Certificate share(PCCERT_CONTEXT context) { return CertificateAccess::wrap(CertDuplicateCertificateContext(context)); }

std::int64_t unix_ns(const FILETIME& time) {
    const auto ticks = static_cast<std::int64_t>((static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime);
    return (ticks - unix_epoch_ticks) * 100;
}
FILETIME file_time(std::int64_t ns) {
    const auto ticks = static_cast<std::uint64_t>(ns / 100 + unix_epoch_ticks);
    return {static_cast<DWORD>(ticks & 0xffffffffU), static_cast<DWORD>(ticks >> 32)};
}

std::string name_string(PCCERT_CONTEXT context, DWORD type, const void* parameter, DWORD flags = 0) {
    if (!context) return {};
    std::array<wchar_t, 1024> buffer{};
    const auto size = CertGetNameStringW(context, type, flags, const_cast<void*>(parameter), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size <= 1) return {};
    return win32::narrow(std::wstring_view(buffer.data(), size - 1));
}

struct StoreHandle {
    HCERTSTORE store{};
    ~StoreHandle() { if (store) CertCloseStore(store, 0); }
};
struct ChainHandle {
    PCCERT_CHAIN_CONTEXT chain{};
    ~ChainHandle() { if (chain) CertFreeCertificateChain(chain); }
};
struct EngineHandle {
    HCERTCHAINENGINE engine{};
    ~EngineHandle() { if (engine) CertFreeCertificateChainEngine(engine); }
};

HCERTSTORE memory_store() { return CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr); }

bool add(HCERTSTORE store, const Certificate& certificate) {
    return CertAddCertificateContextToStore(store, context_of(certificate), CERT_STORE_ADD_USE_EXISTING, nullptr) != FALSE;
}

// Deletes a persisted CNG key (an identity's).
void delete_key(const std::string& name) {
    if (name.empty()) return;
    NCRYPT_PROV_HANDLE provider{};
    if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return;
    NCRYPT_KEY_HANDLE key{};
    if (NCryptOpenKey(provider, &key, win32::widen(name).c_str(), 0, 0) == ERROR_SUCCESS) NCryptDeleteKey(key, 0);
    NCryptFreeObject(provider);
}

} // namespace

Result<Certificate> Certificate::parse(ConstBytes der) {
    if (der.empty() || der.size() > std::numeric_limits<DWORD>::max()) return Error::invalid_argument;
    const auto context = CertCreateCertificateContext(certificate_encoding, reinterpret_cast<const BYTE*>(der.data()), static_cast<DWORD>(der.size()));
    if (!context) return Error::invalid_argument;
    return CertificateAccess::wrap(context);
}

Certificate::Certificate(const Certificate& other)
    : handle_(other.handle_ ? const_cast<CERT_CONTEXT*>(CertDuplicateCertificateContext(static_cast<PCCERT_CONTEXT>(other.handle_))) : nullptr) {}

Certificate& Certificate::operator=(const Certificate& other) {
    if (this != &other) {
        Certificate copy(other);
        *this = std::move(copy);
    }
    return *this;
}

Certificate& Certificate::operator=(Certificate&& other) noexcept {
    if (this != &other) {
        if (handle_) CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(handle_));
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Certificate::~Certificate() { if (handle_) CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(handle_)); }

std::vector<std::uint8_t> Certificate::der() const {
    const auto context = context_of(*this);
    if (!context) return {};
    return std::vector<std::uint8_t>(context->pbCertEncoded, context->pbCertEncoded + context->cbCertEncoded);
}

std::string Certificate::common_name() const { return name_string(context_of(*this), CERT_NAME_ATTR_TYPE, szOID_COMMON_NAME); }
std::string Certificate::display_name() const { return name_string(context_of(*this), CERT_NAME_SIMPLE_DISPLAY_TYPE, nullptr); }
std::string Certificate::email() const { return name_string(context_of(*this), CERT_NAME_EMAIL_TYPE, nullptr); }

std::vector<std::string> Certificate::dns_names() const {
    std::vector<std::string> result;
    const auto context = context_of(*this);
    if (!context) return result;
    const auto extension = CertFindExtension(szOID_SUBJECT_ALT_NAME2, context->pCertInfo->cExtension, context->pCertInfo->rgExtension);
    if (!extension) return result;
    CERT_ALT_NAME_INFO* info{};
    DWORD size{};
    if (!CryptDecodeObjectEx(certificate_encoding, X509_ALTERNATE_NAME, extension->Value.pbData, extension->Value.cbData,
                             CRYPT_DECODE_ALLOC_FLAG, nullptr, &info, &size))
        return result;
    for (DWORD index = 0; index < info->cAltEntry; ++index)
        if (info->rgAltEntry[index].dwAltNameChoice == CERT_ALT_NAME_DNS_NAME) result.push_back(win32::narrow(info->rgAltEntry[index].pwszDNSName));
    LocalFree(info);
    return result;
}

std::vector<std::uint8_t> Certificate::serial_number() const {
    const auto context = context_of(*this);
    if (!context) return {};
    const auto& serial = context->pCertInfo->SerialNumber;   // little-endian
    std::vector<std::uint8_t> result(serial.pbData, serial.pbData + serial.cbData);
    std::reverse(result.begin(), result.end());
    return result;
}

std::vector<std::uint8_t> Certificate::issuer() const {
    const auto context = context_of(*this);
    if (!context) return {};
    const auto& name = context->pCertInfo->Issuer;
    return std::vector<std::uint8_t>(name.pbData, name.pbData + name.cbData);
}

std::vector<std::uint8_t> Certificate::subject() const {
    const auto context = context_of(*this);
    if (!context) return {};
    const auto& name = context->pCertInfo->Subject;
    return std::vector<std::uint8_t>(name.pbData, name.pbData + name.cbData);
}

std::int64_t Certificate::not_before_ns() const {
    const auto context = context_of(*this);
    return context ? unix_ns(context->pCertInfo->NotBefore) : 0;
}

std::int64_t Certificate::not_after_ns() const {
    const auto context = context_of(*this);
    return context ? unix_ns(context->pCertInfo->NotAfter) : 0;
}

bool Certificate::self_issued() const {
    const auto context = context_of(*this);
    return context && CertCompareCertificateName(certificate_encoding, &context->pCertInfo->Issuer, &context->pCertInfo->Subject) != FALSE;
}

Result<std::vector<Certificate>> root_certificates(RootStore store) {
    std::vector<Certificate> result;
    const auto same = [](const Certificate& left, PCCERT_CONTEXT right) {
        const auto context = context_of(left);
        return context->cbCertEncoded == right->cbCertEncoded &&
               std::memcmp(context->pbCertEncoded, right->pbCertEncoded, right->cbCertEncoded) == 0;
    };
    const auto collect = [&](DWORD location, const wchar_t* name, const std::vector<Certificate>* exclude) {
        StoreHandle handle{CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, location | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, name)};
        if (!handle.store) return;
        for (PCCERT_CONTEXT context = nullptr; (context = CertEnumCertificatesInStore(handle.store, context)) != nullptr;) {
            if (exclude && std::any_of(exclude->begin(), exclude->end(), [&](const Certificate& other) { return same(other, context); })) continue;
            if (std::any_of(result.begin(), result.end(), [&](const Certificate& other) { return same(other, context); })) continue;
            result.push_back(share(context));
        }
    };
    switch (store) {
    case RootStore::system:   // the machine's root store
        collect(CERT_SYSTEM_STORE_LOCAL_MACHINE, L"ROOT", nullptr);
        break;
    case RootStore::admin:    // enterprise and group-policy roots
        collect(CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE, L"ROOT", nullptr);
        collect(CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY, L"ROOT", nullptr);
        break;
    case RootStore::user: {   // roots in the user's store that the machine store does not have
        const auto machine = root_certificates(RootStore::system);
        collect(CERT_SYSTEM_STORE_CURRENT_USER, L"ROOT", machine ? &*machine : nullptr);
        break;
    }
    }
    return result;
}

Result<ChainResult> evaluate_chain(const ChainRequest& request) {
    if (!request.leaf) return Error::invalid_argument;
    std::vector<ChainPolicy> policies = request.policies;
    if (policies.empty()) policies.push_back({});
    const ChainPolicy* ssl = nullptr;
    for (const auto& policy : policies)
        if (policy.ssl && !ssl) ssl = &policy;

    StoreHandle extra{memory_store()};
    if (!extra.store) return win32::last_error();
    for (const auto& certificate : request.intermediates)
        if (certificate && !add(extra.store, certificate)) return win32::last_error();

    EngineHandle engine;
    StoreHandle roots;
    if (!request.anchors.empty()) {
        roots.store = memory_store();
        if (!roots.store) return win32::last_error();
        for (const auto& anchor : request.anchors)
            if (anchor && !add(roots.store, anchor)) return win32::last_error();
        CERT_CHAIN_ENGINE_CONFIG config{};
        config.cbSize = sizeof(config);
        config.hExclusiveRoot = roots.store;
        config.dwExclusiveFlags = CERT_CHAIN_EXCLUSIVE_ENABLE_CA_FLAG;
        if (!CertCreateCertificateChainEngine(&config, &engine.engine)) return win32::last_error();
    }

    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof(para);
    LPSTR usage[1]{};
    if (ssl) {
        usage[0] = const_cast<LPSTR>(ssl->server ? szOID_PKIX_KP_SERVER_AUTH : szOID_PKIX_KP_CLIENT_AUTH);
        para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        para.RequestedUsage.Usage.cUsageIdentifier = 1;
        para.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
    }
    DWORD flags = CERT_CHAIN_CACHE_END_CERT;
    if (request.revocation) flags |= CERT_CHAIN_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    if (!request.network_fetch) flags |= CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL | CERT_CHAIN_DISABLE_AIA;
    FILETIME when{};
    const FILETIME* time = nullptr;
    if (request.time_ns) { when = file_time(*request.time_ns); time = &when; }

    ChainHandle chain;
    if (!CertGetCertificateChain(engine.engine, context_of(request.leaf), const_cast<FILETIME*>(time), extra.store, &para, flags, nullptr, &chain.chain))
        return win32::last_error();
    ChainResult result;
    if (chain.chain->cChain > 0) {
        const auto simple = chain.chain->rgpChain[0];
        for (DWORD index = 0; index < simple->cElement; ++index) result.chain.push_back(share(simple->rgpElement[index]->pCertContext));
    }

    // Policies: every policy must pass; the first failure is reported.
    DWORD error = 0;
    for (const auto& policy : policies) {
        CERT_CHAIN_POLICY_PARA policy_para{};
        policy_para.cbSize = sizeof(policy_para);
        CERT_CHAIN_POLICY_STATUS policy_status{};
        policy_status.cbSize = sizeof(policy_status);
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA extra_para{};
        std::wstring server;
        LPCSTR identifier = CERT_CHAIN_POLICY_BASE;
        if (policy.ssl) {
            identifier = CERT_CHAIN_POLICY_SSL;
            server = win32::widen(policy.hostname);
            extra_para.cbSize = sizeof(extra_para);
            extra_para.dwAuthType = policy.server ? AUTHTYPE_SERVER : AUTHTYPE_CLIENT;
            extra_para.pwszServerName = server.empty() ? nullptr : server.data();
            policy_para.pvExtraPolicyPara = &extra_para;
        }
        if (!CertVerifyCertificateChainPolicy(identifier, chain.chain, &policy_para, &policy_status)) return win32::last_error();
        if (policy_status.dwError) { error = policy_status.dwError; break; }
    }
    // The base policy does not report revocation results by itself.
    if (!error && request.revocation) {
        const auto trust = chain.chain->TrustStatus.dwErrorStatus;
        if (trust & CERT_TRUST_IS_REVOKED) error = static_cast<DWORD>(CRYPT_E_REVOKED);
        else if (trust & (CERT_TRUST_REVOCATION_STATUS_UNKNOWN | CERT_TRUST_IS_OFFLINE_REVOCATION))
            error = static_cast<DWORD>(CRYPT_E_REVOCATION_OFFLINE);
    }
    switch (static_cast<HRESULT>(error)) {
    case 0: result.failure = ChainFailure::none; break;
    case CERT_E_EXPIRED: case CERT_E_VALIDITYPERIODNESTING: result.failure = ChainFailure::expired; break;
    case CERT_E_CN_NO_MATCH: result.failure = ChainFailure::name_mismatch; break;
    case CERT_E_WRONG_USAGE: result.failure = ChainFailure::wrong_usage; break;
    case CRYPT_E_REVOKED: result.failure = ChainFailure::revoked; break;
    case CRYPT_E_REVOCATION_OFFLINE: case CRYPT_E_NO_REVOCATION_CHECK: case CERT_E_REVOCATION_FAILURE:
        result.failure = ChainFailure::revocation_unknown; break;
    case TRUST_E_CERT_SIGNATURE: case CRYPT_E_SECURITY_SETTINGS: result.failure = ChainFailure::bad_signature; break;
    default: result.failure = ChainFailure::untrusted; break;
    }
    return result;
}

// ---- Identities (CryptoAPI and CNG keys) ----

Result<Identity> Identity::self_signed(std::string_view common_name, const std::vector<std::string>& dns_names) {
    std::array<std::uint8_t, 16> unique{};
    if (BCryptGenRandom(nullptr, unique.data(), static_cast<ULONG>(unique.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return Error::io;
    std::string name = "Alchemy-TLS-";
    constexpr char digits[] = "0123456789abcdef";
    for (const auto byte : unique) { name += digits[byte >> 4]; name += digits[byte & 15]; }
    auto container = win32::widen(name);

    NCRYPT_PROV_HANDLE provider{};
    if (NCryptOpenStorageProvider(&provider, MS_KEY_STORAGE_PROVIDER, 0) != ERROR_SUCCESS) return Error::unsupported;
    NCRYPT_KEY_HANDLE key{};
    auto status_code = NCryptCreatePersistedKey(provider, &key, BCRYPT_RSA_ALGORITHM, container.c_str(), 0, NCRYPT_OVERWRITE_KEY_FLAG);
    if (status_code == ERROR_SUCCESS) {
        DWORD length = 2048;
        NCryptSetProperty(key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&length), sizeof(length), 0);
        DWORD export_policy = 0;
        NCryptSetProperty(key, NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&export_policy), sizeof(export_policy), 0);
        status_code = NCryptFinalizeKey(key, 0);
    }
    NCryptFreeObject(provider);
    if (status_code != ERROR_SUCCESS) { if (key) NCryptFreeObject(key); return Error::io; }

    const auto subject_text = win32::widen("CN=" + std::string(common_name));
    DWORD subject_size = 0;
    CertStrToNameW(X509_ASN_ENCODING, subject_text.c_str(), CERT_X500_NAME_STR, nullptr, nullptr, &subject_size, nullptr);
    std::vector<BYTE> subject(subject_size);
    if (!CertStrToNameW(X509_ASN_ENCODING, subject_text.c_str(), CERT_X500_NAME_STR, nullptr, subject.data(), &subject_size, nullptr)) {
        NCryptDeleteKey(key, 0);
        return Error::invalid_argument;
    }
    CERT_NAME_BLOB subject_blob{subject_size, subject.data()};

    // subjectAltName (DNS names) and extendedKeyUsage serverAuth.
    std::vector<std::wstring> names;
    for (const auto& entry : dns_names) names.push_back(win32::widen(entry));
    std::vector<CERT_ALT_NAME_ENTRY> entries(names.size());
    for (std::size_t index = 0; index < names.size(); ++index) {
        entries[index].dwAltNameChoice = CERT_ALT_NAME_DNS_NAME;
        entries[index].pwszDNSName = names[index].data();
    }
    CERT_ALT_NAME_INFO alt{static_cast<DWORD>(entries.size()), entries.data()};
    BYTE* alt_encoded{};
    DWORD alt_size{};
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ALTERNATE_NAME, &alt, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &alt_encoded, &alt_size);
    LPSTR server_auth[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
    CERT_ENHKEY_USAGE eku{1, server_auth};
    BYTE* eku_encoded{};
    DWORD eku_size{};
    CryptEncodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, CRYPT_ENCODE_ALLOC_FLAG, nullptr, &eku_encoded, &eku_size);
    std::array<CERT_EXTENSION, 2> extension_list{};
    extension_list[0] = {const_cast<LPSTR>(szOID_SUBJECT_ALT_NAME2), FALSE, {alt_size, alt_encoded}};
    extension_list[1] = {const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE), FALSE, {eku_size, eku_encoded}};
    CERT_EXTENSIONS extensions{alt_encoded && eku_encoded ? 2U : 0U, extension_list.data()};

    CRYPT_KEY_PROV_INFO provider_info{};
    provider_info.pwszContainerName = container.data();
    provider_info.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
    provider_info.dwProvType = 0;
    provider_info.dwKeySpec = 0;
    CRYPT_ALGORITHM_IDENTIFIER algorithm{const_cast<LPSTR>(szOID_RSA_SHA256RSA), {}};
    SYSTEMTIME start{}, end{};
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const auto ticks = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    const std::uint64_t day = 24ULL * 3600 * 10'000'000;
    FILETIME begin_time{static_cast<DWORD>(ticks - day), static_cast<DWORD>((ticks - day) >> 32)};
    FILETIME end_time{static_cast<DWORD>(ticks + 30 * day), static_cast<DWORD>((ticks + 30 * day) >> 32)};
    FileTimeToSystemTime(&begin_time, &start);
    FileTimeToSystemTime(&end_time, &end);
    const auto context = CertCreateSelfSignCertificate(key, &subject_blob, 0, &provider_info, &algorithm, &start, &end, &extensions);
    if (alt_encoded) LocalFree(alt_encoded);
    if (eku_encoded) LocalFree(eku_encoded);
    NCryptFreeObject(key);
    Identity identity;
    identity.key_ = name;
    if (!context) return Error::io;   // the identity deletes the key
    identity.handle_ = const_cast<CERT_CONTEXT*>(context);
    return identity;
}

Result<Identity::Imported> Identity::import_pkcs12(ConstBytes data, std::string_view password) {
    if (data.size() > std::numeric_limits<DWORD>::max()) return Error::invalid_argument;
    CRYPT_DATA_BLOB blob{static_cast<DWORD>(data.size()), reinterpret_cast<BYTE*>(const_cast<std::byte*>(data.data()))};
    if (!PFXIsPFXBlob(&blob)) return Error::invalid_argument;
    const auto secret = win32::widen(password);
    const auto store = PFXImportCertStore(&blob, secret.c_str(), CRYPT_USER_KEYSET | PKCS12_ALWAYS_CNG_KSP);
    if (!store) return GetLastError() == ERROR_INVALID_PASSWORD ? Error::access_denied : Error::invalid_argument;
    Imported result;
    for (PCCERT_CONTEXT context = nullptr; (context = CertEnumCertificatesInStore(store, context)) != nullptr;) {
        DWORD size{};
        if (CertGetCertificateContextProperty(context, CERT_KEY_PROV_INFO_PROP_ID, nullptr, &size) && size) {
            std::vector<BYTE> info(size);
            CertGetCertificateContextProperty(context, CERT_KEY_PROV_INFO_PROP_ID, info.data(), &size);
            const auto* provider = reinterpret_cast<const CRYPT_KEY_PROV_INFO*>(info.data());
            Identity identity;
            identity.handle_ = const_cast<CERT_CONTEXT*>(CertDuplicateCertificateContext(context));
            identity.key_ = provider->pwszContainerName ? win32::narrow(provider->pwszContainerName) : std::string{};
            result.identities.push_back(std::move(identity));
        } else {
            result.certificates.push_back(share(context));
        }
    }
    CertCloseStore(store, 0);
    return result;
}

bool Identity::is_pkcs12(ConstBytes data) {
    if (data.size() > std::numeric_limits<DWORD>::max()) return false;
    CRYPT_DATA_BLOB blob{static_cast<DWORD>(data.size()), reinterpret_cast<BYTE*>(const_cast<std::byte*>(data.data()))};
    return PFXIsPFXBlob(&blob) != FALSE;
}

Identity& Identity::operator=(Identity&& other) noexcept {
    if (this != &other) {
        if (handle_) CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(handle_));
        delete_key(key_);
        handle_ = std::exchange(other.handle_, nullptr);
        key_ = std::move(other.key_);
        other.key_.clear();
    }
    return *this;
}

Identity::~Identity() {
    if (handle_) CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(handle_));
    handle_ = nullptr;
    delete_key(key_);
    key_.clear();
}

Certificate Identity::certificate() const { return handle_ ? share(static_cast<PCCERT_CONTEXT>(handle_)) : Certificate{}; }

// ---- TLS (SChannel over SSPI stream contexts) ----

struct TlsSession::State {
    TlsOptions options;
    CredHandle credentials{};
    bool have_credentials{};
    CtxtHandle context{};
    bool have_context{};
    SecPkgContext_StreamSizes sizes{};
    std::wstring target;
    std::vector<std::uint8_t> alpn;           // SEC_APPLICATION_PROTOCOLS blob (empty: none)
    std::vector<std::uint8_t> incoming, outgoing, plaintext;
    std::size_t outgoing_offset{}, plaintext_offset{};
    bool complete{}, closed{}, peer_closed{}, notified{};
    std::string error;

    ~State() {
        if (have_context) DeleteSecurityContext(&context);
        if (have_credentials) FreeCredentialsHandle(&credentials);
    }
    [[nodiscard]] std::size_t buffered() const noexcept { return plaintext.size() - plaintext_offset; }
    Status fail_with(std::string message) {
        if (error.empty()) error = std::move(message);
        return Status::error;
    }
    Status receive(const TlsRead& read);
    Status flush(const TlsWrite& write);
    Status step(const TlsRead& read, const TlsWrite& write);
};

namespace {

std::string status_text(SECURITY_STATUS status) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lx", static_cast<unsigned long>(status));
    return buffer;
}

DWORD protocol_bits(TlsVersion version) {
    switch (version) {
    case TlsVersion::tls10: return SP_PROT_TLS1_0;
    case TlsVersion::tls11: return SP_PROT_TLS1_1;
    case TlsVersion::tls12: return SP_PROT_TLS1_2;
    case TlsVersion::tls13: return SP_PROT_TLS1_3;
    default: return 0;
    }
}

constexpr TlsVersion tls_versions[] = {TlsVersion::tls10, TlsVersion::tls11, TlsVersion::tls12, TlsVersion::tls13};

} // namespace

Result<TlsSession> TlsSession::create(const TlsOptions& options, std::string* error_text) {
    const auto refuse = [&](Error error, std::string text) -> Result<TlsSession> {
        if (error_text) *error_text = std::move(text);
        return error;
    };
    if (options.server && !(options.identity && *options.identity)) return refuse(Error::invalid_argument, "a TLS server needs a certificate");
    if (static_cast<int>(options.min_version) > static_cast<int>(options.max_version))
        return refuse(Error::invalid_argument, "TLS minimum version above maximum");
    auto state = std::make_unique<State>();
    state->options = options;
    state->target = win32::widen(options.peer_name);

    TLS_PARAMETERS parameters{};
    parameters.grbitDisabledProtocols = SP_PROT_SSL2 | SP_PROT_SSL3;
    for (const auto version : tls_versions) {
        const auto value = static_cast<int>(version);
        if (value < static_cast<int>(options.min_version) || value > static_cast<int>(options.max_version))
            parameters.grbitDisabledProtocols |= protocol_bits(version);
    }
    SCH_CREDENTIALS credentials{};
    credentials.dwVersion = SCH_CREDENTIALS_VERSION;
    credentials.dwFlags = SCH_USE_STRONG_CRYPTO;
    if (!options.server) credentials.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
    PCCERT_CONTEXT certificates[1]{options.identity ? static_cast<PCCERT_CONTEXT>(options.identity->native()) : nullptr};
    if (certificates[0]) {
        credentials.cCreds = 1;
        credentials.paCred = certificates;
    }
    credentials.cTlsParameters = 1;
    credentials.pTlsParameters = &parameters;
    const auto status = AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W),
        options.server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr, &credentials, nullptr, nullptr, &state->credentials, nullptr);
    if (status != SEC_E_OK) return refuse(Error::io, "AcquireCredentialsHandle(SChannel) failed: " + status_text(status));
    state->have_credentials = true;

    if (!options.alpn.empty()) {
        std::vector<std::uint8_t> list;
        for (const auto& protocol : options.alpn) {
            if (protocol.empty() || protocol.size() > 255) return refuse(Error::invalid_argument, "invalid ALPN protocol name");
            list.push_back(static_cast<std::uint8_t>(protocol.size()));
            list.insert(list.end(), protocol.begin(), protocol.end());
        }
        if (list.size() > 0xffff) return refuse(Error::invalid_argument, "ALPN list too long");
        const unsigned long lists_size = static_cast<unsigned long>(sizeof(SEC_APPLICATION_PROTOCOL_NEGOTIATION_EXT) + sizeof(unsigned short) + list.size());
        auto& blob = state->alpn;
        blob.resize(sizeof(unsigned long) + lists_size);
        std::memcpy(blob.data(), &lists_size, sizeof(lists_size));
        const SEC_APPLICATION_PROTOCOL_NEGOTIATION_EXT extension = SecApplicationProtocolNegotiationExt_ALPN;
        std::memcpy(blob.data() + sizeof(unsigned long), &extension, sizeof(extension));
        const auto size = static_cast<unsigned short>(list.size());
        std::memcpy(blob.data() + sizeof(unsigned long) + sizeof(extension), &size, sizeof(size));
        std::memcpy(blob.data() + sizeof(unsigned long) + sizeof(extension) + sizeof(size), list.data(), list.size());
    }
    return TlsSession(std::move(state));
}

TlsSession::TlsSession(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
TlsSession::TlsSession() = default;
TlsSession::TlsSession(TlsSession&&) noexcept = default;
TlsSession& TlsSession::operator=(TlsSession&&) noexcept = default;
TlsSession::~TlsSession() = default;

TlsSession::Status TlsSession::State::receive(const TlsRead& read) {
    std::uint8_t buffer[16384];
    const auto result = read(buffer, sizeof(buffer));
    switch (result.kind) {
    case TlsIo::Kind::ok:
        if (result.size == 0) { peer_closed = true; return Status::closed; }
        incoming.insert(incoming.end(), buffer, buffer + std::min(result.size, sizeof(buffer)));
        return Status::ok;
    case TlsIo::Kind::would_block: return Status::would_block;
    case TlsIo::Kind::closed: peer_closed = true; return Status::closed;
    default: return fail_with("TLS transport read failed");
    }
}

TlsSession::Status TlsSession::State::flush(const TlsWrite& write) {
    while (outgoing_offset < outgoing.size()) {
        const auto result = write(outgoing.data() + outgoing_offset, outgoing.size() - outgoing_offset);
        if (result.kind == TlsIo::Kind::would_block || (result.kind == TlsIo::Kind::ok && result.size == 0)) return Status::would_block;
        if (result.kind != TlsIo::Kind::ok) return fail_with("TLS transport write failed");
        outgoing_offset += std::min(result.size, outgoing.size() - outgoing_offset);
    }
    outgoing.clear();
    outgoing_offset = 0;
    return Status::ok;
}

TlsSession::Status TlsSession::State::step(const TlsRead& read, const TlsWrite& write) {
    for (;;) {
        if (const auto flushed = flush(write); flushed != Status::ok) return flushed;
        if (complete) return Status::ok;
        if (!error.empty()) return Status::error;
        const bool first_client = !options.server && !have_context;
        if (!first_client && incoming.empty()) {
            const auto received = receive(read);
            if (received == Status::closed) return fail_with("TLS peer closed the connection during the handshake");
            if (received != Status::ok) return received;
            continue;
        }
        SecBuffer input[3]{};
        input[0] = {static_cast<unsigned long>(incoming.size()), SECBUFFER_TOKEN, incoming.data()};
        input[1] = {0, SECBUFFER_EMPTY, nullptr};
        unsigned long input_count = 2;
        if (!alpn.empty() && !have_context) {
            input[2] = {static_cast<unsigned long>(alpn.size()), SECBUFFER_APPLICATION_PROTOCOLS, alpn.data()};
            input_count = 3;
        }
        SecBufferDesc input_desc{SECBUFFER_VERSION, input_count, input};
        SecBuffer alpn_only[1]{{static_cast<unsigned long>(alpn.size()), SECBUFFER_APPLICATION_PROTOCOLS, alpn.data()}};
        SecBufferDesc alpn_desc{SECBUFFER_VERSION, 1, alpn_only};
        SecBuffer output[2]{};
        output[0] = {0, SECBUFFER_TOKEN, nullptr};
        output[1] = {0, SECBUFFER_ALERT, nullptr};
        SecBufferDesc output_desc{SECBUFFER_VERSION, 2, output};
        unsigned long attributes{};
        SECURITY_STATUS status;
        if (options.server) {
            unsigned long flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT | ASC_REQ_CONFIDENTIALITY |
                ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM | ASC_REQ_EXTENDED_ERROR;
            if (options.request_client_certificate) flags |= ASC_REQ_MUTUAL_AUTH;
            status = AcceptSecurityContext(&credentials, have_context ? &context : nullptr, &input_desc, flags, 0, &context, &output_desc,
                                           &attributes, nullptr);
        } else {
            const unsigned long flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM | ISC_REQ_EXTENDED_ERROR | ISC_REQ_USE_SUPPLIED_CREDS |
                ISC_REQ_MANUAL_CRED_VALIDATION;
            status = InitializeSecurityContextW(&credentials, have_context ? &context : nullptr, target.empty() ? nullptr : target.data(),
                flags, 0, 0, first_client ? (alpn.empty() ? nullptr : &alpn_desc) : &input_desc, 0, &context, &output_desc, &attributes, nullptr);
        }
        if (status == SEC_E_OK || status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS) have_context = true;
        for (auto& buffer : output) {
            if (buffer.pvBuffer && buffer.cbBuffer && buffer.BufferType == SECBUFFER_TOKEN) {
                const auto* bytes = static_cast<const std::uint8_t*>(buffer.pvBuffer);
                outgoing.insert(outgoing.end(), bytes, bytes + buffer.cbBuffer);
            }
            if (buffer.pvBuffer) FreeContextBuffer(buffer.pvBuffer);
        }
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            const auto received = receive(read);
            if (received == Status::closed) return fail_with("TLS peer closed the connection during the handshake");
            if (received != Status::ok) return received;
            continue;
        }
        if (!first_client) {
            if (input[1].BufferType == SECBUFFER_EXTRA && input[1].cbBuffer && input[1].cbBuffer <= incoming.size())
                incoming.erase(incoming.begin(), incoming.end() - static_cast<std::ptrdiff_t>(input[1].cbBuffer));
            else
                incoming.clear();
        }
        if (status == SEC_E_OK) {
            const auto queried = QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes);
            if (queried != SEC_E_OK) return fail_with("QueryContextAttributes(STREAM_SIZES) failed: " + status_text(queried));
            complete = true;
            continue;   // flush the final token
        }
        if (status == SEC_I_CONTINUE_NEEDED || status == SEC_I_INCOMPLETE_CREDENTIALS) continue;
        static_cast<void>(flush(write));   // best effort: the alert
        return fail_with("TLS handshake failed: " + status_text(status));
    }
}

TlsSession::Status TlsSession::handshake(const TlsRead& read, const TlsWrite& write) {
    if (state_->closed) return Status::closed;
    return state_->step(read, write);
}

TlsSession::Status TlsSession::read(const TlsRead& read, const TlsWrite& write, std::uint8_t* buffer, std::size_t size, std::size_t& got) {
    auto& s = *state_;
    got = 0;
    if (!s.complete) {
        const auto status = s.step(read, write);
        if (status != Status::ok) return status;
    }
    for (;;) {
        if (s.buffered()) {
            const auto count = std::min(size, s.buffered());
            std::memcpy(buffer, s.plaintext.data() + s.plaintext_offset, count);
            s.plaintext_offset += count;
            if (s.plaintext_offset == s.plaintext.size()) { s.plaintext.clear(); s.plaintext_offset = 0; }
            got = count;
            return Status::ok;
        }
        if (s.closed) return Status::closed;
        if (s.incoming.empty()) {
            const auto received = s.receive(read);
            if (received == Status::closed) return Status::closed;
            if (received != Status::ok) return received;
        }
        SecBuffer buffers[4]{};
        buffers[0] = {static_cast<unsigned long>(s.incoming.size()), SECBUFFER_DATA, s.incoming.data()};
        for (int index = 1; index < 4; ++index) buffers[index] = {0, SECBUFFER_EMPTY, nullptr};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
        const auto status = DecryptMessage(&s.context, &desc, 0, nullptr);
        if (status == SEC_E_INCOMPLETE_MESSAGE) {
            const auto received = s.receive(read);
            if (received == Status::closed) return s.fail_with("TLS connection closed inside a record");
            if (received != Status::ok) return received;
            continue;
        }
        if (status != SEC_E_OK && status != SEC_I_RENEGOTIATE && status != SEC_I_CONTEXT_EXPIRED)
            return s.fail_with("TLS decryption failed: " + status_text(status));
        std::vector<std::uint8_t> data, extra;
        for (const auto& item : buffers) {
            const auto* bytes = static_cast<const std::uint8_t*>(item.pvBuffer);
            if (item.BufferType == SECBUFFER_DATA && bytes) data.assign(bytes, bytes + item.cbBuffer);
            if (item.BufferType == SECBUFFER_EXTRA && bytes) extra.assign(bytes, bytes + item.cbBuffer);
        }
        s.plaintext.insert(s.plaintext.end(), data.begin(), data.end());
        s.incoming = std::move(extra);
        if (status == SEC_I_CONTEXT_EXPIRED) { s.closed = true; s.notified = true; continue; }
        if (status == SEC_I_RENEGOTIATE) {
            // TLS 1.3 post-handshake messages (tickets, key updates): the
            // context consumes them through the handshake call.
            s.complete = false;
            const auto stepped = s.step(read, write);
            if (stepped != Status::ok) return stepped;
        }
    }
}

TlsSession::Status TlsSession::write(const TlsWrite& write, const std::uint8_t* data, std::size_t size, std::size_t& accepted) {
    auto& s = *state_;
    accepted = 0;
    if (!s.complete) return s.fail_with("TLS write before the handshake completed");
    if (s.closed) return Status::closed;
    const auto& sizes = s.sizes;
    std::size_t offset = 0;
    while (offset < size) {
        const auto chunk = std::min<std::size_t>(size - offset, sizes.cbMaximumMessage);
        std::vector<std::uint8_t> message(sizes.cbHeader + chunk + sizes.cbTrailer);
        std::memcpy(message.data() + sizes.cbHeader, data + offset, chunk);
        SecBuffer buffers[4]{};
        buffers[0] = {sizes.cbHeader, SECBUFFER_STREAM_HEADER, message.data()};
        buffers[1] = {static_cast<unsigned long>(chunk), SECBUFFER_DATA, message.data() + sizes.cbHeader};
        buffers[2] = {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, message.data() + sizes.cbHeader + chunk};
        buffers[3] = {0, SECBUFFER_EMPTY, nullptr};
        SecBufferDesc desc{SECBUFFER_VERSION, 4, buffers};
        const auto status = EncryptMessage(&s.context, 0, &desc, 0);
        if (status != SEC_E_OK) return s.fail_with("TLS encryption failed: " + status_text(status));
        const auto total = buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer;
        s.outgoing.insert(s.outgoing.end(), message.begin(), message.begin() + total);
        offset += chunk;
    }
    accepted = size;
    return s.flush(write);
}

TlsSession::Status TlsSession::flush(const TlsWrite& write) { return state_->flush(write); }

TlsSession::Status TlsSession::shutdown(const TlsWrite& write) {
    auto& s = *state_;
    if (!s.have_context || s.closed) { s.closed = true; return Status::ok; }
    s.closed = true;
    DWORD type = SCHANNEL_SHUTDOWN;
    SecBuffer token{sizeof(type), SECBUFFER_TOKEN, &type};
    SecBufferDesc token_desc{SECBUFFER_VERSION, 1, &token};
    if (ApplyControlToken(&s.context, &token_desc) != SEC_E_OK) return Status::ok;
    SecBuffer output{0, SECBUFFER_TOKEN, nullptr};
    SecBufferDesc output_desc{SECBUFFER_VERSION, 1, &output};
    unsigned long attributes{};
    if (s.options.server)
        AcceptSecurityContext(&s.credentials, &s.context, nullptr, ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM, 0, nullptr, &output_desc,
                              &attributes, nullptr);
    else
        InitializeSecurityContextW(&s.credentials, &s.context, s.target.empty() ? nullptr : s.target.data(), ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM,
                                   0, 0, nullptr, 0, nullptr, &output_desc, &attributes, nullptr);
    if (output.pvBuffer) {
        const auto* bytes = static_cast<const std::uint8_t*>(output.pvBuffer);
        s.outgoing.insert(s.outgoing.end(), bytes, bytes + output.cbBuffer);
        FreeContextBuffer(output.pvBuffer);
    }
    return s.flush(write);
}

bool TlsSession::complete() const noexcept { return state_ && state_->complete; }
std::size_t TlsSession::buffered() const noexcept { return state_ ? state_->buffered() : 0; }
const std::string& TlsSession::error() const noexcept {
    static const std::string none;
    return state_ ? state_->error : none;
}
bool TlsSession::close_notified() const noexcept { return state_ && state_->notified; }

std::vector<Certificate> TlsSession::peer_certificates() const {
    std::vector<Certificate> result;
    if (!state_ || !state_->have_context) return result;
    PCCERT_CONTEXT remote{};
    if (QueryContextAttributesW(&state_->context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote) != SEC_E_OK || !remote) return result;
    result.push_back(share(remote));
    if (remote->hCertStore) {
        for (PCCERT_CONTEXT context = nullptr; (context = CertEnumCertificatesInStore(remote->hCertStore, context)) != nullptr;) {
            if (CertCompareCertificate(X509_ASN_ENCODING, context->pCertInfo, remote->pCertInfo)) continue;
            result.push_back(share(context));
        }
    }
    CertFreeCertificateContext(remote);
    return result;
}

std::string TlsSession::negotiated_alpn() const {
    if (!complete()) return {};
    SecPkgContext_ApplicationProtocol protocol{};
    if (QueryContextAttributesW(&state_->context, SECPKG_ATTR_APPLICATION_PROTOCOL, &protocol) != SEC_E_OK) return {};
    if (protocol.ProtoNegoStatus != SecApplicationProtocolNegotiationStatus_Success) return {};
    return std::string(reinterpret_cast<const char*>(protocol.ProtocolId), protocol.ProtocolIdSize);
}

TlsVersion TlsSession::negotiated_version() const {
    if (!complete()) return TlsVersion::unknown;
    SecPkgContext_ConnectionInfo info{};
    if (QueryContextAttributesW(&state_->context, SECPKG_ATTR_CONNECTION_INFO, &info) != SEC_E_OK) return TlsVersion::unknown;
    if (info.dwProtocol & SP_PROT_TLS1_3) return TlsVersion::tls13;
    if (info.dwProtocol & SP_PROT_TLS1_2) return TlsVersion::tls12;
    if (info.dwProtocol & SP_PROT_TLS1_1) return TlsVersion::tls11;
    if (info.dwProtocol & SP_PROT_TLS1_0) return TlsVersion::tls10;
    return TlsVersion::unknown;
}

} // namespace alchemy::host::crypto
