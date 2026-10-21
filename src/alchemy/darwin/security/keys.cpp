#include "alchemy/darwin/security/keys.hpp"

#include "alchemy/darwin/security/security_internal.hpp"
#include "alchemy/host/crypto.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>

// SecKey over host::crypto and SecItemImport (PEM / DER keys and
// certificates). Algorithm names and external key layouts follow Apple's
// public SecKey documentation; no Apple code is used.
namespace alchemy::gui::security {
namespace {

constexpr std::string_view lib = security_library;
constexpr std::string_view rsa_oid = "1.2.840.113549.1.1.1", ec_oid = "1.2.840.10045.2.1";

struct Curve { std::size_t coordinate; unsigned bits; const char* oid; };
const Curve curves[] = {{32, 256, "1.2.840.10045.3.1.7"}, {48, 384, "1.3.132.0.34"}, {66, 521, "1.3.132.0.35"}};

const Curve* curve_by_coordinate(std::size_t coordinate) {
    for (const auto& curve : curves) if (curve.coordinate == coordinate) return &curve;
    return nullptr;
}
const Curve* curve_by_oid(std::string_view oid) {
    for (const auto& curve : curves) if (oid == curve.oid) return &curve;
    return nullptr;
}
const Curve* curve_by_bits(unsigned bits) {
    for (const auto& curve : curves) if (curve.bits == bits) return &curve;
    return nullptr;
}

Bytes padded(const Bytes& value, std::size_t size) {
    if (value.size() >= size) return Bytes(value.end() - static_cast<std::ptrdiff_t>(size), value.end());
    Bytes result(size - value.size(), 0);
    result.insert(result.end(), value.begin(), value.end());
    return result;
}

void append(Bytes& out, const Bytes& value) { out.insert(out.end(), value.begin(), value.end()); }

// The key's components for the host.
host::crypto::RsaKey rsa_of(const KeyMaterial& key) { return {key.bits, key.n, key.e, key.d, key.p, key.q, key.dp, key.dq, key.qinv}; }
host::crypto::EcKey ec_of(const KeyMaterial& key) { return {key.coordinate, key.x, key.y, key.k}; }

std::shared_ptr<KeyMaterial> rsa_public(std::span<const std::uint8_t> pkcs1) {
    const auto outer = der::read(pkcs1);
    if (!outer || outer->tag != 0x30 || outer->whole.size() != pkcs1.size()) return nullptr;
    const auto parts = der::children(*outer);
    if (!parts || parts->size() != 2 || (*parts)[0].tag != 0x02 || (*parts)[1].tag != 0x02) return nullptr;
    auto key = std::make_shared<KeyMaterial>();
    key->type = KeyMaterial::Type::rsa;
    key->n = der::unsigned_integer((*parts)[0].value);
    key->e = der::unsigned_integer((*parts)[1].value);
    if (key->n.empty() || key->n[0] == 0) return nullptr;
    key->bits = static_cast<unsigned>(key->n.size() * 8);
    for (auto top = key->n[0]; !(top & 0x80); top = static_cast<std::uint8_t>(top << 1)) --key->bits;
    return key;
}

std::shared_ptr<KeyMaterial> rsa_private(std::span<const std::uint8_t> pkcs1) {
    const auto outer = der::read(pkcs1);
    if (!outer || outer->tag != 0x30) return nullptr;
    const auto parts = der::children(*outer);
    if (!parts || parts->size() < 9) return nullptr;
    for (std::size_t index = 0; index < 9; ++index) if ((*parts)[index].tag != 0x02) return nullptr;
    auto key = std::make_shared<KeyMaterial>();
    key->type = KeyMaterial::Type::rsa;
    key->is_private = true;
    Bytes* fields[] = {&key->n, &key->e, &key->d, &key->p, &key->q, &key->dp, &key->dq, &key->qinv};
    for (std::size_t index = 0; index < 8; ++index) *fields[index] = der::unsigned_integer((*parts)[index + 1].value);
    if (key->n.empty() || key->n[0] == 0) return nullptr;
    key->bits = static_cast<unsigned>(key->n.size() * 8);
    for (auto top = key->n[0]; !(top & 0x80); top = static_cast<std::uint8_t>(top << 1)) --key->bits;
    return key;
}

std::shared_ptr<KeyMaterial> ec_point(std::span<const std::uint8_t> point, std::span<const std::uint8_t> scalar) {
    if (point.size() < 3 || point[0] != 0x04 || (point.size() - 1) % 2) return nullptr;
    const auto coordinate = (point.size() - 1) / 2;
    const auto curve = curve_by_coordinate(coordinate);
    if (!curve) return nullptr;
    auto key = std::make_shared<KeyMaterial>();
    key->type = KeyMaterial::Type::ec;
    key->coordinate = coordinate;
    key->bits = curve->bits;
    key->x.assign(point.begin() + 1, point.begin() + 1 + static_cast<std::ptrdiff_t>(coordinate));
    key->y.assign(point.begin() + 1 + static_cast<std::ptrdiff_t>(coordinate), point.end());
    if (!scalar.empty()) {
        key->is_private = true;
        key->k = padded(Bytes(scalar.begin(), scalar.end()), coordinate);
    }
    return key;
}

KeyPtr validated(std::shared_ptr<KeyMaterial> key) {
    if (!key) return nullptr;
    const auto status = key->type == KeyMaterial::Type::rsa ? host::crypto::check(rsa_of(*key), key->is_private)
                                                            : host::crypto::check(ec_of(*key), key->is_private);
    return status ? key : nullptr;
}

std::optional<host::crypto::Digest> hash_algorithm(std::string_view name) {
    using host::crypto::Digest;
    std::optional<Digest> digest;
    if (name == "SHA1") digest = Digest::sha1;
    else if (name == "SHA224") digest = Digest::sha224;
    else if (name == "SHA256") digest = Digest::sha256;
    else if (name == "SHA384") digest = Digest::sha384;
    else if (name == "SHA512") digest = Digest::sha512;
    else if (name == "MD5") digest = Digest::md5;
    if (digest && !host::crypto::supports(*digest)) return std::nullopt;   // the host lacks it (SHA-224 on Windows)
    return digest;
}

// "algid:<op>:<KEY>:<mode>[:<hash>]" parts.
std::vector<std::string_view> split(std::string_view text) {
    std::vector<std::string_view> parts;
    std::size_t at = 0;
    while (at <= text.size()) {
        const auto next = text.find(':', at);
        parts.push_back(text.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at));
        if (next == std::string_view::npos) break;
        at = next + 1;
    }
    return parts;
}

KeyResult failure(std::int32_t code, std::string message) { return {std::nullopt, code, std::move(message)}; }

Bytes ecdsa_der(std::span<const std::uint8_t> raw) {
    const auto half = raw.size() / 2;
    return der::sequence({der::integer(raw.subspan(0, half)), der::integer(raw.subspan(half))});
}

std::optional<Bytes> ecdsa_raw(std::span<const std::uint8_t> signature, std::size_t coordinate) {
    const auto outer = der::read(signature);
    if (!outer || outer->tag != 0x30) return std::nullopt;
    const auto parts = der::children(*outer);
    if (!parts || parts->size() != 2 || (*parts)[0].tag != 0x02 || (*parts)[1].tag != 0x02) return std::nullopt;
    const auto r = der::unsigned_integer((*parts)[0].value), s = der::unsigned_integer((*parts)[1].value);
    if (r.size() > coordinate || s.size() > coordinate) return std::nullopt;
    auto result = padded(r, coordinate);
    append(result, padded(s, coordinate));
    return result;
}

} // namespace

// ---- KeyMaterial ---------------------------------------------------------------------------------

Bytes KeyMaterial::public_external() const {
    if (type == Type::rsa) return der::sequence({der::integer(n), der::integer(e)});
    Bytes result{0x04};
    append(result, padded(x, coordinate));
    append(result, padded(y, coordinate));
    return result;
}

Bytes KeyMaterial::private_external() const {
    if (!is_private) return {};
    if (type == Type::rsa)
        return der::sequence({der::integer(Bytes{0}), der::integer(n), der::integer(e), der::integer(d), der::integer(p),
                              der::integer(q), der::integer(dp), der::integer(dq), der::integer(qinv)});
    auto result = public_external();
    append(result, padded(k, coordinate));
    return result;
}

Bytes KeyMaterial::spki() const {
    const auto encode_oid = [](std::string_view dotted) {
        std::vector<std::uint64_t> parts;
        std::size_t at = 0;
        while (at < dotted.size()) {
            auto next = dotted.find('.', at);
            if (next == std::string_view::npos) next = dotted.size();
            parts.push_back(std::stoull(std::string(dotted.substr(at, next - at))));
            at = next + 1;
        }
        Bytes body{static_cast<std::uint8_t>(parts[0] * 40 + parts[1])};
        for (std::size_t index = 2; index < parts.size(); ++index) {
            Bytes chunk;
            auto value = parts[index];
            chunk.insert(chunk.begin(), static_cast<std::uint8_t>(value & 0x7f));
            while (value >>= 7) chunk.insert(chunk.begin(), static_cast<std::uint8_t>(0x80 | (value & 0x7f)));
            append(body, chunk);
        }
        return der::encode(0x06, body);
    };
    Bytes algorithm;
    if (type == Type::rsa) algorithm = der::sequence({encode_oid(rsa_oid), der::encode(0x05, Bytes{})});
    else algorithm = der::sequence({encode_oid(ec_oid), encode_oid(curve_by_coordinate(coordinate)->oid)});
    Bytes bits{0};
    append(bits, public_external());
    return der::sequence({algorithm, der::encode(0x03, bits)});
}

std::size_t KeyMaterial::block_size() const noexcept { return type == Type::rsa ? n.size() : coordinate; }

// ---- parsing -----------------------------------------------------------------------------------------

KeyPtr key_from_spki(std::span<const std::uint8_t> bytes) {
    const auto outer = der::read(bytes);
    if (!outer || outer->tag != 0x30) return nullptr;
    const auto parts = der::children(*outer);
    if (!parts || parts->size() != 2 || (*parts)[0].tag != 0x30 || (*parts)[1].tag != 0x03) return nullptr;
    const auto algorithm = der::children((*parts)[0]);
    if (!algorithm || algorithm->empty() || (*algorithm)[0].tag != 0x06) return nullptr;
    const auto oid = der::oid((*algorithm)[0].value);
    const auto bits = (*parts)[1].value;
    if (bits.empty() || bits[0] != 0) return nullptr;
    const auto key_bytes = bits.subspan(1);
    if (oid == rsa_oid) return validated(rsa_public(key_bytes));
    if (oid == ec_oid) {
        if (algorithm->size() < 2 || (*algorithm)[1].tag != 0x06) return nullptr;
        const auto curve = curve_by_oid(der::oid((*algorithm)[1].value));
        if (!curve || key_bytes.size() != 1 + 2 * curve->coordinate) return nullptr;
        return validated(ec_point(key_bytes, {}));
    }
    return nullptr;
}

KeyPtr key_from_external(KeyMaterial::Type type, bool is_private, std::span<const std::uint8_t> bytes) {
    if (type == KeyMaterial::Type::rsa) return validated(is_private ? rsa_private(bytes) : rsa_public(bytes));
    if (bytes.empty() || bytes[0] != 0x04) return nullptr;
    // Public: 04||X||Y (1 + 2c bytes); private: 04||X||Y||K (1 + 3c bytes).
    const auto body = bytes.size() - 1;
    if (is_private) {
        if (body % 3) return nullptr;
        const auto coordinate = body / 3;
        return validated(ec_point(bytes.subspan(0, 1 + 2 * coordinate), bytes.subspan(1 + 2 * coordinate)));
    }
    return validated(ec_point(bytes, {}));
}

KeyPtr key_from_sec1(std::span<const std::uint8_t> bytes) {
    const auto outer = der::read(bytes);
    if (!outer || outer->tag != 0x30) return nullptr;
    const auto parts = der::children(*outer);
    if (!parts || parts->size() < 2 || (*parts)[0].tag != 0x02 || (*parts)[1].tag != 0x04) return nullptr;
    std::span<const std::uint8_t> point;
    for (std::size_t index = 2; index < parts->size(); ++index) {
        if ((*parts)[index].tag != 0xa1) continue;
        const auto inner = der::read((*parts)[index].value);
        if (!inner || inner->tag != 0x03 || inner->value.empty() || inner->value[0] != 0) return nullptr;
        point = inner->value.subspan(1);
    }
    if (point.empty()) return nullptr;   // the public point is required to import into CNG
    return validated(ec_point(point, (*parts)[1].value));
}

KeyPtr key_from_pkcs8(std::span<const std::uint8_t> bytes) {
    const auto outer = der::read(bytes);
    if (!outer || outer->tag != 0x30) return nullptr;
    const auto parts = der::children(*outer);
    if (!parts || parts->size() < 3 || (*parts)[0].tag != 0x02 || (*parts)[1].tag != 0x30 || (*parts)[2].tag != 0x04) return nullptr;
    const auto algorithm = der::children((*parts)[1]);
    if (!algorithm || algorithm->empty() || (*algorithm)[0].tag != 0x06) return nullptr;
    const auto oid = der::oid((*algorithm)[0].value);
    if (oid == rsa_oid) return validated(rsa_private((*parts)[2].value));
    if (oid == ec_oid) return key_from_sec1((*parts)[2].value);
    return nullptr;
}

KeyPtr public_key_of(const KeyMaterial& key) {
    auto result = std::make_shared<KeyMaterial>();
    result->type = key.type;
    result->bits = key.bits;
    result->n = key.n; result->e = key.e;
    result->coordinate = key.coordinate;
    result->x = key.x; result->y = key.y;
    return result;
}

KeyPtr generate_key(KeyMaterial::Type type, unsigned bits) {
    if (type == KeyMaterial::Type::rsa) {
        if (bits < 1024 || bits > 16384 || bits % 8) return nullptr;
        auto generated = host::crypto::generate_rsa(bits);
        if (!generated) return nullptr;
        auto key = std::make_shared<KeyMaterial>();
        key->type = type;
        key->is_private = true;
        key->bits = generated->bits;
        key->e = std::move(generated->e);
        key->n = std::move(generated->n);
        key->p = std::move(generated->p); key->q = std::move(generated->q);
        key->dp = std::move(generated->dp); key->dq = std::move(generated->dq);
        key->qinv = std::move(generated->qinv); key->d = std::move(generated->d);
        return key;
    }
    const auto curve = curve_by_bits(bits);
    if (!curve) return nullptr;
    auto generated = host::crypto::generate_ec(curve->coordinate);
    if (!generated) return nullptr;
    Bytes point{0x04};
    append(point, generated->x);
    append(point, generated->y);
    auto key = ec_point(point, generated->k);
    host::crypto::secure_zero(std::as_writable_bytes(std::span(generated->k)));
    return key;
}

// ---- operations --------------------------------------------------------------------------------------------

std::optional<Bytes> digest(std::string_view name, std::span<const std::uint8_t> data) {
    const auto algorithm = hash_algorithm(name);
    if (!algorithm) return std::nullopt;
    auto value = host::crypto::hash(*algorithm, std::as_bytes(data));
    if (!value) return std::nullopt;
    return std::move(*value);
}

bool key_supports(const KeyMaterial& key, KeyOperation operation, std::string_view algorithm) {
    const auto parts = split(algorithm);
    if (parts.size() < 4 || parts[0] != "algid") return false;
    const bool rsa = key.type == KeyMaterial::Type::rsa;
    const auto hash_ok = [&](std::size_t index) { return parts.size() <= index || hash_algorithm(parts[index]).has_value(); };
    if (parts[1] == "sign") {
        if (operation == KeyOperation::sign && !key.is_private) return false;
        if (operation != KeyOperation::sign && operation != KeyOperation::verify) return false;
        if (parts[2] == "RSA") return rsa && (parts[3] == "raw" || parts[3] == "digest-PKCS1v15" || parts[3] == "message-PKCS1v15" ||
                                              ((parts[3] == "digest-PSS" || parts[3] == "message-PSS") && parts.size() == 5)) && hash_ok(4);
        if (parts[2] == "ECDSA") return !rsa && (parts[3] == "RFC4754" || parts[3] == "digest-X962" ||
                                                 (parts[3] == "message-X962" && parts.size() == 5)) && hash_ok(4);
        return false;
    }
    if (parts[1] == "encrypt") {
        if (operation == KeyOperation::decrypt && !key.is_private) return false;
        if (operation != KeyOperation::encrypt && operation != KeyOperation::decrypt) return false;
        return rsa && parts[2] == "RSA" && (parts[3] == "raw" || parts[3] == "PKCS1" || (parts[3] == "OAEP" && parts.size() == 5 && hash_ok(4)));
    }
    if (parts[1] == "keyexchange")
        return operation == KeyOperation::key_exchange && !rsa && key.is_private && parts[2] == "ECDH" &&
            (parts[3] == "standard" || parts[3] == "cofactor") && parts.size() == 4;
    return false;
}

namespace {

// The failure for a host error: the key itself refused, or `otherwise`.
KeyResult refused(host::Error error, std::int32_t code, std::string message) {
    if (error == host::Error::bad_handle) return failure(status::invalid_key_ref, "the key cannot be imported");
    return failure(code, std::move(message));
}

} // namespace

KeyResult key_sign(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data) {
    if (!key_supports(key, KeyOperation::sign, algorithm)) return failure(status::unsupported_algorithm, "algorithm not supported by the key");
    const auto parts = split(algorithm);
    Bytes input(data.begin(), data.end());
    std::optional<host::crypto::Digest> hash;
    if (parts.size() == 5) {
        hash = hash_algorithm(parts[4]);
        if (parts[3].starts_with("message")) {
            auto value = digest(parts[4], data);
            if (!value) return failure(status::unsupported_algorithm, "digest failed");
            input = std::move(*value);
        }
    }
    using host::crypto::RsaPadding;
    if (key.type == KeyMaterial::Type::rsa) {
        const auto host_key = rsa_of(key);
        if (parts[3] == "raw") {
            if (const auto checked = host::crypto::check(host_key, true); !checked) return refused(checked.error(), status::param, "");
            if (input.size() > key.n.size()) return failure(status::param, "data larger than the RSA block");
            input = padded(input, key.n.size());
            const auto output = host::crypto::rsa_decrypt(host_key, RsaPadding::none, std::nullopt, std::as_bytes(std::span(input)));
            if (!output) return refused(output.error(), status::param, "RSA raw signing failed");
            return {padded(*output, key.n.size()), 0, {}};
        }
        const bool pss = parts[3].ends_with("PSS");
        auto output = host::crypto::rsa_sign(host_key, pss ? RsaPadding::pss : RsaPadding::pkcs1, hash, std::as_bytes(std::span(input)));
        if (!output) return refused(output.error(), status::param, pss ? "RSA-PSS signing failed" : "RSA PKCS#1 signing failed");
        return {std::move(*output), 0, {}};
    }
    auto raw = host::crypto::ecdsa_sign(ec_of(key), std::as_bytes(std::span(input)));
    if (!raw) return refused(raw.error(), status::param, "ECDSA signing failed");
    if (parts[3] == "RFC4754") return {std::move(*raw), 0, {}};
    return {ecdsa_der(*raw), 0, {}};
}

KeyResult key_verify(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data,
                     std::span<const std::uint8_t> signature) {
    if (!key_supports(key, KeyOperation::verify, algorithm)) return failure(status::unsupported_algorithm, "algorithm not supported by the key");
    const auto parts = split(algorithm);
    Bytes input(data.begin(), data.end());
    std::optional<host::crypto::Digest> hash;
    if (parts.size() == 5) {
        hash = hash_algorithm(parts[4]);
        if (parts[3].starts_with("message")) {
            auto value = digest(parts[4], data);
            if (!value) return failure(status::unsupported_algorithm, "digest failed");
            input = std::move(*value);
        }
    }
    using host::crypto::RsaPadding;
    Bytes sig(signature.begin(), signature.end());
    host::Status result;
    if (key.type == KeyMaterial::Type::rsa) {
        const auto host_key = rsa_of(key);
        if (parts[3] == "raw") {
            if (const auto checked = host::crypto::check(host_key, false); !checked) return refused(checked.error(), status::verify_failed, "");
            if (sig.size() != key.n.size()) return failure(status::verify_failed, "RSA signature verification failed");
            const auto output = host::crypto::rsa_encrypt(host_key, RsaPadding::none, std::nullopt, std::as_bytes(std::span(sig)));
            if (!output) return refused(output.error(), status::verify_failed, "RSA signature verification failed");
            return padded(*output, key.n.size()) == padded(input, key.n.size()) ? KeyResult{Bytes{}, 0, {}}
                : failure(status::verify_failed, "RSA signature verification failed");
        }
        result = host::crypto::rsa_verify(host_key, parts[3].ends_with("PSS") ? RsaPadding::pss : RsaPadding::pkcs1, hash,
                                          std::as_bytes(std::span(input)), std::as_bytes(std::span(sig)));
    } else {
        const auto host_key = ec_of(key);
        if (const auto checked = host::crypto::check(host_key, false); !checked) return refused(checked.error(), status::verify_failed, "");
        auto raw = parts[3] == "RFC4754" ? std::optional<Bytes>(sig) : ecdsa_raw(sig, key.coordinate);
        if (!raw || raw->size() != key.coordinate * 2) return failure(status::verify_failed, "ECDSA signature is malformed");
        result = host::crypto::ecdsa_verify(host_key, std::as_bytes(std::span(input)), std::as_bytes(std::span(*raw)));
    }
    if (!result) return refused(result.error(), status::verify_failed, "signature verification failed");
    return {Bytes{}, 0, {}};
}

KeyResult key_encrypt(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data) {
    if (!key_supports(key, KeyOperation::encrypt, algorithm)) return failure(status::unsupported_algorithm, "algorithm not supported by the key");
    const auto parts = split(algorithm);
    const auto host_key = rsa_of(key);
    if (const auto checked = host::crypto::check(host_key, false); !checked) return refused(checked.error(), status::param, "");
    using host::crypto::RsaPadding;
    Bytes input(data.begin(), data.end());
    const auto padding = parts[3] == "raw" ? RsaPadding::none : parts[3] == "PKCS1" ? RsaPadding::pkcs1 : RsaPadding::oaep;
    if (padding == RsaPadding::none) {
        if (input.size() > key.n.size()) return failure(status::param, "data larger than the RSA block");
        input = padded(input, key.n.size());
    }
    const auto output = host::crypto::rsa_encrypt(host_key, padding, padding == RsaPadding::oaep ? hash_algorithm(parts[4]) : std::nullopt,
                                                  std::as_bytes(std::span(input)));
    if (!output) return refused(output.error(), status::param, "RSA encryption failed");
    return {padded(*output, key.n.size()), 0, {}};
}

KeyResult key_decrypt(const KeyMaterial& key, std::string_view algorithm, std::span<const std::uint8_t> data) {
    if (!key_supports(key, KeyOperation::decrypt, algorithm)) return failure(status::unsupported_algorithm, "algorithm not supported by the key");
    const auto parts = split(algorithm);
    if (data.size() != key.n.size()) return failure(status::param, "ciphertext is not one RSA block");
    const auto host_key = rsa_of(key);
    using host::crypto::RsaPadding;
    const auto padding = parts[3] == "raw" ? RsaPadding::none : parts[3] == "PKCS1" ? RsaPadding::pkcs1 : RsaPadding::oaep;
    const auto digest_of = padding == RsaPadding::oaep ? hash_algorithm(parts[4]) : std::nullopt;
    auto output = host::crypto::rsa_decrypt(host_key, padding, digest_of, std::as_bytes(data));
    if (!output) return refused(output.error(), status::decode, "RSA decryption failed");
    if (padding == RsaPadding::none) return {padded(*output, key.n.size()), 0, {}};
    return {std::move(*output), 0, {}};
}

KeyResult key_exchange(const KeyMaterial& key, const KeyMaterial& peer, std::string_view algorithm) {
    if (!key_supports(key, KeyOperation::key_exchange, algorithm)) return failure(status::unsupported_algorithm, "algorithm not supported by the key");
    if (peer.type != KeyMaterial::Type::ec || peer.coordinate != key.coordinate) return failure(status::param, "peer key is not on the same curve");
    auto secret = host::crypto::ecdh(ec_of(key), ec_of(peer));
    if (!secret) return refused(secret.error(), status::param, "ECDH agreement failed");
    return {std::move(*secret), 0, {}};
}

std::span<const AlgorithmName> key_algorithms() {
    static const std::vector<AlgorithmName> names = [] {
        std::vector<AlgorithmName> result;
        static std::vector<std::string> storage;   // stable strings
        const auto add = [&](std::string symbol, std::string value) {
            storage.push_back(std::move(symbol));
            storage.push_back(std::move(value));
        };
        add("kSecKeyAlgorithmRSASignatureRaw", "algid:sign:RSA:raw");
        add("kSecKeyAlgorithmRSASignatureDigestPKCS1v15Raw", "algid:sign:RSA:digest-PKCS1v15");
        add("kSecKeyAlgorithmECDSASignatureRFC4754", "algid:sign:ECDSA:RFC4754");
        add("kSecKeyAlgorithmECDSASignatureDigestX962", "algid:sign:ECDSA:digest-X962");
        add("kSecKeyAlgorithmRSAEncryptionRaw", "algid:encrypt:RSA:raw");
        add("kSecKeyAlgorithmRSAEncryptionPKCS1", "algid:encrypt:RSA:PKCS1");
        add("kSecKeyAlgorithmECDHKeyExchangeStandard", "algid:keyexchange:ECDH:standard");
        add("kSecKeyAlgorithmECDHKeyExchangeCofactor", "algid:keyexchange:ECDH:cofactor");
        for (const auto* hash : {"SHA1", "SHA224", "SHA256", "SHA384", "SHA512"}) {
            const std::string h(hash);
            add("kSecKeyAlgorithmRSASignatureDigestPKCS1v15" + h, "algid:sign:RSA:digest-PKCS1v15:" + h);
            add("kSecKeyAlgorithmRSASignatureMessagePKCS1v15" + h, "algid:sign:RSA:message-PKCS1v15:" + h);
            add("kSecKeyAlgorithmRSASignatureDigestPSS" + h, "algid:sign:RSA:digest-PSS:" + h);
            add("kSecKeyAlgorithmRSASignatureMessagePSS" + h, "algid:sign:RSA:message-PSS:" + h);
            add("kSecKeyAlgorithmECDSASignatureDigestX962" + h, "algid:sign:ECDSA:digest-X962:" + h);
            add("kSecKeyAlgorithmECDSASignatureMessageX962" + h, "algid:sign:ECDSA:message-X962:" + h);
            add("kSecKeyAlgorithmRSAEncryptionOAEP" + h, "algid:encrypt:RSA:OAEP:" + h);
        }
        for (std::size_t index = 0; index < storage.size(); index += 2)
            result.push_back({storage[index].c_str(), storage[index + 1].c_str()});
        return result;
    }();
    return names;
}

// ---- exports ----------------------------------------------------------------------------------------------------

namespace {

// SecExternalFormat / SecExternalItemType.
constexpr std::uint32_t format_unknown = 0, format_openssl = 1, format_raw_key = 4, format_x509 = 9, format_pem_sequence = 10,
    format_pkcs12 = 12;
constexpr std::uint32_t item_unknown = 0, item_private_key = 1, item_public_key = 2, item_certificate = 4, item_aggregate = 5;
constexpr std::int32_t err_unknown_format = -25257, err_passphrase_required = -25260;

struct Imported {
    std::vector<Address> items;   // +1
    std::uint32_t format{format_unknown};
    std::uint32_t type{item_unknown};
    std::int32_t status{};
};

struct Keys {
    Security& sec;
    Bridge& b;

    KeyPtr require_key(Address object) {
        auto key = sec.key_of(object);
        if (!key) fail("expected a SecKeyRef");
        return key;
    }
    std::string algorithm(Address object) { return b.text(object); }
    Address result(const KeyResult& value, Address error_out) {
        if (value.value) return b.data(*value.value);
        sec.set_error(error_out, value.status, value.error);
        return 0;
    }
    Address attributes(const KeyMaterial& key) {
        std::vector<std::pair<Address, Address>> entries;
        std::vector<Address> owned;
        const auto put = [&](std::string_view name, Address value, bool own) {
            entries.emplace_back(sec.key(name), value);
            if (own) owned.push_back(value);
        };
        put("kSecAttrKeyType", sec.key(key.type == KeyMaterial::Type::rsa ? "kSecAttrKeyTypeRSA" : "kSecAttrKeyTypeECSECPrimeRandom"), false);
        put("kSecAttrKeyClass", sec.key(key.is_private ? "kSecAttrKeyClassPrivate" : "kSecAttrKeyClassPublic"), false);
        put("kSecAttrKeySizeInBits", b.number(key.bits), true);
        put("kSecAttrEffectiveKeySize", b.number(key.bits), true);
        put("kSecAttrIsPermanent", b.cf.boolean(false), false);
        const bool rsa = key.type == KeyMaterial::Type::rsa;
        put("kSecAttrCanEncrypt", b.cf.boolean(rsa && !key.is_private), false);
        put("kSecAttrCanDecrypt", b.cf.boolean(rsa && key.is_private), false);
        put("kSecAttrCanSign", b.cf.boolean(key.is_private), false);
        put("kSecAttrCanVerify", b.cf.boolean(!key.is_private), false);
        put("kSecAttrCanDerive", b.cf.boolean(!rsa && key.is_private), false);
        if (const auto label = digest("SHA1", key.public_external())) put("kSecAttrApplicationLabel", b.data(*label), true);
        put("kSecValueData", b.data(key.is_private ? key.private_external() : key.public_external()), true);
        const auto value = b.dictionary(entries);
        b.release_all(owned);
        return value;
    }
    // The key type and size of a key-generation / import dictionary.
    std::optional<std::pair<KeyMaterial::Type, unsigned>> type_of(Address parameters) {
        const auto type = sec.get(parameters, "kSecAttrKeyType");
        std::optional<KeyMaterial::Type> kind;
        if (sec.is(type, "kSecAttrKeyTypeRSA")) kind = KeyMaterial::Type::rsa;
        else if (sec.is(type, "kSecAttrKeyTypeECSECPrimeRandom")) kind = KeyMaterial::Type::ec;
        else if (const auto number = b.integer(type); number && *number == 42) kind = KeyMaterial::Type::rsa;
        else if (number && *number == 73) kind = KeyMaterial::Type::ec;
        if (!kind) return std::nullopt;
        unsigned bits = *kind == KeyMaterial::Type::rsa ? 2048 : 256;
        const auto size = sec.get(parameters, "kSecAttrKeySizeInBits");
        if (const auto number = b.integer(size)) bits = static_cast<unsigned>(*number);
        else if (const auto text = b.maybe_text(size)) bits = static_cast<unsigned>(std::stoul(*text));
        return std::make_pair(*kind, bits);
    }
    // Creating keys in the keychain or in a token (Secure Enclave) is not provided.
    std::optional<std::string> unsupported_storage(Address parameters) {
        if (sec.get_bool(parameters, "kSecAttrIsPermanent")) return "permanent (keychain) keys are not supported";
        for (const auto* nested : {"kSecPrivateKeyAttrs", "kSecPublicKeyAttrs"})
            if (const auto inner = sec.get(parameters, nested); inner && b.is_dictionary(inner) && sec.get_bool(inner, "kSecAttrIsPermanent"))
                return "permanent (keychain) keys are not supported";
        if (sec.get(parameters, "kSecAttrTokenID")) return "token (Secure Enclave) keys are not supported";
        return std::nullopt;
    }

    void add_der(Imported& out, std::span<const std::uint8_t> bytes, std::string_view label) {
        const auto certificate = [&] {
            if (auto value = Certificate::parse(bytes)) {
                out.items.push_back(sec.make_certificate(value));
                out.type = out.type == item_unknown || out.type == item_certificate ? item_certificate : item_aggregate;
                return true;
            }
            return false;
        };
        const auto key = [&](KeyPtr value) {
            if (!value) return false;
            out.items.push_back(sec.make_key(value));
            const auto type = value->is_private ? item_private_key : item_public_key;
            out.type = out.type == item_unknown || out.type == type ? type : item_aggregate;
            return true;
        };
        if (label == "CERTIFICATE" || label == "X509 CERTIFICATE" || label == "TRUSTED CERTIFICATE") {
            if (!certificate()) out.status = status::decode;
            return;
        }
        if (label == "PUBLIC KEY") { if (!key(key_from_spki(bytes))) out.status = status::decode; return; }
        if (label == "RSA PUBLIC KEY") { if (!key(key_from_external(KeyMaterial::Type::rsa, false, bytes))) out.status = status::decode; return; }
        if (label == "RSA PRIVATE KEY") { if (!key(key_from_external(KeyMaterial::Type::rsa, true, bytes))) out.status = status::decode; return; }
        if (label == "PRIVATE KEY") { if (!key(key_from_pkcs8(bytes))) out.status = status::decode; return; }
        if (label == "EC PRIVATE KEY") { if (!key(key_from_sec1(bytes))) out.status = status::decode; return; }
        if (label == "ENCRYPTED PRIVATE KEY") { out.status = status::unimplemented; return; }
        if (!label.empty()) { out.status = err_unknown_format; return; }
        // Unlabelled DER: try each structure.
        if (certificate()) return;
        if (key(key_from_spki(bytes))) return;
        if (key(key_from_pkcs8(bytes))) return;
        if (key(key_from_external(KeyMaterial::Type::rsa, false, bytes))) return;
        if (key(key_from_external(KeyMaterial::Type::rsa, true, bytes))) return;
        if (key(key_from_sec1(bytes))) return;
        out.status = err_unknown_format;
    }

    Imported import_items(const Bytes& data) {
        Imported out;
        const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
        if (text.find("-----BEGIN ") != std::string_view::npos) {
            out.format = format_openssl;
            std::size_t at = 0;
            std::size_t blocks = 0;
            while ((at = text.find("-----BEGIN ", at)) != std::string_view::npos) {
                const auto label_start = at + 11;
                const auto label_end = text.find("-----", label_start);
                if (label_end == std::string_view::npos) { out.status = status::decode; break; }
                const auto label = text.substr(label_start, label_end - label_start);
                const auto end_marker = "-----END " + std::string(label) + "-----";
                const auto body_start = label_end + 5;
                const auto body_end = text.find(end_marker, body_start);
                if (body_end == std::string_view::npos) { out.status = status::decode; break; }
                const auto body = text.substr(body_start, body_end - body_start);
                if (body.find("Proc-Type:") != std::string_view::npos) { out.status = err_passphrase_required; break; }
                const auto decoded = base64_decode(body);
                if (!decoded) { out.status = status::decode; break; }
                add_der(out, *decoded, label);
                if (out.status) break;
                ++blocks;
                at = body_end + end_marker.size();
            }
            if (blocks > 1 && !out.status) out.format = format_pem_sequence;
            if (!blocks && !out.status) out.status = err_unknown_format;
        } else {
            add_der(out, data, {});
            if (!out.status && !out.items.empty())
                out.format = out.type == item_certificate ? format_x509 : format_openssl;
        }
        if (out.status) { b.release_all(out.items); out.items.clear(); }
        return out;
    }
};

} // namespace

void install_keys(Security& sec) {
    auto& b = *sec.b;
    auto self = std::make_shared<Keys>(Keys{sec, b});

    // Attribute keys and values used by SecKey dictionaries (shared with the keychain).
    const std::pair<const char*, const char*> constants[] = {
        {"kSecAttrKeyType", "type"}, {"kSecAttrKeyTypeRSA", "42"}, {"kSecAttrKeyTypeEC", "73"},
        {"kSecAttrKeyTypeECSECPrimeRandom", "73"}, {"kSecAttrKeyClass", "kcls"}, {"kSecAttrKeyClassPublic", "0"},
        {"kSecAttrKeyClassPrivate", "1"}, {"kSecAttrKeyClassSymmetric", "2"}, {"kSecAttrKeySizeInBits", "bsiz"},
        {"kSecAttrEffectiveKeySize", "esiz"}, {"kSecAttrIsPermanent", "perm"}, {"kSecAttrApplicationTag", "atag"},
        {"kSecAttrApplicationLabel", "klbl"}, {"kSecAttrCanEncrypt", "encr"}, {"kSecAttrCanDecrypt", "decr"},
        {"kSecAttrCanSign", "sign"}, {"kSecAttrCanVerify", "vrfy"}, {"kSecAttrCanDerive", "drve"},
        {"kSecAttrCanWrap", "wrap"}, {"kSecAttrCanUnwrap", "unwp"}, {"kSecAttrTokenID", "tkid"},
        {"kSecAttrTokenIDSecureEnclave", "com.apple.setoken"}, {"kSecPrivateKeyAttrs", "private"},
        {"kSecPublicKeyAttrs", "public"}, {"kSecValueData", "v_Data"}, {"kSecValueRef", "v_Ref"},
        {"kSecValuePersistentRef", "v_PersistentRef"}, {"kSecImportExportPassphrase", "passphrase"},
        {"kSecImportItemIdentity", "identity"}, {"kSecImportItemCertChain", "chain"}, {"kSecImportItemTrust", "trust"},
        {"kSecImportItemKeyID", "keyid"}, {"kSecImportItemLabel", "label"}};
    for (const auto& [name, value] : constants) sec.export_constant(name, value);
    for (const auto& algorithm : key_algorithms()) sec.export_constant(algorithm.symbol, algorithm.value);

    b.function(lib, "_SecKeyGetTypeID", [](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(TypeId::key); });
    b.function(lib, "_SecKeyCreateWithData", [self](CpuState& s, GuestMemory&) {
        auto& sec = self->sec;
        if (!self->b.is_data(s.x[0]) || !self->b.is_dictionary(s.x[1])) { sec.set_error(s.x[2], status::param, "invalid key data or attributes"); s.x[0] = 0; return; }
        const auto type = self->type_of(s.x[1]);
        const auto cls = sec.get(s.x[1], "kSecAttrKeyClass");
        if (!type || !(sec.is(cls, "kSecAttrKeyClassPublic") || sec.is(cls, "kSecAttrKeyClassPrivate"))) {
            sec.set_error(s.x[2], status::param, "unsupported key type or class");
            s.x[0] = 0;
            return;
        }
        const auto key = key_from_external(type->first, sec.is(cls, "kSecAttrKeyClassPrivate"), self->b.bytes(s.x[0]));
        if (!key) { sec.set_error(s.x[2], status::decode, "the key data is not a valid key of that type"); s.x[0] = 0; return; }
        s.x[0] = sec.make_key(key);
    });
    b.function(lib, "_SecKeyCopyExternalRepresentation", [self](CpuState& s, GuestMemory&) {
        const auto key = self->require_key(s.x[0]);
        s.x[0] = self->b.data(key->is_private ? key->private_external() : key->public_external());
    });
    b.function(lib, "_SecKeyCopyPublicKey", [self](CpuState& s, GuestMemory&) {
        const auto key = self->require_key(s.x[0]);
        s.x[0] = self->sec.make_key(key->is_private ? public_key_of(*key) : key);
    });
    b.function(lib, "_SecKeyGetBlockSize", [self](CpuState& s, GuestMemory&) { s.x[0] = self->require_key(s.x[0])->block_size(); });
    b.function(lib, "_SecKeyCopyAttributes", [self](CpuState& s, GuestMemory&) { s.x[0] = self->attributes(*self->require_key(s.x[0])); });
    b.function(lib, "_SecKeyCreateRandomKey", [self](CpuState& s, GuestMemory&) {
        const auto type = self->type_of(s.x[0]);
        if (!type) { self->sec.set_error(s.x[1], status::param, "unsupported key type"); s.x[0] = 0; return; }
        if (const auto why = self->unsupported_storage(s.x[0])) { self->sec.set_error(s.x[1], status::unimplemented, *why); s.x[0] = 0; return; }
        const auto key = generate_key(type->first, type->second);
        if (!key) { self->sec.set_error(s.x[1], status::param, "unsupported key size"); s.x[0] = 0; return; }
        s.x[0] = self->sec.make_key(key);
    });
    b.function(lib, "_SecKeyGeneratePair", [self](CpuState& s, GuestMemory& m) {
        const auto type = self->type_of(s.x[0]);
        if (!type || !s.x[1] || !s.x[2]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        if (self->unsupported_storage(s.x[0])) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::unimplemented)); return; }
        const auto key = generate_key(type->first, type->second);
        if (!key) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        m.write64(s.x[1], self->sec.make_key(public_key_of(*key)));
        m.write64(s.x[2], self->sec.make_key(key));
        s.x[0] = 0;
    });
    b.function(lib, "_SecKeyIsAlgorithmSupported", [self](CpuState& s, GuestMemory&) {
        const auto key = self->sec.key_of(s.x[0]);
        s.x[0] = key && s.x[1] <= 4 && self->b.is_string(s.x[2]) &&
            key_supports(*key, static_cast<KeyOperation>(s.x[1]), self->algorithm(s.x[2]));
    });
    b.function(lib, "_SecKeyCreateSignature", [self](CpuState& s, GuestMemory&) {
        const auto key = self->require_key(s.x[0]);
        if (!self->b.is_data(s.x[2])) { self->sec.set_error(s.x[3], status::param, "data to sign is not CFData"); s.x[0] = 0; return; }
        s.x[0] = self->result(key_sign(*key, self->algorithm(s.x[1]), self->b.bytes(s.x[2])), s.x[3]);
    });
    b.function(lib, "_SecKeyVerifySignature", [self](CpuState& s, GuestMemory&) {
        const auto key = self->require_key(s.x[0]);
        if (!self->b.is_data(s.x[2]) || !self->b.is_data(s.x[3])) { self->sec.set_error(s.x[4], status::param, "data is not CFData"); s.x[0] = 0; return; }
        const auto value = key_verify(*key, self->algorithm(s.x[1]), self->b.bytes(s.x[2]), self->b.bytes(s.x[3]));
        if (!value.value) self->sec.set_error(s.x[4], value.status, value.error);
        s.x[0] = value.value ? 1 : 0;
    });
    b.function(lib, "_SecKeyCreateEncryptedData", [self](CpuState& s, GuestMemory&) {
        const auto key = self->require_key(s.x[0]);
        if (!self->b.is_data(s.x[2])) { self->sec.set_error(s.x[3], status::param, "plaintext is not CFData"); s.x[0] = 0; return; }
        s.x[0] = self->result(key_encrypt(*key, self->algorithm(s.x[1]), self->b.bytes(s.x[2])), s.x[3]);
    });
    b.function(lib, "_SecKeyCreateDecryptedData", [self](CpuState& s, GuestMemory&) {
        const auto key = self->require_key(s.x[0]);
        if (!self->b.is_data(s.x[2])) { self->sec.set_error(s.x[3], status::param, "ciphertext is not CFData"); s.x[0] = 0; return; }
        s.x[0] = self->result(key_decrypt(*key, self->algorithm(s.x[1]), self->b.bytes(s.x[2])), s.x[3]);
    });
    b.function(lib, "_SecKeyCopyKeyExchangeResult", [self](CpuState& s, GuestMemory&) {
        const auto key = self->require_key(s.x[0]), peer = self->require_key(s.x[2]);
        if (s.x[3] && self->b.is_dictionary(s.x[3]) && self->b.cf.length(s.x[3]) != 0) {
            self->sec.set_error(s.x[4], status::unimplemented, "key exchange parameters (KDF) are not supported");
            s.x[0] = 0;
            return;
        }
        s.x[0] = self->result(key_exchange(*key, *peer, self->algorithm(s.x[1])), s.x[4]);
    });

    // OSStatus SecItemImport(CFDataRef importedData, CFStringRef fileNameOrExtension,
    //     SecExternalFormat *inputFormat, SecExternalItemType *itemType, SecItemImportExportFlags flags,
    //     const SecItemImportExportKeyParameters *keyParams, SecKeychainRef importKeychain, CFArrayRef *outItems)
    b.function(lib, "_SecItemImport", [self](CpuState& s, GuestMemory& m) {
        if (!self->b.is_data(s.x[0])) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::param)); return; }
        if (s.x[6]) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::unimplemented)); return; }   // keychain import
        const auto requested = s.x[2] ? m.read32(s.x[2]) : format_unknown;
        if (requested == format_raw_key) {   // symmetric session keys
            s.x[0] = static_cast<Address>(static_cast<std::int64_t>(status::unimplemented));
            return;
        }
        const auto bytes = self->b.bytes(s.x[0]);
        if (requested == format_pkcs12 || (requested == format_unknown && host::crypto::Identity::is_pkcs12(std::as_bytes(std::span(bytes))))) {
            // SecItemImportExportKeyParameters: {UInt32 version; UInt32 flags; CFTypeRef passphrase; ...}.
            std::string passphrase;
            if (s.x[5]) if (const auto value = m.read64(s.x[5] + 8); value && self->b.is_string(value)) passphrase = self->b.text(value);
            std::vector<Address> identities, certificates;
            const auto result = import_pkcs12(self->sec, bytes, passphrase, identities, certificates);
            if (result) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(result)); return; }
            identities.insert(identities.end(), certificates.begin(), certificates.end());
            if (s.x[2]) m.write32(s.x[2], format_pkcs12);
            if (s.x[3]) m.write32(s.x[3], item_aggregate);
            if (s.x[7]) m.write64(s.x[7], self->b.array(identities));
            self->b.release_all(identities);
            s.x[0] = 0;
            return;
        }
        auto imported = self->import_items(bytes);
        if (imported.status) { s.x[0] = static_cast<Address>(static_cast<std::int64_t>(imported.status)); return; }
        if (s.x[2]) m.write32(s.x[2], imported.format);
        if (s.x[3]) m.write32(s.x[3], imported.type);
        if (s.x[7]) m.write64(s.x[7], self->b.array(imported.items));
        self->b.release_all(imported.items);
        s.x[0] = 0;
    });
}

} // namespace alchemy::gui::security
